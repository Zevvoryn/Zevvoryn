// ANVIL_CONVERT_V1: converter between world.dat and vanilla Anvil saves.
//
// Anvil format quick reference (Minecraft Java Edition, 1.21.1 / DataVersion 3955):
//  - A region file `region/r.<rx>.<rz>.mca` covers a 32x32 grid of chunks.
//    Header = 4096-byte location table (4 bytes/chunk: 3-byte sector offset +
//    1-byte sector count) + 4096-byte timestamp table (4 bytes/chunk).
//    Each stored chunk = 4-byte big-endian length + 1-byte compression type
//    (1=gzip, 2=zlib, 3=uncompressed) + payload, padded to whole 4096-byte
//    sectors.
//  - Chunk NBT root is an unnamed (empty-name) TAG_Compound with DataVersion,
//    xPos/zPos/yPos, Status, sections (list of Y=-4..19 compounds each with
//    block_states{palette,data} and biomes{palette,data}), block_entities, etc.
//  - level.dat is a gzip-compressed named NBT file: root "" -> "Data" compound
//    with world metadata + WorldGenSettings (seed/dimensions/generator).
//
// See world/anvil.hpp for the fidelity limitations of this converter.

#include "anvil.hpp"
#include "chunk.hpp"
#include "biomegen.hpp" // BIOMEGEN_V1
#include "../utils/nbt.hpp"
#include "../network/zlib_codec.hpp"
#include "../registries/registry.hpp"
#include "../core/item_blocks.gen.hpp" // BLOCKMAP_V1
#include "../core/items.gen.hpp"    // BLOCKENT_V1
#include "worldextra.hpp"              // BLOCKENT_V1
#include "../entity/mob.hpp"           // BLOCKENT_V1
#include "../core/log.hpp"
#include "../core/crash_trace.hpp" // NC_CTRACE

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set> // SAVEFAST_V1
#include <mutex>         // SAVEFAST_V1
#include <span>          // SAVEFAST_V1
#include <algorithm>
#include <cstring>
#include <cmath>

namespace nc::world::anvil {

namespace fs = std::filesystem;
using nc::registries::RegistryManager;

static constexpr i32 DATA_VERSION_1_21_1 = 3955;

// ------------------------------------------------------------------
// Bit packing for palette-index arrays (post-1.16 layout: entries never
// span across a long boundary).
// ------------------------------------------------------------------

static int bitsForPalette(size_t paletteSize, int minBits) {
    int bits = 0;
    while ((size_t(1) << bits) < paletteSize) ++bits;
    return std::max(bits, minBits);
}

static std::vector<i64> packIndices(const std::vector<i32>& indices, int bits) {
    if (bits <= 0) return {};
    const int entriesPerLong = 64 / bits;
    const size_t longCount = (indices.size() + size_t(entriesPerLong) - 1) / size_t(entriesPerLong);
    std::vector<i64> out(longCount, 0);
    for (size_t i = 0; i < indices.size(); ++i) {
        size_t longIdx = i / size_t(entriesPerLong);
        int bitIdx = int(i % size_t(entriesPerLong)) * bits;
        u64 mask = (bits >= 64) ? ~u64(0) : ((u64(1) << bits) - 1);
        u64 v = u64(indices[i]) & mask;
        u64 cur = u64(out[longIdx]);
        cur |= (v << bitIdx);
        out[longIdx] = i64(cur);
    }
    return out;
}

static std::vector<i32> unpackIndices(const std::vector<i64>& longs, int bits, size_t count) {
    std::vector<i32> out(count, 0);
    if (bits <= 0) return out;
    const int entriesPerLong = 64 / bits;
    u64 mask = (bits >= 64) ? ~u64(0) : ((u64(1) << bits) - 1);
    for (size_t i = 0; i < count; ++i) {
        size_t longIdx = i / size_t(entriesPerLong);
        if (longIdx >= longs.size()) break;
        int bitIdx = int(i % size_t(entriesPerLong)) * bits;
        u64 v = (u64(longs[longIdx]) >> bitIdx) & mask;
        out[i] = i32(v);
    }
    return out;
}

// ------------------------------------------------------------------
// Region file writer: sequential append-only allocator. Never reuses freed
// sectors, which is valid (if slightly wasteful) per the Anvil spec -- we
// always regenerate whole regions during export, never patch in place.
// ------------------------------------------------------------------

class RegionWriter {
public:
    RegionWriter() { data_.assign(8192, 0); }

    // compressionType: 2 = zlib (what we always write).
    void putChunk(int localX, int localZ, const std::vector<u8>& compressed, u8 compressionType, u32 timestamp) {
        u32 payloadLen = static_cast<u32>(compressed.size()) + 1; // +1 for compression-type byte
        u32 totalLen = 4 + payloadLen;
        u32 sectors = (totalLen + 4095) / 4096;
        u32 sectorOffset = static_cast<u32>(data_.size() / 4096);
        data_.resize(data_.size() + size_t(sectors) * 4096, 0);
        size_t w = size_t(sectorOffset) * 4096;
        data_[w + 0] = u8((payloadLen >> 24) & 0xFF);
        data_[w + 1] = u8((payloadLen >> 16) & 0xFF);
        data_[w + 2] = u8((payloadLen >> 8) & 0xFF);
        data_[w + 3] = u8(payloadLen & 0xFF);
        data_[w + 4] = compressionType;
        std::memcpy(&data_[w + 5], compressed.data(), compressed.size());

        int idx = localX + localZ * 32;
        size_t locOff = size_t(idx) * 4;
        data_[locOff + 0] = u8((sectorOffset >> 16) & 0xFF);
        data_[locOff + 1] = u8((sectorOffset >> 8) & 0xFF);
        data_[locOff + 2] = u8(sectorOffset & 0xFF);
        data_[locOff + 3] = u8(sectors > 255 ? 255 : sectors);
        size_t tsOff = 4096 + size_t(idx) * 4;
        data_[tsOff + 0] = u8((timestamp >> 24) & 0xFF);
        data_[tsOff + 1] = u8((timestamp >> 16) & 0xFF);
        data_[tsOff + 2] = u8((timestamp >> 8) & 0xFF);
        data_[tsOff + 3] = u8(timestamp & 0xFF);
    }

    bool save(const std::string& path) const {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data_.data()), static_cast<std::streamsize>(data_.size()));
        return f.good();
    }

private:
    std::vector<u8> data_;
};

class RegionReader {
public:
    bool load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        f.seekg(0, std::ios::end);
        auto sz = f.tellg();
        if (sz < 8192) return false;
        data_.resize(static_cast<size_t>(sz));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(data_.data()), sz);
        return static_cast<bool>(f) || f.eof();
    }

    bool getChunk(int localX, int localZ, std::vector<u8>& outUncompressed) const {
        int idx = localX + localZ * 32;
        size_t locOff = size_t(idx) * 4;
        if (locOff + 4 > data_.size()) return false;
        u32 sectorOffset = (u32(data_[locOff]) << 16) | (u32(data_[locOff + 1]) << 8) | u32(data_[locOff + 2]);
        u32 sectorCount = data_[locOff + 3];
        if (sectorOffset == 0 || sectorCount == 0) return false; // not generated
        size_t byteOffset = size_t(sectorOffset) * 4096;
        if (byteOffset + 5 > data_.size()) return false;
        u32 length = (u32(data_[byteOffset]) << 24) | (u32(data_[byteOffset + 1]) << 16)
                   | (u32(data_[byteOffset + 2]) << 8) | u32(data_[byteOffset + 3]);
        if (length < 1) return false;
        u8 compressionType = data_[byteOffset + 4];
        size_t dataLen = size_t(length) - 1;
        if (byteOffset + 5 + dataLen > data_.size()) return false;
        const u8* payload = &data_[byteOffset + 5];
        constexpr size_t kMaxChunkBytes = 16 * 1024 * 1024;
        if (compressionType == 2) { // zlib
            if (dataLen < 2) return false;
            outUncompressed.clear();
            return net::zlibc::inflate(payload + 2, dataLen - 2, outUncompressed, kMaxChunkBytes);
        }
        if (compressionType == 3) { // uncompressed
            outUncompressed.assign(payload, payload + dataLen);
            return true;
        }
        if (compressionType == 1) { // legacy gzip
            return net::zlibc::gzipDecompress(std::span<const u8>(payload, dataLen), outUncompressed, kMaxChunkBytes);
        }
        return false;
    }

private:
    std::vector<u8> data_;
};

// ------------------------------------------------------------------
// Block name <-> state id via the existing registry (see registries/registry.hpp).
// Only names actually registered there survive the round trip; that
// currently covers exactly what the FLAT world generator produces.
// ------------------------------------------------------------------

// BLOCKMAP_V1: full 1.21.1 block table for the round trip.
//
// The registry only registers property variants for a subset of blocks, so
// looking a state id up there alone used to answer "minecraft:air" for almost
// everything and every exported world came out empty. item_blocks.gen.hpp
// carries all 1331 blocks with their vanilla default state id, so any state id
// can at least be resolved to the block it belongs to: find the nearest entry
// whose default id is <= the state id.
//
// Caveat kept on purpose: for a block whose default state is not its lowest
// state (grass_block, doors, etc.) the few states below the default resolve to
// the previous block in id order. Exact variants registered in the registry are
// always preferred, so those blocks round trip correctly.
namespace {

struct StateNameTable {
    std::vector<std::pair<i32, std::string>> byId; // sorted by default state id
    StateNameTable() {
        const auto& m = nc::gen::blockStateByName();
        byId.reserve(m.size() + 1);
        byId.emplace_back(0, std::string("minecraft:air"));
        for (const auto& [shortName, defaultState] : m)
            byId.emplace_back(static_cast<i32>(defaultState), "minecraft:" + shortName);
        std::sort(byId.begin(), byId.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    }
};

const StateNameTable& stateNameTable() {
    static const StateNameTable table;
    return table;
}

} // namespace

static std::string stateIdToName(i32 stateId) {
    if (stateId <= 0) return std::string("minecraft:air");
    const auto* st = RegistryManager::instance().blockStates().getById(stateId);
    if (st && !st->name.empty()) return st->name;
    const auto& byId = stateNameTable().byId;
    auto it = std::upper_bound(byId.begin(), byId.end(), stateId,
        [](i32 value, const std::pair<i32, std::string>& e) { return value < e.first; });
    if (it == byId.begin()) return std::string("minecraft:air");
    --it;
    return it->second;
}

// BLOCKMAP_V1: block state properties (facing, half, waterlogged, ...) for the
// exported palette. Only states registered with variants have them.
static const std::unordered_map<std::string, std::string>* stateProperties(i32 stateId) {
    const auto* st = RegistryManager::instance().blockStates().getById(stateId);
    if (st && !st->properties.empty()) return &st->properties;
    return nullptr;
}

static i32 nameToStateId(const std::string& name,
                         const std::unordered_map<std::string, std::string>& props = {}) {
    if (name == "minecraft:air" || name == "minecraft:cave_air" || name == "minecraft:void_air") return 0;
    auto& reg = RegistryManager::instance();
    if (!props.empty()) {
        if (auto id = reg.getBlockStateId(name, props)) return *id; // exact variant
    }
    if (auto id = reg.getBlockStateId(name)) return *id;            // default state
    // BLOCKMAP_V1: not in the registry, but the generated table knows the block.
    if (name.rfind("minecraft:", 0) == 0) {
        const auto& m = nc::gen::blockStateByName();
        auto it = m.find(name.substr(10));
        if (it != m.end()) return static_cast<i32>(it->second);
    }
    return 0; // truly unknown (modded/newer block) -> air
}

// ------------------------------------------------------------------
// Chunk NBT: export (our ChunkColumn -> vanilla chunk NBT bytes)
// ------------------------------------------------------------------

// ------------------------------------------------------------------
// BLOCKENT_V1: block entities (chests, signs) inside chunk NBT.
//
// Until now the converter wrote an empty block_entities list because the core
// kept chest contents and sign text in RAM only. WORLDEXTRA_V1 persists them,
// so both directions can finally carry them.
// ------------------------------------------------------------------

struct ChunkExtras {
    std::vector<const extra::ChestRecord*> chests;
    std::vector<const extra::SignRecord*> signs;
    std::unordered_map<i64, const extra::FurnaceRecord*> furnaceByPos; // CONTAINER_V3/BLOCKENT_V2: keyed by packPos, matches a furnace-kind ChestRecord
    std::vector<const extra::SpawnerRecord*> spawners; // SPAWNER_V1/BLOCKENT_V2
    std::vector<const extra::BannerRecord*> banners;   // BANNER_V1/BLOCKENT_V2
};

using ExtrasByChunk = std::unordered_map<i64, ChunkExtras>;

static i64 chunkKeyOf(i32 cx, i32 cz) {
    return (static_cast<i64>(cx) << 32) | static_cast<u32>(cz);
}

static ExtrasByChunk groupExtras(const extra::Snapshot& snap) {
    ExtrasByChunk out;
    for (const extra::ChestRecord& c : snap.chests) {
        i32 x = 0, y = 0, z = 0;
        extra::unpackPos(c.pos, x, y, z);
        out[chunkKeyOf(x >> 4, z >> 4)].chests.push_back(&c);
    }
    for (const extra::SignRecord& s : snap.signs) {
        i32 x = 0, y = 0, z = 0;
        extra::unpackPos(s.pos, x, y, z);
        out[chunkKeyOf(x >> 4, z >> 4)].signs.push_back(&s);
    }
    for (const extra::FurnaceRecord& f : snap.furnaces) { // CONTAINER_V3/BLOCKENT_V2
        i32 x = 0, y = 0, z = 0;
        extra::unpackPos(f.pos, x, y, z);
        out[chunkKeyOf(x >> 4, z >> 4)].furnaceByPos[f.pos] = &f;
    }
    for (const extra::SpawnerRecord& sp : snap.spawners) { // SPAWNER_V1/BLOCKENT_V2
        i32 x = 0, y = 0, z = 0;
        extra::unpackPos(sp.pos, x, y, z);
        out[chunkKeyOf(x >> 4, z >> 4)].spawners.push_back(&sp);
    }
    for (const extra::BannerRecord& b : snap.banners) { // BANNER_V1/BLOCKENT_V2
        i32 x = 0, y = 0, z = 0;
        extra::unpackPos(b.pos, x, y, z);
        out[chunkKeyOf(x >> 4, z >> 4)].banners.push_back(&b);
    }
    return out;
}

// BLOCKENT_V2: before this, every container in `chests_` (barrels, furnaces,
// hoppers, dispensers, droppers, ender chests) was exported as a plain
// chest/trapped_chest because the writer only looked at ChestRecord::trapped.
// `kind` mirrors core/server.cpp's ContainerKind enum (CK_NONE=0, CK_CHEST=1,
// CK_TRAPPED=2, CK_ENDER=3, CK_BARREL=4, CK_FURNACE=5, CK_BLAST_FURNACE=6,
// CK_SMOKER=7, CK_HOPPER=8, CK_DISPENSER=9, CK_DROPPER=10).
static const char* chestBlockEntityId(i32 kind) {
    switch (kind) {
        case 2:  return "minecraft:trapped_chest";
        case 3:  return "minecraft:ender_chest";
        case 4:  return "minecraft:barrel";
        case 5:  return "minecraft:furnace";
        case 6:  return "minecraft:blast_furnace";
        case 7:  return "minecraft:smoker";
        case 8:  return "minecraft:hopper";
        case 9:  return "minecraft:dispenser";
        case 10: return "minecraft:dropper";
        default: return "minecraft:chest"; // CK_NONE/CK_CHEST
    }
}

static i32 chestBlockEntityKind(const std::string& id) {
    if (id == "minecraft:trapped_chest") return 2;
    if (id == "minecraft:ender_chest")   return 3;
    if (id == "minecraft:barrel")        return 4;
    if (id == "minecraft:furnace")       return 5;
    if (id == "minecraft:blast_furnace") return 6;
    if (id == "minecraft:smoker")        return 7;
    if (id == "minecraft:hopper")        return 8;
    if (id == "minecraft:dispenser")     return 9;
    if (id == "minecraft:dropper")       return 10;
    return 1; // minecraft:chest
}

static bool isFurnaceKind(i32 kind) { return kind == 5 || kind == 6 || kind == 7; }

// Sign lines are stored as JSON text components since 1.20, so a raw line has
// to be wrapped and escaped rather than written as a bare string.
static std::string jsonQuote(const std::string& s) {
    const char quote = static_cast<char>(34);
    const char esc = static_cast<char>(92);
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back(quote);
    for (char c : s) {
        if (c == quote || c == esc) {
            out.push_back(esc);
            out.push_back(c);
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20) continue; // control chars break the parser
        out.push_back(c);
    }
    out.push_back(quote);
    return out;
}

static std::string jsonUnquote(const std::string& s) {
    const char quote = static_cast<char>(34);
    const char esc = static_cast<char>(92);
    if (s.size() < 2 || s.front() != quote || s.back() != quote) return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i] == esc && i + 2 < s.size()) {
            ++i;
            out.push_back(s[i]);
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

static std::string itemIdName(i32 itemId) {
    return "minecraft:" + gen::itemNameById(itemId);
}

static void writeSignSide(nbt::TagWriter& w, const char* side, const std::array<std::string, 4>* lines) {
    w.beginCompound(side);
    w.writeByte(0, "has_glowing_text");
    w.writeString("black", "color");
    w.beginList("messages", nbt::TagType::String, 4);
    for (int l = 0; l < 4; ++l) {
        w.writeListElementString(lines ? jsonQuote((*lines)[static_cast<size_t>(l)]) : jsonQuote(std::string()));
    }
    w.endList();
    w.endCompound();
}

static void writeBlockEntities(nbt::TagWriter& w, const ChunkExtras* ex) {
    const i32 count = ex ? static_cast<i32>(ex->chests.size() + ex->signs.size() +
                                            ex->spawners.size() + ex->banners.size())
                          : 0;
    w.beginList("block_entities", nbt::TagType::Compound, count);
    if (ex) {
        for (const extra::ChestRecord* c : ex->chests) {
            i32 x = 0, y = 0, z = 0;
            extra::unpackPos(c->pos, x, y, z);
            w.beginListElementCompound();
            w.writeString(chestBlockEntityId(c->kind), "id"); // BLOCKENT_V2: id now follows ChestRecord::kind
            w.writeByte(0, "keepPacked");
            w.writeInt(x, "x");
            w.writeInt(y, "y");
            w.writeInt(z, "z");
            i32 items = 0;
            for (int s = 0; s < 27; ++s) {
                if (c->itemId[static_cast<size_t>(s)] > 0 && c->count[static_cast<size_t>(s)] > 0) ++items;
            }
            w.beginList("Items", nbt::TagType::Compound, items);
            for (int s = 0; s < 27; ++s) {
                const i32 id = c->itemId[static_cast<size_t>(s)];
                const i32 n = c->count[static_cast<size_t>(s)];
                if (id <= 0 || n <= 0) continue;
                w.beginListElementCompound();
                w.writeByte(static_cast<u8>(s), "Slot");
                w.writeString(itemIdName(id), "id");
                w.writeInt(n, "count"); // 1.20.5+ item stack shape
                w.endListElementCompound();
            }
            w.endList();
            if (isFurnaceKind(c->kind)) { // CONTAINER_V3/BLOCKENT_V2: burn/cook progress
                const auto itf = ex->furnaceByPos.find(c->pos);
                const extra::FurnaceRecord* f = (itf != ex->furnaceByPos.end()) ? itf->second : nullptr;
                w.writeShort(f ? f->burn : 0, "BurnTime");
                w.writeShort(f ? f->cook : 0, "CookTime");
                w.writeShort(f ? f->cookTotal : 0, "CookTimeTotal");
            }
            w.endListElementCompound();
        }
        for (const extra::SignRecord* s : ex->signs) {
            i32 x = 0, y = 0, z = 0;
            extra::unpackPos(s->pos, x, y, z);
            w.beginListElementCompound();
            w.writeString("minecraft:sign", "id");
            w.writeByte(0, "keepPacked");
            w.writeInt(x, "x");
            w.writeInt(y, "y");
            w.writeInt(z, "z");
            w.writeByte(0, "is_waxed");
            writeSignSide(w, "front_text", &s->lines);
            writeSignSide(w, "back_text", nullptr);
            w.endListElementCompound();
        }
        for (const extra::SpawnerRecord* sp : ex->spawners) { // SPAWNER_V1/BLOCKENT_V2
            i32 x = 0, y = 0, z = 0;
            extra::unpackPos(sp->pos, x, y, z);
            w.beginListElementCompound();
            w.writeString("minecraft:mob_spawner", "id");
            w.writeInt(x, "x");
            w.writeInt(y, "y");
            w.writeInt(z, "z");
            w.writeShort(sp->delay, "Delay");
            w.writeShort(sp->minDelay, "MinSpawnDelay");
            w.writeShort(sp->maxDelay, "MaxSpawnDelay");
            w.writeShort(sp->spawnCount, "SpawnCount");
            w.writeShort(sp->maxNearby, "MaxNearbyEntities");
            w.writeShort(sp->requiredPlayerRange, "RequiredPlayerRange");
            w.writeShort(sp->spawnRange, "SpawnRange");
            w.beginCompound("SpawnData");
            w.beginCompound("entity");
            w.writeString(sp->entityId, "id");
            w.endCompound();
            w.endCompound();
            w.beginList("SpawnPotentials", nbt::TagType::Compound, 1);
            w.beginListElementCompound();
            w.writeInt(1, "weight");
            w.beginCompound("data");
            w.beginCompound("entity");
            w.writeString(sp->entityId, "id");
            w.endCompound();
            w.endCompound();
            w.endListElementCompound();
            w.endList();
            w.endListElementCompound();
        }
        for (const extra::BannerRecord* b : ex->banners) { // BANNER_V1/BLOCKENT_V2
            i32 x = 0, y = 0, z = 0;
            extra::unpackPos(b->pos, x, y, z);
            w.beginListElementCompound();
            w.writeString("minecraft:banner", "id");
            w.writeInt(x, "x");
            w.writeInt(y, "y");
            w.writeInt(z, "z");
            if (!b->customName.empty()) w.writeString(b->customName, "CustomName");
            w.beginList("patterns", nbt::TagType::Compound, static_cast<i32>(b->patterns.size()));
            for (const auto& pc : b->patterns) {
                w.beginListElementCompound();
                w.writeString(pc.first, "pattern");
                w.writeString(pc.second, "color");
                w.endListElementCompound();
            }
            w.endList();
            w.endListElementCompound();
        }
    }
    w.endList();
}

// Import side: pull chests, signs, furnaces, spawners and banners back out of
// a vanilla chunk compound. `chunk` already has this chunk's blocks applied
// (readRegionsFrom calls applyChunkNbt before this), so banner base color can
// be recovered from the placed *_banner/*_wall_banner block since 1.20.5+
// dropped it from the block entity itself.
static void readBlockEntities(const nbt::NbtValue& root, const ChunkColumn& chunk, extra::Snapshot& out) {
    const nbt::NbtValue* list = root.get("block_entities");
    if (!list || list->type != nbt::TagType::List) return;
    for (const nbt::NbtValue& be : list->list) {
        const std::string id = be.getString("id");
        const i32 x = be.getInt("x");
        const i32 y = be.getInt("y");
        const i32 z = be.getInt("z");
        if (id == "minecraft:chest" || id == "minecraft:trapped_chest" ||
            id == "minecraft:ender_chest" || id == "minecraft:barrel" ||
            id == "minecraft:furnace" || id == "minecraft:blast_furnace" ||
            id == "minecraft:smoker" || id == "minecraft:hopper" ||
            id == "minecraft:dispenser" || id == "minecraft:dropper") {
            const i32 kind = chestBlockEntityKind(id); // BLOCKENT_V2
            extra::ChestRecord rec;
            rec.pos = extra::packPos(x, y, z);
            rec.kind = kind;
            rec.trapped = (kind == 2);
            const nbt::NbtValue* items = be.get("Items");
            if (items && items->type == nbt::TagType::List) {
                for (const nbt::NbtValue& it : items->list) {
                    const i32 slot = static_cast<i32>(it.getByte("Slot"));
                    if (slot < 0 || slot >= 27) continue;
                    const i32 itemId = gen::itemIdByName(it.getString("id"));
                    if (itemId <= 0) continue;
                    // 1.20.5+ writes count as int, older saves used a Count byte.
                    i32 n = it.getInt("count", 0);
                    if (n <= 0) n = static_cast<i32>(it.getByte("Count"));
                    if (n <= 0) continue;
                    rec.itemId[static_cast<size_t>(slot)] = itemId;
                    rec.count[static_cast<size_t>(slot)] = n;
                }
            }
            out.chests.push_back(rec);
            if (isFurnaceKind(kind)) { // CONTAINER_V3/BLOCKENT_V2
                const i32 burn = be.getInt("BurnTime", 0);
                const i32 cook = be.getInt("CookTime", 0);
                const i32 cookTotal = be.getInt("CookTimeTotal", 0);
                if (burn > 0 || cook > 0) {
                    extra::FurnaceRecord f;
                    f.pos = rec.pos;
                    f.burn = burn;
                    f.burnTotal = burn; // vanilla doesn't persist the original fuel's total burn time
                    f.cook = cook;
                    f.cookTotal = cookTotal;
                    out.furnaces.push_back(f);
                }
            }
        } else if (id == "minecraft:sign" || id == "minecraft:hanging_sign") {
            extra::SignRecord rec;
            rec.pos = extra::packPos(x, y, z);
            const nbt::NbtValue* front = be.get("front_text");
            const nbt::NbtValue* msgs = front ? front->get("messages") : nullptr;
            if (msgs && msgs->type == nbt::TagType::List) {
                for (size_t l = 0; l < msgs->list.size() && l < 4; ++l) {
                    rec.lines[l] = jsonUnquote(msgs->list[l].asString);
                }
            } else {
                // Pre-1.20 saves stored Text1..Text4 directly on the compound.
                for (int l = 0; l < 4; ++l) {
                    rec.lines[static_cast<size_t>(l)] =
                        jsonUnquote(be.getString("Text" + std::to_string(l + 1)));
                }
            }
            out.signs.push_back(std::move(rec));
        } else if (id == "minecraft:mob_spawner") { // SPAWNER_V1/BLOCKENT_V2
            extra::SpawnerRecord rec;
            rec.pos = extra::packPos(x, y, z);
            rec.delay = be.getInt("Delay", rec.delay);
            rec.minDelay = be.getInt("MinSpawnDelay", rec.minDelay);
            rec.maxDelay = be.getInt("MaxSpawnDelay", rec.maxDelay);
            rec.spawnCount = be.getInt("SpawnCount", rec.spawnCount);
            rec.maxNearby = be.getInt("MaxNearbyEntities", rec.maxNearby);
            rec.requiredPlayerRange = be.getInt("RequiredPlayerRange", rec.requiredPlayerRange);
            rec.spawnRange = be.getInt("SpawnRange", rec.spawnRange);
            std::string entityId;
            if (const nbt::NbtValue* sd = be.get("SpawnData")) {
                if (const nbt::NbtValue* ent = sd->get("entity")) entityId = ent->getString("id");
            }
            if (entityId.empty()) {
                if (const nbt::NbtValue* pots = be.get("SpawnPotentials")) {
                    if (pots->type == nbt::TagType::List && !pots->list.empty()) {
                        const nbt::NbtValue& first = pots->list[0];
                        if (const nbt::NbtValue* data = first.get("data")) {
                            if (const nbt::NbtValue* ent = data->get("entity")) entityId = ent->getString("id");
                        }
                    }
                }
            }
            rec.entityId = entityId.empty() ? "minecraft:pig" : entityId;
            out.spawners.push_back(std::move(rec));
        } else if (id == "minecraft:banner") { // BANNER_V1/BLOCKENT_V2
            extra::BannerRecord rec;
            rec.pos = extra::packPos(x, y, z);
            rec.customName = be.getString("CustomName");
            const nbt::NbtValue* pats = be.get("patterns");
            if (pats && pats->type == nbt::TagType::List) {
                for (const nbt::NbtValue& p : pats->list) {
                    rec.patterns.emplace_back(p.getString("pattern"), p.getString("color"));
                }
            }
            // Base color isn't stored on the block entity in 1.20.5+; it's
            // implied by which *_banner/*_wall_banner block is placed here.
            std::string tail = stateIdToName(chunk.getBlock(x, y, z));
            const size_t colon = tail.find(':');
            if (colon != std::string::npos) tail = tail.substr(colon + 1);
            for (const char* suffix : {"_wall_banner", "_banner"}) {
                const size_t sl = std::string(suffix).size();
                if (tail.size() > sl && tail.compare(tail.size() - sl, sl, suffix) == 0) {
                    tail = tail.substr(0, tail.size() - sl);
                    break;
                }
            }
            rec.baseColor = tail;
            out.banners.push_back(std::move(rec));
        }
    }
}

static std::vector<u8> buildChunkNbt(const ChunkColumn& chunk, i64 seed,
                                     const ChunkExtras* chunkExtras) {
    nbt::TagWriter w;
    w.beginRootCompoundNamed("");
    w.writeInt(DATA_VERSION_1_21_1, "DataVersion");
    w.writeInt(chunk.getX(), "xPos");
    w.writeInt(chunk.getZ(), "zPos");
    w.writeInt(CHUNK_HEIGHT_MIN / SECTION_HEIGHT, "yPos");
    w.writeString("minecraft:full", "Status");
    w.writeByte(0, "isLightOn"); // let the client/server relight on load
    w.writeLong(0, "LastUpdate");
    w.writeLong(0, "InhabitedTime");

    w.beginList("sections", nbt::TagType::Compound, SECTIONS_PER_CHUNK);
    for (i32 s = 0; s < SECTIONS_PER_CHUNK; ++s) {
        i32 sectionY = CHUNK_HEIGHT_MIN / SECTION_HEIGHT + s;

        // Build a local block palette (order of first appearance).
        std::vector<i32> palette; // BLOCKMAP_V1: state ids, names+properties resolved on write
        std::unordered_map<i32, i32> stateToPaletteIdx;
        std::vector<i32> indices;
        indices.reserve(BLOCKS_PER_SECTION);
        // HANGDIAG_V1 (root-cause fix): chunk.getSection(s) const dereferences the
        // section's unique_ptr WITHOUT a null check (unlike the non-const overload,
        // which lazily allocates, and unlike ChunkColumn::getBlock, which returns 0
        // for a null section). Most sections are never allocated at all — an
        // unloaded/all-air section stays nullptr by design (MEM_V1) — so calling
        // section.getBlock() on it dereferenced `this == nullptr` and crashed with
        // ACCESS_VIOLATION inside ChunkSection::stateAt during world export/save.
        // sectionHasBlocks() checks the pointer itself and never allocates, so only
        // touch getSection()/ChunkSection when we know the pointer is non-null.
        if (chunk.sectionHasBlocks(s)) {
            const ChunkSection& section = chunk.getSection(s);
            for (i32 y = 0; y < SECTION_HEIGHT; ++y) {
                for (i32 z = 0; z < SECTION_WIDTH; ++z) {
                    for (i32 x = 0; x < SECTION_WIDTH; ++x) {
                        i32 stateId = section.getBlock(x, y, z);
                        auto it = stateToPaletteIdx.find(stateId);
                        i32 pIdx;
                        if (it == stateToPaletteIdx.end()) {
                            pIdx = static_cast<i32>(palette.size());
                            palette.push_back(stateId);
                            stateToPaletteIdx.emplace(stateId, pIdx);
                        } else {
                            pIdx = it->second;
                        }
                        indices.push_back(pIdx);
                    }
                }
            }
        }
        if (palette.empty()) palette.push_back(0); // minecraft:air

        w.beginListElementCompound();
        w.writeByte(sectionY, "Y");

        w.beginCompound("block_states");
        w.beginList("palette", nbt::TagType::Compound, static_cast<i32>(palette.size()));
        for (i32 paletteState : palette) {
            w.beginListElementCompound();
            w.writeString(stateIdToName(paletteState), "Name");
            // BLOCKMAP_V1: without Properties vanilla loads every stair, door and
            // slab in its default orientation.
            if (const auto* props = stateProperties(paletteState)) {
                w.beginCompound("Properties");
                for (const auto& [key, value] : *props) w.writeString(value, key);
                w.endCompound();
            }
            w.endListElementCompound();
        }
        w.endList();
        if (palette.size() > 1) {
            int bits = bitsForPalette(palette.size(), 4);
            auto packed = packIndices(indices, bits);
            w.writeLongArray(packed, "data");
        }
        w.endCompound(); // block_states

        // BIOMEGEN_V1: real vanilla biomes per 4x4x4 cell, matching the world
        // seed bit-for-bit via cubiomes (see world/biomegen.hpp).
        i32 chunkBlockX = chunk.getX() * SECTION_WIDTH;
        i32 chunkBlockZ = chunk.getZ() * SECTION_WIDTH;
        i32 sectionBlockY = sectionY * SECTION_HEIGHT;
        std::vector<std::string> biomePalette;
        std::unordered_map<std::string, i32> biomeToPaletteIdx;
        std::vector<i32> biomeIndices;
        biomeIndices.reserve(BIOMES_PER_SECTION);
        for (i32 cy = 0; cy < 4; ++cy) {
            for (i32 cz = 0; cz < 4; ++cz) {
                for (i32 cx = 0; cx < 4; ++cx) {
                    i32 bx = chunkBlockX + cx * 4;
                    i32 by = sectionBlockY + cy * 4;
                    i32 bz = chunkBlockZ + cz * 4;
                    std::string name = biome::getBiomeName(seed, bx, by, bz);
                    auto bit = biomeToPaletteIdx.find(name);
                    i32 bIdx;
                    if (bit == biomeToPaletteIdx.end()) {
                        bIdx = static_cast<i32>(biomePalette.size());
                        biomePalette.push_back(name);
                        biomeToPaletteIdx.emplace(name, bIdx);
                    } else {
                        bIdx = bit->second;
                    }
                    biomeIndices.push_back(bIdx);
                }
            }
        }
        if (biomePalette.empty()) biomePalette.push_back("minecraft:plains");

        w.beginCompound("biomes");
        w.beginList("palette", nbt::TagType::String, static_cast<i32>(biomePalette.size()));
        for (const auto& name : biomePalette) w.writeListElementString(name);
        w.endList();
        if (biomePalette.size() > 1) {
            int bbits = bitsForPalette(biomePalette.size(), 1);
            auto bpacked = packIndices(biomeIndices, bbits);
            w.writeLongArray(bpacked, "data");
        }
        w.endCompound(); // biomes

        w.endListElementCompound(); // section
    }
    w.endList(); // sections

    writeBlockEntities(w, chunkExtras); // BLOCKENT_V1

    w.endCompound(); // root
    return w.toVector();
}

// ------------------------------------------------------------------
// Chunk NBT: import (parsed vanilla chunk NBT -> our ChunkColumn)
// ------------------------------------------------------------------

static void applyChunkNbt(const nbt::NbtValue& root, ChunkColumn& chunk) {
    const auto* sections = root.get("sections");
    if (!sections || sections->type != nbt::TagType::List) return;
    for (const auto& sec : sections->list) {
        if (sec.type != nbt::TagType::Compound) continue;
        i32 sectionY = static_cast<i32>(static_cast<i8>(sec.getByte("Y")));
        i32 baseY = sectionY * SECTION_HEIGHT;
        if (baseY < CHUNK_HEIGHT_MIN || baseY >= CHUNK_HEIGHT_MAX) continue; // outside our supported height range

        const auto* blockStates = sec.get("block_states");
        if (!blockStates) continue;
        const auto* paletteTag = blockStates->get("palette");
        if (!paletteTag || paletteTag->type != nbt::TagType::List) continue;

        std::vector<i32> paletteIds;
        paletteIds.reserve(paletteTag->list.size());
        for (const auto& entry : paletteTag->list) {
            std::string name = entry.getString("Name", "minecraft:air");
            // BLOCKMAP_V1: read Properties back so orientation survives the import.
            std::unordered_map<std::string, std::string> props;
            if (const auto* propsTag = entry.get("Properties");
                propsTag && propsTag->type == nbt::TagType::Compound) {
                for (const auto& [key, value] : propsTag->compound) props.emplace(key, value.asString);
            }
            paletteIds.push_back(nameToStateId(name, props));
        }
        if (paletteIds.empty()) continue;

        std::vector<i32> indices;
        const auto* dataTag = blockStates->get("data");
        if (paletteIds.size() == 1) {
            indices.assign(BLOCKS_PER_SECTION, 0);
        } else if (dataTag && dataTag->type == nbt::TagType::LongArray) {
            int bits = bitsForPalette(paletteIds.size(), 4);
            indices = unpackIndices(dataTag->longArray, bits, BLOCKS_PER_SECTION);
        } else {
            continue;
        }

        size_t i = 0;
        for (i32 y = 0; y < SECTION_HEIGHT && i < indices.size(); ++y) {
            for (i32 z = 0; z < SECTION_WIDTH; ++z) {
                for (i32 x = 0; x < SECTION_WIDTH; ++x, ++i) {
                    i32 pIdx = indices[i];
                    if (pIdx < 0 || size_t(pIdx) >= paletteIds.size()) continue;
                    i32 stateId = paletteIds[size_t(pIdx)];
                    if (stateId == 0) continue; // air is the section default already
                    chunk.setBlock(chunk.getX() * 16 + x, baseY + y, chunk.getZ() * 16 + z, stateId);
                }
            }
        }
    }
}

// ------------------------------------------------------------------
// level.dat (see anvil.hpp: hand-authored flat-world WorldGenSettings,
// not verified against a real Mojang-generated save in this environment).
// ------------------------------------------------------------------

// V57_PLAYERTAG_V1: наша раскладка окна инвентаря -> нумерация слотов ванили.
// Возвращает false для слотов, которые в level.dat не попадают (верстак).
static bool vanillaPlayerSlot(int ourSlot, i32& out) {
    if (ourSlot >= 36 && ourSlot <= 44) { out = ourSlot - 36; return true; }        // хотбар -> 0..8
    if (ourSlot >= 9 && ourSlot <= 35)  { out = ourSlot; return true; }             // рюкзак 1:1
    if (ourSlot == 5) { out = 103; return true; }                                   // шлем
    if (ourSlot == 6) { out = 102; return true; }                                   // нагрудник
    if (ourSlot == 7) { out = 101; return true; }                                   // штаны
    if (ourSlot == 8) { out = 100; return true; }                                   // ботинки
    if (ourSlot == 45) { out = -106; return true; }                                 // вторая рука
    return false;
}

// V57_PLAYERTAG_V1: тег Data.Player — позиция, взгляд, режим, инвентарь
// и эндер-сундук. Формат стака — 1.20.5+ ({Slot, id, count}), как уже
// используется для содержимого сундуков в writeBlockEntities().
static void writePlayerTag(nbt::TagWriter& w, const PlayerExport& p) {
    w.beginCompound("Player");
    w.writeInt(DATA_VERSION_1_21_1, "DataVersion");
    w.writeDoubleList({p.x, p.y, p.z}, "Pos");
    w.writeDoubleList({0.0, 0.0, 0.0}, "Motion");
    w.writeFloatList({p.yaw, p.pitch}, "Rotation");
    w.writeString("minecraft:overworld", "Dimension");
    w.writeInt(p.gameMode, "playerGameType");
    w.writeInt(p.gameMode, "previousPlayerGameType");
    w.writeFloat(p.health, "Health");
    w.writeInt(p.foodLevel, "foodLevel");
    w.writeFloat(5.0f, "foodSaturationLevel");
    w.writeFloat(0.0f, "foodExhaustionLevel");
    w.writeInt(0, "foodTickTimer");
    w.writeInt(p.xpLevel, "XpLevel");
    w.writeFloat(0.0f, "XpP");
    w.writeInt(p.xpTotal, "XpTotal");
    w.writeInt(-1, "XpSeed");
    w.writeInt(0, "Score");
    w.writeShort(300, "Air");
    w.writeShort(-20, "Fire");
    w.writeShort(0, "HurtTime");
    w.writeShort(0, "DeathTime");
    w.writeShort(0, "SleepTimer");
    w.writeFloat(0.0f, "FallDistance");
    w.writeFloat(0.0f, "AbsorptionAmount");
    w.writeInt(0, "HurtByTimestamp");
    w.writeInt(0, "PortalCooldown");
    w.writeInt(p.selectedSlot, "SelectedItemSlot");
    w.writeByte(1, "OnGround");
    w.writeByte(0, "Invulnerable");
    w.writeByte(0, "FallFlying");
    w.writeByte(1, "seenCredits");

    // Способности: в креативе нужен полёт и неуязвимость, иначе клиент
    // откроет мир в креативе, но с выключенными способностями.
    const bool creative = (p.gameMode == 1);
    const bool spectator = (p.gameMode == 3);
    w.beginCompound("abilities");
    w.writeByte((creative || spectator) ? 1 : 0, "flying");
    w.writeByte((creative || spectator) ? 1 : 0, "mayfly");
    w.writeByte(creative ? 1 : 0, "instabuild");
    w.writeByte((creative || spectator) ? 1 : 0, "invulnerable");
    w.writeByte(spectator ? 0 : 1, "mayBuild");
    w.writeFloat(0.05f, "flySpeed");
    w.writeFloat(0.1f, "walkSpeed");
    w.endCompound();

    i32 invCount = 0;
    for (int s = 0; s < 46; ++s) {
        i32 dummy = 0;
        if (p.itemId[static_cast<size_t>(s)] > 0 && p.count[static_cast<size_t>(s)] > 0 &&
            vanillaPlayerSlot(s, dummy)) ++invCount;
    }
    w.beginList("Inventory", nbt::TagType::Compound, invCount);
    for (int s = 0; s < 46; ++s) {
        const i32 id = p.itemId[static_cast<size_t>(s)];
        const i32 n = p.count[static_cast<size_t>(s)];
        i32 vslot = 0;
        if (id <= 0 || n <= 0 || !vanillaPlayerSlot(s, vslot)) continue;
        w.beginListElementCompound();
        w.writeByte(static_cast<i32>(static_cast<i8>(vslot)), "Slot"); // -106 пишется байтом
        w.writeString(itemIdName(id), "id");
        w.writeInt(n, "count");
        w.endListElementCompound();
    }
    w.endList();

    i32 enderCount = 0;
    for (int s = 0; s < 27; ++s)
        if (p.enderItemId[static_cast<size_t>(s)] > 0 && p.enderCount[static_cast<size_t>(s)] > 0) ++enderCount;
    w.beginList("EnderItems", nbt::TagType::Compound, enderCount);
    for (int s = 0; s < 27; ++s) {
        const i32 id = p.enderItemId[static_cast<size_t>(s)];
        const i32 n = p.enderCount[static_cast<size_t>(s)];
        if (id <= 0 || n <= 0) continue;
        w.beginListElementCompound();
        w.writeByte(s, "Slot");
        w.writeString(itemIdName(id), "id");
        w.writeInt(n, "count");
        w.endListElementCompound();
    }
    w.endList();

    w.beginList("Attributes", nbt::TagType::Compound, 0);
    w.endList();
    w.beginList("active_effects", nbt::TagType::Compound, 0);
    w.endList();
    w.endCompound(); // Player
}

static std::vector<u8> buildLevelDatNbt(const std::string& levelName, i64 seed,
                                         i32 spawnX, i32 spawnY, i32 spawnZ, i64 dayTime,
                                         i32 gameMode = 0,                    // V57_GAMETYPE_V1
                                         const PlayerExport* player = nullptr) { // V57_PLAYERTAG_V1
    nbt::TagWriter w;
    w.beginRootCompoundNamed("");
    w.beginCompound("Data");
    w.writeInt(DATA_VERSION_1_21_1, "DataVersion");
    // LEVELFMT_V1: level.dat format marker. 19133 = Anvil, 19132 = old Region.
    // Without this tag the vanilla client cannot classify the save and falls
    // back to data version 0 -> "Unknown data version: 0" on the world list.
    w.writeInt(19133, "version");
    w.writeString(levelName, "LevelName");
    w.writeInt(spawnX, "SpawnX");
    w.writeInt(spawnY, "SpawnY");
    w.writeInt(spawnZ, "SpawnZ");
    w.writeFloat(0.0f, "SpawnAngle");
    w.writeLong(dayTime, "Time");
    w.writeLong(dayTime, "DayTime");
    w.writeLong(0, "LastPlayed");
    w.writeByte(0, "hardcore");
    // V57_GAMETYPE_V1: режим игры больше не забит гвоздями в survival — берётся
    // из gamemode в settings.properties (0 survival / 1 creative / 2 adventure / 3 spectator).
    w.writeInt(gameMode, "GameType");
    w.writeByte(0, "Difficulty");
    w.writeByte(0, "DifficultyLocked");
    w.writeByte(0, "raining");
    w.writeInt(0, "rainTime");
    w.writeByte(0, "thundering");
    w.writeInt(0, "thunderTime");
    w.writeByte(1, "initialized");
    w.writeByte(1, "allowCommands");

    // V57_PLAYERTAG_V1: позиция и инвентарь игрока, если их передали.
    if (player) writePlayerTag(w, *player);

    w.beginCompound("Version");
    w.writeInt(DATA_VERSION_1_21_1, "Id");
    w.writeString("1.21.1", "Name");
    w.writeString("main", "Series");
    w.writeByte(0, "Snapshot");
    w.endCompound();

    w.beginCompound("DataPacks");
    w.beginList("Enabled", nbt::TagType::String, 1);
    w.writeListElementString("vanilla");
    w.endList();
    w.beginList("Disabled", nbt::TagType::String, 0);
    w.endList();
    w.endCompound();

    w.beginCompound("WorldGenSettings");
    w.writeLong(seed, "seed");
    w.writeByte(1, "generate_features");
    w.writeByte(0, "bonus_chest");
    w.beginCompound("dimensions");

    w.beginCompound("minecraft:overworld");
    w.writeString("minecraft:overworld", "type");
    w.beginCompound("generator");
    w.writeString("minecraft:flat", "type");
    w.beginCompound("settings");
    w.writeString("minecraft:plains", "biome");
    w.beginList("layers", nbt::TagType::Compound, 3);
    w.beginListElementCompound(); w.writeInt(1, "height"); w.writeString("minecraft:bedrock", "block"); w.endListElementCompound();
    w.beginListElementCompound(); w.writeInt(2, "height"); w.writeString("minecraft:dirt", "block"); w.endListElementCompound();
    w.beginListElementCompound(); w.writeInt(1, "height"); w.writeString("minecraft:grass_block", "block"); w.endListElementCompound();
    w.endList();
    w.writeByte(0, "features");
    w.writeByte(0, "lakes");
    w.endCompound(); // settings
    w.endCompound(); // generator
    w.endCompound(); // minecraft:overworld

    w.beginCompound("minecraft:the_nether");
    w.writeString("minecraft:the_nether", "type");
    w.beginCompound("generator");
    w.writeString("minecraft:noise", "type");
    w.writeString("minecraft:nether", "settings");
    w.beginCompound("biome_source");
    w.writeString("minecraft:multi_noise", "type");
    w.writeString("minecraft:nether", "preset");
    w.endCompound();
    w.endCompound();
    w.endCompound(); // minecraft:the_nether

    w.beginCompound("minecraft:the_end");
    w.writeString("minecraft:the_end", "type");
    w.beginCompound("generator");
    w.writeString("minecraft:noise", "type");
    w.writeString("minecraft:end", "settings");
    w.beginCompound("biome_source");
    w.writeString("minecraft:the_end", "type");
    w.endCompound();
    w.endCompound();
    w.endCompound(); // minecraft:the_end

    w.endCompound(); // dimensions
    w.endCompound(); // WorldGenSettings

    w.endCompound(); // Data
    w.endCompound(); // root
    return w.toVector();
}

// ------------------------------------------------------------------
// DIMCONV_V1: one region engine, reused by every dimension.
// ------------------------------------------------------------------

// SAVEFAST_V1: автосейв каждые 5 минут заново собирал NBT И ЖАЛ zlib-ом ВСЕ чанки
// (свой дефлейт ~13 мс на чанк => 500 чанков = 6–8 секунд на каждом сохранении),
// даже если в мире не пошевелился ни один блок. Теперь сжатые байты каждого чанка
// кешируются:
//   * чанк чистый (isDirty()==false) и без блок-сущностей -> берём готовый blob, NBT даже не собираем;
//   * иначе собираем NBT и сравниваем его хеш — совпал, значит жать нечего (байт-в-байт тот же чанк);
//   * в регионе ни один чанк не изменился -> .mca вообще не перезаписывается.
// Никакой потери данных: любая правка блока идёт через setBlock -> dirty_=true,
// а содержимое сундуков/табличек меняется без setBlock, поэтому чанки с блок-сущностями
// всегда проверяются по хешу NBT.
struct NcSaveBlob {
    u64 sig = 0;              // FNV-1a от NBT чанка (0 = неизвестно)
    std::vector<u8> zlib;     // готовые сжатые байты для region-файла
};
static std::mutex g_ncSaveCacheMutex;
static std::unordered_map<std::string, std::unordered_map<i64, NcSaveBlob>> g_ncSaveBlobs;   // regionDir -> chunkKey -> blob
static std::unordered_map<std::string, std::unordered_map<i64, u64>> g_ncSaveRegionSig;      // regionDir -> regionKey -> подпись
static u64 ncSaveHash(const std::vector<u8>& bytes) {
    u64 h = 1469598103934665603ull;
    for (u8 b : bytes) { h ^= static_cast<u64>(b); h *= 1099511628211ull; }
    return h;
}

static bool writeRegionsTo(const World& world, const std::string& regionDir, i64 seed,
                           size_t* chunksOut, size_t* regionsOut, std::string* errorOut,
                           const ExtrasByChunk* extras = nullptr) {
    std::error_code ec;
    fs::create_directories(regionDir, ec);

    // Group chunks by region (32x32 chunks each).
    std::unordered_map<i64, std::vector<std::shared_ptr<ChunkColumn>>> regionChunks; // key: (rx<<32)|rz(as u32)
    auto regionKey = [](i32 rx, i32 rz) -> i64 {
        return (static_cast<i64>(rx) << 32) | static_cast<u32>(rz);
    };
    for (const auto& [pos, chunkPtr] : world.getAllChunks()) {
        if (!chunkPtr) continue;
        i32 rx = pos.x >> 5; // arithmetic shift = floor div 32, safe for negative chunk coords
        i32 rz = pos.z >> 5;
        regionChunks[regionKey(rx, rz)].push_back(chunkPtr);
    }

    u32 timestamp = 0; // vanilla just uses this for informational purposes
    // SAVEFAST_V1: инкрементальная запись — жмём и пишем только то, что реально поменялось
    size_t reusedChunks = 0, rebuiltChunks = 0, skippedRegions = 0, writtenRegions = 0;
    std::lock_guard<std::mutex> saveCacheLk(g_ncSaveCacheMutex);
    auto& blobCache = g_ncSaveBlobs[regionDir];
    auto& regionSigs = g_ncSaveRegionSig[regionDir];
    std::unordered_set<i64> liveChunks;
    liveChunks.reserve(world.getAllChunks().size() * 2);

    struct NcSaveItem { int lx; int lz; const std::vector<u8>* blob; };
    for (const auto& [key, chunkList] : regionChunks) {
        i32 rx = static_cast<i32>(key >> 32);
        i32 rz = static_cast<i32>(key & 0xFFFFFFFFu);
        std::vector<NcSaveItem> items;
        items.reserve(chunkList.size());
        u64 regionSig = 1469598103934665603ull;
        for (const auto& chunkPtr : chunkList) {
            const ChunkExtras* be = nullptr; // BLOCKENT_V1
            if (extras) {
                auto itx = extras->find(chunkKeyOf(chunkPtr->getX(), chunkPtr->getZ()));
                if (itx != extras->end()) be = &itx->second;
            }
            const i64 ck = chunkKeyOf(chunkPtr->getX(), chunkPtr->getZ());
            liveChunks.insert(ck);
            auto& slot = blobCache[ck]; // unordered_map — node-based, ссылка остаётся валидной

            // Самый быстрый путь: блоки не правили и блок-сущностей нет — даже NBT не собираем.
            if (!slot.zlib.empty() && slot.sig != 0 && be == nullptr && !chunkPtr->isDirty()) {
                ++reusedChunks;
            } else {
                auto chunkNbt = buildChunkNbt(*chunkPtr, seed, be);
                const u64 sig = ncSaveHash(chunkNbt);
                if (slot.zlib.empty() || slot.sig != sig) {
                    slot.zlib = net::zlibc::compress(std::span<const u8>(chunkNbt.data(), chunkNbt.size()));
                    slot.sig = sig;
                    ++rebuiltChunks;
                } else {
                    ++reusedChunks; // чанк числится грязным, но байты те же — жать нечего
                }
            }
            regionSig ^= slot.sig + 0x9e3779b97f4a7c15ull + static_cast<u64>(ck);
            regionSig *= 1099511628211ull;
            items.push_back(NcSaveItem{ ((chunkPtr->getX() % 32) + 32) % 32,
                                        ((chunkPtr->getZ() % 32) + 32) % 32,
                                        &slot.zlib });
        }

        std::string regionPath = regionDir + "/r." + std::to_string(rx) + "." + std::to_string(rz) + ".mca";
        auto itSig = regionSigs.find(key);
        if (itSig != regionSigs.end() && itSig->second == regionSig && fs::exists(regionPath, ec)) {
            ++skippedRegions; // в регионе ничего не изменилось — файл трогать не надо
            continue;
        }
        RegionWriter region;
        for (const auto& it : items) region.putChunk(it.lx, it.lz, *it.blob, /*compressionType=*/2, timestamp);
        if (!region.save(regionPath)) { if (errorOut) *errorOut = "failed writing " + regionPath; return false; }
        regionSigs[key] = regionSig;
        ++writtenRegions;
    }

    // выгруженные из ОЗУ чанки в кеше держать незачем
    for (auto it = blobCache.begin(); it != blobCache.end(); ) {
        if (liveChunks.find(it->first) == liveChunks.end()) it = blobCache.erase(it);
        else ++it;
    }
    NC_CTRACE("Anvil: save cache -> reused %zu, compressed %zu, regions written %zu, skipped %zu",
              reusedChunks, rebuiltChunks, writtenRegions, skippedRegions);

    if (chunksOut) *chunksOut = world.getAllChunks().size();
    if (regionsOut) *regionsOut = regionChunks.size();
    return true;
}

static size_t readRegionsFrom(World& world, const std::string& regionDir,
                              extra::Snapshot* extrasOut = nullptr) {
    size_t importedChunks = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(regionDir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string fname = entry.path().filename().string();
        // Expect r.<rx>.<rz>.mca
        if (fname.size() < 5 || fname.substr(0, 2) != "r.") continue;
        if (fname.substr(fname.size() - 4) != ".mca") continue;
        std::string mid = fname.substr(2, fname.size() - 2 - 4);
        size_t dot = mid.find('.');
        if (dot == std::string::npos) continue;
        i32 rx = 0, rz = 0;
        try {
            rx = std::stoi(mid.substr(0, dot));
            rz = std::stoi(mid.substr(dot + 1));
        } catch (...) { continue; }

        RegionReader region;
        if (!region.load(entry.path().string())) continue;

        for (int lz = 0; lz < 32; ++lz) {
            for (int lx = 0; lx < 32; ++lx) {
                std::vector<u8> chunkBytes;
                if (!region.getChunk(lx, lz, chunkBytes)) continue;
                nbt::TagReader reader(chunkBytes.data(), chunkBytes.size());
                std::string rootName;
                nbt::NbtValue root;
                if (!reader.readNamedRoot(rootName, root)) continue;

                i32 cx = rx * 32 + lx;
                i32 cz = rz * 32 + lz;
                auto chunk = world.getChunkOrCreate(cx, cz);
                if (!chunk) continue;
                applyChunkNbt(root, *chunk);
                if (extrasOut) readBlockEntities(root, *chunk, *extrasOut); // BLOCKENT_V1/BLOCKENT_V2
                ++importedChunks;
            }
        }
    }
    return importedChunks;
}

// Vanilla keeps the other dimensions inside the save; Bukkit and its forks
// (Spigot / Paper / Purpur / Folia) split them into sibling folders instead.
static std::string dimRegionSubdir(Dimension dim) {
    switch (dim) {
        case Dimension::Nether: return "/DIM-1/region";
        case Dimension::End:    return "/DIM1/region";
        default:                return "/region";
    }
}

static std::vector<std::string> dimRegionCandidates(const std::string& worldDir, Dimension dim) {
    std::vector<std::string> out;
    out.push_back(worldDir + dimRegionSubdir(dim));
    const std::string parent = fs::path(worldDir).parent_path().string();
    if (dim == Dimension::Nether) {
        out.push_back(worldDir + "_nether/DIM-1/region");
        if (!parent.empty()) out.push_back(parent + "/world_nether/DIM-1/region");
    } else if (dim == Dimension::End) {
        out.push_back(worldDir + "_the_end/DIM1/region");
        if (!parent.empty()) out.push_back(parent + "/world_the_end/DIM1/region");
    }
    return out;
}

// ------------------------------------------------------------------
// ENTSAVE_V1: vanilla entity storage (entities/r.<rx>.<rz>.mca).
//
// Since 1.17 mobs, dropped items and vehicles live in their own region folder,
// not inside the chunk NBT. Each stored chunk is a root compound with
// DataVersion, Position (int array of chunk x/z) and an Entities list.
// ------------------------------------------------------------------

// Core dimension ids: 0 overworld, 1 nether, 2 end.
static i32 coreDimOf(Dimension d) {
    if (d == Dimension::Nether) return 1;
    if (d == Dimension::End) return 2;
    return 0;
}

static i32 floorDiv16(f64 v) {
    return static_cast<i32>(std::floor(v / 16.0));
}

// region/ -> entities/ sibling for the same dimension.
static std::string entitiesDirFor(const std::string& regionDir) {
    const std::string tail = "region";
    if (regionDir.size() >= tail.size() &&
        regionDir.compare(regionDir.size() - tail.size(), tail.size(), tail) == 0) {
        return regionDir.substr(0, regionDir.size() - tail.size()) + "entities";
    }
    return regionDir + "/entities";
}

struct ChunkEnts {
    std::vector<const extra::MobRecord*> mobs;
    std::vector<const extra::DropRecord*> drops;
    std::vector<const extra::VehicleRecord*> vehicles;

    i32 count() const {
        return static_cast<i32>(mobs.size() + drops.size() + vehicles.size());
    }
};

static void writeMobEntity(nbt::TagWriter& w, const extra::MobRecord& m) {
    w.beginListElementCompound();
    w.writeString("minecraft:" + std::string(entity::mobDef(m.typeIdx).name), "id");
    w.writeDoubleList({m.x, m.y, m.z}, "Pos");
    w.writeDoubleList({0.0, 0.0, 0.0}, "Motion");
    w.writeFloatList({m.yaw, m.pitch}, "Rotation");
    w.writeFloat(static_cast<f32>(m.health), "Health");
    w.writeFloat(0.0f, "FallDistance");
    w.writeShort(300, "Air");
    w.writeByte(1, "OnGround");
    w.writeByte(0, "Invulnerable");
    w.writeByte(0, "PersistenceRequired");
    if (m.baby) w.writeByte(1, "IsBaby");
    if (m.sheared) w.writeByte(1, "Sheared");
    if (m.wool > 0) w.writeByte(static_cast<u8>(m.wool), "Color");
    if (m.sitting) w.writeByte(1, "Sitting");
    w.endListElementCompound();
}

static void writeDropEntity(nbt::TagWriter& w, const extra::DropRecord& d) {
    w.beginListElementCompound();
    w.writeString("minecraft:item", "id");
    w.writeDoubleList({d.x, d.y, d.z}, "Pos");
    w.writeDoubleList({d.vx, d.vy, d.vz}, "Motion");
    w.writeFloatList({0.0f, 0.0f}, "Rotation");
    w.writeShort(static_cast<i16>(d.age), "Age");
    w.writeShort(0, "PickupDelay");
    w.writeShort(5, "Health");
    w.writeByte(0, "OnGround");
    w.beginCompound("Item");
    w.writeString(itemIdName(d.itemId), "id");
    w.writeInt(d.count, "count");
    w.endCompound();
    w.endListElementCompound();
}

static void writeVehicleEntity(nbt::TagWriter& w, const extra::VehicleRecord& v) {
    // Core type ids: 69 minecart, 10 boat. Vanilla 1.21.1 has no generic boat,
    // so an unspecified boat lands as oak.
    const char* id = (v.typeId == 69) ? "minecraft:minecart" : "minecraft:oak_boat";
    w.beginListElementCompound();
    w.writeString(id, "id");
    w.writeDoubleList({v.x, v.y, v.z}, "Pos");
    w.writeDoubleList({0.0, 0.0, 0.0}, "Motion");
    w.writeFloatList({v.yaw, 0.0f}, "Rotation");
    w.writeFloat(0.0f, "FallDistance");
    w.writeByte(1, "OnGround");
    w.writeByte(0, "Invulnerable");
    w.endListElementCompound();
}

static bool writeEntityRegions(const extra::Snapshot& snap, const std::string& entitiesDir,
                               i32 dim, size_t* countOut, std::string* errorOut) {
    std::unordered_map<i64, ChunkEnts> byChunk;
    for (const extra::MobRecord& m : snap.mobs) {
        if (m.dim != dim) continue;
        byChunk[chunkKeyOf(floorDiv16(m.x), floorDiv16(m.z))].mobs.push_back(&m);
    }
    // Drops and vehicles carry no dimension in the core, so they belong to the
    // overworld and are skipped for the other two.
    if (dim == 0) {
        for (const extra::DropRecord& d : snap.drops) {
            byChunk[chunkKeyOf(floorDiv16(d.x), floorDiv16(d.z))].drops.push_back(&d);
        }
        for (const extra::VehicleRecord& v : snap.vehicles) {
            byChunk[chunkKeyOf(floorDiv16(v.x), floorDiv16(v.z))].vehicles.push_back(&v);
        }
    }
    if (countOut) *countOut = 0;
    if (byChunk.empty()) return true;

    std::error_code ec;
    fs::create_directories(entitiesDir, ec);

    std::unordered_map<i64, std::vector<std::pair<i64, const ChunkEnts*>>> byRegion;
    for (const auto& entry : byChunk) {
        const i32 cx = static_cast<i32>(entry.first >> 32);
        const i32 cz = static_cast<i32>(entry.first & 0xFFFFFFFFu);
        byRegion[chunkKeyOf(cx >> 5, cz >> 5)].push_back({entry.first, &entry.second});
    }

    size_t written = 0;
    for (const auto& reg : byRegion) {
        const i32 rx = static_cast<i32>(reg.first >> 32);
        const i32 rz = static_cast<i32>(reg.first & 0xFFFFFFFFu);
        RegionWriter region;
        for (const auto& item : reg.second) {
            const i32 cx = static_cast<i32>(item.first >> 32);
            const i32 cz = static_cast<i32>(item.first & 0xFFFFFFFFu);
            const ChunkEnts& ents = *item.second;

            nbt::TagWriter w;
            w.beginRootCompoundNamed("");
            w.writeInt(DATA_VERSION_1_21_1, "DataVersion");
            w.writeIntArray({cx, cz}, "Position");
            w.beginList("Entities", nbt::TagType::Compound, ents.count());
            for (const extra::MobRecord* m : ents.mobs) writeMobEntity(w, *m);
            for (const extra::DropRecord* d : ents.drops) writeDropEntity(w, *d);
            for (const extra::VehicleRecord* v : ents.vehicles) writeVehicleEntity(w, *v);
            w.endList();
            w.endCompound();

            const auto bytes = w.toVector();
            const auto packed = net::zlibc::compress(std::span<const u8>(bytes.data(), bytes.size()));
            region.putChunk(((cx % 32) + 32) % 32, ((cz % 32) + 32) % 32, packed, 2, 0);
            written += static_cast<size_t>(ents.count());
        }
        const std::string path = entitiesDir + "/r." + std::to_string(rx) + "." + std::to_string(rz) + ".mca";
        if (!region.save(path)) {
            if (errorOut) *errorOut = "failed writing " + path;
            return false;
        }
    }
    if (countOut) *countOut = written;
    return true;
}

static i32 mobTypeIdxByName(const std::string& id) {
    std::string name = id;
    if (name.rfind("minecraft:", 0) == 0) name = name.substr(10);
    for (i32 i = 0; i < entity::mobTypeCount(); ++i) {
        if (name == entity::mobDef(i).name) return i;
    }
    return -1;
}

static size_t readEntityRegions(const std::string& entitiesDir, i32 dim, extra::Snapshot& out) {
    std::error_code ec;
    if (!fs::exists(entitiesDir, ec) || !fs::is_directory(entitiesDir, ec)) return 0;

    size_t imported = 0;
    for (const auto& entry : fs::directory_iterator(entitiesDir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string fname = entry.path().filename().string();
        if (fname.size() < 5 || fname.substr(0, 2) != "r.") continue;
        if (fname.substr(fname.size() - 4) != ".mca") continue;

        RegionReader region;
        if (!region.load(entry.path().string())) continue;

        for (int lz = 0; lz < 32; ++lz) {
            for (int lx = 0; lx < 32; ++lx) {
                std::vector<u8> bytes;
                if (!region.getChunk(lx, lz, bytes)) continue;
                nbt::TagReader reader(bytes.data(), bytes.size());
                std::string rootName;
                nbt::NbtValue root;
                if (!reader.readNamedRoot(rootName, root)) continue;

                const nbt::NbtValue* list = root.get("Entities");
                if (!list || list->type != nbt::TagType::List) continue;
                for (const nbt::NbtValue& e : list->list) {
                    const std::string id = e.getString("id");
                    const nbt::NbtValue* pos = e.get("Pos");
                    if (!pos || pos->list.size() < 3) continue;
                    const f64 x = pos->list[0].asDouble;
                    const f64 y = pos->list[1].asDouble;
                    const f64 z = pos->list[2].asDouble;

                    if (id == "minecraft:item") {
                        const nbt::NbtValue* item = e.get("Item");
                        if (!item) continue;
                        extra::DropRecord d;
                        d.itemId = gen::itemIdByName(item->getString("id"));
                        d.count = item->getInt("count", 0);
                        if (d.count <= 0) d.count = static_cast<i32>(item->getByte("Count"));
                        if (d.itemId <= 0 || d.count <= 0) continue;
                        d.x = x; d.y = y; d.z = z;
                        const nbt::NbtValue* mo = e.get("Motion");
                        if (mo && mo->list.size() >= 3) {
                            d.vx = mo->list[0].asDouble;
                            d.vy = mo->list[1].asDouble;
                            d.vz = mo->list[2].asDouble;
                        }
                        d.age = e.getInt("Age", 0);
                        out.drops.push_back(d);
                        ++imported;
                        continue;
                    }

                    if (id == "minecraft:minecart" || id.find("_boat") != std::string::npos) {
                        extra::VehicleRecord v;
                        v.typeId = (id == "minecraft:minecart") ? 69 : 10;
                        v.x = x; v.y = y; v.z = z;
                        const nbt::NbtValue* rot = e.get("Rotation");
                        if (rot && !rot->list.empty()) v.yaw = static_cast<f32>(rot->list[0].asDouble);
                        out.vehicles.push_back(v);
                        ++imported;
                        continue;
                    }

                    const i32 idx = mobTypeIdxByName(id);
                    if (idx < 0) continue; // mob type this core does not implement
                    extra::MobRecord m;
                    m.typeIdx = idx;
                    m.dim = dim;
                    m.x = x; m.y = y; m.z = z;
                    const nbt::NbtValue* rot = e.get("Rotation");
                    if (rot && rot->list.size() >= 2) {
                        m.yaw = static_cast<f32>(rot->list[0].asDouble);
                        m.pitch = static_cast<f32>(rot->list[1].asDouble);
                        m.headYaw = m.yaw;
                    }
                    const nbt::NbtValue* hp = e.get("Health");
                    m.health = hp ? static_cast<i32>(hp->asDouble + 0.5) : 0;
                    m.baby = e.getByte("IsBaby") != 0;
                    m.sheared = e.getByte("Sheared") != 0;
                    m.sitting = e.getByte("Sitting") != 0;
                    m.wool = static_cast<i32>(e.getByte("Color"));
                    out.mobs.push_back(m);
                    ++imported;
                }
            }
        }
    }
    return imported;
}

// ------------------------------------------------------------------
// Public entry points
// ------------------------------------------------------------------

bool exportToVanilla(const World& world, const std::string& worldDir, const std::string& levelName,
                      i64 seed, i32 spawnX, i32 spawnY, i32 spawnZ, std::string* errorOut,
                      const extra::Snapshot* extras,
                      i32 defaultGameMode, const PlayerExport* player) {
    std::error_code ec;
    fs::create_directories(worldDir, ec);
    fs::create_directories(worldDir + "/region", ec);

    auto levelNbt = buildLevelDatNbt(levelName, seed, spawnX, spawnY, spawnZ, 0,
                                     defaultGameMode, player); // V57_GAMETYPE_V1 / V57_PLAYERTAG_V1
    auto gz = net::zlibc::gzipCompress(std::span<const u8>(levelNbt.data(), levelNbt.size()));
    {
        std::ofstream f(worldDir + "/level.dat", std::ios::binary | std::ios::trunc);
        if (!f) { if (errorOut) *errorOut = "cannot write level.dat"; return false; }
        f.write(reinterpret_cast<const char*>(gz.data()), static_cast<std::streamsize>(gz.size()));
        if (!f.good()) { if (errorOut) *errorOut = "failed writing level.dat"; return false; }
    }

    size_t chunkCount = 0, regionCount = 0;
    ExtrasByChunk grouped; // BLOCKENT_V1
    if (extras) grouped = groupExtras(*extras);
    if (!writeRegionsTo(world, worldDir + "/region", seed, &chunkCount, &regionCount, errorOut,
                        extras ? &grouped : nullptr)) return false;

    // LOGQUIET_V1: подробности экспорта в ванильный мир не нужны в консоли/логе —
    // остаются только в трассе для крашрепорта (dumpTrace).
    NC_CTRACE("Anvil: exported %zu chunk(s) across %zu region file(s) to %s", chunkCount, regionCount, worldDir.c_str());

    if (extras) { // ENTSAVE_V1
        size_t entCount = 0;
        if (!writeEntityRegions(*extras, worldDir + "/entities", 0, &entCount, errorOut)) return false;
        NC_CTRACE("Anvil: exported %zu chest(s), %zu sign(s) and %zu entity/entities",
                extras->chests.size(), extras->signs.size(), entCount);
    }
    return true;
}

// V56_DATKILL_V1: level.dat already carries the current seed and spawn point
// (written by buildLevelDatNbt on every save), so this reads them back instead
// of relying on separate seed.dat/spawn.dat files.
bool readLevelDatSeedSpawn(const std::string& worldDir, i64& seedOut,
                            i32& spawnX, i32& spawnY, i32& spawnZ) {
    std::ifstream f(worldDir + "/level.dat", std::ios::binary);
    if (!f) return false;
    std::vector<u8> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.empty()) return false;
    std::vector<u8> plain;
    if (!net::zlibc::gzipDecompress(std::span<const u8>(raw.data(), raw.size()), plain,
                                    8u * 1024 * 1024)) return false;
    nbt::TagReader reader(plain.data(), plain.size());
    std::string rootName;
    nbt::NbtValue root;
    if (!reader.readNamedRoot(rootName, root)) return false;
    const nbt::NbtValue* data = root.get("Data");
    if (!data) return false;
    const nbt::NbtValue* wgs = data->get("WorldGenSettings");
    if (wgs) seedOut = wgs->getLong("seed", seedOut);
    spawnX = data->getInt("SpawnX", spawnX);
    spawnY = data->getInt("SpawnY", spawnY);
    spawnZ = data->getInt("SpawnZ", spawnZ);
    return true;
}

bool importFromVanilla(World& world, const std::string& worldDir, std::string* errorOut,
                       extra::Snapshot* extrasOut) {
    std::string regionDir = worldDir + "/region";
    if (!fs::exists(regionDir) || !fs::is_directory(regionDir)) {
        if (errorOut) *errorOut = "no region/ folder found in " + worldDir;
        return false;
    }

    const size_t importedChunks = readRegionsFrom(world, regionDir, extrasOut);

    if (importedChunks == 0) {
        if (errorOut) *errorOut = "no chunks found in " + regionDir;
        return false;
    }
    NC_INFO("Anvil", "Imported {} chunk(s) from {}", importedChunks, worldDir);

    if (extrasOut) { // ENTSAVE_V1
        const size_t ents = readEntityRegions(entitiesDirFor(regionDir), 0, *extrasOut);
        NC_INFO("Anvil", "Imported {} chest(s), {} sign(s) and {} entity/entities",
                extrasOut->chests.size(), extrasOut->signs.size(), ents);
    }
    return true;
}

bool exportDimension(const World& world, const std::string& worldDir, Dimension dim,
                     i64 seed, std::string* errorOut, const extra::Snapshot* extras) {
    const std::string regionDir = worldDir + dimRegionSubdir(dim);
    size_t chunkCount = 0, regionCount = 0;
    // Chests and signs are keyed by position without a dimension in the core,
    // so only the overworld carries block entities.
    const bool overworld = (dim == Dimension::Overworld);
    ExtrasByChunk grouped;
    if (extras && overworld) grouped = groupExtras(*extras);
    if (!writeRegionsTo(world, regionDir, seed, &chunkCount, &regionCount, errorOut,
                        (extras && overworld) ? &grouped : nullptr)) return false;
    // LOGQUIET_V1: подробности экспорта измерения не нужны в консоли/логе — только в трассе.
    NC_CTRACE("Anvil: exported %zu chunk(s) across %zu region file(s) to %s", chunkCount, regionCount, regionDir.c_str());
    if (extras) { // ENTSAVE_V1
        size_t entCount = 0;
        if (!writeEntityRegions(*extras, entitiesDirFor(regionDir), coreDimOf(dim), &entCount, errorOut)) return false;
        if (entCount > 0) NC_CTRACE("Anvil: exported %zu entity/entities to %s", entCount, entitiesDirFor(regionDir).c_str());
    }
    return true;
}

bool importDimension(World& world, const std::string& worldDir, Dimension dim, std::string* errorOut,
                     extra::Snapshot* extrasOut) {
    std::error_code ec;
    const bool overworld = (dim == Dimension::Overworld);
    for (const auto& dir : dimRegionCandidates(worldDir, dim)) {
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) continue;
        const size_t n = readRegionsFrom(world, dir, (extrasOut && overworld) ? extrasOut : nullptr);
        if (n > 0) {
            NC_INFO("Anvil", "Imported {} chunk(s) from {}", n, dir);
            if (extrasOut) { // ENTSAVE_V1
                const size_t ents = readEntityRegions(entitiesDirFor(dir), coreDimOf(dim), *extrasOut);
                if (ents > 0) NC_INFO("Anvil", "Imported {} entity/entities from {}", ents, entitiesDirFor(dir));
            }
            return true;
        }
    }
    if (errorOut) *errorOut = "no region data found for this dimension under " + worldDir;
    return false;
}

} // namespace nc::world::anvil
