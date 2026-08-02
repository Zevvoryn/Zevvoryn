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
#include <cstddef>

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

// TABENC_V1: если tab.properties сохранён в CP1251 (Блокнот на русской Windows по умолчанию),
// кириллица улетала в клиент кракозябрами. Теперь проверяем строку на валидный
// UTF-8 и, если нет, конвертируем из CP1251.
inline bool tabIsValidUtf8(const std::string& s) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    std::size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = p[i];
        std::size_t need = 0;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return false;
        if (i + need >= n) return false;
        for (std::size_t k = 1; k <= need; ++k)
            if ((p[i + k] & 0xC0) != 0x80) return false;
        i += need + 1;
    }
    return true;
}

inline std::string tabCp1251ToUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        unsigned int cp;
        if (c < 0x80) { out.push_back(static_cast<char>(c)); continue; }
        else if (c == 0xA8) cp = 0x0401;
        else if (c == 0xB8) cp = 0x0451;
        else if (c >= 0xC0) cp = 0x0410u + (c - 0xC0u);
        else { out.push_back('?'); continue; }
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

inline std::string tabToUtf8(const std::string& s) {
    return tabIsValidUtf8(s) ? s : tabCp1251ToUtf8(s);
}

// TABSERVER_V1: готовит папку tab_Server (инструкция + tab.properties) и возвращает распарсенный конфиг.
// Читается заново при каждом вызове (файл маленький, а broadcastTabListHeaderFooter шлётся не каждый тик) —
// это позволяет менять настройки без перезапуска сервера.
// CFGDOC_V1: рядом с конфигом кладётся файл-инструкция. Имя и текст — на языке сервера
// (language= в settings.properties), том же, на котором говорит консоль.
// Имя файла собирается через char8_t: на Windows узкий путь трактуется как ANSI,
// и кириллица в имени превратилась бы в мусор. BOM в начале — чтобы старый
// Блокнот не показал текст крякозябрами.
inline void writeConfigDoc(const char* folder, bool ru, const std::string& text) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const char8_t* fileName = ru ? u8"ИНСТРУКЦИЯ.txt" : u8"HOW-TO.txt";
    const fs::path docPath = fs::path(folder) / fs::path(fileName);
    if (fs::exists(docPath, ec)) return;
    std::ofstream out(docPath, std::ios::binary);
    if (!out.is_open()) return;
    out << "\xEF\xBB\xBF" << text;
}

inline TabConfig loadTabConfig(bool ru) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories("tab_Server", ec);

    // CFGDOC_V1: инструкция рядом с конфигом — на языке сервера
    writeConfigDoc("tab_Server", ru, ru
        ? std::string(
            "ПАПКА tab_Server — настройка таб-листа\r\n"
            "====================================================\r\n"
            "\r\n"
            "Таб-лист — это список игроков, который открывается клавишей Tab.\r\n"
            "Всё настраивается в файле tab.properties рядом с этой инструкцией.\r\n"
            "Файл перечитывается на ходу — перезапуск сервера не нужен.\r\n"
            "Сохраняй его в кодировке UTF-8 (в Блокноте: Сохранить как -> UTF-8).\r\n"
            "Удалишь этот файл или tab.properties — сервер создаст их заново.\r\n"
            "\r\n"
            "НАСТРОЙКИ tab.properties\r\n"
            "----------------------------------------------------\r\n"
            "disable         true  — кастомный таб-лист выключен, клиент рисует обычный\r\n"
            "                        ванильный список без шапки и подвала.\r\n"
            "                false — кастомный таб-лист включён (по умолчанию).\r\n"
            "\r\n"
            "name            Имя сервера в шапке таб-листа. Можно с цветами: name=§bZevvoryn\r\n"
            "show-online     true — показывать строку \"Игроков онлайн: N\".\r\n"
            "extra-line-N    Свои строки в подвале, по порядку N = 1, 2, 3...\r\n"
            "                Пустые строки пропускаются, количество не ограничено.\r\n"
            "\r\n"
            "ЦВЕТА И СТИЛИ\r\n"
            "----------------------------------------------------\r\n"
            "§0 чёрный    §1 тёмно-синий  §2 тёмно-зелёный  §3 бирюзовый\r\n"
            "§4 тёмно-красный  §5 фиолетовый  §6 золотой  §7 серый\r\n"
            "§8 тёмно-серый  §9 синий  §a зелёный  §b голубой\r\n"
            "§c красный  §d розовый  §e жёлтый  §f белый\r\n"
            "§l жирный  §o курсив  §n подчёркнутый  §r сброс форматирования\r\n"
            "\r\n"
            "ПРИМЕР\r\n"
            "----------------------------------------------------\r\n"
            "disable=false\r\n"
            "name=§b§lZevvoryn\r\n"
            "show-online=true\r\n"
            "extra-line-1=§7Сайт: example.com\r\n"
            "extra-line-2=§eПриятной игры!\r\n")
        : std::string(
            "FOLDER tab_Server — tab list settings\r\n"
            "====================================================\r\n"
            "\r\n"
            "The tab list is the player list opened with the Tab key.\r\n"
            "Everything is configured in tab.properties next to this file.\r\n"
            "It is re-read while the server runs — no restart needed.\r\n"
            "Save it as UTF-8 (Notepad: Save As -> UTF-8).\r\n"
            "Delete this file or tab.properties and the server recreates them.\r\n"
            "\r\n"
            "tab.properties KEYS\r\n"
            "----------------------------------------------------\r\n"
            "disable         true  — custom tab list off, the client draws the plain\r\n"
            "                        vanilla list with no header and no footer.\r\n"
            "                false — custom tab list on (default).\r\n"
            "\r\n"
            "name            Server name in the tab list header. Colours allowed: name=§bZevvoryn\r\n"
            "show-online     true — show the \"Players online: N\" line.\r\n"
            "extra-line-N    Your own footer lines, in order N = 1, 2, 3...\r\n"
            "                Empty lines are skipped, the count is not limited.\r\n"
            "\r\n"
            "COLOURS AND STYLES\r\n"
            "----------------------------------------------------\r\n"
            "§0 black     §1 dark blue   §2 dark green   §3 dark aqua\r\n"
            "§4 dark red  §5 purple      §6 gold        §7 gray\r\n"
            "§8 dark gray §9 blue        §a green       §b aqua\r\n"
            "§c red       §d pink        §e yellow      §f white\r\n"
            "§l bold  §o italic  §n underline  §r reset formatting\r\n"
            "\r\n"
            "EXAMPLE\r\n"
            "----------------------------------------------------\r\n"
            "disable=false\r\n"
            "name=§b§lZevvoryn\r\n"
            "show-online=true\r\n"
            "extra-line-1=§7Website: example.com\r\n"
            "extra-line-2=§eHave fun!\r\n"));

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
                if (!val.empty()) cfg.name = tabToUtf8(val); // TABENC_V1
            } else if (key == "show-online") {
                cfg.showOnline = tabParseBool(val, true);
            } else if (key.rfind("extra-line-", 0) == 0) {
                if (!val.empty()) {
                    int idx = 0;
                    try { idx = std::stoi(key.substr(11)); } catch (...) { idx = 0; }
                    extras.push_back({idx, tabToUtf8(val)}); // TABENC_V1
                }
            }
        }
    }
    std::sort(extras.begin(), extras.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& pr : extras) cfg.extraLines.push_back(pr.second);
    return cfg;
}

} // namespace nc::tab
