#pragma once

#include "../network/server.hpp"
#include "../protocol/codec_1_21_1/codec_1_21_1.hpp"
#include "../entity/player.hpp"
#include "../entity/mob.hpp" // MOBS_V1
#include "../world/chunk.hpp"
#include "../core/config.hpp"
#include "../core/types.hpp"
#include "../core/miniedit.hpp"
#include "../core/whitelist.hpp" // WHITELIST_V1
#include "../core/rcon.hpp" // RCON_BRIDGE_V1
#include <memory>
#include <future>              // RCON_BRIDGE_V1
#include <unordered_map>
#include <unordered_set>      // FLUID_V1
#include <string>
#include <vector> // SPAWNCFG_V1
#include <mutex>
#include <atomic>
#include <deque>
#include <map>                 // FLUID_V3: scheduled ticks ordered by due tick
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
    // RCON_BRIDGE_V1: optional promise lets the caller (RCON) await this command's console output.
    void queueConsoleCommand(std::string command, std::shared_ptr<std::promise<std::string>> result = nullptr);
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
    bool teleportSafe(const std::shared_ptr<entity::Player>& player, f64 tx, f64 ty, f64 tz, bool snapToSurface); // TPFIX_V1
    void sendCenterChunk(const std::shared_ptr<entity::Player>& player, i32 cx, i32 cz); // TPFIX_V2: Set Center Chunk (0x54)
    void sendFullPlayerInventory(const std::shared_ptr<entity::Player>& player); // INVDIM_V1: полный Set Container Content (0x13) + хотбар
    void pruneAllWorlds(); // MEM_V2: выгрузка далёких чанков во ВСЕХ измерениях
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
    void sendTitleTimes(const std::shared_ptr<entity::Player>& player, i32 fadeIn, i32 stay, i32 fadeOut);
    void sendTitleText(const std::shared_ptr<entity::Player>& player, std::string_view text);
    void sendSubtitleText(const std::shared_ptr<entity::Player>& player, std::string_view text);
    void sendActionBarText(const std::shared_ptr<entity::Player>& player, std::string_view text);
    void sendClearTitles(const std::shared_ptr<entity::Player>& player, bool reset);
    void sendStopSound(const std::shared_ptr<entity::Player>& player);
    void sendCameraPacket(const std::shared_ptr<entity::Player>& player, i32 entityId);
    void sendSimulationDistance(const std::shared_ptr<entity::Player>& player);
    void sendInitialWorldBorder(const std::shared_ptr<entity::Player>& player);
    void sendFacePlayer(const std::shared_ptr<entity::Player>& player, const std::shared_ptr<entity::Player>& target);
    void broadcastEnterCombat(const std::shared_ptr<entity::Player>& player);
    void broadcastEndCombat(const std::shared_ptr<entity::Player>& player, i32 durationTicks);
    void broadcastBlockBreakAnimation(i32 entityId, i32 x, i32 y, i32 z, i8 stage);
    void despawnPlayerFor(const std::shared_ptr<entity::Player>& viewer, const std::shared_ptr<entity::Player>& target); // PLAYER_VIS_V2
    void spawnItemDrop(f64 x, f64 y, f64 z, i32 itemId, i32 count, f64 vx, f64 vy, f64 vz, i32 pickupDelay = 10); // ITEMDROP_V1
    void tickItemDrops(); // ITEMDROP_V1
    void scheduleFallingBlockUpdate(i32 x, i32 y, i32 z, i32 delay = 2); // FALLING_V1
    void tickFallingBlocks(); // FALLING_V1: sand/gravel/concrete powder/anvils
    void solidifyConcretePowderAround(i32 x, i32 y, i32 z); // CONCRETE_V2: water neighbour update
    void scheduleFallingColumnCascade(i32 x, i32 y, i32 z, i32 firstDelay = 2); // FALLING_CHAIN_V3
    void spawnPrimedTnt(f64 x, f64 y, f64 z, i32 ownerEid = 0, i32 fuse = 80, f64 launchImpulse = 0.0); // TNT_V1
    bool primeTntBlock(i32 x, i32 y, i32 z, i32 ownerEid = 0, i32 fuse = 80); // TNT_V1
    void tickPrimedTnt(); // TNT_V1
    i64 tntTotalCount(); // ANTILAG_TNT_V1: combined active (primed) + stationary TNT count
    void explodeAt(f64 x, f64 y, f64 z, f32 radius, i32 sourceEid = 0, i32 ownerEid = 0); // EXPLOSION_V1
    void startBulkTntCollapse(std::span<const std::array<i32, 3>> seeds, i32 ownerEid); // TNT_ANTILAG_V1
    void tickBulkTntCollapse(); // TNT_ANTILAG_V1
    void tickTntLightResync(); // TNT_ANTILAG_V3
    void spawnThrowableProjectile(const std::shared_ptr<entity::Player>& owner, f32 yaw, f32 pitch,
                                  i32 entityTypeId, i32 kind); // PROJECTILE_V2
    void spawnEnderPearl(const std::shared_ptr<entity::Player>& owner, f32 yaw, f32 pitch); // PROJECTILE_V1
    void tickProjectiles(); // PROJECTILE_V2
    void spawnExperienceOrb(f64 x, f64 y, f64 z, i32 amount); // XP_ORB_V1
    void tickExperienceOrbs(); // XP_ORB_V1
    // VEHICLE_PHYSICS_V1: boats (client-authoritative) + minecarts (server-side rail following)
    void spawnVehicle(f64 x, f64 y, f64 z, f32 yaw, i32 typeId, i32 itemId, i32 variant);
    void tickVehicles();
    // MOBS_V1: мирные мобы — спавн вокруг игроков, физика, урон, дроп, молоко/стрижка
    void tickMobs();
    void spawnMobWave();
    void spawnTraderCaravan();   // LLAMA_CARAVAN_V1: странствующий торговец с ламами на привязи
    void spawnMobAt(i32 typeIdx, f64 x, f64 y, f64 z, i32 dim, i32 count, bool asBaby = false); // MOBS_ALL_V1 (SPLIT_V1: asBaby для мелких копий слизней/магма-кубов)
    // MOBS_AI_V1: снаряды мобов — стрелы скелетов, фаерболы гаста/блейза, снежки голема
    void spawnMobProjectile(i32 typeId, i32 dim, i32 ownerEid, f64 x, f64 y, f64 z,
                            f64 vx, f64 vy, f64 vz, f32 damage, bool gravity, bool explosive,
                            i32 targetEid = 0, bool homing = false,       // SHULKER_BULLET_V2
                            i32 effectId = -1, i32 effectAmp = 0, i32 effectDur = 0); // WITCH_POTION_V2
    void tickMobProjectiles();
    // EFFECTS_V1: Update Mob Effect (0x76) — своего списка эффектов на сервере пока нет,
    // клиент сам отрисовывает и применяет движение (левитация, замедление).
    // EFFECTS_V2: серверное хранилище эффектов игрока и их периодика.
    void addPlayerEffect(const std::shared_ptr<entity::Player>& player, i32 effectId,
                         i32 amplifier, i32 durationTicks);
    i32 playerEffectAmplifier(const std::shared_ptr<entity::Player>& player, i32 effectId);
    void removePlayerEffect(const std::shared_ptr<entity::Player>& player, i32 effectId);
    void tickPlayerEffects();   // EFFECTS_APPLY_V1
    void sendMobEffect(const std::shared_ptr<entity::Player>& player, i32 effectId,
                       i32 amplifier, i32 durationTicks);
    // EVOKER_FANGS_V1: клыки как настоящая сущность evoker_fangs (тип 36).
    struct EvokerFang {
        i32 eid = 0; i32 dim = 0;
        f64 x = 0, y = 0, z = 0;
        i32 delay = 0; i32 age = 0;
        f32 damage = 6.0f;
        bool struck = false;
    };
    std::vector<EvokerFang> evokerFangs_;
    std::mutex evokerFangsMutex_;
    void spawnEvokerFangLine(i32 dim, f64 ox, f64 oy, f64 oz, f64 tx, f64 ty, f64 tz, f32 damage);
    void tickEvokerFangs();
    void sendMobsTo(const std::shared_ptr<entity::Player>& player);
    bool mobAttack(const std::shared_ptr<entity::Player>& player, i32 targetEid);
    bool mobInteract(const std::shared_ptr<entity::Player>& player, i32 targetEid);
    bool vehicleInteract(const std::shared_ptr<entity::Player>& player, i32 vehicleEid);
    bool vehicleAttack(const std::shared_ptr<entity::Player>& player, i32 vehicleEid);
    void vehicleDismount(const std::shared_ptr<entity::Player>& player);
    void broadcastVehiclePassengers(i32 vehicleEid, i32 passengerEid);
    void handleVehicleMove(const std::shared_ptr<entity::Player>& player, f64 x, f64 y, f64 z, f32 yaw);
    bool placeVehicleItem(const std::shared_ptr<entity::Player>& player, i32 itemId, i32 bx, i32 by, i32 bz, i32 face);
    i32 computeRailState(i32 x, i32 y, i32 z, i32 state, f32 placerYaw); // RAIL_SHAPE_V1
    void updateRailShapesAround(i32 x, i32 y, i32 z);                   // RAIL_SHAPE_V1
    world::World& worldFor(i32 dim);                                       // MULTIWORLD_V1
    world::World& worldOf(const std::shared_ptr<entity::Player>& player);  // MULTIWORLD_V1
    void ensureDimensionReady(i32 dim);
    void prepareAllDimensions(); // WORLDPREP_V1
    void tickSpawnWarmups();     // SPAWNCFG_V1: обратный отсчёт перед телепортом на спавн
    struct SpawnWarmup {         // SPAWNCFG_V1
        std::string name;
        i32 ticksLeft = 0;
        i32 lastShown = -1;
        f64 sx = 0.0, sy = 0.0, sz = 0.0;
        bool standStill = true;
        bool forceLang = false;
        bool forceRu = false;
        std::string color;
        std::string textCountdown, textDone, textCancelled;
    };
    std::vector<SpawnWarmup> spawnWarmups_; // SPAWNCFG_V1
    void saveWorlds();                                                     // DIMSAVE_V1                                    // MULTIWORLD_V1
    bool travelToDimension(const std::shared_ptr<entity::Player>& player, i32 dim, f64 tx, f64 ty, f64 tz); // MULTIWORLD_V1
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
    bool handleMiniEditCommand(const std::shared_ptr<entity::Player>& player, const std::string& command, bool isOp);
    void publishMiniEditChanges(std::span<const miniedit::BlockChange> changes, bool schedulePhysics = true);
    void flushMiniEditPackets();
    void sendMiniEditOutline(const std::shared_ptr<entity::Player>& player, const miniedit::Selection& selection);
    void tickMiniEditVisuals();
    bool isMiniEditWandHeld(const std::shared_ptr<entity::Player>& player) const;
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
    void extinguishPortalNear(i32 dim, i32 bx, i32 by, i32 bz, i32 brokenState); // PORTALBREAK_V1
    void broadcastBlockIn(i32 dim, i32 x, i32 y, i32 z, i32 state); // PORTAL_V1: ставит блок в СВОЁМ измерении
    void broadcastBlockSound(const char* name, i32 bx, i32 by, i32 bz, f32 volume, f32 pitch); // CHEST_V2: звук (Sound Effect 0x68)
    void handleChestWindowClosed(const std::shared_ptr<entity::Player>& player); // CHEST_V2: игрок закрыл окно сундука
    void broadcastHandState(const std::shared_ptr<entity::Player>& player); // SHIELD_V1: метаданные руки (щит активен)

    net::Server network_;
    protocol::v1_21_1::Codec_1_21_1 codec_;
    world::World world_;
    world::World nether_;      // MULTIWORLD_V1
    world::World end_;         // MULTIWORLD_V1
    bool netherReady_ = false; // MULTIWORLD_V1
    bool endReady_ = false;    // MULTIWORLD_V1
    miniedit::MiniEditManager miniEdit_;
    struct MiniEditPacket { i32 sx; i32 sz; std::vector<u8> payload; };
    std::deque<MiniEditPacket> miniEditPackets_;
    std::mutex miniEditPacketsMutex_;
    ServerConfig config_;
    std::string configPath_; // SOFTRELOAD_V1: чтобы /reload мог перечитать конфиг
    nc::Whitelist whitelist_; // WHITELIST_V1
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
    // MOBS_V1: живые сущности оверворлда (заход 1 — только мирные)
    std::vector<entity::Mob> mobs_;
    std::mutex mobsMutex_;
    // RAID_WAVES_V1: Raid.java — рейд живёт вокруг центра деревни и выдаёт волны
    // по очереди: следующая поднимается, только когда предыдущая выбита.
    struct Raid {
        i32 id = 0; i32 dim = 0;
        f64 x = 0, y = 0, z = 0;
        i32 wave = 0; i32 totalWaves = 3; i32 spawnDelay = 0;
        bool finished = false;
    };
    std::vector<Raid> raids_;
    i32 nextRaidId_ = 1;
    void tickRaids();                                   // RAID_WAVES_V1
    void startRaid(i32 dim, f64 x, f64 y, f64 z);       // RAID_WAVES_V1
    void spawnRaidWave(Raid& raid);                     // RAID_WAVES_V1
    // VILLAGER_TRADE_V1: окно торговли жителя и выполнение выбранной сделки.
    void openVillagerTradeFor(const std::shared_ptr<entity::Player>& player, const entity::Mob& villager);
    bool villagerSelectTrade(const std::shared_ptr<entity::Player>& player, i32 index);
    // FALLING_V1: server-authoritative FallingBlockEntity pipeline.
    struct FallingBlockMotion {
        i32 eid; i32 state; f64 x; f64 y; f64 z; f64 vx; f64 vy; f64 vz;
        i32 time; f64 startY;
        i32 dim = 0; // DIMPHYS_V1: в каком измерении падает блок
    };
    std::vector<FallingBlockMotion> fallingBlocks_;
    std::multimap<i32, u64> fallingQueue_;       // dueTick -> packed BlockPos
    std::unordered_map<u64, i32> fallingDue_;   // earliest scheduled tick
    std::mutex fallingMutex_;
    // TNT_V1: server-authoritative PrimedTnt entities (type 106).
    struct PrimedTntMotion {
        i32 eid; i32 ownerEid; f64 x; f64 y; f64 z; f64 vx; f64 vy; f64 vz; i32 fuse;
    };
    std::vector<PrimedTntMotion> primedTnt_;
    std::mutex primedTntMutex_;
    std::atomic<i64> tntBlockCount_{0}; // ANTILAG_TNT_V1: stationary (placed, unignited) TNT blocks tracked since server start
    struct BulkTntJob {
        std::deque<BlockPos> frontier;
        std::deque<BlockPos> crater;
        std::unordered_map<u64, i32> columnFloor;
        std::unordered_set<u64> touchedChunks;
        i32 ownerEid = 0;
        size_t removed = 0;
        bool scanComplete = false;
    };
    std::deque<BulkTntJob> bulkTntJobs_;
    std::deque<u64> tntLightResync_;
    std::unordered_set<u64> tntLightResyncQueued_;
    // PROJECTILE_V2: common throwable motion; kind 1 = ender pearl (type 32),
    // kind 2 = snowball (type 97), kind 3 = egg (type 28).
    struct ProjectileMotion {
        i32 eid; i32 typeId; i32 kind; i32 ownerEid;
        f64 x; f64 y; f64 z; f64 vx; f64 vy; f64 vz; i32 age;
    };
    std::vector<ProjectileMotion> projectiles_;
    std::mutex projectilesMutex_;
    // MOBS_AI_V1: у снарядов мобов владелец — не игрок, и летать они могут
    // в любом измерении, поэтому свой список, а не projectiles_.
    struct MobProjectile {
        i32 eid; i32 typeId; i32 dim; i32 ownerEid; f64 x; f64 y; f64 z;
        f64 vx; f64 vy; f64 vz; f32 damage; i32 age; bool gravity; bool explosive;
        i32 targetEid = 0;      // SHULKER_BULLET_V2: за кем следит пуля
        bool homing = false;    // SHULKER_BULLET_V2: самонаведение
        i32 effectId = -1;      // WITCH_POTION_V2: эффект при попадании
        i32 effectAmp = 0;
        i32 effectDur = 0;
    };
    std::vector<MobProjectile> mobProjectiles_;
    std::mutex mobProjectilesMutex_;
    // XP_ORB_V1: real vanilla experience orb entities (Spawn Experience Orb 0x02),
    // used instead of an invisible instant grantExperience on impact.
    struct ExperienceOrb {
        i32 eid; f64 x; f64 y; f64 z; f64 vx; f64 vy; f64 vz; i32 amount; i32 age;
    };
    std::vector<ExperienceOrb> xpOrbs_;
    std::mutex xpOrbsMutex_;
    // VEHICLE_PHYSICS_V1: boat (type 10) and minecart (type 69) entities.
    struct VehicleMotion {
        i32 eid; i32 typeId; i32 itemId; i32 variant;
        f64 x; f64 y; f64 z; f32 yaw;
        f64 vx; f64 vy; f64 vz;
        i32 dirX; i32 dirZ; f64 speed;   // minecart rail travel direction + scalar speed
        i32 passengerEid; bool onGround;
        i32 hits = 0;                    // VEHICLE_FIX_V2: boats take more than one punch
    };
    std::vector<VehicleMotion> vehicles_;
    std::mutex vehiclesMutex_;
    // FLUID_V3: ванильные scheduled ticks. У каждой клетки собственный dueTick,
    // а не общий глобальный шаг %5/%30 для всей жидкости сразу.
    std::multimap<i32, u64> fluidQueue_;       // dueTick -> packed BlockPos
    std::unordered_map<u64, i32> fluidDue_;   // earliest scheduled tick per position
    std::mutex fluidMutex_;
    // FIRE_V2: bounded scheduled ticks; age exists only for live fire blocks.
    std::multimap<i32, u64> fireQueue_;
    std::unordered_map<u64, i32> fireDue_;
    std::unordered_map<u64, u8> fireAge_;
    std::mutex fireMutex_;
    void scheduleFluidUpdate(i32 x, i32 y, i32 z, i32 delay = -1); // -1 = delay по типу жидкости
    void scheduleFluidNeighbors(i32 x, i32 y, i32 z); // FLUID_V1: клетка + 6 соседей
    void tickFluids();                                // FLUID_V1: обработать пачку клеток за тик
    void tickFluidsIn(i32 dimIndex, const std::vector<u64>& batch);   // DIMPHYS_V1
    void tickFireIn(i32 dimIndex, const std::vector<u64>& batch);     // DIMPHYS_V1
    void tickRandomBlockUpdatesIn(i32 dimIndex);                      // DIMPHYS_V1
    // PORTAL_V2 / ENDPORTAL_V1: рабочие порталы в Ад и Энд.
    bool tryLightNetherPortal(i32 dim, i32 bx, i32 by, i32 bz, bool dryRun = false);
    bool tryPlaceEnderEye(i32 dim, i32 bx, i32 by, i32 bz);
    void tryCompleteEndPortal(i32 dim, i32 bx, i32 by, i32 bz);
    bool findOrCreateNetherPortal(i32 dim, i32 cx, i32 cy, i32 cz, f64& outX, f64& outY, f64& outZ);
    void buildEndSpawnPlatform();
    void tickPortals();
    void scheduleFireUpdate(i32 x, i32 y, i32 z, i32 delay = 30); // FIRE_V2
    void tickFire();                                      // FIRE_V2: decay/spread/burning blocks
    void tickPlayerEnvironment();                     // ENV_V1: заморозка в снегу + урон лавой по тикам
    // RANDOM_TICK_V1: crop growth, sugar cane/cactus stalk growth, leaf decay, farmland hydration/decay.
    std::unordered_map<u64, u8> stalkGrowth_; // in-memory age counter (0..15) for sugar_cane/cactus, keyed like fluidKey()
    void tickRandomBlockUpdates();
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
    // RCON_BRIDGE_V1: an item optionally carries a promise so RCON can get this command's
    // captured console output back as its response.
    struct ConsoleCommandItem {
        std::string text;
        std::shared_ptr<std::promise<std::string>> result;
    };
    std::deque<ConsoleCommandItem> consoleCommands_;
    std::mutex consoleMutex_;

    // RCON_BRIDGE_V1: RconServer (core/rcon.hpp) existed but was never instantiated/started
    // anywhere — enable-rcon in settings.properties had no effect. Wired up in startCommon().
    nc::rcon::RconServer rcon_;

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
