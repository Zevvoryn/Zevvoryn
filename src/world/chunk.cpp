#include "chunk.hpp"
#include "worldgen.hpp" // WORLDGEN_V1
#include "wg_caves.hpp"  // CAVES_V1
#include "wg_aquifer.hpp" // AQUIFER_V1
#include "wg_features.hpp" // FEATURES_V1
#include "wg_structures.hpp" // STRUCTURES_V1
#include "wg_dim.hpp"        // DIMGEN_V1: генераторы Ада и Энда
#include "../registries/registry.hpp"
#include "../core/item_blocks.gen.hpp" // BLOCKFIX_V1: полная ванильная таблица block-state id 1.21.1
#include <unordered_map> // WORLDGEN_V1
#include "../core/log.hpp"
#include <fstream>
#include <filesystem>
#include <string_view>
#include <thread>   // PERF_ASYNC_V1
#include <chrono>   // PERF_ASYNC_V1
#include <cstring>  // WORLDCOMPRESS_V1
#include "../network/zlib_codec.hpp" // WORLDCOMPRESS_V1

namespace nc::world {

// ============================================================
// ChunkSection
// ============================================================

// ============================================================
// MEM_V4: paletted + bit-packed хранилище секции
// ============================================================

size_t ChunkSection::longsFor(u8 bits) {
    const size_t vpl = 64u / bits;                       // значения не пересекают long
    return (static_cast<size_t>(BLOCKS_PER_SECTION) + vpl - 1) / vpl;
}

u32 ChunkSection::rawAt(size_t idx) const {
    if (bits_ == 0) return 0;
    const size_t vpl = 64u / bits_;
    const u64 word = data_[idx / vpl];
    const size_t shift = (idx % vpl) * bits_;
    return static_cast<u32>((word >> shift) & ((1ull << bits_) - 1ull));
}

void ChunkSection::setRaw(size_t idx, u32 value) {
    if (bits_ == 0) return;
    const size_t vpl = 64u / bits_;
    const size_t shift = (idx % vpl) * bits_;
    const u64 mask = ((1ull << bits_) - 1ull) << shift;
    u64& word = data_[idx / vpl];
    word = (word & ~mask) | ((static_cast<u64>(value) & ((1ull << bits_) - 1ull)) << shift);
}

i32 ChunkSection::stateAt(size_t idx) const {
    if (bits_ == 0) return palette_.empty() ? 0 : static_cast<i32>(palette_[0]);
    const u32 raw = rawAt(idx);
    if (palette_.empty()) return static_cast<i32>(raw);   // direct-палитра
    return raw < palette_.size() ? static_cast<i32>(palette_[raw]) : 0;
}

void ChunkSection::repack(u8 newBits, const std::vector<u16>& newPalette) {
    std::vector<u16> old(static_cast<size_t>(BLOCKS_PER_SECTION));
    for (size_t i = 0; i < static_cast<size_t>(BLOCKS_PER_SECTION); ++i)
        old[i] = static_cast<u16>(stateAt(i));

    std::unordered_map<u16, u32> lookup;
    lookup.reserve(newPalette.size() * 2 + 1);
    for (size_t i = 0; i < newPalette.size(); ++i) lookup.emplace(newPalette[i], static_cast<u32>(i));

    palette_ = newPalette;
    bits_ = newBits;
    data_.assign(longsFor(newBits), 0ull);

    for (size_t i = 0; i < old.size(); ++i) {
        if (palette_.empty()) { setRaw(i, static_cast<u32>(old[i])); continue; }
        auto it = lookup.find(old[i]);
        setRaw(i, it != lookup.end() ? it->second : 0u);
    }
}

ChunkSection::ChunkSection() {
    palette_.push_back(0); // вся секция = air, данные не аллокатим вовсе
    bits_ = 0;
    biomes_.fill(39); // BIOMEGREEN_V1: plains = сетевой id 39 (0 был badlands — поэтому трава была жухлая!)
    nonAirCount_ = 0;
}

void ChunkSection::setBlock(i32 x, i32 y, i32 z, i32 stateId) {
    const size_t idx = blockIndex(x, y, z);
    const i32 old = stateAt(idx);
    if (old == stateId) return;

    // Подсчёт non-air
    auto& mgr = registries::RegistryManager::instance();
    const auto* oldState = mgr.blockStates().getById(old);
    const auto* newState = mgr.blockStates().getById(stateId);

    bool wasAir = !oldState || oldState->isAir();
    bool isAir = newState && newState->isAir();

    if (wasAir && !isAir) nonAirCount_++;
    else if (!wasAir && isAir) nonAirCount_--;

    const u16 st = static_cast<u16>(stateId);

    if (bits_ == 0) {
        // Была однородной — разворачиваемся в 4 бита на блок (2 КБ).
        std::vector<u16> np = palette_;
        if (np.empty()) np.push_back(0);
        np.push_back(st);
        repack(4, np);
        setRaw(idx, static_cast<u32>(np.size() - 1));
        return;
    }

    if (palette_.empty()) { setRaw(idx, static_cast<u32>(st)); return; } // direct

    for (size_t i = 0; i < palette_.size(); ++i)
        if (palette_[i] == st) { setRaw(idx, static_cast<u32>(i)); return; }

    if (palette_.size() < (static_cast<size_t>(1) << bits_)) {
        palette_.push_back(st);
        setRaw(idx, static_cast<u32>(palette_.size() - 1));
        return;
    }

    // Палитра переполнилась: шире на бит, а после 8 — в direct-палитру.
    const u8 nb = static_cast<u8>(bits_ + 1);
    if (nb > 8) {
        repack(DIRECT_BITS, std::vector<u16>{});
        setRaw(idx, static_cast<u32>(st));
        return;
    }
    std::vector<u16> np = palette_;
    np.push_back(st);
    repack(nb, np);
    setRaw(idx, static_cast<u32>(np.size() - 1));
}

i32 ChunkSection::getBlock(i32 x, i32 y, i32 z) const {
    return stateAt(blockIndex(x, y, z));
}

void ChunkSection::setBiome(i32 x, i32 y, i32 z, i32 biomeId) {
    biomes_[biomeIndex(x, y, z)] = static_cast<u16>(biomeId);
}

i32 ChunkSection::getBiome(i32 x, i32 y, i32 z) const {
    return biomes_[biomeIndex(x, y, z)];
}

void ChunkSection::writeTo(net::Buffer& buf, i32 biomeOverride) const { // DIMBIOME_V1
    // FLATWORLD_V1: корректный paletted container для 1.21.1.
    // ВАЖНО: с MC 1.16 значения НЕ пересекают границы long!
    // 15 бит/блок -> 4 блока на long -> 1024 long на секцию.

    buf.writeI16(static_cast<i16>(nonAirCount_));

    // MEM_V4: льём внутреннее представление как есть: оно уже в формате
    // paletted container 1.21.1. Бонусом пакет Chunk Data стал в разы меньше:
    // было всегда 1024 long на секцию (8 КБ), теперь типично 256 long (2 КБ).
    if (bits_ == 0) {
        buf.writeByte(0);                                                   // single-value палитра
        buf.writeVarInt(palette_.empty() ? 0 : static_cast<i32>(palette_[0]));
        buf.writeVarInt(0);                                                 // data array length = 0
    } else if (!palette_.empty()) {
        buf.writeByte(bits_);                                               // indirect: 4..8 бит
        buf.writeVarInt(static_cast<i32>(palette_.size()));
        for (u16 s : palette_) buf.writeVarInt(static_cast<i32>(s));
        buf.writeVarInt(static_cast<i32>(data_.size()));
        for (u64 word : data_) buf.writeU64(word);
    } else {
        buf.writeByte(DIRECT_BITS);                                         // direct: 15 бит
        buf.writeVarInt(static_cast<i32>(data_.size()));
        for (u64 word : data_) buf.writeU64(word);
    }

    // Биомы: у нас один биом на секцию -> single-value палитра
    // (для биомов валидны только 0-3 бита, 4 было вне спеки)
    buf.writeByte(0);                            // bits per entry = 0
    buf.writeVarInt(biomeOverride >= 0 ? biomeOverride : biomes_[0]); // DIMBIOME_V1
    buf.writeVarInt(0);                          // data array length = 0
}

// MEM_V1: serialize an all-air section without allocating one.
void ChunkSection::writeEmpty(net::Buffer& buf, i32 biomeOverride) { // DIMBIOME_V1
    buf.writeI16(0);    // non-air count = 0
    buf.writeByte(0);   // blocks: single-value palette
    buf.writeVarInt(0); // value = air (state 0)
    buf.writeVarInt(0); // data array length = 0
    buf.writeByte(0);   // biomes: single-value palette
    buf.writeVarInt(biomeOverride >= 0 ? biomeOverride : 39); // DIMBIOME_V1
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
    saveBlobValid_ = false; // FASTSAVE_V2: кэш этой колонны устарел
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
        if (sec) sec->writeTo(buf, biomeId_); // DIMBIOME_V1
        else ChunkSection::writeEmpty(buf, biomeId_);
    }
}

// ============================================================
// World
// ============================================================

std::shared_ptr<ChunkColumn> World::getChunk(i32 x, i32 z) {
    ChunkPos pos{x, z};
    std::lock_guard<std::mutex> lk(chunksMutex_); // RACE_FIX_V1
    auto it = chunks_.find(pos);
    return it != chunks_.end() ? it->second : nullptr;
}

std::shared_ptr<ChunkColumn> World::getChunkOrCreate(i32 x, i32 z, bool* wasCreated) {
    ChunkPos pos{x, z};
    std::lock_guard<std::mutex> lk(chunksMutex_); // RACE_FIX_V1: atomic find-or-create
    auto it = chunks_.find(pos);
    if (it != chunks_.end()) {
        if (wasCreated) *wasCreated = false;
        return it->second;
    }

    auto chunk = std::make_shared<ChunkColumn>(x, z);
    chunks_[pos] = chunk;
    if (wasCreated) *wasCreated = true;
    return chunk;
}

void World::setBlock(i32 x, i32 y, i32 z, i32 stateId) {
    i32 cx = x >> 4;
    i32 cz = z >> 4;
    auto chunk = getChunkOrCreate(cx, cz);
    chunk->setBlock(x, y, z, stateId);
    editSeq_.fetch_add(1, std::memory_order_relaxed); // IDLESAVE_V1
}

i32 World::getBlock(i32 x, i32 y, i32 z) const {
    i32 cx = x >> 4;
    i32 cz = z >> 4;
    ChunkPos pos{cx, cz};
    std::lock_guard<std::mutex> lk(chunksMutex_); // RACE_FIX_V1
    auto it = chunks_.find(pos);
    if (it == chunks_.end()) return 0;
    return it->second->getBlock(x, y, z);
}

void World::unloadChunk(i32 x, i32 z) {
    std::lock_guard<std::mutex> lk(chunksMutex_); // RACE_FIX_V1
    chunks_.erase(ChunkPos{x, z});
}

void World::pruneChunks(const std::vector<std::pair<i32, i32>>& keepCenters, i32 keepRadius) {
    if (keepCenters.empty()) return; // never unload everything
    std::vector<ChunkPos> toErase;
    {
        std::lock_guard<std::mutex> lk(chunksMutex_); // RACE_FIX_V1
        for (const auto& kv : chunks_) {
            const ChunkPos& p = kv.first;
            bool keep = false;
            for (const auto& c : keepCenters) {
                i32 dx = p.x - c.first;  if (dx < 0) dx = -dx;
                i32 dz = p.z - c.second; if (dz < 0) dz = -dz;
                if (dx <= keepRadius && dz <= keepRadius) { keep = true; break; }
            }
            // WORLD_DIRTY_PIN_V1: never unload live edits before they have been
            // persisted. Otherwise clients keep a cached wall while the server
            // silently forgets every edited chunk outside the current radius.
            if (!keep && (!kv.second || !kv.second->isDirty())) toErase.push_back(p);
        }
        for (const auto& p : toErase) chunks_.erase(p);
    }
    if (!toErase.empty()) {
        std::lock_guard<std::mutex> lk(genMutex_);
        for (const auto& p : toErase) { genInFlight_.erase(p); genReady_.erase(p); }
    }
    // MEM_V2: фоновый пул генерит всю округу (requestChunkAsync из sendChunksAround),
    // но takeReadyChunk() забирает только то, что реально ушло игроку. Незабранные
    // колонны не лежат в chunks_, поэтому цикл выше их не видел и они висели в genReady_
    // до конца жизни процесса — после каждого дальнего телепорта плюс сотни колонн в ОЗУ.
    {
        std::lock_guard<std::mutex> lk(genMutex_);
        std::vector<ChunkPos> readyErase;
        for (const auto& kv : genReady_) {
            const ChunkPos& p = kv.first;
            bool keep = false;
            for (const auto& c : keepCenters) {
                i32 dx = p.x - c.first;  if (dx < 0) dx = -dx;
                i32 dz = p.z - c.second; if (dz < 0) dz = -dz;
                if (dx <= keepRadius && dz <= keepRadius) { keep = true; break; }
            }
            if (!keep) readyErase.push_back(p);
        }
        for (const auto& p : readyErase) { genReady_.erase(p); genInFlight_.erase(p); }
    }
}

// FLATWORLD_V1: классический суперфлэт: Y=0 бедрок, Y=1-2 земля, Y=3 трава.
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
    if (ru) NC_INFO("World", "Ванильный генератор готов (сид {})", (long long)seed);
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
    // STRUCTURES_V2: постройки строятся целиком и режутся по границе чанка; раутер нужен
    // для высоты привязки из того же шума, что и у рельефа.
    // STRUCT_QUIET_V1: лог построек убран по просьбе — координаты доступны через /locate.
    structures::place(chunk, S.biomes, S.router, S.seed, cx, cz);
    for (i32 si = 0; si < SECTIONS_PER_CHUNK; ++si) {
        i32 midY = CHUNK_HEIGHT_MIN + si * SECTION_HEIGHT + 8;
        int b = S.biomes.getBiomeAtBlock(cx * 16, midY, cz * 16);
        chunk.getSection(si).setBiome(0, 0, 0, wgBiomeNetworkId(b));
    }
}

// STRUCT_LOCATE_V1 -------------------------------------------------------------
i32 World::biomeAtBlock(i32 wx, i32 wy, i32 wz) {
    if (!defaultReady_ || !wgState_) return -1;
    return wgState_->biomes.getBiomeAtBlock(wx, wy, wz);
}

std::string World::structureKeys() {
    int specCount = 0;
    const auto* specs = structures::gridSpecs(specCount);
    std::string out;
    for (int i = 0; i < specCount; ++i) { if (i) out += ", "; out += specs[i].key; }
    return out;
}

bool World::locateStructure(const std::string& key, i32 fromCx, i32 fromCz, i32 maxRadiusChunks,
                            i32& outX, i32& outZ, std::string& outNameRu, std::string& outNameEn) {
    if (!defaultReady_ || !wgState_) return false;
    int specCount = 0;
    const auto* specs = structures::gridSpecs(specCount);
    const structures::GridSpec* spec = nullptr;
    for (int i = 0; i < specCount; ++i) if (key == specs[i].key) { spec = &specs[i]; break; }
    auto& S = *wgState_;
    const bool treasure = (key == "treasure");
    if (!spec && !treasure) return false;
    if (treasure) { outNameRu = "зарытое сокровище"; outNameEn = "buried treasure"; }
    else { outNameRu = spec->nameRu; outNameEn = spec->nameEn; }
    for (i32 r = 0; r <= maxRadiusChunks; ++r) {
        for (i32 dx = -r; dx <= r; ++dx) {
            for (i32 dz = -r; dz <= r; ++dz) {
                if (r > 0 && std::abs(dx) != r && std::abs(dz) != r) continue; // только рамка кольца
                const i32 cx = fromCx + dx, cz = fromCz + dz;
                const i32 wx = cx * 16 + 8, wz = cz * 16 + 8;
                if (treasure) {
                    const int biome = S.biomes.getBiomeAtBlock(cx * 16 + 9, 64, cz * 16 + 9);
                    if (!structures::isBeach(biome)) continue;
                    auto tr = structures::buildRng(S.seed, cx, cz, 10);
                    if (tr.nextInt(100) != 0) continue;
                    outX = cx * 16 + 9; outZ = cz * 16 + 9;
                    return true;
                }
                if (!structures::isStartChunk(cx, cz, S.seed, spec->spacing, spec->separation, spec->salt)) continue;
                const int biome = S.biomes.getBiomeAtBlock(wx, 64, wz);
                if (!structures::biomeOkFor(spec->key, biome)) continue;
                if (key == "outpost") { // частота 0.2 + запретная зона вокруг деревень
                    auto fr = structures::buildRng(S.seed, cx, cz, 44);
                    if (fr.nextInt(5) != 0) continue;
                    if (structures::nearVillageStart(S.seed, cx, cz, 10)) continue;
                }
                outX = wx; outZ = wz;
                return true;
            }
        }
    }
    return false;
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
    // CPU_V1: было hc-1 (на 12 потоках — 11 генераторов). Они грызут все ядра
    // сразу, поэтому CPU улетал в 50%+, а клиент на той же машине лагал (ему
    // нужны ядра на рендер и свой chunk builder). Генерация от этого не ускорялась:
    // узкое место — отдача чанков игроку, а не шум. Стало: четверть ядер, 2..6.
    // CPU_V2: hc/4 (3 потока на 12 ядрах) убило скорость загрузки при view-distance 16:
    // это 1089 колонн на игрока. Стало: половина ядер, 4..8 — есть запас для
    // клиента на той же машине, но пул больше не является узким местом.
    unsigned n = hc / 2;
    if (n < 4) n = 4;
    if (n > 8) n = 8;
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
        if (dimReady_ && dimState_) {                       // DIMGEN_V1
            if (dimKind_ == 1) fillNetherChunk(*col, pos.x, pos.z);
            else               fillEndChunk(*col, pos.x, pos.z);
        } else if (defaultReady_ && wgState_) {
            fillDefaultChunk(*col, pos.x, pos.z);
        } else if (flatReady_) {
            fillFlatColumn(*col, pos.x, pos.z); // GRASSFIX_V1: фоновые чанки тоже уважают пресет измерения
        }
        // MEM_V5 (ГЛАВНАЯ УТЕЧКА): ChunkColumn::setBlock ставит dirty_ = true на ЛЮБОЙ записи,
        // а ворлдген пишет террайн именно через setBlock. Синхронный путь
        // (getOrGenerateChunk) чистит флаг, а ФОНОВЫЙ ПУЛ — НЕТ. Поэтому КАЖДЫЙ
        // сгенерированный в фоне чанк (т.е. почти все) был "грязным", а pruneChunks
        // грязные не выгружает (WORLD_DIRTY_PIN_V1) -> ОЗУ росло бесконечно и линейно
        // по пройденным координатам. Именно отсюда "бегаешь до 10к — забивает всю память".
        col->clearDirty();
        editSeq_.fetch_add(1, std::memory_order_relaxed); // IDLESAVE_V2
        {
            std::lock_guard<std::mutex> lk(genMutex_);
            genReady_[pos] = std::move(col); // stays in genInFlight_ until taken
        }
    }
}

void World::requestChunkAsync(i32 cx, i32 cz) {
    if (!genStarted_) startGenPool(); // lazily started; startGenPool() itself is guarded by genMutex_
    ChunkPos pos{cx, cz};
    {
        std::lock_guard<std::mutex> lk(chunksMutex_); // RACE_FIX_V1
        if (chunks_.find(pos) != chunks_.end()) return; // already live
    }
    {
        std::lock_guard<std::mutex> lk(genMutex_);
        if (genInFlight_.count(pos) || genReady_.count(pos)) return;
        // MEM_V3: backpressure. Раньше очередь была безграничной: при беге/полёте
        // sendChunksAround заказывал чанки быстрее, чем пул успевал их отдавать,
        // и готовые-но-незабранные колонны (~200 КБ каждая) накапливались сотнями —
        // отсюда резкие выбросы ОЗУ в несколько ГБ. Лишние заказы теперь просто
        // отбрасываются: следующий sendChunksAround закажет их заново.
        // CPU_V2: лимиты подняты под view-distance 16. Колонны теперь палитрованные
        // (MEM_V4) и сразу чистые (MEM_V5), так что 160 готовых — десятки МБ, не ГБ.
        if (genReady_.size() >= 160 || genQueue_.size() >= 512) return;
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
    {
        // CHUNKSYNC_V1: generation may finish after the disk loader (or another
        // connection thread) already installed this coordinate. Never replace a
        // live column: a client may already have received it, and replacement
        // would make world_.getBlock() disagree with the client's collision map.
        std::lock_guard<std::mutex> lk(chunksMutex_);
        auto [it, inserted] = chunks_.emplace(pos, col);
        if (!inserted) col = it->second;
    }
    return col;
}

// GRASSFIX_V1: одна общая заливка плоского чанка — и для синхронной, и для фоновой генерации.
// Раньше genWorkerLoop() игнорировал flatPreset_/flatBiomeId_ и лил траву в Аду и Энде.
void World::fillFlatColumn(ChunkColumn& col, i32 cx, i32 cz) {
    if (flatBiomeId_ >= 0) col.setColumnBiome(flatBiomeId_); // DIMBIOME_V1
    const i32 surface = (flatPreset_ == 1 ? flatNetherrackId_ : (flatPreset_ == 2 ? flatEndStoneId_ : flatGrassId_));
    const i32 filler  = (flatPreset_ == 1 ? flatNetherrackId_ : (flatPreset_ == 2 ? flatEndStoneId_ : flatDirtId_));
    for (i32 lx = 0; lx < 16; ++lx) {
        for (i32 lz = 0; lz < 16; ++lz) {
            const i32 wx = cx * 16 + lx;
            const i32 wz = cz * 16 + lz;
            col.setBlock(wx, 0, wz, flatBedrockId_); // FLATNATIVE_V1: пол Y=0..3, стоим на Y=4
            col.setBlock(wx, 1, wz, filler);
            col.setBlock(wx, 2, wz, filler);
            col.setBlock(wx, 3, wz, surface);
        }
    }
}


// ============================================================
// DIMGEN_V1: Ад и Энд с настоящим рельефом.
// Раньше оба измерения были плоскими (setFlatPreset 1/2): бедрок + три слоя.
// ============================================================
struct World::DimGenState {
    std::unique_ptr<wgdim::NetherGen> nether;
    std::unique_ptr<wgdim::EndGen> end;
};

void World::initNetherGenerator(i64 seed) {
    auto& mgr = registries::RegistryManager::instance();
    dimState_ = std::make_unique<DimGenState>();
    dimState_->nether = std::make_unique<wgdim::NetherGen>((int64_t)seed);
    auto& I = dimState_->nether->ids;
    I.bedrock         = wgResolve(mgr, "minecraft:bedrock", 79);
    I.netherrack      = wgResolve(mgr, "minecraft:netherrack", 4157);
    I.lava            = wgResolve(mgr, "minecraft:lava", 96);
    I.soulSand        = wgResolve(mgr, "minecraft:soul_sand", I.netherrack);
    I.soulSoil        = wgResolve(mgr, "minecraft:soul_soil", I.netherrack);
    I.gravel          = wgResolve(mgr, "minecraft:gravel", I.netherrack);
    I.magma           = wgResolve(mgr, "minecraft:magma_block", I.netherrack);
    I.glowstone       = wgResolve(mgr, "minecraft:glowstone", I.netherrack);
    I.blackstone      = wgResolve(mgr, "minecraft:blackstone", I.netherrack);
    I.basalt          = mgr.getBlockStateId("minecraft:basalt", {{"axis", "y"}}).value_or(I.blackstone);
    I.crimsonNylium   = wgResolve(mgr, "minecraft:crimson_nylium", I.netherrack);
    I.warpedNylium    = wgResolve(mgr, "minecraft:warped_nylium", I.netherrack);
    I.netherGoldOre   = wgResolve(mgr, "minecraft:nether_gold_ore", -1);
    I.quartzOre       = wgResolve(mgr, "minecraft:nether_quartz_ore", -1);
    I.netherWartBlock = wgResolve(mgr, "minecraft:nether_wart_block", -1);
    I.warpedWartBlock = wgResolve(mgr, "minecraft:warped_wart_block", -1);
    I.shroomlight     = wgResolve(mgr, "minecraft:shroomlight", -1);
    dimKind_ = 1;
    dimReady_ = true;
    flatReady_ = false;
    defaultReady_ = false;
    if (langRu_) NC_INFO("World", "Генератор Ада готов (сид {})", (long long)seed);
    else         NC_INFO("World", "Nether generator ready (seed {})", (long long)seed);
}

void World::initEndGenerator(i64 seed) {
    auto& mgr = registries::RegistryManager::instance();
    dimState_ = std::make_unique<DimGenState>();
    dimState_->end = std::make_unique<wgdim::EndGen>((int64_t)seed);
    auto& I = dimState_->end->ids;
    I.endStone = wgResolve(mgr, "minecraft:end_stone", 12456);
    I.obsidian = wgResolve(mgr, "minecraft:obsidian", I.endStone);
    dimKind_ = 2;
    dimReady_ = true;
    flatReady_ = false;
    defaultReady_ = false;
    if (langRu_) NC_INFO("World", "Генератор Энда готов (сид {})", (long long)seed);
    else         NC_INFO("World", "End generator ready (seed {})", (long long)seed);
}

void World::generateDimSpawn(i32 centerX, i32 centerZ, i32 radius) {
    for (i32 cx = centerX - radius; cx <= centerX + radius; ++cx)
        for (i32 cz = centerZ - radius; cz <= centerZ + radius; ++cz)
            getOrGenerateChunk(cx, cz);
}

void World::fillNetherChunk(ChunkColumn& col, i32 cx, i32 cz) {
    auto& G = *dimState_->nether;
    const i32 ROOF = wgdim::NetherGen::ROOF_Y;
    col.setColumnBiome(G.biomeAt(cx * 16 + 8, cz * 16 + 8));
    for (i32 lx = 0; lx < 16; ++lx) {
        for (i32 lz = 0; lz < 16; ++lz) {
            const i32 wx = cx * 16 + lx;
            const i32 wz = cz * 16 + lz;
            const int biome = G.biomeAt(wx, wz);
            int depth = 0;
            bool prevSolid = false; // блок выше текущего (идём сверху вниз)
            for (i32 y = ROOF; y >= 0; --y) {
                const uint64_t bh = wgdim::dimHash(((int64_t)wx << 22) ^ (int64_t)y, wz, G.seed);
                // Бедрок: ровный слой снизу и сверху + рваные края, как в ванилле
                if (y == 0 || y == ROOF) { col.setBlock(wx, y, wz, G.ids.bedrock); prevSolid = true; depth = 0; continue; }
                if (y <= 4 && (bh % 5ull) >= (uint64_t)y) { col.setBlock(wx, y, wz, G.ids.bedrock); prevSolid = true; depth = 0; continue; }
                if (y >= ROOF - 4 && (bh % 5ull) >= (uint64_t)(ROOF - y)) { col.setBlock(wx, y, wz, G.ids.bedrock); prevSolid = true; depth = 0; continue; }
                const bool solid = G.isSolid(wx, y, wz);
                if (solid) {
                    depth = prevSolid ? depth + 1 : 0;
                    i32 id = G.surfaceId(biome, wx, y, wz, depth);
                    if (depth > 3) { const int ore = G.oreAt(wx, y, wz); if (ore >= 0) id = ore; }
                    col.setBlock(wx, y, wz, id);
                } else {
                    // Лавовое море до Y=31 и светящийся камень под потолком
                    if (y <= wgdim::NetherGen::LAVA_Y) col.setBlock(wx, y, wz, G.ids.lava);
                    else if (prevSolid && y > 90 && G.ids.glowstone >= 0 && (bh % 61ull) == 0ull)
                        col.setBlock(wx, y, wz, G.ids.glowstone);
                }
                prevSolid = solid;
            }
        }
    }
}

void World::fillEndChunk(ChunkColumn& col, i32 cx, i32 cz) {
    auto& G = *dimState_->end;
    const double vMid = G.islandValue(cx * 16 + 8, cz * 16 + 8);
    col.setColumnBiome(G.biomeAt(cx * 16 + 8, cz * 16 + 8, vMid));
    for (i32 lx = 0; lx < 16; ++lx) {
        for (i32 lz = 0; lz < 16; ++lz) {
            const i32 wx = cx * 16 + lx;
            const i32 wz = cz * 16 + lz;
            const double v = G.islandValue(wx, wz);
            if (v <= 0.0) continue; // пустота между островами
            for (i32 y = 0; y <= 127; ++y)
                if (G.isSolid(wx, y, wz, v)) col.setBlock(wx, y, wz, G.ids.endStone);
        }
    }
}

void World::initFlatGenerator() {
    auto& mgr = registries::RegistryManager::instance();
    flatBedrockId_ = mgr.getBlockStateId("minecraft:bedrock").value_or(79); // LIGHT_V1
    flatDirtId_    = mgr.getBlockStateId("minecraft:dirt").value_or(10);
    flatGrassId_   = mgr.getBlockStateId("minecraft:grass_block", {{"snowy", "false"}}).value_or(9); // LIGHT_V1
    flatNetherrackId_ = wgResolve(mgr, "minecraft:netherrack", 4157); // MULTIWORLD_V1
    flatEndStoneId_   = wgResolve(mgr, "minecraft:end_stone", 12456); // MULTIWORLD_V1
    flatReady_ = true;
}

std::shared_ptr<ChunkColumn> World::getOrGenerateChunk(i32 cx, i32 cz) {
    // RACE_FIX_V1: a separate chunks_.find() here (before getChunkOrCreate()) used
    // to race with other connection threads inserting into chunks_ concurrently.
    // getChunkOrCreate() now does the find-or-create atomically under one lock and
    // reports whether it created a new column, so we only generate content once.
    bool wasCreated = false;
    auto chunk = getChunkOrCreate(cx, cz, &wasCreated);
    if (!wasCreated) return chunk; // already generated by an earlier call
    editSeq_.fetch_add(1, std::memory_order_relaxed); // IDLESAVE_V2: свежий чанк — мир изменён
    if (dimReady_ && dimState_) { // DIMGEN_V1: Ад/Энд со своим рельефом
        if (dimKind_ == 1) fillNetherChunk(*chunk, cx, cz);
        else               fillEndChunk(*chunk, cx, cz);
        chunk->clearDirty();
        return chunk;
    }
    if (defaultReady_ && wgState_) { // WORLDGEN_V1
        fillDefaultChunk(*chunk, cx, cz);
        chunk->clearDirty(); // generated terrain is baseline, not a live edit
        return chunk;
    }
    if (!flatReady_) return chunk;
    fillFlatColumn(*chunk, cx, cz); // GRASSFIX_V1
    chunk->clearDirty(); // generated flat terrain may be pruned normally
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
    (void)radius; // FLATLOG_V1: стартовый лог убран — с тремя мирами он печатался по три раза
}


// ============================================================
// WORLDSAVE_V1: сохранение мира на диск.
// Формат world/world.dat: "ZEVW" + u8 version + i32 chunkCount,
// затем для каждого чанка: i32 cx, i32 cz, u32 blockCount,
// blockCount * (u8 lx, i16 y, u8 lz, i32 stateId) — только не-воздух.
// ============================================================

// SOFTRELOAD_V1: полная выгрузка мира для мягкого рестарта (вызывать с tick-потока)
void World::reset() {
    // остановить и прибрать фоновую загрузку с диска (иначе повторный startBackgroundLoad убьёт процесс на joinable-потоке)
    loadStop_.store(true);
    if (loadWorker_.joinable()) loadWorker_.join();
    {
        std::lock_guard<std::mutex> lk(loadMutex_);
        loadReady_.clear();
    }
    loadDone_.store(true);
    // очистить очереди генерации (воркеры просто ждут новую работу)
    {
        std::lock_guard<std::mutex> lk(genMutex_);
        genQueue_.clear();
        genInFlight_.clear();
        genReady_.clear();
    }
    // выгрузить все чанки
    {
        std::lock_guard<std::mutex> lk(chunksMutex_);
        chunks_.clear();
    }
    if (langRu_) NC_INFO("World", "Все чанки выгружены (мягкий рестарт)");
    else NC_INFO("World", "All chunks unloaded (soft reload)");
}

bool World::saveToDisk(const std::string& path) const {
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    // ASYNCSAVE_V1: пишем во временный файл и атомарно переименовываем — если
    // сервер упадёт посреди записи, старый world.dat останется целым.
    // IDLESAVE_V1: мир не менялся с прошлого сейва и файл на месте — писать нечего.
    {
        std::error_code ecs;
        if (savedSeq_.load(std::memory_order_relaxed) == editSeq_.load(std::memory_order_relaxed) &&
            std::filesystem::exists(p, ecs)) {
            return true;
        }
    }
    const u64 seqAtStart = editSeq_.load(std::memory_order_relaxed);
    const std::string tmpPath = path + ".tmp";
    std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    // ASYNCSAVE_V1: под мьютексом только копируем shared_ptr на чанки (микросекунды),
    // сериализация ~100к блоков на чанк идёт уже БЕЗ блокировки —
    // игровые потоки не ждут диск (раньше мьютекс держался все ~280мс).
    std::vector<std::shared_ptr<ChunkColumn>> snapshot;
    {
        std::lock_guard<std::mutex> lk(chunksMutex_); // RACE_FIX_V1
        snapshot.reserve(chunks_.size());
        for (const auto& [pos, chunk] : chunks_) snapshot.push_back(chunk);
    }
    std::string raw;
    raw.reserve(snapshot.size() * 16384 + 4); // FASTSAVE_V1: меньше realloc'ов на большом мире
    i32 count = static_cast<i32>(snapshot.size());
    raw.append(reinterpret_cast<const char*>(&count), 4);
    for (const auto& chunk : snapshot) {
        i32 cx = chunk->getX();
        i32 cz = chunk->getZ();
        raw.append(reinterpret_cast<const char*>(&cx), 4);
        raw.append(reinterpret_cast<const char*>(&cz), 4);
        // FASTSAVE_V1: раньше каждый сейв проходил ВСЕ 384 слоя столба (98304 getBlock
        // на чанк, ~43 млн на 441 чанк) через мировые координаты с поиском секции на
        // каждый блок. В плоском мире живая только 1 секция из 24 — остальные
        // просто пропускаем, формат файла не меняется.
        { // FASTSAVE_V2: нетронутая колонна — берём байты прошлого сейва
            std::lock_guard<std::mutex> lk(saveCacheMutex_);
            if (const std::string* cached = chunk->saveBlob()) {
                const u32 cachedCount = chunk->saveBlobCount();
                raw.append(reinterpret_cast<const char*>(&cachedCount), 4);
                raw.append(*cached);
                continue;
            }
        }
        std::string blockData;
        u32 blockCount = 0;
        const ChunkColumn& col = *chunk;
        for (i32 si = 0; si < SECTIONS_PER_CHUNK; ++si) {
            if (!col.sectionHasBlocks(si)) continue;
            const ChunkSection& sec = col.getSection(si);
            const i32 baseY = CHUNK_HEIGHT_MIN + si * SECTION_HEIGHT;
            for (i32 ly = 0; ly < SECTION_HEIGHT; ++ly) {
                const i16 by = static_cast<i16>(baseY + ly);
                for (i32 lx = 0; lx < 16; ++lx) {
                    for (i32 lz = 0; lz < 16; ++lz) {
                        const i32 id = sec.getBlock(lx, ly, lz);
                        if (id == 0) continue;
                        const u8 blx = static_cast<u8>(lx);
                        const u8 blz = static_cast<u8>(lz);
                        blockData.append(reinterpret_cast<const char*>(&blx), 1);
                        blockData.append(reinterpret_cast<const char*>(&by), 2);
                        blockData.append(reinterpret_cast<const char*>(&blz), 1);
                        blockData.append(reinterpret_cast<const char*>(&id), 4);
                        ++blockCount;
                    }
                }
            }
        }
        raw.append(reinterpret_cast<const char*>(&blockCount), 4);
        raw.append(blockData);
        { // FASTSAVE_V2: запоминаем готовые байты до следующей правки этой колонны
            std::lock_guard<std::mutex> lk(saveCacheMutex_);
            chunk->setSaveBlob(std::move(blockData), blockCount);
        }
    }
    const auto compressed = net::zlibc::compress(std::span<const u8>(reinterpret_cast<const u8*>(raw.data()), raw.size()));
    f.write("ZEVW", 4);
    u8 version = 5; // WORLDCOMPRESS_V1
    f.write(reinterpret_cast<const char*>(&version), 1);
    const u32 rawSize = static_cast<u32>(raw.size());
    const u32 compSize = static_cast<u32>(compressed.size());
    f.write(reinterpret_cast<const char*>(&rawSize), 4);
    f.write(reinterpret_cast<const char*>(&compSize), 4);
    f.write(reinterpret_cast<const char*>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
    f.close();
    std::error_code ec;
    if (!f.good()) { std::filesystem::remove(tmpPath, ec); return false; }
    std::filesystem::rename(tmpPath, path, ec); // атомарная подмена (на Windows — MoveFileEx c REPLACE_EXISTING)
    if (ec) { std::filesystem::remove(tmpPath, ec); return false; }
    savedSeq_.store(seqAtStart, std::memory_order_relaxed); // IDLESAVE_V1
    return true;
}

bool World::loadFromDisk(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[4] = {};
    f.read(magic, 4);
    if (!f || std::string_view(magic, 4) != "ZEVW") return false;
    u8 version = 0;
    f.read(reinterpret_cast<char*>(&version), 1);
    if (version != 5) return false; // WORLDCOMPRESS_V1: старые несжатые сейвы (v4 и раньше) отбрасываем — мир пересоздаётся
    u32 rawSize = 0, compSize = 0;
    f.read(reinterpret_cast<char*>(&rawSize), 4);
    f.read(reinterpret_cast<char*>(&compSize), 4);
    if (!f || rawSize > (500u * 1024 * 1024) || compSize > (500u * 1024 * 1024)) return false;
    std::vector<u8> compressed(compSize);
    f.read(reinterpret_cast<char*>(compressed.data()), static_cast<std::streamsize>(compSize));
    if (!f) return false;
    std::vector<u8> raw;
    if (!net::zlibc::decompress(std::span<const u8>(compressed.data(), compressed.size()), rawSize, raw)) return false;

    size_t off = 0;
    auto readRaw = [&](void* dst, size_t n) -> bool {
        if (off + n > raw.size()) return false;
        std::memcpy(dst, raw.data() + off, n);
        off += n;
        return true;
    };
    i32 count = 0;
    if (!readRaw(&count, 4) || count < 0 || count > 100000) return false;
    for (i32 c = 0; c < count; ++c) {
        i32 cx = 0, cz = 0;
        u32 blockCount = 0;
        if (!readRaw(&cx, 4) || !readRaw(&cz, 4) || !readRaw(&blockCount, 4)) return false;
        auto chunk = getChunkOrCreate(cx, cz);
        for (u32 i = 0; i < blockCount; ++i) {
            u8 lx = 0, lz = 0;
            i16 y = 0;
            i32 id = 0;
            if (!readRaw(&lx, 1) || !readRaw(&y, 2) || !readRaw(&lz, 1) || !readRaw(&id, 4)) return false;
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
    if (version != 5) return false; // WORLDCOMPRESS_V1
    u32 rawSize = 0, compSize = 0;
    f.read(reinterpret_cast<char*>(&rawSize), 4);
    f.read(reinterpret_cast<char*>(&compSize), 4);
    if (!f || rawSize > (500u * 1024 * 1024) || compSize > (500u * 1024 * 1024)) return false;
    std::vector<u8> compressed(compSize);
    f.read(reinterpret_cast<char*>(compressed.data()), static_cast<std::streamsize>(compSize));
    if (!f) return false;
    std::vector<u8> raw;
    if (!net::zlibc::decompress(std::span<const u8>(compressed.data(), compressed.size()), rawSize, raw)) return false;
    if (raw.size() < 4) return false;
    i32 count = 0; std::memcpy(&count, raw.data(), 4);
    if (count < 0 || count > 100000) return false;
    // Header is valid; hand the heavy per-block parsing to a background thread so
    // the server can start listening immediately.
    loadExpected_ = count;
    loadDone_.store(false);
    loadStop_.store(false);
    loadWorker_ = std::thread([this, raw = std::move(raw), count]() mutable {
        size_t off = 4;
        auto readRaw = [&](void* dst, size_t n) -> bool {
            if (off + n > raw.size()) return false;
            std::memcpy(dst, raw.data() + off, n);
            off += n;
            return true;
        };
        for (i32 c = 0; c < count && !loadStop_.load(); ++c) {
            i32 cx = 0, cz = 0; u32 blockCount = 0;
            if (!readRaw(&cx, 4) || !readRaw(&cz, 4) || !readRaw(&blockCount, 4)) break;
            auto col = std::make_shared<ChunkColumn>(cx, cz);
            for (u32 i = 0; i < blockCount; ++i) {
                u8 lx = 0, lz = 0; i16 y = 0; i32 id = 0;
                if (!readRaw(&lx, 1) || !readRaw(&y, 2) || !readRaw(&lz, 1) || !readRaw(&id, 4)) { loadDone_.store(true); return; }
                col->setBlock(cx * 16 + lx, static_cast<i32>(y), cz * 16 + lz, id);
            }
            col->clearDirty();
            { std::lock_guard<std::mutex> lk(loadMutex_); loadReady_.push_back(std::move(col)); }
        }
        loadDone_.store(true);
    });
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
    std::lock_guard<std::mutex> lk(chunksMutex_); // RACE_FIX_V1
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
