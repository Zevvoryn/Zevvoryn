#pragma once

#include "../core/types.hpp"
#include "../core/log.hpp"
#include "buffer.hpp"
#include "../crypto/mc_crypto.hpp" // ONLINE_V1
#include <functional>
#include <memory>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>
#include <span>

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

    // Колбэк при закрытии (вызывается из close())
    void setOnClose(std::function<void(u64)> cb) { onClose_ = std::move(cb); }

    // Отправка пакета (фрейминг: длина + packetId + payload)
    void sendPacket(i32 packetId, const std::vector<u8>& payload);
    void sendPacket(i32 packetId, std::span<const u8> payload);

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
    template<typename T>
    void setData(std::shared_ptr<T> data) { userData_ = data; }

    template<typename T>
    std::shared_ptr<T> getData() const {
        return std::static_pointer_cast<T>(userData_);
    }

    // Запланировать отправку (потокобезопасно)
    void queueSend(const std::vector<u8>& data);
    void queueSend(std::vector<u8>&& data);

private:
    void doRead();
    void processPacket(i32 length, i32 packetId, Buffer& payload);
    void doWrite();

    socket_t socket_;
    Server& server_;
    u64 id_;
    std::atomic<bool> connected_{false};

    ConnectionState state_ = ConnectionState::Handshaking;

    // Буферы чтения
    std::vector<u8> readBuffer_;
    size_t readPos_ = 0;
    size_t readEnd_ = 0;

    // Пакеты для отправки
    std::queue<std::vector<u8>> writeQueue_;
    std::mutex writeMutex_;
    bool writing_ = false;

    // Компрессия
    bool compressionEnabled_ = false;
    i32 compressionThreshold_ = -1;

    // ONLINE_V1: AES-128-CFB8 socket encryption (Mojang online-mode)
    std::atomic<bool> encryptionEnabled_{false};
    crypto::AesCfb8 encCipher_;
    crypto::AesCfb8 decCipher_;

    // Пользовательские данные
    std::shared_ptr<void> userData_;

    // Колбэк при закрытии
    std::function<void(u64)> onClose_;
};

} // namespace nc::net
