#pragma once
// WHITELIST_V1
#include "types.hpp"
#include "log.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace nc {

class Whitelist {
public:
    void setPath(const std::string& p) { std::lock_guard<std::mutex> g(mx_); path_ = p; }
    std::string path() const { std::lock_guard<std::mutex> g(mx_); return path_; }

    void load() {
        std::string file;
        { std::lock_guard<std::mutex> g(mx_); file = path_; }
        std::vector<std::string> v;
        std::ifstream in(file);
        if (!in.is_open()) {
            { std::lock_guard<std::mutex> g(mx_); names_.clear(); }
            save();
            return;
        }
        std::string line;
        while (std::getline(in, line)) {
            trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (!containsLower(v, lower(line))) v.push_back(line);
        }
        std::lock_guard<std::mutex> g(mx_);
        names_ = std::move(v);
    }

    bool save() const {
        std::string file;
        std::vector<std::string> cp;
        { std::lock_guard<std::mutex> g(mx_); file = path_; cp = names_; }
        std::ofstream out(file, std::ios::trunc);
        if (!out.is_open()) return false;
        out << "# Belyj spisok igrokov (WHITELIST_V1). Odin nik v stroke.\n";
        out << "# white-list=true v settings.properties.\n";
        for (auto& n : cp) out << n << "\n";
        return true;
    }

    bool allowed(const std::string& name) const {
        std::lock_guard<std::mutex> g(mx_);
        return containsLower(names_, lower(name));
    }

    bool add(const std::string& name) {
        std::string c = name;
        trim(c);
        if (c.empty()) return false;
        {
            std::lock_guard<std::mutex> g(mx_);
            if (containsLower(names_, lower(c))) return false;
            names_.push_back(c);
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
            auto before = names_.size();
            names_.erase(std::remove_if(names_.begin(), names_.end(),
                [&](const std::string& n) { return lower(n) == k; }), names_.end());
            if (names_.size() == before) return false;
        }
        save();
        return true;
    }

    std::vector<std::string> names() const { std::lock_guard<std::mutex> g(mx_); return names_; }
    size_t size() const { std::lock_guard<std::mutex> g(mx_); return names_.size(); }

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
    static bool containsLower(const std::vector<std::string>& list, const std::string& lk) {
        for (auto& n : list) if (lower(n) == lk) return true;
        return false;
    }

    mutable std::mutex mx_;
    std::string path_ = "whitelist.txt";
    std::vector<std::string> names_;
};

} // namespace nc
