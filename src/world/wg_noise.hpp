// Zevvoryn WORLDGEN_V1 — детерминированное ядро шума (порт vanilla 1.21.1)
// Xoroshiro128PlusPlus + RandomSupport + PositionalRandomFactory + Mth + ImprovedNoise/PerlinNoise/NormalNoise.
// Стандалон, без зависимостей проекта (чтобы можно было юнит-собрать).
#pragma once
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <array>
#include <stdexcept>

namespace wg {

using u64 = uint64_t;
using i64 = int64_t;
using u32 = uint32_t;
using i32 = int32_t;

// ---------- Mth (точные формулы) ----------
namespace Mth {
inline i32 floor(double d) { i32 n = (i32)d; return d < (double)n ? n - 1 : n; }
inline i64 lfloor(double d) { i64 l = (i64)d; return d < (double)l ? l - 1 : l; }
inline double lerp(double t, double a, double b) { return a + t * (b - a); }
inline double lerp2(double dx, double dy, double a, double b, double c, double d) {
    return lerp(dy, lerp(dx, a, b), lerp(dx, c, d));
}
inline double lerp3(double dx, double dy, double dz,
                    double a, double b, double c, double d,
                    double e, double f, double g, double h) {
    return lerp(dz, lerp2(dx, dy, a, b, c, d), lerp2(dx, dy, e, f, g, h));
}
inline double smoothstep(double d) { return d * d * d * (d * (d * 6.0 - 15.0) + 10.0); }
inline double clamp(double d, double lo, double hi) { if (d < lo) return lo; return d < hi ? d : hi; }
inline double clampedLerp(double a, double b, double t) {
    if (t < 0.0) return a; if (t > 1.0) return b; return lerp(t, a, b);
}
// getSeed(x,y,z) — внимание: x*3129871 считается в 32 битах, потом sign-extend до long
inline i64 getSeed(i32 x, i32 y, i32 z) {
    i32 xm = (i32)((u32)x * (u32)3129871u); // int-переполнение как в Java
    i64 l = ((i64)xm) ^ ((i64)z * 116129781LL) ^ ((i64)y);
    l = l * l * 42317861LL + l * 11LL;
    return l >> 16; // арифметический сдвиг
}
} // namespace Mth

inline u64 rotl(u64 x, int k) { return (x << k) | (x >> (64 - k)); }

// ---------- MD5 (для seedFromHashOf) ----------
struct MD5 {
    static std::array<uint8_t,16> hash(const std::string& msg) {
        auto LEFTROTATE = [](u32 x, u32 c){ return (x << c) | (x >> (32 - c)); };
        static const u32 s[64] = {
            7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
            5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
            4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
            6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21 };
        static const u32 K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
            0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
            0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
            0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
            0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
            0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
            0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
        std::vector<uint8_t> data(msg.begin(), msg.end());
        u64 origLenBits = (u64)data.size() * 8;
        data.push_back(0x80);
        while (data.size() % 64 != 56) data.push_back(0);
        for (int i = 0; i < 8; ++i) data.push_back((uint8_t)((origLenBits >> (8*i)) & 0xff));
        u32 a0=0x67452301, b0=0xefcdab89, c0=0x98badcfe, d0=0x10325476;
        for (size_t off = 0; off < data.size(); off += 64) {
            u32 M[16];
            for (int i = 0; i < 16; ++i)
                M[i] = data[off+i*4] | (data[off+i*4+1]<<8) | (data[off+i*4+2]<<16) | ((u32)data[off+i*4+3]<<24);
            u32 A=a0,B=b0,C=c0,D=d0;
            for (int i = 0; i < 64; ++i) {
                u32 F; int g;
                if (i < 16) { F=(B&C)|(~B&D); g=i; }
                else if (i < 32) { F=(D&B)|(~D&C); g=(5*i+1)%16; }
                else if (i < 48) { F=B^C^D; g=(3*i+5)%16; }
                else { F=C^(B|~D); g=(7*i)%16; }
                F = F + A + K[i] + M[g];
                A=D; D=C; C=B; B=B+LEFTROTATE(F, s[i]);
            }
            a0+=A; b0+=B; c0+=C; d0+=D;
        }
        std::array<uint8_t,16> out;
        u32 vs[4]={a0,b0,c0,d0};
        for (int i=0;i<4;++i) for(int j=0;j<4;++j) out[i*4+j]=(uint8_t)((vs[i]>>(8*j))&0xff);
        return out;
    }
};

// ---------- RandomSupport ----------
struct Seed128 { u64 lo, hi; };
inline u64 mixStafford13(u64 l) {
    l = (l ^ (l >> 30)) * 0xBF58476D1CE4E5B9ULL; // -4658895280553007687
    l = (l ^ (l >> 27)) * 0x94D049BB133111EBULL; // -7723592293110705685
    return l ^ (l >> 31);
}
inline Seed128 upgradeSeedTo128bitUnmixed(u64 l) {
    u64 lo = l ^ 0x6A09E667F3BCC909ULL;
    u64 hi = lo + 0x9E3779B97F4A7C15ULL; // + (-7046029254386353131) as u64
    return {lo, hi};
}
inline Seed128 seedMixed(Seed128 s) { return { mixStafford13(s.lo), mixStafford13(s.hi) }; }
inline Seed128 upgradeSeedTo128bit(u64 l) { return seedMixed(upgradeSeedTo128bitUnmixed(l)); }
inline Seed128 seedFromHashOf(const std::string& str) {
    auto b = MD5::hash(str);
    auto be = [&](int o){ u64 v=0; for(int i=0;i<8;++i) v=(v<<8)|b[o+i]; return v; };
    return { be(0), be(8) };
}

// ---------- Xoroshiro128PlusPlus ----------
struct Xoro {
    u64 lo, hi;
    Xoro(u64 l, u64 h) : lo(l), hi(h) { if ((lo|hi)==0){ lo=0x9E3779B97F4A7C15ULL; hi=0x6A09E667F3BCC909ULL; } }
    explicit Xoro(Seed128 s) : Xoro(s.lo, s.hi) {}
    u64 nextLong() {
        u64 l = lo, h = hi;
        u64 r = rotl(l + h, 17) + l;
        h ^= l;
        lo = rotl(l, 49) ^ h ^ (h << 21);
        hi = rotl(h, 28);
        return r;
    }
};

// ---------- RandomSource (Xoroshiro) ----------
struct PositionalFactory; // fwd
struct RandomSource {
    Xoro rng;
    explicit RandomSource(u64 seed) : rng(upgradeSeedTo128bit(seed)) {}
    RandomSource(u64 l, u64 h) : rng(l, h) {}
    explicit RandomSource(Seed128 s) : rng(s) {}
    static const constexpr double DOUBLE_UNIT = (double)(float)1.110223e-16f;
    static const constexpr double FLOAT_UNIT  = (double)(float)5.9604645e-8f;
    u64 nextBits(int n) { return rng.nextLong() >> (64 - n); }
    i32 nextInt() { return (i32)(u32)(rng.nextLong() & 0xFFFFFFFFULL); }
    i32 nextInt(i32 bound) {
        u64 l = (u64)(u32)nextInt();
        u64 m = l * (u64)(u32)bound;
        u64 low = m & 0xFFFFFFFFULL;
        if (low < (u64)(u32)bound) {
            u32 t = ((u32)(~bound + 1)) % (u32)bound; // remainderUnsigned(-bound, bound)
            while (low < (u64)t) {
                l = (u64)(u32)nextInt();
                m = l * (u64)(u32)bound;
                low = m & 0xFFFFFFFFULL;
            }
        }
        return (i32)(m >> 32);
    }
    u64 nextLong() { return rng.nextLong(); }
    double nextDouble() { return (double)nextBits(53) * DOUBLE_UNIT; }
    void consumeCount(int n) { for (int i=0;i<n;++i) rng.nextLong(); }
    PositionalFactory forkPositional();
};

struct PositionalFactory {
    u64 seedLo, seedHi;
    RandomSource fromHashOf(const std::string& s) const {
        Seed128 h = seedFromHashOf(s);
        return RandomSource(Seed128{ h.lo ^ seedLo, h.hi ^ seedHi });
    }
    RandomSource fromSeed(u64 l) const { return RandomSource(l ^ seedLo, l ^ seedHi); }
    RandomSource at(i32 x, i32 y, i32 z) const {
        u64 l = (u64)Mth::getSeed(x, y, z);
        return RandomSource(l ^ seedLo, seedHi);
    }
};
inline PositionalFactory RandomSource::forkPositional() {
    return PositionalFactory{ rng.nextLong(), rng.nextLong() };
}

// ---------- SimplexNoise gradient (для ImprovedNoise) ----------
static const int GRADIENT[16][3] = {
    {1,1,0},{-1,1,0},{1,-1,0},{-1,-1,0},{1,0,1},{-1,0,1},{1,0,-1},{-1,0,-1},
    {0,1,1},{0,-1,1},{0,1,-1},{0,-1,-1},{1,1,0},{0,-1,1},{-1,1,0},{0,-1,-1} };
inline double gdot(const int* g, double x, double y, double z) { return g[0]*x + g[1]*y + g[2]*z; }

// ---------- ImprovedNoise ----------
struct ImprovedNoise {
    double xo, yo, zo;
    uint8_t p[256];
    explicit ImprovedNoise(RandomSource& r) {
        xo = r.nextDouble() * 256.0;
        yo = r.nextDouble() * 256.0;
        zo = r.nextDouble() * 256.0;
        for (int i=0;i<256;++i) p[i]=(uint8_t)i;
        for (int i=0;i<256;++i) {
            int j = r.nextInt(256 - i);
            uint8_t t = p[i]; p[i]=p[i+j]; p[i+j]=t;
        }
    }
    int pi(int i) const { return p[i & 0xFF] & 0xFF; }
    double noise(double x, double y, double z) const {
        double dx = x + xo, dy = y + yo, dz = z + zo;
        int ix = Mth::floor(dx), iy = Mth::floor(dy), iz = Mth::floor(dz);
        double fx = dx - ix, fy = dy - iy, fz = dz - iz;
        int a = pi(ix), b = pi(ix+1);
        int aa = pi(a+iy), ab = pi(a+iy+1), ba = pi(b+iy), bb = pi(b+iy+1);
        double d5  = gdot(GRADIENT[pi(aa+iz)&0xF],   fx,     fy,     fz);
        double d6  = gdot(GRADIENT[pi(ba+iz)&0xF],   fx-1.0, fy,     fz);
        double d7  = gdot(GRADIENT[pi(ab+iz)&0xF],   fx,     fy-1.0, fz);
        double d8  = gdot(GRADIENT[pi(bb+iz)&0xF],   fx-1.0, fy-1.0, fz);
        double d9  = gdot(GRADIENT[pi(aa+iz+1)&0xF], fx,     fy,     fz-1.0);
        double d10 = gdot(GRADIENT[pi(ba+iz+1)&0xF], fx-1.0, fy,     fz-1.0);
        double d11 = gdot(GRADIENT[pi(ab+iz+1)&0xF], fx,     fy-1.0, fz-1.0);
        double d12 = gdot(GRADIENT[pi(bb+iz+1)&0xF], fx-1.0, fy-1.0, fz-1.0);
        double u = Mth::smoothstep(fx), v = Mth::smoothstep(fy), w = Mth::smoothstep(fz);
        return Mth::lerp3(u, v, w, d5, d6, d7, d8, d9, d10, d11, d12);
    }
    // обобщённый сэмпл: fyg — y для градиента, fys — y для smoothstep
    double sampleAndLerp(int ix, int iy, int iz, double fx, double fyg, double fz, double fys) const {
        int a = pi(ix), b = pi(ix+1);
        int aa = pi(a+iy), ab = pi(a+iy+1), ba = pi(b+iy), bb = pi(b+iy+1);
        double e5  = gdot(GRADIENT[pi(aa+iz)&0xF],   fx,     fyg,     fz);
        double e6  = gdot(GRADIENT[pi(ba+iz)&0xF],   fx-1.0, fyg,     fz);
        double e7  = gdot(GRADIENT[pi(ab+iz)&0xF],   fx,     fyg-1.0, fz);
        double e8  = gdot(GRADIENT[pi(bb+iz)&0xF],   fx-1.0, fyg-1.0, fz);
        double e9  = gdot(GRADIENT[pi(aa+iz+1)&0xF], fx,     fyg,     fz-1.0);
        double e10 = gdot(GRADIENT[pi(ba+iz+1)&0xF], fx-1.0, fyg,     fz-1.0);
        double e11 = gdot(GRADIENT[pi(ab+iz+1)&0xF], fx,     fyg-1.0, fz-1.0);
        double e12 = gdot(GRADIENT[pi(bb+iz+1)&0xF], fx-1.0, fyg-1.0, fz-1.0);
        double u = Mth::smoothstep(fx), v = Mth::smoothstep(fys), w = Mth::smoothstep(fz);
        return Mth::lerp3(u, v, w, e5, e6, e7, e8, e9, e10, e11, e12);
    }
    // 5-арг noise (с y-смазыванием) — для BlendedNoise
    double noise5(double x, double y, double z, double yScale, double yMax) const {
        double dx = x + xo, dy = y + yo, dz = z + zo;
        int ix = Mth::floor(dx), iy = Mth::floor(dy), iz = Mth::floor(dz);
        double fx = dx - ix, fy = dy - iy, fz = dz - iz;
        double d7;
        if (yScale != 0.0) {
            double d8 = (yMax >= 0.0 && yMax < fy) ? yMax : fy;
            d7 = std::floor(d8 / yScale + 1.0e-7) * yScale;
        } else d7 = 0.0;
        return sampleAndLerp(ix, iy, iz, fx, fy - d7, fz, fy);
    }
};

inline double wrap(double d) { return d - (double)Mth::lfloor(d / 3.3554432e7 + 0.5) * 3.3554432e7; }

// ---------- PerlinNoise (только "create" ветка, positional) ----------
struct PerlinNoise {
    std::vector<ImprovedNoise*> levels; // может быть nullptr при amplitude==0
    i32 firstOctave;
    std::vector<double> amps;
    double lowestFreqInputFactor, lowestFreqValueFactor, maxV;
    PerlinNoise(RandomSource& r, i32 firstOct, const std::vector<double>& amplitudes) {
        firstOctave = firstOct; amps = amplitudes;
        int n = (int)amps.size();
        int n2 = -firstOctave;
        levels.assign(n, nullptr);
        PositionalFactory pf = r.forkPositional();
        for (int i=0;i<n;++i) {
            if (amps[i]==0.0) continue;
            int oct = firstOctave + i;
            RandomSource sub = pf.fromHashOf("octave_" + std::to_string(oct));
            levels[i] = new ImprovedNoise(sub);
        }
        lowestFreqInputFactor = std::pow(2.0, -n2);
        lowestFreqValueFactor = std::pow(2.0, n-1) / (std::pow(2.0, n) - 1.0);
        maxV = edgeValue(2.0);
    }
    // legacy (createLegacyForBlendedNoise): октавы из ОДНОГО источника, без positional
    PerlinNoise(RandomSource& r, i32 firstOct, const std::vector<double>& amplitudes, bool /*legacy*/) {
        firstOctave = firstOct; amps = amplitudes;
        int n = (int)amps.size();
        int n2 = -firstOctave;
        levels.assign(n, nullptr);
        if (n2 >= 0 && n2 < n && amps[n2] != 0.0) levels[n2] = new ImprovedNoise(r);
        for (int i = n2 - 1; i >= 0; --i) {
            if (i < n && amps[i] != 0.0) levels[i] = new ImprovedNoise(r);
            else r.consumeCount(262);
        }
        lowestFreqInputFactor = std::pow(2.0, -n2);
        lowestFreqValueFactor = std::pow(2.0, n-1) / (std::pow(2.0, n) - 1.0);
        maxV = edgeValue(2.0);
    }
    // октава idx: 0 = самая низкая частота (как getOctaveNoise ваниллы)
    ImprovedNoise* getOctaveNoise(int idx) const { return levels[(int)levels.size() - 1 - idx]; }
    double maxBrokenValue(double d) const { return edgeValue(d + 2.0); }
    double edgeValue(double x) const {
        double v=0.0, f=lowestFreqValueFactor;
        for (int i=0;i<(int)levels.size();++i) { if (levels[i]) v += amps[i]*x*f; f/=2.0; }
        return v;
    }
    double getValue(double x, double y, double z) const {
        double v=0.0, inF=lowestFreqInputFactor, vaF=lowestFreqValueFactor;
        for (int i=0;i<(int)levels.size();++i) {
            if (levels[i]) {
                double nn = levels[i]->noise(wrap(x*inF), wrap(y*inF), wrap(z*inF));
                v += amps[i]*nn*vaF;
            }
            inF *= 2.0; vaF /= 2.0;
        }
        return v;
    }
    double maxValue() const { return maxV; }
};

// ---------- NormalNoise ----------
struct NormalNoise {
    double valueFactor, maxV;
    PerlinNoise* first; PerlinNoise* second;
    NormalNoise(RandomSource& r, i32 firstOct, const std::vector<double>& amplitudes) {
        first = new PerlinNoise(r, firstOct, amplitudes);
        second = new PerlinNoise(r, firstOct, amplitudes);
        int mn = INT32_MAX, mx = INT32_MIN;
        for (int i=0;i<(int)amplitudes.size();++i) { if (amplitudes[i]!=0.0){ if(i<mn)mn=i; if(i>mx)mx=i; } }
        valueFactor = 0.16666666666666666 / expectedDeviation(mx - mn);
        maxV = (first->maxValue() + second->maxValue()) * valueFactor;
    }
    static double expectedDeviation(int n) { return 0.1 * (1.0 + 1.0/(double)(n+1)); }
    double getValue(double x, double y, double z) const {
        const double IF = 1.0181268882175227;
        return (first->getValue(x,y,z) + second->getValue(x*IF, y*IF, z*IF)) * valueFactor;
    }
    double maxValue() const { return maxV; }
};

} // namespace wg
