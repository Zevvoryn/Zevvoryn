#pragma once

#include "../core/types.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <functional>

namespace nc::registries {

// ============================================================
// Реестр блоков. Каждый блок имеет numeric ID, имя, и свойства.
// Используется для построения чанков и для трансляции между версиями.
// ============================================================

struct BlockState {
    i32 id = 0;          // Глобальный runtime ID (state ID)
    i32 blockId = 0;     // Базовый блок ID
    std::string name;    // minecraft:stone
    // Свойства (waterlogged, axis, facing, etc.)
    std::unordered_map<std::string, std::string> properties;

    bool isAir() const { return name == "minecraft:air" || name == "minecraft:cave_air" || name == "minecraft:void_air"; }
    bool isSolid() const;  // Определить по базовому блоку
    bool isLiquid() const { return name.find("water") != std::string::npos || name.find("lava") != std::string::npos; }
};

struct Block {
    i32 id = 0;            // Базовый ID (minecraft:stone → 1)
    std::string name;      // minecraft:stone
    i32 maxStateId = 0;    // Максимальный state ID для этого блока
    i32 minStateId = 0;    // Минимальный state ID
    bool solid = true;
    bool liquid = false;
    f32 hardness = 1.0f;
    f32 resistance = 1.0f;
    f32 luminance = 0.0f;
};

// ============================================================
// Реестр предметов
// ============================================================

struct ItemEntry {
    i32 id = 0;
    std::string name;     // minecraft:diamond_sword
    i32 maxStackSize = 64;
    f32 attackDamage = 1.0f;
    f32 attackSpeed = 4.0f;
};

// ============================================================
// Реестр энтити
// ============================================================

struct EntityEntry {
    i32 id = 0;
    std::string name;     // minecraft:zombie
    f32 width = 0.6f;
    f32 height = 1.8f;
    bool living = false;
    bool hostile = false;
};

// ============================================================
// Реестр биомов
// ============================================================

struct BiomeEntry {
    i32 id = 0;
    std::string name;     // minecraft:plains
    f32 temperature = 0.8f;
    f32 downfall = 0.4f;
    bool has_precipitation = true;
};

// ============================================================
// Реестр частиц
// ============================================================

struct ParticleEntry {
    i32 id = 0;
    std::string name;     // minecraft:flame
};

// ============================================================
// Реестр звуков
// ============================================================

struct SoundEntry {
    i32 id = 0;
    std::string name;     // minecraft:block.anvil.break
};

// ============================================================
// Универсальный реестр — типизированное хрилище
// ============================================================

template<typename T>
class Registry {
public:
    void registerEntry(T entry) {
        byId_[entry.id] = entry;
        byName_[entry.name] = entry;
        ordered_.push_back(std::move(entry));
    }

    // Сбросить и загрузить из JSON-файла
    void loadFromJson(const std::string& json);

    const T* getById(i32 id) const {
        auto it = byId_.find(id);
        return it != byId_.end() ? &it->second : nullptr;
    }

    const T* getByName(std::string_view name) const {
        auto it = byName_.find(std::string(name));
        return it != byName_.end() ? &it->second : nullptr;
    }

    const std::vector<T>& getAll() const { return ordered_; }

    size_t size() const { return ordered_.size(); }

    // Маппинг для трансляции: старый ID → новый ID
    std::unordered_map<i32, i32> buildRemapTable(
        const Registry<T>& other,
        std::function<i32(const T&)> keyFn = [](const T& e) { return e.id; }
    ) const {
        std::unordered_map<i32, i32> remap;
        for (const auto& entry : ordered_) {
            const auto* otherEntry = other.getByName(entry.name);
            if (otherEntry) {
                remap[keyFn(entry)] = keyFn(*otherEntry);
            }
        }
        return remap;
    }

private:
    std::unordered_map<i32, T> byId_;
    std::unordered_map<std::string, T> byName_;
    std::vector<T> ordered_;
};

// ============================================================
// Глобальный реестр — хранит все реестры.
// Загружается при старте сервера.
// ============================================================

class RegistryManager {
public:
    static RegistryManager& instance() {
        static RegistryManager mgr;
        return mgr;
    }

    Registry<Block>& blocks() { return blocks_; }
    Registry<BlockState>& blockStates() { return blockStates_; }
    Registry<ItemEntry>& items() { return items_; }
    Registry<EntityEntry>& entities() { return entities_; }
    Registry<BiomeEntry>& biomes() { return biomes_; }
    Registry<ParticleEntry>& particles() { return particles_; }
    Registry<SoundEntry>& sounds() { return sounds_; }

    const Registry<Block>& blocks() const { return blocks_; }
    const Registry<BlockState>& blockStates() const { return blockStates_; }
    const Registry<ItemEntry>& items() const { return items_; }
    const Registry<EntityEntry>& entities() const { return entities_; }
    const Registry<BiomeEntry>& biomes() const { return biomes_; }

    // Загрузить все реестры из встроенных данных
    void loadDefaults();

    // Получить state ID по имени блока и свойствам
    std::optional<i32> getBlockStateId(std::string_view name,
        const std::unordered_map<std::string, std::string>& props = {}) const;

    // Получить state ID для плоского мира
    i32 getFlatWorldBlockStateId() const;

private:
    RegistryManager() = default;

    void loadBlockDefaults();
    // STATE_VARIANTS_V1: регистрация property-вариантов состояний блоков.
    void registerStateVariants();
    void loadItemDefaults();
    void loadEntityDefaults();
    void loadBiomeDefaults();

    Registry<Block> blocks_;
    Registry<BlockState> blockStates_;
    Registry<ItemEntry> items_;
    Registry<EntityEntry> entities_;
    Registry<BiomeEntry> biomes_;
    Registry<ParticleEntry> particles_;
    Registry<SoundEntry> sounds_;
};

} // namespace nc::registries
