// BANMGR_V1 — implementation (see ban_manager.hpp).
#include "ban_manager.hpp"
#include "op_manager.hpp" // offlineUuid()

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

namespace nc {
namespace {

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
                if (c < 0x20) { char t[8]; std::snprintf(t, sizeof(t), "\\u%04x", c); out += t; }
                else out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

// Same minimal reader as OpManager: array of flat string objects.
struct MiniJson {
    const std::string& s;
    size_t i = 0;
    explicit MiniJson(const std::string& src) : s(src) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
    bool str(std::string& out) {
        ws();
        if (i >= s.size() || s[i] != '"') return false;
        ++i; out.clear();
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
            } else out.push_back(c);
        }
        if (i < s.size()) ++i;
        return true;
    }
    std::string scalar() {
        ws();
        const size_t a = i;
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
               s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') ++i;
        return s.substr(a, i - a);
    }
};

std::string nowStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

} // namespace

BanManager& BanManager::instance() {
    static BanManager inst;
    return inst;
}

std::string BanManager::lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

void BanManager::init(const std::filesystem::path& serverRoot) {
    {
        std::unique_lock lk(mutex_);
        path_ = serverRoot.empty() ? std::filesystem::path("banned-players.json")
                                   : (serverRoot / "banned-players.json");
    }
    load();
}

bool BanManager::isBanned(const std::string& name) const {
    const std::string key = lower(name);
    std::shared_lock lk(mutex_);
    for (const auto& e : entries_) if (lower(e.name) == key) return true;
    return false;
}

std::optional<BanEntry> BanManager::find(const std::string& name) const {
    const std::string key = lower(name);
    std::shared_lock lk(mutex_);
    for (const auto& e : entries_) if (lower(e.name) == key) return e;
    return std::nullopt;
}

std::vector<BanEntry> BanManager::list() const {
    std::shared_lock lk(mutex_);
    return entries_;
}

bool BanManager::ban(const std::string& name, const std::string& uuid, const std::string& source,
                     const std::string& reason) {
    if (name.empty()) return false;
    {
        std::unique_lock lk(mutex_);
        const std::string key = lower(name);
        for (const auto& e : entries_) if (lower(e.name) == key) return false;
        BanEntry e;
        e.uuid    = uuid.empty() ? OpManager::offlineUuid(name) : uuid;
        e.name    = name;
        e.created = nowStamp();
        e.source  = source.empty() ? std::string("Console") : source;
        e.expires = "forever";
        e.reason  = reason.empty() ? std::string("Banned by an operator.") : reason;
        entries_.push_back(std::move(e));
    }
    save();
    return true;
}

bool BanManager::pardon(const std::string& name) {
    {
        std::unique_lock lk(mutex_);
        const std::string key = lower(name);
        const size_t before = entries_.size();
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const BanEntry& e) { return lower(e.name) == key; }),
                       entries_.end());
        if (entries_.size() == before) return false;
    }
    save();
    return true;
}

bool BanManager::load() {
    std::filesystem::path p;
    { std::shared_lock lk(mutex_); p = path_; }
    std::vector<BanEntry> parsed;
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        std::stringstream ss; ss << in.rdbuf();
        std::string text = ss.str();
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
            text.erase(0, 3);
        MiniJson j(text);
        if (j.eat('[')) {
            j.ws();
            if (!j.eat(']')) {
                do {
                    if (!j.eat('{')) break;
                    BanEntry e;
                    do {
                        std::string key;
                        if (!j.str(key)) break;
                        if (!j.eat(':')) break;
                        j.ws();
                        if (j.i < text.size() && text[j.i] == '"') {
                            std::string val; j.str(val);
                            if (key == "uuid") e.uuid = val;
                            else if (key == "name") e.name = val;
                            else if (key == "created") e.created = val;
                            else if (key == "source") e.source = val;
                            else if (key == "expires") e.expires = val;
                            else if (key == "reason") e.reason = val;
                        } else {
                            (void)j.scalar();
                        }
                    } while (j.eat(','));
                    j.eat('}');
                    if (!e.name.empty()) {
                        if (e.uuid.empty()) e.uuid = OpManager::offlineUuid(e.name);
                        parsed.push_back(std::move(e));
                    }
                } while (j.eat(','));
            }
        }
    }
    std::unique_lock lk(mutex_);
    entries_ = std::move(parsed);
    return true;
}

std::string BanManager::serialize() const {
    std::string out = "[\n";
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        out += "  {\n";
        out += "    \"uuid\": \""    + jsonEscape(e.uuid)    + "\",\n";
        out += "    \"name\": \""    + jsonEscape(e.name)    + "\",\n";
        out += "    \"created\": \"" + jsonEscape(e.created) + "\",\n";
        out += "    \"source\": \""  + jsonEscape(e.source)  + "\",\n";
        out += "    \"expires\": \"" + jsonEscape(e.expires) + "\",\n";
        out += "    \"reason\": \""  + jsonEscape(e.reason)  + "\"\n";
        out += (i + 1 < entries_.size()) ? "  },\n" : "  }\n";
    }
    out += "]\n";
    return out;
}

bool BanManager::save() const {
    std::string json;
    std::filesystem::path p;
    {
        std::shared_lock lk(mutex_);
        json = serialize();
        p = path_;
    }
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::filesystem::path tmp = p; tmp += ".tmp";
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

} // namespace nc
