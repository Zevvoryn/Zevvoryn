#pragma once

#include "../core/types.hpp"
#include <string>

namespace nc::world::biome {

// ============================================================
// BIOMEGEN_V1: exact vanilla (Java Minecraft 1.21.1) biome lookup, powered by
// the cubiomes library (Cubitect, MIT license -- see
// thirdparty/cubiomes/LICENSE). This reproduces Mojang's real biome-placement
// algorithm bit-for-bit for a given world seed, independent of and in
// addition to this server's own homegrown DEFAULT terrain generator
// (world/wg_biome.hpp). Currently used by world/anvil.cpp to write real
// per-4x4x4-cell biomes into vanilla-exported saves.
//
// Not thread-safe across different seeds on the same thread in a tight loop
// with other threads using a *different* seed at the same time -- each
// thread keeps its own cached generator (see biomegen.cpp), so calls for a
// single seed from a single thread are cheap and fine to make per-cell.
// ============================================================

// Returns the vanilla biome id (cubiomes' BiomeID enum value) at the given
// block position. `y` is a real block Y coordinate (e.g. ~63 at sea level).
// Returns -1 ("none") on failure.
i32 getBiomeIdAt(i64 seed, i32 x, i32 y, i32 z);

// Returns the biome's registry name including the "minecraft:" namespace,
// e.g. "minecraft:plains". Falls back to "minecraft:plains" if the id can't
// be resolved to a name.
std::string getBiomeName(i64 seed, i32 x, i32 y, i32 z);

} // namespace nc::world::biome
