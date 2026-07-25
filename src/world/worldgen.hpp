// worldgen.hpp — единая точка включения ванильного ворлдгена Minecraft 1.21.1 (overworld)
// Подключай только этот файл.
//
//   #include "worldgen.hpp"
//   using namespace wg;
//   OverworldRouter router(seed);       // шумовой роутер (continentalness/erosion/ridges/...)
//   BiomeSource biomes(router);         // multi-noise биомы
//   for (чанк cx,cz) {
//       ChunkTerrain terrain(router, cx, cz);
//       for (lx,lz) {
//           int surfaceY = terrain.surfaceY(lx, lz);
//           for (worldY = MIN_Y .. surfaceY) {
//               double dens = terrain.densityAt(lx, worldY, lz);
//               if (dens > 0) -> твёрдый блок (камень/грунт по биому)
//           }
//           int biome = biomes.getBiomeAtBlock(bx, surfaceY, bz);
//           SurfaceBlock top = topBlock(biome), fill = fillerBlock(biome);
//       }
//   }
#pragma once
#include "wg_noise.hpp"    // Xoroshiro128++, RandomSource, Perlin/Normal noise
#include "wg_spline.hpp"   // TerrainProvider сплайны (offset/factor/jaggedness)
#include "wg_density.hpp"  // BlendedNoise, OverworldRouter, ChunkTerrain
#include "wg_biome.hpp"    // Climate params, BiomeSource
#include "wg_surface.hpp"  // SURFACE_RULES_V1: biome/height/noise surface rules
