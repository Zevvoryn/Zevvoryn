#pragma once

#include "../core/types.hpp"
#include "../core/log.hpp"
#include "buffer.hpp"
#include "../crypto/mc_crypto.hpp" // ONLINE_V1
#include <functional>
#include <memory>
#include <vector>
#include <queue>
#include <deque>   // NETQUEUE_V1
#include <mutex>
#include <atomic>
#include <span>
#include <thread>              // NETASYNC_V2
#include <condition_variable>  // NETASYNC_V2

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using socket_t = SOCKET;
    constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <unistd.h>
    #include <fcntl.h>
    using socket_t = int;
    constexpr socket_t INVALID_SOCK = -1;
#endif

namespace nc::net {

class Server;
class Connection;

// ============================================================
// Колбэки для обработки событий соединения
// ============================================================
using ConnectionHandler = std::function<void(std::shared_ptr<Connection>)>;
using PacketHandler    = std::function<void(std::shared_ptr<Connection>, Buffer& packetData, i32 packetId)>;
using DisconnectHandler = std::function<void(std::shared_ptr<Connection>)>;

// ============================================================
// TCP соединение с одним клиентом.
// Управляет фреймингом пакетов (length-prefixed + optional zlib).
// ============================================================
class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(socket_t sock, Server& server, u64 id);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Запуск чтения (асинхронный цикл)
    void start();
    void close();
    // GRACECLOSE_V1: мягкое закрытие — сокет закрывается ПОСЛЕ отправки всего, что в очереди.
    // Нужен для Login Disconnect («Сервер заполнен»): обычный close() рвал сокет раньше,
    // чем writerLoop успевал отправить пакет, и клиент видел «Не удалось подключиться».
    void closeAfterFlush();
    void gracefulShutdown(); // GRACECLOSE_V2: FIN + пауза, чтобы клиент успел прочитать Disconnect

    // Колбэк при закрытии (вызывается из close())
    void setOnClose(std::function<void(u64)> cb) { onClose_ = std::move(cb); }

    // Отправка пакета (фрейминг: длина + packetId + payload)
    void sendPacket(i32 packetId, const std::vector<u8>& payload, bool droppable = false);
    void sendPacket(i32 packetId, std::span<const u8> payload, bool droppable = false);

    // PKTLIMIT_V1: предел длины входящего пакета из max-packet-size.
    // Общий для всех соединений, ставится один раз при старте сервера.
    static void setMaxPacketSize(i32 bytes);
    static i32 getMaxPacketSize();

    // Отправка буфера напрямую (для raw-данных, например статус-ответ)
    void sendRaw(std::span<const u8> data);

    // Состояние
    bool isConnected() const { return connected_.load(std::memory_order_relaxed); }
    u64 getId() const { return id_; }
    socket_t getSocket() const { return socket_; }

    // Установить состояние протокола
    void setConnectionState(ConnectionState state) { state_ = state; }
    ConnectionState getConnectionState() const { return state_; }

    // Компрессия
    void enableCompression(i32 threshold);
    // ONLINE_V1: enable AES-128-CFB8 (key and IV are the 16-byte shared secret)
    void enableEncryption(std::span<const u8> secret);
    bool isCompressionEnabled() const { return compressionEnabled_; }
    i32 getCompressionThreshold() const { return compressionThreshold_; }

    // Хранилище пользовательских данных (player, session, etc.)
    // DATARACE_FIX_V1: setData(nullptr) при дисконнекте зовётся из тик-/writer-потока
    // (keepalive-таймаут, ошибка отправки), а read-поток параллельно копирует userData_
    // на КАЖДОМ пакете. Одновременные copy+assign у shared_ptr — UB и порча кучи
    // (отложенный SEH 0xC0000005 в «фоновом потоке» из стресс-теста). Теперь под мьютексом.
    template<typename T>
    void setData(std::shared_ptr<T> data) {
        std::lock_guard<std::mutex> lk(userDataMutex_); // DATARACE_FIX_V1
        userData_ = data;
    }

    template<typename T>
    std::shared_ptr<T> getData() const {
        std::lock_guard<std::mutex> lk(userDataMutex_); // DATARACE_FIX_V1
        return std::static_pointer_cast<T>(userData_);
    }

    // NETQUEUE_V1: выкинуть из очереди всё необязательное (движение, частицы, звуки).
    // Зовётся перед телепортом: критичные чанки и позиция не должны ждать мегабайты устаревших апдейтов.
    // Возвращает число выброшенных кадров.
    size_t dropQueuedDroppable();
    // NETQUEUE_V1: сколько байт сейчас ждёт отправки (для диагностики / бэкпрешера)
    size_t pendingBytes() const;
    u64 getDroppedPackets() const { return droppedPackets_.load(std::memory_order_relaxed); }

    // Запланировать отправку (потокобезопасно)
    void queueSend(const std::vector<u8>& data, bool droppable = false);
    void queueSend(std::vector<u8>&& data, bool droppable = false);

private:
    void doRead();
    void processPacket(i32 length, i32 packetId, Buffer& payload);
    void writerLoop(); // NETASYNC_V2: тело выделенного потока-писателя
    size_t dropDroppableLocked();                      // NETQUEUE_V1 (вызывать под writeMutex_)
    bool enqueueFrame(std::vector<u8>&& data, bool droppable); // NETQUEUE_V1

    socket_t socket_;
    Server& server_;
    u64 id_;
    std::atomic<bool> connected_{false};

    ConnectionState state_ = ConnectionState::Handshaking;

    // Буферы чтения
    std::vector<u8> readBuffer_;
    size_t readPos_ = 0;
    size_t readEnd_ = 0;

    // NETASYNC_V2: очередь обслуживает ВЫДЕЛЕННЫЙ поток-писатель (writerLoop).
    // queueSend() только кладёт данные и будит поток — никаких ::send() в потоке вызывающего.
    // NETQUEUE_V1: теперь храним признак «можно выкинуть» рядом с кадром,
    // чтобы при переполнении чистить только мусор, а не рвать соединение.
    struct QueuedFrame {
        std::vector<u8> data;
        bool droppable = false;
    };
    std::deque<QueuedFrame> writeQueue_;
    std::mutex writeMutex_;
    std::condition_variable writeCv_;   // NETASYNC_V2
    std::thread writerThread_;          // NETASYNC_V2
    size_t writeQueueBytes_ = 0;        // NETASYNC_V2
    static constexpr size_t kMaxWriteQueueBytes = 8u * 1024u * 1024u; // NETASYNC_V2
    // NETSHED_V1: мягкий порог — droppable-пакеты (движение/мета) сбрасываем, НЕ рвём сокет.
    static constexpr size_t kSoftDropBytes = 2u * 1024u * 1024u;
    std::atomic<u64> droppedPackets_{0}; // NETSHED_V1: сколько апдейтов сброшено под нагрузкой
    std::atomic<bool> closeAfterFlush_{0}; // GRACECLOSE_V1: закрыть сокет после слива очереди

    // Компрессия
    bool compressionEnabled_ = false;
    i32 compressionThreshold_ = -1;

    // ONLINE_V1: AES-128-CFB8 socket encryption (Mojang online-mode)
    std::atomic<bool> encryptionEnabled_{false};
    crypto::AesCfb8 encCipher_;
    crypto::AesCfb8 decCipher_;

    // Пользовательские данные
    std::shared_ptr<void> userData_;
    mutable std::mutex userDataMutex_; // DATARACE_FIX_V1

    // Колбэк при закрытии
    std::function<void(u64)> onClose_;
};

} // namespace nc::net
