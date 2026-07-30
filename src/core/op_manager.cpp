// OPMGR_V1 — implementation of the operator manager (see op_manager.hpp).
#include "op_manager.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace nc {
namespace {

// ---------------------------------------------------------------------------
// Tiny MD5 — only needed to build the vanilla offline-mode UUID
// (UUID v3 of the ASCII string "OfflinePlayer:<name>"). Self-contained so the
// module does not pull in a crypto dependency.
// ---------------------------------------------------------------------------
struct Md5 {
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    std::vector<uint8_t> buf;

    static uint32_t rotl(uint32_t x, uint32_t c) { return (x << c) | (x >> (32 - c)); }

    std::array<uint8_t, 16> digest(const std::string& msg) {
        static const uint32_t K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
            0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
            0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
            0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
            0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
            0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
            0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
        static const uint32_t R[64] = {
            7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
            5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
            4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
            6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};

        std::vector<uint8_t> m(msg.begin(), msg.end());
        const uint64_t bitLen = static_cast<uint64_t>(m.size()) * 8ull;
        m.push_back(0x80);
        while (m.size() % 64 != 56) m.push_back(0x00);
        for (int i = 0; i < 8; ++i) m.push_back(static_cast<uint8_t>((bitLen >> (8 * i)) & 0xff));

        for (size_t off = 0; off < m.size(); off += 64) {
            uint32_t M[16];
            for (int i = 0; i < 16; ++i)
                M[i] = static_cast<uint32_t>(m[off + i * 4]) |
                       (static_cast<uint32_t>(m[off + i * 4 + 1]) << 8) |
                       (static_cast<uint32_t>(m[off + i * 4 + 2]) << 16) |
                       (static_cast<uint32_t>(m[off + i * 4 + 3]) << 24);
            uint32_t A = a0, B = b0, C = c0, D = d0;
            for (uint32_t i = 0; i < 64; ++i) {
                uint32_t F, g;
                if (i < 16)      { F = (B & C) | (~B & D);            g = i; }
                else if (i < 32) { F = (D & B) | (~D & C);            g = (5 * i + 1) % 16; }
                else if (i < 48) { F = B ^ C ^ D;                     g = (3 * i + 5) % 16; }
                else             { F = C ^ (B | ~D);                  g = (7 * i) % 16; }
                F = F + A + K[i] + M[g];
                A = D; D = C; C = B;
                B = B + rotl(F, R[i]);
            }
            a0 += A; b0 += B; c0 += C; d0 += D;
        }

        std::array<uint8_t, 16> out{};
        const uint32_t words[4] = {a0, b0, c0, d0};
        for (int w = 0; w < 4; ++w)
            for (int i = 0; i < 4; ++i) out[static_cast<size_t>(w * 4 + i)] = static_cast<uint8_t>((words[w] >> (8 * i)) & 0xff);
        return out;
    }
};

std::string hex2(uint8_t b) {
    static const char* d = "0123456789abcdef";
    std::string s(2, '0');
    s[0] = d[(b >> 4) & 0xf];
    s[1] = d[b & 0xf];
    return s;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char tmp[8];
                    std::snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    out += tmp;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Minimal JSON reader for exactly the shape ops.json has: an array of flat
// objects with string/number/bool values. Unknown keys are ignored, so a file
// written by vanilla (or by a plugin) still loads.
// ---------------------------------------------------------------------------
struct MiniJson {
    const std::string& s;
    size_t i = 0;
    explicit MiniJson(const std::string& src) : s(src) {}

    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }

    bool parseString(std::string& out) {
        ws();
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < s.size()) {
                char e = s[i++];
                switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'u': i += 4; out.push_back('?'); break;
                    default: out.push_back(e);
                }
            } else {
                out.push_back(c);
            }
        }
        if (i < s.size()) ++i; // closing quote
        return true;
    }

    // Reads a raw scalar token (number / true / false / null).
    std::string parseScalar() {
        ws();
        const size_t start = i;
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
               s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') ++i;
        return s.substr(start, i - start);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Background writer
// ---------------------------------------------------------------------------
struct OpManager::Writer {
    std::thread thread;
    std::mutex m;
    std::condition_variable cv;
    bool dirty = false;
    bool stop = false;
};

OpManager& OpManager::instance() {
    static OpManager inst;
    return inst;
}

OpManager::~OpManager() {
    shutdown();
}

std::string OpManager::lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

std::string OpManager::offlineUuid(const std::string& name) {
    Md5 md5;
    auto d = md5.digest("OfflinePlayer:" + name);
    d[6] = static_cast<uint8_t>((d[6] & 0x0f) | 0x30); // version 3
    d[8] = static_cast<uint8_t>((d[8] & 0x3f) | 0x80); // IETF variant
    std::string out;
    for (size_t i = 0; i < 16; ++i) {
        out += hex2(d[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) out += '-';
    }
    return out;
}

void OpManager::init(const std::filesystem::path& serverRoot) {
    {
        std::unique_lock lk(mutex_);
        path_ = serverRoot.empty() ? std::filesystem::path("ops.json")
                                   : (serverRoot / "ops.json");
    }
    load();
    startWriter();
}

void OpManager::setRefreshHook(std::function<void(const std::string&, int)> hook) {
    std::unique_lock lk(mutex_);
    refreshHook_ = std::move(hook);
}

bool OpManager::isOp(const std::string& name) const {
    return level(name) > 0;
}

int OpManager::level(const std::string& name) const {
    const std::string key = lower(name);
    std::shared_lock lk(mutex_);
    for (const auto& e : entries_)
        if (lower(e.name) == key) return e.level;
    return 0;
}

bool OpManager::bypassesPlayerLimit(const std::string& name) const {
    const std::string key = lower(name);
    std::shared_lock lk(mutex_);
    for (const auto& e : entries_)
        if (lower(e.name) == key) return e.bypassesPlayerLimit;
    return false;
}

bool OpManager::empty() const {
    std::shared_lock lk(mutex_);
    return entries_.empty();
}

std::vector<OpEntry> OpManager::list() const {
    std::shared_lock lk(mutex_);
    return entries_;
}

std::optional<OpEntry> OpManager::find(const std::string& name) const {
    const std::string key = lower(name);
    std::shared_lock lk(mutex_);
    for (const auto& e : entries_)
        if (lower(e.name) == key) return e;
    return std::nullopt;
}

bool OpManager::addOp(const std::string& name, const std::string& uuid, int level,
                      bool bypassesPlayerLimit) {
    if (name.empty()) return false;
    const int lvl = std::clamp(level, 1, 4);
    std::function<void(const std::string&, int)> hook;
    {
        std::unique_lock lk(mutex_);
        const std::string key = lower(name);
        bool changed = true;
        bool found = false;
        for (auto& e : entries_) {
            if (lower(e.name) != key) continue;
            found = true;
            changed = (e.level != lvl) || (e.bypassesPlayerLimit != bypassesPlayerLimit) ||
                      (!uuid.empty() && e.uuid != uuid);
            e.level = lvl;
            e.bypassesPlayerLimit = bypassesPlayerLimit;
            e.name = name;
            if (!uuid.empty()) e.uuid = uuid;
            break;
        }
        if (!found) {
            OpEntry e;
            e.uuid = uuid.empty() ? offlineUuid(name) : uuid;
            e.name = name;
            e.level = lvl;
            e.bypassesPlayerLimit = bypassesPlayerLimit;
            entries_.push_back(std::move(e));
        }
        if (!changed && found) return false;
        hook = refreshHook_;
    }
    requestSave();
    if (hook) hook(name, lvl);
    return true;
}

bool OpManager::removeOp(const std::string& name) {
    std::function<void(const std::string&, int)> hook;
    {
        std::unique_lock lk(mutex_);
        const std::string key = lower(name);
        const size_t before = entries_.size();
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const OpEntry& e) { return lower(e.name) == key; }),
                       entries_.end());
        if (entries_.size() == before) return false;
        hook = refreshHook_;
    }
    requestSave();
    if (hook) hook(name, 0);
    return true;
}

bool OpManager::load() {
    std::filesystem::path p;
    {
        std::shared_lock lk(mutex_);
        p = path_;
    }
    std::vector<OpEntry> parsed;
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();
        // tolerate a UTF-8 BOM written by a text editor
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
            text.erase(0, 3);

        MiniJson j(text);
        if (j.eat('[')) {
            j.ws();
            if (!j.eat(']')) {
                do {
                    if (!j.eat('{')) break;
                    OpEntry e;
                    do {
                        std::string key;
                        if (!j.parseString(key)) break;
                        if (!j.eat(':')) break;
                        j.ws();
                        if (j.i < text.size() && text[j.i] == '"') {
                            std::string val;
                            j.parseString(val);
                            if (key == "uuid") e.uuid = val;
                            else if (key == "name") e.name = val;
                        } else {
                            const std::string val = j.parseScalar();
                            if (key == "level") { try { e.level = std::stoi(val); } catch (...) { e.level = 4; } }
                            else if (key == "bypassesPlayerLimit") e.bypassesPlayerLimit = (val == "true");
                        }
                    } while (j.eat(','));
                    j.eat('}');
                    if (!e.name.empty()) {
                        e.level = std::clamp(e.level, 1, 4);
                        if (e.uuid.empty()) e.uuid = offlineUuid(e.name);
                        parsed.push_back(std::move(e));
                    }
                } while (j.eat(','));
            }
        }
    }

    std::unique_lock lk(mutex_);
    entries_ = std::move(parsed);
    loaded_ = true;
    return true;
}

std::string OpManager::serialize() const {
    std::string out = "[\n";
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        out += "  {\n";
        out += "    \"uuid\": \"" + jsonEscape(e.uuid) + "\",\n";
        out += "    \"name\": \"" + jsonEscape(e.name) + "\",\n";
        out += "    \"level\": " + std::to_string(e.level) + ",\n";
        out += std::string("    \"bypassesPlayerLimit\": ") + (e.bypassesPlayerLimit ? "true" : "false") + "\n";
        out += (i + 1 < entries_.size()) ? "  },\n" : "  }\n";
    }
    out += "]\n";
    return out;
}

bool OpManager::writeFile(const std::string& json) const {
    std::filesystem::path p;
    {
        std::shared_lock lk(mutex_);
        p = path_;
    }
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);

    // Atomic-ish write: temp file first, then rename over the real one, so a
    // crash during the write cannot corrupt ops.json.
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << json;
        out.flush();
        if (!out) return false;
    }
    std::filesystem::remove(p, ec);
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        std::filesystem::copy_file(tmp, p, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp, ec);
    }
    return !ec;
}

bool OpManager::save() {
    std::string json;
    {
        std::shared_lock lk(mutex_);
        json = serialize();
    }
    return writeFile(json);
}

void OpManager::startWriter() {
    std::unique_lock lk(mutex_);
    if (writer_) return;
    writer_ = new Writer();
    Writer* w = writer_;
    w->thread = std::thread([this, w]() {
        for (;;) {
            std::unique_lock<std::mutex> wl(w->m);
            w->cv.wait(wl, [w] { return w->dirty || w->stop; });
            const bool stopping = w->stop;
            const bool dirty = w->dirty;
            w->dirty = false;
            wl.unlock();
            if (dirty) save();
            if (stopping) return;
        }
    });
}

void OpManager::requestSave() {
    Writer* w = nullptr;
    {
        std::shared_lock lk(mutex_);
        w = writer_;
    }
    if (!w) { save(); return; }
    {
        std::lock_guard<std::mutex> wl(w->m);
        w->dirty = true;
    }
    w->cv.notify_one();
}

void OpManager::shutdown() {
    Writer* w = nullptr;
    {
        std::unique_lock lk(mutex_);
        w = writer_;
        writer_ = nullptr;
    }
    if (!w) return;
    {
        std::lock_guard<std::mutex> wl(w->m);
        w->stop = true;
        w->dirty = true;
    }
    w->cv.notify_one();
    if (w->thread.joinable()) w->thread.join();
    delete w;
    save();
}

void OpManager::importLegacyCsv(const std::string& opsCsv) {
    if (opsCsv.empty()) return;
    {
        std::shared_lock lk(mutex_);
        if (!entries_.empty()) return; // ops.json already has data — it wins
    }
    size_t pos = 0;
    bool any = false;
    while (pos <= opsCsv.size()) {
        size_t comma = opsCsv.find(',', pos);
        if (comma == std::string::npos) comma = opsCsv.size();
        std::string tok = opsCsv.substr(pos, comma - pos);
        const size_t a = tok.find_first_not_of(" \t");
        const size_t b = tok.find_last_not_of(" \t");
        if (a != std::string::npos) {
            const std::string name = tok.substr(a, b - a + 1);
            std::unique_lock lk(mutex_);
            OpEntry e;
            e.uuid = offlineUuid(name);
            e.name = name;
            e.level = 4;
            entries_.push_back(std::move(e));
            any = true;
        }
        pos = comma + 1;
    }
    if (any) requestSave();
}

// ---------------------------------------------------------------------------
// Helpers used by core/server.cpp
// ---------------------------------------------------------------------------
namespace {
bool csvHas(const std::string& opsCsv, const std::string& name) {
    if (opsCsv.empty()) return false;
    std::string lower;
    lower.reserve(name.size());
    for (unsigned char c : name) lower.push_back(static_cast<char>(std::tolower(c)));
    size_t pos = 0;
    while (pos <= opsCsv.size()) {
        size_t comma = opsCsv.find(',', pos);
        if (comma == std::string::npos) comma = opsCsv.size();
        std::string tok = opsCsv.substr(pos, comma - pos);
        const size_t a = tok.find_first_not_of(" \t");
        const size_t b = tok.find_last_not_of(" \t");
        if (a != std::string::npos) {
            std::string t = tok.substr(a, b - a + 1);
            for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (t == lower) return true;
        }
        pos = comma + 1;
    }
    return false;
}
} // namespace

int opLevelOf(const std::string& opsCsv, const std::string& name) {
    const int lvl = OpManager::instance().level(name);
    if (lvl > 0) return lvl;
    if (csvHas(opsCsv, name)) return 4;
    // Bootstrap mode: brand new server with no operators configured anywhere.
    if (opsCsv.empty() && OpManager::instance().empty()) return 4;
    return 0;
}

bool opAllowed(const std::string& opsCsv, const std::string& name) {
    return opLevelOf(opsCsv, name) > 0;
}

} // namespace nc
