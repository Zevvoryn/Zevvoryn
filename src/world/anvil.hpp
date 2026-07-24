#pragma once

#include "../core/types.hpp"
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
//  - Only block names registered by name in RegistryManager convert with
//    their real name (currently: air, stone, grass_block, dirt, bedrock,
//    oak_planks, water) -- exactly what a FLAT-generator world uses.
//    Anything else falls back to air on export/import.
//  - Per-block orientation/shape properties (stairs, doors, chests beyond
//    the built-in facing hack, etc.) are not preserved; only the base block
//    name round-trips.
//  - BIOMEGEN_V1: real per-4x4x4-cell vanilla biomes (matching the world
//    seed bit-for-bit via the cubiomes library) are now written on export;
//    biome data is still not read back on import (see importFromVanilla).
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
bool exportToVanilla(const World& world, const std::string& worldDir,
                      const std::string& levelName, i64 seed,
                      i32 spawnX, i32 spawnY, i32 spawnZ,
                      std::string* errorOut = nullptr);

// Imports a vanilla save from `worldDir` (must contain level.dat and a
// region/ folder) into `world`, adding/overwriting chunks found on disk.
bool importFromVanilla(World& world, const std::string& worldDir,
                        std::string* errorOut = nullptr);

} // namespace nc::world::anvil
