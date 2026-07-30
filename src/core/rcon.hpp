#pragma once
// ============================================================
// RCON_V1: ванильный Source RCON для Zevvoryn.
//
// Формат пакета (всё little-endian):
//   int32 length   — длина остатка пакета (без себя)
//   int32 id       — идентификатор запроса (возвращаем как есть; -1 = отказ авторизации)
//   int32 type     — 3 AUTH, 2 EXECCOMMAND / AUTH_RESPONSE, 0 RESPONSE_VALUE
//   body           — ASCII-текст + '\0' + '\0'
//
// Сам сервер команды не выполняет: он зовёт CommandHandler, а тот (см.
// RCON_BRIDGE_V1 в server.cpp) передаёт команду тик-потоку и ждёт вывод.
// ============================================================

#include "types.hpp"
#include "log.hpp"

#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // NOMINMAX_V1: иначе windef.h определит макросы min/max и сломает std::min/std::max
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace nc::rcon {

#ifdef _WIN32
using rsocket_t = SOCKET;
constexpr rsocket_t INVALID_RSOCK = INVALID_SOCKET;
inline void closeSocket(rsocket_t s) { ::closesocket(s); }
#else
using rsocket_t = int;
constexpr rsocket_t INVALID_RSOCK = -1;
inline void closeSocket(rsocket_t s) { ::close(s); }
#endif

// Типы пакетов Source RCON
constexpr i32 kAuth          = 3;
constexpr i32 kAuthResponse  = 2;
constexpr i32 kExecCommand   = 2;
constexpr i32 kResponseValue = 0;

// Защита от мусора в сокете
constexpr i32 kMinPacketLen = 10;
constexpr i32 kMaxPacketLen = 8192;
constexpr size_t kMaxBodyOut = 4000;

using CommandHandler = std::function<std::string(const std::string&)>;

class RconServer {
public:
    RconServer() = default;
    ~RconServer() { stop(); }

    RconServer(const RconServer&) = delete;
    RconServer& operator=(const RconServer&) = delete;

    bool isRunning() const { return running_.load(std::memory_order_acquire); }
    i32 clientCount() const { return clients_.load(std::memory_order_acquire); }

    bool start(u16 port, const std::string& password, i32 maxClients, CommandHandler handler) {
        if (running_.load(std::memory_order_acquire)) return true;
        if (password.empty()) {
            NC_WARN("RCON", "Пустой пароль — удалённая консоль не запущена");
            return false;
        }

#ifdef _WIN32
        // WSACleanup не зовём: сетевой стек уже использует игровой сокет.
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_ == INVALID_RSOCK) {
            NC_ERROR("RCON", "Не удалось создать сокет");
            return false;
        }

        int yes = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            NC_ERROR("RCON", "Порт {} занят — удалённая консоль не запущена", port);
            closeSocket(listen_);
            listen_ = INVALID_RSOCK;
            return false;
        }
        if (::listen(listen_, 8) != 0) {
            NC_ERROR("RCON", "listen() не удался на порту {}", port);
            closeSocket(listen_);
            listen_ = INVALID_RSOCK;
            return false;
        }

        password_ = password;
        maxClients_ = (maxClients < 1) ? 1 : maxClients;
        handler_ = std::move(handler);
        running_.store(true, std::memory_order_release);
        acceptThread_ = std::thread([this] { acceptLoop(); });

        NC_INFO("RCON", "Удалённая консоль слушает 0.0.0.0:{} (клиентов до {})", port, maxClients_);
        return true;
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;

        if (listen_ != INVALID_RSOCK) {
            closeSocket(listen_);      // рвёт accept() в потоке приёма
            listen_ = INVALID_RSOCK;
        }
        if (acceptThread_.joinable()) acceptThread_.join();

        // Клиентские потоки detached: они завершатся сами по ошибке recv.
        NC_INFO("RCON", "Удалённая консоль остановлена");
    }

private:
    void acceptLoop() {
        while (running_.load(std::memory_order_acquire)) {
            sockaddr_in peer{};
#ifdef _WIN32
            int peerLen = static_cast<int>(sizeof(peer));
#else
            socklen_t peerLen = sizeof(peer);
#endif
            rsocket_t sock = ::accept(listen_, reinterpret_cast<sockaddr*>(&peer), &peerLen);
            if (sock == INVALID_RSOCK) {
                if (!running_.load(std::memory_order_acquire)) break;
                continue;
            }

            if (clients_.load(std::memory_order_acquire) >= maxClients_) {
                NC_WARN("RCON", "Лимит клиентов ({}) — подключение отклонено", maxClients_);
                closeSocket(sock);
                continue;
            }

            char ipText[64] = {0};
#ifdef _WIN32
            ::inet_ntop(AF_INET, &peer.sin_addr, ipText, sizeof(ipText));
#else
            ::inet_ntop(AF_INET, &peer.sin_addr, ipText, sizeof(ipText));
#endif
            std::string peerName = std::string(ipText) + ":" + std::to_string(ntohs(peer.sin_port));

            clients_.fetch_add(1, std::memory_order_acq_rel);
            std::thread([this, sock, peerName] {
                clientLoop(sock, peerName);
                clients_.fetch_sub(1, std::memory_order_acq_rel);
            }).detach();
        }
    }

    void clientLoop(rsocket_t sock, std::string peer) {
        // Если клиент молчит пять минут — рвём соединение, чтобы не копить потоки.
#ifdef _WIN32
        DWORD tv = 300000;
#else
        timeval tv{};
        tv.tv_sec = 300;
        tv.tv_usec = 0;
#endif
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&tv), sizeof(tv));

        bool authorized = false;

        while (running_.load(std::memory_order_acquire)) {
            char lenBuf[4];
            if (!recvExact(sock, lenBuf, 4)) break;
            const i32 length = get32(lenBuf);
            if (length < kMinPacketLen || length > kMaxPacketLen) {
                NC_WARN("RCON", "Кривая длина пакета от {} — разрыв", peer);
                break;
            }

            std::vector<char> buf(static_cast<size_t>(length));
            if (!recvExact(sock, buf.data(), buf.size())) break;

            const i32 id = get32(buf.data());
            const i32 type = get32(buf.data() + 4);
            // Тело без двух завершающих нулей
            std::string body(buf.begin() + 8, buf.end());
            while (!body.empty() && body.back() == '\0') body.pop_back();

            if (type == kAuth) {
                authorized = (body == password_);
                // Ванильный порядок: пустой RESPONSE_VALUE, затем AUTH_RESPONSE.
                sendPacket(sock, id, kResponseValue, "");
                sendPacket(sock, authorized ? id : -1, kAuthResponse, "");
                if (authorized) NC_INFO("RCON", "Клиент {} авторизован", peer);
                else {
                    NC_WARN("RCON", "Неверный пароль от {}", peer);
                    break;
                }
                continue;
            }

            if (type != kExecCommand) continue;

            if (!authorized) {
                sendPacket(sock, -1, kResponseValue, "Not authenticated");
                break;
            }

            std::string reply;
            if (handler_) {
                try {
                    reply = handler_(body);
                } catch (const std::exception& e) {
                    reply = std::string("RCON error: ") + e.what();
                } catch (...) {
                    reply = "RCON error";
                }
            }
            if (reply.size() > kMaxBodyOut) reply.resize(kMaxBodyOut);
            sendPacket(sock, id, kResponseValue, reply);
        }

        closeSocket(sock);
    }

    static i32 get32(const char* p) {
        const unsigned char* u = reinterpret_cast<const unsigned char*>(p);
        return static_cast<i32>(static_cast<u32>(u[0])
             | (static_cast<u32>(u[1]) << 8)
             | (static_cast<u32>(u[2]) << 16)
             | (static_cast<u32>(u[3]) << 24));
    }

    static void put32(std::string& out, i32 value) {
        const u32 v = static_cast<u32>(value);
        out.push_back(static_cast<char>(v & 0xFF));
        out.push_back(static_cast<char>((v >> 8) & 0xFF));
        out.push_back(static_cast<char>((v >> 16) & 0xFF));
        out.push_back(static_cast<char>((v >> 24) & 0xFF));
    }

    static bool sendAll(rsocket_t sock, const char* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            const int n = ::send(sock, data + sent, static_cast<int>(len - sent), 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    static bool recvExact(rsocket_t sock, char* data, size_t len) {
        size_t got = 0;
        while (got < len) {
            const int n = ::recv(sock, data + got, static_cast<int>(len - got), 0);
            if (n <= 0) return false;
            got += static_cast<size_t>(n);
        }
        return true;
    }

    static bool sendPacket(rsocket_t sock, i32 id, i32 type, const std::string& body) {
        std::string packet;
        const i32 length = static_cast<i32>(4 + 4 + body.size() + 2);
        put32(packet, length);
        put32(packet, id);
        put32(packet, type);
        packet += body;
        packet.push_back('\0');
        packet.push_back('\0');
        return sendAll(sock, packet.data(), packet.size());
    }

    std::atomic<bool> running_{false};
    std::atomic<i32> clients_{0};
    rsocket_t listen_ = INVALID_RSOCK;
    std::string password_;
    i32 maxClients_ = 4;
    CommandHandler handler_;
    std::thread acceptThread_;
};

} // namespace nc::rcon
