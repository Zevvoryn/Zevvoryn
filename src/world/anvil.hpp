#pragma once
// BLOCKENT_V1 / ENTSAVE_V1: the converter also carries chests, signs, mobs,
// dropped items and vehicles, handed over as a world::extra::Snapshot.

#include "../core/types.hpp"
#include "worldextra.hpp" // BLOCKENT_V1 / ENTSAVE_V1
#include <array>
#include <string>

namespace nc::world {
class World;
}

namespace nc::world::anvil {

// ============================================================
// ANVIL_CONVERT_V1: bidirectional converter between the server's own
// world.dat format and vanilla Minecraft 1.21.1's Anvil format
// (level.dat + region/*.mca), so worlds can round-trip through singleplayer.
//
// Known limitations (see world/anvil.cpp header comment for detail):
//  - BLOCKMAP_V1: all 1331 vanilla 1.21.1 blocks now convert by name (the
//    generated table in core/item_blocks.gen.hpp is used when a state id is
//    not registered with variants). Only modded/newer blocks fall back to air.
//  - BLOCKMAP_V1: block state properties (facing, half, axis, waterlogged,
//    ...) are written into the palette on export and read back on import for
//    every state the registry knows as a variant. For blocks whose variants
//    are not registered yet, the default state is used, and a state id below
//    its block default may resolve to the previous block in id order.
//  - BIOMEGEN_V1: real per-4x4x4-cell vanilla biomes (matching the world
//    seed bit-for-bit via the cubiomes library) are now written on export;
//    biome data is still not read back on import (see importFromVanilla).
//  - BLOCKENT_V1: chest contents and sign text round-trip through the chunk
//    block_entities list. Other block entities (furnaces, hoppers, barrels,
//    banners, spawners) are not modelled by the core yet, so they are lost.
//  - ENTSAVE_V1: mobs, dropped items, minecarts and boats round-trip through
//    entities/*.mca. A mob type this core does not implement is skipped on
//    import rather than replaced. Boats without a known wood type land as oak.
//  - Exported chunks are marked isLightOn=0 so vanilla recomputes lighting
//    on first load instead of us calculating it.
//  - The level.dat generator/world-gen settings are hand-authored for a
//    flat superflat world (matching this server's FLAT generator) and were
//    not verified against a real Mojang-generated save in this environment
//    -- test on a copy of your world first.
// ============================================================

// Exports `world` into `worldDir` (created if missing) as a vanilla-loadable
// singleplayer/server save: worldDir/level.dat + worldDir/region/*.mca.
// BIOMEGEN_V1: `seed` is used both for the exported level.dat's WorldGenSettings
// seed and to compute real vanilla biomes per 4x4x4 cell via cubiomes.
// `extras` (optional) carries chests, signs, mobs, dropped items and vehicles;
// pass nullptr to export blocks only.
// V57_PLAYERTAG_V1: состояние игрока для тега Data.Player в level.dat. Без него
// ванильный клиент ставит игрока в точку спавна с пустым инвентарём.
// Слоты — в раскладке окна инвентаря сервера: 5..8 броня (шлем..ботинки),
// 9..35 рюкзак, 36..44 хотбар, 45 вторая рука. Перевод в нумерацию
// ванили (0..8 хотбар, 9..35 рюкзак, 100..103 броня, -106 оффхенд)
// делает сам экспортер.
struct PlayerExport {
    f64 x = 0, y = 0, z = 0;
    f32 yaw = 0, pitch = 0;
    i32 gameMode = 0;
    i32 selectedSlot = 0; // 0..8
    f32 health = 20.0f;
    i32 foodLevel = 20;
    f32 foodSaturation = 5.0f;
    f32 foodExhaustion = 0.0f;
    i32 foodTickTimer = 0;
    i32 xpLevel = 0;
    i32 xpTotal = 0;
    std::array<i32, 46> itemId{};
    std::array<i32, 46> count{};
    std::array<i32, 46> damage{};
    std::array<i32, 27> enderItemId{};
    std::array<i32, 27> enderCount{};
};

bool exportToVanilla(const World& world, const std::string& worldDir,
                      const std::string& levelName, i64 seed,
                      i32 spawnX, i32 spawnY, i32 spawnZ,
                      std::string* errorOut = nullptr,
                      const extra::Snapshot* extras = nullptr,
                      i32 defaultGameMode = 0,           // V57_GAMETYPE_V1
                      const PlayerExport* player = nullptr); // V57_PLAYERTAG_V1

// Imports a vanilla save from `worldDir` (must contain level.dat and a
// region/ folder) into `world`, adding/overwriting chunks found on disk.
// `extrasOut` (optional) collects the chests, signs and entities found in the
// save; records are appended, so pass a cleared snapshot.
bool importFromVanilla(World& world, const std::string& worldDir,
                        std::string* errorOut = nullptr,
                        extra::Snapshot* extrasOut = nullptr);

// DIMCONV_V1: the Nether and the End. Vanilla keeps them inside the save
// (<save>/DIM-1, <save>/DIM1); Bukkit and its forks (Spigot / Paper / Purpur /
// Folia) split them into sibling folders (<save>_nether/DIM-1,
// <save>_the_end/DIM1). Export always writes the vanilla layout; import
// accepts both, trying the vanilla path first.
enum class Dimension { Overworld, Nether, End };

// Writes `world` as the given dimension's region files under `worldDir`.
// level.dat is NOT touched here -- call exportToVanilla() for the overworld
// first, then this for each extra dimension.
bool exportDimension(const World& world, const std::string& worldDir, Dimension dim,
                     i64 seed, std::string* errorOut = nullptr,
                     const extra::Snapshot* extras = nullptr);

// Reads the given dimension's region files into `world`. Returns false when no
// region data for that dimension exists in any of the known layouts.
bool importDimension(World& world, const std::string& worldDir, Dimension dim,
                     std::string* errorOut = nullptr,
                     extra::Snapshot* extrasOut = nullptr);

// V56_DATKILL_V1: reads the seed and spawn point back out of an already-exported
// level.dat (worldDir + "/level.dat"), so seed.dat/spawn.dat are no longer needed
// as separate files -- level.dat is rewritten with the current seed/spawn on
// every save (see saveVanillaMirror -> exportToVanilla -> buildLevelDatNbt).
// Returns false if level.dat is missing or unreadable; seedOut/spawnX/Y/Z are
// left untouched in that case.
bool readLevelDatSeedSpawn(const std::string& worldDir, i64& seedOut,
                            i32& spawnX, i32& spawnY, i32& spawnZ);

} // namespace nc::world::anvil
