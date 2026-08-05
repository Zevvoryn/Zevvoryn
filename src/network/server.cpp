#include "server.hpp"
#include "../core/log.hpp"
#include <cstring>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <signal.h>
#endif

namespace nc::net {

Server::~Server() {
    stop();
}

bool Server::start(u16 port, i32 backlog, bool enableIpv6) { // IPV6_V1
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        NC_FATAL("Net", "WSAStartup failed"); // LANGFIX_V1
        return false;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    // IPV6_V1: при enable-ipv6=true открываем AF_INET6 с выключенным IPV6_V6ONLY — такой
    // сокет принимает И IPv6, И обычные IPv4-подключения на том же порту.
    // Если система IPv6 не даёт — тихо откатываемся на чистый IPv4.
    bool usingV6 = false;
    if (enableIpv6) {
        listenSocket_ = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket_ != INVALID_SOCK) {
            int v6only = 0;
            if (setsockopt(listenSocket_, IPPROTO_IPV6, IPV6_V6ONLY,
                    reinterpret_cast<const char*>(&v6only), sizeof(v6only)) == 0) {
                usingV6 = true;
            } else {
#ifdef _WIN32
                ::closesocket(listenSocket_);
#else
                ::close(listenSocket_);
#endif
                listenSocket_ = INVALID_SOCK;
                NC_WARN("Net", "IPv6 dual-stack unavailable, falling back to IPv4");
            }
        } else {
            NC_WARN("Net", "IPv6 socket unavailable, falling back to IPv4");
        }
    }
    if (!usingV6) {
        listenSocket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }
    if (listenSocket_ == INVALID_SOCK) {
        NC_FATAL("Net", "Failed to create socket"); // LANGFIX_V1
        return false;
    }

    // PORTLOCK_V1: на Windows SO_REUSEADDR РАЗРЕШАЕТ второму процессу сесть на тот же порт
    // (и перехватить чужие подключения) — именно так два сервера из разных папок уживались
    // на 25565. SO_EXCLUSIVEADDRUSE запирает ПОРТ, а не папку: другие порты свободно поднимаются.
    int reuse = 1;
#if defined(_WIN32) && defined(SO_EXCLUSIVEADDRUSE)
    setsockopt(listenSocket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif

    // IPV6_V1: адрес привязки зависит от семейства сокета
    sockaddr_in  addr4{};
    sockaddr_in6 addr6{};
    const sockaddr* bindAddr = nullptr;
    socklen_t bindLen = 0;
    if (usingV6) {
        addr6.sin6_family = AF_INET6;
        addr6.sin6_addr   = in6addr_any;
        addr6.sin6_port   = htons(port);
        bindAddr = reinterpret_cast<const sockaddr*>(&addr6);
        bindLen  = static_cast<socklen_t>(sizeof(addr6));
    } else {
        addr4.sin_family      = AF_INET;
        addr4.sin_addr.s_addr = INADDR_ANY;
        addr4.sin_port        = htons(port);
        bindAddr = reinterpret_cast<const sockaddr*>(&addr4);
        bindLen  = static_cast<socklen_t>(sizeof(addr4));
    }

    if (::bind(listenSocket_, bindAddr, bindLen) != 0) {
        NC_FATAL("Net", "Failed to bind port {} (порт занят другой программой? / port already in use?)", port); // LANGFIX_V1
        return false;
    }

    if (::listen(listenSocket_, backlog) != 0) {
        NC_FATAL("Net", "Failed to listen on port {}", port); // LANGFIX_V1
        return false;
    }

    running_.store(true, std::memory_order_release);
    acceptThread_ = std::thread(&Server::acceptLoop, this);

    // LANGFIX_V1: сетевой модуль не знает язык — строку о запуске логирует ядро (core/server.cpp) на выбранном языке
    return true;
}

void Server::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;

    if (ruLang_) NC_INFO("Net", "Остановка сетевого прослушивателя..."); // LANGFIX_V1
    else NC_INFO("Net", "Stopping network listener..."); // LANGFIX_V1

    // Закрываем сокет прослушивания
    if (listenSocket_ != INVALID_SOCK) {
#ifdef _WIN32
        closesocket(listenSocket_);
#else
        ::close(listenSocket_);
#endif
        listenSocket_ = INVALID_SOCK;
    }

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }

    // NETSTOP_V1: закрывать соединения, ДЕРЖА connectionsMutex_, нельзя:
    // conn->close() синхронно зовёт onClose_ -> колбэк из acceptLoop зовёт
    // getConnection()/removeConnection(), которые лочат ТОТ ЖЕ mutex в этом же
    // потоке. Рекурсивный захват std::mutex = UB -> abort() при /stop с игроками.
    // Поэтому под замком только снимаем срез и чистим карту, а close() — снаружи.
    std::vector<std::shared_ptr<Connection>> toClose;
    {
        std::lock_guard lock(connectionsMutex_);
        toClose.reserve(connections_.size());
        for (auto& [id, conn] : connections_) toClose.push_back(conn);
        connections_.clear();
    }
    for (auto& conn : toClose) conn->close();

#ifdef _WIN32
    WSACleanup();
#endif

    if (ruLang_) NC_INFO("Net", "Сетевой прослушиватель остановлен"); // LANGFIX_V1
    else NC_INFO("Net", "Network listener stopped"); // LANGFIX_V1
}

// CRASHNET_V1: см. комментарий в server.hpp. Максимально самодостаточно и без
// блокировок, которые могли бы навесить дедлок прямо в обработчике краша.
void Server::crashShutdown() {
    // Останавливаем accept-луп: новые соединения больше не принимаются.
    running_.store(false, std::memory_order_release);

    // Закрываем сокет прослушивания — это ГЛАВНОЕ: после этого зайти на сервер
    // уже нельзя, даже пока висит 180-секундное окно с краш-репортом.
    if (listenSocket_ != INVALID_SOCK) {
#ifdef _WIN32
        closesocket(listenSocket_);
#else
        ::shutdown(listenSocket_, SHUT_RDWR);
        ::close(listenSocket_);
#endif
        listenSocket_ = INVALID_SOCK;
    }

    // Рвём уже подключённых игроков, чтобы никто не остался на «мёртвом» сервере.
    // try_lock: если краш случился ВНУТРИ секции под connectionsMutex_, обычный
    // lock повесил бы дедлок прямо в обработчике. Не смогли взять — не страшно,
    // listen-сокет уже закрыт, новых входов не будет.
    if (connectionsMutex_.try_lock()) {
        for (auto& [id, conn] : connections_) {
            if (conn) conn->close();
        }
        connectionsMutex_.unlock();
    }
}

void Server::run() {
    while (isRunning()) {
        // Основной цикл тиков — 20 TPS
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void Server::acceptLoop() {
    while (isRunning()) {
        // IPV6_V1: в dual-stack режиме адрес клиента может быть и v4, и v6
        sockaddr_storage clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);

        socket_t clientSocket = ::accept(listenSocket_,
            reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);

        if (clientSocket == INVALID_SOCK) {
            if (isRunning()) {
                NC_WARN("Net", "accept() returned an invalid socket"); // LANGFIX_V1
            }
            continue;
        }

        u64 id = nextConnectionId_.fetch_add(1, std::memory_order_relaxed);
        auto conn = std::make_shared<Connection>(clientSocket, *this, id);

        // Колбэк при закрытии — уведомляем сервер
        conn->setOnClose([this](u64 connId) {
            if (onDisconnect_) {
                auto c = getConnection(connId);
                if (c) onDisconnect_(c);
            }
            removeConnection(connId);
        });

        {
            std::lock_guard lock(connectionsMutex_);
            connections_[id] = conn;
        }
        connectionCount_.fetch_add(1, std::memory_order_relaxed);

        if (onConnect_) {
            onConnect_(conn);
        }

        // STRESSHARDEN_V1: под штурмом 300 ботов + churn поток-на-соединение упирается
        // в лимиты ОС (память под стеки / хендлы), а любой throw внутри read-цикла или
        // неудачный спавн std::thread раньше вылетал наружу из acceptLoop (отдельный поток) и
        // НЕ ловился нигде -> std::terminate -> весь сервер падал (в стресс-тесте
        // "процесс не найден, возможен краш"). Теперь: (1) тело потока обёрнуто
        // try/catch, (2) сам спавн обёрнут — сбой роняет только ЭТО соединение, а не процесс.
        try {
            std::thread([conn]() {
                try {
                    conn->start();
                } catch (const std::exception& e) {
                    NC_ERROR("Net", "Connection thread crashed (id={}): {}", conn->getId(), e.what());
                    conn->close();
                } catch (...) {
                    NC_ERROR("Net", "Connection thread crashed (id={}): unknown error", conn->getId());
                    conn->close();
                }
            }).detach();
        } catch (const std::exception& e) {
            NC_ERROR("Net", "Failed to spawn connection thread (id={}): {} — dropping this client", id, e.what());
            conn->close();
        }
    }
}

void Server::removeConnection(u64 id) {
    std::lock_guard lock(connectionsMutex_);
    connections_.erase(id);
    connectionCount_.fetch_sub(1, std::memory_order_relaxed);
}

std::shared_ptr<Connection> Server::getConnection(u64 id) {
    std::lock_guard lock(connectionsMutex_);
    auto it = connections_.find(id);
    return it != connections_.end() ? it->second : nullptr;
}

std::vector<std::shared_ptr<Connection>> Server::getAllConnections() {
    std::lock_guard lock(connectionsMutex_);
    std::vector<std::shared_ptr<Connection>> result;
    result.reserve(connections_.size());
    for (auto& [id, conn] : connections_) {
        result.push_back(conn);
    }
    return result;
}

void Server::handleIncomingPacket(std::shared_ptr<Connection> conn, i32 packetId, Buffer& payload) {
    if (onPacket_) {
        onPacket_(conn, payload, packetId);
    }
}

} // namespace nc::net
