#pragma once
// ============================================================
// ZLIB_V1: чистая C++ реализация zlib (RFC 1950/1951) без внешних библиотек.
// compress: LZ77 + fixed-Huffman deflate; decompress: stored/fixed/dynamic inflate.
// Используется для сетевого сжатия протокола Minecraft (Set Compression).
// ============================================================
#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>

namespace nc::net::zlibc {

inline std::uint32_t adler32(const std::uint8_t* data, std::size_t len) {
    std::uint32_t a = 1, b = 0;
    std::size_t i = 0;
    while (i < len) {
        std::size_t end = i + 5552;
        if (end > len) end = len;
        for (; i < end; ++i) { a += data[i]; b += a; }
        a %= 65521; b %= 65521;
    }
    return (b << 16) | a;
}

// ---------- таблицы длин/дистанций (RFC 1951) ----------
inline const std::uint16_t LEN_BASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
inline const std::uint8_t  LEN_EXTRA[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
inline const std::uint16_t DIST_BASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
inline const std::uint8_t  DIST_EXTRA[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

// ---------- запись битов (LSB-first) ----------
struct BitWriter {
    std::vector<std::uint8_t>& out;
    std::uint32_t bitBuf = 0;
    int bitCnt = 0;
    explicit BitWriter(std::vector<std::uint8_t>& o) : out(o) {}
    void putBits(std::uint32_t val, int n) {
        bitBuf |= (val << bitCnt);
        bitCnt += n;
        while (bitCnt >= 8) { out.push_back(std::uint8_t(bitBuf & 0xFF)); bitBuf >>= 8; bitCnt -= 8; }
    }
    void putHuff(std::uint32_t code, int n) { // код Хаффмана пишется старшим битом вперёд
        std::uint32_t rev = 0;
        for (int i = 0; i < n; ++i) rev = (rev << 1) | ((code >> i) & 1u);
        putBits(rev, n);
    }
    void flushByte() { if (bitCnt > 0) { out.push_back(std::uint8_t(bitBuf & 0xFF)); bitBuf = 0; bitCnt = 0; } }
};

// ---------- deflate: LZ77 + fixed Huffman ----------
inline void deflateFixed(const std::uint8_t* in, std::size_t n, std::vector<std::uint8_t>& out) {
    BitWriter bw(out);
    bw.putBits(1, 1); // BFINAL = 1 (один блок)
    bw.putBits(1, 2); // BTYPE = 01 (fixed Huffman)

    constexpr int HASH_BITS = 15;
    constexpr std::uint32_t HASH_SIZE = 1u << HASH_BITS;
    std::vector<std::int32_t> head(HASH_SIZE, -1);
    std::vector<std::int32_t> prev(n > 0 ? n : 1, -1);

    auto hash3 = [&](std::size_t i) -> std::uint32_t {
        return ((std::uint32_t(in[i]) << 10) ^ (std::uint32_t(in[i + 1]) << 5) ^ in[i + 2]) & (HASH_SIZE - 1);
    };
    auto emitLit = [&](std::uint8_t c) {
        if (c < 144) bw.putHuff(0x30u + c, 8);
        else bw.putHuff(0x190u + std::uint32_t(c - 144), 9);
    };
    auto emitLenDist = [&](int len, int dist) {
        int lc = 28;
        while (lc > 0 && LEN_BASE[lc] > len) --lc;
        int sym = 257 + lc;
        if (sym <= 279) bw.putHuff(std::uint32_t(sym - 256), 7);
        else bw.putHuff(0xC0u + std::uint32_t(sym - 280), 8);
        bw.putBits(std::uint32_t(len - LEN_BASE[lc]), LEN_EXTRA[lc]);
        int dc = 29;
        while (dc > 0 && DIST_BASE[dc] > dist) --dc;
        bw.putHuff(std::uint32_t(dc), 5);
        bw.putBits(std::uint32_t(dist - DIST_BASE[dc]), DIST_EXTRA[dc]);
    };

    std::size_t i = 0;
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        if (i + 3 <= n) {
            std::size_t maxLen = n - i;
            if (maxLen > 258) maxLen = 258;
            std::int32_t cand = head[hash3(i)];
            int chain = 128;
            while (cand >= 0 && chain-- > 0) {
                std::size_t dist = i - std::size_t(cand);
                if (dist > 32768) break;
                const std::uint8_t* p = in + std::size_t(cand);
                const std::uint8_t* q = in + i;
                std::size_t l = 0;
                while (l < maxLen && p[l] == q[l]) ++l;
                if (int(l) > bestLen) {
                    bestLen = int(l);
                    bestDist = int(dist);
                    if (l >= maxLen) break;
                }
                cand = prev[std::size_t(cand)];
            }
        }
        if (bestLen >= 4 || (bestLen == 3 && bestDist <= 4096)) {
            emitLenDist(bestLen, bestDist);
            std::size_t end = i + std::size_t(bestLen);
            std::size_t insEnd = (end + 2 <= n) ? end : (n >= 2 ? n - 2 : 0);
            for (; i < insEnd; ++i) { auto h = hash3(i); prev[i] = head[h]; head[h] = std::int32_t(i); }
            i = end;
        } else {
            emitLit(in[i]);
            if (i + 3 <= n) { auto h = hash3(i); prev[i] = head[h]; head[h] = std::int32_t(i); }
            ++i;
        }
    }
    bw.putHuff(0, 7); // символ 256 = конец блока
    bw.flushByte();
}

// zlib-контейнер: [0x78 0x9C][deflate][adler32]
inline std::vector<std::uint8_t> compress(std::span<const std::uint8_t> in) {
    std::vector<std::uint8_t> out;
    out.reserve(in.size() / 2 + 64);
    out.push_back(0x78);
    out.push_back(0x9C); // (0x78 * 256 + 0x9C) % 31 == 0
    deflateFixed(in.data(), in.size(), out);
    std::uint32_t ad = adler32(in.data(), in.size());
    out.push_back(std::uint8_t(ad >> 24));
    out.push_back(std::uint8_t(ad >> 16));
    out.push_back(std::uint8_t(ad >> 8));
    out.push_back(std::uint8_t(ad));
    return out;
}

// ---------- чтение битов (LSB-first) ----------
struct BitReader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t pos = 0;
    std::uint32_t bitBuf = 0;
    int bitCnt = 0;
    bool err = false;
    std::uint32_t getBits(int n) {
        while (bitCnt < n) {
            if (pos >= size) { err = true; return 0; }
            bitBuf |= std::uint32_t(data[pos++]) << bitCnt;
            bitCnt += 8;
        }
        std::uint32_t v = bitBuf & ((n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u));
        bitBuf >>= n;
        bitCnt -= n;
        return v;
    }
    int getBit() { return int(getBits(1)); }
    void alignByte() { int drop = bitCnt & 7; bitBuf >>= drop; bitCnt -= drop; }
};

// ---------- декодер Хаффмана (канонический, как в puff.c) ----------
struct HuffDec {
    std::uint16_t counts[16];
    std::uint16_t syms[318];
    void build(const std::uint8_t* lens, int n) {
        for (int i = 0; i < 16; ++i) counts[i] = 0;
        for (int i = 0; i < n; ++i) counts[lens[i]]++;
        counts[0] = 0;
        std::uint16_t offs[16];
        offs[0] = 0;
        offs[1] = 0;
        for (int l = 1; l < 15; ++l) offs[l + 1] = std::uint16_t(offs[l] + counts[l]);
        for (int i = 0; i < n; ++i) if (lens[i]) syms[offs[lens[i]]++] = std::uint16_t(i);
    }
    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= br.getBit();
            if (br.err) return -1;
            int count = counts[len];
            if (code - first < count) return syms[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    }
};

// ---------- inflate: stored / fixed / dynamic ----------
inline bool inflate(const std::uint8_t* in, std::size_t inLen, std::vector<std::uint8_t>& out, std::size_t maxOut) {
    BitReader br{in, inLen};

    static const HuffDec* fixedLit = [] {
        static HuffDec d;
        std::uint8_t lens[288];
        for (int i = 0; i < 144; ++i) lens[i] = 8;
        for (int i = 144; i < 256; ++i) lens[i] = 9;
        for (int i = 256; i < 280; ++i) lens[i] = 7;
        for (int i = 280; i < 288; ++i) lens[i] = 8;
        d.build(lens, 288);
        return &d;
    }();
    static const HuffDec* fixedDist = [] {
        static HuffDec d;
        std::uint8_t lens[30];
        for (int i = 0; i < 30; ++i) lens[i] = 5;
        d.build(lens, 30);
        return &d;
    }();

    bool finalBlock = false;
    while (!finalBlock) {
        finalBlock = br.getBits(1) != 0;
        std::uint32_t type = br.getBits(2);
        if (br.err) return false;

        if (type == 0) { // stored
            br.alignByte();
            std::uint32_t len = br.getBits(16);
            std::uint32_t nlen = br.getBits(16);
            if (br.err || ((len ^ 0xFFFFu) != (nlen & 0xFFFFu))) return false;
            for (std::uint32_t k = 0; k < len; ++k) {
                std::uint32_t b = br.getBits(8);
                if (br.err || out.size() >= maxOut + 1) return false;
                if (out.size() >= maxOut) return false;
                out.push_back(std::uint8_t(b));
            }
        } else if (type == 1 || type == 2) {
            HuffDec dynLit, dynDist;
            const HuffDec* lit = fixedLit;
            const HuffDec* dst = fixedDist;
            if (type == 2) { // dynamic
                int hlit = int(br.getBits(5)) + 257;
                int hdist = int(br.getBits(5)) + 1;
                int hclen = int(br.getBits(4)) + 4;
                if (br.err || hlit > 286 || hdist > 30) return false;
                static const int ORDER[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                std::uint8_t clens[19] = {0};
                for (int k = 0; k < hclen; ++k) clens[ORDER[k]] = std::uint8_t(br.getBits(3));
                if (br.err) return false;
                HuffDec cdec;
                cdec.build(clens, 19);
                std::uint8_t lens[318] = {0};
                int total = hlit + hdist;
                int idx = 0;
                while (idx < total) {
                    int sym = cdec.decode(br);
                    if (sym < 0) return false;
                    if (sym < 16) {
                        lens[idx++] = std::uint8_t(sym);
                    } else if (sym == 16) {
                        if (idx == 0) return false;
                        int rep = 3 + int(br.getBits(2));
                        std::uint8_t v = lens[idx - 1];
                        while (rep-- > 0 && idx < total) lens[idx++] = v;
                    } else if (sym == 17) {
                        int rep = 3 + int(br.getBits(3));
                        while (rep-- > 0 && idx < total) lens[idx++] = 0;
                    } else {
                        int rep = 11 + int(br.getBits(7));
                        while (rep-- > 0 && idx < total) lens[idx++] = 0;
                    }
                    if (br.err) return false;
                }
                dynLit.build(lens, hlit);
                dynDist.build(lens + hlit, hdist);
                lit = &dynLit;
                dst = &dynDist;
            }
            while (true) {
                int sym = lit->decode(br);
                if (sym < 0) return false;
                if (sym < 256) {
                    if (out.size() >= maxOut) return false;
                    out.push_back(std::uint8_t(sym));
                } else if (sym == 256) {
                    break;
                } else {
                    sym -= 257;
                    if (sym >= 29) return false;
                    int len = int(LEN_BASE[sym]) + int(br.getBits(LEN_EXTRA[sym]));
                    int dsym = dst->decode(br);
                    if (dsym < 0 || dsym >= 30) return false;
                    int dist = int(DIST_BASE[dsym]) + int(br.getBits(DIST_EXTRA[dsym]));
                    if (br.err || dist <= 0 || std::size_t(dist) > out.size()) return false;
                    if (out.size() + std::size_t(len) > maxOut) return false;
                    std::size_t from = out.size() - std::size_t(dist);
                    for (int k = 0; k < len; ++k) out.push_back(out[from + std::size_t(k)]);
                }
            }
        } else {
            return false; // BTYPE = 11 — ошибка
        }
    }
    return true;
}

// zlib-контейнер: проверка заголовка, inflate, сверка размера и adler32
inline bool decompress(std::span<const std::uint8_t> in, std::size_t expectedSize, std::vector<std::uint8_t>& out) {
    if (in.size() < 6) return false;
    if ((in[0] & 0x0F) != 8) return false;                                    // CM = deflate
    if (((std::uint32_t(in[0]) << 8) | in[1]) % 31 != 0) return false;        // FCHECK
    if (in[1] & 0x20) return false;                                           // FDICT не поддерживаем
    out.clear();
    out.reserve(expectedSize);
    if (!inflate(in.data() + 2, in.size() - 2, out, expectedSize)) return false;
    if (out.size() != expectedSize) return false;
    std::size_t t = in.size() - 4;
    std::uint32_t ad = (std::uint32_t(in[t]) << 24) | (std::uint32_t(in[t + 1]) << 16)
                     | (std::uint32_t(in[t + 2]) << 8) | std::uint32_t(in[t + 3]);
    return adler32(out.data(), out.size()) == ad;
}

// ============================================================
// ANVIL_CONVERT_V1: gzip (RFC 1952) wrap/unwrap around the same raw deflate
// primitives used above. Needed for level.dat, which vanilla always stores
// gzip-compressed (unlike region-file chunks, which use plain zlib).
// ============================================================

inline const std::uint32_t* crc32Table() {
    static std::uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[n] = c;
        }
        built = true;
    }
    return table;
}

inline std::uint32_t crc32(const std::uint8_t* data, std::size_t len) {
    const std::uint32_t* table = crc32Table();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// gzip container: [10-byte header][raw deflate][crc32 LE][isize LE]
inline std::vector<std::uint8_t> gzipCompress(std::span<const std::uint8_t> in) {
    std::vector<std::uint8_t> out;
    out.reserve(in.size() / 2 + 64);
    out.push_back(0x1F);
    out.push_back(0x8B);
    out.push_back(0x08); // CM = deflate
    out.push_back(0x00); // FLG
    out.push_back(0x00); out.push_back(0x00); out.push_back(0x00); out.push_back(0x00); // MTIME (unset)
    out.push_back(0x00); // XFL
    out.push_back(0xFF); // OS = unknown
    deflateFixed(in.data(), in.size(), out);
    std::uint32_t c = crc32(in.data(), in.size());
    std::uint32_t isize = static_cast<std::uint32_t>(in.size());
    out.push_back(std::uint8_t(c & 0xFF));
    out.push_back(std::uint8_t((c >> 8) & 0xFF));
    out.push_back(std::uint8_t((c >> 16) & 0xFF));
    out.push_back(std::uint8_t((c >> 24) & 0xFF));
    out.push_back(std::uint8_t(isize & 0xFF));
    out.push_back(std::uint8_t((isize >> 8) & 0xFF));
    out.push_back(std::uint8_t((isize >> 16) & 0xFF));
    out.push_back(std::uint8_t((isize >> 24) & 0xFF));
    return out;
}

// Decompresses a gzip stream. maxOut bounds the decompressed size (defends
// against corrupt/huge inputs); actual size is taken from the trailer once
// validated against what inflate() produced.
inline bool gzipDecompress(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t maxOut) {
    if (in.size() < 18) return false;
    if (in[0] != 0x1F || in[1] != 0x8B || in[2] != 0x08) return false;
    std::uint8_t flg = in[3];
    std::size_t pos = 10;
    if (flg & 0x04) { // FEXTRA
        if (pos + 2 > in.size()) return false;
        std::uint16_t xlen = std::uint16_t(in[pos]) | (std::uint16_t(in[pos + 1]) << 8);
        pos += 2 + xlen;
    }
    if (flg & 0x08) { while (pos < in.size() && in[pos] != 0) ++pos; ++pos; } // FNAME
    if (flg & 0x10) { while (pos < in.size() && in[pos] != 0) ++pos; ++pos; } // FCOMMENT
    if (flg & 0x02) pos += 2; // FHCRC
    if (pos >= in.size() || in.size() - pos < 8) return false;
    std::size_t deflateEnd = in.size() - 8;
    out.clear();
    out.reserve(maxOut > 0 ? std::min(maxOut, deflateEnd - pos) : (deflateEnd - pos));
    if (!inflate(in.data() + pos, deflateEnd - pos, out, maxOut)) return false;
    std::uint32_t expectedCrc = std::uint32_t(in[deflateEnd]) | (std::uint32_t(in[deflateEnd + 1]) << 8)
                              | (std::uint32_t(in[deflateEnd + 2]) << 16) | (std::uint32_t(in[deflateEnd + 3]) << 24);
    return crc32(out.data(), out.size()) == expectedCrc;
}

} // namespace nc::net::zlibc
