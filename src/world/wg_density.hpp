// wg_density.hpp — density-интерпретатор overworld (порт NoiseRouterData + BlendedNoise + NoiseChunk cell-interp)
// Стадия 2–3a worldgen. Зависит от wg_noise.hpp и wg_spline.hpp.
#pragma once
#include "wg_noise.hpp"
#include "wg_spline.hpp"
#include <cmath>
#include <vector>
#include <algorithm>

namespace wg {

// ---------- density helpers (DensityFunctions) ----------
inline double clampedMap(double d, double dmin, double dmax, double emin, double emax) {
    return Mth::clampedLerp(emin, emax, (d - dmin) / (dmax - dmin));
}
inline double halfNegative(double d)    { return d > 0.0 ? d : d * 0.5; }
inline double quarterNegative(double d) { return d > 0.0 ? d : d * 0.25; }
inline double squeeze(double d) { double c = Mth::clamp(d, -1.0, 1.0); return c / 2.0 - c * c * c / 24.0; }
// peaksAndValleys (double-версия, для ridges-folded)
inline double densPeaksAndValleys(double f) {
    return -(std::fabs(std::fabs(f) - 0.6666666666666666) - 0.3333333333333333) * 3.0;
}

// ---------- BlendedNoise (base_3d_noise) — НЕ зависит от сида мира (createUnseeded) ----------
struct BlendedNoise {
    PerlinNoise* minLimitNoise;
    PerlinNoise* maxLimitNoise;
    PerlinNoise* mainNoise;
    double xzMultiplier, yMultiplier, xzFactor, yFactor, smearScaleMultiplier;
    double maxValueV;
    BlendedNoise(double xzScale, double yScale, double xzFac, double yFac, double smear) {
        RandomSource r(0ULL); // XoroshiroRandomSource(0L)
        minLimitNoise = new PerlinNoise(r, -15, std::vector<double>(16, 1.0), true);
        maxLimitNoise = new PerlinNoise(r, -15, std::vector<double>(16, 1.0), true);
        mainNoise     = new PerlinNoise(r, -7,  std::vector<double>(8, 1.0),  true);
        xzMultiplier = 684.412 * xzScale;
        yMultiplier  = 684.412 * yScale;
        xzFactor = xzFac; yFactor = yFac; smearScaleMultiplier = smear;
        maxValueV = minLimitNoise->maxBrokenValue(yMultiplier);
    }
    double compute(double x, double y, double z) const {
        double d  = x * xzMultiplier;
        double d2 = y * yMultiplier;
        double d3 = z * xzMultiplier;
        double d4 = d  / xzFactor;
        double d5 = d2 / yFactor;
        double d6 = d3 / xzFactor;
        double d7 = yMultiplier * smearScaleMultiplier;
        double d8 = d7 / yFactor;
        double d9 = 0.0, d10 = 0.0, d11 = 0.0, d12 = 1.0;
        for (int i = 0; i < 8; ++i) {
            ImprovedNoise* imp = mainNoise->getOctaveNoise(i);
            d11 += imp->noise5(wrap(d4 * d12), wrap(d5 * d12), wrap(d6 * d12), d8 * d12, d5 * d12) / d12;
            d12 /= 2.0;
        }
        double d13 = (d11 / 10.0 + 1.0) / 2.0;
        bool bl2 = d13 >= 1.0;
        bool bl3 = d13 <= 0.0;
        d12 = 1.0;
        for (int i = 0; i < 16; ++i) {
            double d14 = wrap(d  * d12);
            double d15 = wrap(d2 * d12);
            double d16 = wrap(d3 * d12);
            double d17 = d7 * d12;
            if (!bl2) { ImprovedNoise* imp = minLimitNoise->getOctaveNoise(i); d9  += imp->noise5(d14, d15, d16, d17, d2 * d12) / d12; }
            if (!bl3) { ImprovedNoise* imp = maxLimitNoise->getOctaveNoise(i); d10 += imp->noise5(d14, d15, d16, d17, d2 * d12) / d12; }
            d12 /= 2.0;
        }
        return Mth::clampedLerp(d9 / 512.0, d10 / 512.0, d13) / 128.0;
    }
    double maxValue() const { return maxValueV; }
};

// ---------- OverworldRouter ----------
struct OverworldRouter {
    NormalNoise* shiftNoise;      // minecraft:offset  -3 {1,1,1,0}
    NormalNoise* continentalness; // -9 {1,1,2,2,2,1,1,1,1}
    NormalNoise* erosionN;        // -9 {1,1,0,1,1}
    NormalNoise* ridgeN;          // -7 {1,2,1,0,0,0}
    NormalNoise* jaggedNoise;     // -16 {16x1.0}
    NormalNoise* temperatureN;    // -10 {1.5,0,1,0,0,0}
    NormalNoise* vegetationN;     // -8 {1,1,0,0,0,0}
    NormalNoise* surfaceN;        // minecraft:surface (-6, {1,1,1}) — SurfaceRules
    // NOISE_CAVES_V1: vanilla underground density noises (NoiseRouterData/NoiseData, exact params)
    NormalNoise* spag2dN;      NormalNoise* spag2dElev;   NormalNoise* spag2dMod;    NormalNoise* spag2dThick;
    NormalNoise* spag3d1;      NormalNoise* spag3d2;      NormalNoise* spag3dRarity; NormalNoise* spag3dThick;
    NormalNoise* spagRoughN;   NormalNoise* spagRoughMod;
    NormalNoise* caveEntranceN; NormalNoise* caveLayerN;  NormalNoise* caveCheeseN;
    NormalNoise* noodleNz;     NormalNoise* noodleThickN; NormalNoise* noodleRidgeAN; NormalNoise* noodleRidgeBN;
    NormalNoise* pillarN;      NormalNoise* pillarRareN;  NormalNoise* pillarThickN;
    // AQUIFER_V1: шумы аквифера (NoiseData.java + NoiseRouterData.overworld())
    NormalNoise* barrierN;       // minecraft:aquifer_barrier          -3 {1.0}  scale 0.5
    NormalNoise* floodednessN;   // minecraft:aquifer_fluid_level_floodedness -7 {1.0} scale 0.67
    NormalNoise* spreadN;        // minecraft:aquifer_fluid_level_spread -5 {1.0} scale 0.7142857...
    NormalNoise* lavaN;          // minecraft:aquifer_lava              -1 {1.0}  scale 1.0
    BlendedNoise* base3d;
    SplinePtr offsetSpline, factorSpline, jaggednessSpline;

    static NormalNoise* mkNoise(PositionalFactory& wf, const char* key, i32 fo, std::vector<double> amps) {
        RandomSource s = wf.fromHashOf(key);
        return new NormalNoise(s, fo, amps);
    }
    explicit OverworldRouter(u64 seed) {
        RandomSource base(seed);
        PositionalFactory wf = base.forkPositional();
        shiftNoise      = mkNoise(wf, "minecraft:offset",         -3,  {1,1,1,0});
        continentalness = mkNoise(wf, "minecraft:continentalness",-9,  {1,1,2,2,2,1,1,1,1});
        erosionN        = mkNoise(wf, "minecraft:erosion",        -9,  {1,1,0,1,1});
        ridgeN          = mkNoise(wf, "minecraft:ridge",          -7,  {1,2,1,0,0,0});
        jaggedNoise     = mkNoise(wf, "minecraft:jagged",         -16, std::vector<double>(16, 1.0));
        temperatureN    = mkNoise(wf, "minecraft:temperature",    -10, {1.5,0,1,0,0,0});
        vegetationN     = mkNoise(wf, "minecraft:vegetation",     -8,  {1,1,0,0,0,0});
        surfaceN        = mkNoise(wf, "minecraft:surface",         -6,  {1,1,1}); // SURFACE_RULES_V1
        // NOISE_CAVES_V1 (NoiseData.java firstOctave/amplitudes)
        spag2dN       = mkNoise(wf, "minecraft:spaghetti_2d",           -7,  {1});
        spag2dElev    = mkNoise(wf, "minecraft:spaghetti_2d_elevation", -8,  {1});
        spag2dMod     = mkNoise(wf, "minecraft:spaghetti_2d_modulator", -11, {1});
        spag2dThick   = mkNoise(wf, "minecraft:spaghetti_2d_thickness", -11, {1});
        spag3d1       = mkNoise(wf, "minecraft:spaghetti_3d_1",         -7,  {1});
        spag3d2       = mkNoise(wf, "minecraft:spaghetti_3d_2",         -7,  {1});
        spag3dRarity  = mkNoise(wf, "minecraft:spaghetti_3d_rarity",    -11, {1});
        spag3dThick   = mkNoise(wf, "minecraft:spaghetti_3d_thickness", -8,  {1});
        spagRoughN    = mkNoise(wf, "minecraft:spaghetti_roughness",    -5,  {1});
        spagRoughMod  = mkNoise(wf, "minecraft:spaghetti_roughness_modulator", -8, {1});
        caveEntranceN = mkNoise(wf, "minecraft:cave_entrance",          -7,  {0.4,0.5,1});
        caveLayerN    = mkNoise(wf, "minecraft:cave_layer",             -8,  {1});
        caveCheeseN   = mkNoise(wf, "minecraft:cave_cheese",            -8,  {0.5,1,2,1,2,1,0,2,0});
        noodleNz      = mkNoise(wf, "minecraft:noodle",                 -8,  {1});
        noodleThickN  = mkNoise(wf, "minecraft:noodle_thickness",       -8,  {1});
        noodleRidgeAN = mkNoise(wf, "minecraft:noodle_ridge_a",         -7,  {1});
        noodleRidgeBN = mkNoise(wf, "minecraft:noodle_ridge_b",         -7,  {1});
        pillarN       = mkNoise(wf, "minecraft:pillar",                 -7,  {1,1});
        pillarRareN   = mkNoise(wf, "minecraft:pillar_rareness",        -8,  {1});
        pillarThickN  = mkNoise(wf, "minecraft:pillar_thickness",       -8,  {1});
        // AQUIFER_V1
        barrierN      = mkNoise(wf, "minecraft:aquifer_barrier",                 -3, {1.0});
        floodednessN  = mkNoise(wf, "minecraft:aquifer_fluid_level_floodedness", -7, {1.0});
        spreadN       = mkNoise(wf, "minecraft:aquifer_fluid_level_spread",      -5, {1.0});
        lavaN         = mkNoise(wf, "minecraft:aquifer_lava",                    -1, {1.0});
        base3d = new BlendedNoise(0.25, 0.125, 80.0, 160.0, 8.0);
        offsetSpline     = TerrainProvider::overworldOffset(SC_CONTINENTS, SC_EROSION, SC_RIDGES_FOLDED, false);
        factorSpline     = TerrainProvider::overworldFactor(SC_CONTINENTS, SC_EROSION, SC_RIDGES, SC_RIDGES_FOLDED, false);
        jaggednessSpline = TerrainProvider::overworldJaggedness(SC_CONTINENTS, SC_EROSION, SC_RIDGES, SC_RIDGES_FOLDED, false);
    }

    // SurfaceRules.noiseCondition(Noises.SURFACE): vanilla samples in block coordinates.
    double surfaceNoise(double x, double z) const { return surfaceN->getValue(x, 0.0, z); } // SURFACE_RULES_V1

    // shiftA / shiftB от offset-шума
    double shiftX(double x, double z) const { return shiftNoise->getValue(x * 0.25, 0.0, z * 0.25) * 4.0; }
    double shiftZ(double x, double z) const { return shiftNoise->getValue(z * 0.25, x * 0.25, 0.0) * 4.0; }

    // сплайн-контекст (continents/erosion/ridges/ridges-folded) в точке (x,z)
    SplineContext ctxAt(double x, double z, double& cOut, double& eOut, double& rOut) const {
        double sx = shiftX(x, z), sz = shiftZ(x, z);
        double px = x * 0.25 + sx, pz = z * 0.25 + sz;
        double c = continentalness->getValue(px, 0.0, pz);
        double e = erosionN->getValue(px, 0.0, pz);
        double r = ridgeN->getValue(px, 0.0, pz); // BIOMESCALE_V1: ванильный raw ridge
        cOut = c; eOut = e; rOut = r;
        SplineContext s;
        s.coords[SC_CONTINENTS]    = (float)c;
        s.coords[SC_EROSION]       = (float)e;
        s.coords[SC_RIDGES]        = (float)r;
        s.coords[SC_RIDGES_FOLDED] = (float)densPeaksAndValleys(r);
        return s;
    }
    double offsetAt(const SplineContext& s) const     { return -0.50375 + (double)offsetSpline->apply(s); }
    double factorAt(const SplineContext& s) const      { return (double)factorSpline->apply(s); }
    double jaggednessAt(const SplineContext& s) const  { return (double)jaggednessSpline->apply(s); }

    double depthAt(double y, double offset) const { return clampedMap(y, -64.0, 320.0, 1.5, -1.5) + offset; }
    double jaggedTerm(double x, double z, double jaggedness) const {
        return jaggedness * halfNegative(jaggedNoise->getValue(x * 1500.0, 0.0, z * 1500.0));
    }
    double slopedCheese(double x, double y, double z, double offset, double factor, double jaggedness) const {
        double depth = depthAt(y, offset);
        double d = depth + jaggedTerm(x, z, jaggedness);
        double noiseGradientDensity = 4.0 * quarterNegative(d * factor);
        return noiseGradientDensity + base3d->compute(x, y, z);
    }
    double slideOverworld(double y, double df) const {
        double topGrad = clampedMap(y, 240.0, 256.0, 1.0, 0.0);
        df = topGrad * (df + 0.078125) - 0.078125;      // lerp(topGrad, -0.078125, df)
        double botGrad = clampedMap(y, -64.0, -40.0, 0.0, 1.0);
        df = botGrad * (df - 0.1171875) + 0.1171875;    // lerp(botGrad, 0.1171875, df)
        return df;
    }
    // ---------- NOISE_CAVES_V1: overworld/caves/* (NoiseRouterData, точный порт) ----------
    static double rarity2D(double d) { if (d < -0.75) return 0.5; if (d < -0.5) return 0.75; if (d < 0.5) return 1.0; if (d < 0.75) return 2.0; return 3.0; }
    static double rarity3D(double d) { if (d < -0.5) return 0.75; if (d < 0.0) return 1.0; if (d < 0.5) return 1.5; return 2.0; }
    double nzs(NormalNoise* n, double x, double y, double z, double xz, double ys) const { return n->getValue(x * xz, y * ys, z * xz); }
    double spaghettiRoughness(double x, double y, double z) const {
        double rough = nzs(spagRoughN, x, y, z, 1.0, 1.0);
        double mod   = clampedMap(nzs(spagRoughMod, x, y, z, 1.0, 1.0), -1.0, 1.0, 0.0, -0.1);
        return mod * (std::fabs(rough) - 0.4);
    }
    double spaghetti2dThicknessMod(double x, double y, double z) const {
        return clampedMap(nzs(spag2dThick, x, y, z, 2.0, 1.0), -1.0, 1.0, -0.6, -1.3);
    }
    double spaghetti2d(double x, double y, double z) const {
        double mod = nzs(spag2dMod, x, y, z, 2.0, 1.0);
        double r = rarity2D(mod);
        double w = r * std::fabs(spag2dN->getValue(x / r, y / r, z / r)); // WeirdScaledSampler TYPE2
        double elev = clampedMap(nzs(spag2dElev, x, 0.0, z, 1.0, 0.0), -1.0, 1.0, -8.0, 8.0);
        double thick = spaghetti2dThicknessMod(x, y, z);
        double d5 = std::fabs(elev + clampedMap(y, -64.0, 320.0, 8.0, -40.0));
        double d6 = d5 + thick; d6 = d6 * d6 * d6;
        double d7 = w + 0.083 * thick;
        return Mth::clamp(std::max(d7, d6), -1.0, 1.0);
    }
    double cavesEntrances(double x, double y, double z) const {
        double rar = nzs(spag3dRarity, x, y, z, 2.0, 1.0);
        double thick = clampedMap(nzs(spag3dThick, x, y, z, 1.0, 1.0), -1.0, 1.0, -0.065, -0.088);
        double r = rarity3D(rar);
        double w1 = r * std::fabs(spag3d1->getValue(x / r, y / r, z / r)); // WeirdScaledSampler TYPE1
        double w2 = r * std::fabs(spag3d2->getValue(x / r, y / r, z / r));
        double d5 = Mth::clamp(std::max(w1, w2) + thick, -1.0, 1.0);
        double d6 = spaghettiRoughness(x, y, z);
        double d7 = nzs(caveEntranceN, x, y, z, 0.75, 0.5);
        double d8 = d7 + 0.37 + clampedMap(y, -10.0, 30.0, 0.3, 0.0);
        return std::min(d8, d6 + d5);
    }
    double cavesPillars(double x, double y, double z) const {
        double p = nzs(pillarN, x, y, z, 25.0, 0.3);
        double rare  = clampedMap(nzs(pillarRareN,  x, y, z, 1.0, 1.0), -1.0, 1.0, 0.0, -2.0);
        double thick = clampedMap(nzs(pillarThickN, x, y, z, 1.0, 1.0), -1.0, 1.0, 0.0, 1.1);
        double d4 = p * 2.0 + rare;
        return d4 * thick * thick * thick;
    }
    double cavesUnderground(double x, double y, double z, double sc) const {
        double sp2 = spaghetti2d(x, y, z) + spaghettiRoughness(x, y, z);
        double layer = nzs(caveLayerN, x, y, z, 1.0, 8.0);
        double d5 = 4.0 * layer * layer;
        double cheese = nzs(caveCheeseN, x, y, z, 1.0, 0.6666666666666666);
        double d7 = Mth::clamp(0.27 + cheese, -1.0, 1.0) + Mth::clamp(1.5 - 0.64 * sc, 0.0, 0.5);
        double d8 = d5 + d7;
        double d9 = std::min(std::min(d8, cavesEntrances(x, y, z)), sp2);
        double pil = cavesPillars(x, y, z);
        double d11 = pil < 0.03 ? -1000000.0 : pil;
        return std::max(d9, d11);
    }
    // noodle-компоненты (yLimitedInterpolatable: интерполируются по ячейкам, комбинируются поблочно)
    double noodleSel(double x, double y, double z) const { return (y >= -60.0 && y < 321.0) ? nzs(noodleNz, x, y, z, 1.0, 1.0) : -1.0; }
    double noodleThk(double x, double y, double z) const { return (y >= -60.0 && y < 321.0) ? clampedMap(nzs(noodleThickN, x, y, z, 1.0, 1.0), -1.0, 1.0, -0.05, -0.1) : 0.0; }
    double noodleRA (double x, double y, double z) const { return (y >= -60.0 && y < 321.0) ? nzs(noodleRidgeAN, x, y, z, 2.6666666666666665, 2.6666666666666665) : 0.0; }
    double noodleRB (double x, double y, double z) const { return (y >= -60.0 && y < 321.0) ? nzs(noodleRidgeBN, x, y, z, 2.6666666666666665, 2.6666666666666665) : 0.0; }

    // Q = slideOverworld(d14), где d14 включает noise caves (NOISE_CAVES_V1) — именно Q ванилла интерполирует по ячейкам 4x8x4
    double preSqueezeDensity(double x, double y, double z, double off, double fac, double jag) const {
        double sc = slopedCheese(x, y, z, off, fac, jag);
        double d14;
        if (sc < 1.5625) d14 = std::min(sc, 5.0 * cavesEntrances(x, y, z)); // близко к поверхности: только входы пещер
        else             d14 = cavesUnderground(x, y, z, sc);              // глубоко: cheese+spaghetti+entrances+pillars
        return slideOverworld(y, d14);
    }
    double finalFromQ(double q) const { return squeeze(q * 0.64); }
    double finalDensity(double x, double y, double z, double off, double fac, double jag) const {
        return finalFromQ(preSqueezeDensity(x, y, z, off, fac, jag));
    }

    // ---------- климат-сэмпл (для биомов); weirdness = raw ridges ----------
    void climate(double x, double y, double z,
                 double& temp, double& hum, double& cont, double& ero, double& depth, double& weird) const {
        double sx = shiftX(x, z), sz = shiftZ(x, z);
        double px = x * 0.25 + sx, pz = z * 0.25 + sz;
        cont  = continentalness->getValue(px, 0.0, pz);
        ero   = erosionN->getValue(px, 0.0, pz);
        weird = ridgeN->getValue(px, 0.0, pz); // BIOMESCALE_V1: без сжатия — иначе полосы биомов тянутся
        temp  = temperatureN->getValue(px, 0.0, pz);
        hum   = vegetationN->getValue(px, 0.0, pz);
        SplineContext s;
        s.coords[SC_CONTINENTS]    = (float)cont;
        s.coords[SC_EROSION]       = (float)ero;
        s.coords[SC_RIDGES]        = (float)weird;
        s.coords[SC_RIDGES_FOLDED] = (float)densPeaksAndValleys(weird);
        depth = depthAt(y, offsetAt(s));
    }
};

// ---------- Константы ячеек overworld (noise_settings) ----------
static const int CELL_WIDTH   = 4;
static const int CELL_HEIGHT  = 8;
static const int MIN_Y        = -64;
static const int WORLD_HEIGHT = 384;
static const int SEA_LEVEL    = 63;
static const int CELLS_XZ     = 16 / CELL_WIDTH;            // 4
static const int CELLS_Y      = WORLD_HEIGHT / CELL_HEIGHT;  // 48

// Генератор террейна одного чанка: сэмпл углов Q + трилинейная интерполяция (NoiseChunk).
struct ChunkTerrain {
    const OverworldRouter& router;
    int chunkX, chunkZ, minX, minZ;
    std::vector<double> corners;
    int NX, NZ, NY;
    inline int cidx(int ix, int iz, int iy) const { return (ix * NZ + iz) * NY + iy; }

    std::vector<double> nSel, nThk, nRA, nRB; // NOISE_CAVES_V1: noodle component grids

    ChunkTerrain(const OverworldRouter& r, int cx, int cz)
        : router(r), chunkX(cx), chunkZ(cz), minX(cx * 16), minZ(cz * 16) {
        NX = CELLS_XZ + 1; NZ = CELLS_XZ + 1; NY = CELLS_Y + 1;
        corners.assign((size_t)NX * NZ * NY, 0.0);
        nSel.assign(corners.size(), 0.0); nThk.assign(corners.size(), 0.0);
        nRA.assign(corners.size(), 0.0);  nRB.assign(corners.size(), 0.0);
        for (int ix = 0; ix < NX; ++ix)
            for (int iz = 0; iz < NZ; ++iz) {
                double x = minX + ix * CELL_WIDTH;
                double z = minZ + iz * CELL_WIDTH;
                double c, e, rd; SplineContext s = router.ctxAt(x, z, c, e, rd);
                double off = router.offsetAt(s), fac = router.factorAt(s), jag = router.jaggednessAt(s);
                for (int iy = 0; iy < NY; ++iy) {
                    double y = MIN_Y + iy * CELL_HEIGHT;
                    size_t k = (size_t)cidx(ix, iz, iy);
                    corners[k] = router.preSqueezeDensity(x, y, z, off, fac, jag);
                    nSel[k] = router.noodleSel(x, y, z);
                    nThk[k] = router.noodleThk(x, y, z);
                    nRA[k]  = router.noodleRA(x, y, z);
                    nRB[k]  = router.noodleRB(x, y, z);
                }
            }
    }
    double lerpCell(const std::vector<double>& g, int cellX, int cellZ, int cellY, double dx, double dy, double dz) const {
        double c000 = g[cidx(cellX,     cellZ,     cellY)];
        double c010 = g[cidx(cellX,     cellZ,     cellY + 1)];
        double c001 = g[cidx(cellX,     cellZ + 1, cellY)];
        double c011 = g[cidx(cellX,     cellZ + 1, cellY + 1)];
        double c100 = g[cidx(cellX + 1, cellZ,     cellY)];
        double c110 = g[cidx(cellX + 1, cellZ,     cellY + 1)];
        double c101 = g[cidx(cellX + 1, cellZ + 1, cellY)];
        double c111 = g[cidx(cellX + 1, cellZ + 1, cellY + 1)];
        double vXZ00 = Mth::lerp(dy, c000, c010);
        double vXZ10 = Mth::lerp(dy, c100, c110);
        double vXZ01 = Mth::lerp(dy, c001, c011);
        double vXZ11 = Mth::lerp(dy, c101, c111);
        double vZ0 = Mth::lerp(dx, vXZ00, vXZ10);
        double vZ1 = Mth::lerp(dx, vXZ01, vXZ11);
        return Mth::lerp(dz, vZ0, vZ1);
    }
    double densityAt(int bx, int worldY, int bz) const {
        int cellX = bx / CELL_WIDTH, inCellX = bx % CELL_WIDTH;
        int cellZ = bz / CELL_WIDTH, inCellZ = bz % CELL_WIDTH;
        int yy = worldY - MIN_Y;
        int cellY = yy / CELL_HEIGHT, inCellY = yy % CELL_HEIGHT;
        double dx = (double)inCellX / CELL_WIDTH;
        double dy = (double)inCellY / CELL_HEIGHT;
        double dz = (double)inCellZ / CELL_WIDTH;
        double base = router.finalFromQ(lerpCell(corners, cellX, cellZ, cellY, dx, dy, dz));
        // NOISE_CAVES_V1: final_density = min(postProcess(...), noodle)
        double sel = lerpCell(nSel, cellX, cellZ, cellY, dx, dy, dz);
        double noodleVal;
        if (sel < 0.0) noodleVal = 64.0;
        else {
            double ra = lerpCell(nRA, cellX, cellZ, cellY, dx, dy, dz);
            double rb = lerpCell(nRB, cellX, cellZ, cellY, dx, dy, dz);
            noodleVal = lerpCell(nThk, cellX, cellZ, cellY, dx, dy, dz) + 1.5 * std::max(std::fabs(ra), std::fabs(rb));
        }
        return std::min(base, noodleVal);
    }
    int surfaceY(int bx, int bz) const {
        for (int y = MIN_Y + WORLD_HEIGHT - 1; y >= MIN_Y; --y)
            if (densityAt(bx, y, bz) > 0.0) return y;
        return MIN_Y - 1;
    }
};

} // namespace wg
