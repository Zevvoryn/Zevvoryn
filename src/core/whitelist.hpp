#pragma once
// WHITELIST_V2 - vanilla-shaped whitelist.json (uuid + name), with a one-time
// automatic migration from the legacy WHITELIST_V1 whitelist.txt.
//
//   [ { "uuid": "...", "name": "Player" } ]
//
// Offline-mode UUIDs are derived exactly like vanilla does (UUID v3 of
// "OfflinePlayer:<name>", see OpManager::offlineUuid), so the file can be
// dropped straight into a vanilla / Spigot / Paper / Purpur server root and
// vice versa.
#include "types.hpp"
#include "log.hpp"
#include "op_manager.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace nc {

struct WhitelistEntry {
    std::string uuid;
    std::string name;
};

class Whitelist {
public:
    // Accepts either whitelist.txt (legacy) or whitelist.json. The JSON file is
    // the one we read and write; the .txt is only consulted for migration.
    void setPath(const std::string& p) {
        std::lock_guard<std::mutex> g(mx_);
        std::filesystem::path fp(p);
        std::filesystem::path js = fp; js.replace_extension(".json");
        std::filesystem::path tx = fp; tx.replace_extension(".txt");
        path_ = js.string();
        legacyPath_ = tx.string();
    }
    std::string path() const { std::lock_guard<std::mutex> g(mx_); return path_; }
    std::string legacyPath() const { std::lock_guard<std::mutex> g(mx_); return legacyPath_; }

    void load() {
        std::string file, legacy;
        { std::lock_guard<std::mutex> g(mx_); file = path_; legacy = legacyPath_; }
        std::error_code ec;
        std::vector<WhitelistEntry> parsed;
        if (std::filesystem::exists(file, ec)) {
            parseJson(readFile(file), parsed);
            std::lock_guard<std::mutex> g(mx_);
            entries_ = std::move(parsed);
            return;
        }
        if (!legacy.empty() && std::filesystem::exists(legacy, ec)) {
            std::ifstream in(legacy);
            std::string line;
            while (std::getline(in, line)) {
                trim(line);
                if (line.empty() || line[0] == '#') continue;
                if (contains(parsed, lower(line))) continue;
                parsed.push_back(WhitelistEntry{OpManager::offlineUuid(line), line});
            }
            in.close();
            size_t moved = parsed.size();
            { std::lock_guard<std::mutex> g(mx_); entries_ = std::move(parsed); }
            save();
            NC_INFO("Whitelist", "WHITELIST_V2: migrated {} name(s) from whitelist.txt to whitelist.json", moved);
            return;
        }
        { std::lock_guard<std::mutex> g(mx_); entries_.clear(); }
        save();
    }

    bool save() const {
        std::string file, json;
        {
            std::lock_guard<std::mutex> g(mx_);
            file = path_;
            json = serialize();
        }
        std::error_code ec;
        std::filesystem::path p(file);
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

    bool allowed(const std::string& name) const {
        std::lock_guard<std::mutex> g(mx_);
        return contains(entries_, lower(name));
    }

    bool add(const std::string& name, const std::string& uuid = std::string()) {
        std::string c = name;
        trim(c);
        if (c.empty()) return false;
        {
            std::lock_guard<std::mutex> g(mx_);
            if (contains(entries_, lower(c))) return false;
            entries_.push_back(WhitelistEntry{uuid.empty() ? OpManager::offlineUuid(c) : uuid, c});
        }
        save();
        return true;
    }

    bool remove(const std::string& name) {
        std::string c = name;
        trim(c);
        if (c.empty()) return false;
        const std::string k = lower(c);
        {
            std::lock_guard<std::mutex> g(mx_);
            const size_t before = entries_.size();
            entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                [&](const WhitelistEntry& e) { return lower(e.name) == k; }), entries_.end());
            if (entries_.size() == before) return false;
        }
        save();
        return true;
    }

    std::vector<std::string> names() const {
        std::lock_guard<std::mutex> g(mx_);
        std::vector<std::string> out;
        out.reserve(entries_.size());
        for (const auto& e : entries_) out.push_back(e.name);
        return out;
    }
    std::vector<WhitelistEntry> entries() const { std::lock_guard<std::mutex> g(mx_); return entries_; }
    size_t size() const { std::lock_guard<std::mutex> g(mx_); return entries_.size(); }

private:
    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)::tolower(c); });
        return s;
    }
    static void trim(std::string& s) {
        const char* ws = " \t\r\n";
        auto a = s.find_first_not_of(ws);
        if (a == std::string::npos) { s.clear(); return; }
        s = s.substr(a, s.find_last_not_of(ws) - a + 1);
    }
    static bool contains(const std::vector<WhitelistEntry>& list, const std::string& lk) {
        for (const auto& e : list) if (lower(e.name) == lk) return true;
        return false;
    }
    static std::string readFile(const std::string& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return std::string();
        std::stringstream ss; ss << in.rdbuf();
        std::string text = ss.str();
        if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
            (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.erase(0, 3);
        return text;
    }
    // Minimal reader: pulls every quoted token in order and pairs up the
    // "uuid"/"name" keys. Tolerates extra vanilla fields and hand editing.
    static void parseJson(const std::string& text, std::vector<WhitelistEntry>& out) {
        const char quote = char(34);
        const char esc = char(92);
        std::vector<std::string> toks;
        size_t i = 0;
        while (i < text.size()) {
            if (text[i] == quote) {
                std::string s;
                ++i;
                while (i < text.size() && text[i] != quote) {
                    if (text[i] == esc && i + 1 < text.size()) ++i;
                    s.push_back(text[i]);
                    ++i;
                }
                if (i < text.size()) ++i;
                toks.push_back(std::move(s));
            } else {
                ++i;
            }
        }
        WhitelistEntry cur;
        for (size_t k = 0; k + 1 < toks.size(); ++k) {
            if (toks[k] == "uuid") {
                cur.uuid = toks[k + 1];
                ++k;
            } else if (toks[k] == "name") {
                cur.name = toks[k + 1];
                ++k;
                if (!cur.name.empty()) {
                    if (cur.uuid.empty()) cur.uuid = OpManager::offlineUuid(cur.name);
                    if (!contains(out, lower(cur.name))) out.push_back(cur);
                }
                cur = WhitelistEntry{};
            }
        }
    }
    static std::string jsonEsc(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 4);
        for (char c : s) {
            if (c == char(34) || c == char(92)) out.push_back(char(92));
            out.push_back(c);
        }
        return out;
    }
    // caller holds mx_
    std::string serialize() const {
        const std::string q(1, char(34));
        const std::string nl(1, char(10));
        std::string out = "[" + nl;
        for (size_t i = 0; i < entries_.size(); ++i) {
            const auto& e = entries_[i];
            out += "  {" + nl;
            out += "    " + q + "uuid" + q + ": " + q + jsonEsc(e.uuid) + q + "," + nl;
            out += "    " + q + "name" + q + ": " + q + jsonEsc(e.name) + q + nl;
            out += (i + 1 < entries_.size()) ? ("  }," + nl) : ("  }" + nl);
        }
        out += "]" + nl;
        return out;
    }

    mutable std::mutex mx_;
    std::string path_ = "whitelist.json";
    std::string legacyPath_ = "whitelist.txt";
    std::vector<WhitelistEntry> entries_;
};

} // namespace nc
