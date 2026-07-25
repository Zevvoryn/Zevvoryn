// wg_spline.hpp - порт net.minecraft.util.CubicSpline + net.minecraft.data.worldgen.TerrainProvider (1.21.1)
// Стадия 2 worldgen. Чистая float-математика, без внешних зависимостей.
#pragma once
#include <vector>
#include <memory>
#include <cmath>
#include <functional>
#include <algorithm>

namespace wg {

// Координаты-входы для overworld terrain сплайнов.
// Соответствуют DensityFunctions.Spline.Coordinate (continents / erosion / ridges-folded).
// SC_RIDGES = raw ridges, SC_RIDGES_FOLDED = peaksAndValleys(ridges). offset использует folded;
// factor/jaggedness используют и raw, и folded (как в NoiseRouterData).
enum SplineCoord { SC_CONTINENTS = 0, SC_EROSION = 1, SC_RIDGES = 2, SC_RIDGES_FOLDED = 3, SC_COORD_COUNT = 4 };

struct SplineContext {
    float coords[SC_COORD_COUNT];
    float get(int c) const { return coords[c]; }
};

// ToFloatFunction трансформации значений (для amplified). Для обычного overworld = IDENTITY.
using FloatXform = std::function<float(float)>;
inline float xIdentityFn(float f){ return f; }

// ванильный Mth.lerp(float,float,float)
inline float mlerpf(float d, float a, float b){ return a + d * (b - a); }

// ванильный Mth.binarySearch
inline int mBinarySearch(int lo, int hi, const std::function<bool(int)>& pred){
    int n3 = hi - lo;
    int n = lo;
    while (n3 > 0){
        int n4 = n3 / 2;
        int n5 = n + n4;
        if (pred(n5)){ n3 = n4; continue; }
        n = n5 + 1; n3 -= n4 + 1;
    }
    return n;
}

// NoiseRouterData.peaksAndValleys
inline float peaksAndValleys(float f){
    return -(std::fabs(std::fabs(f) - 0.6666667f) - 0.33333334f) * 3.0f;
}

// CubicSpline: либо константа, либо multipoint по координате.
struct CubicSpline {
    bool constant = false;
    float value = 0.0f;                    // для constant
    int coordinate = 0;                    // для multipoint
    std::vector<float> locations;
    std::vector<std::shared_ptr<CubicSpline>> values;
    std::vector<float> derivatives;

    static std::shared_ptr<CubicSpline> makeConstant(float v){
        auto s = std::make_shared<CubicSpline>();
        s->constant = true; s->value = v; return s;
    }

    float apply(const SplineContext& ctx) const {
        if (constant) return value;
        float f = ctx.get(coordinate);
        int n = findIntervalStart(f);
        int n2 = (int)locations.size() - 1;
        if (n < 0) return linearExtend(f, values[0]->apply(ctx), 0);
        if (n == n2) return linearExtend(f, values[n2]->apply(ctx), n2);
        float f2 = locations[n];
        float f3 = locations[n + 1];
        float f4 = (f - f2) / (f3 - f2);
        float f7 = values[n]->apply(ctx);
        float f8 = values[n + 1]->apply(ctx);
        float f5 = derivatives[n];
        float f6 = derivatives[n + 1];
        float f9 = f5 * (f3 - f2) - (f8 - f7);
        float f10 = -f6 * (f3 - f2) + (f8 - f7);
        return mlerpf(f4, f7, f8) + f4 * (1.0f - f4) * mlerpf(f4, f9, f10);
    }

    float linearExtend(float f, float childVal, int n) const {
        float d = derivatives[n];
        if (d == 0.0f) return childVal;
        return childVal + d * (f - locations[n]);
    }

    int findIntervalStart(float f) const {
        auto& loc = locations;
        return mBinarySearch(0, (int)loc.size(), [&](int n){ return f < loc[n]; }) - 1;
    }
};
using SplinePtr = std::shared_ptr<CubicSpline>;

// Builder аналогичный CubicSpline.Builder
struct SplineBuilder {
    int coordinate;
    FloatXform xform;
    std::vector<float> locations;
    std::vector<SplinePtr> values;
    std::vector<float> derivatives;
    SplineBuilder(int c, FloatXform x = xIdentityFn) : coordinate(c), xform(std::move(x)) {}

    SplineBuilder& addPoint(float f, float v){ return addPoint(f, CubicSpline::makeConstant(xform(v)), 0.0f); }
    SplineBuilder& addPoint(float f, float v, float d){ return addPoint(f, CubicSpline::makeConstant(xform(v)), d); }
    SplineBuilder& addPoint(float f, SplinePtr s){ return addPoint(f, std::move(s), 0.0f); }
    SplineBuilder& addPoint(float f, SplinePtr s, float d){
        locations.push_back(f); values.push_back(std::move(s)); derivatives.push_back(d); return *this;
    }
    SplinePtr build(){
        auto s = std::make_shared<CubicSpline>();
        s->constant = false; s->coordinate = coordinate;
        s->locations = locations; s->values = values; s->derivatives = derivatives;
        return s;
    }
};

// ---------- TerrainProvider (net.minecraft.data.worldgen.TerrainProvider) ----------
// трансформации для amplified
inline float xAmplifiedOffset(float f){ return f < 0.0f ? f : f * 2.0f; }
inline float xAmplifiedFactor(float f){ return 1.25f - 6.25f / (f + 5.0f); }
inline float xAmplifiedJagged(float f){ return f * 2.0f; }

struct TerrainProvider {
    // Координаты: continents(i), erosion(i2), ridges-folded(i3)
    static float calculateSlope(float a, float b, float c, float d){ return (b - a) / (d - c); }

    static float mountainContinentalness(float f, float f2, float f3){
        float f6 = 1.0f - (1.0f - f2) * 0.5f;
        float f7 = 0.5f * (1.0f - f2);
        float f8 = (f + 1.17f) * 0.46082947f;
        float f9 = f8 * f6 - f7;
        if (f < f3) return std::max(f9, -0.2222f);
        return std::max(f9, 0.0f);
    }
    static float calcMountainRidgeZeroContPoint(float f){
        float f4 = 1.0f - (1.0f - f) * 0.5f;
        float f5 = 0.5f * (1.0f - f);
        return f5 / (0.46082947f * f4) - 1.17f;
    }

    static SplinePtr buildMountainRidgeSplineWithPoints(int i2, float f, bool bl, FloatXform xf){
        SplineBuilder b(i2, xf);
        float f4 = mountainContinentalness(-1.0f, f, -0.7f);
        float f6 = mountainContinentalness(1.0f, f, -0.7f);
        float f7 = calcMountainRidgeZeroContPoint(f);
        if (-0.65f < f7 && f7 < 1.0f){
            float f9 = mountainContinentalness(-0.65f, f, -0.7f);
            float f11 = mountainContinentalness(-0.75f, f, -0.7f);
            float f12 = calculateSlope(f4, f11, -1.0f, -0.75f);
            b.addPoint(-1.0f, f4, f12);
            b.addPoint(-0.75f, f11);
            b.addPoint(-0.65f, f9);
            float f13 = mountainContinentalness(f7, f, -0.7f);
            float f14 = calculateSlope(f13, f6, f7, 1.0f);
            b.addPoint(f7 - 0.01f, f13);
            b.addPoint(f7, f13, f14);
            b.addPoint(1.0f, f6, f14);
        } else {
            float f16 = calculateSlope(f4, f6, -1.0f, 1.0f);
            if (bl){
                b.addPoint(-1.0f, std::max(0.2f, f4));
                b.addPoint(0.0f, mlerpf(0.5f, f4, f6), f16);
            } else {
                b.addPoint(-1.0f, f4, f16);
            }
            b.addPoint(1.0f, f6, f16);
        }
        return b.build();
    }

    static SplinePtr ridgeSpline(int i, float f, float f2, float f3, float f4, float f5, float f6, FloatXform xf){
        float f7 = std::max(0.5f * (f2 - f), f6);
        float f8 = 5.0f * (f3 - f2);
        SplineBuilder b(i, xf);
        b.addPoint(-1.0f, f, f7);
        b.addPoint(-0.4f, f2, std::min(f7, f8));
        b.addPoint(0.0f, f3, f8);
        b.addPoint(0.4f, f4, 2.0f * (f4 - f3));
        b.addPoint(1.0f, f5, 0.7f * (f5 - f4));
        return b.build();
    }

    static SplinePtr buildErosionOffsetSpline(int i, int i2, float f, float f2, float f3, float f4,
                                              float f5, float f6, bool bl, bool bl2, FloatXform xf){
        SplinePtr cs  = buildMountainRidgeSplineWithPoints(i2, mlerpf(f4, 0.6f, 1.5f), bl2, xf);
        SplinePtr cs2 = buildMountainRidgeSplineWithPoints(i2, mlerpf(f4, 0.6f, 1.0f), bl2, xf);
        SplinePtr cs3 = buildMountainRidgeSplineWithPoints(i2, f4, bl2, xf);
        SplinePtr cs4 = ridgeSpline(i2, f - 0.15f, 0.5f * f4, mlerpf(0.5f, 0.5f, 0.5f) * f4, 0.5f * f4, 0.6f * f4, 0.5f, xf);
        SplinePtr cs5 = ridgeSpline(i2, f, f5 * f4, f2 * f4, 0.5f * f4, 0.6f * f4, 0.5f, xf);
        SplinePtr cs6 = ridgeSpline(i2, f, f5, f5, f2, f3, 0.5f, xf);
        SplinePtr cs7 = ridgeSpline(i2, f, f5, f5, f2, f3, 0.5f, xf);
        SplinePtr cs8 = SplineBuilder(i2, xf).addPoint(-1.0f, f).addPoint(-0.4f, cs6).addPoint(0.0f, f3 + 0.07f).build();
        SplinePtr cs9 = ridgeSpline(i2, -0.02f, f6, f6, f2, f3, 0.0f, xf);
        SplineBuilder b(i, xf);
        b.addPoint(-0.85f, cs).addPoint(-0.7f, cs2).addPoint(-0.4f, cs3).addPoint(-0.35f, cs4).addPoint(-0.1f, cs5).addPoint(0.2f, cs6);
        if (bl){ b.addPoint(0.4f, cs7).addPoint(0.45f, cs8).addPoint(0.55f, cs8).addPoint(0.58f, cs7); }
        b.addPoint(0.7f, cs9);
        return b.build();
    }

    static SplinePtr overworldOffset(int i, int i2, int i3, bool bl){
        FloatXform xf = bl ? (FloatXform)xAmplifiedOffset : (FloatXform)xIdentityFn;
        SplinePtr c1 = buildErosionOffsetSpline(i2, i3, -0.15f, 0.0f, 0.0f, 0.1f, 0.0f, -0.03f, false, false, xf);
        SplinePtr c2 = buildErosionOffsetSpline(i2, i3, -0.1f, 0.03f, 0.1f, 0.1f, 0.01f, -0.03f, false, false, xf);
        SplinePtr c3 = buildErosionOffsetSpline(i2, i3, -0.1f, 0.03f, 0.1f, 0.7f, 0.01f, -0.03f, true, true, xf);
        SplinePtr c4 = buildErosionOffsetSpline(i2, i3, -0.05f, 0.03f, 0.1f, 1.0f, 0.01f, 0.01f, true, true, xf);
        SplineBuilder b(i, xf);
        b.addPoint(-1.1f, 0.044f).addPoint(-1.02f, -0.2222f).addPoint(-0.51f, -0.2222f).addPoint(-0.44f, -0.12f)
         .addPoint(-0.18f, -0.12f).addPoint(-0.16f, c1).addPoint(-0.15f, c1).addPoint(-0.1f, c2)
         .addPoint(0.25f, c3).addPoint(1.0f, c4);
        return b.build();
    }

    static SplinePtr getErosionFactor(int i, int i2, int i3, float f, bool bl, FloatXform xf){
        SplinePtr cs = SplineBuilder(i2, xf).addPoint(-0.2f, 6.3f).addPoint(0.2f, f).build();
        SplineBuilder b(i, xf);
        b.addPoint(-0.6f, cs)
         .addPoint(-0.5f, SplineBuilder(i2, xf).addPoint(-0.05f, 6.3f).addPoint(0.05f, 2.67f).build())
         .addPoint(-0.35f, cs).addPoint(-0.25f, cs)
         .addPoint(-0.1f, SplineBuilder(i2, xf).addPoint(-0.05f, 2.67f).addPoint(0.05f, 6.3f).build())
         .addPoint(0.03f, cs);
        if (bl){
            SplinePtr cs2 = SplineBuilder(i2, xf).addPoint(0.0f, f).addPoint(0.1f, 0.625f).build();
            SplinePtr cs3 = SplineBuilder(i3, xf).addPoint(-0.9f, f).addPoint(-0.69f, cs2).build();
            b.addPoint(0.35f, f).addPoint(0.45f, cs3).addPoint(0.55f, cs3).addPoint(0.62f, f);
        } else {
            SplinePtr cs4 = SplineBuilder(i3, xf).addPoint(-0.7f, cs).addPoint(-0.15f, 1.37f).build();
            SplinePtr cs5 = SplineBuilder(i3, xf).addPoint(0.45f, cs).addPoint(0.7f, 1.56f).build();
            b.addPoint(0.05f, cs5).addPoint(0.4f, cs5).addPoint(0.45f, cs4).addPoint(0.55f, cs4).addPoint(0.58f, f);
        }
        return b.build();
    }

    static SplinePtr overworldFactor(int i, int i2, int i3, int i4, bool bl){
        FloatXform xf = bl ? (FloatXform)xAmplifiedFactor : (FloatXform)xIdentityFn;
        SplineBuilder b(i, xIdentityFn);
        b.addPoint(-0.19f, 3.95f)
         .addPoint(-0.15f, getErosionFactor(i2, i3, i4, 6.25f, true, xIdentityFn))
         .addPoint(-0.1f, getErosionFactor(i2, i3, i4, 5.47f, true, xf))
         .addPoint(0.03f, getErosionFactor(i2, i3, i4, 5.08f, true, xf))
         .addPoint(0.06f, getErosionFactor(i2, i3, i4, 4.69f, false, xf));
        return b.build();
    }

    static SplinePtr buildWeirdnessJaggednessSpline(int i, float f, FloatXform xf){
        float f2 = 0.63f * f, f3 = 0.3f * f;
        return SplineBuilder(i, xf).addPoint(-0.01f, f2).addPoint(0.01f, f3).build();
    }
    static SplinePtr buildRidgeJaggednessSpline(int i, int i2, float f, float f2, FloatXform xf){
        float f3 = peaksAndValleys(0.4f);
        float f4 = peaksAndValleys(0.56666666f);
        float f5 = (f3 + f4) / 2.0f;
        SplineBuilder b(i2, xf);
        b.addPoint(f3, 0.0f);
        if (f2 > 0.0f) b.addPoint(f5, buildWeirdnessJaggednessSpline(i, f2, xf)); else b.addPoint(f5, 0.0f);
        if (f > 0.0f)  b.addPoint(1.0f, buildWeirdnessJaggednessSpline(i, f, xf)); else b.addPoint(1.0f, 0.0f);
        return b.build();
    }
    static SplinePtr buildErosionJaggednessSpline(int i, int i2, int i3, float f, float f2, float f3, float f4, FloatXform xf){
        SplinePtr cs  = buildRidgeJaggednessSpline(i2, i3, f, f3, xf);
        SplinePtr cs2 = buildRidgeJaggednessSpline(i2, i3, f2, f4, xf);
        return SplineBuilder(i, xf).addPoint(-1.0f, cs).addPoint(-0.78f, cs2).addPoint(-0.5775f, cs2).addPoint(-0.375f, 0.0f).build();
    }
    static SplinePtr overworldJaggedness(int i, int i2, int i3, int i4, bool bl){
        FloatXform xf = bl ? (FloatXform)xAmplifiedJagged : (FloatXform)xIdentityFn;
        SplineBuilder b(i, xf);
        b.addPoint(-0.11f, 0.0f)
         .addPoint(0.03f, buildErosionJaggednessSpline(i2, i3, i4, 1.0f, 0.5f, 0.0f, 0.0f, xf))
         .addPoint(0.65f, buildErosionJaggednessSpline(i2, i3, i4, 1.0f, 1.0f, 1.0f, 0.0f, xf));
        return b.build();
    }
};

} // namespace wg
