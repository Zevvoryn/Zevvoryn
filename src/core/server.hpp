#pragma once

#include "../network/server.hpp"
#include "../protocol/codec_1_21_1/codec_1_21_1.hpp"
#include "../entity/player.hpp"
#include "../world/chunk.hpp"
#include "../core/config.hpp"
#include "../core/types.hpp"
#include <memory>
#include <unordered_map>
#include <string>
#include <mutex>
#include <atomic>
#include <deque>
#include <thread>              // CHATASYNC_V1
#include <condition_variable>  // CHATASYNC_V1

namespace nc {

// ============================================================
// Главный класс сервера NetherCraft.
// Связывает сеть, протокол, мир, игроков.
// ============================================================

class NetherCraftServer {
public:
    NetherCraftServer();
    ~NetherCraftServer();

    bool start(const std::string& configPath = "settings.properties");
    bool startWithConfig(const ServerConfig& cfg);
    void stop();
    void softReload(); // SOFTRELOAD_V1: мягкий рестарт без завершения процесса (только tick-поток)
    void run();

    // CONSOLE_V2: main thread queues console input; it is executed safely on server tick.
    void queueConsoleCommand(std::string command);
    bool isRunning() const { return network_.isRunning(); }

    // Доступ к подсистемам
    net::Server& getNetwork() { return network_; }
    world::World& getWorld() { return world_; }
    ServerConfig& getConfig() { return config_; }
    protocol::v1_21_1::Codec_1_21_1& getCodec() { return codec_; }

private:
    bool startCommon();

    // Обработка событий сети
    void onPlayerConnect(std::shared_ptr<net::Connection> conn);
    void onPlayerDisconnect(std::shared_ptr<net::Connection> conn);
    void onPacketReceived(std::shared_ptr<net::Connection> conn, net::Buffer& data, i32 packetId);

    // Обработка пакетов по состояниям
    void handleHandshake(std::shared_ptr<entity::Player> player, net::Buffer& data);
    void handleStatus(std::shared_ptr<entity::Player> player, net::Buffer& data, i32 wireId);
    void handleLogin(std::shared_ptr<entity::Player> player, net::Buffer& data, i32 wireId);
    void handleConfiguration(std::shared_ptr<entity::Player> player, net::Buffer& data, i32 wireId);
    void handlePlay(std::shared_ptr<entity::Player> player, net::Buffer& data, i32 wireId);

    // Отправка пакетов клиенту
    void sendStatusResponse(std::shared_ptr<entity::Player> player);
    void sendLoginSuccess(std::shared_ptr<entity::Player> player);
    void sendConfigurationFinish(std::shared_ptr<entity::Player> player);
    void sendInitialConfig(std::shared_ptr<entity::Player> player);
    void sendRegistryData(std::shared_ptr<entity::Player> player);
    void sendJoinPlay(std::shared_ptr<entity::Player> player);
    void sendSpawnPosition(std::shared_ptr<entity::Player> player);
    void sendPlayerPositionAndLook(std::shared_ptr<entity::Player> player);
    void sendChunksAround(std::shared_ptr<entity::Player> player, i32 centerX, i32 centerZ, i32 radius, i32 maxChunks = 9); // CLIENT_BATCH_V1
    void sendPlayerAbilities(std::shared_ptr<entity::Player> player);
    void sendTimeUpdate(std::shared_ptr<entity::Player> player);
    void sendKeepAlive(std::shared_ptr<entity::Player> player);

    // MP_V1: мультиплеер — видимость и синхронизация игроков между собой
    void broadcastToOthers(const std::shared_ptr<entity::Player>& except, i32 packetId, const std::vector<u8>& payload, bool droppable = false);
    void spawnPlayerFor(const std::shared_ptr<entity::Player>& viewer, const std::shared_ptr<entity::Player>& target);
    void sendPlayerEquipment(const std::shared_ptr<entity::Player>& viewer, const std::shared_ptr<entity::Player>& target); // EQUIP_V1
    void broadcastHeldEquipment(const std::shared_ptr<entity::Player>& player); // EQUIP_V1
    void broadcastEntityMeta(const std::shared_ptr<entity::Player>& player); // PLAYER_VIS_V1
    void despawnPlayerFor(const std::shared_ptr<entity::Player>& viewer, const std::shared_ptr<entity::Player>& target); // PLAYER_VIS_V2
    void spawnItemDrop(f64 x, f64 y, f64 z, i32 itemId, i32 count, f64 vx, f64 vy, f64 vz, i32 pickupDelay = 10); // ITEMDROP_V1
    void tickItemDrops(); // ITEMDROP_V1
    void refreshSpectatorVisibility(const std::shared_ptr<entity::Player>& player, bool wasSpectator, bool isSpectator); // PLAYER_VIS_V2
    void applyGameMode(const std::shared_ptr<entity::Player>& target, i32 mode); // CONSOLE_V3: единая смена режима (команда + консоль)
    void onPlayerEnterPlay(const std::shared_ptr<entity::Player>& player);
    void applyEnvironmentalDamage(const std::shared_ptr<entity::Player>& player,
                                  f32 damage, i32 damageTypeId,
                                  const std::string& deathMessage);
    void applyFallDamage(const std::shared_ptr<entity::Player>& player,
                         f64 newY, bool newOnGround);
    void broadcastPlayerMovement(const std::shared_ptr<entity::Player>& player, bool posChanged, bool rotChanged);
    void broadcastPlayerRemove(const std::shared_ptr<entity::Player>& player);
    void broadcastTabListHeaderFooter(); // TABLIST_COUNT_V1: header/footer таб-листа с числом игроков (RU/EN)

    // Тик
    void tick();
    void tickKeepAlive();
    // TPS_BOSS_V1: Java 1.21.1 Boss Event overlay, sampled once per second.
    void updateTpsBossbar();
    void sendTpsBossbar(const std::shared_ptr<entity::Player>& player, bool add);
    void removeTpsBossbar(const std::shared_ptr<entity::Player>& player);
    void processConsoleCommands(); // CONSOLE_V2

    // WORLDSAVE_V1: данные игроков (world/playerdata/<ник>.txt)
    void savePlayerData(std::shared_ptr<entity::Player> player);
    void loadPlayerData(std::shared_ptr<entity::Player> player);
    void streamChunks(std::shared_ptr<entity::Player> player); // FLATWORLD_V1

    // CHEST_V1: серверные контейнеры сундуков (27 слотов) по позиции блока.
    struct ChestData { i32 itemId[27] = {}; i32 count[27] = {}; };
    std::unordered_map<u64, ChestData> chests_;
    std::mutex chestsMutex_;
    void openChestFor(const std::shared_ptr<entity::Player>& player, i32 bx, i32 by, i32 bz, bool isEnder); // CHEST_V1
    void sendContainerContent(const std::shared_ptr<entity::Player>& player); // CHEST_V1
    int countChestViewers(u64 key, bool ender); // CHEST_V2: сколько игроков смотрит в сундук с данным ключом
    void broadcastChestLid(i32 bx, i32 by, i32 bz, i32 blockState, i32 viewers); // CHEST_V2: анимация крышки (Block Action 0x08)
    void broadcastBlockSound(const char* name, i32 bx, i32 by, i32 bz, f32 volume, f32 pitch); // CHEST_V2: звук (Sound Effect 0x68)
    void handleChestWindowClosed(const std::shared_ptr<entity::Player>& player); // CHEST_V2: игрок закрыл окно сундука
    void broadcastHandState(const std::shared_ptr<entity::Player>& player); // SHIELD_V1: метаданные руки (щит активен)

    net::Server network_;
    protocol::v1_21_1::Codec_1_21_1 codec_;
    world::World world_;
    ServerConfig config_;
    std::string configPath_; // SOFTRELOAD_V1: чтобы /reload мог перечитать конфиг
    std::string iconFavicon_; // ICON_V1: data:image/png;base64,... для Status Response

    // Игроки по socket ID
    std::unordered_map<u64, std::shared_ptr<entity::Player>> players_;
    std::mutex playersMutex_;

    std::atomic<u64> nextEntityId_{1}; // STRESS_FIX_V1: было plain u64 — гонка при параллельных onPlayerConnect
    std::atomic<bool> tabListDirty_{false}; // STRESS_FIX_V1: копим join/leave за тик, рассылаем раз в тик пачкой
    // ENTITIES_V1: не-игроковые сущности (демо-пайплайн spawn/despawn + резюме для новых игроков)
    struct SpawnedEntity { i32 eid; i32 typeId; f64 x; f64 y; f64 z; };
    std::vector<SpawnedEntity> entities_;
    std::mutex entitiesMutex_;
    // ITEMDROP_V1: выпавшие предметы — физика (гравитация/пол/стены), подбор игроками, деспавн через 5 минут
    struct ItemDrop { i32 eid; i32 itemId; i32 count; f64 x; f64 y; f64 z; f64 vx; f64 vy; f64 vz; i32 age; i32 pickupDelay; };
    std::vector<ItemDrop> itemDrops_;
    std::mutex itemDropsMutex_;
    i32 tickCounter_ = 0;
    std::chrono::steady_clock::time_point tpsSampleStart_{};
    i32 tpsSampleTicks_ = 0;
    f32 tps_ = 20.0f;
    std::chrono::steady_clock::time_point lastLowTpsWarn_{}; // TPSCHAT_V1: троттлинг алерта о лагах в чат

    // HUD_V1: real OS-reported RAM/CPU for this process, sampled once per second
    // alongside the TPS boss bar, so the in-game bar shows actual numbers instead
    // of only a tick-rate estimate. See sampleProcessStats() in server.cpp.
    f32 ramMb_ = 0.0f;
    f32 cpuPercent_ = 0.0f;
    u64 lastCpuTotal100ns_ = 0;
    std::chrono::steady_clock::time_point lastCpuSampleTime_{};
    void sampleProcessStats();

    // Все игроки (копия)
    std::vector<std::shared_ptr<entity::Player>> getAllPlayersCopy();

    // Keep alive
    // KEEPALIVE_TIMEOUT_V1: был мёртвый общий вектор pendingKeepAlives_, который никто не читал и не чистил
    // (чистая утечка) — теперь состояние keep-alive хранится на самом Player.
    i64 lastKeepAliveTime_ = 0;

    // CONSOLE_V2: only queue access happens across threads; world/player work runs in tick().
    std::deque<std::string> consoleCommands_;
    std::mutex consoleMutex_;

    // CHATASYNC_V1: chat broadcast used to run inline on the tick thread — for
    // every chat line it looped over all players doing a blocking ::send() each,
    // so on a full server a single message could stall the whole tick loop
    // (visible lag / TPS dip). Now the tick/packet thread just drops the fully
    // formatted line into this queue and returns instantly; a dedicated worker
    // thread does the actual per-player sends off the hot path.
    void chatWorkerLoop();
    void enqueueChatBroadcast(std::string line);
    std::thread chatThread_;
    std::deque<std::string> chatQueue_;
    std::mutex chatMutex_;
    std::condition_variable chatCv_;
    std::atomic<bool> chatRunning_{false};

    // ASYNCSAVE_V1: автосейв уехал в фоновый поток — тик #6000 больше не стоит ~280мс
    // на сериализации мира и записи на диск.
    std::thread saveThread_;
    std::atomic<bool> saveBusy_{false};
    std::atomic<bool> stoppedOnce_{false}; // STOPONCE_V1: stop() выполняется только один раз
};

} // namespace nc
