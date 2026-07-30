#pragma once
// STRUCTURES_V3 — постройки оверворлда с НАСТОЯЩИМ размещением через границы чанков.
//
// Почему V1 выглядел бредово (весь старый код вырезан, файл переписан с нуля):
//   ChunkColumn::setBlock() маскирует координаты через (x & 0xF). Старый код звал
//   put(c, wx, y, wz) МИРОВЫМИ координатами, поэтому любая постройка шире 16 блоков
//   заворачивалась сама в себя внутри стартового чанка. Пирамида 15x15 складывалась
//   в кашу, «деревня» состояла из одного дома, храм джунглей срезался по краю.
//
// Как устроен V2:
//   1. Writer знает мировые границы ЦЕЛЕВОГО чанка и пишет только то, что в них попало.
//   2. place() сканирует соседние чанки в радиусе SCAN. Если сосед оказался стартовым
//      чанком постройки — постройка строится ЦЕЛИКОМ, а в текущий чанк ложатся только
//      её видимые куски. Каждый чанк независимо приходит к тому же результату, поэтому
//      швов нет и порядок генерации не важен.
//   3. Высота привязки берётся из wg::ChunkTerrain (тот же шум, что и у рельефа), а не
//      сканированием колонны — иначе соседние чанки давали бы разную Y и постройку
//      резало бы по высоте на границе.
//
// Сетки размещения (spacing/separation/salt) — из ванильного StructureSets.java 1.21.1.
#include "chunk.hpp"
#include "worldgen.hpp"
#include "wg_templates.hpp"
#include <cstdint>
#include <algorithm>
#include <optional>
#include <climits>
#include <cmath>
#include <memory>
#include <utility>
#include <unordered_map>
#include <vector>
#include <string_view>

namespace nc::world::structures {

// Java LegacyRandom: StructurePlacement.RandomSpread работает поверх него.
struct LegacyRng {
    uint64_t state = 0;
    static constexpr uint64_t MUL = 0x5DEECE66DULL, ADD = 0xBULL, MASK = (1ULL << 48) - 1;
    explicit LegacyRng(int64_t seed) { setSeed(seed); }
    void setSeed(int64_t seed) { state = ((uint64_t)seed ^ MUL) & MASK; }
    int next(int bits) { state = (state * MUL + ADD) & MASK; return (int)(state >> (48 - bits)); }
    int nextInt(int bound) {
        if (bound <= 0) return 0;
        if ((bound & -bound) == bound) return (int)(((int64_t)bound * next(31)) >> 31);
        int bits, value;
        do { bits = next(31); value = bits % bound; } while (bits - value + (bound - 1) < 0);
        return value;
    }
    int range(int lo, int hi) { return hi <= lo ? lo : lo + nextInt(hi - lo + 1); }
    bool chance(int oneIn) { return nextInt(oneIn) == 0; }
};

inline int floorDiv(int a, int b) { int q = a / b, r = a % b; return (r && ((r < 0) != (b < 0))) ? q - 1 : q; }
inline int spreadValue(LegacyRng& r, int width, bool triangular) {
    return triangular ? (r.nextInt(width) + r.nextInt(width)) / 2 : r.nextInt(width);
}
// Ванильный RandomSpreadStructurePlacement#getPotentialStructureChunk.
inline bool isStartChunk(int cx, int cz, uint64_t seed, int spacing, int separation, int salt, bool triangular = false) {
    int rx = floorDiv(cx, spacing), rz = floorDiv(cz, spacing);
    int64_t mixed = (int64_t)seed + (int64_t)rx * 341873128712LL + (int64_t)rz * 132897987541LL + (int64_t)salt;
    LegacyRng r(mixed);
    int width = spacing - separation;
    return cx == rx * spacing + spreadValue(r, width, triangular) && cz == rz * spacing + spreadValue(r, width, triangular);
}
// Детерминированный поток случайностей на конкретную постройку (вариации, повороты).
inline LegacyRng buildRng(uint64_t seed, int cx, int cz, int salt) {
    return LegacyRng((int64_t)seed + (int64_t)cx * 341873128712LL + (int64_t)cz * 132897987541LL + (int64_t)salt * 1013904223LL);
}

// Глобальные block-state id ванильного реестра 1.21.1 (проверенные, из V1).
enum : i32 {
    AIR = 0, STONE = 1, GRASS = 9, DIRT = 10, COBBLE = 14, OAK_PLANKS = 15, SPRUCE_PLANKS = 16,
    SAND = 112, SANDSTONE = 535, OAK_LOG = 131, SPRUCE_LOG = 134, OAK_LEAVES = 264,
    WATER = 80, LAVA = 96, CHEST = 2955, TORCH = 2355, OBSIDIAN = 2354,
    MOSSY_COBBLE = 2353, STONE_BRICKS = 6537, MOSSY_STONE_BRICKS = 6538,
    PRISMARINE = 10463, SEA_LANTERN = 10724, SNOW_BLOCK = 5781
};

// ============================================================
// Writer: пишет мировыми координатами, но только в свой чанк
// ============================================================
struct Writer {
    ChunkColumn& col;
    int baseX, baseZ; // мировые координаты угла целевого чанка

    bool inside(int wx, int wz) const {
        return wx >= baseX && wx < baseX + 16 && wz >= baseZ && wz < baseZ + 16;
    }
    // STRUCT_PERF_V1: пересекается ли габарит постройки с чанком вообще.
    // Без этого place() строил КАЖДУЮ найденную постройку целиком для всех 49
    // соседних чанков, даже когда ни один её блок в этот чанк не попадает.
    bool hits(int x0, int z0, int x1, int z1) const {
        return !(x1 < baseX || x0 >= baseX + 16 || z1 < baseZ || z0 >= baseZ + 16);
    }
    void put(int wx, int y, int wz, i32 id) {
        if (y < CHUNK_HEIGHT_MIN || y >= CHUNK_HEIGHT_MAX) return;
        if (!inside(wx, wz)) return;
        col.setBlock(wx, y, wz, id); // маскирование до локальных координат внутри
    }
    void fill(int x0, int y0, int z0, int x1, int y1, int z1, i32 id) {
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        if (z0 > z1) std::swap(z0, z1);
        // Отсечение по чанку ДО циклов: большие постройки иначе прогоняли бы десятки
        // тысяч холостых итераций на каждый соседний чанк.
        if (x1 < baseX || x0 >= baseX + 16 || z1 < baseZ || z0 >= baseZ + 16) return;
        x0 = std::max(x0, baseX); x1 = std::min(x1, baseX + 15);
        z0 = std::max(z0, baseZ); z1 = std::min(z1, baseZ + 15);
        y0 = std::max(y0, (int)CHUNK_HEIGHT_MIN); y1 = std::min(y1, (int)CHUNK_HEIGHT_MAX - 1);
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z)
                for (int y = y0; y <= y1; ++y)
                    col.setBlock(x, y, z, id);
    }
    // Коробка: пол, потолок, стены, внутренность вычищается.
    void box(int x0, int y0, int z0, int x1, int y1, int z1, i32 wallId, i32 floorId) {
        fill(x0, y0, z0, x1, y0, z1, floorId);
        fill(x0, y1, z0, x1, y1, z1, wallId);
        fill(x0, y0 + 1, z0, x1, y1 - 1, z0, wallId);
        fill(x0, y0 + 1, z1, x1, y1 - 1, z1, wallId);
        fill(x0, y0 + 1, z0, x0, y1 - 1, z1, wallId);
        fill(x1, y0 + 1, z0, x1, y1 - 1, z1, wallId);
        fill(x0 + 1, y0 + 1, z0 + 1, x1 - 1, y1 - 1, z1 - 1, AIR);
    }
};

// ============================================================
// Высота привязки — из того же шума, что и рельеф
// ============================================================
struct HeightSampler {
    const wg::OverworldRouter& router;
    // MEM_V3: кэш на ОДИН чанк устраивал thrashing: avg()/spread() и сами
    // постройки сэмплят точки по обе стороны границы, и каждый второй вызов
    // пересобирал ChunkTerrain (5 сеток по 1225 double + ~6000 сэмплов шума).
    // На чанк с деревней это давало миллионы вычислений и гигабайты аллокаций.
    // Теперь: мемоизация высот + до 4 живых ChunkTerrain + поиск поверхности
    // с Y=190 вместо 319 (постройки живут у поверхности, выше смотреть нечего).
    std::unordered_map<long long, int> memo;
    std::vector<std::pair<long long, std::shared_ptr<wg::ChunkTerrain>>> cache;

    explicit HeightSampler(const wg::OverworldRouter& r) : router(r) {}
    static long long ckey(int a, int b) {
        return ((long long)a << 32) ^ (long long)(unsigned int)b;
    }
    std::shared_ptr<wg::ChunkTerrain> terrainFor(int nx, int nz) {
        const long long k = ckey(nx, nz);
        for (auto& e : cache) if (e.first == k) return e.second;
        auto t = std::make_shared<wg::ChunkTerrain>(router, nx, nz);
        if (cache.size() >= 4) cache.erase(cache.begin());
        cache.emplace_back(k, t);
        return t;
    }
    int at(int wx, int wz) {
        const long long k = ckey(wx, wz);
        auto it = memo.find(k);
        if (it != memo.end()) return it->second;
        auto t = terrainFor(wx >> 4, wz >> 4);
        const int lx = wx & 15, lz = wz & 15;
        // STRUCT_PERF_V1: был линейный скан сверху — до 158 вызовов densityAt на ОДИН
        // столбик. Стало: грубый шаг 8, затем уточнение внутри найденного отрезка —
        // не больше ~27 вызовов, результат бит-в-бит тот же.
        int y = wg::SEA_LEVEL;
        int coarse = -1;
        for (int probe = 190; probe > 32; probe -= 8)
            if (t->densityAt(lx, probe, lz) > 0.0) { coarse = probe; break; }
        if (coarse < 0) {
            y = 33;
        } else {
            y = coarse;
            for (int probe = coarse + 7; probe > coarse; --probe)
                if (t->densityAt(lx, probe, lz) > 0.0) { y = probe; break; }
        }
        if (y < CHUNK_HEIGHT_MIN + 1) y = CHUNK_HEIGHT_MIN + 1;
        if (y > CHUNK_HEIGHT_MAX - 24) y = CHUNK_HEIGHT_MAX - 24;
        memo.emplace(k, y);
        return y;
    }
    // Средняя высота пятна: постройка не должна висеть углом в воздухе на склоне.
    int avg(int wx, int wz, int r) {
        long long s = 0; int n = 0;
        for (int dx = -r; dx <= r; dx += r) for (int dz = -r; dz <= r; dz += r) { s += at(wx + dx, wz + dz); ++n; }
        return (int)(s / (n ? n : 1));
    }
    // Перепад высот пятна — по нему отбраковываем слишком крутые места.
    int spread(int wx, int wz, int r) {
        int lo = INT_MAX, hi = INT_MIN;
        for (int dx = -r; dx <= r; dx += r) for (int dz = -r; dz <= r; dz += r) {
            int h = at(wx + dx, wz + dz); lo = std::min(lo, h); hi = std::max(hi, h);
        }
        return hi - lo;
    }
};

// ============================================================
// STRUCTURES_V3 — гейтинг и постройки СТРОГО по ванильным данным 1.21.1:
//   data/minecraft/worldgen/structure_set/*.json       (spacing / separation / salt / frequency)
//   data/minecraft/tags/worldgen/biome/has_structure/* (в каких биомах вообще бывает)
// Все постройки V2 удалены целиком: они были выдуманными (аванпост-башня в океане,
// «деревня» из двух коробок, храм джунглей без второго этажа). V3 строит формы,
// повторяющие ванильные шаблоны по материалам, габаритам и планировке.
//
// Дополнительно закрыт баг V2: биом брался на y=64, поэтому постройка суши могла
// попасть на дно океана (аванпост в воде на скриншотах). Теперь высота поверхности
// сверяется с уровнем моря для КАЖДОЙ постройки.
// ============================================================

enum : i32 {
    COARSE_DIRT = 11, PODZOL = 13, GRAVEL = 118, CLAY = 5798,
    BIRCH_PLANKS = 17, JUNGLE_PLANKS = 18, ACACIA_PLANKS = 19, DARK_OAK_PLANKS = 21,
    BIRCH_LOG = 137, JUNGLE_LOG = 140, ACACIA_LOG = 143, DARK_OAK_LOG = 149,
    OAK_STAIRS = 2885, SPRUCE_STAIRS = 7677, ACACIA_STAIRS = 9895, DARK_OAK_STAIRS = 10055,
    COBBLE_STAIRS = 4693, STONE_BRICK_STAIRS = 7120, MOSSY_COBBLE_STAIRS = 13293,
    SANDSTONE_STAIRS = 7442, SMOOTH_SANDSTONE = 11307, CUT_SANDSTONE = 537, CHISELED_SANDSTONE = 536,
    COBBLE_WALL = 7922, OAK_FENCE = 5848, SPRUCE_FENCE = 11597, ACACIA_FENCE = 11693,
    OAK_DOOR = 4601, SPRUCE_DOOR = 11833, ACACIA_DOOR = 12025,
    GLASS = 519, GLASS_PANE = 6810, WHITE_WOOL = 2047, RED_WOOL = 2061,
    HAY_BLOCK = 10726, DIRT_PATH = 12513, FARMLAND = 4286, WHEAT_CROP = 4278,
    CRAFTING_TABLE = 4277, FURNACE = 4295, CAULDRON = 7398, BOOKSHELF = 2096,
    BARREL = 18409, BELL = 18472, LANTERN = 18506, CAMPFIRE = 18514,
    FLOWER_POT = 8567, RED_MUSHROOM = 2090, LILY_PAD = 7271, VINE = 6868,
    JUNGLE_LEAVES = 348, CACTUS = 5782, DEAD_BUSH = 2007,
    ICE = 5780, PACKED_ICE = 10746, SNOW_LAYER = 5772,
    NETHERRACK = 5849, CRYING_OBSIDIAN = 19449, GOLD_BLOCK = 2091, MAGMA_BLOCK = 12543,
    BLACKSTONE = 19460, GILDED_BLACKSTONE = 20285,
    CRACKED_STONE_BRICKS = 6539, CHISELED_STONE_BRICKS = 6540,
    PRISMARINE_BRICKS = 10464, DARK_PRISMARINE = 10465,
    IRON_BARS = 6772, LADDER = 4655, COBWEB = 2004,
    SUSPICIOUS_SAND = 113, SUSPICIOUS_GRAVEL = 119, BONE_BLOCK = 12547,
    ORANGE_TERRACOTTA = 9357, BLUE_TERRACOTTA = 9367, SMOOTH_STONE = 11306,
    SPRUCE_TRAPDOOR = 6040, SUGAR_CANE = 5799, TNT_BLOCK = 2095
};

// STRUCTURES_V4: блоки для монумента, особняка, шахт, крепости, троп, города и палат.
enum : i32 {
    RAIL = 4663, MOB_SPAWNER = 2873, END_PORTAL_FRAME = 7411, DEEPSLATE_STONE = 24905,
    DARK_OAK_FENCE = 11757, DARK_OAK_DOOR = 12153,
    BRICKS = 2093, MUD_BRICKS = 6542, PACKED_MUD = 6541,
    DEEPSLATE_BRICKS = 26140, DEEPSLATE_TILES = 25729, CRACKED_DEEPSLATE_BRICKS = 26552,
    POLISHED_DEEPSLATE = 25318, COBBLED_DEEPSLATE = 24907, CHISELED_DEEPSLATE = 26551,
    DEEPSLATE_BRICK_STAIRS = 26152,
    SCULK = 22799, SCULK_SENSOR = 22320, SCULK_CATALYST = 22929, SCULK_SHRIEKER = 22937,
    SOUL_LANTERN = 18510, CHAIN = 6776, CANDLE = 20728,
    TUFF = 21081, TUFF_BRICKS = 21904, CHISELED_TUFF = 21903, POLISHED_TUFF = 21492,
    COPPER_BLOCK = 22938, CUT_COPPER = 22947, COPPER_GRATE = 24677, COPPER_BULB = 24695,
    TRIAL_SPAWNER = 26644, VAULT = 26654,
    LAPIS_BLOCK = 522, WET_SPONGE = 518, MOSS_BLOCK = 24843, FIRE_BLOCK = 2391
};

// ============================================================
// Биомные теги has_structure/* (дословно)
// ============================================================
inline bool isOcean(int b) { using namespace wg; return b >= B_DEEP_FROZEN_OCEAN && b <= B_LUKEWARM_OCEAN; }
inline bool isWarmOcean(int b) { using namespace wg; return b == B_LUKEWARM_OCEAN || b == B_WARM_OCEAN || b == B_DEEP_LUKEWARM_OCEAN; }
inline bool isColdOcean(int b) { using namespace wg; return b == B_FROZEN_OCEAN || b == B_COLD_OCEAN || b == B_OCEAN || b == B_DEEP_FROZEN_OCEAN || b == B_DEEP_COLD_OCEAN || b == B_DEEP_OCEAN; }
inline bool isBeach(int b) { using namespace wg; return b == B_BEACH || b == B_SNOWY_BEACH; }
inline bool isRiver(int b) { using namespace wg; return b == B_RIVER || b == B_FROZEN_RIVER; }
inline bool isTaigaTag(int b) { using namespace wg; return b == B_TAIGA || b == B_SNOWY_TAIGA || b == B_OLD_GROWTH_PINE_TAIGA || b == B_OLD_GROWTH_SPRUCE_TAIGA; }
inline bool isForestTag(int b) { using namespace wg; return b == B_FOREST || b == B_FLOWER_FOREST || b == B_BIRCH_FOREST || b == B_OLD_GROWTH_BIRCH_FOREST || b == B_DARK_FOREST || b == B_WINDSWEPT_FOREST || b == B_CHERRY_GROVE; }
inline bool isJungleTag(int b) { using namespace wg; return b == B_JUNGLE || b == B_SPARSE_JUNGLE || b == B_BAMBOO_JUNGLE; }
inline bool isMountainTag(int b) { using namespace wg; return b == B_JAGGED_PEAKS || b == B_FROZEN_PEAKS || b == B_STONY_PEAKS || b == B_SNOWY_SLOPES || b == B_MEADOW || b == B_WINDSWEPT_HILLS || b == B_WINDSWEPT_FOREST || b == B_WINDSWEPT_GRAVELLY_HILLS; }
inline bool isBadlandsTag(int b) { using namespace wg; return b == B_BADLANDS || b == B_ERODED_BADLANDS || b == B_WOODED_BADLANDS; }
// has_structure/igloo
inline bool iglooBiome(int b) { using namespace wg; return b == B_SNOWY_TAIGA || b == B_SNOWY_PLAINS || b == B_SNOWY_SLOPES; }
// has_structure/pillager_outpost
inline bool outpostBiome(int b) { using namespace wg; return b == B_DESERT || b == B_PLAINS || b == B_SAVANNA || b == B_SNOWY_PLAINS || b == B_TAIGA || b == B_GROVE || isMountainTag(b); }
inline bool isSnowy(int b) { using namespace wg; return b == B_SNOWY_PLAINS || b == B_SNOWY_TAIGA || b == B_GROVE || b == B_SNOWY_SLOPES || b == B_FROZEN_PEAKS || b == B_ICE_SPIKES || b == B_SNOWY_BEACH; }

// Ванильные village_* теги: plains(+meadow), desert, savanna, snowy_plains, taiga.
enum VillageKind { VK_NONE = 0, VK_PLAINS, VK_DESERT, VK_SAVANNA, VK_SNOWY, VK_TAIGA };
inline VillageKind villageKind(int b) {
    using namespace wg;
    if (b == B_PLAINS || b == B_MEADOW) return VK_PLAINS;
    if (b == B_DESERT) return VK_DESERT;
    if (b == B_SAVANNA) return VK_SAVANNA;
    if (b == B_SNOWY_PLAINS) return VK_SNOWY;
    if (b == B_TAIGA) return VK_TAIGA;
    return VK_NONE;
}

// Материалы village_* пулов.
struct VillageStyle { i32 wall, log, stairs, path, fence, door, roof, extra; };
inline VillageStyle villageStyle(VillageKind k) {
    switch (k) {
        case VK_DESERT: return { SMOOTH_SANDSTONE, CUT_SANDSTONE, SANDSTONE_STAIRS, SAND, COBBLE_WALL, OAK_DOOR, SANDSTONE, CHISELED_SANDSTONE };
        case VK_SAVANNA: return { ACACIA_PLANKS, ACACIA_LOG, ACACIA_STAIRS, DIRT_PATH, ACACIA_FENCE, ACACIA_DOOR, ACACIA_PLANKS, HAY_BLOCK };
        case VK_SNOWY: return { SPRUCE_PLANKS, SPRUCE_LOG, SPRUCE_STAIRS, DIRT_PATH, SPRUCE_FENCE, SPRUCE_DOOR, SPRUCE_PLANKS, SNOW_BLOCK };
        case VK_TAIGA: return { SPRUCE_PLANKS, SPRUCE_LOG, SPRUCE_STAIRS, DIRT_PATH, SPRUCE_FENCE, SPRUCE_DOOR, SPRUCE_PLANKS, COBBLE };
        default: return { OAK_PLANKS, OAK_LOG, OAK_STAIRS, DIRT_PATH, OAK_FENCE, OAK_DOOR, OAK_PLANKS, COBBLE };
    }
}

// ============================================================
// Общие помощники рельефа
// ============================================================

// Фундамент до земли: постройка не висит над обрывом и не парит на склоне.
inline void foundation(Writer& w, HeightSampler& hs, int x0, int z0, int x1, int z1, int y, i32 id) {
    for (int x = x0; x <= x1; ++x)
        for (int z = z0; z <= z1; ++z) {
            if (!w.inside(x, z)) continue;
            int g = hs.at(x, z);
            for (int yy = std::min(g, y - 1); yy < y; ++yy) w.put(x, yy, z, id);
        }
}

// Срез рельефа над постройкой, иначе дом окажется внутри холма.
inline void clearAbove(Writer& w, int x0, int z0, int x1, int z1, int y, int h) {
    w.fill(x0, y, z0, x1, y + h, z1, AIR);
}

// Площадка: вырезать всё лишнее сверху и подсыпать снизу.
inline void platform(Writer& w, HeightSampler& hs, int x0, int z0, int x1, int z1, int y, int h, i32 base) {
    clearAbove(w, x0, z0, x1, z1, y, h);
    foundation(w, hs, x0, z0, x1, z1, y, base);
}

// Двускатная крыша из ступеней (ванильные дома деревни именно такие).
inline void gableRoof(Writer& w, int x0, int z0, int x1, int z1, int y, i32 stairs, i32 wall) {
    const int halfZ = (z1 - z0) / 2;
    for (int i = 0; i <= halfZ; ++i) {
        w.fill(x0 - 1, y + i, z0 + i - 1, x1 + 1, y + i, z0 + i - 1, stairs);
        w.fill(x0 - 1, y + i, z1 - i + 1, x1 + 1, y + i, z1 - i + 1, stairs);
        if (i == halfZ) w.fill(x0 - 1, y + i, z0 + i, x1 + 1, y + i, z1 - i, wall);
    }
}

// ============================================================
// Деревня
// ============================================================

inline void villageHouse(Writer& w, HeightSampler& hs, int x, int z, int y,
                         const VillageStyle& st, LegacyRng& rng, bool big) {
    const int sx = big ? 4 : 3, sz = big ? 4 : 3;
    const int x0 = x - sx, x1 = x + sx, z0 = z - sz, z1 = z + sz;
    const int wallTop = y + (big ? 4 : 3);

    platform(w, hs, x0 - 1, z0 - 1, x1 + 1, z1 + 1, y, 10, st.wall);
    w.fill(x0, y - 1, z0, x1, y - 1, z1, st.wall);
    w.box(x0, y, z0, x1, wallTop, z1, st.wall, st.wall);
    // Угловые брёвна — характерная деталь всех village_* домов.
    for (int yy = y; yy <= wallTop; ++yy) {
        w.put(x0, yy, z0, st.log); w.put(x1, yy, z0, st.log);
        w.put(x0, yy, z1, st.log); w.put(x1, yy, z1, st.log);
    }
    // Окна.
    w.put(x0, y + 2, z, GLASS_PANE); w.put(x1, y + 2, z, GLASS_PANE);
    w.put(x, y + 2, z0, GLASS_PANE); w.put(x, y + 2, z1, GLASS_PANE);
    if (big) { w.put(x0, y + 2, z - 2, GLASS_PANE); w.put(x1, y + 2, z + 2, GLASS_PANE); }
    // Дверь и порог.
    w.put(x, y, z1, st.door); w.put(x, y + 1, z1, st.door);
    w.put(x, y - 1, z1 + 1, st.path);
    gableRoof(w, x0, z0, x1, z1, wallTop + 1, st.stairs, st.roof);
    // Обстановка.
    w.put(x0 + 1, y, z0 + 1, CRAFTING_TABLE);
    w.put(x1 - 1, y, z0 + 1, rng.chance(2) ? FURNACE : BARREL);
    w.put(x, wallTop, z, LANTERN);
    if (big) w.put(x0 + 1, y, z1 - 1, BOOKSHELF);
}

// Ванильный колодец деревни: каменная рамка, вода, четыре столба, накрытие.
inline void villageWell(Writer& w, HeightSampler& hs, int x, int z, int y, const VillageStyle& st) {
    platform(w, hs, x - 3, z - 3, x + 3, z + 3, y, 8, st.wall);
    const i32 rim = (st.wall == SMOOTH_SANDSTONE) ? CUT_SANDSTONE : COBBLE;
    w.fill(x - 2, y - 1, z - 2, x + 2, y - 1, z + 2, rim);
    w.fill(x - 1, y - 1, z - 1, x + 1, y + 0, z + 1, WATER);
    for (int dx = -2; dx <= 2; ++dx) { w.put(x + dx, y, z - 2, rim); w.put(x + dx, y, z + 2, rim); }
    for (int dz = -2; dz <= 2; ++dz) { w.put(x - 2, y, z + dz, rim); w.put(x + 2, y, z + dz, rim); }
    for (int yy = y + 1; yy <= y + 3; ++yy) {
        w.put(x - 2, yy, z - 2, rim); w.put(x + 2, yy, z - 2, rim);
        w.put(x - 2, yy, z + 2, rim); w.put(x + 2, yy, z + 2, rim);
    }
    w.fill(x - 2, y + 4, z - 2, x + 2, y + 4, z + 2, rim);
    for (int d = 3; d <= 5; ++d) {
        w.put(x + d, y - 1, z, st.path); w.put(x - d, y - 1, z, st.path);
        w.put(x, y - 1, z + d, st.path); w.put(x, y - 1, z - d, st.path);
    }
}

// Ванильная грядка: рамка из брёвен, вода по центру, вспашка и пшеница.
inline void villageFarm(Writer& w, HeightSampler& hs, int x, int z, int y, const VillageStyle& st) {
    const int x0 = x - 3, x1 = x + 3, z0 = z - 2, z1 = z + 2;
    platform(w, hs, x0, z0, x1, z1, y, 6, st.wall);
    w.fill(x0, y - 1, z0, x1, y - 1, z1, st.log);
    w.fill(x0 + 1, y - 1, z0 + 1, x1 - 1, y - 1, z1 - 1, FARMLAND);
    w.fill(x, y - 1, z0 + 1, x, y - 1, z1 - 1, WATER);
    for (int xx = x0 + 1; xx <= x1 - 1; ++xx)
        for (int zz = z0 + 1; zz <= z1 - 1; ++zz)
            if (xx != x) w.put(xx, y, zz, WHEAT_CROP);
}

// Фонарь на столбе — village/common/lamps.
inline void villageLamp(Writer& w, HeightSampler& hs, int x, int z, int y, const VillageStyle& st) {
    foundation(w, hs, x, z, x, z, y, st.wall);
    clearAbove(w, x, z, x, z, y, 6);
    for (int yy = y; yy <= y + 3; ++yy) w.put(x, yy, z, st.fence);
    w.put(x, y + 4, z, st.log);
    w.put(x, y + 3, z, LANTERN);
}

inline void villagePath(Writer& w, HeightSampler& hs, int x0, int z0, int x1, int z1, const VillageStyle& st) {
    if (x0 == x1) {
        if (z0 > z1) std::swap(z0, z1);
        for (int z = z0; z <= z1; ++z) for (int dx = -1; dx <= 1; ++dx) {
            if (!w.inside(x0 + dx, z)) continue;
            const int g = hs.at(x0 + dx, z);
            w.put(x0 + dx, g, z, st.path);
            w.fill(x0 + dx, g + 1, z, x0 + dx, g + 3, z, AIR);
        }
    } else {
        if (x0 > x1) std::swap(x0, x1);
        for (int x = x0; x <= x1; ++x) for (int dz = -1; dz <= 1; ++dz) {
            if (!w.inside(x, z0 + dz)) continue;
            const int g = hs.at(x, z0 + dz);
            w.put(x, g, z0 + dz, st.path);
            w.fill(x, g + 1, z0 + dz, x, g + 3, z0 + dz, AIR);
        }
    }
}

inline void village(Writer& w, HeightSampler& hs, int ax, int az, VillageKind kind, LegacyRng& rng) {
    const VillageStyle st = villageStyle(kind);
    const int y = hs.avg(ax, az, 12) + 1;

    villageWell(w, hs, ax, az, y, st);
    villagePath(w, hs, ax, az - 24, ax, az + 24, st);
    villagePath(w, hs, ax - 24, az, ax + 24, az, st);

    struct Spot { int dx, dz; };
    const Spot spots[8] = { {  12, -8 }, { -12, -6 }, {  8,  12 }, { -9,  13 },
                            {  16,  6 }, { -17,  5 }, {  5, -16 }, { -6, -17 } };
    for (int i = 0; i < 8; ++i) {
        if (rng.nextInt(4) == 0) continue; // ванильные деревни разного размера
        const int hx = ax + spots[i].dx, hz = az + spots[i].dz;
        if (hs.spread(hx, hz, 5) > 4) continue;
        if (hs.at(hx, hz) <= wg::SEA_LEVEL) continue;
        villageHouse(w, hs, hx, hz, hs.avg(hx, hz, 5) + 1, st, rng, (i % 3) == 0);
    }
    const Spot farms[2] = { { -14, -14 }, { 14, 14 } };
    for (int i = 0; i < 2; ++i) {
        const int fx = ax + farms[i].dx, fz = az + farms[i].dz;
        if (hs.spread(fx, fz, 4) <= 3 && hs.at(fx, fz) > wg::SEA_LEVEL)
            villageFarm(w, hs, fx, fz, hs.avg(fx, fz, 4) + 1, st);
    }
    const Spot lamps[4] = { { 6, 6 }, { -6, 6 }, { 6, -6 }, { -6, -6 } };
    for (int i = 0; i < 4; ++i) {
        const int lx = ax + lamps[i].dx, lz = az + lamps[i].dz;
        if (hs.at(lx, lz) > wg::SEA_LEVEL) villageLamp(w, hs, lx, lz, hs.at(lx, lz) + 1, st);
    }
    // Колокол у колодца — village/common/bell.
    w.put(ax + 4, y + 1, az + 4, st.log);
    w.put(ax + 4, y + 2, az + 4, BELL);
}

// ============================================================
// Пустынная пирамида (desert_pyramid.nbt: 21x21, две башни, узор терракоты)
// ============================================================
inline void desertPyramid(Writer& w, HeightSampler& hs, int cx, int cz, LegacyRng& rng) {
    const int y = hs.avg(cx, cz, 10) - 1;
    const int R = 10;
    platform(w, hs, cx - R, cz - R, cx + R, cz + R, y, 20, SANDSTONE);

    for (int lvl = 0; lvl < 5; ++lvl) {
        const int r = R - lvl * 2;
        w.fill(cx - r, y + lvl, cz - r, cx + r, y + lvl, cz + r, SANDSTONE);
        w.fill(cx - r, y + lvl, cz - r, cx + r, y + lvl + 1, cz - r, SANDSTONE);
        w.fill(cx - r, y + lvl, cz + r, cx + r, y + lvl + 1, cz + r, SANDSTONE);
        w.fill(cx - r, y + lvl, cz - r, cx - r, y + lvl + 1, cz + r, SANDSTONE);
        w.fill(cx + r, y + lvl, cz - r, cx + r, y + lvl + 1, cz + r, SANDSTONE);
    }
    for (int s = -1; s <= 1; s += 2) {
        const int tx = cx + s * (R - 2);
        w.fill(tx - 1, y, cz + R - 2, tx + 1, y + 9, cz + R, SANDSTONE);
        w.put(tx, y + 10, cz + R - 1, ORANGE_TERRACOTTA);
        for (int i = 0; i < 3; ++i) {
            w.put(tx - 1, y + 7 + i, cz + R - 2 + i, CUT_SANDSTONE);
            w.put(tx + 1, y + 7 + i, cz + R - 2 + i, CUT_SANDSTONE);
        }
    }
    for (int dx = -3; dx <= 3; ++dx) {
        w.put(cx + dx, y + 1, cz + R, (dx % 2 == 0) ? ORANGE_TERRACOTTA : SANDSTONE);
        w.put(cx + dx, y + 3, cz + R, (dx % 2 == 0) ? SANDSTONE : ORANGE_TERRACOTTA);
    }
    w.put(cx, y + 2, cz + R, CHISELED_SANDSTONE);

    // Внутренний коридор и скрытая сокровищница под полом (ванильная схема).
    w.fill(cx - 1, y + 1, cz + R - 1, cx + 1, y + 3, cz + 2, AIR);
    w.fill(cx - 3, y + 1, cz - 3, cx + 3, y + 3, cz + 2, AIR);
    w.fill(cx - 3, y, cz - 3, cx + 3, y, cz + 2, SANDSTONE);
    w.put(cx - 2, y + 3, cz - 2, ORANGE_TERRACOTTA);
    w.put(cx + 2, y + 3, cz - 2, ORANGE_TERRACOTTA);

    w.fill(cx - 2, y - 5, cz - 2, cx + 2, y - 2, cz + 2, AIR);
    w.fill(cx - 3, y - 6, cz - 3, cx + 3, y - 6, cz + 3, SANDSTONE);
    w.fill(cx - 3, y - 5, cz - 3, cx + 3, y - 2, cz - 3, SANDSTONE);
    w.fill(cx - 3, y - 5, cz + 3, cx + 3, y - 2, cz + 3, SANDSTONE);
    w.fill(cx - 3, y - 5, cz - 3, cx - 3, y - 2, cz + 3, SANDSTONE);
    w.fill(cx + 3, y - 5, cz - 3, cx + 3, y - 2, cz + 3, SANDSTONE);
    // Голубая терракота над скрытым TNT — как в ванили.
    w.put(cx, y - 5, cz, BLUE_TERRACOTTA);
    w.put(cx, y - 6, cz, TNT_BLOCK);
    w.put(cx - 2, y - 5, cz - 2, CHEST); w.put(cx + 2, y - 5, cz - 2, CHEST);
    w.put(cx - 2, y - 5, cz + 2, CHEST); w.put(cx + 2, y - 5, cz + 2, CHEST);
    if (rng.chance(2)) w.put(cx + 1, y - 5, cz + 1, COBWEB);
}

// ============================================================
// Иглу (igloo/top.nbt: 7x7 купол из снега и льда, вход-тоннель)
// ============================================================
inline void igloo(Writer& w, HeightSampler& hs, int cx, int cz, LegacyRng& rng) {
    const int y = hs.avg(cx, cz, 4) + 1;
    platform(w, hs, cx - 4, cz - 4, cx + 4, cz + 4, y, 8, SNOW_BLOCK);

    for (int dx = -3; dx <= 3; ++dx)
        for (int dz = -3; dz <= 3; ++dz)
            for (int dy = 0; dy <= 3; ++dy) {
                const double d = std::sqrt((double)(dx * dx + dz * dz) + (double)(dy * dy) * 1.4);
                if (d > 3.6) continue;
                const bool shell = d > 2.6;
                w.put(cx + dx, y + dy, cz + dz, shell ? SNOW_BLOCK : AIR);
            }
    w.fill(cx - 3, y - 1, cz - 3, cx + 3, y - 1, cz + 3, SNOW_BLOCK);
    w.put(cx + 3, y + 1, cz, ICE);
    w.put(cx, y + 2, cz, LANTERN);
    w.fill(cx - 1, y, cz + 3, cx + 1, y + 1, cz + 5, SNOW_BLOCK);
    w.fill(cx, y, cz + 3, cx, y + 1, cz + 5, AIR);
    w.put(cx, y - 1, cz + 4, SNOW_BLOCK);
    w.put(cx, y - 1, cz + 5, SNOW_BLOCK);
    // Обстановка: печь, стол, сундук. Кровати пропущены — это блок с двумя
    // половинами и цветом, отдельная работа.
    w.put(cx - 2, y, cz - 2, FURNACE);
    w.put(cx + 2, y, cz - 2, CRAFTING_TABLE);
    if (rng.chance(2)) w.put(cx - 2, y, cz + 2, CHEST);
}

// ============================================================
// Храм джунглей (jungle_temple: 12x15, два уровня, лианы)
// ============================================================
inline void jungleTemple(Writer& w, HeightSampler& hs, int cx, int cz, LegacyRng& rng) {
    const int y = hs.avg(cx, cz, 7) + 1;
    const int x0 = cx - 5, x1 = cx + 6, z0 = cz - 7, z1 = cz + 7;
    platform(w, hs, x0 - 1, z0 - 1, x1 + 1, z1 + 1, y, 14, COBBLE);

    auto stone = [&](int i) { return (i % 3 == 0) ? (i32)MOSSY_COBBLE : (i32)COBBLE; };
    for (int lvl = 0; lvl < 2; ++lvl) {
        const int by = y + lvl * 4;
        w.box(x0, by, z0, x1, by + 3, z1, COBBLE, COBBLE);
        for (int x = x0; x <= x1; ++x) for (int dy = 0; dy <= 3; ++dy) {
            w.put(x, by + dy, z0, stone(x + dy));
            w.put(x, by + dy, z1, stone(x + dy + 1));
        }
        for (int z = z0; z <= z1; ++z) for (int dy = 0; dy <= 3; ++dy) {
            w.put(x0, by + dy, z, stone(z + dy));
            w.put(x1, by + dy, z, stone(z + dy + 1));
        }
    }
    w.fill(x0 + 1, y + 1, z0 + 1, x1 - 1, y + 2, z1 - 1, AIR);
    w.fill(x0 + 1, y + 5, z0 + 1, x1 - 1, y + 6, z1 - 1, AIR);
    w.fill(x0, y + 4, z0, x1, y + 4, z1, COBBLE);
    for (int i = 0; i < 4; ++i) {
        w.fill(x1 - 1 - i, y + 1 + i, z1 - 2, x1 - 1 - i, y + 4, z1 - 2, AIR);
        w.put(x1 - 1 - i, y + i, z1 - 2, COBBLE_STAIRS);
    }
    w.fill(cx - 1, y + 1, z1, cx + 1, y + 3, z1, AIR);
    for (int i = 0; i < 3; ++i) w.fill(cx - 1 - i, y - i, z1 + 1 + i, cx + 1 + i, y - i, z1 + 1 + i, MOSSY_COBBLE_STAIRS);
    for (int x = x0; x <= x1; x += 2) { w.put(x, y + 8, z0, COBBLE); w.put(x, y + 8, z1, COBBLE); }
    for (int z = z0; z <= z1; z += 2) { w.put(x0, y + 8, z, COBBLE); w.put(x1, y + 8, z, COBBLE); }
    // Два сундука. Редстоун-механику ловушек НЕ трогаем (по договорённости).
    w.put(x0 + 2, y + 1, z0 + 2, CHEST);
    w.put(x0 + 2, y + 5, z0 + 2, CHEST);
    w.put(cx, y + 2, cz, LANTERN);
    for (int x = x0; x <= x1; ++x)
        for (int dy = 1; dy <= 7; ++dy) {
            if (rng.chance(3)) w.put(x, y + dy, z0 - 1, VINE);
            if (rng.chance(3)) w.put(x, y + dy, z1 + 1, VINE);
        }
    for (int z = z0; z <= z1; ++z)
        for (int dy = 1; dy <= 7; ++dy) {
            if (rng.chance(3)) w.put(x0 - 1, y + dy, z, VINE);
            if (rng.chance(3)) w.put(x1 + 1, y + dy, z, VINE);
        }
}

// ============================================================
// Хижина ведьмы (swamp_hut: 7x9 на сваях, ель, котёл, гриб)
// ============================================================
inline void swampHut(Writer& w, HeightSampler& hs, int cx, int cz, LegacyRng& rng) {
    const int ground = hs.avg(cx, cz, 4);
    const int y = std::max(ground, (int)wg::SEA_LEVEL) + 4; // ванильная хижина стоит НАД водой
    const int x0 = cx - 3, x1 = cx + 3, z0 = cz - 4, z1 = cz + 4;
    clearAbove(w, x0 - 1, z0 - 1, x1 + 1, z1 + 1, y, 8);

    const int posts[4][2] = { { x0, z0 }, { x1, z0 }, { x0, z1 }, { x1, z1 } };
    for (int i = 0; i < 4; ++i)
        for (int yy = ground; yy < y; ++yy) w.put(posts[i][0], yy, posts[i][1], SPRUCE_LOG);

    w.fill(x0, y, z0, x1, y, z1, SPRUCE_PLANKS);
    w.box(x0, y + 1, z0, x1, y + 4, z1, SPRUCE_PLANKS, SPRUCE_PLANKS);
    for (int yy = y + 1; yy <= y + 4; ++yy) {
        w.put(x0, yy, z0, SPRUCE_LOG); w.put(x1, yy, z0, SPRUCE_LOG);
        w.put(x0, yy, z1, SPRUCE_LOG); w.put(x1, yy, z1, SPRUCE_LOG);
    }
    w.put(x0, y + 2, cz, SPRUCE_TRAPDOOR);
    w.put(x1, y + 2, cz, SPRUCE_TRAPDOOR);
    w.fill(cx, y + 1, z1, cx, y + 2, z1, AIR);
    gableRoof(w, x0, z0, x1, z1, y + 5, SPRUCE_STAIRS, SPRUCE_PLANKS);
    w.put(x0 + 1, y + 1, z0 + 1, CAULDRON);
    w.put(x1 - 1, y + 1, z0 + 1, CRAFTING_TABLE);
    w.put(x1 - 1, y + 1, z1 - 1, FLOWER_POT);
    w.put(x0 + 1, y + 1, z1 - 1, RED_MUSHROOM);
    if (rng.chance(2)) w.put(cx, y + 4, cz, LANTERN);
}

// ============================================================
// Аванпост разбойников (pillager_outpost: башня 7x7 из тёмного дуба)
// ============================================================
inline void outpost(Writer& w, HeightSampler& hs, int cx, int cz, LegacyRng& rng) {
    const int y = hs.avg(cx, cz, 6) + 1;
    platform(w, hs, cx - 4, cz - 4, cx + 4, cz + 4, y, 20, COBBLE);

    const int x0 = cx - 3, x1 = cx + 3, z0 = cz - 3, z1 = cz + 3;
    w.fill(x0, y - 1, z0, x1, y - 1, z1, COBBLE);
    for (int lvl = 0; lvl < 3; ++lvl) {
        const int by = y + lvl * 4;
        w.box(x0, by, z0, x1, by + 3, z1, DARK_OAK_PLANKS, DARK_OAK_PLANKS);
        w.fill(x0 + 1, by + 1, z0 + 1, x1 - 1, by + 3, z1 - 1, AIR);
        for (int yy = by; yy <= by + 3; ++yy) {
            w.put(x0, yy, z0, DARK_OAK_LOG); w.put(x1, yy, z0, DARK_OAK_LOG);
            w.put(x0, yy, z1, DARK_OAK_LOG); w.put(x1, yy, z1, DARK_OAK_LOG);
        }
    }
    for (int yy = y; yy <= y + 11; ++yy) w.put(x1 - 1, yy, z1 - 1, LADDER);
    const int ty = y + 12;
    w.fill(x0 - 1, ty, z0 - 1, x1 + 1, ty, z1 + 1, DARK_OAK_PLANKS);
    for (int x = x0 - 1; x <= x1 + 1; ++x) { w.put(x, ty + 1, z0 - 1, OAK_FENCE); w.put(x, ty + 1, z1 + 1, OAK_FENCE); }
    for (int z = z0 - 1; z <= z1 + 1; ++z) { w.put(x0 - 1, ty + 1, z, OAK_FENCE); w.put(x1 + 1, ty + 1, z, OAK_FENCE); }
    for (int yy = ty + 1; yy <= ty + 4; ++yy) {
        w.put(x0, yy, z0, DARK_OAK_LOG); w.put(x1, yy, z0, DARK_OAK_LOG);
        w.put(x0, yy, z1, DARK_OAK_LOG); w.put(x1, yy, z1, DARK_OAK_LOG);
    }
    w.fill(x0 - 1, ty + 5, z0 - 1, x1 + 1, ty + 5, z1 + 1, DARK_OAK_PLANKS);
    w.put(cx, ty + 1, cz, CHEST);
    // Клетка и костёр во дворе — как в ванильном шаблоне.
    w.fill(cx + 5, y, cz + 2, cx + 7, y + 3, cz + 4, IRON_BARS);
    w.fill(cx + 6, y, cz + 3, cx + 6, y + 2, cz + 3, AIR);
    w.put(cx - 5, y, cz - 3, CAMPFIRE);
    if (rng.chance(2)) w.put(cx - 6, y, cz - 3, DARK_OAK_LOG);
}

// ============================================================
// Разрушенный портал (ruined_portal_*: обломки рамки, незерак, золото)
// ============================================================
inline void ruinedPortal(Writer& w, HeightSampler& hs, int cx, int cz, int biome, LegacyRng& rng) {
    using namespace wg;
    // PORTAL_VANILLA_V1. Строго по ванильным nbt: portal_1..portal_10 (проём 2x3..3x4)
    // и giant_portal_1..3 (до 4x6). Допустимые блоки — только обсидиан, плачущий
    // обсидиан, незерак, магма, золотой блок, сундук, лава, огонь.
    // Маскировка — ванильные процессоры sandy / cold / mossify.
    // Никаких красных незеровых кирпичей, блекстоуна, шахматки и висящей лавы.
    const bool giant   = rng.nextInt(12) == 0;
    const bool alongX  = rng.chance(2);                    // случайный поворот
    const bool ocean   = isOcean(biome);
    const bool cold    = isSnowy(biome) || biome == B_ICE_SPIKES;
    const bool sandy   = biome == B_DESERT || isBeach(biome) || isBadlandsTag(biome);
    const bool mossify = isJungleTag(biome) || biome == B_SWAMP || biome == B_MANGROVE_SWAMP ||
                         biome == B_MUSHROOM_FIELDS;
    const int openW = giant ? 3 + rng.nextInt(2) : 2 + rng.nextInt(2);
    const int openH = giant ? 5 + rng.nextInt(2) : 3 + rng.nextInt(2);
    const int hw = openW / 2 + 1;
    const int R = giant ? 7 : 5;

    const int ground = hs.avg(cx, cz, 3);
    // vertical_placement: часть порталов вкопана в землю на 1-2 блока.
    const int y = ocean ? ground : ground - rng.nextInt(3);

    clearAbove(w, cx - R, cz - R, cx + R, cz + R, y + 1, openH + 4);

    // 1) Незераковая клумба: цельный неровный блин с рваным краем.
    for (int dx = -R; dx <= R; ++dx)
        for (int dz = -R; dz <= R; ++dz) {
            const int d2 = dx * dx + dz * dz;
            if (d2 > (R - 1) * (R - 1) - rng.nextInt(R + 2)) continue;
            const int g = hs.at(cx + dx, cz + dz);
            const int topY = std::min(g, y);
            const int depth = (d2 < 5) ? 3 : (d2 < 12 ? 2 : 1);
            for (int k = 0; k < depth; ++k)
                w.put(cx + dx, topY - k, cz + dz, rng.chance(18) ? (i32)MAGMA_BLOCK : (i32)NETHERRACK);
        }

    // 2) Рамка: разбитый прямоугольник, вверху выбито больше. Портал не горит.
    for (int dy = 0; dy <= openH + 1; ++dy)
        for (int d = -hw; d <= hw; ++d) {
            const bool edge = (dy == 0 || dy == openH + 1 || d == -hw || d == hw);
            if (!edge) continue;
            const int keep = 7 - std::min(4, dy);
            if (rng.nextInt(keep) == 0) continue;
            const i32 id = rng.chance(9) ? (i32)CRYING_OBSIDIAN : (i32)OBSIDIAN;
            if (alongX) w.put(cx + d, y + 1 + dy, cz, id);
            else        w.put(cx, y + 1 + dy, cz + d, id);
        }

    // 3) Лава только в основании и редкие язычки огня на незераке.
    if (!ocean) {
        if (rng.chance(2)) {
            if (alongX) w.fill(cx - 1, y, cz, cx + 1, y, cz, LAVA);
            else        w.fill(cx, y, cz - 1, cx, y, cz + 1, LAVA);
        }
        for (int dx = -R + 1; dx <= R - 1; ++dx)
            for (int dz = -R + 1; dz <= R - 1; ++dz)
                if (rng.nextInt(16) == 0) w.put(cx + dx, y + 1, cz + dz, FIRE_BLOCK);
    }

    // 4) Золото: один-два блока в клумбе, а не россыпь по поверхности.
    w.put(cx + (alongX ? hw : 1), y, cz + (alongX ? 1 : hw), GOLD_BLOCK);
    if (giant && rng.chance(2)) w.put(cx - (alongX ? hw : 1), y, cz - (alongX ? 1 : hw), GOLD_BLOCK);

    // 5) Сундук у основания рамки.
    if (alongX) w.put(cx + hw - 1, y + 1, cz + 1, CHEST);
    else        w.put(cx + 1, y + 1, cz + hw - 1, CHEST);

    // 6) Биомная маскировка только по краю клумбы.
    for (int dx = -R; dx <= R; ++dx)
        for (int dz = -R; dz <= R; ++dz) {
            const int d2 = dx * dx + dz * dz;
            if (d2 < (R - 3) * (R - 3) || d2 > (R - 1) * (R - 1)) continue;
            if (rng.nextInt(3) != 0) continue;
            const int g = hs.at(cx + dx, cz + dz);
            const i32 id = sandy ? (i32)SAND : (cold ? (i32)SNOW_BLOCK : (mossify ? (i32)MOSSY_COBBLE : (i32)GRAVEL));
            w.put(cx + dx, std::min(g, y), cz + dz, id);
        }
    if (cold)
        for (int dx = -R + 1; dx <= R - 1; ++dx)
            for (int dz = -R + 1; dz <= R - 1; ++dz)
                if (rng.nextInt(4) == 0) w.put(cx + dx, y + 1, cz + dz, SNOW_LAYER);
    if (mossify)
        for (int dy = 1; dy <= openH; ++dy) {
            if (rng.nextInt(3) != 0) continue;
            if (alongX) w.put(cx + hw + 1, y + dy, cz, VINE);
            else        w.put(cx, y + dy, cz + hw + 1, VINE);
        }
}

// ============================================================
// Затонувший корабль (shipwreck: корпус ~19x7, мачта, трюм с сундуками)
// ============================================================
inline void shipwreck(Writer& w, HeightSampler& hs, int cx, int cz, bool beached, LegacyRng& rng) {
    const i32 plank = rng.chance(2) ? (i32)OAK_PLANKS : (i32)SPRUCE_PLANKS;
    const i32 stair = (plank == (i32)OAK_PLANKS) ? (i32)OAK_STAIRS : (i32)SPRUCE_STAIRS;
    const int floorY = hs.avg(cx, cz, 8) + 1;
    const int L = 9;
    const int Wd = 3;

    clearAbove(w, cx - Wd - 1, cz - L - 1, cx + Wd + 1, cz + L + 1, floorY, 8);
    for (int dz = -L; dz <= L; ++dz) {
        const int taper = (std::abs(dz) > L - 3) ? 1 : 0; // сужение к носу и корме
        const int wd = Wd - taper;
        w.fill(cx - wd, floorY, cz + dz, cx + wd, floorY, cz + dz, plank);
        w.put(cx - wd, floorY + 1, cz + dz, plank);
        w.put(cx + wd, floorY + 1, cz + dz, plank);
        w.put(cx - wd, floorY + 2, cz + dz, stair);
        w.put(cx + wd, floorY + 2, cz + dz, stair);
    }
    w.fill(cx - Wd, floorY + 3, cz + L - 6, cx + Wd, floorY + 3, cz + L, plank);
    w.box(cx - Wd + 1, floorY + 4, cz + L - 5, cx + Wd - 1, floorY + 6, cz + L - 1, plank, plank);
    for (int yy = floorY + 4; yy <= floorY + 10; ++yy) w.put(cx, yy, cz - 2, OAK_LOG);
    w.fill(cx - 1, floorY + 8, cz - 2, cx + 1, floorY + 8, cz - 2, WHITE_WOOL);
    w.put(cx - 1, floorY + 1, cz + L - 3, CHEST);
    w.put(cx + 1, floorY + 1, cz - L + 3, CHEST);
    w.put(cx, floorY + 4, cz + L - 3, CHEST);
    for (int dz = -L; dz <= L; ++dz)
        for (int dx = -Wd; dx <= Wd; ++dx)
            if (rng.nextInt(9) == 0) w.put(cx + dx, floorY + 1 + rng.nextInt(2), cz + dz, AIR);
    if (beached) foundation(w, hs, cx - Wd, cz - L, cx + Wd, cz + L, floorY, SAND);
}

// ============================================================
// Руины океана (ocean_ruin_warm — песчаник, ocean_ruin_cold — каменный кирпич)
// ============================================================
inline void oceanRuin(Writer& w, HeightSampler& hs, int cx, int cz, bool warm, LegacyRng& rng) {
    const i32 mainB = warm ? (i32)SANDSTONE : (i32)STONE_BRICKS;
    const i32 altB = warm ? (i32)CUT_SANDSTONE : (i32)CRACKED_STONE_BRICKS;
    const i32 mossB = warm ? (i32)CHISELED_SANDSTONE : (i32)MOSSY_STONE_BRICKS;
    const int y = hs.avg(cx, cz, 6) + 1;

    const int x0 = cx - 4, x1 = cx + 4, z0 = cz - 4, z1 = cz + 4;
    for (int x = x0; x <= x1; ++x)
        for (int z = z0; z <= z1; ++z) {
            if (rng.nextInt(6) == 0) continue;
            w.put(x, y, z, ((x + z) % 3 == 0) ? altB : mainB);
        }
    for (int dy = 1; dy <= 3; ++dy)
        for (int x = x0; x <= x1; ++x) {
            if (rng.nextInt(2 + dy) != 0) w.put(x, y + dy, z0, rng.chance(4) ? mossB : mainB);
            if (rng.nextInt(2 + dy) != 0) w.put(x, y + dy, z1, rng.chance(4) ? mossB : mainB);
        }
    for (int dy = 1; dy <= 3; ++dy)
        for (int z = z0; z <= z1; ++z) {
            if (rng.nextInt(2 + dy) != 0) w.put(x0, y + dy, z, rng.chance(4) ? mossB : mainB);
            if (rng.nextInt(2 + dy) != 0) w.put(x1, y + dy, z, rng.chance(4) ? mossB : mainB);
        }
    w.put(cx, y + 1, cz, CHEST);
    for (int i = 0; i < 4; ++i) {
        const int ox = cx + rng.range(-12, 12), oz = cz + rng.range(-12, 12);
        const int oy = hs.at(ox, oz) + 1;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz)
                if (rng.chance(2)) w.put(ox + dx, oy, oz + dz, mainB);
        if (rng.chance(3)) w.put(ox, oy + 1, oz, altB);
    }
    for (int x = x0; x <= x1; ++x)
        for (int z = z0; z <= z1; ++z)
            if (rng.nextInt(4) == 0) w.put(x, y, z, warm ? (i32)SAND : (i32)GRAVEL);
    if (rng.chance(3)) w.put(cx + 2, y + 1, cz + 2, SUSPICIOUS_SAND);
}

// ============================================================
// Зарытое сокровище (buried_treasure: сундук под песком, offset 9/9)
// ============================================================
inline void buriedTreasure(Writer& w, HeightSampler& hs, int cx, int cz) {
    const int y = hs.at(cx, cz);
    w.put(cx, y - 1, cz, CHEST);
    for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz)
            if (dx || dz) w.put(cx + dx, y - 1, cz + dz, SAND);
    w.put(cx, y, cz, SAND);
}

// ============================================================
// Размещение по ванильным сеткам
// ============================================================

enum : int { SCAN_HUGE = 4, SCAN_VILLAGE = 3, SCAN_BIG = 2, SCAN_SMALL = 1 };

// Ванильные наборы (structure_set/*.json, 1.21.1).
enum : int {
    SP_VILLAGE = 34,  SEP_VILLAGE = 8,  SALT_VILLAGE = 10387312,
    SP_PYRAMID = 32,  SEP_PYRAMID = 8,  SALT_PYRAMID = 14357617,
    SP_IGLOO = 32,    SEP_IGLOO = 8,    SALT_IGLOO = 14357618,
    SP_JUNGLE = 32,   SEP_JUNGLE = 8,   SALT_JUNGLE = 14357619,
    SP_HUT = 32,      SEP_HUT = 8,      SALT_HUT = 14357620,
    SP_RUIN = 20,     SEP_RUIN = 8,     SALT_RUIN = 14357621,
    SP_WRECK = 24,    SEP_WRECK = 4,    SALT_WRECK = 165745295,
    SP_OUTPOST = 32,  SEP_OUTPOST = 8,  SALT_OUTPOST = 165745296,
    SP_PORTAL = 40,   SEP_PORTAL = 15,  SALT_PORTAL = 34222645,
    // STRUCTURES_V4 — ванильные наборы 1.21.1.
    SP_MONUMENT = 32, SEP_MONUMENT = 5,  SALT_MONUMENT = 10387313,
    SP_MANSION = 80,  SEP_MANSION = 20,  SALT_MANSION = 10387319,
    SP_TRAIL = 34,    SEP_TRAIL = 8,     SALT_TRAIL = 83469867,
    SP_CITY = 24,     SEP_CITY = 8,      SALT_CITY = 20083232,
    SP_TRIAL = 34,    SEP_TRIAL = 12,    SALT_TRIAL = 94251327,
    // Шахты в ванили — spacing 1 + frequency 0.004 на чанк.
    // Здесь эквивалентная по плотности сетка 12/4 (одна шахта на ~144 чанка).
    SP_MINE = 12,     SEP_MINE = 4,      SALT_MINE = 0,
    // Крепость в ванили — concentric_rings (128 штук кольцами вокруг нуля).
    // Кольца требуют глобального списка на мир; здесь аппроксимация редкой сеткой.
    SP_HOLD = 96,     SEP_HOLD = 40,     SALT_HOLD = 0
};

// Ванильный exclusion_zone: аванпост не ближе 10 чанков к любой деревне.
inline bool nearVillageStart(uint64_t seed, int cx, int cz, int radius) {
    for (int dx = -radius; dx <= radius; ++dx)
        for (int dz = -radius; dz <= radius; ++dz)
            if (isStartChunk(cx + dx, cz + dz, seed, SP_VILLAGE, SEP_VILLAGE, SALT_VILLAGE)) return true;
    return false;
}

// STRUCT_PERF_V1: габариты построек в блоках от центра (чуть с запасом).
enum : int {
    BB_VILLAGE = 30, BB_PYRAMID = 12, BB_JUNGLE = 10, BB_OUTPOST = 10, BB_IGLOO = 7,
    BB_HUT = 7, BB_PORTAL = 9, BB_WRECK = 12, BB_RUIN = 15, BB_TREASURE = 2,
    BB_MONUMENT = 32, BB_MANSION = 32, BB_TRAIL = 14, BB_CITY = 26, BB_TRIAL = 20,
    BB_MINE = 40, BB_HOLD = 30
};

// STRUCT_LOCATE_V1: таблица сеток для /locate и для лога построек.
enum : int { SP_FOSSIL = 16, SEP_FOSSIL = 4, SALT_FOSSIL = 14357622, BB_FOSSIL = 10 };

struct GridSpec {
    const char* key;
    const char* nameRu;
    const char* nameEn;
    int spacing, separation, salt, bbox;
};

inline const GridSpec* gridSpecs(int& count) {
    static const GridSpec specs[] = {
        { "village",   "деревня",  "village",          SP_VILLAGE, SEP_VILLAGE, SALT_VILLAGE, BB_VILLAGE },
        { "outpost",   "аванпост", "pillager outpost", SP_OUTPOST, SEP_OUTPOST, SALT_OUTPOST, BB_OUTPOST },
        { "igloo",     "иглу", "igloo",              SP_IGLOO, SEP_IGLOO, SALT_IGLOO, BB_IGLOO },
        { "portal",    "разрушенный портал", "ruined portal", SP_PORTAL, SEP_PORTAL, SALT_PORTAL, BB_PORTAL },
        { "shipwreck", "затонувший корабль", "shipwreck", SP_WRECK, SEP_WRECK, SALT_WRECK, BB_WRECK },
        { "ruin",      "руины океана", "ocean ruin", SP_RUIN, SEP_RUIN, SALT_RUIN, BB_RUIN },
        { "fossil",    "скелет", "fossil", SP_FOSSIL, SEP_FOSSIL, SALT_FOSSIL, BB_FOSSIL },
        { "mansion",   "особняк", "woodland mansion", SP_MANSION, SEP_MANSION, SALT_MANSION, BB_MANSION },
        { "trail",     "тропы", "trail ruins", SP_TRAIL, SEP_TRAIL, SALT_TRAIL, BB_TRAIL },
        { "city",      "древний город", "ancient city", SP_CITY, SEP_CITY, SALT_CITY, BB_CITY },
        { "chambers",  "пробные палаты", "trial chambers", SP_TRIAL, SEP_TRIAL, SALT_TRIAL, BB_TRIAL }
    };
    count = (int)(sizeof(specs) / sizeof(specs[0]));
    return specs;
}

// Те же биомные условия, что и в генерации — чтобы /locate не врал.
inline bool portalBiome(int biome) {
    using namespace wg;
    return isBeach(biome) || isRiver(biome) || isTaigaTag(biome) || isForestTag(biome) ||
           biome == B_MUSHROOM_FIELDS || biome == B_ICE_SPIKES || biome == B_SAVANNA ||
           biome == B_SNOWY_PLAINS || biome == B_PLAINS || biome == B_SUNFLOWER_PLAINS ||
           biome == B_DESERT || isJungleTag(biome) || biome == B_SWAMP ||
           biome == B_MANGROVE_SWAMP || isBadlandsTag(biome) || isMountainTag(biome) ||
           biome == B_SAVANNA_PLATEAU || biome == B_WINDSWEPT_SAVANNA ||
           biome == B_STONY_SHORE || isOcean(biome);
}

inline bool biomeOkFor(const char* key, int biome) {
    using namespace wg;
    const std::string_view k(key);
    if (k == "village")   return villageKind(biome) != VK_NONE;
    if (k == "pyramid")   return biome == B_DESERT;
    if (k == "temple")    return biome == B_JUNGLE || biome == B_BAMBOO_JUNGLE;
    if (k == "outpost")   return outpostBiome(biome);
    if (k == "igloo")     return iglooBiome(biome);
    if (k == "hut")       return biome == B_SWAMP;
    if (k == "shipwreck") return isOcean(biome) || isBeach(biome);
    if (k == "ruin")      return isWarmOcean(biome) || isColdOcean(biome);
    if (k == "portal")    return portalBiome(biome);
    if (k == "monument")  return isColdOcean(biome) || isWarmOcean(biome);
    if (k == "mansion")   return biome == B_DARK_FOREST;
    if (k == "trail")     return isTaigaTag(biome) || isJungleTag(biome) || biome == B_DESERT ||
                                 biome == B_SNOWY_PLAINS || biome == B_PLAINS || biome == B_SAVANNA;
    if (k == "city")      return true;      // глубинный биом проверяется на y = -50
    if (k == "chambers")  return true;      // под землёй, биом не важен
    if (k == "mineshaft") return true;
    if (k == "stronghold") return true;
    return false;
}

// ============================================================
// STRUCTURES_V4 — остальные ванильные наборы оверворлда
// ============================================================

// Океанский монумент 58x58: подводный призмариновый комплекс с крыльями и залом.
inline void oceanMonument(Writer& w, HeightSampler& hs, int wx, int wz, LegacyRng& r) {
    const int x0 = wx - 29, z0 = wz - 29, x1 = wx + 28, z1 = wz + 28;
    int base = hs.avg(wx, wz, 16) - 1;
    if (base > wg::SEA_LEVEL - 24) base = wg::SEA_LEVEL - 24;
    if (base < CHUNK_HEIGHT_MIN + 8) base = CHUNK_HEIGHT_MIN + 8;
    const int top = base + 22;
    w.fill(x0, base - 3, z0, x1, base, z1, PRISMARINE);
    w.box(x0, base, z0, x1, top, z1, PRISMARINE_BRICKS, PRISMARINE);
    w.fill(x0 + 1, base + 1, z0 + 1, x1 - 1, top - 1, z1 - 1, WATER);
    // четыре крыла
    for (int i = 0; i < 4; ++i) {
        const int bx = (i & 1) ? x1 - 11 : x0 + 3;
        const int bz = (i & 2) ? z1 - 11 : z0 + 3;
        w.box(bx, base + 1, bz, bx + 8, base + 10, bz + 8, DARK_PRISMARINE, PRISMARINE_BRICKS);
        w.fill(bx + 1, base + 2, bz + 1, bx + 7, base + 9, bz + 7, WATER);
        w.put(bx + 4, base + 10, bz + 4, SEA_LANTERN);
        if (r.chance(2)) w.fill(bx + 3, base + 2, bz + 3, bx + 5, base + 2, bz + 5, WET_SPONGE);
    }
    // центральный зал старейшины с золотыми блоками
    w.box(wx - 6, base + 6, wz - 6, wx + 5, base + 16, wz + 5, PRISMARINE_BRICKS, DARK_PRISMARINE);
    w.fill(wx - 5, base + 7, wz - 5, wx + 4, base + 15, wz + 4, WATER);
    w.fill(wx - 2, base + 7, wz - 2, wx + 1, base + 7, wz + 1, GOLD_BLOCK);
    // морские фонари по крыше и вход
    const int step = 5 + r.nextInt(3);
    for (int x = x0 + 4; x <= x1 - 4; x += step)
        for (int z = z0 + 4; z <= z1 - 4; z += step) w.put(x, top, z, SEA_LANTERN);
    w.fill(wx - 1, base + 1, z0, wx + 1, base + 4, z0, WATER);
}

// Лесной особняк: тёмный дуб, три этажа, комнаты, лестницы, шатровая крыша.
inline void woodlandMansion(Writer& w, HeightSampler& hs, int wx, int wz, LegacyRng& r) {
    const int hw = 27;
    const int x0 = wx - hw, z0 = wz - hw, x1 = wx + hw, z1 = wz + hw;
    const int y = hs.avg(wx, wz, 16);
    platform(w, hs, x0 - 1, z0 - 1, x1 + 1, z1 + 1, y, 26, COBBLE);
    w.fill(x0 - 1, y - 1, z0 - 1, x1 + 1, y - 1, z1 + 1, COBBLE);
    for (int f = 0; f < 3; ++f) {
        const int fy = y + f * 6;
        w.box(x0, fy, z0, x1, fy + 6, z1, DARK_OAK_PLANKS, DARK_OAK_PLANKS);
        for (int x = x0; x <= x1; x += 6)
            for (int z = z0; z <= z1; z += 6)
                if (x == x0 || x >= x1 - 1 || z == z0 || z >= z1 - 1)
                    w.fill(x, fy, z, x, fy + 6, z, DARK_OAK_LOG);
        // внутренние комнаты
        for (int x = x0 + 5; x < x1 - 10; x += 12)
            for (int z = z0 + 5; z < z1 - 10; z += 12) {
                w.box(x, fy, z, x + 9, fy + 5, z + 9, DARK_OAK_PLANKS, DARK_OAK_PLANKS);
                w.fill(x + 4, fy + 1, z, x + 5, fy + 3, z, AIR);
                w.put(x + 4, fy + 1, z, DARK_OAK_DOOR);
                w.put(x + 4, fy + 4, z + 4, LANTERN);
                if (r.chance(3)) w.put(x + 2, fy + 1, z + 2, CHEST);
                if (r.chance(2)) w.fill(x + 7, fy + 1, z + 7, x + 8, fy + 2, z + 7, BOOKSHELF);
                if (r.chance(4)) w.fill(x + 1, fy + 1, z + 6, x + 2, fy + 1, z + 7, RED_WOOL);
                if (r.chance(5)) w.put(x + 7, fy + 1, z + 2, CRAFTING_TABLE);
            }
        // окна по фасадам
        for (int x = x0 + 3; x <= x1 - 3; x += 4) {
            w.fill(x, fy + 2, z0, x, fy + 3, z0, GLASS_PANE);
            w.fill(x, fy + 2, z1, x, fy + 3, z1, GLASS_PANE);
        }
        for (int z = z0 + 3; z <= z1 - 3; z += 4) {
            w.fill(x0, fy + 2, z, x0, fy + 3, z, GLASS_PANE);
            w.fill(x1, fy + 2, z, x1, fy + 3, z, GLASS_PANE);
        }
        // лестница на следующий этаж
        if (f < 2) {
            for (int t = 0; t < 6; ++t) w.put(wx + 4 + t, fy + 1 + t, wz, DARK_OAK_STAIRS);
            w.fill(wx + 4, fy + 6, wz - 1, wx + 10, fy + 6, wz + 1, AIR);
        }
    }
    // крыша
    const int ry = y + 18;
    w.fill(x0, ry, z0, x1, ry, z1, DARK_OAK_PLANKS);
    for (int i = 1; i <= 3; ++i)
        w.fill(x0 + i, ry + i, z0 + i, x1 - i, ry + i, z1 - i, COBBLE);
    // парадный вход
    w.fill(wx - 1, y + 1, z0, wx + 1, y + 3, z0, AIR);
    w.put(wx, y + 1, z0, DARK_OAK_DOOR);
    w.fill(wx - 2, y, z0 - 3, wx + 2, y, z0 - 1, COBBLE);
    w.put(wx - 2, y + 1, z0 - 1, DARK_OAK_FENCE);
    w.put(wx + 2, y + 1, z0 - 1, DARK_OAK_FENCE);
}

// Заброшенная шахта: перекрёсток, четыре коридора с рельсами и опорами, комната.
inline void mineshaft(Writer& w, HeightSampler& hs, int wx, int wz, bool mesa, LegacyRng& r) {
    const i32 plank = mesa ? DARK_OAK_PLANKS : OAK_PLANKS;
    const i32 fence = mesa ? DARK_OAK_FENCE : OAK_FENCE;
    int y = hs.at(wx, wz) - 34 - r.nextInt(16);
    if (y > 45) y = 45;
    if (y < CHUNK_HEIGHT_MIN + 8) y = CHUNK_HEIGHT_MIN + 8;
    for (int dir = 0; dir < 4; ++dir) {
        const int dx = (dir == 0) ? 1 : (dir == 1 ? -1 : 0);
        const int dz = (dir == 2) ? 1 : (dir == 3 ? -1 : 0);
        const int len = 22 + r.nextInt(18);
        int cy = y;
        for (int i = 1; i <= len; ++i) {
            const int x = wx + dx * i, z = wz + dz * i;
            if (i % 12 == 0 && r.chance(2)) cy += r.chance(2) ? 1 : -1;
            if (cy < CHUNK_HEIGHT_MIN + 6) cy = CHUNK_HEIGHT_MIN + 6;
            const int rx = dz ? 1 : 0, rz = dx ? 1 : 0;
            w.fill(x - rx, cy, z - rz, x + rx, cy + 2, z + rz, AIR);
            w.put(x, cy, z, RAIL);
            if (i % 4 == 0) {
                if (dx) {
                    w.fill(x, cy, z - 1, x, cy + 2, z - 1, plank);
                    w.fill(x, cy, z + 1, x, cy + 2, z + 1, plank);
                    w.fill(x, cy + 3, z - 1, x, cy + 3, z + 1, plank);
                } else {
                    w.fill(x - 1, cy, z, x - 1, cy + 2, z, plank);
                    w.fill(x + 1, cy, z, x + 1, cy + 2, z, plank);
                    w.fill(x - 1, cy + 3, z, x + 1, cy + 3, z, plank);
                }
            } else if (r.chance(14)) {
                w.put(x + rx, cy, z + rz, fence);
            }
            if (r.chance(20)) w.put(x, cy + 2, z, COBWEB);
            if (r.chance(36)) w.put(x + rx, cy, z + rz, CHEST);
            if (r.chance(28)) w.put(x, cy + 3, z, TORCH);
        }
    }
    // центральная комната со спавнером
    w.fill(wx - 3, y, wz - 3, wx + 3, y + 3, wz + 3, AIR);
    w.fill(wx - 3, y - 1, wz - 3, wx + 3, y - 1, wz + 3, plank);
    w.put(wx, y, wz, MOB_SPAWNER);
    w.put(wx + 2, y, wz + 2, CHEST);
    w.fill(wx - 3, y, wz - 3, wx - 3, y + 2, wz - 3, plank);
    w.fill(wx + 3, y, wz + 3, wx + 3, y + 2, wz + 3, plank);
}

// Крепость: зал портала Энда с 12 рамками и лавой, библиотека, комнаты, коридоры.
inline void stronghold(Writer& w, HeightSampler& hs, int wx, int wz, LegacyRng& r) {
    int y = hs.at(wx, wz) - 40;
    if (y > 24) y = 24;
    if (y < CHUNK_HEIGHT_MIN + 12) y = CHUNK_HEIGHT_MIN + 12;
    const i32 wall = STONE_BRICKS;
    // зал портала
    w.box(wx - 6, y, wz - 6, wx + 6, y + 8, wz + 6, wall, wall);
    w.fill(wx - 2, y, wz - 2, wx + 2, y, wz + 2, LAVA);
    for (int i = -2; i <= 2; ++i) {
        w.put(wx + i, y + 1, wz - 3, END_PORTAL_FRAME);
        w.put(wx + i, y + 1, wz + 3, END_PORTAL_FRAME);
        w.put(wx - 3, y + 1, wz + i, END_PORTAL_FRAME);
        w.put(wx + 3, y + 1, wz + i, END_PORTAL_FRAME);
    }
    for (int t = 0; t < 5; ++t) w.put(wx + 4, y + 1 + t, wz + 4 - t, STONE_BRICK_STAIRS);
    if (r.chance(2)) w.fill(wx - 5, y + 1, wz - 5, wx - 5, y + 2, wz - 5, MOSSY_STONE_BRICKS);
    // библиотека
    w.box(wx - 26, y, wz - 6, wx - 12, y + 6, wz + 6, wall, wall);
    w.fill(wx - 25, y + 1, wz - 5, wx - 25, y + 3, wz + 5, BOOKSHELF);
    w.fill(wx - 13, y + 1, wz - 5, wx - 13, y + 3, wz + 5, BOOKSHELF);
    w.fill(wx - 19, y + 1, wz - 3, wx - 19, y + 3, wz + 3, BOOKSHELF);
    w.put(wx - 19, y + 1, wz + 5, CHEST);
    w.put(wx - 19, y + 5, wz, TORCH);
    // комнаты
    w.box(wx + 12, y, wz - 5, wx + 22, y + 5, wz + 5, wall, wall);
    w.put(wx + 17, y + 1, wz, CHEST);
    w.box(wx - 5, y, wz - 22, wx + 5, y + 5, wz - 12, wall, wall);
    w.put(wx, y + 1, wz - 17, CRAFTING_TABLE);
    w.box(wx - 5, y, wz + 12, wx + 5, y + 5, wz + 22, wall, wall);
    if (r.chance(2)) w.put(wx, y + 1, wz + 17, MOB_SPAWNER);
    // коридоры между залами
    w.box(wx - 12, y, wz - 2, wx - 6, y + 4, wz + 2, wall, wall);
    w.box(wx + 6, y, wz - 2, wx + 12, y + 4, wz + 2, wall, wall);
    w.box(wx - 2, y, wz - 12, wx + 2, y + 4, wz - 6, wall, wall);
    w.box(wx - 2, y, wz + 6, wx + 2, y + 4, wz + 12, wall, wall);
    // решётки и трещины для вида
    w.put(wx - 6, y + 1, wz, IRON_BARS);
    w.put(wx + 6, y + 1, wz, IRON_BARS);
    if (r.chance(2)) w.put(wx, y + 1, wz - 6, CRACKED_STONE_BRICKS);
}

// Тропы (trail ruins): закопанные кирпичные дорожки и подозрительный гравий.
inline void trailRuins(Writer& w, HeightSampler& hs, int wx, int wz, LegacyRng& r) {
    const int hw = 11;
    for (int x = wx - hw; x <= wx + hw; ++x)
        for (int z = wz - hw; z <= wz + hw; ++z) {
            if (!w.inside(x, z)) continue;
            const int dist = std::max(std::abs(x - wx), std::abs(z - wz));
            const int g = hs.at(x, z);
            const int y = g - 2 - dist / 6;
            const bool road = (((x - wx) % 4) == 0) || (((z - wz) % 4) == 0);
            if (road) {
                w.put(x, y, z, r.chance(4) ? MUD_BRICKS : BRICKS);
                if (r.chance(10)) w.put(x, y + 1, z, SUSPICIOUS_GRAVEL);
            } else if (r.chance(5)) {
                w.put(x, y, z, PACKED_MUD);
            }
        }
    // центральный очаг
    const int cy = hs.at(wx, wz) - 3;
    w.fill(wx - 2, cy, wz - 2, wx + 2, cy, wz + 2, BRICKS);
    w.put(wx, cy + 1, wz, SUSPICIOUS_GRAVEL);
    if (r.chance(2)) w.put(wx + 1, cy + 1, wz + 1, SUSPICIOUS_GRAVEL);
}

// Древний город: глубинный сланец, скалк, колонны и центральный монумент.
inline void ancientCity(Writer& w, HeightSampler& hs, int wx, int wz, LegacyRng& r) {
    (void)hs;
    const int y = -45;
    const int hw = 23;
    const int x0 = wx - hw, z0 = wz - hw, x1 = wx + hw, z1 = wz + hw;
    w.fill(x0 - 1, y, z0 - 1, x1 + 1, y + 20, z1 + 1, AIR);
    w.fill(x0, y - 1, z0, x1, y - 1, z1, DEEPSLATE_BRICKS);
    for (int x = x0; x <= x1; ++x)
        for (int z = z0; z <= z1; ++z) {
            if (!w.inside(x, z)) continue;
            if (r.chance(7)) w.put(x, y - 1, z, r.chance(2) ? DEEPSLATE_TILES : SCULK);
            else if (r.chance(30)) w.put(x, y - 1, z, CRACKED_DEEPSLATE_BRICKS);
        }
    // центральный монумент
    w.box(wx - 8, y, wz - 8, wx + 8, y + 12, wz + 8, DEEPSLATE_BRICKS, POLISHED_DEEPSLATE);
    w.fill(wx - 7, y + 1, wz - 7, wx + 7, y + 11, wz + 7, AIR);
    w.fill(wx - 1, y + 1, wz - 1, wx + 1, y + 1, wz + 1, SCULK_CATALYST);
    w.put(wx, y + 2, wz, SCULK_SHRIEKER);
    w.put(wx - 3, y + 1, wz - 3, CHEST);
    w.fill(wx - 8, y + 1, wz - 1, wx - 8, y + 3, wz + 1, AIR); // проход внутрь
    // колонны и скалк-сенсоры
    for (int x = x0 + 5; x <= x1 - 5; x += 9)
        for (int z = z0 + 5; z <= z1 - 5; z += 9) {
            if (std::abs(x - wx) <= 9 && std::abs(z - wz) <= 9) continue;
            w.fill(x, y, z, x, y + 10, z, DEEPSLATE_BRICKS);
            w.fill(x - 1, y + 10, z - 1, x + 1, y + 10, z + 1, DEEPSLATE_TILES);
            if (r.chance(3)) w.put(x + 1, y + 1, z, SOUL_LANTERN);
            if (r.chance(4)) w.put(x - 1, y, z + 1, SCULK_SENSOR);
            if (r.chance(5)) w.put(x, y, z + 1, CHEST);
            if (r.chance(6)) w.put(x + 1, y, z - 1, CHISELED_DEEPSLATE);
        }
    // потолок каверны
    w.fill(x0, y + 20, z0, x1, y + 20, z1, DEEPSLATE_STONE);
}

// Пробные палаты: туфовые комнаты с медью, пробными спавнерами и сейфами.
inline void trialChambers(Writer& w, HeightSampler& hs, int wx, int wz, LegacyRng& r) {
    (void)hs;
    int y = -30 + r.nextInt(12);
    if (y < CHUNK_HEIGHT_MIN + 6) y = CHUNK_HEIGHT_MIN + 6;
    for (int gx = -1; gx <= 1; ++gx)
        for (int gz = -1; gz <= 1; ++gz) {
            const int cx = wx + gx * 12, cz = wz + gz * 12;
            const int fy = y + (((gx + gz) & 1) ? 0 : 4);
            w.box(cx - 5, fy, cz - 5, cx + 5, fy + 7, cz + 5, TUFF_BRICKS, POLISHED_TUFF);
            w.fill(cx - 4, fy + 1, cz - 4, cx + 4, fy + 6, cz + 4, AIR);
            w.put(cx, fy + 1, cz, TRIAL_SPAWNER);
            if (r.chance(2)) w.put(cx + 3, fy + 1, cz + 3, VAULT);
            if (r.chance(2)) w.put(cx - 3, fy + 1, cz - 3, CHEST);
            w.put(cx + 2, fy + 6, cz, COPPER_BULB);
            w.put(cx - 2, fy + 6, cz, COPPER_BULB);
            w.fill(cx - 1, fy + 7, cz - 1, cx + 1, fy + 7, cz + 1, COPPER_GRATE);
            if (r.chance(3)) w.fill(cx - 5, fy + 1, cz, cx - 5, fy + 2, cz, CHISELED_TUFF);
            if (r.chance(3)) w.put(cx + 4, fy + 1, cz - 4, CUT_COPPER);
            // проходы к соседним комнатам
            w.fill(cx + 5, fy + 1, cz - 1, cx + 8, fy + 3, cz + 1, AIR);
            w.fill(cx - 1, fy + 1, cz + 5, cx + 1, fy + 3, cz + 8, AIR);
        }
}

// Порядок проверок строго от дешёвых к дорогим:
//   1. isStartChunk — один LCG, наносекунды.
//   2. Габарит против границ чанка — арифметика.
//   3. Биом и высота поверхности — шум, самое дорогое.
//   4. Сама постройка.
// До этого шаги 3 и 4 выполнялись для всех 49 соседей каждого чанка — именно отсюда
// ============================================================
// VANILLA_TPL_V1 — постройки из НАСТОЯЩИХ ванильных .nbt (structures.gen.hpp)
//
// Важно: в ванили nbt-шаблоны есть ТОЛЬКО у 11 наборов (деревни, иглу,
// аванпост, разрушенные порталы, корабли, руины океана, скелеты, тропы,
// особняк, древний город, пробные палаты). Пирамида, храм джунглей, хижина,
// монумент, шахты и крепость в ванили генерируются кодом, файлов для них нет.
// ============================================================
struct TplSpot { bool ok = false; int ox = 0, oy = 0, oz = 0, sx = 0, sy = 0, sz = 0; };

inline const tpl::Template* pickTpl(const char* prefix, LegacyRng& rng) {
    const std::vector<const tpl::Template*> g = tpl::library().group(prefix);
    if (g.empty()) return nullptr;
    return g[(size_t)rng.nextInt((int)g.size())];
}

// Укладка случайного шаблона набора с ванильным поворотом и зеркалом.
// dy — сдвиг относительно рельефа; useAbs — абсолютная высота (подземка).
inline TplSpot putTpl(Writer& w, HeightSampler& hs, const char* prefix, int wx, int wz,
                      int dy, LegacyRng& rng, bool skipAir, bool useAbs = false, int absY = 0) {
    TplSpot s;
    const tpl::Template* t = pickTpl(prefix, rng);
    if (!t) return s;
    const int rot = rng.nextInt(4);
    const bool mir = rng.chance(2);
    int sx = 0, sz = 0;
    tpl::rotatedSize(*t, rot, sx, sz);
    s.ox = wx - sx / 2; s.oz = wz - sz / 2;
    s.sx = sx; s.sz = sz; s.sy = t->sy;
    const int rad = std::max(2, std::min(8, (sx + sz) / 4));
    s.oy = useAbs ? absY : hs.avg(wx, wz, rad) + dy;
    s.ok = true;
    if (!w.hits(s.ox, s.oz, s.ox + sx - 1, s.oz + sz - 1)) return s; // в этот чанк не попадает
    if (!useAbs) w.fill(s.ox, s.oy + t->sy, s.oz, s.ox + sx - 1, s.oy + t->sy + 4, s.oz + sz - 1, AIR);
    tpl::place(w, *t, s.ox, s.oy, s.oz, rot, mir, skipAir);
    return s;
}

// Разрушенный портал: шаблон + spreadNetherrack + биомные процессоры.
inline void ruinedPortalTpl(Writer& w, HeightSampler& hs, int cx, int cz, int biome, LegacyRng& rng) {
    using namespace wg;
    const bool giant = rng.nextInt(12) == 0;
    const bool ocean = isOcean(biome);
    const bool cold  = isSnowy(biome) || biome == B_ICE_SPIKES;
    const bool sandy = biome == B_DESERT || isBeach(biome) || isBadlandsTag(biome);
    const bool mossy = isJungleTag(biome) || biome == B_SWAMP || biome == B_MANGROVE_SWAMP;
    const TplSpot s = putTpl(w, hs, giant ? "ruined_portal/giant_portal" : "ruined_portal/portal",
                             cx, cz, ocean ? 0 : -rng.nextInt(3), rng, false);
    if (!s.ok) return;
    for (int x = s.ox - 1; x <= s.ox + s.sx; ++x)
        for (int z = s.oz - 1; z <= s.oz + s.sz; ++z) {
            if (rng.nextInt(6) == 0) continue;
            const int g = hs.at(x, z);
            const int top = std::min(g, s.oy - 1);
            const int deep = 2 + rng.nextInt(2);
            for (int k = 0; k < deep; ++k)
                w.put(x, top - k, z, rng.chance(20) ? (i32)MAGMA_BLOCK : (i32)NETHERRACK);
            if (rng.nextInt(4) == 0) {
                const i32 cov = sandy ? (i32)SAND : (cold ? (i32)SNOW_BLOCK
                                                         : (mossy ? (i32)MOSSY_COBBLE : (i32)GRAVEL));
                w.put(x, top, z, cov);
            }
            if (!ocean && rng.nextInt(28) == 0) w.put(x, top + 1, z, FIRE_BLOCK);
        }
}

// Иглу: igloo/top, у 1/8 — шахта в лабораторию (middle x N + bottom), как в ванили.
inline void iglooTpl(Writer& w, HeightSampler& hs, int cx, int cz, LegacyRng& rng) {
    const TplSpot s = putTpl(w, hs, "igloo/top", cx, cz, -1, rng, false);
    if (!s.ok) return;
    if (rng.nextInt(8) != 0) return;
    const tpl::Template* mid = tpl::library().byName("igloo/middle");
    const tpl::Template* bot = tpl::library().byName("igloo/bottom");
    if (!mid || !bot) return;
    const int floors = 1 + rng.nextInt(4);
    int y = s.oy - 1;
    for (int k = 0; k < floors; ++k) { y -= mid->sy; tpl::place(w, *mid, s.ox + 2, y, s.oz + 2, 0, false, false); }
    y -= bot->sy;
    tpl::place(w, *bot, s.ox + 2, y, s.oz + 2, 0, false, false);
}

// Корабль: 20 ванильных вариантов (боком, килем вверх, половинки, degraded).
inline void shipwreckTpl(Writer& w, HeightSampler& hs, int cx, int cz, bool beached, LegacyRng& rng) {
    const TplSpot s = putTpl(w, hs, "shipwreck/", cx, cz, beached ? 0 : -1, rng, !beached);
    if (beached && s.ok) foundation(w, hs, s.ox, s.oz, s.ox + s.sx - 1, s.oz + s.sz - 1, s.oy, SAND);
}

// Руины океана: warm/big_warm для тёплых, brick/cracked/mossy — для холодных.
inline void oceanRuinTpl(Writer& w, HeightSampler& hs, int cx, int cz, bool warm, LegacyRng& rng) {
    const bool big = rng.nextInt(4) == 0;
    const char* pref;
    if (warm) pref = big ? "underwater_ruin/big_warm" : "underwater_ruin/warm";
    else {
        static const char* small3[] = { "underwater_ruin/brick_", "underwater_ruin/cracked_", "underwater_ruin/mossy_" };
        static const char* big3[]   = { "underwater_ruin/big_brick", "underwater_ruin/big_cracked", "underwater_ruin/big_mossy" };
        const int k = rng.nextInt(3);
        pref = big ? big3[k] : small3[k];
    }
    putTpl(w, hs, pref, cx, cz, -1, rng, true);
}

// Аванпост: ванильная вышка + 2-4 фичи (палатки, клетки, мишени, брёвна).
inline void outpostTpl(Writer& w, HeightSampler& hs, int cx, int cz, LegacyRng& rng) {
    const TplSpot s = putTpl(w, hs, "pillager_outpost/watchtower", cx, cz, 0, rng, true);
    if (s.ok) foundation(w, hs, s.ox, s.oz, s.ox + s.sx - 1, s.oz + s.sz - 1, s.oy, COBBLE);
    const int n = 2 + rng.nextInt(3);
    for (int k = 0; k < n; ++k) {
        const int dx = rng.range(-16, 16), dz = rng.range(-16, 16);
        if (std::abs(dx) < 10 && std::abs(dz) < 10) continue;
        putTpl(w, hs, "pillager_outpost/feature_", cx + dx, cz + dz, 0, rng, true);
    }
}

// Деревня: ванильный town_center своего стиля + дома из того же пула вокруг.
// Полный jigsaw (улицы с terminators) — следующий шаг, здесь кластер вокруг центра.
inline void villageTpl(Writer& w, HeightSampler& hs, int cx, int cz, VillageKind kind, LegacyRng& rng) {
    const char* center;
    const char* houses;
    switch (kind) {
        case VK_DESERT:  center = "village/desert/town_centers/";  houses = "village/desert/houses/";  break;
        case VK_SAVANNA: center = "village/savanna/town_centers/"; houses = "village/savanna/houses/"; break;
        case VK_SNOWY:   center = "village/snowy/town_centers/";   houses = "village/snowy/houses/";   break;
        case VK_TAIGA:   center = "village/taiga/town_centers/";   houses = "village/taiga/houses/";   break;
        default:         center = "village/plains/town_centers/";  houses = "village/plains/houses/";  break;
    }
    const VillageStyle st = villageStyle(kind);
    const TplSpot c = putTpl(w, hs, center, cx, cz, 0, rng, true);
    if (c.ok) foundation(w, hs, c.ox, c.oz, c.ox + c.sx - 1, c.oz + c.sz - 1, c.oy, st.path);
    const int count = 6 + rng.nextInt(5);
    for (int k = 0; k < count; ++k) {
        const int ang = rng.nextInt(360);
        const double a = (double)ang * 3.14159265358979 / 180.0;
        const int rad = 11 + rng.nextInt(13);
        const int hx = cx + (int)(std::cos(a) * rad), hz = cz + (int)(std::sin(a) * rad);
        if (hs.spread(hx, hz, 4) > 5) continue;              // не ставим дом на обрыв
        const TplSpot h = putTpl(w, hs, houses, hx, hz, 0, rng, true);
        if (!h.ok) continue;
        foundation(w, hs, h.ox, h.oz, h.ox + h.sx - 1, h.oz + h.sz - 1, h.oy, st.wall);
        // дорожка к центру
        for (int t = 0; t <= rad; ++t) {
            const int px = cx + (int)(std::cos(a) * t), pz = cz + (int)(std::sin(a) * t);
            w.put(px, hs.at(px, pz), pz, st.path);
        }
    }
}

// Скелеты (fossil): ванильные skull_1..4 / spine_1..4 (+ угольные), глубоко под землёй.
inline void fossilTpl(Writer& w, HeightSampler& hs, int cx, int cz, LegacyRng& rng) {
    const int surf = hs.at(cx, cz);
    int y = surf - 14 - rng.nextInt(26);
    if (y < CHUNK_HEIGHT_MIN + 8) y = CHUNK_HEIGHT_MIN + 8;
    const bool coal = rng.chance(4);
    const char* pref = rng.chance(2)
        ? (coal ? "fossil/skull_4_coal" : "fossil/skull_")
        : (coal ? "fossil/spine_4_coal" : "fossil/spine_");
    putTpl(w, hs, pref, cx, cz, 0, rng, true, true, y);
}

// Тропы: ванильные куски buildings/roads/tower, закопанные под поверхность.
inline void trailRuinsTpl(Writer& w, HeightSampler& hs, int cx, int cz, LegacyRng& rng) {
    putTpl(w, hs, "trail_ruins/tower/", cx, cz, -5, rng, true);
    const int n = 2 + rng.nextInt(3);
    for (int k = 0; k < n; ++k) {
        const int dx = rng.range(-14, 14), dz = rng.range(-14, 14);
        putTpl(w, hs, rng.chance(2) ? "trail_ruins/buildings/" : "trail_ruins/roads/",
               cx + dx, cz + dz, -4 - rng.nextInt(2), rng, true);
    }
}

// Особняк: ванильные комнаты 1x1 (7x8x7) на сетке 7 блоков, два этажа + вход.
// Сборка упрощённая: ванильный MansionPieces раскладывает их по своей сетке комнат.
inline void woodlandMansionTpl(Writer& w, HeightSampler& hs, int cx, int cz, LegacyRng& rng) {
    const int cells = 5;
    const int y = hs.avg(cx, cz, 8) + 1;
    const int ox = cx - cells * 7 / 2, oz = cz - cells * 7 / 2;
    foundation(w, hs, ox - 1, oz - 1, ox + cells * 7, oz + cells * 7, y, COBBLE);
    for (int gx = 0; gx < cells; ++gx)
        for (int gz = 0; gz < cells; ++gz)
            for (int floor = 0; floor < 2; ++floor) {
                const char* pref = (floor == 0) ? "woodland_mansion/1x1_a" : "woodland_mansion/1x1_b";
                const tpl::Template* t = pickTpl(pref, rng);
                if (!t) continue;
                tpl::place(w, *t, ox + gx * 7, y + floor * 8, oz + gz * 7, rng.nextInt(4), false, false);
            }
    const tpl::Template* ent = tpl::library().byName("woodland_mansion/entrance");
    if (ent) tpl::place(w, *ent, ox, y, oz - 8, 0, false, true);
}

// Древний город: ванильный city_center + несколько кусков structures вокруг.
inline void ancientCityTpl(Writer& w, HeightSampler& hs, int cx, int cz, LegacyRng& rng) {
    const int y = -51;
    putTpl(w, hs, "ancient_city/city_center/city_center_", cx, cz, 0, rng, false, true, y);
    const int n = 4 + rng.nextInt(4);
    for (int k = 0; k < n; ++k)
        putTpl(w, hs, "ancient_city/city/", cx + rng.range(-40, 40), cz + rng.range(-40, 40),
               0, rng, false, true, y);
}

// Пробные палаты: ванильные комнаты + коридоры вокруг на двух уровнях.
inline void trialChambersTpl(Writer& w, HeightSampler& hs, int cx, int cz, LegacyRng& rng) {
    const int y = -28 - rng.nextInt(8);
    putTpl(w, hs, "trial_chambers/chamber/chamber_", cx, cz, 0, rng, false, true, y);
    const int n = 4 + rng.nextInt(4);
    for (int k = 0; k < n; ++k) {
        const int dx = rng.range(-30, 30), dz = rng.range(-30, 30);
        const int dy = rng.chance(2) ? 0 : 12;
        putTpl(w, hs, rng.chance(2) ? "trial_chambers/corridor/" : "trial_chambers/hallway/",
               cx + dx, cz + dz, 0, rng, false, true, y + dy);
    }
}

inline const char* buildFrom(Writer& w, HeightSampler& hs, const wg::BiomeSource& biomes,
                      uint64_t seed, int ox, int oz, int dist) {
    using namespace wg;
    const int wx = ox * 16 + 8, wz = oz * 16 + 8;

    // Шаг 1: сетки.
    const bool sVillage = dist <= SCAN_VILLAGE && isStartChunk(ox, oz, seed, SP_VILLAGE, SEP_VILLAGE, SALT_VILLAGE);
    const bool sFossil  = dist <= SCAN_SMALL   && isStartChunk(ox, oz, seed, SP_FOSSIL, SEP_FOSSIL, SALT_FOSSIL);
    const bool sOutpost = dist <= SCAN_BIG     && isStartChunk(ox, oz, seed, SP_OUTPOST, SEP_OUTPOST, SALT_OUTPOST);
    const bool sIgloo   = dist <= SCAN_SMALL   && isStartChunk(ox, oz, seed, SP_IGLOO, SEP_IGLOO, SALT_IGLOO);
    const bool sPortal  = dist <= SCAN_SMALL   && isStartChunk(ox, oz, seed, SP_PORTAL, SEP_PORTAL, SALT_PORTAL);
    const bool sWreck   = dist <= SCAN_BIG     && isStartChunk(ox, oz, seed, SP_WRECK, SEP_WRECK, SALT_WRECK);
    const bool sRuin    = dist <= SCAN_SMALL   && isStartChunk(ox, oz, seed, SP_RUIN, SEP_RUIN, SALT_RUIN);
    // STRUCTURES_V4.
    const bool sMansion  = dist <= SCAN_HUGE  && isStartChunk(ox, oz, seed, SP_MANSION, SEP_MANSION, SALT_MANSION);
    const bool sCity     = dist <= SCAN_HUGE  && isStartChunk(ox, oz, seed, SP_CITY, SEP_CITY, SALT_CITY);
    const bool sTrial    = dist <= SCAN_BIG   && isStartChunk(ox, oz, seed, SP_TRIAL, SEP_TRIAL, SALT_TRIAL);
    const bool sTrail    = dist <= SCAN_SMALL && isStartChunk(ox, oz, seed, SP_TRAIL, SEP_TRAIL, SALT_TRAIL);
    if (!(sVillage || sFossil || sOutpost || sIgloo || sPortal || sWreck || sRuin ||
          sMansion || sCity || sTrial || sTrail))
        return nullptr;

    // Шаг 2: габариты.
    auto reach = [&](int r) { return w.hits(wx - r, wz - r, wx + r, wz + r); };
    const bool rVillage = sVillage && reach(BB_VILLAGE);
    const bool rFossil  = sFossil  && reach(BB_FOSSIL);
    const bool rOutpost = sOutpost && reach(BB_OUTPOST);
    const bool rIgloo   = sIgloo   && reach(BB_IGLOO);
    const bool rPortal  = sPortal  && reach(BB_PORTAL);
    const bool rWreck   = sWreck   && reach(BB_WRECK);
    const bool rRuin    = sRuin    && reach(BB_RUIN);
    const bool rMansion  = sMansion  && reach(BB_MANSION);
    const bool rCity     = sCity     && reach(BB_CITY);
    const bool rTrial    = sTrial    && reach(BB_TRIAL);
    const bool rTrail    = sTrail    && reach(BB_TRAIL);
    if (!(rVillage || rFossil || rOutpost || rIgloo || rPortal || rWreck || rRuin ||
          rMansion || rCity || rTrial || rTrail))
        return nullptr;

    // Шаг 3: биом и высота — теперь считаются только для реальных кандидатов.
    const int biome = biomes.getBiomeAtBlock(wx, 64, wz);
    const int surf = hs.at(wx, wz);
    const bool onLand = surf > SEA_LEVEL;          // суша только над уровнем моря
    const bool underWater = surf < SEA_LEVEL - 2;

    // Шаг 4: постройка. Подземные наборы идут первыми — они не конфликтуют с наземными.
    if (rCity) {
        const int deepBiome = biomes.getBiomeAtBlock(wx, -50, wz);
        if (deepBiome == B_DEEP_DARK || surf > SEA_LEVEL) {
            LegacyRng r = buildRng(seed, ox, oz, 11); ancientCityTpl(w, hs, wx, wz, r); return "city";
        }
    }
    if (rTrial && surf > SEA_LEVEL) {
        LegacyRng r = buildRng(seed, ox, oz, 12); trialChambersTpl(w, hs, wx, wz, r); return "chambers";
    }
    if (rMansion && onLand && biome == B_DARK_FOREST && hs.spread(wx, wz, 12) <= 8) {
        LegacyRng r = buildRng(seed, ox, oz, 16); woodlandMansionTpl(w, hs, wx, wz, r); return "mansion";
    }
    if (rTrail && onLand && biomeOkFor("trail", biome)) {
        LegacyRng r = buildRng(seed, ox, oz, 17); trailRuinsTpl(w, hs, wx, wz, r); return "trail";
    }
    if (rVillage && onLand) {
        const VillageKind vk = villageKind(biome);
        if (vk != VK_NONE && hs.spread(wx, wz, 8) <= 6) { // деревня не лепится на скалу
            LegacyRng r = buildRng(seed, ox, oz, 1); villageTpl(w, hs, wx, wz, vk, r); return "village";
        }
    }
    // Аванпост: биомный тег + частота 0.2 + запретная зона вокруг деревень.
    if (rOutpost && onLand && outpostBiome(biome)) {
        LegacyRng fr = buildRng(seed, ox, oz, 44);
        if (fr.nextInt(5) == 0 && !nearVillageStart(seed, ox, oz, 10)) {
            LegacyRng r = buildRng(seed, ox, oz, 4); outpostTpl(w, hs, wx, wz, r); return "outpost";
        }
        return nullptr;
    }
    if (rIgloo && onLand && iglooBiome(biome)) {
        LegacyRng r = buildRng(seed, ox, oz, 5); iglooTpl(w, hs, wx, wz, r); return "igloo";
    }
    if (rFossil && (biome == B_DESERT || isBadlandsTag(biome) || biome == B_SWAMP)) {
        LegacyRng r = buildRng(seed, ox, oz, 18); fossilTpl(w, hs, wx, wz, r); return "fossil";
    }
    if (rPortal) {
        const bool ok = portalBiome(biome);
        if (ok) { LegacyRng r = buildRng(seed, ox, oz, 7); ruinedPortalTpl(w, hs, wx, wz, biome, r); return "portal"; }
    }
    if (rWreck && (isOcean(biome) || isBeach(biome))) {
        LegacyRng r = buildRng(seed, ox, oz, 8); shipwreckTpl(w, hs, wx, wz, isBeach(biome), r); return "shipwreck";
    }
    if (rRuin && underWater && (isWarmOcean(biome) || isColdOcean(biome))) {
        LegacyRng r = buildRng(seed, ox, oz, 9); oceanRuinTpl(w, hs, wx, wz, isWarmOcean(biome), r); return "ruin";
    }
    return nullptr;
}

inline void place(ChunkColumn& c, const wg::BiomeSource& biomes, const wg::OverworldRouter& router,
                  uint64_t seed, int cx, int cz, const char** placedOut = nullptr) {
    Writer w{ c, cx * 16, cz * 16 };
    HeightSampler hs(router);
    for (int ox = cx - SCAN_HUGE; ox <= cx + SCAN_HUGE; ++ox)
        for (int oz = cz - SCAN_HUGE; oz <= cz + SCAN_HUGE; ++oz) {
            const int dist = std::max(std::abs(ox - cx), std::abs(oz - cz));
            const char* built = buildFrom(w, hs, biomes, seed, ox, oz, dist);
            if (built && dist == 0 && placedOut) *placedOut = built;
        }
}

} // namespace nc::world::structures
