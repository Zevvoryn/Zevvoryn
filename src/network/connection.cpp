#include "connection.hpp"
#include "server.hpp"
#include "zlib_codec.hpp" // ZLIB_V1
#include "../core/log.hpp"
#include <cstring>

namespace nc::net {

Connection::Connection(socket_t sock, Server& server, u64 id)
    : socket_(sock), server_(server), id_(id)
{
    readBuffer_.resize(65536);
    connected_.store(true, std::memory_order_release);
}

Connection::~Connection() {
    close();
}

void Connection::start() {
    int flag = 1;
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
    doRead();
}

void Connection::close() {
    bool expected = true;
    if (connected_.compare_exchange_strong(expected, false)) {
#ifdef _WIN32
        closesocket(socket_);
#else
        ::close(socket_);
#endif
        if (onClose_) onClose_(id_);
    }
}

// ZLIB_V1: включение сетевого сжатия после Set Compression
void Connection::enableCompression(i32 threshold) {
    compressionThreshold_ = threshold;
    compressionEnabled_ = (threshold >= 0);
}

// ONLINE_V1: enable AES-128-CFB8 on the socket (both directions share one secret)
void Connection::enableEncryption(std::span<const u8> secret) {
    if (secret.size() != 16) return;
    if (!encCipher_.init(secret) || !decCipher_.init(secret)) {
        NC_ERROR("Net", "Failed to initialize AES cipher");
        return;
    }
    encryptionEnabled_.store(true, std::memory_order_release);
}

void Connection::sendPacket(i32 packetId, const std::vector<u8>& payload) {
    sendPacket(packetId, std::span<const u8>(payload));
}

void Connection::sendPacket(i32 packetId, std::span<const u8> payload) {
    if (!isConnected()) return;

    auto appendVarInt = [](std::vector<u8>& v, i32 val) {
        u32 uval = static_cast<u32>(val);
        while (uval > 0x7F) {
            v.push_back(static_cast<u8>((uval & 0x7F) | 0x80));
            uval >>= 7;
        }
        v.push_back(static_cast<u8>(uval));
    };

    // packetBody = [VarInt packetId][payload]
    std::vector<u8> packetBody;
    packetBody.reserve(payload.size() + 5);
    appendVarInt(packetBody, packetId);
    packetBody.insert(packetBody.end(), payload.begin(), payload.end());

    std::vector<u8> frame;

    if (compressionEnabled_) {
        // ZLIB_V1: сжатый фрейминг [VarInt packetLength][VarInt dataLength][zlib(id+payload)]
        if (static_cast<i32>(packetBody.size()) >= compressionThreshold_) {
            std::vector<u8> compressed = zlibc::compress(std::span<const u8>(packetBody));
            std::vector<u8> inner;
            inner.reserve(5);
            appendVarInt(inner, static_cast<i32>(packetBody.size())); // dataLength = размер до сжатия
            frame.reserve(inner.size() + compressed.size() + 5);
            appendVarInt(frame, static_cast<i32>(inner.size() + compressed.size()));
            frame.insert(frame.end(), inner.begin(), inner.end());
            frame.insert(frame.end(), compressed.begin(), compressed.end());
        } else {
            // мелкий пакет: dataLength = 0, дальше несжатые данные
            frame.reserve(packetBody.size() + 6);
            appendVarInt(frame, static_cast<i32>(packetBody.size() + 1));
            frame.push_back(0);
            frame.insert(frame.end(), packetBody.begin(), packetBody.end());
        }
    } else {
        frame.reserve(packetBody.size() + 5);
        appendVarInt(frame, static_cast<i32>(packetBody.size()));
        frame.insert(frame.end(), packetBody.begin(), packetBody.end());
    }

    queueSend(std::move(frame));
}

void Connection::sendRaw(std::span<const u8> data) {
    std::vector<u8> frame(data.begin(), data.end());
    queueSend(std::move(frame));
}

void Connection::queueSend(const std::vector<u8>& data) {
    bool shouldWrite = false;
    {
        std::lock_guard lock(writeMutex_);
        writeQueue_.push(data);
        if (!writing_) {
            writing_ = true;
            shouldWrite = true;
        }
    }
    if (shouldWrite) doWrite();
}

void Connection::queueSend(std::vector<u8>&& data) {
    bool shouldWrite = false;
    {
        std::lock_guard lock(writeMutex_);
        writeQueue_.push(std::move(data));
        if (!writing_) {
            writing_ = true;
            shouldWrite = true;
        }
    }
    if (shouldWrite) doWrite();
}

void Connection::doWrite() {
    while (isConnected()) {
        std::vector<u8> data;
        {
            std::lock_guard lock(writeMutex_);
            if (writeQueue_.empty()) {
                writing_ = false;
                return;
            }
            data = std::move(writeQueue_.front());
            writeQueue_.pop();
        }

        if (encryptionEnabled_.load(std::memory_order_acquire) && !data.empty()) {
            encCipher_.encrypt(data.data(), data.size()); // ONLINE_V1
        }

        i32 totalSent = 0;
        i32 dataSize = static_cast<i32>(data.size());

        while (totalSent < dataSize && isConnected()) {
            i32 sent = ::send(socket_,
                reinterpret_cast<const char*>(data.data() + totalSent),
                dataSize - totalSent, 0);
            if (sent <= 0) {
                close();
                return;
            }
            totalSent += sent;
        }
    }
}

// ============================================================
// Чтение данных из сокета и парсинг пакетов
// Без сжатия: [VarInt length][VarInt packetId][payload...]
// Со сжатием (ZLIB_V1): [VarInt packetLength][VarInt dataLength][(zlib) packetId+payload]
// ============================================================

// Прочитать VarInt из буфера, вернуть количество байт consumed
// Если VarInt не полный — вернуть 0
static int tryReadVarInt(const u8* data, size_t available, i32& out) {
    out = 0;
    int shift = 0;
    for (size_t i = 0; i < available && i < 5; ++i) {
        u8 byte = data[i];
        out |= static_cast<i32>(byte & 0x7F) << shift;
        shift += 7;
        if (!(byte & 0x80)) {
            return static_cast<int>(i + 1);
        }
    }
    return 0; // не полный VarInt
}

void Connection::doRead() {
    while (isConnected()) {
        // Расширяем буфер если нужно
        if (readEnd_ + 4096 > readBuffer_.size()) {
            readBuffer_.resize(readBuffer_.size() + 4096);
        }

        int bytesRead = ::recv(socket_,
            reinterpret_cast<char*>(readBuffer_.data() + readEnd_),
            static_cast<int>(readBuffer_.size() - readEnd_), 0);

        if (bytesRead <= 0) {
            close();
            return;
        }

        if (encryptionEnabled_.load(std::memory_order_acquire)) {
            decCipher_.decrypt(readBuffer_.data() + readEnd_, static_cast<size_t>(bytesRead)); // ONLINE_V1
        }

        readEnd_ += static_cast<size_t>(bytesRead);

        // Парсим все полные пакеты из буфера
        bool progress = true;
        while (progress && readPos_ < readEnd_) {
            progress = false;

            // 1. Читаем длину пакета (VarInt)
            i32 packetLength = 0;
            int lenBytes = tryReadVarInt(
                readBuffer_.data() + readPos_,
                readEnd_ - readPos_,
                packetLength);

            if (lenBytes == 0) break; // VarInt не полный
            if (packetLength < 0 || packetLength > 2097151) {
                NC_ERROR("Net", "Invalid packet length: {}", packetLength);
                close();
                return;
            }

            // 2. Проверяем, достаточно ли данных
            size_t dataStart = readPos_ + lenBytes;
            if (readEnd_ < dataStart + static_cast<size_t>(packetLength)) {
                break; // Ждём больше данных
            }

            // ZLIB_V1: при включённом сжатии внутри пакета сначала идёт dataLength
            if (compressionEnabled_) {
                i32 dataLength = 0;
                int dlBytes = tryReadVarInt(
                    readBuffer_.data() + dataStart,
                    static_cast<size_t>(packetLength),
                    dataLength);

                if (dlBytes == 0 || dataLength < 0 || dataLength > 8388608) {
                    NC_ERROR("Net", "Invalid compressed dataLength: {}", dataLength);
                    close();
                    return;
                }

                const u8* bodyPtr = readBuffer_.data() + dataStart + dlBytes;
                size_t bodySize = static_cast<size_t>(packetLength) - dlBytes;

                std::vector<u8> plain; // должен жить до конца processPacket
                if (dataLength > 0) {
                    if (!zlibc::decompress(std::span<const u8>(bodyPtr, bodySize),
                                           static_cast<size_t>(dataLength), plain)) {
                        NC_ERROR("Net", "Failed to decompress packet (dataLength={})", dataLength);
                        close();
                        return;
                    }
                    bodyPtr = plain.data();
                    bodySize = plain.size();
                }

                i32 packetId = 0;
                int idBytes = tryReadVarInt(bodyPtr, bodySize, packetId);
                if (idBytes == 0) {
                    NC_ERROR("Net", "Invalid packet ID VarInt (compressed frame)");
                    close();
                    return;
                }

                Buffer packetData(bodyPtr + idBytes, bodySize - static_cast<size_t>(idBytes));
                readPos_ = dataStart + static_cast<size_t>(packetLength);
                processPacket(packetLength, packetId, packetData);
                progress = true;
                continue;
            }

            // 3. Читаем packet ID (VarInt внутри пакета)
            i32 packetId = 0;
            int idBytes = tryReadVarInt(
                readBuffer_.data() + dataStart,
                static_cast<size_t>(packetLength),
                packetId);

            if (idBytes == 0) {
                NC_ERROR("Net", "Invalid packet ID VarInt");
                close();
                return;
            }

            // 4. Payload = всё после packet ID
            size_t payloadStart = dataStart + idBytes;
            size_t payloadSize = static_cast<size_t>(packetLength) - idBytes;

            Buffer packetData(readBuffer_.data() + payloadStart, payloadSize);

            // 5. Сдвигаем позицию чтения
            readPos_ = dataStart + static_cast<size_t>(packetLength);

            // 6. Обрабатываем пакет
            processPacket(packetLength, packetId, packetData);
            progress = true;
        }

        // Сдвигаем буфер если прочитали данные
        if (readPos_ > 0) {
            size_t remaining = readEnd_ - readPos_;
            if (remaining > 0) {
                std::memmove(readBuffer_.data(), readBuffer_.data() + readPos_, remaining);
            }
            readEnd_ = remaining;
            readPos_ = 0;
        }
    }
}

void Connection::processPacket(i32 length, i32 packetId, Buffer& payload) {
    try {
        server_.handleIncomingPacket(shared_from_this(), packetId, payload);
    } catch (const std::exception& e) {
        NC_ERROR("Net", "Exception in packet handler (id={}): {}", packetId, e.what());
        close();
    } catch (...) {
        NC_ERROR("Net", "Unknown exception in packet handler (id={})", packetId);
        close();
    }
}

} // namespace nc::net
