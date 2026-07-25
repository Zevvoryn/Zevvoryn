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

    // ANVIL_CONVERT_V1: named root compound — TAG_Compound + u16 name length +
    // name bytes + body. Used for file-based NBT (level.dat, chunk NBT), which
    // (unlike the network/protocol NBT written by beginRootCompound()) always
    // includes a root name field (usually an empty string).
    void beginRootCompoundNamed(std::string_view name) {
        buffer_.writeByte(static_cast<u8>(TagType::Compound));
        writeNameRaw(name);
        stack_.push_back(State::Compound);
    }

    // Starts a List tag. elemType must match the type of every element written
    // with beginListElementCompound()/writeListElementString() etc. count must
    // match the number of elements written before calling endList().
    void beginList(std::string_view name, TagType elemType, i32 count) {
        writeTagHeader(TagType::List, name);
        buffer_.writeByte(static_cast<u8>(elemType));
        buffer_.writeI32(count);
        stack_.push_back(State::List);
    }

    // Lists carry no delimiter after their fixed element count, so this only
    // pops the internal bookkeeping stack (no bytes are written).
    void endList() {
        if (!stack_.empty()) stack_.pop_back();
    }

    // Compound elements inside a List have no per-element tag header/name —
    // just the compound body directly, terminated by TAG_End.
    void beginListElementCompound() {
        stack_.push_back(State::Compound);
    }

    void endListElementCompound() {
        buffer_.writeByte(static_cast<u8>(TagType::End));
        if (!stack_.empty()) stack_.pop_back();
    }

    // String elements inside a List<String> — raw payload only, no tag header.
    void writeListElementString(std::string_view value) {
        writeStringPayload(value);
    }

    void writeByteArray(const std::vector<u8>& values, std::string_view name) {
        writeTagHeader(TagType::ByteArray, name);
        buffer_.writeI32(static_cast<i32>(values.size()));
        for (auto v : values) buffer_.writeByte(v);
    }

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

// ============================================================
// ANVIL_CONVERT_V1: NBT TagReader — parses Named NBT (Java Edition) into a
// generic tree. Used to read vanilla chunk NBT and level.dat contents when
// importing an Anvil world. Big-endian, per the Java NBT spec.
// ============================================================

struct NbtValue {
    TagType type = TagType::End;
    i64 asLong = 0;                                    // Byte/Short/Int/Long
    f64 asDouble = 0.0;                                 // Float/Double
    std::string asString;                               // String
    std::vector<u8> byteArray;                          // ByteArray
    std::vector<i32> intArray;                          // IntArray
    std::vector<i64> longArray;                         // LongArray
    TagType listElemType = TagType::End;                // List
    std::vector<NbtValue> list;                         // List
    std::vector<std::pair<std::string, NbtValue>> compound; // Compound (ordered)

    const NbtValue* get(std::string_view key) const {
        if (type != TagType::Compound) return nullptr;
        for (const auto& [k, v] : compound) {
            if (k == key) return &v;
        }
        return nullptr;
    }

    i32 getInt(std::string_view key, i32 def = 0) const {
        const auto* v = get(key);
        return v ? static_cast<i32>(v->asLong) : def;
    }
    i64 getLong(std::string_view key, i64 def = 0) const {
        const auto* v = get(key);
        return v ? v->asLong : def;
    }
    u8 getByte(std::string_view key, u8 def = 0) const {
        const auto* v = get(key);
        return v ? static_cast<u8>(v->asLong) : def;
    }
    std::string getString(std::string_view key, std::string def = "") const {
        const auto* v = get(key);
        return v ? v->asString : def;
    }
};

class TagReader {
public:
    TagReader(const u8* data, size_t len) : data_(data), len_(len) {}

    // Reads a full named-root compound (TAG_Compound + u16 name len + name +
    // body + TAG_End). Used for level.dat and chunk NBT. Returns false on any
    // malformed/truncated input.
    bool readNamedRoot(std::string& outName, NbtValue& outValue) {
        if (!ok_ || pos_ >= len_) return false;
        auto t = static_cast<TagType>(readU8());
        if (!ok_ || t != TagType::Compound) return false;
        outName = readNameString();
        if (!ok_) return false;
        outValue.type = TagType::Compound;
        readCompoundBody(outValue);
        return ok_;
    }

    bool good() const { return ok_; }

private:
    const u8* data_;
    size_t len_;
    size_t pos_ = 0;
    bool ok_ = true;

    void need(size_t n) { if (pos_ + n > len_) ok_ = false; }

    u8 readU8() { need(1); if (!ok_) return 0; return data_[pos_++]; }
    u16 readU16() {
        need(2); if (!ok_) return 0;
        u16 v = (static_cast<u16>(data_[pos_]) << 8) | data_[pos_ + 1];
        pos_ += 2; return v;
    }
    i32 readI32() {
        need(4); if (!ok_) return 0;
        u32 v = 0; for (int i = 0; i < 4; ++i) v = (v << 8) | data_[pos_++];
        return static_cast<i32>(v);
    }
    i64 readI64() {
        need(8); if (!ok_) return 0;
        u64 v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | data_[pos_++];
        return static_cast<i64>(v);
    }
    f32 readF32() { i32 v = readI32(); f32 f; std::memcpy(&f, &v, sizeof(f)); return f; }
    f64 readF64() { i64 v = readI64(); f64 f; std::memcpy(&f, &v, sizeof(f)); return f; }

    std::string readNameString() {
        u16 len = readU16();
        if (!ok_) return {};
        need(len); if (!ok_) return {};
        std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += len;
        return s;
    }

    NbtValue readPayload(TagType t) {
        NbtValue v; v.type = t;
        if (!ok_) return v;
        switch (t) {
            case TagType::Byte: v.asLong = static_cast<i8>(readU8()); break;
            case TagType::Short: { u16 x = readU16(); v.asLong = static_cast<i16>(x); break; }
            case TagType::Int: v.asLong = readI32(); break;
            case TagType::Long: v.asLong = readI64(); break;
            case TagType::Float: v.asDouble = readF32(); break;
            case TagType::Double: v.asDouble = readF64(); break;
            case TagType::ByteArray: {
                i32 n = readI32(); if (!ok_ || n < 0) { ok_ = false; break; }
                need(static_cast<size_t>(n)); if (!ok_) break;
                v.byteArray.resize(static_cast<size_t>(n));
                for (i32 i = 0; i < n; ++i) v.byteArray[static_cast<size_t>(i)] = readU8();
                break;
            }
            case TagType::String: v.asString = readNameString(); break;
            case TagType::List: {
                auto elemT = static_cast<TagType>(readU8());
                i32 n = readI32();
                if (!ok_ || n < 0) { ok_ = false; break; }
                v.listElemType = elemT;
                v.list.reserve(static_cast<size_t>(n));
                for (i32 i = 0; i < n && ok_; ++i) v.list.push_back(readPayload(elemT));
                break;
            }
            case TagType::Compound: readCompoundBody(v); break;
            case TagType::IntArray: {
                i32 n = readI32(); if (!ok_ || n < 0) { ok_ = false; break; }
                v.intArray.resize(static_cast<size_t>(n));
                for (i32 i = 0; i < n && ok_; ++i) v.intArray[static_cast<size_t>(i)] = readI32();
                break;
            }
            case TagType::LongArray: {
                i32 n = readI32(); if (!ok_ || n < 0) { ok_ = false; break; }
                v.longArray.resize(static_cast<size_t>(n));
                for (i32 i = 0; i < n && ok_; ++i) v.longArray[static_cast<size_t>(i)] = readI64();
                break;
            }
            default: break;
        }
        return v;
    }

    void readCompoundBody(NbtValue& out) {
        while (ok_ && pos_ < len_) {
            auto t = static_cast<TagType>(readU8());
            if (!ok_) return;
            if (t == TagType::End) return;
            std::string name = readNameString();
            if (!ok_) return;
            NbtValue val = readPayload(t);
            if (!ok_) return;
            out.compound.emplace_back(std::move(name), std::move(val));
        }
    }
};

} // namespace nc::nbt
