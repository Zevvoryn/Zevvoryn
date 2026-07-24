#pragma once
// TABSERVER_V1: настраиваемый header/footer таб-листа через папку tab_Server.
// Пользователь может задать своё имя сервера, отключить строку с онлайном, добавить свои
// строки в подвал таб-листа, либо полностью отключить кастомный таб-лист (disable=true) —
// тогда клиент рисует обычный ванильный список без шапки/подвала.
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <cctype>

namespace nc::tab {

struct TabConfig {
    bool disable = false;             // true = не слать кастомный tab_list вовсе (ванильный майнкрафт)
    std::string name = "Zevvoryn";    // имя сервера в шапке таб-листа
    bool showOnline = true;           // показывать строку "Игроков онлайн: N" / "Players online: N"
    std::vector<std::string> extraLines; // свои строки, добавляются в подвал по порядку
};

inline std::string tabTrim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

inline bool tabParseBool(const std::string& v, bool def) {
    std::string s = v;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (s == "true" || s == "1" || s == "yes") return true;
    if (s == "false" || s == "0" || s == "no") return false;
    return def;
}

// TABSERVER_V1: готовит папку tab_Server (инструкция + tab.properties) и возвращает распарсенный конфиг.
// Читается заново при каждом вызове (файл маленький, а broadcastTabListHeaderFooter шлётся не каждый тик) —
// это позволяет менять настройки без перезапуска сервера.
inline TabConfig loadTabConfig(bool ru) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories("tab_Server", ec);

    const char* cfgPath = "tab_Server/tab.properties";
    if (!fs::exists(cfgPath, ec)) {
        std::ofstream out(cfgPath, std::ios::binary);
        if (out.is_open()) {
            if (ru) {
                out <<
                    "# Настройка таб-листа (список игроков справа в игре)\n"
                    "# ВАЖНО: сохраняйте этот файл в кодировке UTF-8 (в Блокноте — 'Сохранить как' -> UTF-8),\n"
                    "# иначе кириллица в имени/строках может отобразиться крякозябрами.\n"
                    "# ==========================================\n"
                    "\n"
                    "# disable=true - полностью отключить кастомный таб-лист и вернуть обычный\n"
                    "# ванильный майнкрафтовский (без шапки и подвала со статистикой)\n"
                    "disable=false\n"
                    "\n"
                    "# Имя сервера в шапке таб-листа\n"
                    "name=Zevvoryn\n"
                    "\n"
                    "# Показывать строку \"Игроков онлайн: N\"\n"
                    "show-online=true\n"
                    "\n"
                    "# Свои строки в подвал таб-листа (добавляйте extra-line-N по порядку, N = 1, 2, 3...)\n"
                    "# Пустые строки игнорируются.\n"
                    "extra-line-1=\n"
                    "extra-line-2=\n"
                    "extra-line-3=\n";
            } else {
                out <<
                    "# Tab list (in-game player list) settings\n"
                    "# IMPORTANT: save this file as UTF-8 (in Notepad: 'Save As' -> UTF-8),\n"
                    "# otherwise non-ASCII characters in name/lines may show up garbled.\n"
                    "# ==========================================\n"
                    "\n"
                    "# disable=true - fully disable the custom tab list and use plain vanilla\n"
                    "# Minecraft behavior (no header/footer with server stats)\n"
                    "disable=false\n"
                    "\n"
                    "# Server name shown in the tab list header\n"
                    "name=Zevvoryn\n"
                    "\n"
                    "# Show the \"Players online: N\" line\n"
                    "show-online=true\n"
                    "\n"
                    "# Your own footer lines (add extra-line-N in order, N = 1, 2, 3...)\n"
                    "# Empty lines are ignored.\n"
                    "extra-line-1=\n"
                    "extra-line-2=\n"
                    "extra-line-3=\n";
            }
        }
    }

    TabConfig cfg;
    std::ifstream in(cfgPath, std::ios::binary);
    std::string line;
    std::vector<std::pair<int, std::string>> extras;
    if (in.is_open()) {
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string t = tabTrim(line);
            if (t.empty() || t[0] == '#') continue;
            auto eq = t.find('=');
            if (eq == std::string::npos) continue;
            std::string key = tabTrim(t.substr(0, eq));
            std::string val = tabTrim(t.substr(eq + 1));
            if (key == "disable") {
                cfg.disable = tabParseBool(val, false);
            } else if (key == "name") {
                if (!val.empty()) cfg.name = val;
            } else if (key == "show-online") {
                cfg.showOnline = tabParseBool(val, true);
            } else if (key.rfind("extra-line-", 0) == 0) {
                if (!val.empty()) {
                    int idx = 0;
                    try { idx = std::stoi(key.substr(11)); } catch (...) { idx = 0; }
                    extras.push_back({idx, val});
                }
            }
        }
    }
    std::sort(extras.begin(), extras.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& pr : extras) cfg.extraLines.push_back(pr.second);
    return cfg;
}

} // namespace nc::tab
