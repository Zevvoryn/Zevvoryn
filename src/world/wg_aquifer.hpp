// wg_aquifer.hpp — ванильный NoiseBasedAquifer 1.18+ (порт Aquifer.java 1.21.1)
// Зависит от wg_density.hpp (OverworldRouter + PositionalFactory + Mth).
#pragma once
#include "wg_density.hpp"
#include <vector>
#include <unordered_map>
#include <cmath>
#include <climits>
#include <algorithm>
#include <cfloat>

namespace wg {

// ---------- вспомогательные Mth (отсутствуют в wg_noise.hpp) ----------
namespace AqMth {
    // Mth.map: линейное отображение без зажима
    inline double map(double v, double fLo, double fHi, double tLo, double tHi) {
        return tLo + (tHi - tLo) * (v - fLo) / (fHi - fLo);
    }
    // Mth.quantize: floor(d/unit)*unit (unit=int)
    inline int quantize(double d, int unit) {
        return (int)std::floor(d / (double)unit) * unit;
    }
    // Math.floorDiv Java-semantics
    inline int floorDiv(int a, int b) {
        int q = a / b;
        if ((a ^ b) < 0 && q * b != a) --q;
        return q;
    }
}

// ---------- BlockPos long-packing (точно как в Java) ----------
// X: bits [38..63], Z: bits [12..37], Y: bits [0..11]
inline long long bpAsLong(int x, int y, int z) {
    return ((long long)(x & 0x3FFFFFF) << 38LL)
         | ((long long)(z & 0x3FFFFFF) << 12LL)
         | ((long long)(y & 0xFFF));
}
inline int bpGetX(long long l) { return (int)(l << 1LL  >> 39LL); }
inline int bpGetY(long long l) { return (int)(l << 52LL >> 52LL); }
inline int bpGetZ(long long l) { return (int)(l << 26LL >> 38LL); }

static constexpr int AQ_WAY_BELOW = -4096;  // DimensionType.WAY_BELOW_MIN_Y

// FluidStatus: y < fluidLevel → fluidType, иначе 0 (воздух)
struct AqFluid {
    int fluidLevel = AQ_WAY_BELOW;
    int fluidType  = 0;        // block-state id: waterBlock / lavaBlock / 0
    int at(int y) const { return (y < fluidLevel) ? fluidType : 0; }
};

// NoiseBasedAquifer — точный порт Aquifer.NoiseBasedAquifer
class NoiseBasedAquifer {
public:
    static constexpr int X_SP = 16, Y_SP = 12, Z_SP = 16;
    static constexpr int X_R  = 10, Y_R  =  9, Z_R  = 10;
    // similarity(sq(10), sq(12)) = 1 - |144-100|/25 = -0.76
    static constexpr double FLOWING_SIM = -0.76;
    // 13 chunk-offsets для сэмплинга поверхности
    static constexpr int SOFF[13][2] = {
        {0,0},{-2,-1},{-1,-1},{0,-1},{1,-1},{-3,0},{-2,0},{-1,0},{1,0},{-2,1},{-1,1},{0,1},{1,1}
    };

    const OverworldRouter& router;
    PositionalFactory      pf;
    int waterBlock, lavaBlock;
    int minGX, minGY, minGZ, szX, szY, szZ;
    std::vector<AqFluid*>  statusCache;
    std::vector<long long> locCache;
    std::vector<AqFluid>   pool;
    // Each fluid cell asks 13 surface samples; many overlap. Caching avoids
    // repeatedly evaluating the full terrain density column.
    std::unordered_map<unsigned long long, int> terrainSurfaceCache;

    NoiseBasedAquifer(const OverworldRouter& r, int cx, int cz,
                      int minY, int height,
                      int wb, int lb, PositionalFactory factory)
        : router(r), pf(factory), waterBlock(wb), lavaBlock(lb)
    {
        int mnBX = cx*16, mxBX = mnBX+15;
        int mnBZ = cz*16, mxBZ = mnBZ+15;
        minGX = AqMth::floorDiv(mnBX, X_SP) - 1;
        szX   = AqMth::floorDiv(mxBX, X_SP) + 1 - minGX + 1;
        minGY = AqMth::floorDiv(minY, Y_SP) - 1;
        szY   = AqMth::floorDiv(minY+height, Y_SP) + 1 - minGY + 1;
        minGZ = AqMth::floorDiv(mnBZ, Z_SP) - 1;
        szZ   = AqMth::floorDiv(mxBZ, Z_SP) + 1 - minGZ + 1;
        int total = szX * szY * szZ;
        statusCache.assign(total, nullptr);
        locCache.assign(total, LLONG_MAX);
        pool.reserve(total);
    }

    // Возвращает: -1 = твёрдый блок (камень/deepslate/surface),
    //              0 = воздух, waterBlock, lavaBlock
    int computeSubstance(int wx, int wy, int wz, double density) {
        if (density > 0.0) return -1;

        AqFluid global = globalFluid(wy);
        if (global.at(wy) == lavaBlock) return lavaBlock;

        int gx = AqMth::floorDiv(wx - 5, X_SP);
        int gy = AqMth::floorDiv(wy + 1, Y_SP);
        int gz = AqMth::floorDiv(wz - 5, Z_SP);

        int d1=INT_MAX, d2=INT_MAX, d3=INT_MAX;
        long long l1=0, l2=0, l3=0;

        for (int i = 0; i <= 1; ++i)
        for (int j = -1; j <= 1; ++j)
        for (int k = 0; k <= 1; ++k) {
            long long loc = getOrMakeLoc(gx+i, gy+j, gz+k);
            int dx = bpGetX(loc)-wx, dy = bpGetY(loc)-wy, dz = bpGetZ(loc)-wz;
            int dist = dx*dx + dy*dy + dz*dz;
            if      (d1 >= dist) { l3=l2; l2=l1; l1=loc; d3=d2; d2=d1; d1=dist; }
            else if (d2 >= dist) { l3=l2; l2=loc;         d3=d2; d2=dist;        }
            else if (d3 >  dist) {        l3=loc;                 d3=dist;        }
        }

        AqFluid* fs1 = getStatus(l1);
        double   sim12 = sim(d1, d2);
        int block1 = fs1->at(wy);

        if (sim12 <= 0.0) return block1;

        // water above lava → liquid boundary
        if (block1 == waterBlock) {
            if (globalFluid(wy-1).at(wy-1) == lavaBlock) return block1;
        }

        double barrier = DBL_MAX;  // NaN-sentinel: DBL_MAX = "not yet computed"
        AqFluid* fs2 = getStatus(l2);
        double p12 = sim12 * pressure(wx, wy, wz, barrier, *fs1, *fs2);
        if (density + p12 > 0.0) return -1;

        AqFluid* fs3 = getStatus(l3);
        double sim13 = sim(d1, d3);
        if (sim13 > 0.0) {
            double p13 = sim12 * sim13 * pressure(wx, wy, wz, barrier, *fs1, *fs3);
            if (density + p13 > 0.0) return -1;
        }
        double sim23 = sim(d2, d3);
        if (sim23 > 0.0) {
            double p23 = sim12 * sim23 * pressure(wx, wy, wz, barrier, *fs2, *fs3);
            if (density + p23 > 0.0) return -1;
        }
        return block1;
    }

private:
    AqFluid globalFluid(int wy) const {
        if (wy < -60) return { -60, lavaBlock }; // tuned: keep global lava deeper
        return { 63, waterBlock };
    }
    int gIdx(int gx, int gy, int gz) const {
        int ix=gx-minGX, iy=gy-minGY, iz=gz-minGZ;
        if (ix<0||ix>=szX||iy<0||iy>=szY||iz<0||iz>=szZ) return -1;
        return (iy*szZ+iz)*szX+ix;
    }
    long long getOrMakeLoc(int gx, int gy, int gz) {
        int i = gIdx(gx, gy, gz);
        if (i >= 0 && locCache[i] != LLONG_MAX) return locCache[i];
        RandomSource rng = pf.at(gx, gy, gz);
        long long loc = bpAsLong(gx*X_SP + rng.nextInt(X_R),
                                  gy*Y_SP + rng.nextInt(Y_R),
                                  gz*Z_SP + rng.nextInt(Z_R));
        if (i >= 0) locCache[i] = loc;
        return loc;
    }
    AqFluid* getStatus(long long l) {
        int x=bpGetX(l), y=bpGetY(l), z=bpGetZ(l);
        int gx=AqMth::floorDiv(x,X_SP), gy=AqMth::floorDiv(y,Y_SP), gz=AqMth::floorDiv(z,Z_SP);
        int i = gIdx(gx, gy, gz);
        if (i >= 0 && statusCache[i]) return statusCache[i];
        pool.push_back(computeFluid(x, y, z));
        AqFluid* ptr = &pool.back();
        if (i >= 0) statusCache[i] = ptr;
        return ptr;
    }
    static double sim(int a, int b) { return 1.0 - (double)std::abs(b-a) / 25.0; }

    // Предварительная поверхность: только slopedCheese (без пещерных шумов),
    // как ванильный NoiseChunk::preliminarySurfaceLevel
    int terrainSurfY(double wx, double wz) {
        int ix=(int)wx, iz=(int)wz;
        unsigned long long key=((unsigned long long)(unsigned int)ix<<32) | (unsigned int)iz;
        auto hit=terrainSurfaceCache.find(key);
        if(hit!=terrainSurfaceCache.end()) return hit->second;
        double c,e,rd;
        SplineContext sc = router.ctxAt(wx, wz, c, e, rd);
        double off=router.offsetAt(sc), fac=router.factorAt(sc), jag=router.jaggednessAt(sc);
        int result=MIN_Y-1;
        for (int cy = CELLS_Y-1; cy >= 0 && result==MIN_Y-1; --cy) {
            int y = MIN_Y + cy*CELL_HEIGHT;
            if (router.slopedCheese(wx,(double)y,wz,off,fac,jag) > 0.0) {
                for (int dy=CELL_HEIGHT-1; dy>=0; --dy)
                    if (router.slopedCheese(wx,(double)(y+dy),wz,off,fac,jag) > 0.0) { result=y+dy; break; }
            }
        }
        terrainSurfaceCache.emplace(key,result);
        return result;
    }

    // OverworldBiomeBuilder.isDeepDarkRegion: erosion < -0.78 && depth > 0.2
    bool isDeepDark(double wx, double wy, double wz) const {
        double px=wx*0.25, pz=wz*0.25;
        double ero = router.erosionN->getValue(px, 0.0, pz);
        if (ero >= -0.78) return false;
        double c,e,rd;
        SplineContext sc = router.ctxAt(wx, wz, c, e, rd);
        double dep = router.depthAt(wy, router.offsetAt(sc));
        return dep > 0.2;
    }

    AqFluid computeFluid(int x, int y, int z) {
        AqFluid global = globalFluid(y);
        int maxY = y + Y_SP;   // y + 12
        int minY2= y - Y_SP;   // y - 12
        bool aboveSurf = false;
        int minSurf = INT_MAX;

        for (auto& off : SOFF) {
            int sx = x + (off[0] << 4);   // SectionPos.sectionToBlockCoord = <<4
            int sz = z + (off[1] << 4);
            int surf  = terrainSurfY((double)sx, (double)sz);
            int surf8 = surf + 8;
            bool isCenter = (off[0]==0 && off[1]==0);

            // aquifer grid cell ganz über der Oberfläche → zurück global (Luft)
            if (isCenter && (minY2 > surf8)) return global;

            bool nearTop = (maxY > surf8);
            if (nearTop || isCenter) {
                AqFluid candidate = globalFluid(surf8);
                bool hasFluid = (candidate.at(surf8) != 0);
                if (hasFluid) {
                    if (isCenter) aboveSurf = true;
                    if (nearTop)  return candidate;
                }
            }
            if (surf < minSurf) minSurf = surf;
        }

        int lvl  = surfaceLevel(x, y, z, global, minSurf, aboveSurf);
        int type = fluidType(x, y, z, global, lvl);
        return { lvl, type };
    }

    int surfaceLevel(int x, int y, int z, const AqFluid& global, int minSurf, bool isAbove) const {
        if (isDeepDark((double)x,(double)y,(double)z)) return AQ_WAY_BELOW;

        int dist = minSurf + 8 - y;                          // n4+8-n2
        // d3 = clampedMap(dist, 0, 64, 1.0, 0.0)
        double d3 = isAbove ? Mth::clampedLerp(1.0, 0.0, (double)dist/64.0) : 0.0;

        // floodednessNoise: noise(AQUIFER_FLUID_LEVEL_FLOODEDNESS, 0.67) → xz=y=0.67
        double flood = Mth::clamp(router.floodednessN->getValue(x*0.67, y*0.67, z*0.67) - 0.25, -1.0, 1.0); // tuned: fewer flooded caves

        // Mth.map(d3, 1.0, 0.0, -0.3, 0.8): d3∈[0,1] mapped from range[1→0] to [-0.3→0.8]
        double wetT = AqMth::map(d3, 1.0, 0.0, -0.3,  0.8);  // threshold for global flood
        double dryT = AqMth::map(d3, 1.0, 0.0, -0.8,  0.4);  // threshold for randomized

        double d_val  = flood - wetT;
        double d2_val = flood - dryT;

        if (d_val  > 0.0) return global.fluidLevel;
        if (d2_val > 0.0) return randomizedLevel(x, y, z, minSurf);
        return AQ_WAY_BELOW;
    }

    int randomizedLevel(int x, int y, int z, int surfY) const {
        int gx=AqMth::floorDiv(x,16), gy=AqMth::floorDiv(y,40), gz=AqMth::floorDiv(z,16);
        int base = gy*40 + 20;
        // spreadNoise: noise(AQUIFER_FLUID_LEVEL_SPREAD, 0.7142857...) → xz=y=0.71428
        double sp = router.spreadN->getValue(gx*0.7142857142857143,
                                              gy*0.7142857142857143,
                                              gz*0.7142857142857143) * 10.0;
        int q = AqMth::quantize(sp, 3);
        return std::min(surfY, base + q);
    }

    int fluidType(int x, int y, int z, const AqFluid& global, int lvl) const {
        if (lvl <= -10 && lvl != AQ_WAY_BELOW && global.fluidType != lavaBlock) {
            int gx=AqMth::floorDiv(x,64), gy=AqMth::floorDiv(y,40), gz=AqMth::floorDiv(z,64);
            // lavaNoise: noise(AQUIFER_LAVA) → xz=y=1.0
            if (std::fabs(router.lavaN->getValue((double)gx,(double)gy,(double)gz)) > 0.80)
                return lavaBlock; // tuned: rare local lava pockets
        }
        return waterBlock;
    }

    double pressure(int wx, int wy, int wz, double& barrierCache,
                    const AqFluid& f1, const AqFluid& f2) const {
        int b1=f1.at(wy), b2=f2.at(wy);
        if ((b1==lavaBlock&&b2==waterBlock)||(b1==waterBlock&&b2==lavaBlock)) return 2.0;

        int diff = std::abs(f1.fluidLevel - f2.fluidLevel);
        if (diff == 0) return 0.0;

        double mid  = 0.5*(f1.fluidLevel + f2.fluidLevel);
        double dy   = (double)wy + 0.5 - mid;
        double half = (double)diff / 2.0;
        double d12  = half - std::fabs(dy);
        double d13  = (dy > 0.0)
            ? ((d12>0.0) ? d12/1.5 : d12/2.5)
            : ((3.0+d12>0.0) ? (3.0+d12)/3.0 : (3.0+d12)/10.0);

        double bar = 0.0;
        if (d13 >= -2.0 && d13 <= 2.0) {
            if (barrierCache == DBL_MAX) {
                // barrierNoise: noise(AQUIFER_BARRIER, 0.5) → xz=y=0.5
                barrierCache = router.barrierN->getValue(wx*0.5, wy*0.5, wz*0.5);
            }
            bar = barrierCache;
        }
        return 2.0 * (bar + d13);
    }
};

} // namespace wg
