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
#include "../core/log.hpp"

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <algorithm>
#include <cstring>

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

static std::string stateIdToName(i32 stateId) {
    const auto* st = RegistryManager::instance().blockStates().getById(stateId);
    return st ? st->name : std::string("minecraft:air");
}

static i32 nameToStateId(const std::string& name) {
    auto id = RegistryManager::instance().getBlockStateId(name);
    if (id) return *id;
    if (name == "minecraft:air" || name == "minecraft:cave_air" || name == "minecraft:void_air") return 0;
    return 0; // unknown block name -> air (best effort, see anvil.hpp limitations)
}

// ------------------------------------------------------------------
// Chunk NBT: export (our ChunkColumn -> vanilla chunk NBT bytes)
// ------------------------------------------------------------------

static std::vector<u8> buildChunkNbt(const ChunkColumn& chunk, i64 seed) {
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
        const ChunkSection& section = chunk.getSection(s);
        i32 sectionY = CHUNK_HEIGHT_MIN / SECTION_HEIGHT + s;

        // Build a local block palette (order of first appearance).
        std::vector<std::string> palette;
        std::unordered_map<i32, i32> stateToPaletteIdx;
        std::vector<i32> indices;
        indices.reserve(BLOCKS_PER_SECTION);
        for (i32 y = 0; y < SECTION_HEIGHT; ++y) {
            for (i32 z = 0; z < SECTION_WIDTH; ++z) {
                for (i32 x = 0; x < SECTION_WIDTH; ++x) {
                    i32 stateId = section.getBlock(x, y, z);
                    auto it = stateToPaletteIdx.find(stateId);
                    i32 pIdx;
                    if (it == stateToPaletteIdx.end()) {
                        pIdx = static_cast<i32>(palette.size());
                        palette.push_back(stateIdToName(stateId));
                        stateToPaletteIdx.emplace(stateId, pIdx);
                    } else {
                        pIdx = it->second;
                    }
                    indices.push_back(pIdx);
                }
            }
        }
        if (palette.empty()) palette.push_back("minecraft:air");

        w.beginListElementCompound();
        w.writeByte(sectionY, "Y");

        w.beginCompound("block_states");
        w.beginList("palette", nbt::TagType::Compound, static_cast<i32>(palette.size()));
        for (const auto& name : palette) {
            w.beginListElementCompound();
            w.writeString(name, "Name");
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

    w.beginList("block_entities", nbt::TagType::Compound, 0);
    w.endList();

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
            paletteIds.push_back(nameToStateId(name));
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

static std::vector<u8> buildLevelDatNbt(const std::string& levelName, i64 seed,
                                         i32 spawnX, i32 spawnY, i32 spawnZ, i64 dayTime) {
    nbt::TagWriter w;
    w.beginRootCompoundNamed("");
    w.beginCompound("Data");
    w.writeInt(DATA_VERSION_1_21_1, "DataVersion");
    w.writeString(levelName, "LevelName");
    w.writeInt(spawnX, "SpawnX");
    w.writeInt(spawnY, "SpawnY");
    w.writeInt(spawnZ, "SpawnZ");
    w.writeFloat(0.0f, "SpawnAngle");
    w.writeLong(dayTime, "Time");
    w.writeLong(dayTime, "DayTime");
    w.writeLong(0, "LastPlayed");
    w.writeByte(0, "hardcore");
    w.writeInt(0, "GameType"); // 0 = survival
    w.writeByte(0, "Difficulty");
    w.writeByte(0, "DifficultyLocked");
    w.writeByte(0, "raining");
    w.writeInt(0, "rainTime");
    w.writeByte(0, "thundering");
    w.writeInt(0, "thunderTime");
    w.writeByte(1, "initialized");
    w.writeByte(1, "allowCommands");

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
// Public entry points
// ------------------------------------------------------------------

bool exportToVanilla(const World& world, const std::string& worldDir, const std::string& levelName,
                      i64 seed, i32 spawnX, i32 spawnY, i32 spawnZ, std::string* errorOut) {
    std::error_code ec;
    fs::create_directories(worldDir, ec);
    fs::create_directories(worldDir + "/region", ec);

    auto levelNbt = buildLevelDatNbt(levelName, seed, spawnX, spawnY, spawnZ, 0);
    auto gz = net::zlibc::gzipCompress(std::span<const u8>(levelNbt.data(), levelNbt.size()));
    {
        std::ofstream f(worldDir + "/level.dat", std::ios::binary | std::ios::trunc);
        if (!f) { if (errorOut) *errorOut = "cannot write level.dat"; return false; }
        f.write(reinterpret_cast<const char*>(gz.data()), static_cast<std::streamsize>(gz.size()));
        if (!f.good()) { if (errorOut) *errorOut = "failed writing level.dat"; return false; }
    }

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
    for (const auto& [key, chunkList] : regionChunks) {
        i32 rx = static_cast<i32>(key >> 32);
        i32 rz = static_cast<i32>(key & 0xFFFFFFFFu);
        RegionWriter region;
        for (const auto& chunkPtr : chunkList) {
            auto chunkNbt = buildChunkNbt(*chunkPtr, seed);
            auto zlibBytes = net::zlibc::compress(std::span<const u8>(chunkNbt.data(), chunkNbt.size()));
            int localX = ((chunkPtr->getX() % 32) + 32) % 32;
            int localZ = ((chunkPtr->getZ() % 32) + 32) % 32;
            region.putChunk(localX, localZ, zlibBytes, /*compressionType=*/2, timestamp);
        }
        std::string regionPath = worldDir + "/region/r." + std::to_string(rx) + "." + std::to_string(rz) + ".mca";
        if (!region.save(regionPath)) { if (errorOut) *errorOut = "failed writing " + regionPath; return false; }
    }

    NC_INFO("Anvil", "Exported {} chunk(s) across {} region file(s) to {}",
        world.getAllChunks().size(), regionChunks.size(), worldDir);
    return true;
}

bool importFromVanilla(World& world, const std::string& worldDir, std::string* errorOut) {
    std::string regionDir = worldDir + "/region";
    if (!fs::exists(regionDir) || !fs::is_directory(regionDir)) {
        if (errorOut) *errorOut = "no region/ folder found in " + worldDir;
        return false;
    }

    size_t importedChunks = 0;
    for (const auto& entry : fs::directory_iterator(regionDir)) {
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
                ++importedChunks;
            }
        }
    }

    if (importedChunks == 0) {
        if (errorOut) *errorOut = "no chunks found in " + regionDir;
        return false;
    }
    NC_INFO("Anvil", "Imported {} chunk(s) from {}", importedChunks, worldDir);
    return true;
}

} // namespace nc::world::anvil
