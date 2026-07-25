#pragma once

#include "connection.hpp"
#include "../core/types.hpp"
#include "../core/log.hpp"
#include <thread>
#include <atomic>
#include <vector>
#include <functional>

namespace nc::net {

// ============================================================
// TCP сервер — слушает порт, принимает соединения.
// Все I/O — синхронное в отдельных потоках (модель PocketMine).
// ============================================================

class Server {
public:
    Server() = default;
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Запуск на порту. Блокирующий!
    bool start(u16 port, i32 backlog = 512); // STRESS_V2: очередь accept под штурм 300 ботов (было 128 — WinError 10054 у клиентов)
    void stop();
    void run(); // Основной цикл

    // CRASHNET_V1: аварийное отключение сети из обработчика краша.
    // Вызывается прямо из crashNote() (main.cpp) ДО 180-секундного окна ожидания.
    // Мгновенно уводит сервер оффлайн: закрывает listen-сокет (новые игроки уже
    // не зайдут) и рвёт все текущие соединения. Не джойнит потоки и берёт
    // connectionsMutex_ через try_lock — безопасно звать из любого падающего потока.
    void crashShutdown();

    // Обработчики событий
    void onConnection(ConnectionHandler handler) { onConnect_ = std::move(handler); }
    void onPacket(PacketHandler handler) { onPacket_ = std::move(handler); }
    void onDisconnect(DisconnectHandler handler) { onDisconnect_ = std::move(handler); }

    // Управление соединениями
    void removeConnection(u64 id);
    std::shared_ptr<Connection> getConnection(u64 id);
    std::vector<std::shared_ptr<Connection>> getAllConnections();

    bool isRunning() const { return running_.load(std::memory_order_relaxed); }
    u64 getConnectionCount() const { return connectionCount_.load(std::memory_order_relaxed); }

    // Вызывается из Connection::processPacket
    void handleIncomingPacket(std::shared_ptr<Connection> conn, i32 packetId, Buffer& payload);

private:
    void acceptLoop();

    socket_t listenSocket_ = INVALID_SOCK;
    std::atomic<bool> running_{false};
    std::atomic<u64> nextConnectionId_{1};
    std::atomic<u64> connectionCount_{0};

    std::thread acceptThread_;

    // Соединения
    std::mutex connectionsMutex_;
    std::unordered_map<u64, std::shared_ptr<Connection>> connections_;

    // Обработчики
    ConnectionHandler onConnect_;
    PacketHandler onPacket_;
    DisconnectHandler onDisconnect_;
};

} // namespace nc::net
