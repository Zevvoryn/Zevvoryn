#include "player.hpp"
#include <span>
#include "../protocol/codec_1_21_1/packet_ids.hpp"
#include "../core/log.hpp"

namespace nc::entity {

void Player::sendPacket(protocol::PacketId id, const std::vector<u8>& data) {
    if (!connection_ || !connection_->isConnected()) return;
    connection_->sendPacket(static_cast<i32>(id), data);
}

void Player::sendSystemMessage(std::string_view message) {
    // SYSCHAT_V1: 1.21.1 System Chat = wire id 0x6C, content = NBT text component
    if (!connection_ || !connection_->isConnected()) return;
    net::Buffer buf;
    // Network NBT (unnamed root): TAG_String(0x08) + u16 length + UTF-8 bytes
    buf.writeByte(0x08);
    std::string msg(message);
    buf.writeU16(static_cast<u16>(msg.size()));
    buf.writeBytes(std::span<const u8>(reinterpret_cast<const u8*>(msg.data()), msg.size()));
    buf.writeBool(false); // overlay: false = chat, true = actionbar
    connection_->sendPacket(0x6C, std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
}

} // namespace nc::entity
