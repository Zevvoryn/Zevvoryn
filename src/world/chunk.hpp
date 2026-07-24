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
    void writeTo(net::Buffer& buf) const;
    // MEM_V1: serialize an all-air section (used for unallocated/lazy sections).
    static void writeEmpty(net::Buffer& buf);

private:
    // MEM_V1: block/biome state ids fit in 16 bits (max ~27k states in 1.21.1),
    // so store as u16 instead of i32 — halves per-section RAM (16KB -> 8KB).
    std::array<u16, BLOCKS_PER_SECTION> blocks_;
    std::array<u16, BIOMES_PER_SECTION> biomes_;
    i32 nonAirCount_ = 0;

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

    // Были ли изменения с момента последней отправки
    bool isDirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

private:
    i32 x_, z_;
    std::array<std::unique_ptr<ChunkSection>, SECTIONS_PER_CHUNK> sections_;
    bool dirty_ = false;
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

    // WORLDGEN_V1: ванильный overworld генератор
    void initDefaultGenerator(i64 seed, bool ru = false); // SPAWN_V1
    void generateDefault(i64 seed, i32 centerX, i32 centerZ, i32 radius, bool ru = false); // SPAWN_V1
    void setLanguageRu(bool ru) { langRu_ = ru; } // LANGFIX_V1: язык логов мира
    bool langRu_ = false; // LANGFIX_V1
    bool findWorldSpawn(i64 seed, i32& outX, i32& outY, i32& outZ); // SPAWN_V1
    bool defaultReady_ = false;
    struct WorldGenState;
    std::unique_ptr<WorldGenState> wgState_;
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

    // PERF_ASYNC_V1: background generation pool state.
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
