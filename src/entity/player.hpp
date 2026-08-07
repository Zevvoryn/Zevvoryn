#pragma once

#include "../core/types.hpp"
#include "../network/connection.hpp"
#include "../world/chunk.hpp"
#include <string>
#include <memory>
#include <vector>        // INVENTORY_V4: effects + mode 5 drag slot list
#include <unordered_set> // LIGHT_V1
#include <mutex>         // CHUNKVIS_V1

namespace nc::protocol { enum class PacketId : u32; }

namespace nc::entity {

// ============================================================
// Игрок — хранит состояние конкретного подключённого игрока.
// ============================================================

enum class PlayerState : u8 {
    Handshaking,
    Status,
    Login,
    Configuration,
    Play,
    Disconnected,
};

class Player : public std::enable_shared_from_this<Player> {
public:
    Player(u64 entityId, std::shared_ptr<net::Connection> connection)
        : entityId_(entityId), connection_(std::move(connection)) {}

    u64 getEntityId() const { return entityId_; }
    const std::string& getName() const { return name_; }
    const UUID& getUuid() const { return uuid_; }
    PlayerState getState() const { return state_; }

    void setName(std::string_view name) { name_ = name; }
    void setUuid(const UUID& uuid) { uuid_ = uuid; }
    void setState(PlayerState state) { state_ = state; }

    // Позиция в мире
    f64 getX() const { return posX_; }
    f64 getY() const { return posY_; }
    f64 getZ() const { return posZ_; }
    f32 get_yaw() const { return yaw_; }
    f32 get_pitch() const { return pitch_; }
    bool isOnGround() const { return onGround_; }

    void setPosition(f64 x, f64 y, f64 z) { posX_ = x; posY_ = y; posZ_ = z; }
    void setRotation(f32 yaw, f32 pitch) { yaw_ = yaw; pitch_ = pitch; }
    void setOnGround(bool onGround) { onGround_ = onGround; }

    // Просматриваемый чанк
    i32 getViewCenterX() const { return viewCenterChunkX_; }
    i32 getViewCenterZ() const { return viewCenterChunkZ_; }
    void setViewCenter(i32 cx, i32 cz) { viewCenterChunkX_ = cx; viewCenterChunkZ_ = cz; }

    // KEEPALIVE_TIMEOUT_V1: ждём ли мы ответ на последний Keep Alive (чтобы рвать соединения
    // призраков, которые не отвечают серверу — раньше эти сокеты/буферы висели в памяти вечно)
    i64 pendingKeepAliveId = 0;
    bool awaitingKeepAlive = false;
    i64 keepAliveSentAtMs = 0;
    // PINGSTAT_V1: последний измеренный round-trip игрока в мс (-1 = ещё не знаем).
    // Считается по ответу на Keep Alive: id пакета и есть метка отправки.
    i32 pingMs = -1;

    // COMBAT_V8: ванильные Enter/End Combat Event (0x3B / 0x3A)
    bool inCombat = false;
    i64 combatStartMs = 0;

    // LIGHT_V1: какие чанки уже отправлены игроку (чтобы не слать их повторно)
    static u64 chunkKey(i32 cx, i32 cz) {
        return (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u64>(static_cast<u32>(cz));
    }
    bool hasSeenChunk(i32 cx, i32 cz) const { return sentChunks_.count(chunkKey(cx, cz)) != 0; }
    i32 dimension = 0; // MULTIWORLD_V1: 0 = overworld, 1 = nether, 2 = end
    void markChunkSeen(i32 cx, i32 cz) { sentChunks_.insert(chunkKey(cx, cz)); }
    void forgetChunk(i32 cx, i32 cz) { sentChunks_.erase(chunkKey(cx, cz)); }
    void clearSeenChunks() { sentChunks_.clear(); } // RESPAWN_V2: клиент сбрасывает все чанки после Respawn — шлём заново

    // TPFIX_V2: Teleport ID больше не константа 1. Клиент отбрасывает Confirm Teleport
    // с чужим id, а сервер до подтверждения обязан игнорировать позиции клиента —
    // иначе после дальнего /tp игрока «резинит» назад в старую точку.
    i32 nextTeleportId() { if (++teleportIdCounter_ <= 0) teleportIdCounter_ = 1; pendingTeleportId = teleportIdCounter_; awaitingTeleport = true; return teleportIdCounter_; }
    i32 pendingTeleportId = 0;
    bool awaitingTeleport = false;

    // CHUNKVIS_V1: какие сущности-игроки сейчас ЗАСПАВНЕНЫ у этого игрока (для видимости по чанкам).
    // Под мьютексом: сет трогают read-потоки РАЗНЫХ игроков одновременно (в отличие от sentChunks_).
    bool hasVisibleEntity(u64 eid) const { std::lock_guard<std::mutex> lk(visMutex_); return visibleEntities_.count(eid) != 0; }
    bool addVisibleEntity(u64 eid) { std::lock_guard<std::mutex> lk(visMutex_); return visibleEntities_.insert(eid).second; }
    bool removeVisibleEntity(u64 eid) { std::lock_guard<std::mutex> lk(visMutex_); return visibleEntities_.erase(eid) != 0; }
    void clearVisibleEntities() { std::lock_guard<std::mutex> lk(visMutex_); visibleEntities_.clear(); }

    // Отправка пакетов
    void sendPacket(protocol::PacketId id, const std::vector<u8>& data);
    void sendSystemMessage(std::string_view message);
    void kick(std::string_view reason); // KICKFIX_V1: proper Disconnect (0x1D) + close, not a raw drop

    std::shared_ptr<net::Connection> getConnection() { return connection_; }

    // Статистика
    bool isAlive() const { return connection_ && connection_->isConnected(); }

    // I18N_V1: язык клиента из ClientInformation ("ru_ru", "en_us", ...).
    // Используется для ответов команд; по умолчанию — английский.
    std::string clientLocale = "en_us";

    // BLOCKS_V1: выбранный слот хотбара и блоки в хотбаре (для креатива)
    i32 heldSlot = 0;
    i32 hotbarBlockState[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};

    // INVENTORY_V3: полный инвентарь окна игрока (46 слотов):
    //   0 = результат крафта, 1-4 = сетка крафта, 5-8 = броня,
    //   9-35 = рюкзак, 36-44 = хотбар, 45 = оффхенд.
    // itemId 0 или count 0 = пустой слот. Нумерация совпадает с Set Creative Mode Slot.
    static constexpr int INV_SIZE = 46;
    i32 invItemId[INV_SIZE] = {};
    i32 invCount[INV_SIZE] = {};
    i32 invDamage[INV_SIZE] = {}; // DURABILITY_V1: minecraft:damage per slot
    std::string invCustomName[INV_SIZE]; // ALLPACKETS_V3: Edit Book / Rename Item persist a real per-slot custom name
    bool builderWandOwned = false; // MINIEDIT_V4: persists named wand across reconnects
    // ALLPACKETS_V3: real per-player recipe book UI state (Recipe Book Settings / Seen Recipe)
    bool recipeBookOpen = false;
    bool recipeBookFilterActive = false;
    std::unordered_set<std::string> seenRecipes;
    // PLAYER_VIS_V1: состояние приседа/спринта для метаданных сущности
    bool sneaking = false;
    bool sprinting = false;
    // ELYTRA_V1: authoritative glide state; toggled by Player Command action 8
    // only when the chest slot contains a verified elytra item.
    bool elytraFlying = false;
    // MOBS_AI_V1: общие i-frames от ударов мобов (ванильные 10 тиков):
    // без них стая била каждым мобом отдельно и сносила игрока за секунду.
    i32 mobHurtCooldown = 0;
    // VILLAGER_TRADE_V1: eid жителя, чьё окно торговли сейчас открыто (0 = нет).
    i32 openMerchantEid = 0;
    // RAID_WAVES_V1: уровень Дурного предзнаменования после убийства капитана патруля.
    i32 badOmen = 0;
    // EFFECTS_V2: LivingEntity.activeEffects — серверное хранилище эффектов игрока.
    // Раньше эффект только уходил клиенту пакетом и ни на что не влиял.
    struct ActiveEffect { i32 id = 0; i32 amplifier = 0; i32 ticks = 0; };
    std::vector<ActiveEffect> effects;
    i32 effectPulse = 0;   // общий счётчик для периодических эффектов
    // TPS_BOSS_V1: /tps enables a per-player Java bossbar.
    bool tpsBossbarEnabled = false; // TPS_BOSS_V4: opt-in only; bar goes only to players who toggled /tps
    std::vector<u8> encVerifyToken; // ONLINE_V1: verify token sent in EncryptionRequest
    bool tpsBossbarShown = false;
    i32 tpsBossbarColor = -1; // BOSSCOLOR_V1: последний отправленный цвет бара (при смене шлём UPDATE_STYLE)
    // JOINSAFE_V1: true только после того, как клиенту ушёл Login (Play) и у него есть мир.
    // До этого любой мировой пакет = NullPointerException и Network Protocol Error.
    bool playReady = false;
    bool tabHeaderFooterSent = false; // TABLIST_V2: новый/перезашедший игрок должен получить header/footer даже без смены payload

    // SKIN_V1: game-profile "textures" property (skin + cape). Forwarded to other
    // players in Player Info Update so skins/capes render in multiplayer. Filled
    // from Mojang session (online) or fetched by name (offline).
    std::string texturesValue;      // base64 "textures" value
    std::string texturesSignature;  // Yggdrasil signature (empty in offline mode)
    // SKIN_V1: displayed skin layers bitmask (hat/jacket/sleeves/pants/cape) from
    // Client Information; relayed via entity metadata so layers & capes show.
    u8 displayedSkinParts = 0x7F;   // default: all layers enabled

    // GM_V1: режим игры конкретного игрока (0=survival 1=creative 2=adventure 3=spectator)
    i32 gameMode = 1;

    // COMBAT_V1: PvP — здоровье (20.0 = 10 сердец) и флаг смерти игрока.
    f32 health = 20.0f;
    f32 absorptionAmount = 0.0f; // EFFECTS_V3: visible golden absorption hearts
    bool dead = false;
    // FOOD_V1: vanilla hunger, saturation, exhaustion and natural regeneration.
    i32 foodLevel = 20;
    f32 foodSaturation = 5.0f;
    f32 foodExhaustion = 0.0f;
    i32 foodTickTimer = 0;
    bool usingFood = false;
    i32 usingFoodSlot = -1;
    i32 usingFoodItem = 0;
    i32 usingFoodTicks = 0;
    i32 usingFoodHand = 0;

    // ENV_V1: урон окружением — заморозка в рыхлом снегу и урон лавой.
    i32 ticksFrozen = 0;       // 0..140; при 140 — урон морозом (ванильный TICKS_REQUIRED_TO_FREEZE)
    i32 frozenSynced = -1;     // последнее отправленное клиенту значение (метаданные 0x58)
    i32 lavaHurtCooldown = 0;  // i-frames урона лавой (тики)
    i32 contactHurtCooldown = 0;
    i32 fireHurtCooldown = 0; // FIRE_V2: периодический урон после выхода из блока огня // ENV_PHYSICS_V2: cactus/fire/magma/berry i-frames
    f64 envLastX = 0.0;
    f64 envLastZ = 0.0;
    bool envPositionReady = false;
    i32 respawnInvulnerabilityTicks = 0; // RESPAWN_INVULN_V1: 60 тиков полной защиты после смерти
    i32 remainingFireTicks = 0; // LAVAFIRE_V1: ванильные 15 секунд горения после лавы
    bool fireFlagSynced = false; // последнее отправленное DATA_SHARED_FLAGS.ON_FIRE
    i32 airSupply = 300;       // ENVWATER_V1: запас воздуха 0..300 (пузыри над хотбаром)
    i32 airSynced = -999;      // последнее отправленное клиенту значение воздуха
    f32 armorSynced = -1.0f;   // ENVARMOR_V1: последнее отправленное клиенту значение брони

    // INVENTORY_CLICK_V1: предмет «в курсоре» при работе с окном инвентаря.
    i32 cursorItemId = 0;
    i32 cursorCount = 0;
    // INVENTORY_V4: курсор — полноценный стак. Без этих полей любой перенос предмета
    // через курсор обнулял minecraft:damage, то есть «ремонтировал» инструмент, и терял
    // кастомное имя из Rename Item / Edit Book.
    i32 cursorDamage = 0;
    std::string cursorCustomName;
    // INVENTORY_V4: состояние протягивания (mode 5). Клиент обрамляет протягивание
    // кадрами со slot == -999 (button 0/4/8 — начало, 2/6/10 — конец) и между ними
    // присылает закрашенные слоты. Раньше эти -999 кадры попадали в ветку «клик мимо
    // окна» и сервер выбрасывал стак с курсора на землю.
    i32 dragKind = -1;            // -1 = нет протягивания, 0 = ЛКМ (разделить), 1 = ПКМ (по одному), 2 = креатив
    std::vector<i32> dragSlots;   // закрашенные слоты текущего протягивания

    // CHEST_V1: открытое окно контейнера (сундука) и личный эндер-сундук игрока.
    i32 openWindowId = 0;      // 0 = контейнер не открыт
    u64 openContainerKey = 0;  // ключ позиции открытого сундука
    bool openIsEnder = false;  // открыт персональный эндер-сундук
    i32 nextWindowId = 1;      // счётчик id окон (1..99)
    i32 openBlockState = 0;    // CHEST_V2: стейт блока открытого сундука (для анимации крышки)
    i64 lastAttackMs = 0;      // COMBAT_V2: время предыдущего удара (для кулдауна атаки)
    bool digging = false;       // MINING_V1: server-authoritative destroy progress
    i32 digX = 0, digY = 0, digZ = 0;
    i32 digState = 0;
    i32 digStartTick = 0;
    i32 digExpectedTicks = 0;
    i32 digSequence = 0;      // MINING_V3: acknowledge the original START sequence
    bool digCorrectTool = true;
    // MINING_V8: последняя отправленная зрителям стадия трещин (0..9), -1 = ничего не шлём.
    // Без Set Block Destroy Stage клиент рисовал только свою предсказанную анимацию,
    // и если сервер считал дольше, картинка «замирала», а потом блок внезапно ломался.
    i32 digLastStage = -1;
    // MINING_V6: /digdebug — печатает игроку расчёт времени добычи для блока,
    // по которому он начал копать. Нужен, чтобы спорные случаи обсуждать
    // по цифрам, а не по ощущениям. По умолчанию выключен, лог не спамит.
    bool digDebug = false;
    bool usingShield = false;  // SHIELD_V1: щит поднят (активная фаза Use Item)
    i32 usingShieldHand = 0;   // SHIELD_V1: 0 = основная рука, 1 = оффхенд
    i64 shieldRaisedMs = 0;        // SHIELD_V2: когда щит поднят (ванильный прогрев 5 тиков)
    i64 shieldDisabledUntilMs = 0; // SHIELD_V2: щит отключён топором до этого момента
    i32 teleportIdCounter_ = 0;  // TPFIX_V2
    i32 portalCooldownTicks = 0; // PORTAL_V1: тики до следующего перехода (80 = 4 сек)
    i32 portalTimeTicks = 0;     // PORTAL_V1: сколько тиков подряд стоим в портале
    i32 enderPearlCooldownTicks = 0; // PEARL_V3: ровно 20 server ticks, как ItemCooldowns vanilla
    i32 ridingVehicleEid = 0;   // VEHICLE_PHYSICS_V1: 0 = not riding anything
    f32 vehicleForward = 0.0f;  // VEHICLE_PHYSICS_V1: last Player Input (0x26)
    f32 vehicleSideways = 0.0f; // VEHICLE_PHYSICS_V1
    i32 totalExperience = 0;      // XP_BOTTLE_V2: суммарный опыт игрока
    i32 experienceLevel = 0;      // XP_BOTTLE_V2: текущий уровень
    f32 experienceProgress = 0.0f; // XP_BOTTLE_V2: прогресс 0..1 до следующего уровня
    bool openIsDouble = false; // CHEST_V3: открыт двойной сундук (54 слота)
    i32 openContSlots = 27;    // CONTAINER_V1: слотов у открытого контейнера (27 сундук/бочка, 3 печка, 5 воронка, 9 раздатчик)
    i32 openContKind = 0;      // CONTAINER_V1: тип открытого контейнера (ContainerKind в server.cpp)
    u64 openContainerKey2 = 0; // CHEST_V3: ключ второй половины двойного сундука
    i32 enderItemId[27] = {};  // содержимое эндер-сундука
    i32 enderCount[27] = {};
    bool vehicleSneakLatched = false; // VEHSHIFT_V1
    i64 lastPortalWarnMs = 0;         // PORTALMSG_V1
    i64 lastRailWarnMs   = 0;         // RAILSPAM_V1

    // FALLDMG_V1: вершина текущего падения и предыдущее состояние земли.
    f64 fallPeakY = 4.0;
    i64 lastHurtMs = 0;       // IFRAME_V1: когда игрок последний раз получил урон (мс, steady clock)
    f32 lastDamageTaken = 0.0f; // IFRAME_V2: полный урон последнего удара в окне i-frames (для досчёта разницы)
    bool eggDeepTold = false; // EGG_V1: пасхалка на Y<-1000 уже показана в этом погружении
    bool fallWasOnGround = true;

    // MP_V1: последняя позиция, разосланная другим игрокам (точка отсчёта для delta-move пакетов)
    f64 mpLastX = 0.0;
    f64 mpLastY = 4.0;
    f64 mpLastZ = 0.0;
    // CHUNKVIS_V2: чанк, в котором последний раз пересчитывали видимость (только read-поток игрока)
    i32 mpVisChunkX = 2147483647;
    i32 mpVisChunkZ = 2147483647;

private:
    u64 entityId_;
    std::string name_;
    UUID uuid_{};
    PlayerState state_ = PlayerState::Handshaking;
    std::shared_ptr<net::Connection> connection_;

    f64 posX_ = 0.0;
    f64 posY_ = 4.0; // FLATWORLD_V1: спавн на траве (grass на Y=3)
    f64 posZ_ = 0.0;
    f32 yaw_ = 0.0f;
    f32 pitch_ = 0.0f;
    bool onGround_ = true;

    i32 viewCenterChunkX_ = 0;
    i32 viewCenterChunkZ_ = 0;
    std::unordered_set<u64> sentChunks_; // LIGHT_V1
    std::unordered_set<u64> visibleEntities_; // CHUNKVIS_V1: заспавненные у меня сущности-игроки
    mutable std::mutex visMutex_;             // CHUNKVIS_V1
};

} // namespace nc::entity
