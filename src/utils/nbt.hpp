#pragma once

#include "../core/types.hpp"
#include "../network/buffer.hpp"
#include <string>
#include <vector>

namespace nc::nbt {

enum class TagType : u8 {
    End        = 0,
    Byte       = 1,
    Short      = 2,
    Int        = 3,
    Long       = 4,
    Float      = 5,
    Double     = 6,
    ByteArray  = 7,
    String     = 8,
    List       = 9,
    Compound   = 10,
    IntArray   = 11,
    LongArray  = 12,
};

// ============================================================
// NBT TagWriter — пишет Named NBT (Java Edition).
// TAG_Compound + u16 root name + tags + TAG_End
// ============================================================

class TagWriter {
public:
    // anonymousNbt: TAG_Compound + tags + TAG_End (без имени корня!)
    void beginRootCompound() {
        buffer_.writeByte(static_cast<u8>(TagType::Compound));
        stack_.push_back(State::Compound);
    }

    void beginCompound(std::string_view name) {
        writeTagHeader(TagType::Compound, name);
        stack_.push_back(State::Compound);
    }

    void endCompound() {
        buffer_.writeByte(static_cast<u8>(TagType::End));
        if (!stack_.empty()) stack_.pop_back();
    }

    void writeByte(i32 value, std::string_view name) {
        writeTagHeader(TagType::Byte, name);
        buffer_.writeByte(static_cast<u8>(value & 0xFF));
    }

    void writeShort(i32 value, std::string_view name) {
        writeTagHeader(TagType::Short, name);
        buffer_.writeI16(static_cast<i16>(value));
    }

    void writeInt(i32 value, std::string_view name) {
        writeTagHeader(TagType::Int, name);
        buffer_.writeI32(value);
    }

    void writeLong(i64 value, std::string_view name) {
        writeTagHeader(TagType::Long, name);
        buffer_.writeI64(value);
    }

    void writeFloat(f32 value, std::string_view name) {
        writeTagHeader(TagType::Float, name);
        buffer_.writeF32(value);
    }

    void writeDouble(f64 value, std::string_view name) {
        writeTagHeader(TagType::Double, name);
        buffer_.writeF64(value);
    }

    void writeString(std::string_view value, std::string_view name) {
        writeTagHeader(TagType::String, name);
        writeStringPayload(value);
    }

    void writeLongArray(const std::vector<i64>& values, std::string_view name) {
        writeTagHeader(TagType::LongArray, name);
        buffer_.writeI32(static_cast<i32>(values.size()));
        for (auto v : values) {
            buffer_.writeI64(v);
        }
    }

    void writeIntArray(const std::vector<i32>& values, std::string_view name) {
        writeTagHeader(TagType::IntArray, name);
        buffer_.writeI32(static_cast<i32>(values.size()));
        for (auto v : values) {
            buffer_.writeI32(v);
        }
    }

    std::vector<u8> toVector() const {
        return std::vector<u8>(buffer_.writtenSpan().begin(), buffer_.writtenSpan().end());
    }

    net::Buffer& getBuffer() { return buffer_; }

private:
    enum class State : u8 { Root, Compound, List };
    std::vector<State> stack_{State::Root};
    net::Buffer buffer_{16384};

    void writeTagHeader(TagType type, std::string_view name) {
        buffer_.writeByte(static_cast<u8>(type));
        writeNameRaw(name);
    }

    // u16 big-endian length + UTF-8 bytes (Java NBT standard)
    void writeNameRaw(std::string_view name) {
        auto len = static_cast<u16>(name.size());
        buffer_.writeByte(static_cast<u8>((len >> 8) & 0xFF));
        buffer_.writeByte(static_cast<u8>(len & 0xFF));
        if (len > 0) {
            buffer_.writeBytes(std::span<const u8>(
                reinterpret_cast<const u8*>(name.data()), len));
        }
    }

    // NBT String: u16 length + UTF-8 bytes
    void writeStringPayload(std::string_view value) {
        auto len = static_cast<u16>(value.size());
        buffer_.writeByte(static_cast<u8>((len >> 8) & 0xFF));
        buffer_.writeByte(static_cast<u8>(len & 0xFF));
        if (len > 0) {
            buffer_.writeBytes(std::span<const u8>(
                reinterpret_cast<const u8*>(value.data()), len));
        }
    }
};

} // namespace nc::nbt
