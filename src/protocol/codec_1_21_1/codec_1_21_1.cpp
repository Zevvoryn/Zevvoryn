#include "codec_1_21_1.hpp"
#include "../../network/buffer.hpp"

namespace nc::protocol::v1_21_1 {

std::expected<DecodedPacket, std::string>
Codec_1_21_1::decode(net::Buffer& buffer, ConnectionState state) {
    // Packet ID уже прочитан сетевым кодом, buffer содержит payload
    i32 wireId = buffer.readVarInt();

    PacketDirection dir = PacketDirection::Serverbound;
    PacketId internalId = mapWireToInternal(wireId, dir, state);

    if (internalId == PacketId::None) {
        return std::unexpected(std::format("Неизвестный wire packet ID: 0x{:02X} в состоянии {}", wireId, static_cast<int>(state)));
    }

    // Копируем остаток буфера как payload
    std::vector<u8> payload(buffer.readSpan().begin(), buffer.readSpan().end());

    return DecodedPacket{
        .id = internalId,
        .payload = std::move(payload),
        .protocolVersion = getProtocolVersion(),
    };
}

std::expected<EncodedPacket, std::string>
Codec_1_21_1::encode(PacketId id, const std::vector<u8>& data, ConnectionState state) {
    PacketDirection dir = PacketDirection::Clientbound;
    i32 wireId = mapInternalToWire(id, dir, state);

    if (wireId < 0) {
        return std::unexpected(std::format("Неизвестный внутренний packet ID для кодирования"));
    }

    return EncodedPacket{
        .packetId = wireId,
        .payload = data,
    };
}

const PacketInfo* Codec_1_21_1::getPacketInfo(PacketId id) const {
    struct Entry { PacketId id; PacketInfo info; };
    static const Entry infos[] = {
        {PacketId::Handshake,                         {"Handshake", "Handshake", PacketDirection::Serverbound, nc::ConnectionState::Handshaking}},
        {PacketId::StatusRequest,                     {"StatusRequest", "Status Request", PacketDirection::Serverbound, nc::ConnectionState::Status}},
        {PacketId::StatusResponse,                    {"StatusResponse", "Status Response", PacketDirection::Clientbound, nc::ConnectionState::Status}},
        {PacketId::StatusPing,                        {"StatusPing", "Status Ping", PacketDirection::Serverbound, nc::ConnectionState::Status}},
        {PacketId::LoginStart,                        {"LoginStart", "Login Start", PacketDirection::Serverbound, nc::ConnectionState::Login}},
        {PacketId::LoginSuccess,                      {"LoginSuccess", "Login Success", PacketDirection::Clientbound, nc::ConnectionState::Login}},
        {PacketId::LoginCompression,                  {"LoginCompression", "Login Compression", PacketDirection::Clientbound, nc::ConnectionState::Login}},
        {PacketId::ConfigurationFinish,               {"ConfigFinish", "Configuration Finish", PacketDirection::Clientbound, nc::ConnectionState::Configuration}},
        {PacketId::ConfigurationRegistryData,         {"ConfigRegistryData", "Configuration Registry Data", PacketDirection::Clientbound, nc::ConnectionState::Configuration}},
        {PacketId::ConfigurationPluginMessage,        {"ConfigPluginMessage", "Configuration Plugin Message", PacketDirection::Clientbound, nc::ConnectionState::Configuration}},
        {PacketId::ConfigurationFinishAcknowledged,   {"ConfigFinishAck", "Configuration Finish Acknowledged", PacketDirection::Serverbound, nc::ConnectionState::Configuration}},
        {PacketId::ConfigurationClientInformation,    {"ConfigClientInfo", "Configuration Client Information", PacketDirection::Serverbound, nc::ConnectionState::Configuration}},
        {PacketId::ConfigurationPluginMessageServerbound, {"ConfigPluginMsgSB", "Configuration Plugin Message (SB)", PacketDirection::Serverbound, nc::ConnectionState::Configuration}},
        {PacketId::PlayKeepAlive,                     {"KeepAlive", "Keep Alive", PacketDirection::Clientbound, nc::ConnectionState::Play}},
        {PacketId::PlayPositionAndLook,               {"PositionAndLook", "Player Position And Look", PacketDirection::Clientbound, nc::ConnectionState::Play}},
        {PacketId::PlayChunkDataAndUpdateLight,       {"ChunkData", "Chunk Data And Update Light", PacketDirection::Clientbound, nc::ConnectionState::Play}},
        {PacketId::PlaySystemChatMessage,             {"SystemChat", "System Chat Message", PacketDirection::Clientbound, nc::ConnectionState::Play}},
        {PacketId::PlaySpawnPlayer,                   {"SpawnPlayer", "Spawn Player", PacketDirection::Clientbound, nc::ConnectionState::Play}},
        {PacketId::PlayPlayerInfo,                    {"PlayerInfo", "Player Info", PacketDirection::Clientbound, nc::ConnectionState::Play}},
    };

    for (const auto& e : infos) {
        if (e.id == id) return &e.info;
    }
    return nullptr;
}

PacketId Codec_1_21_1::mapWireToInternal(i32 wireId, PacketDirection dir, ConnectionState state) const {
    const auto* mapping = findByWire(wireId, dir, state);
    if (mapping) return mapping->internalId;
    return PacketId::None;
}

i32 Codec_1_21_1::mapInternalToWire(PacketId id, PacketDirection dir, ConnectionState state) const {
    const auto* mapping = findMapping(id, dir, state);
    if (mapping) return mapping->wireId;
    return -1;
}

} // namespace nc::protocol::v1_21_1
