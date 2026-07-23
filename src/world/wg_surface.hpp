// wg_surface.hpp — Overworld Surface Rules, derived from vanilla 1.21.1 SurfaceRuleData.
// The density terrain is generated first; this layer replaces the outer stone
// according to biome, height, water level and minecraft:surface noise.
#pragma once
#include "wg_biome.hpp"
#include <cmath>

namespace wg {

enum SurfaceMaterial {
    SM_STONE, SM_DEEPSLATE, SM_DIRT, SM_GRASS, SM_COARSE_DIRT, SM_PODZOL,
    SM_SAND, SM_SANDSTONE, SM_RED_SAND, SM_RED_SANDSTONE, SM_GRAVEL,
    SM_TERRACOTTA, SM_WHITE_TERRACOTTA, SM_ORANGE_TERRACOTTA,
    SM_YELLOW_TERRACOTTA, SM_BROWN_TERRACOTTA, SM_RED_TERRACOTTA,
    SM_LIGHT_GRAY_TERRACOTTA, SM_MYCELIUM, SM_SNOW_BLOCK, SM_PACKED_ICE,
    SM_ICE, SM_MUD, SM_CALCITE, SM_BEDROCK
};

inline bool isBadlandsBiome(int b) {
    return b == B_BADLANDS || b == B_ERODED_BADLANDS || b == B_WOODED_BADLANDS;
}
inline bool isOceanBiome(int b) {
    return b == B_DEEP_FROZEN_OCEAN || b == B_DEEP_COLD_OCEAN || b == B_DEEP_OCEAN ||
           b == B_DEEP_LUKEWARM_OCEAN || b == B_WARM_OCEAN || b == B_FROZEN_OCEAN ||
           b == B_COLD_OCEAN || b == B_OCEAN || b == B_LUKEWARM_OCEAN;
}

// Vanilla uses SurfaceSystem clay bands with a seed-derived offset. This compact
// equivalent preserves recognizable horizontal badlands strata.
inline SurfaceMaterial badlandsBand(int y, double noise) {
    int band = (int)std::floor((double)y + noise * 6.0);
    band %= 64;
    if (band < 0) band += 64;
    if (band == 0 || band == 1 || band == 20 || band == 21) return SM_WHITE_TERRACOTTA;
    if (band == 2 || band == 3 || band == 14 || band == 15 || band == 40) return SM_ORANGE_TERRACOTTA;
    if (band == 8 || band == 9 || band == 10 || band == 31 || band == 32) return SM_YELLOW_TERRACOTTA;
    if (band == 26 || band == 27 || band == 48) return SM_BROWN_TERRACOTTA;
    if (band == 36 || band == 37 || band == 55) return SM_RED_TERRACOTTA;
    if (band == 44 || band == 45) return SM_LIGHT_GRAY_TERRACOTTA;
    return SM_TERRACOTTA;
}

inline SurfaceMaterial overworldSurface(const OverworldRouter& router, int biome,
                                        int x, int y, int z, int surfaceY, int depth) {
    if (y <= 0) return SM_DEEPSLATE;
    const double n = router.surfaceNoise(x, z);
    const bool top = depth == 0;
    const bool under = depth > 0 && depth <= 3;
    const bool deepUnder = depth > 3 && depth <= 8;

    if (isBadlandsBiome(biome)) {
        if (surfaceY <= SEA_LEVEL + 1) {
            if (top || under) return SM_RED_SAND;
            if (deepUnder) return SM_RED_SANDSTONE;
        }
        if (top && surfaceY < 74) return SM_ORANGE_TERRACOTTA;
        return badlandsBand(y, n);
    }
    if (biome == B_DESERT || biome == B_BEACH || biome == B_SNOWY_BEACH ||
        biome == B_WARM_OCEAN || biome == B_LUKEWARM_OCEAN || biome == B_DEEP_LUKEWARM_OCEAN) {
        if (top || under) return SM_SAND;
        if (deepUnder) return SM_SANDSTONE;
        return SM_STONE;
    }
    if (isOceanBiome(biome) || biome == B_RIVER || biome == B_FROZEN_RIVER) {
        return (top || under) ? SM_GRAVEL : SM_STONE;
    }
    if (biome == B_MANGROVE_SWAMP) return (top || under) ? SM_MUD : SM_DIRT;
    if (biome == B_SWAMP) return (top || under) ? SM_GRASS : SM_DIRT;
    if (biome == B_MUSHROOM_FIELDS) return (top || under) ? SM_MYCELIUM : SM_DIRT;

    if (biome == B_OLD_GROWTH_PINE_TAIGA || biome == B_OLD_GROWTH_SPRUCE_TAIGA) {
        if (top && n > 0.20) return SM_COARSE_DIRT;
        if (top && n > -0.15) return SM_PODZOL;
        return (top || under) ? SM_DIRT : SM_STONE;
    }
    if (biome == B_FROZEN_PEAKS) {
        if (top && (surfaceY > 110 || n > 0.20)) return SM_PACKED_ICE;
        if (top) return SM_SNOW_BLOCK;
        return SM_STONE;
    }
    if (biome == B_SNOWY_SLOPES || biome == B_ICE_SPIKES || biome == B_GROVE ||
        biome == B_SNOWY_PLAINS || biome == B_SNOWY_TAIGA) return (top || under) ? SM_SNOW_BLOCK : SM_DIRT;
    if (biome == B_STONY_PEAKS) return (top && std::fabs(n) < 0.06) ? SM_CALCITE : SM_STONE;
    if (biome == B_JAGGED_PEAKS || biome == B_STONY_SHORE) return SM_STONE;
    if (biome == B_WINDSWEPT_GRAVELLY_HILLS) {
        if (top && n > 0.24) return SM_GRAVEL;
        if (top && n > 0.12) return SM_STONE;
        return (top || under) ? SM_DIRT : SM_STONE;
    }
    if (biome == B_WINDSWEPT_HILLS || biome == B_WINDSWEPT_FOREST || biome == B_WINDSWEPT_SAVANNA) {
        if (top && n > 0.12) return SM_STONE;
        return (top || under) ? SM_DIRT : SM_STONE;
    }
    return top ? SM_GRASS : (under ? SM_DIRT : SM_STONE);
}

} // namespace wg
