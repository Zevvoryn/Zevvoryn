#pragma once
// TEMPLATES_V1 -- runtime for REAL vanilla structure templates (1.21.1).
//
// Templates come from client.jar (data/minecraft/structure/**.nbt) and are baked
// into structures.gen.hpp as a base64 blob. Here: decoder, name index and the
// geometric placement with rotation/mirror (StructureTemplate.placeInWorld).
//
// TEMPLATES_V2: block PROPERTIES are now exact. The blob stores real block state
// ids (from generated/reports/blocks.json), and statexform.gen.hpp holds the
// rot90 / mirrorZ state tables, so stairs, doors, logs, signs, rails, fences and
// waterlogging survive rotation just like StructureTemplate does in vanilla.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "../core/types.hpp"
#include "structures.gen.hpp"
#include "statexform.gen.hpp"

namespace nc::world::tpl {

struct Template {
    std::string name;
    int sx = 0, sy = 0, sz = 0;
    std::vector<i32> palette;
    std::vector<u32> pos; // x | y<<8 | z<<16
    std::vector<u16> idx;

    int count() const { return (int)pos.size(); }
};

struct Library {
    std::vector<Template> all;

    const Template* byName(const char* n) const {
        for (const auto& t : all) if (t.name == n) return &t;
        return nullptr;
    }
    // All templates of a set, e.g. "ruined_portal/" or "village/plains/houses/".
    std::vector<const Template*> group(const char* prefix) const {
        const size_t n = std::strlen(prefix);
        std::vector<const Template*> out;
        for (const auto& t : all)
            if (t.name.size() > n && std::memcmp(t.name.data(), prefix, n) == 0) out.push_back(&t);
        return out;
    }
};

// ---------- base64 ----------
inline int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

inline void decodeBlob(std::vector<unsigned char>& out) {
    int parts = 0;
    const char* const* p = gen::structureBlobParts(parts);
    size_t chars = 0;
    for (int i = 0; i < parts; ++i) chars += std::strlen(p[i]);
    out.reserve(chars / 4 * 3 + 8);
    int acc = 0, bits = 0;
    for (int i = 0; i < parts; ++i)
        for (const char* s = p[i]; *s; ++s) {
            const int v = b64val(*s);
            if (v < 0) continue; // '=' padding
            acc = (acc << 6) | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back((unsigned char)((acc >> bits) & 0xFF));
            }
        }
}

struct Reader {
    const unsigned char* b;
    size_t n, i = 0;
    unsigned char u8v() { return i < n ? b[i++] : 0; }
    u16 u16v() { u16 v = (u16)(b[i] | (b[i + 1] << 8)); i += 2; return v; }
    u32 u32v() {
        u32 v = (u32)b[i] | ((u32)b[i + 1] << 8) | ((u32)b[i + 2] << 16) | ((u32)b[i + 3] << 24);
        i += 4;
        return v;
    }
};

inline const Library& library() {
    static Library lib = [] {
        Library L;
        std::vector<unsigned char> blob;
        decodeBlob(blob);
        if (blob.size() < 8 || std::memcmp(blob.data(), "ZVT2", 4) != 0) return L;
        Reader r{blob.data(), blob.size(), 4};
        const u32 count = r.u32v();
        L.all.resize(count);
        for (u32 k = 0; k < count; ++k) {
            Template& t = L.all[k];
            const int nameLen = r.u8v();
            t.name.assign((const char*)blob.data() + r.i, (size_t)nameLen);
            r.i += (size_t)nameLen;
            t.sx = r.u8v();
            t.sy = r.u8v();
            t.sz = r.u8v();
            const int wide = r.u8v();
            const int pal = r.u16v();
            t.palette.resize(pal);
            for (int q = 0; q < pal; ++q) t.palette[q] = (i32)r.u16v();
            const u32 nb = r.u32v();
            t.pos.resize(nb);
            t.idx.resize(nb);
            for (u32 q = 0; q < nb; ++q) {
                const u32 x = r.u8v(), y = r.u8v(), z = r.u8v();
                t.pos[q] = x | (y << 8) | (z << 16);
                t.idx[q] = wide ? r.u16v() : (u16)r.u8v();
            }
        }
        return L;
    }();
    return lib;
}

// ---------- TEMPLATES_V2: state rotation / mirror tables ----------
struct XformTables {
    std::vector<u16> rot90, mirZ;
    bool ok = false;
};

inline const XformTables& xforms() {
    static XformTables X = [] {
        XformTables T;
        int parts = 0;
        const char* const* p = gen::stateXformParts(parts);
        std::vector<unsigned char> blob;
        {
            size_t chars = 0;
            for (int i = 0; i < parts; ++i) chars += std::strlen(p[i]);
            blob.reserve(chars / 4 * 3 + 8);
            int acc = 0, bits = 0;
            for (int i = 0; i < parts; ++i)
                for (const char* s = p[i]; *s; ++s) {
                    const int v = b64val(*s);
                    if (v < 0) continue;
                    acc = (acc << 6) | v;
                    bits += 6;
                    if (bits >= 8) { bits -= 8; blob.push_back((unsigned char)((acc >> bits) & 0xFF)); }
                }
        }
        if (blob.size() < 8 || std::memcmp(blob.data(), "ZVX1", 4) != 0) return T;
        Reader r{blob.data(), blob.size(), 4};
        const u32 n = r.u32v();
        if (blob.size() < 8 + (size_t)n * 4) return T;
        T.rot90.resize(n);
        T.mirZ.resize(n);
        for (u32 i = 0; i < n; ++i) T.rot90[i] = r.u16v();
        for (u32 i = 0; i < n; ++i) T.mirZ[i] = r.u16v();
        T.ok = true;
        return T;
    }();
    return X;
}

// Same order as the geometry below: mirror first, then rot90 applied `rot` times.
inline i32 xformState(i32 id, int rot, bool mirrorZ) {
    const XformTables& X = xforms();
    if (!X.ok || id <= 0 || (size_t)id >= X.rot90.size()) return id;
    u16 s = (u16)id;
    if (mirrorZ) s = X.mirZ[s];
    for (int k = 0; k < (rot & 3); ++k) s = X.rot90[s];
    return (i32)s;
}

// Footprint after rotation (rot: 0..3 = 0/90/180/270 degrees).
inline void rotatedSize(const Template& t, int rot, int& sx, int& sz) {
    if (rot & 1) { sx = t.sz; sz = t.sx; } else { sx = t.sx; sz = t.sz; }
}

// Places the template. W is any writer with put(wx, y, wz, id) -- the chunk
// writer clips everything outside the target chunk by itself.
// skipAir: keep the existing terrain instead of carving vanilla air blocks.
template <class W>
inline void place(W& w, const Template& t, int ox, int oy, int oz, int rot, bool mirrorZ,
                  bool skipAir = false) {
    rot &= 3;
    const int n = t.count();
    for (int k = 0; k < n; ++k) {
        const u32 p = t.pos[k];
        const i32 raw = t.palette[t.idx[k]];
        if (skipAir && raw == 0) continue;
        const i32 id = (rot || mirrorZ) ? xformState(raw, rot, mirrorZ) : raw;
        int x = (int)(p & 0xFF);
        const int y = (int)((p >> 8) & 0xFF);
        int z = (int)((p >> 16) & 0xFF);
        if (mirrorZ) z = t.sz - 1 - z;
        int rx = x, rz = z;
        switch (rot) {
            case 1: rx = t.sz - 1 - z; rz = x; break;
            case 2: rx = t.sx - 1 - x; rz = t.sz - 1 - z; break;
            case 3: rx = z;            rz = t.sx - 1 - x; break;
            default: break;
        }
        w.put(ox + rx, oy + y, oz + rz, id);
    }
}

} // namespace nc::world::tpl
