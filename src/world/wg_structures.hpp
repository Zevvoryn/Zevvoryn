#pragma once
// STRUCTURES_V1 — deterministic overworld structures.
// Placement grids/salts are taken from vanilla 1.21.1 StructureSets.java.
// The server has no NBT template / jigsaw runtime yet, therefore this first
// implementation builds compact block equivalents directly in the start chunk.
#include "chunk.hpp"
#include "worldgen.hpp"
#include <cstdint>
#include <algorithm>

namespace nc::world::structures {

// Java LegacyRandom: StructurePlacement.RandomSpread uses WorldgenRandom over it.
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
};

inline int floorDiv(int a, int b) { int q=a/b, r=a%b; return (r && ((r<0)!=(b<0))) ? q-1 : q; }
inline int spreadValue(LegacyRng& r, int width, bool triangular) {
    return triangular ? (r.nextInt(width) + r.nextInt(width)) / 2 : r.nextInt(width);
}
// Vanilla RandomSpreadStructurePlacement#getPotentialStructureChunk.
inline bool isStartChunk(int cx, int cz, uint64_t seed, int spacing, int separation, int salt, bool triangular=false) {
    int rx=floorDiv(cx,spacing), rz=floorDiv(cz,spacing);
    int64_t mixed=(int64_t)seed + (int64_t)rx*341873128712LL + (int64_t)rz*132897987541LL + (int64_t)salt;
    LegacyRng r(mixed);
    int width=spacing-separation;
    return cx==rx*spacing+spreadValue(r,width,triangular) && cz==rz*spacing+spreadValue(r,width,triangular);
}

// Global-palette block-state IDs from the vanilla 1.21.1 registry bundled with the project.
enum : i32 {
    AIR=0, STONE=1, GRASS=9, DIRT=10, COBBLE=14, OAK_PLANKS=15, SPRUCE_PLANKS=16,
    SAND=112, SANDSTONE=535, OAK_LOG=131, SPRUCE_LOG=134, OAK_LEAVES=264,
    WATER=80, LAVA=96, CHEST=2955, TORCH=2355, OBSIDIAN=2354,
    MOSSY_COBBLE=2353, STONE_BRICKS=6537, MOSSY_STONE_BRICKS=6538,
    PRISMARINE=10463, SEA_LANTERN=10724, SNOW_BLOCK=5781
};

inline int surface(const ChunkColumn& c, int wx, int wz) {
    for (int y=CHUNK_HEIGHT_MAX-1;y>=CHUNK_HEIGHT_MIN;--y) {
        int id=c.getBlock(wx,y,wz);
        if (id!=AIR && id!=WATER && id!=LAVA) return y;
    }
    return CHUNK_HEIGHT_MIN;
}
inline void put(ChunkColumn& c,int wx,int y,int wz,int id) {
    if (y>=CHUNK_HEIGHT_MIN && y<CHUNK_HEIGHT_MAX) c.setBlock(wx,y,wz,id);
}
inline void fill(ChunkColumn& c,int x0,int y0,int z0,int x1,int y1,int z1,int id) {
    for(int x=x0;x<=x1;++x) for(int z=z0;z<=z1;++z) for(int y=y0;y<=y1;++y) put(c,x,y,z,id);
}
inline bool solidAt(const ChunkColumn& c,int x,int y,int z) { int v=c.getBlock(x,y,z); return v!=AIR&&v!=WATER&&v!=LAVA; }
inline bool isOcean(int b) { using namespace wg; return b>=B_DEEP_FROZEN_OCEAN&&b<=B_LUKEWARM_OCEAN; }
inline bool isSnowy(int b) { using namespace wg; return b==B_SNOWY_PLAINS||b==B_SNOWY_TAIGA||b==B_GROVE||b==B_SNOWY_SLOPES||b==B_FROZEN_PEAKS||b==B_ICE_SPIKES; }
inline bool villageBiome(int b) { using namespace wg; return b==B_PLAINS||b==B_SUNFLOWER_PLAINS||b==B_DESERT||b==B_SAVANNA||b==B_SAVANNA_PLATEAU||b==B_TAIGA||b==B_SNOWY_PLAINS; }

inline void buriedTreasure(ChunkColumn& c,int x,int z) {
    int y=surface(c,x,z);
    put(c,x,y,z,SAND); put(c,x,y-1,z,CHEST);
    for(int dx=-1;dx<=1;++dx)for(int dz=-1;dz<=1;++dz) if(dx||dz) put(c,x+dx,y-1,z+dz,SAND);
}
inline void ruinedPortal(ChunkColumn& c,int x,int z) {
    int y=surface(c,x,z)+1;
    // Broken 4x5 obsidian frame; no portal activation block is placed.
    for(int dy=0;dy<=4;++dy) { put(c,x-2,y+dy,z,OBSIDIAN); if(dy!=2) put(c,x+2,y+dy,z,OBSIDIAN); }
    for(int dx=-2;dx<=2;++dx) { put(c,x+dx,y,z,OBSIDIAN); if(dx!=0) put(c,x+dx,y+4,z,OBSIDIAN); }
    put(c,x+3,y,z,CHEST); put(c,x-3,y,z,MOSSY_COBBLE);
}
inline void swampHut(ChunkColumn& c,int x,int z) {
    int y=std::max(63,surface(c,x,z))+1;
    // 7x7 stilt hut, intentionally compact to fit its vanilla start chunk.
    fill(c,x-3,y,z-3,x+3,y,z+3,SPRUCE_PLANKS);
    for(int dx : {-3,3}) for(int dz : {-3,3}) fill(c,x+dx,y-4,z+dz,x+dx,y,z+dz,SPRUCE_LOG);
    for(int yy=1;yy<=3;++yy) for(int d=-3;d<=3;++d) { put(c,x+d,y+yy,z-3,SPRUCE_PLANKS); put(c,x+d,y+yy,z+3,SPRUCE_PLANKS); put(c,x-3,y+yy,z+d,SPRUCE_PLANKS); put(c,x+3,y+yy,z+d,SPRUCE_PLANKS); }
    fill(c,x-4,y+4,z-4,x+4,y+4,z+4,SPRUCE_PLANKS); put(c,x,y+1,z,CHEST);
}
inline void igloo(ChunkColumn& c,int x,int z) {
    int y=surface(c,x,z)+1;
    // Vanilla igloo is an ice dome; this is a complete compact dome + buried chest.
    for(int dy=0;dy<=3;++dy) {
        int r=3-(dy>1?1:0);
        for(int dx=-r;dx<=r;++dx) for(int dz=-r;dz<=r;++dz)
            if(dx*dx+dz*dz>=r*r-1) put(c,x+dx,y+dy,z+dz,SNOW_BLOCK);
    }
    put(c,x,y,z-3,AIR); put(c,x,y-1,z,CHEST);
}
inline void jungleTemple(ChunkColumn& c,int x,int z) {
    int y=surface(c,x,z)+1;
    fill(c,x-6,y,z-6,x+6,y,z+6,COBBLE);
    for(int yy=1;yy<=6;++yy) {
        int r=6-yy/2;
        for(int d=-r;d<=r;++d) { put(c,x+d,y+yy,z-r,(yy%3)?MOSSY_COBBLE:COBBLE); put(c,x+d,y+yy,z+r,COBBLE); put(c,x-r,y+yy,z+d,COBBLE); put(c,x+r,y+yy,z+d,COBBLE); }
    }
    fill(c,x-2,y+1,z-6,x+2,y+3,z-6,AIR); put(c,x,y+1,z,CHEST); put(c,x-3,y+1,z+3,CHEST);
}
inline void desertPyramid(ChunkColumn& c,int x,int z) {
    int y=surface(c,x,z)+1;
    // 15x15 stepped pyramid matching the vanilla footprint.
    for(int level=0;level<5;++level) {
        int r=7-level;
        for(int dx=-r;dx<=r;++dx) for(int dz=-r;dz<=r;++dz) {
            bool edge=dx==-r||dx==r||dz==-r||dz==r||level==0;
            if(edge) put(c,x+dx,y+level,z+dz,(level==0&&((dx+dz)&3)==0)?SANDSTONE:SAND);
        }
    }
    fill(c,x-1,y,z-1,x+1,y+4,z+1,AIR);
    put(c,x,y-2,z,CHEST); put(c,x-2,y-2,z,CHEST); put(c,x+2,y-2,z,CHEST); put(c,x,y-2,z+2,CHEST);
}
inline void villageHouse(ChunkColumn& c,int x,int z,int biome) {
    int y=surface(c,x,z)+1; int wall=(biome==wg::B_DESERT)?SANDSTONE:(biome==wg::B_TAIGA?SPRUCE_PLANKS:OAK_PLANKS);
    fill(c,x-4,y,z-4,x+4,y,z+4,wall);
    for(int yy=1;yy<=3;++yy) for(int d=-4;d<=4;++d) { put(c,x+d,y+yy,z-4,wall); put(c,x+d,y+yy,z+4,wall); put(c,x-4,y+yy,z+d,wall); put(c,x+4,y+yy,z+d,wall); }
    fill(c,x-5,y+4,z-5,x+5,y+4,z+5,(biome==wg::B_DESERT)?SANDSTONE:OAK_PLANKS);
    fill(c,x-1,y+1,z-4,x+1,y+2,z-4,AIR); put(c,x+2,y+1,z+2,CHEST);
}
inline void outpost(ChunkColumn& c,int x,int z) {
    int y=surface(c,x,z)+1;
    for(int dx : {-3,3}) for(int dz : {-3,3}) fill(c,x+dx,y,z+dz,x+dx,y+10,z+dz,SPRUCE_LOG);
    for(int yy=3;yy<=9;yy+=3) fill(c,x-4,y+yy,z-4,x+4,y+yy,z+4,SPRUCE_PLANKS);
    fill(c,x-5,y+10,z-5,x+5,y+10,z+5,SPRUCE_PLANKS); put(c,x,y+4,z,CHEST);
}
inline void shipwreck(ChunkColumn& c,int x,int z) {
    int y=std::min(62,surface(c,x,z)+1);
    for(int dz=-6;dz<=6;++dz) { int w=3-std::abs(dz)/3; for(int dx=-w;dx<=w;++dx) put(c,x+dx,y,z+dz,OAK_PLANKS); }
    for(int dz=-4;dz<=4;++dz) { put(c,x-2,y+1,z+dz,OAK_PLANKS); put(c,x+2,y+1,z+dz,OAK_PLANKS); }
    fill(c,x,y+1,z-1,x,y+7,z-1,OAK_LOG); put(c,x,y+1,z+3,CHEST);
}
inline void oceanRuin(ChunkColumn& c,int x,int z) {
    int y=surface(c,x,z)+1;
    for(int dx=-3;dx<=3;++dx) for(int dz=-3;dz<=3;++dz) if(std::abs(dx)==3||std::abs(dz)==3) put(c,x+dx,y,z+dz,(dx+dz)&1?MOSSY_COBBLE:COBBLE);
    put(c,x,y+1,z,CHEST);
}

inline void place(ChunkColumn& c,const wg::BiomeSource& biomes,uint64_t seed,int cx,int cz) {
    using namespace wg;
    const int wx=cx*16+8, wz=cz*16+8;
    const int biome=biomes.getBiomeAtBlock(wx,64,wz);
    // Vanilla StructureSets.java settings: spacing, separation, salt, spread type.
    if (biome==B_DESERT && isStartChunk(cx,cz,seed,32,8,14357617)) { desertPyramid(c,wx,wz); return; }
    if (isSnowy(biome) && isStartChunk(cx,cz,seed,32,8,14357618)) { igloo(c,wx,wz); return; }
    if ((biome==B_JUNGLE||biome==B_BAMBOO_JUNGLE) && isStartChunk(cx,cz,seed,32,8,14357619)) { jungleTemple(c,wx,wz); return; }
    if ((biome==B_SWAMP||biome==B_MANGROVE_SWAMP) && isStartChunk(cx,cz,seed,32,8,14357620)) { swampHut(c,wx,wz); return; }
    if (villageBiome(biome) && isStartChunk(cx,cz,seed,20,6,10387312)) { villageHouse(c,wx,wz,biome); return; }
    if (!isOcean(biome) && isStartChunk(cx,cz,seed,28,10,34222645)) { ruinedPortal(c,wx,wz); return; }
    if (isOcean(biome) && isStartChunk(cx,cz,seed,24,4,165745295)) { shipwreck(c,wx,wz); return; }
    if (isOcean(biome) && isStartChunk(cx,cz,seed,20,8,14357621)) { oceanRuin(c,wx,wz); return; }
    if ((biome==B_BEACH||biome==B_SNOWY_BEACH) && isStartChunk(cx,cz,seed,1,0,0)) {
        // Vanilla buried treasure also has legacy frequency reduction 1%; deterministic equivalent.
        LegacyRng r((int64_t)seed + (int64_t)cx*341873128712LL + (int64_t)cz*132897987541LL);
        if(r.nextInt(100)==0) buriedTreasure(c,wx,wz);
        return;
    }
    if (!isOcean(biome) && isStartChunk(cx,cz,seed,24,7,165745296) && biome!=B_DESERT) outpost(c,wx,wz);
}

} // namespace nc::world::structures
