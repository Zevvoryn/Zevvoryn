#pragma once

#include "../core/types.hpp"
#include "../network/buffer.hpp"
#include <array>
#include <vector>
#include <optional>
#include <memory>
#include <string>
#include <unordered_map> // PERF_ASYNC_V1
#include <unordered_set> // PERF_ASYNC_V1
#include <deque>         // PERF_ASYNC_V1
#include <thread>        // PERF_ASYNC_V1
#include <mutex>         // PERF_ASYNC_V1
#include <condition_variable> // PERF_ASYNC_V1
#include <atomic>        // PERF_ASYNC_V1

namespace nc::world {

// ============================================================
// Чанк-секция (16x16x16 блоков)
// В 1.21.1: palette-based block storage + biomes palette.
// ============================================================

static constexpr int SECTION_WIDTH  = 16;
static constexpr int SECTION_HEIGHT = 16;
static constexpr int SECTIONS_PER_CHUNK = 24; // Y: -64 до 320 = 384 блока = 24 секции
static constexpr int CHUNK_HEIGHT_MIN = -64;
static constexpr int CHUNK_HEIGHT_MAX = 320;
static constexpr int BLOCKS_PER_SECTION = SECTION_WIDTH * SECTION_WIDTH * SECTION_HEIGHT;
static constexpr int BIOMES_PER_SECTION = 4 * 4 * 4; // 64 биомы на секцию (4x4x4 блока = 1 биом)

// ============================================================
// Простой inline palette для блоков (без бит-пакинга — для старта)
// ============================================================

class ChunkSection {
public:
    ChunkSection();

    void setBlock(i32 x, i32 y, i32 z, i32 stateId);
    i32 getBlock(i32 x, i32 y, i32 z) const;
    i32 getNonAirBlockCount() const { return nonAirCount_; }

    // Установить биом для 4x4x4 кластера
    void setBiome(i32 x, i32 y, i32 z, i32 biomeId);
    i32 getBiome(i32 x, i32 y, i32 z) const;

    // Сериализация для chunk data packet
    void writeTo(net::Buffer& buf, i32 biomeOverride = -1) const; // DIMBIOME_V1
    // MEM_V1: serialize an all-air section (used for unallocated/lazy sections).
    static void writeEmpty(net::Buffer& buf, i32 biomeOverride = -1); // DIMBIOME_V1

private:
    // MEM_V4: палитра + бит-пакинг вместо плоского массива.
    // Было (MEM_V1): std::array<u16, 4096> = 8 КБ на секцию ВСЕГДА, даже если
    // секция целиком из камня. Колонна с рельефом — ~13 живых секций = ~110 КБ.
    // Плоский мир трогает ОДНУ секцию из 24 — поэтому там 24 МБ, а в обычном мире 500.
    // Стало: однородная секция (весь камень/весь воздух) = 0 байт данных,
    // типичная подземная (камень/сланец/воздух/руды, <=16 состояний) = 4 бита
    // на блок = 2 КБ. Палитра растёт 4->5->6->7->8 бит, дальше direct 15 бит.
    // Формат совпадает с сетевым paletted container 1.21.1, поэтому writeTo()
    // льёт внутреннее представление как есть, без переупаковки в 15 бит.
    static constexpr u8 DIRECT_BITS = 15;

    std::vector<u16> palette_;  // сетевые state id; пустая = direct-палитра
    std::vector<u64> data_;     // упакованные индексы; значения НЕ пересекают long
    u8 bits_ = 0;               // 0 = вся секция равна palette_[0]
    std::array<u16, BIOMES_PER_SECTION> biomes_;
    i32 nonAirCount_ = 0;

    static size_t longsFor(u8 bits);
    i32 stateAt(size_t idx) const;
    u32 rawAt(size_t idx) const;
    void setRaw(size_t idx, u32 value);
    void repack(u8 newBits, const std::vector<u16>& newPalette);

    static size_t blockIndex(i32 x, i32 y, i32 z) {
        return static_cast<size_t>((y & 0xF) * SECTION_WIDTH * SECTION_WIDTH + (z & 0xF) * SECTION_WIDTH + (x & 0xF));
    }

    static size_t biomeIndex(i32 x, i32 y, i32 z) {
        return static_cast<size_t>(((y >> 2) & 0x3) * 16 + ((z >> 2) & 0x3) * 4 + ((x >> 2) & 0x3));
    }
};

// ============================================================
// Чанк колонна (16x384x16 блоков)
// ============================================================

class ChunkColumn {
public:
    explicit ChunkColumn(i32 x, i32 z);

    i32 getX() const { return x_; }
    i32 getZ() const { return z_; }

    ChunkSection& getSection(i32 index);
    const ChunkSection& getSection(i32 index) const;

    void setBlock(i32 x, i32 y, i32 z, i32 stateId);
    i32 getBlock(i32 x, i32 y, i32 z) const;

    // Секция по мировой Y координате
    i32 sectionIndex(i32 worldY) const { return (worldY - CHUNK_HEIGHT_MIN) / SECTION_HEIGHT; }

    // Записать полный chunk data payload для 1.21.1
    void writeTo(net::Buffer& buf, bool includeBiomes = true) const;

    // FASTSAVE_V1: есть ли в секции хоть один не-воздушный блок.
    // Важно: НЕ трогает getSection(), который лениво создаёт секцию.
    bool sectionHasBlocks(i32 index) const {
        if (index < 0 || index >= SECTIONS_PER_CHUNK) return false;
        const auto& sec = sections_[static_cast<size_t>(index)];
        return sec && sec->getNonAirBlockCount() > 0;
    }

    // Были ли изменения с момента последней отправки
    bool isDirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    // FASTSAVE_V2: кэш сериализованных блоков для saveToDisk(). Колонна, которую никто
    // не трогал с прошлого сейва, больше не перебирается поблочно.
    const std::string* saveBlob() const { return saveBlobValid_ ? &saveBlob_ : nullptr; }
    u32 saveBlobCount() const { return saveBlobCount_; }
    void setSaveBlob(std::string blob, u32 count) const {
        saveBlob_ = std::move(blob);
        saveBlobCount_ = count;
        saveBlobValid_ = true;
    }

    // DIMBIOME_V1: весь столбец одним биомом (Ад/Энд); -1 = как было
    void setColumnBiome(i32 b) { biomeId_ = b; }
    i32 columnBiome() const { return biomeId_; }

private:
    i32 x_, z_;
    std::array<std::unique_ptr<ChunkSection>, SECTIONS_PER_CHUNK> sections_;
    bool dirty_ = false;
    i32 biomeId_ = -1; // DIMBIOME_V1
    mutable std::string saveBlob_;      // FASTSAVE_V2
    mutable u32 saveBlobCount_ = 0;     // FASTSAVE_V2
    mutable bool saveBlobValid_ = false; // FASTSAVE_V2
};

// ============================================================
// Мир
// ============================================================

class World {
public:
    World(); // WORLDGEN_FIX_V1 (out-of-line: WorldGenState неполный тип)
    ~World(); // WORLDGEN_V1

    std::shared_ptr<ChunkColumn> getChunk(i32 x, i32 z);
    // RACE_FIX_V1: optional out-param reports whether this call created a brand-new
    // (empty) column vs returned an already-live one, so callers like
    // getOrGenerateChunk() can decide whether to fill it in — all in one atomic
    // lock instead of a separate find() + getChunkOrCreate() race.
    std::shared_ptr<ChunkColumn> getChunkOrCreate(i32 x, i32 z, bool* wasCreated = nullptr);

    void setBlock(i32 x, i32 y, i32 z, i32 stateId);
    i32 getBlock(i32 x, i32 y, i32 z) const;

    void unloadChunk(i32 x, i32 z);
    // MEM_V1: drop loaded chunks far from every keep-center so RAM does not grow
    // without bound as players roam. keepRadius is in chunks. Centers are (cx,cz).
    void pruneChunks(const std::vector<std::pair<i32, i32>>& keepCenters, i32 keepRadius);

    size_t getLoadedChunkCount() const { std::lock_guard<std::mutex> lk(chunksMutex_); return chunks_.size(); }

    // ANVIL_CONVERT_V1: read-only iteration over all currently loaded chunks,
    // used by the vanilla Anvil world exporter/importer.
    const std::unordered_map<ChunkPos, std::shared_ptr<ChunkColumn>>& getAllChunks() const { return chunks_; }

    // PERF_TUNE_V1: override worldgen worker-thread count (0 = auto). Set from the
    // config max-cores value before the pool lazily starts.
    void setGenThreadCount(unsigned n) { genThreadOverride_ = n; }

    // Генерация flat-мира
    void generateFlat(i64 seed, i32 centerX, i32 centerZ, i32 radius);
    // MULTIWORLD_V1: 0 = overworld superflat, 1 = nether flat, 2 = end flat
    void setFlatPreset(i32 preset) { flatPreset_ = preset; flatBiomeId_ = (preset == 1 ? 34 : (preset == 2 ? 55 : -1)); } // DIMBIOME_V1: nether_wastes / the_end

    // DIMGEN_V1: настоящая генерация Ада/Энда вместо плоских пресетов.
    // dimKind_: 1 = Ад, 2 = Энд. Выбирается раньше flat- и overworld-путей.
    void initNetherGenerator(i64 seed);
    void initEndGenerator(i64 seed);
    void generateDimSpawn(i32 centerX, i32 centerZ, i32 radius);
    bool dimReady() const { return dimReady_; }
    i32  dimKind() const { return dimKind_; }

    // WORLDSAVE_V1: сохранение/загрузка мира на диск
    bool saveToDisk(const std::string& path) const;
    // SOFTRELOAD_V1: выгрузить ВСЕ чанки и очереди генерации/загрузки.
    // Потоки пула генерации не останавливаются — они без состояния и ждут новую работу.
    void reset();
    bool loadFromDisk(const std::string& path);

    // FASTBOOT_V1: read the save header on the calling (main) thread and return
    // true if a valid save exists, then stream the chunk bodies from disk on a
    // background thread so startup does not block. Parsed columns are handed off
    // via a queue and installed by drainLoadedChunks() on the main thread only,
    // so chunks_ stays single-threaded (no lock needed on it).
    bool startBackgroundLoad(const std::string& path);
    // FASTBOOT_V1: main thread only. Install background-parsed columns into the
    // live world (insert-if-absent so live/edited chunks are never clobbered).
    void drainLoadedChunks();
    bool isBackgroundLoadDone() const { return loadDone_.load(); }

    // FLATWORLD_V1: генерация чанков на лету (бесконечный flat-мир)
    void initFlatGenerator();
    std::shared_ptr<ChunkColumn> getOrGenerateChunk(i32 x, i32 z);

    // PERF_ASYNC_V1: background chunk-generation thread pool (parallel worldgen).
    void startGenPool();
    void stopGenPool();
    // Enqueue (cx,cz) for background generation unless it is already live, pending, or ready.
    void requestChunkAsync(i32 cx, i32 cz);
    // True if (cx,cz) is queued, generating, or generated-but-not-yet-collected.
    bool isChunkPending(i32 cx, i32 cz);
    // If the pool finished (cx,cz), install it into the live world and return it; else nullptr. Main thread only.
    std::shared_ptr<ChunkColumn> takeReadyChunk(i32 cx, i32 cz);
    bool flatReady_ = false;
    i32 flatBedrockId_ = 79; // LIGHT_V1
    i32 flatDirtId_ = 10;
    i32 flatGrassId_ = 9;  // LIGHT_V1
    i32 flatPreset_ = 0;           // MULTIWORLD_V1
    struct DimGenState;            // DIMGEN_V1
    std::unique_ptr<DimGenState> dimState_; // DIMGEN_V1
    bool dimReady_ = false;        // DIMGEN_V1
    i32  dimKind_ = 0;             // DIMGEN_V1: 1 = nether, 2 = end
    i32 flatBiomeId_ = -1;         // DIMBIOME_V1
    i32 flatNetherrackId_ = 4157;  // MULTIWORLD_V1: resolved in initFlatGenerator
    i32 flatEndStoneId_ = 12456;   // MULTIWORLD_V1: resolved in initFlatGenerator

    // WORLDGEN_V1: ванильный overworld генератор
    void initDefaultGenerator(i64 seed, bool ru = false); // SPAWN_V1
    void generateDefault(i64 seed, i32 centerX, i32 centerZ, i32 radius, bool ru = false); // SPAWN_V1
    void setLanguageRu(bool ru) { langRu_ = ru; } // LANGFIX_V1: язык логов мира
    bool langRu_ = false; // LANGFIX_V1
    bool findWorldSpawn(i64 seed, i32& outX, i32& outY, i32& outZ); // SPAWN_V1
    bool defaultReady_ = false;
    struct WorldGenState;
    std::unique_ptr<WorldGenState> wgState_;
    // STRUCT_LOCATE_V1: биом в точке (-1, если генератор не ванильный).
    i32 biomeAtBlock(i32 wx, i32 wy, i32 wz);
    // STRUCT_LOCATE_V1: поиск ближайшей структуры только по арифметике сетки
    // и шуму биомов: ни одного чанка не генерируется.
    bool locateStructure(const std::string& key, i32 fromCx, i32 fromCz, i32 maxRadiusChunks,
                         i32& outX, i32& outZ, std::string& outNameRu, std::string& outNameEn);
    static std::string structureKeys();
    void fillDefaultChunk(ChunkColumn& chunk, i32 cx, i32 cz);

private:
    // RACE_FIX_V1: handlePlay() runs on each connection's OWN thread (one thread
    // per player, see network::Server accept loop), NOT the main tick thread.
    // sendChunksAround() therefore calls getOrGenerateChunk()/takeReadyChunk()
    // concurrently from many player threads at once. Both mutate chunks_, so
    // without a lock this was a data race on a std::unordered_map (concurrent
    // insert/rehash from multiple threads = undefined behavior: corruption,
    // infinite loops in a broken bucket chain, or crashes) — exactly what a
    // mass join/churn burst with 300 bots would trigger. All access to chunks_
    // now goes through chunksMutex_.
    mutable std::mutex chunksMutex_;
    std::unordered_map<ChunkPos, std::shared_ptr<ChunkColumn>> chunks_;

    // IDLESAVE_V1: счётчик правок мира. Автосейв каждые 5 минут молотил полную
    // сериализацию + сжатие, даже когда в мире не менялся ни один блок.
    mutable std::mutex saveCacheMutex_; // FASTSAVE_V2: два параллельных сейва не дерут кэш
    std::atomic<u64> editSeq_{0};
    mutable std::atomic<u64> savedSeq_{~0ull};
public:
    u64 editSeq() const { return editSeq_.load(std::memory_order_relaxed); }
    void markEdited() { editSeq_.fetch_add(1, std::memory_order_relaxed); }
    bool saveUpToDate() const { return savedSeq_.load(std::memory_order_relaxed) == editSeq(); }
private:

    // PERF_ASYNC_V1: background generation pool state.
    void fillFlatColumn(ChunkColumn& col, i32 cx, i32 cz); // GRASSFIX_V1
    void fillNetherChunk(ChunkColumn& col, i32 cx, i32 cz); // DIMGEN_V1
    void fillEndChunk(ChunkColumn& col, i32 cx, i32 cz);    // DIMGEN_V1
    void genWorkerLoop();
    std::vector<std::thread> genWorkers_;
    std::mutex genMutex_;
    std::condition_variable genCv_;
    std::deque<ChunkPos> genQueue_;
    std::unordered_set<ChunkPos> genInFlight_; // queued, generating, or ready-not-taken
    std::unordered_map<ChunkPos, std::shared_ptr<ChunkColumn>> genReady_;
    std::atomic<bool> genStop_{false};
    bool genStarted_ = false;
    unsigned genThreadOverride_ = 0; // PERF_TUNE_V1: 0 = auto-detect

    // FASTBOOT_V1: background disk-load handoff. The worker parses chunk bodies
    // off-thread into standalone columns and pushes them here; the main thread
    // installs them via drainLoadedChunks(). chunks_ stays main-thread-only.
    void loadWorkerLoop(std::string path, i32 count);
    std::thread loadWorker_;
    std::mutex loadMutex_;
    std::deque<std::shared_ptr<ChunkColumn>> loadReady_;
    std::atomic<bool> loadDone_{false};
    std::atomic<bool> loadStop_{false};
    i32 loadExpected_ = 0;
};

} // namespace nc::world
