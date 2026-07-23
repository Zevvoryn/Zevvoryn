#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <span>
#include <memory>
#include <functional>
#include <expected>
#include <optional>
#include <variant>
#include <concepts>
#include <chrono>
#include <format>
#include <ranges>
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <queue>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include <latch>
#include <stop_token>
#include <source_location>
#include <filesystem>
#include <cassert>
#include <bit>
#include <cstring>

// ============================================================
// Базовые типы протокола Minecraft
// ============================================================

namespace nc {

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using f32 = float;
using f64 = double;

// VarInt / VarLong — стандартные кодировки Minecraft
using VarInt  = i32;
using VarLong = i64;

// UUID в двух форматах: как два i64 (Minecraft) и как массив байт
struct UUID {
    u64 mostSignificant;
    u64 leastSignificant;

    static UUID fromString(std::string_view str);
    std::string toString() const;

    bool operator==(const UUID& other) const = default;
};

// Позиция в мире (с блочной точностью)
struct BlockPos {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;

    bool operator==(const BlockPos&) const = default;
};

// Позиция суб区块 (chunk section coordinates)
struct ChunkPos {
    i32 x = 0;
    i32 z = 0;

    bool operator==(const ChunkPos&) const = default;
};

// Позиция с плавающей точкой
struct BlockPosDouble {
    f64 x = 0.0;
    f64 y = 0.0;
    f64 z = 0.0;
};

// Angle для поворотов в протоколе (0-255)
struct Angle {
    u8 value = 0;

    f64 toDegrees() const { return static_cast<f64>(value) * 360.0 / 256.0; }
    static Angle fromDegrees(f64 deg) {
        return Angle{ static_cast<u8>(static_cast<i32>(deg * 256.0 / 360.0) & 0xFF) };
    }
};

// Слот инвентаря
struct SlotData {
    bool present = false;
    i32 itemId = -1;
    i8 count = 0;
    // NBT будет добавлено позже
};

// Direction (6 сторон)
enum class Direction : u8 {
    Down  = 0,
    Up    = 1,
    North = 2,
    South = 3,
    West  = 4,
    East  = 5,
};

// Состояние соединения (вынесено из протокола, т.к. сеть зависит от него)
enum class ConnectionState : u8 {
    Handshaking,
    Status,
    Login,
    Configuration,
    Play,
};

} // namespace nc

// Хешеры для использования в unordered_* контейнерах
namespace std {
template<> struct hash<nc::BlockPos> {
    size_t operator()(const nc::BlockPos& p) const {
        size_t h = std::hash<nc::i32>{}(p.x);
        h ^= std::hash<nc::i32>{}(p.y) << 2;
        h ^= std::hash<nc::i32>{}(p.z) << 4;
        return h;
    }
};
template<> struct hash<nc::ChunkPos> {
    size_t operator()(const nc::ChunkPos& p) const {
        size_t h = std::hash<nc::i32>{}(p.x);
        h ^= std::hash<nc::i32>{}(p.z) << 4;
        return h;
    }
};
template<> struct hash<nc::UUID> {
    size_t operator()(const nc::UUID& u) const {
        size_t h = std::hash<nc::u64>{}(u.mostSignificant);
        h ^= std::hash<nc::u64>{}(u.leastSignificant) << 1;
        return h;
    }
};
} // namespace std
