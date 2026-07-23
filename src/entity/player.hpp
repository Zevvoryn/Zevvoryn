#pragma once

#include "../core/types.hpp"
#include "../network/connection.hpp"
#include "../world/chunk.hpp"
#include <string>
#include <memory>
#include <unordered_set> // LIGHT_V1

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

    // LIGHT_V1: какие чанки уже отправлены игроку (чтобы не слать их повторно)
    static u64 chunkKey(i32 cx, i32 cz) {
        return (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u64>(static_cast<u32>(cz));
    }
    bool hasSeenChunk(i32 cx, i32 cz) const { return sentChunks_.count(chunkKey(cx, cz)) != 0; }
    void markChunkSeen(i32 cx, i32 cz) { sentChunks_.insert(chunkKey(cx, cz)); }
    void clearSeenChunks() { sentChunks_.clear(); } // RESPAWN_V2: клиент сбрасывает все чанки после Respawn — шлём заново

    // Отправка пакетов
    void sendPacket(protocol::PacketId id, const std::vector<u8>& data);
    void sendSystemMessage(std::string_view message);

    std::shared_ptr<net::Connection> getConnection() { return connection_; }

    // Статистика
    bool isAlive() const { return connection_ && connection_->isConnected(); }

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
    // PLAYER_VIS_V1: состояние приседа/спринта для метаданных сущности
    bool sneaking = false;
    bool sprinting = false;
    // TPS_BOSS_V1: /tps enables a per-player Java bossbar.
    bool tpsBossbarEnabled = true; // TPS_BOSS_V2: TPS check visible on join (/tps toggles it)
    std::vector<u8> encVerifyToken; // ONLINE_V1: verify token sent in EncryptionRequest
    bool tpsBossbarShown = false;

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
    bool dead = false;

    // INVENTORY_CLICK_V1: предмет «в курсоре» при работе с окном инвентаря.
    i32 cursorItemId = 0;
    i32 cursorCount = 0;

    // CHEST_V1: открытое окно контейнера (сундука) и личный эндер-сундук игрока.
    i32 openWindowId = 0;      // 0 = контейнер не открыт
    u64 openContainerKey = 0;  // ключ позиции открытого сундука
    bool openIsEnder = false;  // открыт персональный эндер-сундук
    i32 nextWindowId = 1;      // счётчик id окон (1..99)
    i32 openBlockState = 0;    // CHEST_V2: стейт блока открытого сундука (для анимации крышки)
    i64 lastAttackMs = 0;      // COMBAT_V2: время предыдущего удара (для кулдауна атаки)
    bool usingShield = false;  // SHIELD_V1: щит поднят (активная фаза Use Item)
    i32 usingShieldHand = 0;   // SHIELD_V1: 0 = основная рука, 1 = оффхенд
    bool openIsDouble = false; // CHEST_V3: открыт двойной сундук (54 слота)
    u64 openContainerKey2 = 0; // CHEST_V3: ключ второй половины двойного сундука
    i32 enderItemId[27] = {};  // содержимое эндер-сундука
    i32 enderCount[27] = {};

    // FALLDMG_V1: вершина текущего падения и предыдущее состояние земли.
    f64 fallPeakY = 4.0;
    i64 lastHurtMs = 0;       // IFRAME_V1: когда игрок последний раз получил урон (мс, steady clock)
    bool eggDeepTold = false; // EGG_V1: пасхалка на Y<-1000 уже показана в этом погружении
    bool fallWasOnGround = true;

    // MP_V1: последняя позиция, разосланная другим игрокам (точка отсчёта для delta-move пакетов)
    f64 mpLastX = 0.0;
    f64 mpLastY = 4.0;
    f64 mpLastZ = 0.0;

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
};

} // namespace nc::entity
