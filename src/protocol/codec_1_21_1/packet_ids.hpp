#pragma once

#include "../common/protocol_codec.hpp"
#include <unordered_map>

namespace nc::protocol::v1_21_1 {

// ============================================================
// 1.21.1 (protocol 767) — Правильные wire ID из prismarinejs/minecraft-data
// ============================================================

struct PacketMapping {
    i32 wireId;
    PacketId internalId;
    PacketDirection direction;
    ConnectionState state;
};

// Таблица всех пакетов 1.21.1
inline const PacketMapping packetTable[] = {
    // === Handshaking ===
    {0x00, PacketId::Handshake, PacketDirection::Serverbound, ConnectionState::Handshaking},

    // === Status ===
    {0x00, PacketId::StatusRequest,     PacketDirection::Serverbound, ConnectionState::Status},
    {0x00, PacketId::StatusResponse,    PacketDirection::Clientbound, ConnectionState::Status},
    {0x01, PacketId::StatusPing,        PacketDirection::Serverbound, ConnectionState::Status},

    // === Login ===
    {0x00, PacketId::LoginStart,                PacketDirection::Serverbound, ConnectionState::Login},
    {0x02, PacketId::LoginSuccess,              PacketDirection::Clientbound, ConnectionState::Login},
    {0x03, PacketId::LoginCompression,          PacketDirection::Clientbound, ConnectionState::Login},

    // === Configuration (Clientbound) — 1.21.1 ===
    {0x03, PacketId::ConfigurationFinish,             PacketDirection::Clientbound, ConnectionState::Configuration},
    {0x07, PacketId::ConfigurationRegistryData,       PacketDirection::Clientbound, ConnectionState::Configuration},

    // === Configuration (Serverbound) — 1.21.1 ===
    {0x00, PacketId::ConfigurationClientInformation,  PacketDirection::Serverbound, ConnectionState::Configuration},
    {0x02, PacketId::ConfigurationPluginMessageServerbound, PacketDirection::Serverbound, ConnectionState::Configuration},
    {0x03, PacketId::ConfigurationFinishAcknowledged, PacketDirection::Serverbound, ConnectionState::Configuration},

    // === Play (Clientbound) — 1.21.1 — ТОЛЬКО нужные пакеты ===
    {0x26, PacketId::PlayKeepAlive,                 PacketDirection::Clientbound, ConnectionState::Play}, // PKTFIX_V1: было 0x2B — это Login-пакет, а не KeepAlive
    {0x27, PacketId::PlayChunkDataAndUpdateLight,  PacketDirection::Clientbound, ConnectionState::Play},
    {0x21, PacketId::PlayUnloadChunk,               PacketDirection::Clientbound, ConnectionState::Play},
    {0x40, PacketId::PlayPositionAndLook,           PacketDirection::Clientbound, ConnectionState::Play},
    {0x56, PacketId::PlaySpawnPosition,             PacketDirection::Clientbound, ConnectionState::Play},
    {0x38, PacketId::PlayPlayerAbilities,           PacketDirection::Clientbound, ConnectionState::Play},
    {0x3E, PacketId::PlayPlayerInfo,                PacketDirection::Clientbound, ConnectionState::Play},
    {0x54, PacketId::PlayUpdateViewPosition,        PacketDirection::Clientbound, ConnectionState::Play},
    {0x55, PacketId::PlayUpdateViewDistance,        PacketDirection::Clientbound, ConnectionState::Play},
    {0x6C, PacketId::PlaySystemChatMessage,         PacketDirection::Clientbound, ConnectionState::Play},
    {0x1D, PacketId::PlayDisconnect,                PacketDirection::Clientbound, ConnectionState::Play},
    {0x22, PacketId::PlayGameEvent,                 PacketDirection::Clientbound, ConnectionState::Play},
    {0x09, PacketId::PlayBlockUpdate,               PacketDirection::Clientbound, ConnectionState::Play},
    {0x49, PacketId::PlayMultiBlockChange,          PacketDirection::Clientbound, ConnectionState::Play},
    {0x01, PacketId::PlaySpawnEntity,               PacketDirection::Clientbound, ConnectionState::Play},
    {0x01, PacketId::PlaySpawnPlayer,               PacketDirection::Clientbound, ConnectionState::Play}, // PKTFIX_V1: в 1.21.1 отдельного SpawnPlayer нет — игрок спавнится обычным SpawnEntity (0x02 = шарик опыта)
    {0x03, PacketId::PlayEntityAnimation,           PacketDirection::Clientbound, ConnectionState::Play},
    {0x2E, PacketId::PlayEntityPosition,            PacketDirection::Clientbound, ConnectionState::Play},
    {0x30, PacketId::PlayEntityRotation,            PacketDirection::Clientbound, ConnectionState::Play},
    {0x48, PacketId::PlayEntityHeadRotation,        PacketDirection::Clientbound, ConnectionState::Play},
    {0x58, PacketId::PlayEntityMetadata,            PacketDirection::Clientbound, ConnectionState::Play},
    {0x5A, PacketId::PlayEntityVelocity,            PacketDirection::Clientbound, ConnectionState::Play},
    {0x70, PacketId::PlayEntityTeleport,            PacketDirection::Clientbound, ConnectionState::Play},
    {0x42, PacketId::PlayDestroyEntities,           PacketDirection::Clientbound, ConnectionState::Play},
    {0x25, PacketId::PlayWorldBorderInitialize,     PacketDirection::Clientbound, ConnectionState::Play},
    {0x4D, PacketId::PlayWorldBorderCenter,         PacketDirection::Clientbound, ConnectionState::Play},
    {0x4F, PacketId::PlayWorldBorderSize,           PacketDirection::Clientbound, ConnectionState::Play},
    {0x64, PacketId::PlayTimeUpdate,                PacketDirection::Clientbound, ConnectionState::Play},
    {0x6D, PacketId::PlayPlayerListHeaderAndFooter, PacketDirection::Clientbound, ConnectionState::Play},
    {0x0A, PacketId::PlayBossBar,                   PacketDirection::Clientbound, ConnectionState::Play},
    {0x65, PacketId::PlayTitle,                     PacketDirection::Clientbound, ConnectionState::Play},
    {0x63, PacketId::PlayTitleSubTitle,             PacketDirection::Clientbound, ConnectionState::Play},
    {0x66, PacketId::PlayTitleTimes,                PacketDirection::Clientbound, ConnectionState::Play},
    {0x39, PacketId::PlayChatMessage,               PacketDirection::Clientbound, ConnectionState::Play},
    {0x2F, PacketId::PlayEntityPositionAndRotation, PacketDirection::Clientbound, ConnectionState::Play},

    // === Play (Serverbound) — 1.21.1 ===
    {0x06, PacketId::PlayServerboundChat,               PacketDirection::Serverbound, ConnectionState::Play},
    {0x0B, PacketId::PlayServerboundCommandSuggestions,  PacketDirection::Serverbound, ConnectionState::Play},
    {0x04, PacketId::PlayServerboundCommand,             PacketDirection::Serverbound, ConnectionState::Play},
    {0x0E, PacketId::PlayServerboundClickWindow,         PacketDirection::Serverbound, ConnectionState::Play},
    {0x0F, PacketId::PlayServerboundCloseWindow,         PacketDirection::Serverbound, ConnectionState::Play},
    {0x1D, PacketId::PlayServerboundPlayerMovement,      PacketDirection::Serverbound, ConnectionState::Play},
    {0x1A, PacketId::PlayServerboundPlayerPosition,      PacketDirection::Serverbound, ConnectionState::Play},
    {0x1B, PacketId::PlayServerboundPlayerPositionAndRotation, PacketDirection::Serverbound, ConnectionState::Play},
    {0x1C, PacketId::PlayServerboundPlayerRotation,      PacketDirection::Serverbound, ConnectionState::Play},
    {0x18, PacketId::PlayServerboundKeepAlive,           PacketDirection::Serverbound, ConnectionState::Play},
    {0x23, PacketId::PlayServerboundPlayerAbilities,     PacketDirection::Serverbound, ConnectionState::Play},
    {0x2F, PacketId::PlayServerboundHeldItemChange,      PacketDirection::Serverbound, ConnectionState::Play},
    {0x32, PacketId::PlayServerboundCreativeInventoryAction, PacketDirection::Serverbound, ConnectionState::Play},
};

inline constexpr size_t packetTableSize = sizeof(packetTable) / sizeof(packetTable[0]);

inline const PacketMapping* findMapping(PacketId id, PacketDirection dir, ConnectionState state) {
    for (size_t i = 0; i < packetTableSize; ++i) {
        if (packetTable[i].internalId == id &&
            packetTable[i].direction == dir &&
            packetTable[i].state == state) {
            return &packetTable[i];
        }
    }
    return nullptr;
}

inline const PacketMapping* findByWire(i32 wireId, PacketDirection dir, ConnectionState state) {
    for (size_t i = 0; i < packetTableSize; ++i) {
        if (packetTable[i].wireId == wireId &&
            packetTable[i].direction == dir &&
            packetTable[i].state == state) {
            return &packetTable[i];
        }
    }
    return nullptr;
}

} // namespace nc::protocol::v1_21_1
