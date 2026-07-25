#pragma once

#include "../core/types.hpp"
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <span>

namespace nc::net {

// ============================================================
// Буфер для чтения/записи пакетов Minecraft протокола.
// Поддерживает VarInt/VarLong кодирование, UTF-8 строки, UUID и т.д.
// Напоминает Netty ByteBuf, но RAII и std::span.
// ============================================================

class Buffer {
public:
    // Создать буфер с начальной ёмкостью
    explicit Buffer(size_t capacity = 4096)
        : data_(capacity), readPos_(0), writePos_(0) {}

    // Создать из готового массива байт (для чтения входящих пакетов)
    Buffer(const u8* data, size_t len)
        : data_(data, data + len), readPos_(0), writePos_(len) {}

    Buffer(std::vector<u8> data)
        : data_(std::move(data)), readPos_(0), writePos_(data_.size()) {}

    // --- Запись ---

    void writeByte(u8 value) {
        ensureWritable(1);
        data_[writePos_++] = value;
    }

    void writeBool(bool value) {
        writeByte(value ? 1 : 0);
    }

    void writeU16(u16 value) {
        ensureWritable(2);
        data_[writePos_++] = static_cast<u8>((value >> 8) & 0xFF);
        data_[writePos_++] = static_cast<u8>(value & 0xFF);
    }

    void writeI16(i16 value) {
        writeU16(static_cast<u16>(value));
    }

    void writeU32(u32 value) {
        ensureWritable(4);
        data_[writePos_++] = static_cast<u8>((value >> 24) & 0xFF);
        data_[writePos_++] = static_cast<u8>((value >> 16) & 0xFF);
        data_[writePos_++] = static_cast<u8>((value >> 8)  & 0xFF);
        data_[writePos_++] = static_cast<u8>(value & 0xFF);
    }

    void writeI32(i32 value) {
        writeU32(static_cast<u32>(value));
    }

    void writeU64(u64 value) {
        ensureWritable(8);
        for (int i = 7; i >= 0; --i) {
            data_[writePos_++] = static_cast<u8>((value >> (i * 8)) & 0xFF);
        }
    }

    void writeI64(i64 value) {
        writeU64(static_cast<u64>(value));
    }

    void writeF32(f32 value) {
        u32 tmp;
        std::memcpy(&tmp, &value, sizeof(f32));
        writeU32(tmp);
    }

    void writeF64(f64 value) {
        u64 tmp;
        std::memcpy(&tmp, &value, sizeof(f64));
        writeU64(tmp);
    }

    // VarInt (Minecraft encoding)
    void writeVarInt(i32 value) {
        u32 uval = static_cast<u32>(value);
        while (uval > 0x7F) {
            writeByte(static_cast<u8>((uval & 0x7F) | 0x80));
            uval >>= 7;
        }
        writeByte(static_cast<u8>(uval));
    }

    // VarLong
    void writeVarLong(i64 value) {
        u64 uval = static_cast<u64>(value);
        while (uval > 0x7F) {
            writeByte(static_cast<u8>((uval & 0x7F) | 0x80));
            uval >>= 7;
        }
        writeByte(static_cast<u8>(uval));
    }

    void writeString(std::string_view str) {
        writeVarInt(static_cast<i32>(str.size()));
        ensureWritable(str.size());
        std::memcpy(data_.data() + writePos_, str.data(), str.size());
        writePos_ += str.size();
    }

    void writeAngle(Angle angle) {
        writeByte(angle.value);
    }

    void writeBytes(std::span<const u8> bytes) {
        ensureWritable(bytes.size());
        std::memcpy(data_.data() + writePos_, bytes.data(), bytes.size());
        writePos_ += bytes.size();
    }

    void writeUUID(const UUID& uuid) {
        writeU64(uuid.mostSignificant);
        writeU64(uuid.leastSignificant);
    }

    void writePosition(const BlockPos& pos) {
        i64 val = (static_cast<i64>(pos.x & 0x3FFFFFF) << 38) |
                  (static_cast<i64>(pos.z & 0x3FFFFFF) << 12) |
                  (static_cast<i64>(pos.y & 0xFFF));
        writeI64(val);
    }

    // Заполнить нулями до нужной длины
    void writeZeroes(size_t count) {
        ensureWritable(count);
        std::memset(data_.data() + writePos_, 0, count);
        writePos_ += count;
    }

    // --- Чтение ---

    u8 readByte() {
        checkReadable(1);
        return data_[readPos_++];
    }

    bool readBool() {
        return readByte() != 0;
    }

    u16 readU16() {
        checkReadable(2);
        u16 val = (static_cast<u16>(data_[readPos_]) << 8) |
                   static_cast<u16>(data_[readPos_ + 1]);
        readPos_ += 2;
        return val;
    }

    i16 readI16() {
        return static_cast<i16>(readU16());
    }

    u32 readU32() {
        checkReadable(4);
        u32 val = (static_cast<u32>(data_[readPos_])     << 24) |
                  (static_cast<u32>(data_[readPos_ + 1]) << 16) |
                  (static_cast<u32>(data_[readPos_ + 2]) << 8)  |
                   static_cast<u32>(data_[readPos_ + 3]);
        readPos_ += 4;
        return val;
    }

    i32 readI32() {
        return static_cast<i32>(readU32());
    }

    u64 readU64() {
        checkReadable(8);
        u64 val = 0;
        for (int i = 0; i < 8; ++i) {
            val = (val << 8) | data_[readPos_ + i];
        }
        readPos_ += 8;
        return val;
    }

    i64 readI64() {
        return static_cast<i64>(readU64());
    }

    f32 readF32() {
        u32 tmp = readU32();
        f32 val;
        std::memcpy(&val, &tmp, sizeof(f32));
        return val;
    }

    f64 readF64() {
        u64 tmp = readU64();
        f64 val;
        std::memcpy(&val, &tmp, sizeof(f64));
        return val;
    }

    i32 readVarInt() {
        i32 result = 0;
        int shift = 0;
        u8 byte;
        do {
            byte = readByte();
            result |= static_cast<i32>(byte & 0x7F) << shift;
            shift += 7;
            if (shift >= 35) {
                throw std::runtime_error("VarInt слишком длинный");
            }
        } while (byte & 0x80);
        return result;
    }

    i64 readVarLong() {
        i64 result = 0;
        int shift = 0;
        u8 byte;
        do {
            byte = readByte();
            result |= static_cast<i64>(byte & 0x7F) << shift;
            shift += 7;
            if (shift >= 70) {
                throw std::runtime_error("VarLong слишком длинный");
            }
        } while (byte & 0x80);
        return result;
    }

    std::string readString() {
        i32 len = readVarInt();
        if (len < 0 || len > 32767) {
            throw std::runtime_error("Некорректная длина строки");
        }
        checkReadable(static_cast<size_t>(len));
        std::string result(reinterpret_cast<const char*>(data_.data() + readPos_), static_cast<size_t>(len));
        readPos_ += len;
        return result;
    }

    std::vector<u8> readBytes(size_t count) {
        checkReadable(count);
        std::vector<u8> result(data_.begin() + readPos_, data_.begin() + readPos_ + static_cast<ptrdiff_t>(count));
        readPos_ += count;
        return result;
    }

    UUID readUUID() {
        return UUID{readU64(), readU64()};
    }

    BlockPos readPosition() {
        i64 val = readI64();
        i32 x = static_cast<i32>((val >> 38) & 0x3FFFFFF);
        i32 y = static_cast<i32>((val >> 26) & 0xFFF);
        i32 z = static_cast<i32>(val & 0x3FFFFFF);
        // Sign-extend
        if (x >= 0x2000000) x -= 0x4000000;
        if (z >= 0x2000000) z -= 0x4000000;
        return BlockPos{x, y, z};
    }

    // --- Утилиты ---

    // Скопировать данные из другого буфера
    void writeFrom(Buffer& other, size_t count) {
        auto bytes = other.readBytes(count);
        writeBytes(bytes);
    }

    void skipBytes(size_t count) {
        checkReadable(count);
        readPos_ += count;
    }

    // Поменять местами read/write позиции (для перекодирования)
    void swapWith(Buffer& other) {
        std::swap(data_, other.data_);
        std::swap(readPos_, other.readPos_);
        std::swap(writePos_, other.writePos_);
    }

    // Аксессоры
    size_t readableBytes() const { return writePos_ - readPos_; }
    size_t writtenBytes() const { return writePos_; }
    size_t capacity() const { return data_.size(); }

    const u8* readPtr() const { return data_.data() + readPos_; }
    u8* writePtr() { return data_.data() + writePos_; }

    std::span<const u8> readSpan() const {
        return std::span<const u8>(data_.data() + readPos_, readableBytes());
    }

    std::span<const u8> writtenSpan() const {
        return std::span<const u8>(data_.data(), writePos_);
    }

    void reset() {
        readPos_ = 0;
        writePos_ = 0;
    }

    // Сбросить позицию чтения, чтобы перечитать
    void resetRead() { readPos_ = 0; }

private:
    std::vector<u8> data_;
    size_t readPos_;
    size_t writePos_;

    void ensureWritable(size_t needed) {
        if (writePos_ + needed > data_.size()) {
            data_.resize(writePos_ + needed + 1024);
        }
    }

    void checkReadable(size_t needed) const {
        if (readPos_ + needed > writePos_) {
            throw std::runtime_error("Buffer: недостаточно данных для чтения");
        }
    }
};

} // namespace nc::net
