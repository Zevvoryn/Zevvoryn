#include "chunk.hpp"
#include "worldgen.hpp" // WORLDGEN_V1
#include "wg_caves.hpp"  // CAVES_V1
#include "wg_aquifer.hpp" // AQUIFER_V1
#include "wg_features.hpp" // FEATURES_V1
#include "wg_structures.hpp" // STRUCTURES_V1
#include "../registries/registry.hpp"
#include "../core/item_blocks.gen.hpp" // BLOCKFIX_V1: полная ванильная таблица block-state id 1.21.1
#include <unordered_map> // WORLDGEN_V1
#include "../core/log.hpp"
#include <fstream>
#include <filesystem>
#include <string_view>
#include <thread>   // PERF_ASYNC_V1
#include <chrono>   // PERF_ASYNC_V1

namespace nc::world {

// ============================================================
// ChunkSection
// ============================================================

ChunkSection::ChunkSection() {
    blocks_.fill(0); // air = state ID 0
    biomes_.fill(0); // plains = biome ID 0
    nonAirCount_ = 0;
}

void ChunkSection::setBlock(i32 x, i32 y, i32 z, i32 stateId) {
    auto idx = blockIndex(x, y, z);
    i32 old = blocks_[idx];
    if (old == stateId) return;

    // Подсчёт non-air
    auto& mgr = registries::RegistryManager::instance();
    const auto* oldState = mgr.blockStates().getById(old);
    const auto* newState = mgr.blockStates().getById(stateId);

    bool wasAir = !oldState || oldState->isAir();
    bool isAir = newState && newState->isAir();

    if (wasAir && !isAir) nonAirCount_++;
    else if (!wasAir && isAir) nonAirCount_--;

    blocks_[idx] = static_cast<u16>(stateId);
}

i32 ChunkSection::getBlock(i32 x, i32 y, i32 z) const {
    return blocks_[blockIndex(x, y, z)];
}

void ChunkSection::setBiome(i32 x, i32 y, i32 z, i32 biomeId) {
    biomes_[biomeIndex(x, y, z)] = static_cast<u16>(biomeId);
}

i32 ChunkSection::getBiome(i32 x, i32 y, i32 z) const {
    return biomes_[biomeIndex(x, y, z)];
}

void ChunkSection::writeTo(net::Buffer& buf) const {
    // FLATWORLD_V1: корректный paletted container для 1.21.1.
    // ВАЖНО: с MC 1.16 значения НЕ пересекают границы long!
    // 15 бит/блок -> 4 блока на long -> 1024 long на секцию.

    buf.writeI16(static_cast<i16>(nonAirCount_));

    if (nonAirCount_ == 0) {
        // Пустая секция: single-value палитра (воздух) — экономит ~8КБ
        buf.writeByte(0);   // bits per entry = 0
        buf.writeVarInt(0); // единственное значение: air (state 0)
        buf.writeVarInt(0); // data array length = 0
    } else {
        const u8 bitsPerBlock = 15; // direct palette для 1.21.1
        buf.writeByte(bitsPerBlock);
        const size_t valuesPerLong = 64 / bitsPerBlock; // 4
        const size_t longsNeeded = (BLOCKS_PER_SECTION + valuesPerLong - 1) / valuesPerLong; // 1024
        buf.writeVarInt(static_cast<i32>(longsNeeded));
        size_t i = 0;
        for (size_t l = 0; l < longsNeeded; ++l) {
            u64 packed = 0;
            for (size_t v = 0; v < valuesPerLong && i < BLOCKS_PER_SECTION; ++v, ++i) {
                u64 val = static_cast<u64>(blocks_[i]) & ((1ull << bitsPerBlock) - 1);
                packed |= val << (v * bitsPerBlock);
            }
            buf.writeU64(packed);
        }
    }

    // Биомы: у нас один биом на секцию -> single-value палитра
    // (для биомов валидны только 0-3 бита, 4 было вне спеки)
    buf.writeByte(0);                            // bits per entry = 0
    buf.writeVarInt(biomes_[0]);                 // единственный биом секции
    buf.writeVarInt(0);                          // data array length = 0
}

// MEM_V1: serialize an all-air section without allocating one.
void ChunkSection::writeEmpty(net::Buffer& buf) {
    buf.writeI16(0);    // non-air count = 0
    buf.writeByte(0);   // blocks: single-value palette
    buf.writeVarInt(0); // value = air (state 0)
    buf.writeVarInt(0); // data array length = 0
    buf.writeByte(0);   // biomes: single-value palette
    buf.writeVarInt(0); // biome = plains (0)
    buf.writeVarInt(0); // data array length = 0
}

// ============================================================
// ChunkColumn
// ============================================================

ChunkColumn::ChunkColumn(i32 x, i32 z) : x_(x), z_(z) {
    // MEM_V1: sections are allocated lazily on first non-air write. All-air
    // sections (sky, void) then cost nothing instead of ~8KB each.
}

ChunkSection& ChunkColumn::getSection(i32 index) {
    auto& sec = sections_.at(static_cast<size_t>(index));
    if (!sec) sec = std::make_unique<ChunkSection>(); // MEM_V1: lazy alloc
    return *sec;
}

const ChunkSection& ChunkColumn::getSection(i32 index) const {
    return *sections_.at(static_cast<size_t>(index));
}

void ChunkColumn::setBlock(i32 x, i32 y, i32 z, i32 stateId) {
    i32 secIdx = sectionIndex(y);
    if (secIdx < 0 || secIdx >= SECTIONS_PER_CHUNK) return;
    auto& sec = sections_[static_cast<size_t>(secIdx)];
    if (!sec) {
        if (stateId == 0) return; // MEM_V1: air into an unallocated (air) section = no-op
        sec = std::make_unique<ChunkSection>();
    }
    sec->setBlock(x, y, z, stateId);
    dirty_ = true;
}

i32 ChunkColumn::getBlock(i32 x, i32 y, i32 z) const {
    i32 secIdx = sectionIndex(y);
    if (secIdx < 0 || secIdx >= SECTIONS_PER_CHUNK) return 0;
    const auto& sec = sections_[static_cast<size_t>(secIdx)];
    return sec ? sec->getBlock(x, y, z) : 0; // MEM_V1: null section = all air
}

void ChunkColumn::writeTo(net::Buffer& buf, bool includeBiomes) const {
    // 1.21.1 Chunk Data payload:
    // i32: heightmap type (0 = World surface)
    // varint: primary bit mask (какие секции отправляем)
    // chunk data: секции

    // Heightmaps (пропускаем — необязательно для базовой работы)
    // Пока просто отправляем пустой NBT для heightmaps

    // Chunk sections (MEM_V1: unallocated sections serialize as all-air)
    for (i32 i = 0; i < SECTIONS_PER_CHUNK; ++i) {
        const auto& sec = sections_[static_cast<size_t>(i)];
        if (sec) sec->writeTo(buf);
        else ChunkSection::writeEmpty(buf);
    }
}

// ============================================================
// World
// ============================================================

std::shared_ptr<ChunkColumn> World::getChunk(i32 x, i32 z) {
    ChunkPos pos{x, z};
    auto it = chunks_.find(pos);
    return it != chunks_.end() ? it->second : nullptr;
}

std::shared_ptr<ChunkColumn> World::getChunkOrCreate(i32 x, i32 z) {
    ChunkPos pos{x, z};
    auto it = chunks_.find(pos);
    if (it != chunks_.end()) return it->second;

    auto chunk = std::make_shared<ChunkColumn>(x, z);
    chunks_[pos] = chunk;
    return chunk;
}

void World::setBlock(i32 x, i32 y, i32 z, i32 stateId) {
    i32 cx = x >> 4;
    i32 cz = z >> 4;
    auto chunk = getChunkOrCreate(cx, cz);
    chunk->setBlock(x, y, z, stateId);
}

i32 World::getBlock(i32 x, i32 y, i32 z) const {
    i32 cx = x >> 4;
    i32 cz = z >> 4;
    ChunkPos pos{cx, cz};
    auto it = chunks_.find(pos);
    if (it == chunks_.end()) return 0;
    return it->second->getBlock(x, y, z);
}

void World::unloadChunk(i32 x, i32 z) {
    chunks_.erase(ChunkPos{x, z});
}

void World::pruneChunks(const std::vector<std::pair<i32, i32>>& keepCenters, i32 keepRadius) {
    if (keepCenters.empty()) return; // never unload everything
    std::vector<ChunkPos> toErase;
    for (const auto& kv : chunks_) {
        const ChunkPos& p = kv.first;
        bool keep = false;
        for (const auto& c : keepCenters) {
            i32 dx = p.x - c.first;  if (dx < 0) dx = -dx;
            i32 dz = p.z - c.second; if (dz < 0) dz = -dz;
            if (dx <= keepRadius && dz <= keepRadius) { keep = true; break; }
        }
        if (!keep) toErase.push_back(p);
    }
    for (const auto& p : toErase) chunks_.erase(p);
    if (!toErase.empty()) {
        std::lock_guard<std::mutex> lk(genMutex_);
        for (const auto& p : toErase) { genInFlight_.erase(p); genReady_.erase(p); }
    }
}

// FLATWORLD_V1: классический суперфлэт: Y=0 бедрок, Y=1-2 з��мля, Y=3 трава.
// Чанки генерятся НА ЛЕТУ в getOrGenerateChunk -> мир бесконечный.

// ============================================================
// WORLDGEN_V1: ванильный overworld (noise -> density -> terrain -> biomes)
// ============================================================

struct World::WorldGenState {
    wg::OverworldRouter router;
    wg::BiomeSource biomes;
    wg::u64 seed;
    i32 stone = 1, deepslate = 24905, dirt = 10, grass = 9, coarseDirt = 11, podzol = 13,
        sand = 112, sandstone = 535, redSand = 117, redSandstone = 11079, gravel = 118,
        water = 80, snow = 5781, packedIce = 10746, ice = 5780, mud = 24903,
        terracotta = 10744, whiteTerracotta = 9356, orangeTerracotta = 9357,
        yellowTerracotta = 9360, brownTerracotta = 9368, redTerracotta = 9370,
        lightGrayTerracotta = 9364, calcite = 22316, mycelium = 7270, bedrock = 79; // SURFACE_RULES_V1
    explicit WorldGenState(i64 value) : router((wg::u64)value), biomes(router), seed((wg::u64)value) {} // CAVES_V3: carvers are stateless
};

static i32 wgResolve(registries::RegistryManager& mgr, const char* name, i32 fallback) {
    if (auto v = mgr.getBlockStateId(name)) return *v;
    // BLOCKFIX_V1: реестр может быть неполным — берём id из полной ванильной таблицы 1.21.1
    std::string n(name);
    auto pos = n.find(':');
    if (pos != std::string::npos) n = n.substr(pos + 1);
    i32 g = ::nc::gen::blockNameToState(n);
    return (g >= 0) ? g : fallback;
}

static i32 wgMaterialId(const World::WorldGenState& S, wg::SurfaceMaterial m) {
    switch (m) {
        case wg::SM_STONE:                 return S.stone;
        case wg::SM_DEEPSLATE:             return S.deepslate;
        case wg::SM_DIRT:                  return S.dirt;
        case wg::SM_GRASS:                 return S.grass;
        case wg::SM_COARSE_DIRT:           return S.coarseDirt;
        case wg::SM_PODZOL:                return S.podzol;
        case wg::SM_SAND:                  return S.sand;
        case wg::SM_SANDSTONE:             return S.sandstone;
        case wg::SM_RED_SAND:              return S.redSand;
        case wg::SM_RED_SANDSTONE:         return S.redSandstone;
        case wg::SM_GRAVEL:                return S.gravel;
        case wg::SM_TERRACOTTA:            return S.terracotta;
        case wg::SM_WHITE_TERRACOTTA:      return S.whiteTerracotta;
        case wg::SM_ORANGE_TERRACOTTA:     return S.orangeTerracotta;
        case wg::SM_YELLOW_TERRACOTTA:     return S.yellowTerracotta;
        case wg::SM_BROWN_TERRACOTTA:      return S.brownTerracotta;
        case wg::SM_RED_TERRACOTTA:        return S.redTerracotta;
        case wg::SM_LIGHT_GRAY_TERRACOTTA: return S.lightGrayTerracotta;
        case wg::SM_MYCELIUM:              return S.mycelium;
        case wg::SM_SNOW_BLOCK:            return S.snow;
        case wg::SM_PACKED_ICE:            return S.packedIce;
        case wg::SM_ICE:                   return S.ice;
        case wg::SM_MUD:                   return S.mud;
        case wg::SM_CALCITE:               return S.calcite;
        case wg::SM_BEDROCK:               return S.bedrock;
        default:                           return S.stone;
    }
}

static wg::u64 wgMix(wg::u64 v) { // deterministic per-world bedrock variation
    v += 0x9E3779B97F4A7C15ULL; v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ULL;
    v = (v ^ (v >> 27)) * 0x94D049BB133111EBULL; return v ^ (v >> 31);
}

// Сетевой id биома = индекс в реестре minecraft:worldgen/biome (порядок из server.cpp)
static i32 wgBiomeNetworkId(int biome) {
    static const std::unordered_map<std::string, int> ids = {
        {"badlands",0},{"bamboo_jungle",1},{"basalt_deltas",2},{"beach",3},{"birch_forest",4},
        {"cherry_grove",5},{"cold_ocean",6},{"crimson_forest",7},{"dark_forest",8},{"deep_cold_ocean",9},
        {"deep_dark",10},{"deep_frozen_ocean",11},{"deep_lukewarm_ocean",12},{"deep_ocean",13},{"desert",14},
        {"dripstone_caves",15},{"end_barrens",16},{"end_highlands",17},{"end_midlands",18},{"eroded_badlands",19},
        {"flower_forest",20},{"forest",21},{"frozen_ocean",22},{"frozen_peaks",23},{"frozen_river",24},
        {"grove",25},{"ice_spikes",26},{"jagged_peaks",27},{"jungle",28},{"lukewarm_ocean",29},
        {"lush_caves",30},{"mangrove_swamp",31},{"meadow",32},{"mushroom_fields",33},{"nether_wastes",34},
        {"ocean",35},{"old_growth_birch_forest",36},{"old_growth_pine_taiga",37},{"old_growth_spruce_taiga",38},
        {"plains",39},{"river",40},{"savanna",41},{"savanna_plateau",42},{"small_end_islands",43},
        {"snowy_beach",44},{"snowy_plains",45},{"snowy_slopes",46},{"snowy_taiga",47},{"soul_sand_valley",48},
        {"sparse_jungle",49},{"stony_peaks",50},{"stony_shore",51},{"sunflower_plains",52},{"swamp",53},
        {"taiga",54},{"the_end",55},{"the_void",56},{"warm_ocean",57},{"warped_forest",58},
        {"windswept_forest",59},{"windswept_gravelly_hills",60},{"windswept_hills",61},{"windswept_savanna",62},
        {"wooded_badlands",63},
    };
    auto it = ids.find(wg::biomeName(biome));
    return it == ids.end() ? 39 : it->second; // 39 = plains
}

void World::initDefaultGenerator(i64 seed, bool ru) {
    wgState_ = std::make_unique<WorldGenState>(seed);
    auto& mgr = registries::RegistryManager::instance();
    auto& S = *wgState_;
    S.stone      = wgResolve(mgr, "minecraft:stone", 1);
    S.deepslate  = wgResolve(mgr, "minecraft:deepslate", 24905);
    S.dirt       = wgResolve(mgr, "minecraft:dirt", 10);
    S.coarseDirt = wgResolve(mgr, "minecraft:coarse_dirt", 11);
    S.sand       = wgResolve(mgr, "minecraft:sand", 112);
    S.sandstone  = wgResolve(mgr, "minecraft:sandstone", 535);
    S.redSand    = wgResolve(mgr, "minecraft:red_sand", 117);
    S.redSandstone = wgResolve(mgr, "minecraft:red_sandstone", 11079);
    S.gravel     = wgResolve(mgr, "minecraft:gravel", 118);
    S.water      = wgResolve(mgr, "minecraft:water", 80);
    S.snow       = wgResolve(mgr, "minecraft:snow_block", 5781);
    S.packedIce  = wgResolve(mgr, "minecraft:packed_ice", 10746);
    S.ice        = wgResolve(mgr, "minecraft:ice", 5780);
    S.mud        = wgResolve(mgr, "minecraft:mud", 24903);
    S.terracotta = wgResolve(mgr, "minecraft:terracotta", 10744);
    S.whiteTerracotta = wgResolve(mgr, "minecraft:white_terracotta", 9356);
    S.orangeTerracotta = wgResolve(mgr, "minecraft:orange_terracotta", 9357);
    S.yellowTerracotta = wgResolve(mgr, "minecraft:yellow_terracotta", 9360);
    S.brownTerracotta = wgResolve(mgr, "minecraft:brown_terracotta", 9368);
    S.redTerracotta = wgResolve(mgr, "minecraft:red_terracotta", 9370);
    S.lightGrayTerracotta = wgResolve(mgr, "minecraft:light_gray_terracotta", 9364);
    S.calcite    = wgResolve(mgr, "minecraft:calcite", 22316);
    S.bedrock    = wgResolve(mgr, "minecraft:bedrock", 79);
    S.grass      = mgr.getBlockStateId("minecraft:grass_block", {{"snowy", "false"}}).value_or(::nc::gen::blockNameToState("grass_block"));
    S.mycelium   = mgr.getBlockStateId("minecraft:mycelium", {{"snowy", "false"}}).value_or(::nc::gen::blockNameToState("mycelium"));
    S.podzol     = mgr.getBlockStateId("minecraft:podzol", {{"snowy", "false"}}).value_or(::nc::gen::blockNameToState("podzol"));
    defaultReady_ = true;
    flatReady_ = false;
    if (ru) NC_INFO("World", "Ванильный ге����ератор готов (сид {})", (long long)seed);
    else    NC_INFO("World", "Overworld generator ready (seed {})", (long long)seed);
}

void World::fillDefaultChunk(ChunkColumn& chunk, i32 cx, i32 cz) {
    auto& S = *wgState_;
    wg::ChunkTerrain terrain(S.router, cx, cz);
    // AQUIFER_V1: vanilla NoiseBasedAquifer — подземные озёра воды/лавы
    // seed: world seed хешируется по cx/cz (как в WorldCarver.setLargeFeatureSeed)
    wg::RandomSource aqRng((wg::u64)((i64)S.seed ^
        (i64)cx * (i64)341873128712LL ^
        (i64)cz * (i64)132897987541LL));
    wg::NoiseBasedAquifer aquifer(S.router, cx, cz,
                                   wg::MIN_Y, wg::WORLD_HEIGHT,
                                   S.water, /*lava=*/96,
                                   aqRng.forkPositional());
    for (i32 lx = 0; lx < 16; ++lx) {
        for (i32 lz = 0; lz < 16; ++lz) {
            i32 wx = cx * 16 + lx;
            i32 wz = cz * 16 + lz;
            int sy = terrain.surfaceY(lx, lz);
            if (sy < wg::MIN_Y) sy = wg::MIN_Y;
            int biomeY = (sy < wg::SEA_LEVEL) ? wg::SEA_LEVEL : sy;
            int biome = S.biomes.getBiomeAtBlock(wx, biomeY, wz);
            // SURFACE_RULES_V1 + AQUIFER_V1
            for (int y = wg::MIN_Y; y <= sy; ++y) {
                double d = terrain.densityAt(lx, y, lz);
                int sub = aquifer.computeSubstance(wx, y, wz, d);
                if (sub == -1) {
                    // Твёрдый блок
                    const int depth = sy - y;
                    i32 id = y <= 0 ? S.deepslate : S.stone;
                    if (depth <= 8)
                        id = wgMaterialId(S, wg::overworldSurface(S.router, biome, wx, y, wz, sy, depth));
                    chunk.setBlock(wx, y, wz, id);
                } else if (sub != 0) {
                    // Жидкость из аквифера (waterBlock / lavaBlock)
                    chunk.setBlock(wx, y, wz, sub);
                }
                // sub==0: воздух, не трогаем
            }
            // BEDROCK_FLOOR_V1
            chunk.setBlock(wx, wg::MIN_Y, wz, S.bedrock);
            // Вода над поверхностью — океан/река
            for (int y = sy + 1; y <= wg::SEA_LEVEL; ++y)
                chunk.setBlock(wx, y, wz, S.water);
        }
    }
    // CAVES_V3: vanilla carvers (cave + cave_extra + canyon) over neighbours [-8..8].
    caves::carve(chunk, cx, cz, S.seed, /*air*/0, S.water, /*lava*/96);
    // FEATURES_V1: place ores, trees and plants after carvers (vanilla feature step).
    features::place(chunk, S.biomes, S.seed, cx, cz);
    // STRUCTURES_V1: vanilla structure-set placement grids and compact start-chunk builds.
    structures::place(chunk, S.biomes, S.seed, cx, cz);
    for (i32 si = 0; si < SECTIONS_PER_CHUNK; ++si) {
        i32 midY = CHUNK_HEIGHT_MIN + si * SECTION_HEIGHT + 8;
        int b = S.biomes.getBiomeAtBlock(cx * 16, midY, cz * 16);
        chunk.getSection(si).setBiome(0, 0, 0, wgBiomeNetworkId(b));
    }
}

void World::generateDefault(i64 seed, i32 centerX, i32 centerZ, i32 radius, bool ru) {
    initDefaultGenerator(seed, ru);
    if (ru) NC_INFO("World", "Подготовка спавн-зоны (генератор ландшафта NoiseBasedChunkGenerator)...");
    else    NC_INFO("World", "Preparing start region for dimension minecraft:overworld");
    i32 side = radius * 2 + 1;
    i32 total = side * side;
    i32 done = 0, lastBucket = -1;
    for (i32 cx = centerX - radius; cx <= centerX + radius; ++cx) {
        for (i32 cz = centerZ - radius; cz <= centerZ + radius; ++cz) {
            getOrGenerateChunk(cx, cz);
            ++done;
            i32 pct = (i32)((done * 100LL) / (total > 0 ? total : 1));
            if (pct / 10 != lastBucket) {
                lastBucket = pct / 10;
                if (ru) NC_INFO("World", "Подготовка спавн-зоны: {}%", pct);
                else    NC_INFO("World", "Preparing spawn area: {}%", pct);
            }
        }
    }
    if (ru) NC_INFO("World", "Спавн-зона успешно сгенерирована (радиус {} чанков, всего {} чанков)", radius, total);
    else    NC_INFO("World", "Spawn area prepared (radius {} chunks, {} chunks total)", radius, total);
}

// SPAWN_V1: подобрать случайную точку спавна на поверхности (не в океане)
bool World::findWorldSpawn(i64 seed, i32& outX, i32& outY, i32& outZ) {
    if (!defaultReady_ || !wgState_) { outX = 0; outY = 5; outZ = 0; return false; }
    u64 s = (u64)seed ^ 0x9E3779B97F4A7C15ULL;
    auto nextRand = [&]() -> u64 { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
    const i32 waterId = wgState_->water;
    for (int attempt = 0; attempt < 96; ++attempt) {
        i32 x = (i32)(nextRand() % 4001) - 2000;
        i32 z = (i32)(nextRand() % 4001) - 2000;
        getOrGenerateChunk(x >> 4, z >> 4);
        for (i32 y = CHUNK_HEIGHT_MAX - 1; y >= CHUNK_HEIGHT_MIN; --y) {
            i32 id = getBlock(x, y, z);
            if (id > 0) {
                if (id == waterId) break; // океан/река — берём другую точку
                outX = x; outY = y + 1; outZ = z;
                return true;
            }
        }
    }
    outX = 0; outY = 70; outZ = 0; return true;
}

World::World() = default; // WORLDGEN_FIX_V1 (тип полный здесь)
World::~World() { // PERF_ASYNC_V1 // WORLDGEN_V1 (WorldGenState полный тип здесь)
    loadStop_.store(true); // FASTBOOT_V1: stop + join the background disk loader
    if (loadWorker_.joinable()) loadWorker_.join();
    stopGenPool();
}

// PERF_ASYNC_V1: parallel chunk generation. Worldgen sampling is read-only on the
// shared router/biomes, so many threads can build columns at once. Workers never
// touch chunks_; only the main thread installs finished columns via takeReadyChunk.
void World::startGenPool() {
    std::lock_guard<std::mutex> lk(genMutex_);
    if (genStarted_) return;
    genStarted_ = true;
    genStop_.store(false, std::memory_order_release);
    unsigned hc = std::thread::hardware_concurrency();
    unsigned n = (hc > 1) ? (hc - 1) : 1; // leave one core for the tick/main thread
    if (genThreadOverride_ > 0) n = genThreadOverride_; // PERF_TUNE_V1: config max-cores
    if (n < 1) n = 1;
    if (n > 32) n = 32; // PERF_TUNE_V1: raised cap (was 8) for high-core CPUs
    genWorkers_.reserve(n);
    for (unsigned i = 0; i < n; ++i) genWorkers_.emplace_back([this]{ genWorkerLoop(); });
    NC_DEBUG("World", "Async chunk generator started: {} worker thread(s)", n);
}

void World::stopGenPool() {
    {
        std::lock_guard<std::mutex> lk(genMutex_);
        if (!genStarted_) return;
        genStop_.store(true, std::memory_order_release);
    }
    genCv_.notify_all();
    for (auto& t : genWorkers_) if (t.joinable()) t.join();
    genWorkers_.clear();
    std::lock_guard<std::mutex> lk(genMutex_);
    genStarted_ = false;
    genQueue_.clear();
    genInFlight_.clear();
    genReady_.clear();
}

void World::genWorkerLoop() {
    for (;;) {
        ChunkPos pos{0, 0};
        {
            std::unique_lock<std::mutex> lk(genMutex_);
            genCv_.wait(lk, [this]{ return genStop_.load(std::memory_order_acquire) || !genQueue_.empty(); });
            if (genStop_.load(std::memory_order_acquire)) return;
            pos = genQueue_.front();
            genQueue_.pop_front();
        }
        // Heavy CPU work happens OUTSIDE the lock on a private column.
        auto col = std::make_shared<ChunkColumn>(pos.x, pos.z);
        if (defaultReady_ && wgState_) {
            fillDefaultChunk(*col, pos.x, pos.z);
        } else if (flatReady_) {
            for (i32 lx = 0; lx < 16; ++lx) {
                for (i32 lz = 0; lz < 16; ++lz) {
                    i32 wx = pos.x * 16 + lx;
                    i32 wz = pos.z * 16 + lz;
                    col->setBlock(wx, 0, wz, flatBedrockId_);
                    col->setBlock(wx, 1, wz, flatDirtId_);
                    col->setBlock(wx, 2, wz, flatDirtId_);
                    col->setBlock(wx, 3, wz, flatGrassId_);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(genMutex_);
            genReady_[pos] = std::move(col); // stays in genInFlight_ until taken
        }
    }
}

void World::requestChunkAsync(i32 cx, i32 cz) {
    if (!genStarted_) startGenPool(); // lazy start (main thread only)
    ChunkPos pos{cx, cz};
    if (chunks_.find(pos) != chunks_.end()) return; // already live
    {
        std::lock_guard<std::mutex> lk(genMutex_);
        if (genInFlight_.count(pos) || genReady_.count(pos)) return;
        genInFlight_.insert(pos);
        genQueue_.push_back(pos);
    }
    genCv_.notify_one();
}

bool World::isChunkPending(i32 cx, i32 cz) {
    ChunkPos pos{cx, cz};
    std::lock_guard<std::mutex> lk(genMutex_);
    return genInFlight_.count(pos) != 0;
}

std::shared_ptr<ChunkColumn> World::takeReadyChunk(i32 cx, i32 cz) {
    ChunkPos pos{cx, cz};
    std::shared_ptr<ChunkColumn> col;
    {
        std::lock_guard<std::mutex> lk(genMutex_);
        auto it = genReady_.find(pos);
        if (it == genReady_.end()) return nullptr;
        col = std::move(it->second);
        genReady_.erase(it);
        genInFlight_.erase(pos);
    }
    chunks_[pos] = col; // install into the live world (main thread only)
    return col;
}

void World::initFlatGenerator() {
    auto& mgr = registries::RegistryManager::instance();
    flatBedrockId_ = mgr.getBlockStateId("minecraft:bedrock").value_or(79); // LIGHT_V1
    flatDirtId_    = mgr.getBlockStateId("minecraft:dirt").value_or(10);
    flatGrassId_   = mgr.getBlockStateId("minecraft:grass_block", {{"snowy", "false"}}).value_or(9); // LIGHT_V1
    flatReady_ = true;
}

std::shared_ptr<ChunkColumn> World::getOrGenerateChunk(i32 cx, i32 cz) {
    auto it = chunks_.find(ChunkPos{cx, cz});
    if (it != chunks_.end()) return it->second;
    auto chunk = getChunkOrCreate(cx, cz);
    if (defaultReady_ && wgState_) { // WORLDGEN_V1
        fillDefaultChunk(*chunk, cx, cz);
        return chunk;
    }
    if (!flatReady_) return chunk;
    for (i32 lx = 0; lx < 16; ++lx) {
        for (i32 lz = 0; lz < 16; ++lz) {
            i32 worldX = cx * 16 + lx;
            i32 worldZ = cz * 16 + lz;
            chunk->setBlock(worldX, -64, worldZ, flatBedrockId_); // FLATVANILLA_V1: ванильный суперфлэт -64..-61
            chunk->setBlock(worldX, -63, worldZ, flatDirtId_);
            chunk->setBlock(worldX, -62, worldZ, flatDirtId_);
            chunk->setBlock(worldX, -61, worldZ, flatGrassId_);
        }
    }
    return chunk;
}

void World::generateFlat(i64 seed, i32 centerX, i32 centerZ, i32 radius) {
    (void)seed;
    initFlatGenerator();
    for (i32 cx = centerX - radius; cx <= centerX + radius; ++cx) {
        for (i32 cz = centerZ - radius; cz <= centerZ + radius; ++cz) {
            getOrGenerateChunk(cx, cz);
        }
    }
    if (langRu_) NC_INFO("World", "Flat мир: слои Y=-64..-61 (как в ванилле), стартовая зона {}x{} чанков, дальше — на лету", // FLATVANILLA_V1
        radius * 2 + 1, radius * 2 + 1);
    else NC_INFO("World", "Flat world: layers Y=-64..-61 (vanilla superflat), start area {}x{} chunks, the rest generated on the fly",
        radius * 2 + 1, radius * 2 + 1); // LANGFIX_V1
}


// ============================================================
// WORLDSAVE_V1: сохранение мира на диск.
// Формат world/world.dat: "ZEVW" + u8 version + i32 chunkCount,
// затем для каждого чанка: i32 cx, i32 cz, u32 blockCount,
// blockCount * (u8 lx, i16 y, u8 lz, i32 stateId) — только не-воздух.
// ============================================================

bool World::saveToDisk(const std::string& path) const {
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write("ZEVW", 4);
    u8 version = 3; // LIGHT_V1: ванильные ID блоков
    f.write(reinterpret_cast<const char*>(&version), 1);
    i32 count = static_cast<i32>(chunks_.size());
    f.write(reinterpret_cast<const char*>(&count), 4);
    for (const auto& [pos, chunk] : chunks_) {
        i32 cx = chunk->getX();
        i32 cz = chunk->getZ();
        f.write(reinterpret_cast<const char*>(&cx), 4);
        f.write(reinterpret_cast<const char*>(&cz), 4);
        std::string blockData;
        u32 blockCount = 0;
        for (i32 y = CHUNK_HEIGHT_MIN; y < CHUNK_HEIGHT_MAX; ++y) {
            for (i32 lx = 0; lx < 16; ++lx) {
                for (i32 lz = 0; lz < 16; ++lz) {
                    i32 id = chunk->getBlock(cx * 16 + lx, y, cz * 16 + lz);
                    if (id == 0) continue;
                    u8 blx = static_cast<u8>(lx);
                    u8 blz = static_cast<u8>(lz);
                    i16 by = static_cast<i16>(y);
                    blockData.append(reinterpret_cast<const char*>(&blx), 1);
                    blockData.append(reinterpret_cast<const char*>(&by), 2);
                    blockData.append(reinterpret_cast<const char*>(&blz), 1);
                    blockData.append(reinterpret_cast<const char*>(&id), 4);
                    ++blockCount;
                }
            }
        }
        f.write(reinterpret_cast<const char*>(&blockCount), 4);
        f.write(blockData.data(), static_cast<std::streamsize>(blockData.size()));
    }
    return f.good();
}

bool World::loadFromDisk(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[4] = {};
    f.read(magic, 4);
    if (!f || std::string_view(magic, 4) != "ZEVW") return false;
    u8 version = 0;
    f.read(reinterpret_cast<char*>(&version), 1);
    if (version != 3) return false; // LIGHT_V1: сейвы со старыми ID отбрасываем
    i32 count = 0;
    f.read(reinterpret_cast<char*>(&count), 4);
    if (!f || count < 0 || count > 100000) return false;
    for (i32 c = 0; c < count && f; ++c) {
        i32 cx = 0, cz = 0;
        u32 blockCount = 0;
        f.read(reinterpret_cast<char*>(&cx), 4);
        f.read(reinterpret_cast<char*>(&cz), 4);
        f.read(reinterpret_cast<char*>(&blockCount), 4);
        if (!f) return false;
        auto chunk = getChunkOrCreate(cx, cz);
        for (u32 i = 0; i < blockCount && f; ++i) {
            u8 lx = 0, lz = 0;
            i16 y = 0;
            i32 id = 0;
            f.read(reinterpret_cast<char*>(&lx), 1);
            f.read(reinterpret_cast<char*>(&y), 2);
            f.read(reinterpret_cast<char*>(&lz), 1);
            f.read(reinterpret_cast<char*>(&id), 4);
            chunk->setBlock(cx * 16 + lx, static_cast<i32>(y), cz * 16 + lz, id);
        }
    }
    if (langRu_) NC_INFO("World", "Мир загружен из {}: {} чанков", path, chunks_.size());
    else NC_INFO("World", "World loaded from {}: {} chunks", path, chunks_.size()); // LANGFIX_V1
    return true;
}

// FASTBOOT_V1: validate + read the header on the calling (main) thread, then spawn
// a worker that parses chunk bodies off-thread. Returns false if there is no valid
// save (the caller then generates a fresh world synchronously).
bool World::startBackgroundLoad(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[4] = {};
    f.read(magic, 4);
    if (!f || std::string_view(magic, 4) != "ZEVW") return false;
    u8 version = 0;
    f.read(reinterpret_cast<char*>(&version), 1);
    if (version != 3) return false; // LIGHT_V1: old saves are dropped
    i32 count = 0;
    f.read(reinterpret_cast<char*>(&count), 4);
    if (!f || count < 0 || count > 100000) return false;
    // Header is valid; hand the heavy per-block parsing to a background thread so
    // the server can start listening immediately.
    loadExpected_ = count;
    loadDone_.store(false);
    loadStop_.store(false);
    loadWorker_ = std::thread([this, path, count]() { loadWorkerLoop(path, count); });
    return true;
}

void World::loadWorkerLoop(std::string path, i32 count) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { loadDone_.store(true); return; }
    f.seekg(4 + 1 + 4, std::ios::beg); // skip "ZEVW" + version + count
    for (i32 c = 0; c < count && f && !loadStop_.load(); ++c) {
        i32 cx = 0, cz = 0;
        u32 blockCount = 0;
        f.read(reinterpret_cast<char*>(&cx), 4);
        f.read(reinterpret_cast<char*>(&cz), 4);
        f.read(reinterpret_cast<char*>(&blockCount), 4);
        if (!f) break;
        // Build a standalone column off-thread (touches no shared state).
        auto col = std::make_shared<ChunkColumn>(cx, cz);
        for (u32 i = 0; i < blockCount && f; ++i) {
            u8 lx = 0, lz = 0;
            i16 y = 0;
            i32 id = 0;
            f.read(reinterpret_cast<char*>(&lx), 1);
            f.read(reinterpret_cast<char*>(&y), 2);
            f.read(reinterpret_cast<char*>(&lz), 1);
            f.read(reinterpret_cast<char*>(&id), 4);
            col->setBlock(cx * 16 + lx, static_cast<i32>(y), cz * 16 + lz, id);
        }
        col->clearDirty(); // loaded from disk, not a live player edit
        {
            std::lock_guard<std::mutex> lk(loadMutex_);
            loadReady_.push_back(std::move(col));
        }
    }
    loadDone_.store(true);
}

void World::drainLoadedChunks() {
    std::deque<std::shared_ptr<ChunkColumn>> batch;
    {
        std::lock_guard<std::mutex> lk(loadMutex_);
        if (loadReady_.empty()) return;
        batch.swap(loadReady_);
    }
    for (auto& col : batch) {
        if (!col) continue;
        ChunkPos pos{col->getX(), col->getZ()};
        // insert-if-absent: never clobber a chunk already live (generated or edited
        // during the short load window) so there is no client desync.
        if (chunks_.find(pos) == chunks_.end()) {
            chunks_[pos] = std::move(col);
        }
    }
}

} // namespace nc::world
