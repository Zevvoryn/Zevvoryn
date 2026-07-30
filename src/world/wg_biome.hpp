// wg_biome.hpp — multi-noise биомы overworld (дословный порт Climate + OverworldBiomeBuilder 1.21.1)
// RTree заменён brute-force argmin fitness (доказано эквивалентным). Зависит от wg_density.hpp.
#pragma once
#include "wg_density.hpp"
#include <vector>
#include <climits>

namespace wg {

enum Biome {
    B_NONE = 0,
    B_MUSHROOM_FIELDS,
    B_DEEP_FROZEN_OCEAN, B_DEEP_COLD_OCEAN, B_DEEP_OCEAN, B_DEEP_LUKEWARM_OCEAN, B_WARM_OCEAN,
    B_FROZEN_OCEAN, B_COLD_OCEAN, B_OCEAN, B_LUKEWARM_OCEAN,
    B_SNOWY_PLAINS, B_SNOWY_TAIGA, B_TAIGA, B_PLAINS, B_FOREST, B_OLD_GROWTH_SPRUCE_TAIGA,
    B_FLOWER_FOREST, B_BIRCH_FOREST, B_DARK_FOREST, B_SAVANNA, B_JUNGLE, B_DESERT,
    B_ICE_SPIKES, B_OLD_GROWTH_PINE_TAIGA, B_SUNFLOWER_PLAINS, B_OLD_GROWTH_BIRCH_FOREST,
    B_SPARSE_JUNGLE, B_BAMBOO_JUNGLE,
    B_MEADOW, B_SAVANNA_PLATEAU, B_BADLANDS, B_WOODED_BADLANDS,
    B_CHERRY_GROVE, B_ERODED_BADLANDS,
    B_WINDSWEPT_GRAVELLY_HILLS, B_WINDSWEPT_HILLS, B_WINDSWEPT_FOREST,
    B_STONY_SHORE, B_SWAMP, B_MANGROVE_SWAMP, B_FROZEN_RIVER, B_RIVER, B_SNOWY_BEACH, B_BEACH,
    B_SNOWY_SLOPES, B_GROVE, B_JAGGED_PEAKS, B_FROZEN_PEAKS, B_STONY_PEAKS, B_WINDSWEPT_SAVANNA,
    B_DRIPSTONE_CAVES, B_LUSH_CAVES, B_DEEP_DARK,
    B_COUNT
};

inline const char* biomeName(int b) {
    static const char* names[B_COUNT] = {
        "none","mushroom_fields","deep_frozen_ocean","deep_cold_ocean","deep_ocean","deep_lukewarm_ocean",
        "warm_ocean","frozen_ocean","cold_ocean","ocean","lukewarm_ocean","snowy_plains","snowy_taiga",
        "taiga","plains","forest","old_growth_spruce_taiga","flower_forest","birch_forest","dark_forest",
        "savanna","jungle","desert","ice_spikes","old_growth_pine_taiga","sunflower_plains",
        "old_growth_birch_forest","sparse_jungle","bamboo_jungle","meadow","savanna_plateau","badlands",
        "wooded_badlands","cherry_grove","eroded_badlands","windswept_gravelly_hills","windswept_hills",
        "windswept_forest","stony_shore","swamp","mangrove_swamp","frozen_river","river","snowy_beach",
        "beach","snowy_slopes","grove","jagged_peaks","frozen_peaks","stony_peaks","windswept_savanna",
        "dripstone_caves","lush_caves","deep_dark"
    };
    if (b < 0 || b >= B_COUNT) return "none";
    return names[b];
}

namespace climate {
using q_t = long long;
inline q_t quantize(float f) { return (q_t)(long long)(f * 10000.0f); }
struct Param {
    q_t min, max;
    static Param span(float lo, float hi) { return Param{quantize(lo), quantize(hi)}; }
    static Param point(float f) { return span(f, f); }
    static Param spanPP(const Param& a, const Param& b) { return Param{a.min, b.max}; }
    q_t distance(q_t v) const { q_t a = v - max, b = min - v; if (a > 0) return a; return b > 0 ? b : 0; }
};
struct PPoint {
    Param t, h, c, e, d, w; q_t offset; int biome;
    long long fitness(q_t T, q_t H, q_t C, q_t E, q_t D, q_t W) const {
        long long s = 0, x;
        x = t.distance(T); s += x * x;
        x = h.distance(H); s += x * x;
        x = c.distance(C); s += x * x;
        x = e.distance(E); s += x * x;
        x = d.distance(D); s += x * x;
        x = w.distance(W); s += x * x;
        s += offset * offset;
        return s;
    }
};
} // namespace climate

// ---------- Построение списка параметров (OverworldBiomeBuilder) ----------
struct BiomeBuilder {
    using P = climate::Param;
    using PP = climate::PPoint;
    std::vector<PP>* out;

    P FULL = P::span(-1.0f, 1.0f);
    P T[5] = { P::span(-1.0f,-0.45f), P::span(-0.45f,-0.15f), P::span(-0.15f,0.2f), P::span(0.2f,0.55f), P::span(0.55f,1.0f) };
    P H[5] = { P::span(-1.0f,-0.35f), P::span(-0.35f,-0.1f), P::span(-0.1f,0.1f), P::span(0.1f,0.3f), P::span(0.3f,1.0f) };
    P E[7] = { P::span(-1.0f,-0.78f), P::span(-0.78f,-0.375f), P::span(-0.375f,-0.2225f), P::span(-0.2225f,0.05f), P::span(0.05f,0.45f), P::span(0.45f,0.55f), P::span(0.55f,1.0f) };
    P FROZEN = T[0];
    P UNFROZEN = P::spanPP(T[1], T[4]);
    P mushroom = P::span(-1.2f,-1.05f);
    P deepOcean = P::span(-1.05f,-0.455f);
    P ocean = P::span(-0.455f,-0.19f);
    P coast = P::span(-0.19f,-0.11f);
    P inland = P::span(-0.11f,0.55f);
    P nearInland = P::span(-0.11f,0.03f);
    P midInland = P::span(0.03f,0.3f);
    P farInland = P::span(0.3f,1.0f);

    int OCEANS[2][5] = {
        { B_DEEP_FROZEN_OCEAN, B_DEEP_COLD_OCEAN, B_DEEP_OCEAN, B_DEEP_LUKEWARM_OCEAN, B_WARM_OCEAN },
        { B_FROZEN_OCEAN, B_COLD_OCEAN, B_OCEAN, B_LUKEWARM_OCEAN, B_WARM_OCEAN }
    };
    int MIDDLE[5][5] = {
        { B_SNOWY_PLAINS, B_SNOWY_PLAINS, B_SNOWY_PLAINS, B_SNOWY_TAIGA, B_TAIGA },
        { B_PLAINS, B_PLAINS, B_FOREST, B_TAIGA, B_OLD_GROWTH_SPRUCE_TAIGA },
        { B_FLOWER_FOREST, B_PLAINS, B_FOREST, B_BIRCH_FOREST, B_DARK_FOREST },
        { B_SAVANNA, B_SAVANNA, B_FOREST, B_JUNGLE, B_JUNGLE },
        { B_DESERT, B_DESERT, B_DESERT, B_DESERT, B_DESERT }
    };
    int MIDDLE_V[5][5] = {
        { B_ICE_SPIKES, B_NONE, B_SNOWY_TAIGA, B_NONE, B_NONE },
        { B_NONE, B_NONE, B_NONE, B_NONE, B_OLD_GROWTH_PINE_TAIGA },
        { B_SUNFLOWER_PLAINS, B_NONE, B_NONE, B_OLD_GROWTH_BIRCH_FOREST, B_NONE },
        { B_NONE, B_NONE, B_PLAINS, B_SPARSE_JUNGLE, B_BAMBOO_JUNGLE },
        { B_NONE, B_NONE, B_NONE, B_NONE, B_NONE }
    };
    int PLATEAU[5][5] = {
        { B_SNOWY_PLAINS, B_SNOWY_PLAINS, B_SNOWY_PLAINS, B_SNOWY_TAIGA, B_SNOWY_TAIGA },
        { B_MEADOW, B_MEADOW, B_FOREST, B_TAIGA, B_OLD_GROWTH_SPRUCE_TAIGA },
        { B_MEADOW, B_MEADOW, B_MEADOW, B_MEADOW, B_DARK_FOREST },
        { B_SAVANNA_PLATEAU, B_SAVANNA_PLATEAU, B_FOREST, B_FOREST, B_JUNGLE },
        { B_BADLANDS, B_BADLANDS, B_BADLANDS, B_WOODED_BADLANDS, B_WOODED_BADLANDS }
    };
    int PLATEAU_V[5][5] = {
        { B_ICE_SPIKES, B_NONE, B_NONE, B_NONE, B_NONE },
        { B_CHERRY_GROVE, B_NONE, B_MEADOW, B_MEADOW, B_OLD_GROWTH_PINE_TAIGA },
        { B_CHERRY_GROVE, B_CHERRY_GROVE, B_FOREST, B_BIRCH_FOREST, B_NONE },
        { B_NONE, B_NONE, B_NONE, B_NONE, B_NONE },
        { B_ERODED_BADLANDS, B_ERODED_BADLANDS, B_NONE, B_NONE, B_NONE }
    };
    int SHATTERED[5][5] = {
        { B_WINDSWEPT_GRAVELLY_HILLS, B_WINDSWEPT_GRAVELLY_HILLS, B_WINDSWEPT_HILLS, B_WINDSWEPT_FOREST, B_WINDSWEPT_FOREST },
        { B_WINDSWEPT_GRAVELLY_HILLS, B_WINDSWEPT_GRAVELLY_HILLS, B_WINDSWEPT_HILLS, B_WINDSWEPT_FOREST, B_WINDSWEPT_FOREST },
        { B_WINDSWEPT_HILLS, B_WINDSWEPT_HILLS, B_WINDSWEPT_HILLS, B_WINDSWEPT_FOREST, B_WINDSWEPT_FOREST },
        { B_NONE, B_NONE, B_NONE, B_NONE, B_NONE },
        { B_NONE, B_NONE, B_NONE, B_NONE, B_NONE }
    };

    // ---- эмиттеры ----
    void sb(P t, P h, P c, P e, P w, float off, int biome) {
        out->push_back(PP{ t, h, c, e, P::point(0.0f), w, climate::quantize(off), biome });
        out->push_back(PP{ t, h, c, e, P::point(1.0f), w, climate::quantize(off), biome });
    }
    void ub(P t, P h, P c, P e, P w, float off, int biome) {
        out->push_back(PP{ t, h, c, e, P::span(0.2f,0.9f), w, climate::quantize(off), biome });
    }
    void bb(P t, P h, P c, P e, P w, float off, int biome) {
        out->push_back(PP{ t, h, c, e, P::point(1.1f), w, climate::quantize(off), biome });
    }

    // ---- pick* ----
    int pickMiddle(int i, int j, P wd) {
        if (wd.max < 0) return MIDDLE[i][j];
        int v = MIDDLE_V[i][j];
        return v == B_NONE ? MIDDLE[i][j] : v;
    }
    int pickBadlands(int j, P wd) {
        if (j < 2) return wd.max < 0 ? B_BADLANDS : B_ERODED_BADLANDS;
        if (j < 3) return B_BADLANDS;
        return B_WOODED_BADLANDS;
    }
    int pickMiddleOrBadlandsIfHot(int i, int j, P wd) {
        return i == 4 ? pickBadlands(j, wd) : pickMiddle(i, j, wd);
    }
    int pickMiddleOrBadlandsIfHotOrSlopeIfCold(int i, int j, P wd) {
        return i == 0 ? pickSlope(i, j, wd) : pickMiddleOrBadlandsIfHot(i, j, wd);
    }
    int maybeWindsweptSavanna(int i, int j, P wd, int fallback) {
        if (i > 1 && j < 4 && wd.max >= 0) return B_WINDSWEPT_SAVANNA;
        return fallback;
    }
    int pickShatteredCoast(int i, int j, P wd) {
        int rk = wd.max >= 0 ? pickMiddle(i, j, wd) : pickBeach(i, j);
        return maybeWindsweptSavanna(i, j, wd, rk);
    }
    int pickBeach(int i, int j) {
        if (i == 0) return B_SNOWY_BEACH;
        if (i == 4) return B_DESERT;
        return B_BEACH;
    }
    int pickPlateau(int i, int j, P wd) {
        if (wd.max >= 0) { int v = PLATEAU_V[i][j]; if (v != B_NONE) return v; }
        return PLATEAU[i][j];
    }
    int pickPeak(int i, int j, P wd) {
        if (i <= 2) return wd.max < 0 ? B_JAGGED_PEAKS : B_FROZEN_PEAKS;
        if (i == 3) return B_STONY_PEAKS;
        return pickBadlands(j, wd);
    }
    int pickSlope(int i, int j, P wd) {
        if (i >= 3) return pickPlateau(i, j, wd);
        if (j <= 1) return B_SNOWY_SLOPES;
        return B_GROVE;
    }
    int pickShattered(int i, int j, P wd) {
        int rk = SHATTERED[i][j];
        return rk == B_NONE ? pickMiddle(i, j, wd) : rk;
    }

    // ---- слои ----
    void addOffCoast() {
        sb(FULL, FULL, mushroom, FULL, FULL, 0.0f, B_MUSHROOM_FIELDS);
        for (int i = 0; i < 5; ++i) {
            sb(T[i], FULL, deepOcean, FULL, FULL, 0.0f, OCEANS[0][i]);
            sb(T[i], FULL, ocean, FULL, FULL, 0.0f, OCEANS[1][i]);
        }
    }
    void addPeaks(P wd) {
        for (int i = 0; i < 5; ++i) { P t = T[i];
            for (int j = 0; j < 5; ++j) { P h = H[j];
                int rk = pickMiddle(i, j, wd);
                int rk2 = pickMiddleOrBadlandsIfHot(i, j, wd);
                int rk3 = pickMiddleOrBadlandsIfHotOrSlopeIfCold(i, j, wd);
                int rk4 = pickPlateau(i, j, wd);
                int rk5 = pickShattered(i, j, wd);
                int rk6 = maybeWindsweptSavanna(i, j, wd, rk5);
                int rk7 = pickPeak(i, j, wd);
                sb(t, h, P::spanPP(coast, farInland), E[0], wd, 0.0f, rk7);
                sb(t, h, P::spanPP(coast, nearInland), E[1], wd, 0.0f, rk3);
                sb(t, h, P::spanPP(midInland, farInland), E[1], wd, 0.0f, rk7);
                sb(t, h, P::spanPP(coast, nearInland), P::spanPP(E[2], E[3]), wd, 0.0f, rk);
                sb(t, h, P::spanPP(midInland, farInland), E[2], wd, 0.0f, rk4);
                sb(t, h, midInland, E[3], wd, 0.0f, rk2);
                sb(t, h, farInland, E[3], wd, 0.0f, rk4);
                sb(t, h, P::spanPP(coast, farInland), E[4], wd, 0.0f, rk);
                sb(t, h, P::spanPP(coast, nearInland), E[5], wd, 0.0f, rk6);
                sb(t, h, P::spanPP(midInland, farInland), E[5], wd, 0.0f, rk5);
                sb(t, h, P::spanPP(coast, farInland), E[6], wd, 0.0f, rk);
            }
        }
    }
    void addHighSlice(P wd) {
        for (int i = 0; i < 5; ++i) { P t = T[i];
            for (int j = 0; j < 5; ++j) { P h = H[j];
                int rk = pickMiddle(i, j, wd);
                int rk2 = pickMiddleOrBadlandsIfHot(i, j, wd);
                int rk3 = pickMiddleOrBadlandsIfHotOrSlopeIfCold(i, j, wd);
                int rk4 = pickPlateau(i, j, wd);
                int rk5 = pickShattered(i, j, wd);
                int rk6 = maybeWindsweptSavanna(i, j, wd, rk);
                int rk7 = pickSlope(i, j, wd);
                int rk8 = pickPeak(i, j, wd);
                sb(t, h, coast, P::spanPP(E[0], E[1]), wd, 0.0f, rk);
                sb(t, h, nearInland, E[0], wd, 0.0f, rk7);
                sb(t, h, P::spanPP(midInland, farInland), E[0], wd, 0.0f, rk8);
                sb(t, h, nearInland, E[1], wd, 0.0f, rk3);
                sb(t, h, P::spanPP(midInland, farInland), E[1], wd, 0.0f, rk7);
                sb(t, h, P::spanPP(coast, nearInland), P::spanPP(E[2], E[3]), wd, 0.0f, rk);
                sb(t, h, P::spanPP(midInland, farInland), E[2], wd, 0.0f, rk4);
                sb(t, h, midInland, E[3], wd, 0.0f, rk2);
                sb(t, h, farInland, E[3], wd, 0.0f, rk4);
                sb(t, h, P::spanPP(coast, farInland), E[4], wd, 0.0f, rk);
                sb(t, h, P::spanPP(coast, nearInland), E[5], wd, 0.0f, rk6);
                sb(t, h, P::spanPP(midInland, farInland), E[5], wd, 0.0f, rk5);
                sb(t, h, P::spanPP(coast, farInland), E[6], wd, 0.0f, rk);
            }
        }
    }
    void addMidSlice(P wd) {
        sb(FULL, FULL, coast, P::spanPP(E[0], E[2]), wd, 0.0f, B_STONY_SHORE);
        sb(P::spanPP(T[1], T[2]), FULL, P::spanPP(nearInland, farInland), E[6], wd, 0.0f, B_SWAMP);
        sb(P::spanPP(T[3], T[4]), FULL, P::spanPP(nearInland, farInland), E[6], wd, 0.0f, B_MANGROVE_SWAMP);
        for (int i = 0; i < 5; ++i) { P t = T[i];
            for (int j = 0; j < 5; ++j) { P h = H[j];
                int rk = pickMiddle(i, j, wd);
                int rk2 = pickMiddleOrBadlandsIfHot(i, j, wd);
                int rk3 = pickMiddleOrBadlandsIfHotOrSlopeIfCold(i, j, wd);
                int rk4 = pickShattered(i, j, wd);
                int rk5 = pickPlateau(i, j, wd);
                int rk6 = pickBeach(i, j);
                int rk7 = maybeWindsweptSavanna(i, j, wd, rk);
                int rk8 = pickShatteredCoast(i, j, wd);
                int rk9 = pickSlope(i, j, wd);
                sb(t, h, P::spanPP(nearInland, farInland), E[0], wd, 0.0f, rk9);
                sb(t, h, P::spanPP(nearInland, midInland), E[1], wd, 0.0f, rk3);
                sb(t, h, farInland, E[1], wd, 0.0f, i == 0 ? rk9 : rk5);
                sb(t, h, nearInland, E[2], wd, 0.0f, rk);
                sb(t, h, midInland, E[2], wd, 0.0f, rk2);
                sb(t, h, farInland, E[2], wd, 0.0f, rk5);
                sb(t, h, P::spanPP(coast, nearInland), E[3], wd, 0.0f, rk);
                sb(t, h, P::spanPP(midInland, farInland), E[3], wd, 0.0f, rk2);
                if (wd.max < 0) {
                    sb(t, h, coast, E[4], wd, 0.0f, rk6);
                    sb(t, h, P::spanPP(nearInland, farInland), E[4], wd, 0.0f, rk);
                } else {
                    sb(t, h, P::spanPP(coast, farInland), E[4], wd, 0.0f, rk);
                }
                sb(t, h, coast, E[5], wd, 0.0f, rk8);
                sb(t, h, nearInland, E[5], wd, 0.0f, rk7);
                sb(t, h, P::spanPP(midInland, farInland), E[5], wd, 0.0f, rk4);
                if (wd.max < 0) sb(t, h, coast, E[6], wd, 0.0f, rk6);
                else sb(t, h, coast, E[6], wd, 0.0f, rk);
                if (i != 0) continue;
                sb(t, h, P::spanPP(nearInland, farInland), E[6], wd, 0.0f, rk);
            }
        }
    }
    void addLowSlice(P wd) {
        sb(FULL, FULL, coast, P::spanPP(E[0], E[2]), wd, 0.0f, B_STONY_SHORE);
        sb(P::spanPP(T[1], T[2]), FULL, P::spanPP(nearInland, farInland), E[6], wd, 0.0f, B_SWAMP);
        sb(P::spanPP(T[3], T[4]), FULL, P::spanPP(nearInland, farInland), E[6], wd, 0.0f, B_MANGROVE_SWAMP);
        for (int i = 0; i < 5; ++i) { P t = T[i];
            for (int j = 0; j < 5; ++j) { P h = H[j];
                int rk = pickMiddle(i, j, wd);
                int rk2 = pickMiddleOrBadlandsIfHot(i, j, wd);
                int rk3 = pickMiddleOrBadlandsIfHotOrSlopeIfCold(i, j, wd);
                int rk4 = pickBeach(i, j);
                int rk5 = maybeWindsweptSavanna(i, j, wd, rk);
                int rk6 = pickShatteredCoast(i, j, wd);
                sb(t, h, nearInland, P::spanPP(E[0], E[1]), wd, 0.0f, rk2);
                sb(t, h, P::spanPP(midInland, farInland), P::spanPP(E[0], E[1]), wd, 0.0f, rk3);
                sb(t, h, nearInland, P::spanPP(E[2], E[3]), wd, 0.0f, rk);
                sb(t, h, P::spanPP(midInland, farInland), P::spanPP(E[2], E[3]), wd, 0.0f, rk2);
                sb(t, h, coast, P::spanPP(E[3], E[4]), wd, 0.0f, rk4);
                sb(t, h, P::spanPP(nearInland, farInland), E[4], wd, 0.0f, rk);
                sb(t, h, coast, E[5], wd, 0.0f, rk6);
                sb(t, h, nearInland, E[5], wd, 0.0f, rk5);
                sb(t, h, P::spanPP(midInland, farInland), E[5], wd, 0.0f, rk);
                sb(t, h, coast, E[6], wd, 0.0f, rk4);
                if (i != 0) continue;
                sb(t, h, P::spanPP(nearInland, farInland), E[6], wd, 0.0f, rk);
            }
        }
    }
    void addValleys(P wd) {
        sb(FROZEN, FULL, coast, P::spanPP(E[0], E[1]), wd, 0.0f, wd.max < 0 ? B_STONY_SHORE : B_FROZEN_RIVER);
        sb(UNFROZEN, FULL, coast, P::spanPP(E[0], E[1]), wd, 0.0f, wd.max < 0 ? B_STONY_SHORE : B_RIVER);
        sb(FROZEN, FULL, nearInland, P::spanPP(E[0], E[1]), wd, 0.0f, B_FROZEN_RIVER);
        sb(UNFROZEN, FULL, nearInland, P::spanPP(E[0], E[1]), wd, 0.0f, B_RIVER);
        sb(FROZEN, FULL, P::spanPP(coast, farInland), P::spanPP(E[2], E[5]), wd, 0.0f, B_FROZEN_RIVER);
        sb(UNFROZEN, FULL, P::spanPP(coast, farInland), P::spanPP(E[2], E[5]), wd, 0.0f, B_RIVER);
        sb(FROZEN, FULL, coast, E[6], wd, 0.0f, B_FROZEN_RIVER);
        sb(UNFROZEN, FULL, coast, E[6], wd, 0.0f, B_RIVER);
        sb(P::spanPP(T[1], T[2]), FULL, P::spanPP(inland, farInland), E[6], wd, 0.0f, B_SWAMP);
        sb(P::spanPP(T[3], T[4]), FULL, P::spanPP(inland, farInland), E[6], wd, 0.0f, B_MANGROVE_SWAMP);
        sb(FROZEN, FULL, P::spanPP(inland, farInland), E[6], wd, 0.0f, B_FROZEN_RIVER);
        for (int i = 0; i < 5; ++i) { P t = T[i];
            for (int j = 0; j < 5; ++j) { P h = H[j];
                int rk = pickMiddleOrBadlandsIfHot(i, j, wd);
                sb(t, h, P::spanPP(midInland, farInland), P::spanPP(E[0], E[1]), wd, 0.0f, rk);
            }
        }
    }
    void addUnderground() {
        ub(FULL, FULL, P::span(0.8f,1.0f), FULL, FULL, 0.0f, B_DRIPSTONE_CAVES);
        ub(FULL, P::span(0.7f,1.0f), FULL, FULL, FULL, 0.0f, B_LUSH_CAVES);
        bb(FULL, FULL, FULL, P::spanPP(E[0], E[1]), FULL, 0.0f, B_DEEP_DARK);
    }
    void addInland() {
        addMidSlice(P::span(-1.0f,-0.93333334f));
        addHighSlice(P::span(-0.93333334f,-0.7666667f));
        addPeaks(P::span(-0.7666667f,-0.56666666f));
        addHighSlice(P::span(-0.56666666f,-0.4f));
        addMidSlice(P::span(-0.4f,-0.26666668f));
        addLowSlice(P::span(-0.26666668f,-0.05f));
        addValleys(P::span(-0.05f,0.05f)); // BIOMESCALE_V1: ванильная ширина речной полосы
        addLowSlice(P::span(0.05f,0.26666668f));
        addMidSlice(P::span(0.26666668f,0.4f));
        addHighSlice(P::span(0.4f,0.56666666f));
        addPeaks(P::span(0.56666666f,0.7666667f));
        addHighSlice(P::span(0.7666667f,0.93333334f));
        addMidSlice(P::span(0.93333334f,1.0f));
    }
    void build(std::vector<PP>& dst) {
        out = &dst;
        addOffCoast();
        addInland();
        addUnderground();
    }
};

// ---------- Источник биомов ----------
struct BiomeSource {
    const OverworldRouter& router;
    std::vector<climate::PPoint> points;
    explicit BiomeSource(const OverworldRouter& r) : router(r) {
        BiomeBuilder b; b.build(points);
    }
    // биом по quart-координатам (QuartPos)
    int getBiome(int quartX, int quartY, int quartZ) const {
        int bx = quartX << 2, by = quartY << 2, bz = quartZ << 2;
        double t, h, c, e, d, w;
        router.climate(bx, by, bz, t, h, c, e, d, w);
        using climate::quantize;
        climate::q_t T = quantize((float)t), Hq = quantize((float)h), C = quantize((float)c),
                     Eq = quantize((float)e), D = quantize((float)d), W = quantize((float)w);
        long long best = LLONG_MAX; int biome = B_NONE;
        for (const auto& p : points) {
            long long f = p.fitness(T, Hq, C, Eq, D, W);
            if (f < best) { best = f; biome = p.biome; }
        }
        return biome;
    }
    int getBiomeAtBlock(int bx, int by, int bz) const { return getBiome(bx >> 2, by >> 2, bz >> 2); }
};

// ---------- Маппинг поверхности (упрощённый, биом-зависимый) ----------
// Полный SurfaceRules.java пока не портирован; здесь — узнаваемые top/filler по биому.
enum SurfaceBlock { SB_AIR, SB_STONE, SB_DIRT, SB_GRASS, SB_SAND, SB_RED_SAND, SB_GRAVEL,
                    SB_WATER, SB_SNOW_BLOCK, SB_TERRACOTTA, SB_MYCELIUM, SB_PODZOL, SB_BEDROCK };
inline SurfaceBlock topBlock(int b) {
    switch (b) {
        case B_DESERT: case B_BEACH: case B_SNOWY_BEACH: return SB_SAND;
        case B_BADLANDS: case B_WOODED_BADLANDS: case B_ERODED_BADLANDS: return SB_RED_SAND;
        case B_STONY_SHORE: case B_STONY_PEAKS: case B_JAGGED_PEAKS: case B_FROZEN_PEAKS:
        case B_WINDSWEPT_GRAVELLY_HILLS: case B_WINDSWEPT_HILLS: return SB_STONE;
        case B_MUSHROOM_FIELDS: return SB_MYCELIUM;
        case B_SNOWY_PLAINS: case B_SNOWY_TAIGA: case B_ICE_SPIKES: case B_SNOWY_SLOPES:
        case B_GROVE: case B_FROZEN_RIVER: return SB_SNOW_BLOCK;
        case B_OLD_GROWTH_PINE_TAIGA: case B_OLD_GROWTH_SPRUCE_TAIGA: return SB_PODZOL;
        case B_DEEP_FROZEN_OCEAN: case B_DEEP_COLD_OCEAN: case B_DEEP_OCEAN: case B_DEEP_LUKEWARM_OCEAN:
        case B_WARM_OCEAN: case B_FROZEN_OCEAN: case B_COLD_OCEAN: case B_OCEAN: case B_LUKEWARM_OCEAN:
        case B_RIVER: return SB_GRAVEL;
        case B_NONE: return SB_STONE;
        default: return SB_GRASS;
    }
}
inline SurfaceBlock fillerBlock(int b) {
    switch (b) {
        case B_DESERT: case B_BEACH: case B_SNOWY_BEACH: return SB_SAND;
        case B_BADLANDS: case B_WOODED_BADLANDS: case B_ERODED_BADLANDS: return SB_TERRACOTTA;
        case B_STONY_SHORE: case B_STONY_PEAKS: case B_JAGGED_PEAKS: case B_FROZEN_PEAKS:
        case B_WINDSWEPT_GRAVELLY_HILLS: case B_WINDSWEPT_HILLS: case B_NONE: return SB_STONE;
        default: return SB_DIRT;
    }
}

} // namespace wg
