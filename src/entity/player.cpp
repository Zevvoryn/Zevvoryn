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

void Player::kick(std::string_view reason) {
    // KICKFIX_V1: send a real Play Disconnect (wire id 0x1D) before closing the
    // socket, so the client shows a proper disconnect screen with the reason
    // text instead of just losing the connection (which Minecraft otherwise
    // renders as a generic, unhelpful "Connection Lost" error).
    if (!connection_ || !connection_->isConnected()) return;
    net::Buffer buf;
    buf.writeByte(0x08);
    std::string msg(reason);
    buf.writeU16(static_cast<u16>(msg.size()));
    buf.writeBytes(std::span<const u8>(reinterpret_cast<const u8*>(msg.data()), msg.size()));
    connection_->sendPacket(0x1D, std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
    // KICKFIX_V2: close() рвал сокет раньше, чем writer успевал отправить Disconnect,
    // и клиент видел «Connection reset» вместо причины кика.
    connection_->closeAfterFlush();
}

} // namespace nc::entity
