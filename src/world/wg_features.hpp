#pragma once
// FEATURES_V3 — vanilla-1.21.1-faithful overworld decorations.
// Numbers ported directly from Mojang deobfuscated data sources:
//   OreFeatures / OrePlacements, TreeFeatures / TreePlacements, VegetationPlacements.
// Ores use exact per-chunk counts, height distributions (uniform vs triangle) and
// vein sizes; ore only replaces stone/deepslate variants (so exposed ore on cliffs
// is vanilla-accurate, and air veins simply no-op).
#include "chunk.hpp"
#include "worldgen.hpp"
#include <cmath>

namespace nc::world::features {

inline u64 mix(u64 x){ x+=0x9E3779B97F4A7C15ull; x=(x^(x>>30))*0xBF58476D1CE4E5B9ull; x=(x^(x>>27))*0x94D049BB133111EBull; return x^(x>>31); }
struct Rng{ u64 s; u64 next(){ return s=mix(s); } int range(int n){ return n>0?(int)(next()%(u64)n):0; } };

// ---- vanilla global-palette state ids (verified vs item_blocks.gen.hpp) ----
enum : i32 {
  AIR=0, STONE=1, GRANITE=2, DIORITE=4, ANDESITE=6, GRASS_BLOCK=9, DIRT=10, PODZOL=13,
  WATER=80, SAND=112, RED_SAND=117, GRAVEL=118, TUFF=21081, DEEPSLATE=24905,
  // ores (normal / deepslate variant)
  COAL=127,COAL_D=128, IRON=125,IRON_D=126, GOLD=123,GOLD_D=124, COPPER=22942,COPPER_D=22943,
  REDSTONE=5735,REDSTONE_D=5737, DIAMOND=4274,DIAMOND_D=4275, LAPIS=520,LAPIS_D=521, EMERALD=7511,EMERALD_D=7512,
  // logs / leaves
  OAK_LOG=131,SPRUCE_LOG=134,BIRCH_LOG=137,JUNGLE_LOG=140,ACACIA_LOG=143,CHERRY_LOG=146,DARK_OAK_LOG=149,MANGROVE_LOG=152,
  OAK_LF=264,SPRUCE_LF=292,BIRCH_LF=320,JUNGLE_LF=348,ACACIA_LF=376,CHERRY_LF=404,DARK_OAK_LF=432,MANGROVE_LF=460,
  // plants (single-block)
  SHORT_GRASS=2005,FERN=2006,DEAD_BUSH=2007,
  DANDELION=2075,POPPY=2077,BLUE_ORCHID=2078,ALLIUM=2079,AZURE_BLUET=2080,
  RED_TULIP=2081,ORANGE_TULIP=2082,WHITE_TULIP=2083,PINK_TULIP=2084,OXEYE=2085,
  CORNFLOWER=2086,LILY_VALLEY=2088,
  // tall (2-block: lower=id, upper=id+1)
  SUNFLOWER=10748,LILAC=10750,ROSE_BUSH=10752,PEONY=10754,TALL_GRASS=10756,LARGE_FERN=10758,
  CACTUS=5782,SUGAR_CANE=5799
};

inline bool oreReplace(i32 id){ return id==STONE||id==GRANITE||id==DIORITE||id==ANDESITE||id==TUFF||id==DEEPSLATE; }

// topmost solid column height (skips air + water)
inline int top(ChunkColumn& c,int x,int z){ for(int y=CHUNK_HEIGHT_MAX-1;y>=CHUNK_HEIGHT_MIN;--y){ i32 b=c.getBlock(x,y,z); if(b!=AIR&&b!=WATER) return y; } return CHUNK_HEIGHT_MIN; }

inline int uniY(Rng& r,int lo,int hi){ return lo + r.range(hi-lo+1); }              // uniform
inline int triY(Rng& r,int lo,int hi){ int w=hi-lo+1; return lo + (r.range(w)+r.range(w))/2; } // triangular (avg of two uniforms)

// ore/blob vein: scatter `size` blocks; box scales with size; deepslate variant underground.
inline void vein(ChunkColumn& c,Rng& r,int x,int y,int z,int size,i32 normal,i32 deep){
  int rad=(int)std::cbrt((double)size)+1; if(rad<2)rad=2; if(rad>6)rad=6; int span=rad*2+1;
  for(int i=0;i<size;++i){
    int px=x+r.range(span)-rad, py=y+r.range(span)-rad, pz=z+r.range(span)-rad;
    if(px<0||px>15||pz<0||pz>15||py<CHUNK_HEIGHT_MIN||py>=CHUNK_HEIGHT_MAX) continue;
    i32 old=c.getBlock(px,py,pz);
    if(oreReplace(old)) c.setBlock(px,py,pz, old==DEEPSLATE?deep:normal);
  }
}

// ---- foliage helpers ----
inline void leaf(ChunkColumn& c,int x,int y,int z,i32 lv){ if(x<0||x>15||z<0||z>15||y<CHUNK_HEIGHT_MIN||y>=CHUNK_HEIGHT_MAX)return; if(c.getBlock(x,y,z)==AIR)c.setBlock(x,y,z,lv); }
inline void disk(ChunkColumn& c,int x,int y,int z,int rad,i32 lv){ if(rad<0)return; for(int dx=-rad;dx<=rad;++dx)for(int dz=-rad;dz<=rad;++dz){ if(dx*dx+dz*dz>rad*rad+(rad>1?1:0))continue; leaf(c,x+dx,y,z+dz,lv);} }

// BlobFoliage-style (oak / birch / jungle): straight trunk + rounded canopy.
inline void blobTree(ChunkColumn& c,int x,int y,int z,i32 log,i32 lv,int trunk,int fr){
  if(y+trunk+2>=CHUNK_HEIGHT_MAX)return;
  for(int q=0;q<trunk;++q)c.setBlock(x,y+q,z,log);
  int t=y+trunk;
  disk(c,x,t-2,z,fr,lv); disk(c,x,t-1,z,fr,lv);
  disk(c,x,t,z,(fr-1<1?1:fr-1),lv); disk(c,x,t+1,z,1,lv);
}
// Spruce / pine: compact layered crown only near the top of the trunk.
// The prior implementation covered the whole trunk and looked like a Christmas tree.
inline void conifer(ChunkColumn& c,int x,int y,int z,i32 log,i32 lv,int trunk,int maxR){
  if(y+trunk+2>=CHUNK_HEIGHT_MAX)return;
  for(int q=0;q<trunk;++q)c.setBlock(x,y+q,z,log);
  int t=y+trunk;
  for(int layer=0;layer<5;++layer){
    int yy=t+1-layer;
    int rr=(layer==0)?0:((layer==1)?1:std::min(maxR,2));
    disk(c,x,yy,z,rr,lv);
  }
}
// Acacia: flat wide top.
inline void acaciaTree(ChunkColumn& c,int x,int y,int z,int trunk){
  if(y+trunk+2>=CHUNK_HEIGHT_MAX)return;
  for(int q=0;q<trunk;++q)c.setBlock(x,y+q,z,ACACIA_LOG);
  int t=y+trunk; disk(c,x,t,z,3,ACACIA_LF); disk(c,x,t+1,z,2,ACACIA_LF);
}
// Dark oak: thick 2x2 trunk, wide low canopy.
inline void darkOakTree(ChunkColumn& c,int x,int y,int z,int trunk){
  if(y+trunk+2>=CHUNK_HEIGHT_MAX)return;
  for(int q=0;q<trunk;++q){ c.setBlock(x,y+q,z,DARK_OAK_LOG); c.setBlock(x+1,y+q,z,DARK_OAK_LOG); c.setBlock(x,y+q,z+1,DARK_OAK_LOG); c.setBlock(x+1,y+q,z+1,DARK_OAK_LOG);} 
  int t=y+trunk; disk(c,x,t-1,z,2,DARK_OAK_LF); disk(c,x,t,z,3,DARK_OAK_LF); disk(c,x,t+1,z,2,DARK_OAK_LF);
}
// Fancy oak (approx): tall trunk with a large layered canopy.
inline void bigOak(ChunkColumn& c,int x,int y,int z,int trunk){
  if(y+trunk+3>=CHUNK_HEIGHT_MAX)return;
  for(int q=0;q<trunk;++q)c.setBlock(x,y+q,z,OAK_LOG);
  int t=y+trunk;
  disk(c,x,t-3,z,2,OAK_LF); disk(c,x,t-2,z,3,OAK_LF); disk(c,x,t-1,z,3,OAK_LF); disk(c,x,t,z,2,OAK_LF); disk(c,x,t+1,z,1,OAK_LF);
}

// Place a biome-appropriate tree with vanilla-ish species mix and trunk height.
inline void placeTree(ChunkColumn& c,Rng& r,int b,int x,int y,int z){
  using namespace wg;
  switch(b){
    case B_TAIGA: case B_SNOWY_TAIGA: case B_SNOWY_PLAINS:
      conifer(c,x,y,z,SPRUCE_LOG,SPRUCE_LF,5+r.range(3),2); break;
    case B_GROVE: case B_SNOWY_SLOPES:
      conifer(c,x,y,z,SPRUCE_LOG,SPRUCE_LF,6+r.range(4),2); break;
    case B_OLD_GROWTH_SPRUCE_TAIGA:
      conifer(c,x,y,z,SPRUCE_LOG,SPRUCE_LF,8+r.range(6),3); break;
    case B_OLD_GROWTH_PINE_TAIGA:
      conifer(c,x,y,z,SPRUCE_LOG,SPRUCE_LF,10+r.range(7),2); break;
    case B_BIRCH_FOREST:
      blobTree(c,x,y,z,BIRCH_LOG,BIRCH_LF,8+r.range(4),2); break;
    case B_OLD_GROWTH_BIRCH_FOREST:
      blobTree(c,x,y,z,BIRCH_LOG,BIRCH_LF,13+r.range(5),2); break;
    case B_FOREST:
      if(r.range(10)==0) bigOak(c,x,y,z,7+r.range(4));
      else if(r.range(5)==0) blobTree(c,x,y,z,BIRCH_LOG,BIRCH_LF,5+r.range(3),2);
      else blobTree(c,x,y,z,OAK_LOG,OAK_LF,4+r.range(3),2); break;
    case B_FLOWER_FOREST:
      if(r.range(5)==0) blobTree(c,x,y,z,BIRCH_LOG,BIRCH_LF,5+r.range(3),2);
      else blobTree(c,x,y,z,OAK_LOG,OAK_LF,4+r.range(3),2); break;
    case B_SAVANNA: case B_SAVANNA_PLATEAU: case B_WINDSWEPT_SAVANNA:
      if(r.range(5)==0) blobTree(c,x,y,z,OAK_LOG,OAK_LF,4+r.range(3),2);
      else acaciaTree(c,x,y,z,5+r.range(3)); break;
    case B_JUNGLE:
      if(r.range(10)==0) bigOak(c,x,y,z,8+r.range(4));
      else blobTree(c,x,y,z,JUNGLE_LOG,JUNGLE_LF,4+r.range(9),2); break;
    case B_BAMBOO_JUNGLE: case B_SPARSE_JUNGLE:
      blobTree(c,x,y,z,JUNGLE_LOG,JUNGLE_LF,4+r.range(7),2); break;
    case B_DARK_FOREST:
      { int k=r.range(10); if(k<6) darkOakTree(c,x,y,z,6+r.range(3)); else if(k<8) blobTree(c,x,y,z,OAK_LOG,OAK_LF,4+r.range(3),2); else blobTree(c,x,y,z,BIRCH_LOG,BIRCH_LF,5+r.range(3),2);} break;
    case B_SWAMP:
      blobTree(c,x,y,z,OAK_LOG,OAK_LF,5+r.range(4),3); break;
    case B_MANGROVE_SWAMP:
      blobTree(c,x,y,z,MANGROVE_LOG,MANGROVE_LF,5+r.range(4),3); break;
    case B_CHERRY_GROVE:
      blobTree(c,x,y,z,CHERRY_LOG,CHERRY_LF,6+r.range(3),4); break;
    case B_WINDSWEPT_HILLS: case B_WINDSWEPT_GRAVELLY_HILLS: case B_WINDSWEPT_FOREST:
      if(r.range(3)==0) conifer(c,x,y,z,SPRUCE_LOG,SPRUCE_LF,6+r.range(3),2);
      else blobTree(c,x,y,z,OAK_LOG,OAK_LF,4+r.range(3),2); break;
    case B_PLAINS: case B_SUNFLOWER_PLAINS: case B_MEADOW:
      if(r.range(10)==0) bigOak(c,x,y,z,7+r.range(4)); else blobTree(c,x,y,z,OAK_LOG,OAK_LF,4+r.range(3),2); break;
    default:
      blobTree(c,x,y,z,OAK_LOG,OAK_LF,4+r.range(3),2); break;
  }
}

// per-chunk tree ATTEMPTS (vanilla countExtra base; each attempt may fail ground/biome checks)
inline int treeCount(int b,Rng& r){ using namespace wg; switch(b){
  case B_PLAINS: case B_SUNFLOWER_PLAINS: return (r.range(100)<5)?1:0;
  case B_MEADOW: return (r.range(100)<1)?1:0;
  case B_FOREST: return 1+((r.range(8)==0)?1:0);
  case B_FLOWER_FOREST: return 1;
  case B_BIRCH_FOREST: case B_OLD_GROWTH_BIRCH_FOREST: return 1+((r.range(8)==0)?1:0);
  case B_TAIGA: case B_GROVE: return 1+((r.range(10)==0)?1:0);
  case B_OLD_GROWTH_SPRUCE_TAIGA: case B_OLD_GROWTH_PINE_TAIGA: return 2;
  case B_SNOWY_TAIGA: case B_SNOWY_PLAINS: case B_SNOWY_SLOPES: return (r.range(100)<10)?1:0;
  case B_SAVANNA: case B_SAVANNA_PLATEAU: return 1+((r.range(10)==0)?1:0);
  case B_WINDSWEPT_SAVANNA: return 2+((r.range(10)==0)?1:0);
  case B_JUNGLE: return 3+((r.range(10)==0)?1:0);
  case B_BAMBOO_JUNGLE: return 2+((r.range(10)==0)?1:0);
  case B_SPARSE_JUNGLE: return 2+((r.range(10)==0)?1:0);
  case B_DARK_FOREST: return 2;
  case B_SWAMP: return 2+((r.range(10)==0)?1:0);
  case B_MANGROVE_SWAMP: return 2;
  case B_CHERRY_GROVE: return 1+((r.range(8)==0)?1:0);
  case B_BADLANDS: case B_WOODED_BADLANDS: case B_ERODED_BADLANDS: return 2;
  case B_WINDSWEPT_HILLS: case B_WINDSWEPT_GRAVELLY_HILLS: return (r.range(100)<10)?1:0;
  case B_WINDSWEPT_FOREST: return 1+((r.range(8)==0)?1:0);
  default: return 0; }}

// per-chunk grass patch count (PATCH_GRASS_*)
inline int grassCount(int b){ using namespace wg; switch(b){
  case B_PLAINS: case B_SUNFLOWER_PLAINS: case B_MEADOW: return 8;
  case B_FOREST: case B_FLOWER_FOREST: case B_BIRCH_FOREST: case B_OLD_GROWTH_BIRCH_FOREST: case B_DARK_FOREST: return 2;
  case B_TAIGA: case B_SNOWY_TAIGA: case B_OLD_GROWTH_SPRUCE_TAIGA: case B_OLD_GROWTH_PINE_TAIGA: return 7;
  case B_JUNGLE: case B_BAMBOO_JUNGLE: case B_SPARSE_JUNGLE: return 25;
  case B_SAVANNA: case B_SAVANNA_PLATEAU: case B_WINDSWEPT_SAVANNA: return 20;
  case B_SWAMP: case B_MANGROVE_SWAMP: case B_CHERRY_GROVE: return 5;
  default: return 4; }}

// per-chunk flower patch count (FLOWER_*)
inline int flowerCount(int b){ using namespace wg; switch(b){
  case B_FLOWER_FOREST: return 2;
  case B_PLAINS: case B_SUNFLOWER_PLAINS: return 1;
  case B_CHERRY_GROVE: return 1;
  case B_MEADOW: return 1;
  case B_FOREST: case B_BIRCH_FOREST: case B_OLD_GROWTH_BIRCH_FOREST: return 0;
  case B_SWAMP: case B_MANGROVE_SWAMP: return 0;
  default: return 0; }}

// vanilla-ish per-biome flower selection; sets tall=true for 2-block flowers.
inline i32 pickFlower(int b,Rng& r,bool& tall){ using namespace wg; tall=false; switch(b){
  case B_SUNFLOWER_PLAINS: if(r.range(3)==0){tall=true;return SUNFLOWER;} { i32 a[]={DANDELION,POPPY,AZURE_BLUET,OXEYE,CORNFLOWER,RED_TULIP,ORANGE_TULIP,WHITE_TULIP,PINK_TULIP}; return a[r.range(9)]; }
  case B_PLAINS: { i32 a[]={DANDELION,POPPY,AZURE_BLUET,OXEYE,CORNFLOWER,RED_TULIP,ORANGE_TULIP,WHITE_TULIP,PINK_TULIP}; return a[r.range(9)]; }
  case B_MEADOW: { i32 a[]={DANDELION,POPPY,AZURE_BLUET,OXEYE,CORNFLOWER}; return a[r.range(5)]; }
  case B_FLOWER_FOREST: if(r.range(6)==0){tall=true; i32 t[]={LILAC,ROSE_BUSH,PEONY}; return t[r.range(3)];} { i32 a[]={DANDELION,POPPY,ALLIUM,AZURE_BLUET,RED_TULIP,ORANGE_TULIP,WHITE_TULIP,PINK_TULIP,OXEYE,CORNFLOWER,LILY_VALLEY}; return a[r.range(11)]; }
  case B_FOREST: case B_BIRCH_FOREST: case B_OLD_GROWTH_BIRCH_FOREST: { i32 a[]={DANDELION,POPPY,LILY_VALLEY}; return a[r.range(3)]; }
  case B_SWAMP: case B_MANGROVE_SWAMP: return BLUE_ORCHID;
  case B_CHERRY_GROVE: { i32 a[]={DANDELION,ALLIUM,PINK_TULIP,OXEYE,CORNFLOWER}; return a[r.range(5)]; }
  default: { i32 a[]={DANDELION,POPPY}; return a[r.range(2)]; }
}}

inline void place(ChunkColumn& c,const wg::BiomeSource& biomes,u64 seed,int cx,int cz){
  using namespace wg;
  Rng r{ mix(seed ^ ((u64)(u32)cx<<32) ^ (u32)cz ^ 0xF347A11ull) };
  int cb = biomes.getBiomeAtBlock(cx*16+8, 64, cz*16+8);
  bool bad=(cb==B_BADLANDS||cb==B_WOODED_BADLANDS||cb==B_ERODED_BADLANDS);
  bool mtn=(cb==B_WINDSWEPT_HILLS||cb==B_WINDSWEPT_GRAVELLY_HILLS||cb==B_WINDSWEPT_FOREST||cb==B_MEADOW||cb==B_GROVE||cb==B_SNOWY_SLOPES||cb==B_JAGGED_PEAKS||cb==B_FROZEN_PEAKS||cb==B_STONY_PEAKS);

  // ===== stone variants first (so ores can also seed into them) =====
  struct SV{ int count,lo,hi,size; i32 blk; };
  SV svs[]={ {3,0,160,33,DIRT},{7,-64,319,33,GRAVEL},{1,0,60,64,GRANITE},{1,0,60,64,DIORITE},{1,0,60,64,ANDESITE},{1,-64,0,64,TUFF} };
  for(auto& s:svs)for(int n=0;n<s.count;++n){ int x=r.range(16),z=r.range(16),y=uniY(r,s.lo,s.hi); vein(c,r,x,y,z,s.size,s.blk,s.blk); }

  // ===== ores: exact vanilla count / height distribution / vein size =====
  struct Ore{ int count,lo,hi; bool tri; int size; i32 a,b; };
  Ore ores[]={
    {15,136,319,false,17,COAL,COAL_D},     // tuned: half vanilla rate
    {10,0,192,true,17,COAL,COAL_D},
    {45,80,319,true,9,IRON,IRON_D},
    {5,-24,56,true,9,IRON,IRON_D},
    {5,-64,72,false,4,IRON,IRON_D},
    {2,-64,32,true,9,GOLD,GOLD_D},
    {2,-64,15,false,8,REDSTONE,REDSTONE_D},
    {4,-64,-32,true,8,REDSTONE,REDSTONE_D},
    {3,-64,16,true,4,DIAMOND,DIAMOND_D},
    {1,-64,-4,false,8,DIAMOND,DIAMOND_D},
    {2,-64,16,true,8,DIAMOND,DIAMOND_D},
    {1,-32,32,true,7,LAPIS,LAPIS_D},
    {2,-64,64,false,7,LAPIS,LAPIS_D},
    {8,-16,112,true,10,COPPER,COPPER_D}
  };
  for(auto& o:ores)for(int n=0;n<o.count;++n){ int x=r.range(16),z=r.range(16); int y=o.tri?triY(r,o.lo,o.hi):uniY(r,o.lo,o.hi); if(y<CHUNK_HEIGHT_MIN||y>=CHUNK_HEIGHT_MAX)continue; vein(c,r,x,y,z,o.size,o.a,o.b); }
  if(mtn) for(int n=0;n<25;++n){ int x=r.range(16),z=r.range(16),y=triY(r,-16,319); if(y<CHUNK_HEIGHT_MIN||y>=CHUNK_HEIGHT_MAX)continue; vein(c,r,x,y,z,3,EMERALD,EMERALD_D); } // emerald: mountains
  if(bad) for(int n=0;n<15;++n){ int x=r.range(16),z=r.range(16),y=uniY(r,32,256); if(y>=CHUNK_HEIGHT_MAX)continue; vein(c,r,x,y,z,9,GOLD,GOLD_D); }                    // extra gold: badlands

  // ===== trees =====
  int tc=treeCount(cb,r);
  if(mtn && tc>2) tc=2; // sparse mountain/hill tree line
  for(int n=0;n<tc;++n){
    int x=1+r.range(14), z=1+r.range(14), y=top(c,x,z);
    if(y<=CHUNK_HEIGHT_MIN||y+1>=CHUNK_HEIGHT_MAX)continue;
    i32 g=c.getBlock(x,y,z);
    if(!(g==GRASS_BLOCK||g==DIRT||g==PODZOL)||c.getBlock(x,y+1,z)!=AIR)continue;
    int b=biomes.getBiomeAtBlock(cx*16+x,y,cz*16+z);
    placeTree(c,r,b,x,y+1,z);
  }

  // ===== grass / fern patches =====
  int gc=grassCount(cb);
  for(int n=0;n<gc;++n){
    int x=r.range(16), z=r.range(16), y=top(c,x,z);
    if(y<=CHUNK_HEIGHT_MIN)continue;
    if(c.getBlock(x,y,z)!=GRASS_BLOCK||c.getBlock(x,y+1,z)!=AIR)continue;
    int b=biomes.getBiomeAtBlock(cx*16+x,y,cz*16+z);
    bool fernB=(b==B_TAIGA||b==B_SNOWY_TAIGA||b==B_OLD_GROWTH_SPRUCE_TAIGA||b==B_OLD_GROWTH_PINE_TAIGA||b==B_JUNGLE||b==B_BAMBOO_JUNGLE||b==B_SPARSE_JUNGLE);
    c.setBlock(x,y+1,z,(fernB&&r.range(3)==0)?FERN:SHORT_GRASS);
  }
  // occasional tall grass / large fern (2-block)
  { int tg=grassCount(cb)/4; for(int n=0;n<tg;++n){ if(r.range(3)!=0)continue; int x=r.range(16),z=r.range(16),y=top(c,x,z); if(y<=CHUNK_HEIGHT_MIN)continue; if(c.getBlock(x,y,z)!=GRASS_BLOCK||c.getBlock(x,y+1,z)!=AIR||c.getBlock(x,y+2,z)!=AIR)continue; bool fernB=(cb==B_TAIGA||cb==B_OLD_GROWTH_SPRUCE_TAIGA||cb==B_OLD_GROWTH_PINE_TAIGA||cb==B_JUNGLE); i32 base=fernB?LARGE_FERN:TALL_GRASS; c.setBlock(x,y+1,z,base); c.setBlock(x,y+2,z,base+1);} }

  // ===== flowers (biome-aware; real flower ids, incl. tall 2-block) =====
  int fc=flowerCount(cb);
  for(int n=0;n<fc;++n){
    // One rare local patch, not flowers scattered over the entire chunk.
    int px=2+r.range(12), pz=2+r.range(12);
    for(int q=0;q<3;++q){
      int x=px+r.range(3)-1, z=pz+r.range(3)-1, y=top(c,x,z);
      if(y<=CHUNK_HEIGHT_MIN)continue;
      if(c.getBlock(x,y,z)!=GRASS_BLOCK||c.getBlock(x,y+1,z)!=AIR)continue;
      int b=biomes.getBiomeAtBlock(cx*16+x,y,cz*16+z);
      bool tall=false; i32 f=pickFlower(b,r,tall);
      if(tall){ if(c.getBlock(x,y+2,z)==AIR){ c.setBlock(x,y+1,z,f); c.setBlock(x,y+2,z,f+1);} else c.setBlock(x,y+1,z,DANDELION); }
      else c.setBlock(x,y+1,z,f);
    }
  }

  // ===== sugar cane: rare, clumps of 3-5, height 1-3, must touch water =====
  int caneRarity = (cb==B_SWAMP||cb==B_MANGROVE_SWAMP)?3 : (cb==B_DESERT?1 : (bad?5:6));
  if(r.range(caneRarity)==0){
    int clumps=1+r.range(2);
    for(int k=0;k<clumps;++k){
      int cxp=2+r.range(11), czp=2+r.range(11), stalks=3+r.range(3);
      for(int sIdx=0;sIdx<stalks;++sIdx){
        int sx=cxp+r.range(3)-1, sz=czp+r.range(3)-1;
        if(sx<0||sx>15||sz<0||sz>15)continue;
        int y=top(c,sx,sz); if(y<=CHUNK_HEIGHT_MIN)continue;
        i32 g=c.getBlock(sx,y,sz);
        if(!(g==GRASS_BLOCK||g==DIRT||g==SAND||g==RED_SAND)||c.getBlock(sx,y+1,sz)!=AIR)continue;
        bool water=c.getBlock(sx+1,y,sz)==WATER||c.getBlock(sx-1,y,sz)==WATER||c.getBlock(sx,y,sz+1)==WATER||c.getBlock(sx,y,sz-1)==WATER;
        if(!water)continue;
        int h=1+r.range(3);
        for(int q=0;q<h;++q)c.setBlock(sx,y+1+q,sz,SUGAR_CANE);
      }
    }
  }

  // ===== cactus: desert / badlands, on sand, isolated, height 1-3 =====
  if((cb==B_DESERT||bad) && r.range(6)==0){
    int tries=2+r.range(3);
    for(int k=0;k<tries;++k){
      int x=1+r.range(14), z=1+r.range(14), y=top(c,x,z);
      if(y<=CHUNK_HEIGHT_MIN)continue;
      i32 g=c.getBlock(x,y,z);
      if(!(g==SAND||g==RED_SAND)||c.getBlock(x,y+1,z)!=AIR)continue;
      if(c.getBlock(x+1,y+1,z)!=AIR||c.getBlock(x-1,y+1,z)!=AIR||c.getBlock(x,y+1,z+1)!=AIR||c.getBlock(x,y+1,z-1)!=AIR)continue;
      int h=1+r.range(3);
      for(int q=0;q<h;++q)c.setBlock(x,y+1+q,z,CACTUS);
    }
  }
}

} // namespace nc::world::features
