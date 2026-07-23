#pragma once

#include "../common/protocol_codec.hpp"
#include "packet_ids.hpp"

namespace nc::protocol::v1_21_1 {

// ============================================================
// Реализация кодека протокола 1.21.1 (protocol version 767)
// ============================================================

class Codec_1_21_1 : public ProtocolCodec {
public:
    i32 getProtocolVersion() const override { return 767; }
    std::string_view getVersionString() const override { return "1.21.1"; }

    void registerPackets() override {}

    // Декодирование: читаем wire ID, маппим на внутренний PacketId
    std::expected<DecodedPacket, std::string>
    decode(net::Buffer& buffer, ConnectionState state) override;

    // Кодирование: маппим внутренний PacketId на wire ID
    std::expected<EncodedPacket, std::string>
    encode(PacketId id, const std::vector<u8>& data, ConnectionState state) override;

    const PacketInfo* getPacketInfo(PacketId id) const override;

    PacketId mapWireToInternal(i32 wireId, PacketDirection dir, ConnectionState state) const override;
    i32 mapInternalToWire(PacketId id, PacketDirection dir, ConnectionState state) const override;
};

} // namespace nc::protocol::v1_21_1
