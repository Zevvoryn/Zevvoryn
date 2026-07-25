#pragma once

#include "../../core/types.hpp"
#include "../../network/buffer.hpp"
#include <functional>
#include <memory>
#include <unordered_map>
#include <string>

namespace nc::protocol {

// Направление пакета
enum class PacketDirection : u8 {
    Serverbound, // Клиент → Сервер
    Clientbound, // Сервер → Клиент
};

// ConnectionState определён в nc:: (types.hpp) — используем его
using nc::ConnectionState;

// Внутренний ID пакета. Используется модулями игры.
// НЕ совпадает с wire ID протокола — маппинг делает Codec.
enum class PacketId : u32 {
    None = 0,

    // === Handshake ===
    Handshake,

    // === Status ===
    StatusRequest,
    StatusResponse,
    StatusPing,

    // === Login ===
    LoginStart,
    LoginDisconnect,
    LoginSuccess,
    LoginEncryptionRequest,
    LoginEncryptionResponse,
    LoginCompression,

    // === Configuration ===
    ConfigurationFinish,
    ConfigurationRegistryData,
    ConfigurationPluginMessage,
    ConfigurationFinishAcknowledged,
    ConfigurationClientInformation,
    ConfigurationPluginMessageServerbound,

    // === Play: general ===
    PlayKeepAlive,
    PlayChunkDataAndUpdateLight,
    PlayUnloadChunk,
    PlayPositionAndLook,
    PlaySpawnPosition,
    PlayPlayerAbilities,
    PlayPlayerInfo,
    PlayPlayerListItem,
    PlayUpdateViewPosition,
    PlayUpdateViewDistance,
    PlaySetCenterChunk,
    PlayChatMessage,
    PlaySystemChatMessage,
    PlayDisconnect,
    PlayGameEvent,

    // === Play: world ===
    PlayBlockUpdate,
    PlayMultiBlockChange,

    // === Play: entities ===
    PlaySpawnEntity,
    PlaySpawnPlayer,
    PlayEntityAnimation,
    PlayEntityPosition,
    PlayEntityPositionAndRotation,
    PlayEntityRotation,
    PlayEntityHeadRotation,
    PlayEntityMetadata,
    PlayEntityVelocity,
    PlayEntityTeleport,
    PlayDestroyEntities,

    // === Play: inventory ===
    PlayWindowItems,
    PlaySetSlot,
    PlaySetContainerContent,
    PlayOpenScreen,

    // === Play: world border ===
    PlayWorldBorderInitialize,
    PlayWorldBorderCenter,
    PlayWorldBorderSize,

    // === Play: time ===
    PlayTimeUpdate,

    // === Play: scoreboard ===
    PlayScoreboardObjective,
    PlayUpdateScore,
    PlayDisplayObjective,

    // === Play: tab list ===
    PlayPlayerListHeaderAndFooter,

    // === Play: boss bar ===
    PlayBossBar,

    // === Play: combat ===
    PlayCombatEvent,

    // === Play: title ===
    PlayTitle,
    PlayTitleSubTitle,
    PlayTitleTimes,

    // === Play: entity effects ===
    PlayEntityEffect,

    // === Serverbound Play ===
    PlayServerboundChat,
    PlayServerboundCommandSuggestions,
    PlayServerboundCommand,
    PlayServerboundClickWindow,
    PlayServerboundCloseWindow,
    PlayServerboundPlayerMovement,
    PlayServerboundPlayerPosition,
    PlayServerboundPlayerPositionAndRotation,
    PlayServerboundPlayerRotation,
    PlayServerboundPlayerOnGround,
    PlayServerboundPlayerAbilities,
    PlayServerboundHeldItemChange,
    PlayServerboundCreativeInventoryAction,
    PlayServerboundSetBeaconEffect,
    PlayServerboundCloseContainer,
    PlayServerboundSetCreativeSlot,
    PlayServerboundKeepAlive,
    PlayServerboundPong,

    PacketId_MAX
};

// Описание пакета
struct PacketInfo {
    std::string name;
    std::string displayName;
    PacketDirection direction;
    nc::ConnectionState state;
};

// ============================================================
// Интерфейс кодека протокола.
// Каждая версия Minecraft (1.21.1, 1.20.6, ...) реализует этот интерфейс.
// Это ключевой интерфейс для ViaVersion-трансляции.
// ============================================================

// Десериализатор: из буфера в структуру пакета (данные уже прочитаны из wire)
using PacketDeserializer = std::function<std::vector<u8>(net::Buffer&)>;
// Сериализатор: из внутреннего формата в буфер для отправки
using PacketSerializer   = std::function<void(net::Buffer&, const std::vector<u8>& data)>;

struct PacketCodecEntry {
    PacketId internalId;
    std::string name;
    PacketSerializer serializer;
    PacketDeserializer deserializer;
};

// Результат декодирования пакета из wire
struct DecodedPacket {
    PacketId id;
    std::vector<u8> payload; // Сериализованные данные пакета (без заголовка)
    i32 protocolVersion;
};

// Результат кодирования пакета для отправки в wire
struct EncodedPacket {
    i32 packetId; // Wire ID
    std::vector<u8> payload;
};

// ============================================================
// Абстрактный кодек протокола.
// Каждая реализация привязана к одной версии Minecraft.
// ============================================================

class ProtocolCodec {
public:
    virtual ~ProtocolCodec() = default;

    // Версия протокола (767 = 1.21.1)
    virtual i32 getProtocolVersion() const = 0;
    virtual std::string_view getVersionString() const = 0;

    // Декодирование: wire bytes → внутренний PacketId + payload
    virtual std::expected<DecodedPacket, std::string>
    decode(net::Buffer& buffer, ConnectionState state) = 0;

    // Кодирование: внутренний PacketId + payload → wire bytes
    virtual std::expected<EncodedPacket, std::string>
    encode(PacketId id, const std::vector<u8>& data, ConnectionState state) = 0;

    // Регистрация пакетов для данного состояния
    virtual void registerPackets() = 0;

    // Получить описание пакета по внутреннему ID
    virtual const PacketInfo* getPacketInfo(PacketId id) const = 0;

    // Маппинг ID: wire ID → internal ID для конкретного состояния
    virtual PacketId mapWireToInternal(i32 wireId, PacketDirection dir, ConnectionState state) const = 0;
    virtual i32 mapInternalToWire(PacketId id, PacketDirection dir, ConnectionState state) const = 0;
};

} // namespace nc::protocol
