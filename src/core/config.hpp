#pragma once

#include "types.hpp"
#include "log.hpp"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <streambuf> // WIZTEE_V1
#include <memory>    // WIZTEE_V1
#include <sstream>
#include <random>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <thread>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace nc {

// VERSION_V1: единая версия ядра. Менять ТОЛЬКО здесь.
inline constexpr const char* NC_VERSION = "0.2.0";
inline constexpr const char* NC_CODENAME = "Zevvoryn";

// ============================================================
// ServerConfig — читает/пишет settings.properties (key=value)
// Формат совместим с PMCpp / PocketMine style
// ============================================================
struct ServerConfig {
    // ── Сервер ──
    std::string language        = "rus";
    std::string motd            = "Zevvoryn Server";
    std::string brand           = "Zevvoryn"; // BRAND_V1: что видно в F3 как "<brand>" server
    std::string subMotd         = "";
    i32 port                    = 25565;
    i32 portV6                  = -1;       // -1 = выкл
    i32 maxPlayers              = 20;
    i32 viewDistance             = 10;
    i32 simulationDistance       = 8;

    // ── Ресурсы (RESOURCE_V1) ──
    i32 maxRamGb                 = 0;   // 0 = auto / no limit
    i32 maxCores                 = 0;   // 0 = auto

    // ── Мир ──
    std::string levelName       = "world";
    std::string generator       = "DEFAULT";   // DEFAULT, FLAT, VOID // WORLDTYPE_DEFAULT_V1: стандартный мир теперь дефолт
    i64 levelSeed               = 0;
    // DIMTOGGLE_V1: миры Ада и Энда можно вообще не создавать (спрашивает мастер установки).
    // Выключено = нет генерации, порталы не зажигаются, /nether и /end отвечают отказом.
    bool enableNether           = true;
    bool enableEnd              = true;

    // ── Геймплей ──
    std::string gamemode        = "creative";
    bool forceGamemode          = false;
    // DISCMSG_V1: true = пускаем только 1.21.1 (767). false = никого не вышвыриваем
    // насильно — на случай прокси/плагина вроде ViaVersion перед сервером.
    bool strictVersionCheck     = true;
    // GUICON_V1: classic = обычное консольное окно (как было), gui = собственный терминал.
    // Любое другое значение, не Windows или ошибка создания окна — тихий откат к classic.
    std::string consoleMode     = "classic";
    bool pvp                    = true;
    i32 difficulty              = 2;
    bool difficultyLocked       = false; // ALLPACKETS_V3: Lock Difficulty (0x19) is now real state
    bool showCoordinates        = true;
    i32 spawnProtection         = 0;   // SETTINGS_V10: по умолчанию защита спавна выключена
    std::string ops             = ""; // OPS_V1

    // ── Аутентификация ──
    bool xboxAuth               = false;
    bool whiteList              = false;

    // ── Сеть ──
    std::string serverIp        = "";
    bool enableIpv6             = false;
    i32 maxPacketSize           = 4194304;
    i32 compressionThreshold    = 256;

    // ── Сохранение ──
    bool autoSave               = true;
    i32 autoSaveInterval        = 300;
    // VANILLA_MIRROR_V1: рядом с world.dat пишется настоящая ванильная раскладка
    // (world/level.dat + region/ + entities/ + DIM-1 + DIM1 + playerdata/*.dat).
    // Источник истины пока всё равно world.dat — зеркало только для того,
    // чтобы папку можно было сразу открыть клиентом или ява-ядром, без export-vanilla.
    bool vanillaMirror          = true;
    i32 vanillaMirrorInterval   = 300; // секунды; 0 = писать каждое сохранение

    // ── Логирование ──
    std::string logLevel        = "INFO";

    // —— RCON (RCON_V1) ——
    bool enableRcon          = false;
    i32  rconPort            = 25575;
    std::string rconPassword;
    i32  rconMaxClients      = 4;
    bool rconLogCommands     = false; // RCONQUIET_V1: панель опрашивает сервер постоянно — по умолчанию молчим

    // AUTOSTARTPANEL_V1: автозапуск Discord-бота/веб-панели (DiscrordBotRcon) вместе с zevvoryn.exe
    bool autoStartPanel      = false;

    // ── Парсинг key=value ──
    void setProperty(const std::string& key, const std::string& rawValue) {
        std::string val = rawValue;
        // Убираем комментарии
        auto hashPos = val.find('#');
        if (hashPos != std::string::npos) val = val.substr(0, hashPos);
        // Трим пробелов
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
        };
        trim(val);
        if (val.empty()) return;

        auto toLower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); // WARNFIX_V1
            return s;
        };

        auto toBool = [&](const std::string& v) -> bool {
            std::string low = toLower(v);
            return low == "true" || low == "1" || low == "yes" || low == "on";
        };

        auto toInt = [](const std::string& v, int def) -> i32 {
            try { return static_cast<i32>(std::stol(v)); }
            catch (...) { return def; }
        };

        std::string lk = toLower(key);

        if      (lk == "language")              language = val;
        else if (lk == "motd")                  motd = val;
        else if (lk == "brand")                 brand = val; // BRAND_V1
        else if (lk == "sub-motd")              subMotd = val;
        else if (lk == "server-port")           port = toInt(val, port);
        else if (lk == "server-portv6")         portV6 = toInt(val, portV6);
        else if (lk == "max-players")           maxPlayers = toInt(val, maxPlayers);
        else if (lk == "view-distance")         viewDistance = toInt(val, viewDistance);
        else if (lk == "simulation-distance")   simulationDistance = toInt(val, simulationDistance);
        else if (lk == "max-ram-gb")            maxRamGb = toInt(val, maxRamGb);
        else if (lk == "max-cores")             maxCores = toInt(val, maxCores);
        else if (lk == "level-name")            levelName = val;
        else if (lk == "generator")            { generator = toLower(val); }
        else if (lk == "level-seed")            levelSeed = static_cast<i64>(std::stoll(val.empty() ? "0" : val));
        else if (lk == "enable-nether")         enableNether = toBool(val); // DIMTOGGLE_V1
        else if (lk == "enable-end")            enableEnd = toBool(val);    // DIMTOGGLE_V1
        else if (lk == "gamemode")              gamemode = toLower(val);
        else if (lk == "force-gamemode")        forceGamemode = toBool(val);
        else if (lk == "strict-version-check")  strictVersionCheck = toBool(val); // DISCMSG_V1
        else if (lk == "console-mode")          consoleMode = toLower(val);  // GUICON_V1
        else if (lk == "pvp")                   pvp = toBool(val);
        else if (lk == "difficulty")            { difficulty = toInt(val, difficulty); if (difficulty < 0) difficulty = 0; if (difficulty > 3) difficulty = 3; }
        else if (lk == "show-coordinates")      showCoordinates = toBool(val);
        else if (lk == "spawn-protection")      spawnProtection = toInt(val, spawnProtection);
        else if (lk == "ops")                   ops = toLower(val); // OPS_V1
        else if (lk == "xbox-auth")             xboxAuth = toBool(val);
        else if (lk == "white-list")            whiteList = toBool(val);
        else if (lk == "server-ip")             serverIp = val;
        else if (lk == "enable-ipv6")           enableIpv6 = toBool(val);
        else if (lk == "max-packet-size")       maxPacketSize = toInt(val, maxPacketSize);
        else if (lk == "compression-threshold") compressionThreshold = toInt(val, compressionThreshold);
        else if (lk == "auto-save")             autoSave = toBool(val);
        else if (lk == "auto-save-interval")    autoSaveInterval = toInt(val, autoSaveInterval);
        else if (lk == "vanilla-mirror")         vanillaMirror = toBool(val);            // VANILLA_MIRROR_V1
        else if (lk == "vanilla-mirror-interval") vanillaMirrorInterval = toInt(val, vanillaMirrorInterval); // VANILLA_MIRROR_V1
        else if (lk == "log-level")             logLevel = toUpper(val);
        // RCON_V1
        else if (lk == "enable-rcon")           enableRcon = toBool(val);
        else if (lk == "rcon.port")             rconPort = toInt(val, rconPort);
        else if (lk == "rcon.password")         rconPassword = val;
        else if (lk == "rcon.max-clients")      rconMaxClients = toInt(val, rconMaxClients);
        else if (lk == "rcon.log-commands")     rconLogCommands = toBool(val);
        else if (lk == "auto-start-panel")       autoStartPanel = toBool(val); // AUTOSTARTPANEL_V1
    }

    static std::string toUpper(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); }); // WARNFIX_V2
        return s;
    }

    // ── Загрузка из settings.properties ──
    static ServerConfig loadFrom(const std::filesystem::path& path) {
        ServerConfig cfg;
        std::ifstream in(path);
        if (!in.is_open()) return cfg;

        std::string line;
        while (std::getline(in, line)) {
            // Пропускаем комментарии и пустые строки
            std::string trimmed = line;
            trimmed.erase(0, trimmed.find_first_not_of(" \t"));
            if (trimmed.empty() || trimmed[0] == '#') continue;

            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            cfg.setProperty(key, val);
        }

        return cfg;
    }

    // ── Сохранение в settings.properties ──
    // SETTINGS_V10: все комментарии пишутся через cm(ru, en), чтобы ни одна
    // строка не осталась без перевода.
    void saveTo(const std::filesystem::path& path) const {
        const bool ru = (language == "rus");
        std::ofstream out(path);
        auto cm = [&](const char* r, const char* e) { out << (ru ? r : e) << "\n"; };
        auto blank = [&]() { out << "\n"; };

        out << "# ============================================\n";
        cm("# Конфигурация сервера Zevvoryn", "# Zevvoryn Server Configuration");
        cm("# Создано мастером установки", "# Generated by the setup wizard");
        out << "# ============================================\n";
        blank();

        cm("# Язык вывода в консоли и в этом файле", "# Language of the console output and of this file");
        cm("# Варианты: eng, rus", "# Options: eng, rus");
        out << "language=" << language << "\n";
        blank();

        cm("# === Сервер ===", "# === Server ===");
        cm("# MOTD — первая строка описания сервера в списке серверов",
           "# MOTD — first line of the server description in the server list");
        out << "motd=" << motd << "\n";
        cm("# Имя ядра, которое клиент показывает в F3",
           "# Server brand shown by the client in the F3 screen");
        out << "brand=" << brand << "\n";
        cm("# Вторая строка MOTD. Пусто = описание в одну строку",
           "# Second MOTD line. Empty = single-line description");
        out << "sub-motd=" << subMotd << "\n";
        cm("# Порт для подключения Java-клиентов (TCP)", "# Listening port for Java clients (TCP)");
        out << "server-port=" << port << "\n";
        if (portV6 > 0) {
            cm("# Порт для IPv6 (опционально)", "# IPv6 listening port (optional)");
            out << "server-portv6=" << portV6 << "\n";
        }
        cm("# Максимальное количество игроков на сервере", "# Maximum number of players on the server");
        out << "max-players=" << maxPlayers << "\n";
        cm("# Дальность прорисовки в чанках (сколько мира видит игрок)",
           "# View distance in chunks (how much world the player sees)");
        out << "view-distance=" << viewDistance << "\n";
        cm("# Дальность симуляции в чанках: ближе этого радиуса тикают мобы,",
           "# Simulation distance in chunks: mobs, redstone and physics only tick");
        cm("# редстоун и физика. Ниже значение = меньше нагрузки на CPU",
           "# inside this radius. Lower value = less CPU load");
        out << "simulation-distance=" << simulationDistance << "\n";
        cm("# Подсказка по памяти в ГБ (0 = без лимита). Это не жёсткий потолок:",
           "# Memory hint in GB (0 = no limit). This is not a hard cap:");
        cm("# сервер по нему подбирает размеры кэшей чанков",
           "# the server uses it to pick chunk cache sizes");
        out << "max-ram-gb=" << maxRamGb << "\n";
        cm("# Сколько ядер CPU отдать генерации мира (0 = авто по числу ядер)",
           "# CPU cores used for world generation (0 = auto-detect)");
        out << "max-cores=" << maxCores << "\n";
        blank();

        cm("# === Мир ===", "# === World ===");
        cm("# Имя папки с миром", "# World folder name");
        out << "level-name=" << levelName << "\n";
        cm("# Генератор: DEFAULT (обычный), FLAT (плоский), VOID (пустой)",
           "# Generator: DEFAULT (normal), FLAT (superflat), VOID (empty)");
        out << "generator=" << generator << "\n";
        cm("# Сид мира (0 = случайный)", "# World seed (0 = random)");
        out << "level-seed=" << levelSeed << "\n";
        cm("# Создавать мир Ада (Nether). false = измерения нет, порталы не зажигаются",
           "# Generate the Nether. false = no such dimension, portals stay dead");
        out << "enable-nether=" << boolToStr(enableNether) << "\n";
        cm("# Создавать мир Энда (End). false = измерения нет, око Эндера не сработает",
           "# Generate the End. false = no such dimension, the eye of ender does nothing");
        out << "enable-end=" << boolToStr(enableEnd) << "\n";
        blank();

        cm("# === Геймплей ===", "# === Gameplay ===");
        cm("# Режим игры для новичков: survival, creative, adventure, spectator",
           "# Game mode for new players: survival, creative, adventure, spectator");
        out << "gamemode=" << gamemode << "\n";
        cm("# true = каждый вход возвращает игрока в режим gamemode выше,",
           "# true = every login puts the player back into the gamemode above,");
        cm("# даже если ему меняли режим через /gamemode. false = режим запоминается",
           "# even if it was changed with /gamemode. false = the mode is remembered");
        out << "force-gamemode=" << boolToStr(forceGamemode) << "\n";
        cm("# true = пускаем только клиентов 1.21.1 (протокол 767), остальным показываем",
           "# true = only 1.21.1 clients (protocol 767) may join, everyone else gets");
        cm("# вежливое сообщение. false = версию не проверяем — нужно, если впереди",
           "# a polite message. false = no version check - needed when a proxy or a");
        cm("# стоит прокси или плагин вроде ViaVersion, который сам переводит протокол",
           "# ViaVersion-like plugin in front of the server translates the protocol");
        out << "strict-version-check=" << boolToStr(strictVersionCheck) << "\n"; // DISCMSG_V1
        cm("# Оболочка сервера: classic = обычное окно консоли (cmd/Windows Terminal),",
           "# Server shell: classic = the usual console window (cmd/Windows Terminal),");
        cm("# gui = собственное окно с цветным логом и корректным закрытием по крестику.",
           "# gui = our own window with a colored log and a proper close-button shutdown.");
        cm("# Вне Windows и при любой ошибке значение gui тихо становится classic.",
           "# Outside Windows, or on any failure, gui silently falls back to classic.");
        out << "console-mode=" << consoleMode << "\n"; // GUICON_V1
        cm("# Разрешить игрокам бить друг друга. false = удары по игрокам не наносят урона",
           "# Allow players to damage each other. false = hits on players deal no damage");
        out << "pvp=" << boolToStr(pvp) << "\n";
        cm("# Сложность: 0=Мирный, 1=Легко, 2=Норма, 3=Сложно",
           "# Difficulty: 0=Peaceful, 1=Easy, 2=Normal, 3=Hard");
        out << "difficulty=" << difficulty << "\n";
        cm("# Радиус защиты спавна в блоках (0 = выкл). Внутри радиуса",
           "# Spawn protection radius in blocks (0 = off). Inside the radius");
        cm("# ставить и ломать блоки могут только операторы из ops.json",
           "# only operators listed in ops.json can place and break blocks");
        out << "spawn-protection=" << spawnProtection << "\n";
        blank();

        cm("# === Microsoft Auth ===", "# === Microsoft Auth ===");
        cm("# Требовать аутентификацию Microsoft. true = пускаем только владельцев",
           "# Require Microsoft authentication. true = only players who own");
        cm("# лицензионного Minecraft, false = пускаем также пиратские клиенты",
           "# a licensed Minecraft account, false = cracked clients are allowed too");
        out << "xbox-auth=" << boolToStr(xboxAuth) << "\n";
        cm("# Пускать только игроков из whitelist.json",
           "# Only allow players listed in whitelist.json");
        out << "white-list=" << boolToStr(whiteList) << "\n";
        blank();

        cm("# === Сеть ===", "# === Network ===");
        cm("# На каком IP-адресе машины слушать. Пусто = на всех сетевых",
           "# Which local IP address to listen on. Empty = listen on every network");
        cm("# интерфейсах (обычно именно это и нужно)",
           "# interface (this is what you normally want)");
        out << "server-ip=" << serverIp << "\n";
        cm("# Принимать подключения по IPv6. true = слушаем dual-stack сокет,",
           "# Accept IPv6 connections. true = listen on a dual-stack socket,");
        cm("# то есть СРАЗУ и IPv4, и IPv6 на том же порту. Нужно только",
           "# meaning BOTH IPv4 and IPv6 on the same port. Only needed if some");
        cm("# если у части игроков провайдер без IPv4 (редкость)",
           "# of your players have an IPv6-only provider (rare)");
        out << "enable-ipv6=" << boolToStr(enableIpv6) << "\n";
        cm("# Защита от мусорных пакетов: всё, что больше этого размера, рвёт соединение",
           "# Junk-packet guard: anything larger than this size drops the connection");
        out << "max-packet-size=" << maxPacketSize << "\n";
        cm("# Сжимать пакеты больше этого размера в байтах. -1 = сжатие выключено",
           "# Compress packets larger than this size in bytes. -1 = compression off");
        cm("# (быстрее CPU, но сильно больше трафика; ванильное значение 256)",
           "# (less CPU but far more traffic; the vanilla value is 256)");
        out << "compression-threshold=" << compressionThreshold << "\n";
        blank();

        cm("# === Автосохранение ===", "# === Auto-save ===");
        cm("# Автоматически сохранять мир по расписанию", "# Automatically save the world on a schedule");
        out << "auto-save=" << boolToStr(autoSave) << "\n";
        cm("# Интервал сохранения в секундах (минимум 30)", "# Save interval in seconds (minimum 30)");
        out << "auto-save-interval=" << autoSaveInterval << "\n";
        out << "vanilla-mirror=" << boolToStr(vanillaMirror) << "\n";                 // VANILLA_MIRROR_V1
        out << "vanilla-mirror-interval=" << vanillaMirrorInterval << "\n";            // VANILLA_MIRROR_V1
        blank();

        cm("# === Логирование ===", "# === Logging ===");
        cm("# Уровень логов: TRACE, DEBUG, INFO, WARN, ERROR", "# Log level: TRACE, DEBUG, INFO, WARN, ERROR");
        out << "log-level=" << logLevel << "\n";
        blank();

        cm("# === RCON ===", "# === RCON ===");
        cm("# RCON — удалённое управление консолью сервера. Нужно",
           "# RCON is remote access to the server console. Required by");
        cm("# Discord-боту и веб-панели, иначе они не смогут выполнять команды",
           "# the Discord bot and the web panel, otherwise they cannot run commands");
        out << "enable-rcon=" << boolToStr(enableRcon) << "\n";
        cm("# Порт RCON (не открывай его наружу без необходимости)",
           "# RCON port (do not expose it to the internet unless you must)");
        out << "rcon.port=" << rconPort << "\n";
        cm("# Пароль RCON — это полный доступ к консоли, никому его не показывай",
           "# The RCON password grants full console access, never share it");
        out << "rcon.password=" << rconPassword << "\n";
        cm("# Сколько RCON-клиентов могут быть подключены одновременно",
           "# How many RCON clients may be connected at the same time");
        out << "rcon.max-clients=" << rconMaxClients << "\n";
        cm("# Записывать в лог каждую команду, выполненную через RCON",
           "# Write every command executed through RCON into the log");
        out << "rcon.log-commands=" << boolToStr(rconLogCommands) << "\n";
        blank();

        cm("# Запускать Discord-бота и веб-панель вместе с сервером",
           "# Start the Discord bot and the web panel together with the server");
        cm("# (Windows: node.exe запускается в фоновом окне)",
           "# (Windows: node.exe runs in a background window)");
        out << "auto-start-panel=" << boolToStr(autoStartPanel) << "\n";
    }

private:
    static std::string boolToStr(bool v) { return v ? "true" : "false"; }
};

// ============================================================
// Интерактивный мастер установки (встроенный в exe)
// ============================================================
namespace setup {

// WIZARD_BACK_V1: ввод "b"/"back"/"назад" возвращает на предыдущий шаг мастера
inline bool g_back = false;
inline int g_prompts = 0; // WIZARD_BACK_V2: счётчик вопросов текущего шага

inline bool isBackInput(const std::string& input) {
    return input == "b" || input == "B" || input == "back" || input == "\xd0\xbd\xd0\xb0\xd0\xb7\xd0\xb0\xd0\xb4";
}

// WIZTEE_V1: в gui-режиме stdout уведён в NUL, а сама консоль отвязана через
// FreeConsole(). Из-за этого весь текст мастера, напечатанный обычным std::cout
// (списки вариантов, пояснения, предупреждения), просто пропадал: в окне
// оставались только заголовок шага и строка ввода, и было непонятно, из чего
// вообще выбираешь. Перехватываем std::cout построчно и отдаём в окно.
inline bool g_coutTee = false;

class GuiCoutTee : public std::streambuf {
public:
    GuiCoutTee() {
        prev_ = std::cout.rdbuf(this);
        g_coutTee = true;
    }
    ~GuiCoutTee() override {
        if (!line_.empty()) flushLine();
        std::cout.rdbuf(prev_);
        g_coutTee = false;
    }

protected:
    int overflow(int ch) override {
        if (ch == traits_type::eof()) return 0;
        const char c = static_cast<char>(ch);
        if (c == static_cast<char>(10)) flushLine();
        else if (c != static_cast<char>(13)) line_ += c;
        return ch;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        for (std::streamsize i = 0; i < n; ++i) overflow(static_cast<unsigned char>(s[i]));
        return n;
    }

private:
    void flushLine() {
        nc::log::rawLine(stripAnsi(line_));
        line_.clear();
    }
    // Цвета в окне свои, ANSI-последовательности из std::cout срезаем,
    // иначе в лог уедет мусор вида [36m.
    static std::string stripAnsi(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == static_cast<char>(27)) {
                while (i < s.size() && s[i] != 'm') ++i;
                continue;
            }
            out += s[i];
        }
        return out;
    }
    std::streambuf* prev_ = nullptr;
    std::string line_;
};

inline std::string readLine(const std::string& prompt, const std::string& defaultVal) {
    if (g_back) return defaultVal; // WIZARD_BACK_V1
    ++g_prompts;
    const std::string label = prompt + " [" + defaultVal + "]: ";
    std::string input;
    if (nc::log::hasGuiReadLine()) { // GUIWIZARD_V1: мастер идёт в окне-терминале
        nc::log::guiLine(label, 6);
        input = nc::log::guiReadLine();
    } else {
        std::cout << label;
        std::getline(std::cin, input);
    }
    if (isBackInput(input)) { g_back = true; return defaultVal; }
    const std::string val = input.empty() ? defaultVal : input;
    nc::log::rawLine(label + val); // LOGBANNER_V1: ответы мастера в лог
    return val;
}

inline i32 readInt(const std::string& prompt, i32 defaultVal) {
    if (g_back) return defaultVal; // WIZARD_BACK_V1
    ++g_prompts;
    const std::string label = prompt + " [" + std::to_string(defaultVal) + "]: ";
    std::string input;
    if (nc::log::hasGuiReadLine()) { // GUIWIZARD_V1
        nc::log::guiLine(label, 6);
        input = nc::log::guiReadLine();
    } else {
        std::cout << label;
        std::getline(std::cin, input);
    }
    if (isBackInput(input)) { g_back = true; return defaultVal; }
    i32 val = defaultVal;
    if (!input.empty()) { try { val = static_cast<i32>(std::stol(input)); } catch (...) {} }
    nc::log::rawLine(label + std::to_string(val)); // LOGBANNER_V1
    return val;
}

inline i64 readInt64(const std::string& prompt, i64 defaultVal) {
    if (g_back) return defaultVal; // WIZARD_BACK_V1
    ++g_prompts;
    const std::string label = prompt + " [" + std::to_string(defaultVal) + "]: ";
    std::string input;
    if (nc::log::hasGuiReadLine()) { // GUIWIZARD_V1
        nc::log::guiLine(label, 6);
        input = nc::log::guiReadLine();
    } else {
        std::cout << label;
        std::getline(std::cin, input);
    }
    if (isBackInput(input)) { g_back = true; return defaultVal; }
    i64 val = defaultVal;
    if (!input.empty()) { try { val = std::stoll(input); } catch (...) {} }
    nc::log::rawLine(label + std::to_string(val)); // LOGBANNER_V1
    return val;
}

inline bool readBool(const std::string& prompt, bool defaultVal) {
    if (g_back) return defaultVal; // WIZARD_BACK_V1
    ++g_prompts;
    const std::string def = defaultVal ? "y" : "n";
    const std::string label = prompt + " [" + def + "]: ";
    std::string input;
    if (nc::log::hasGuiReadLine()) { // GUIWIZARD_V1
        nc::log::guiLine(label, 6);
        input = nc::log::guiReadLine();
    } else {
        std::cout << label;
        std::getline(std::cin, input);
    }
    if (isBackInput(input)) { g_back = true; return defaultVal; }
    const bool val = input.empty() ? defaultVal : (input[0] == 'y' || input[0] == 'Y');
    nc::log::rawLine(label + (val ? "y" : "n")); // LOGBANNER_V1
    return val;
}

// UTF-8 box drawing helpers
inline const char* BOX_TL = "\xe2\x95\x94"; // ╔
inline const char* BOX_TR = "\xe2\x95\x97"; // ╗
inline const char* BOX_BL = "\xe2\x95\x9a"; // ╚
inline const char* BOX_BR = "\xe2\x95\x9d"; // ╝
inline const char* BOX_H  = "\xe2\x95\x90"; // ═
inline const char* BOX_V  = "\xe2\x95\x91"; // ║

inline std::string boxLine(int width) {
    std::string s = "  ";
    s += BOX_TL;
    for (int i = 0; i < width; ++i) s += BOX_H;
    s += BOX_TR;
    return s;
}
inline std::string boxBottom(int width) {
    std::string s = "  ";
    s += BOX_BL;
    for (int i = 0; i < width; ++i) s += BOX_H;
    s += BOX_BR;
    return s;
}
// WIZARD_UI_V1: ширина в терминальных колонках (UTF-8)
// NO_HARDCORE_V1: поле hardcore полностью удалено
inline int displayCols(const std::string& s) {
    int cols = 0;
    for (unsigned char ch : s) if ((ch & 0xC0) != 0x80) ++cols;
    return cols;
}

inline std::string boxRow(const std::string& content, int width) {
    int pad = width - displayCols(content);
    int left = pad / 2;
    int right = pad - left;
    std::string s = "  ";
    s += BOX_V;
    for (int i = 0; i < left; ++i) s += ' ';
    s += content;
    for (int i = 0; i < right; ++i) s += ' ';
    s += BOX_V;
    return s;
}

inline void printBanner() {
    // NOPIXELART_V1: большой логотип из блоковых символов █ убран совсем — выглядел как грубые
    // пиксели и бил по глазам, особенно ярким цветом. Остаём только аккуратную рамку-плашку.
    std::cout << "\n";
    std::cout << "\033[36m";
    int w = 58;
    const std::string kRows[] = {
        boxLine(w),
        boxRow("ZEVVORYN SERVER", w),
        boxRow("Minecraft Java Edition 1.21.1", w),
        boxRow("C++20 Native \xe2\x94\x82 No Java \xe2\x94\x82 No Wrappers", w),
        boxBottom(w),
    };
    if (!g_coutTee) nc::log::rawLine(""); // LOGBANNER_V1 / WIZTEE_V1
    for (const auto& r : kRows) { std::cout << r << "\n"; if (!g_coutTee) nc::log::rawLine(r); }
    std::cout << "\033[0m\n";
    if (!g_coutTee) nc::log::rawLine(""); // LOGBANNER_V1 / WIZTEE_V1
}

inline void printStep(int num, int total, const std::string& title) {
    std::cout << "  [" << num << "/" << total << "] " << title << "\n";
    std::cout << "  " << std::string(44, '-') << "\n";
    if (!g_coutTee) nc::log::rawLine("  [" + std::to_string(num) + "/" + std::to_string(total) + "] " + title); // LOGBANNER_V1 / WIZTEE_V1
}

// RESOURCE_V1: определение железа (ядра + ОЗУ)
struct SysInfo { i32 cores = 0; i32 ramGb = 0; };
inline SysInfo detectSysInfo() {
    SysInfo si;
    unsigned hc = std::thread::hardware_concurrency();
    si.cores = (hc > 0) ? static_cast<i32>(hc) : 1;
#ifdef _WIN32
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        si.ramGb = static_cast<i32>((ms.ullTotalPhys + (1ull << 30) - 1) / (1ull << 30));
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && psize > 0)
        si.ramGb = static_cast<i32>(((u64)pages * (u64)psize + (1ull << 30) - 1) / (1ull << 30));
#endif
    return si;
}

// SINGLEPASS_V1: раньше установщик спрашивал три пароля: RCON, веб-панель и рут.
// Пароль RCON человеку не нужен вообще — это внутренний токен между сервером
// и панелью/ботом на той же машине, поэтому генерируем его сами.
inline std::string makeSecret(std::size_t len = 24) {
    static const char kAlphabet[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::random_device rd;
    std::mt19937_64 gen(((u64)rd() << 32) ^ (u64)rd());
    std::uniform_int_distribution<int> pick(0, (int)sizeof(kAlphabet) - 2);
    std::string out;
    out.reserve(len);
    for (std::size_t i = 0; i < len; ++i) out.push_back(kAlphabet[pick(gen)]);
    return out;
}

// SINGLEPASS_V1: если DiscrordBotRcon/.env уже существует, подменяем в нём строку
// RCON_PASSWORD, чтобы панель и сервер никогда не разъехались по токену.
inline void syncEnvRconPassword(const std::string& password) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path envPath = fs::path("DiscrordBotRcon") / ".env";
    if (!fs::exists(envPath, ec)) return;
    std::ifstream in(envPath, std::ios::binary);
    if (!in.is_open()) return;
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::string out;
    out.reserve(all.size() + 64);
    std::size_t pos = 0;
    bool replaced = false;
    while (pos <= all.size()) {
        std::size_t nl = all.find('\n', pos);
        std::string line = all.substr(pos, (nl == std::string::npos ? all.size() : nl) - pos);
        if (line.rfind("RCON_PASSWORD=", 0) == 0) {
            const bool crlf = (!line.empty() && line.back() == '\r');
            line = "RCON_PASSWORD=" + password + (crlf ? "\r" : "");
            replaced = true;
        }
        out += line;
        if (nl == std::string::npos) break;
        out += '\n';
        pos = nl + 1;
    }
    if (!replaced) out += "\r\nRCON_PASSWORD=" + password + "\r\n";
    std::ofstream ov(envPath, std::ios::binary | std::ios::trunc);
    if (ov.is_open()) ov << out;
}

inline ServerConfig runWizard() { // WIZARD_BACK_V1
    ServerConfig cfg;
    // WIZARD_RCON_BOT_V1 / WIZARD_WEB_V1
    bool wizardBotSetup = false;
    std::string wizardBotToken, wizardBotClientId, wizardBotGuildId;
    bool wizardWebEnabled = false;
    i32  wizardWebPort    = 3000;
    std::string wizardWebPassword;
    std::string wizardWebHost = "127.0.0.1";  // WIZARD_WEBHOST_V3: 127.0.0.1 | 0.0.0.0 | a specific interface IP
    i32  wizardWebSessionTtl = 720;         // WIZARD_WEBPASS_V2: жизнь сессии, минут
    // WIZARD_WHITELIST_V1
    std::vector<std::string> wizardWhitelistNames;

    // WIZTEE_V1: пока мастер идёт в окне-терминале, весь std::cout зеркалим в окно.
    std::unique_ptr<GuiCoutTee> guiTee;
    if (nc::log::hasGuiReadLine()) guiTee = std::make_unique<GuiCoutTee>();

    printBanner();
    std::cout << "  \xd0\x9f\xd0\xbe\xd0\xb4\xd1\x81\xd0\xba\xd0\xb0\xd0\xb7\xd0\xba\xd0\xb0\x3a\x20\xd0\xb2\xd0\xb2\xd0\xb5\xd0\xb4\xd0\xb8\xd1\x82\xd0\xb5\x20\x62\x20\x28\xd0\xb8\xd0\xbb\xd0\xb8\x20\"\xd0\xbd\xd0\xb0\xd0\xb7\xd0\xb0\xd0\xb4\"\x2c\x20\x62\x61\x63\x6b\x29\x2c\x20\xd1\x87\xd1\x82\xd0\xbe\xd0\xb1\xd1\x8b\x20\xd0\xb2\xd0\xb5\xd1\x80\xd0\xbd\xd1\x83\xd1\x82\xd1\x8c\xd1\x81\xd1\x8f\x20\xd0\xbd\xd0\xb0\x20\xd1\x88\xd0\xb0\xd0\xb3\x20\xd0\xbd\xd0\xb0\xd0\xb7\xd0\xb0\xd0\xb4\n";
    std::cout << "  Hint: type b (or \"back\") to return to the previous step\n\n";

    bool ru = true;
    int step = 1;
    while (step <= 10) { // WIZARD_CONSOLE_V1 WIZARD_RCON_BOT_V1 WIZARD_WEB_V1 WIZARD_WHITELIST_V1
        g_back = false;
        g_prompts = 0;
        switch (step) {
        case 1: {
            // ── Шаг 1: Язык (первый, чтобы знать остальные) ──
            printStep(1, 10, "Language / \xd0\xaf\xd0\xb7\xd1\x8b\xd0\xba");
            std::cout << "    1) \xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9 (Russian)\n";
            std::cout << "    2) English\n";
            std::string langChoice = readLine("  Select / \xd0\x92\xd1\x8b\xd0\xb1\xd0\xb5\xd1\x80\xd0\xb8\xd1\x82\xd0\xb5", "1");
            cfg.language = (langChoice == "2" || langChoice == "eng") ? "eng" : "rus";
            ru = (cfg.language == "rus"); // WIZARD_BACK_V1
            std::cout << "\n";
            break;
        }
        case 2: {
            // ── Шаг 2: Сервер ──
            printStep(2, 10, ru ? "\xd0\xa1\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80" : "Server");
            cfg.motd = readLine(ru ? "  \xd0\x98\xd0\xbc\xd1\x8f (MOTD)" : "  Server name (MOTD)", cfg.motd);
            cfg.port = readInt(ru ? "  \xd0\x9f\xd0\xbe\xd1\x80\xd1\x82" : "  Port", cfg.port);
            cfg.maxPlayers = readInt(ru ? "  \xd0\x9c\xd0\xb0\xd0\xba\xd1\x81. \xd0\xb8\xd0\xb3\xd1\x80\xd0\xbe\xd0\xba\xd0\xbe\xd0\xb2" : "  Max players", cfg.maxPlayers);
            cfg.viewDistance = readInt(ru ? "  \xd0\x94\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c \xd0\xbf\xd1\x80\xd0\xbe\xd1\x80\xd0\xb8\xd1\x81\xd0\xbe\xd0\xb2\xd0\xba\xd0\xb8 (\xd1\x87\xd0\xb0\xd0\xbd\xd0\xba\xd0\xb8)" : "  View distance (chunks)", cfg.viewDistance);
            cfg.simulationDistance = readInt(ru ? "  \xd0\x94\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c \xd1\x81\xd0\xb8\xd0\xbc\xd1\x83\xd0\xbb\xd1\x8f\xd1\x86\xd0\xb8\xd0\xb8" : "  Simulation distance (chunks)", cfg.simulationDistance);
            std::cout << "\n";
            break;
        }
        case 3: {
            // ── Шаг 3: Мир ──
            printStep(3, 10, ru ? "\xd0\x9c\xd0\xb8\xd1\x80" : "World");
            std::cout << "    1) DEFAULT" << (ru ? " (\xd0\xbe\xd0\xb1\xd1\x8b\xd1\x87\xd0\xbd\xd1\x8b\xd0\xb9 \xe2\x80\x94 \xd0\xb1\xd0\xb8\xd0\xbe\xd0\xbc\xd1\x8b, \xd1\x80\xd0\xb5\xd0\xbb\xd1\x8c\xd0\xb5\xd1\x84, \xd1\x80\xd0\xb5\xd0\xba\xd0\xbe\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb4\xd1\x83\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f)" : " (normal - biomes & terrain, recommended)") << "\n";
            std::cout << "    2) FLAT   " << (ru ? "(\xd0\xbf\xd0\xbb\xd0\xbe\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9 \xd0\xbc\xd0\xb8\xd1\x80)" : "(superflat)") << "\n";
            std::string genChoice = readLine(ru ? "  \xd0\x93\xd0\xb5\xd0\xbd\xd0\xb5\xd1\x80\xd0\xb0\xd1\x82\xd0\xbe\xd1\x80" : "  Generator", "1");
            cfg.generator = (genChoice == "2") ? "FLAT" : "DEFAULT"; // WIZARD_WORLDTYPE_V1: DEFAULT теперь выбор по умолчанию (Enter)
            cfg.levelSeed = readInt64(ru ? "  \xd0\xa1\xd0\xb8\xd0\xb4 (0 = \xd1\x81\xd0\xbb\xd1\x83\xd1\x87\xd0\xb0\xd0\xb9\xd0\xbd\xd0\xbe\xd0\xb5)" : "  Seed (0 = random)", cfg.levelSeed);
            // DIMTOGGLE_V1: спрашиваем, нужны ли вообще Ад и Энд.
            cfg.enableNether = readBool(ru ? "  Создавать мир Ада (Nether) y/n" : "  Generate the Nether y/n", cfg.enableNether);
            cfg.enableEnd = readBool(ru ? "  Создавать мир Энда (End) y/n" : "  Generate the End y/n", cfg.enableEnd);
            if (!cfg.enableNether || !cfg.enableEnd)
                std::cout << (ru ? "  Ок: выключенный мир не генерируется, его порталы не зажигаются, /nether и /end скажут, что мир выключен.\n"
                                 : "  Ok: a disabled dimension is never generated, its portals stay dead, /nether and /end will refuse.\n");
            std::cout << "\n";
            break;
        }
        case 4: {
            // ── Шаг 4: Геймплей ──
            printStep(4, 10, ru ? "\xd0\x93\xd0\xb5\xd0\xb9\xd0\xbc\xd0\xbf\xd0\xbb\xd0\xb5\xd0\xb9" : "Gameplay");
            std::cout << "    1) survival " << (ru ? "(\xd0\xb2\xd1\x8b\xd0\xb6\xd0\xb8\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xb8\xd0\xb5)" : "(survival)") << "\n";
            std::cout << "    2) creative " << (ru ? "(\xd0\xba\xd1\x80\xd0\xb5\xd0\xb0\xd1\x82\xd0\xb8\xd0\xb2)" : "(creative)") << "\n";
            std::cout << "    3) adventure" << (ru ? " (\xd0\xbf\xd1\x80\xd0\xb8\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5)" : " (adventure)") << "\n";
            std::cout << "    4) spectator" << (ru ? " (\xd0\xbd\xd0\xb0\xd0\xb1\xd0\xbb\xd1\x8e\xd0\xb4\xd0\xb0\xd1\x82\xd0\xb5\xd0\xbb\xd1\x8c)" : " (spectator)") << "\n";
            std::string gmChoice = readLine(ru ? "  \xd0\xa0\xd0\xb5\xd0\xb6\xd0\xb8\xd0\xbc \xd0\xbf\xd0\xbe \xd1\x83\xd0\xbc\xd0\xbe\xd0\xbb\xd1\x87\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x8e" : "  Default gamemode", "2");
            if (gmChoice == "1") cfg.gamemode = "survival";
            else if (gmChoice == "3") cfg.gamemode = "adventure";
            else if (gmChoice == "4") cfg.gamemode = "spectator";
            else cfg.gamemode = "creative";

            cfg.pvp = readBool(ru ? "  PvP y/n (\xd1\x80\xd0\xb5\xd0\xba\xd0\xbe\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb4\xd1\x83\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f: y)" : "  PvP y/n (recommended: y)", cfg.pvp); // WIZARD_UI_V1
            std::cout << "    0=Peaceful  1=Easy  2=Normal  3=Hard\n";
            cfg.difficulty = readInt(ru ? "  \xd0\xa1\xd0\xbb\xd0\xbe\xd0\xb6\xd0\xbd\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c" : "  Difficulty", cfg.difficulty);
            cfg.spawnProtection = readInt(ru ? "  \xd0\x97\xd0\xb0\xd1\x89\xd0\xb8\xd1\x82\xd0\xb0 \xd1\x81\xd0\xbf\xd0\xb0\xd0\xb2\xd0\xbd\xd0\xb0 (0=\xd0\xb2\xd1\x8b\xd0\xba\xd0\xbb)" : "  Spawn protection (0=off)", cfg.spawnProtection);
            std::cout << "\n";
            break;
        }
        case 5: {
            // ── Шаг 5: Продвинутое ──
            printStep(5, 10, ru ? "\xd0\x94\xd0\xbe\xd0\xbf\xd0\xbe\xd0\xbb\xd0\xbd\xd0\xb8\xd1\x82\xd0\xb5\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe" : "Advanced");
            // RESOURCE_V1: RAM, cores and a recommended base derived from hardware + chosen limits
            {
                SysInfo si = detectSysInfo();
                std::cout << (ru ? "\x20\x20\xd0\x9e\xd0\xb1\xd0\xbd\xd0\xb0\xd1\x80\xd1\x83\xd0\xb6\xd0\xb5\xd0\xbd\xd0\xbe\x20\xd0\xb6\xd0\xb5\xd0\xbb\xd0\xb5\xd0\xb7\xd0\xbe\x3a\x20" : "  Detected hardware: ")
                          << si.cores << (ru ? "\x20\xd1\x8f\xd0\xb4\xd0\xb5\xd1\x80\x2c\x20" : " cores, ")
                          << si.ramGb << (ru ? "\x20\xd0\x93\xd0\x91\x20\xd0\x9e\xd0\x97\xd0\xa3\n" : " GB RAM\n");
                std::cout << (ru ? "\x20\x20\xd0\x9b\xd0\xb8\xd0\xbc\xd0\xb8\xd1\x82\x20\xd0\x9e\xd0\x97\xd0\xa3\x3a\n" : "  RAM limit:\n");
                std::cout << "    1) 1 GB\n    2) 2 GB\n    3) 3 GB\n    4) 4 GB\n    5) 5 GB\n";
                std::cout << (ru ? "\x20\x20\x20\x20\x36\x29\x20\xd1\x81\xd0\xb2\xd0\xbe\xd0\xb9\x20\xd0\xb2\xd1\x8b\xd0\xb1\xd0\xbe\xd1\x80\n\x20\x20\x20\x20\x30\x29\x20\xd0\xb0\xd0\xb2\xd1\x82\xd0\xbe\x20\x28\xd0\xb1\xd0\xb5\xd0\xb7\x20\xd0\xbb\xd0\xb8\xd0\xbc\xd0\xb8\xd1\x82\xd0\xb0\x29\n" : "    6) custom\n    0) auto (no limit)\n");
                std::string ramChoice = readLine(ru ? "\x20\x20\xd0\x92\xd1\x8b\xd0\xb1\xd0\xbe\xd1\x80" : "  Choice", "0");
                if (ramChoice == "6") cfg.maxRamGb = readInt(ru ? "\x20\x20\xd0\x93\xd0\x91\x20\xd0\x9e\xd0\x97\xd0\xa3" : "  RAM (GB)", si.ramGb > 0 ? si.ramGb : 4);
                else if (ramChoice.empty() || ramChoice == "0") cfg.maxRamGb = 0;
                else { try { cfg.maxRamGb = std::stoi(ramChoice); } catch (...) { cfg.maxRamGb = 0; } }
                if (cfg.maxRamGb < 0) cfg.maxRamGb = 0;
                if (cfg.maxRamGb > 64) cfg.maxRamGb = 64;
                cfg.maxCores = readInt(ru ? "\x20\x20\xd0\xaf\xd0\xb4\xd1\x80\xd0\xb0\x20\x43\x50\x55\x20\x28\x30\x20\x3d\x20\xd0\xb0\xd0\xb2\xd1\x82\xd0\xbe\x29" : "  CPU cores (0 = auto)", 0);
                if (cfg.maxCores < 0) cfg.maxCores = 0;
                if (si.cores > 0 && cfg.maxCores > si.cores * 2) cfg.maxCores = si.cores * 2;
                i32 effRam = (cfg.maxRamGb > 0) ? cfg.maxRamGb : si.ramGb;
                if (cfg.maxRamGb > 0 && si.ramGb > 0 && si.ramGb < effRam) effRam = si.ramGb;
                i32 effCores = (cfg.maxCores > 0) ? cfg.maxCores : si.cores;
                if (si.cores > 0 && effCores > si.cores) effCores = si.cores;
                if (effRam <= 0) effRam = 4;
                if (effCores <= 0) effCores = 2;
                i32 recView = (effRam <= 2) ? 6 : (effRam <= 4) ? 8 : (effRam <= 6) ? 10 : (effRam <= 8) ? 12 : 16;
                i32 recSim = (effCores <= 2) ? 4 : (effCores <= 4) ? 6 : (effCores <= 6) ? 8 : 10;
                if (recSim > recView) recSim = recView;
                i32 recPlayers = effRam * 5;
                if (recPlayers < 4) recPlayers = 4;
                if (recPlayers > 200) recPlayers = 200;
                std::cout << (ru ? "\x20\x20\xd0\xa0\xd0\xb5\xd0\xba\xd0\xbe\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb4\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xbd\xd0\xb0\xd1\x8f\x20\xd0\xb1\xd0\xb0\xd0\xb7\xd0\xb0\x20\x28\xd0\xbf\xd0\xbe\x20\xd0\xb6\xd0\xb5\xd0\xbb\xd0\xb5\xd0\xb7\xd1\x83\x20\xd0\xb8\x20\xd0\xbb\xd0\xb8\xd0\xbc\xd0\xb8\xd1\x82\xd0\xb0\xd0\xbc\x29\x3a\n" : "  Recommended base (from hardware & limits):\n");
                std::cout << (ru ? "\x20\x20\x20\x20\xd0\x9f\xd1\x80\xd0\xbe\xd1\x80\xd0\xb8\xd1\x81\xd0\xbe\xd0\xb2\xd0\xba\xd0\xb0\x3a\x20" : "    View distance: ") << recView << "\n";
                std::cout << (ru ? "\x20\x20\x20\x20\xd0\xa1\xd0\xb8\xd0\xbc\xd1\x83\xd0\xbb\xd1\x8f\xd1\x86\xd0\xb8\xd1\x8f\x3a\x20" : "    Simulation distance: ") << recSim << "\n";
                std::cout << (ru ? "\x20\x20\x20\x20\xd0\x9c\xd0\xb0\xd0\xba\xd1\x81\x2e\x20\xd0\xb8\xd0\xb3\xd1\x80\xd0\xbe\xd0\xba\xd0\xbe\xd0\xb2\x3a\x20" : "    Max players: ") << recPlayers << "\n";
                bool applyBase = readBool(ru ? "\x20\x20\xd0\x9f\xd1\x80\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x82\xd1\x8c\x20\xd1\x80\xd0\xb5\xd0\xba\xd0\xbe\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb4\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xbd\xd1\x83\xd1\x8e\x20\xd0\xb1\xd0\xb0\xd0\xb7\xd1\x83\x3f" : "  Apply recommended base?", true);
                if (applyBase) {
                    cfg.viewDistance = recView;
                    cfg.simulationDistance = recSim;
                    cfg.maxPlayers = recPlayers;
                }
            }
            cfg.xboxAuth = readBool(ru ? "  \xd0\x9e\xd0\xbd\xd0\xbb\xd0\xb0\xd0\xb9\xd0\xbd \xd1\x80\xd0\xb5\xd0\xb6\xd0\xb8\xd0\xbc (Mojang auth) y/n (\xd1\x80\xd0\xb5\xd0\xba\xd0\xbe\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb4\xd1\x83\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f: n)" : "  Online mode (Mojang auth) y/n (recommended: n)", cfg.xboxAuth); // WIZARD_UI_V1
            cfg.autoSave = readBool(ru ? "  \xd0\x90\xd0\xb2\xd1\x82\xd0\xbe\xd1\x81\xd0\xbe\xd1\x85\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5 y/n (\xd1\x80\xd0\xb5\xd0\xba\xd0\xbe\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb4\xd1\x83\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f: y)" : "  Auto-save y/n (recommended: y)", cfg.autoSave); // WIZARD_UI_V1
            if (cfg.autoSave) {
                cfg.autoSaveInterval = readInt(ru ? "  \xd0\x98\xd0\xbd\xd1\x82\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb0\xd0\xbb (\xd1\x81\xd0\xb5\xd0\xba)" : "  Interval (seconds)", cfg.autoSaveInterval);
            }
            if (ru) { // WIZARD_UI_V1
                std::cout << "  \xd0\xa3\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb5\xd0\xbd\xd1\x8c \xd0\xbb\xd0\xbe\xd0\xb3\xd0\xbe\xd0\xb2:\n";
                std::cout << "    1 = TRACE (\xd0\xbc\xd0\xb0\xd0\xba\xd1\x81\xd0\xb8\xd0\xbc\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe \xd0\xbf\xd0\xbe\xd0\xb4\xd1\x80\xd0\xbe\xd0\xb1\xd0\xbd\xd0\xbe)\n";
                std::cout << "    2 = DEBUG (\xd0\xbe\xd1\x82\xd0\xbb\xd0\xb0\xd0\xb4\xd0\xba\xd0\xb0)\n";
                std::cout << "    3 = INFO  (\xd0\xbe\xd0\xb1\xd1\x8b\xd1\x87\xd0\xbd\xd1\x8b\xd0\xb9 \xd1\x80\xd0\xb5\xd0\xb6\xd0\xb8\xd0\xbc, \xd1\x80\xd0\xb5\xd0\xba\xd0\xbe\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb4\xd1\x83\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f)\n";
                std::cout << "    4 = WARN  (\xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd0\xbf\xd1\x80\xd0\xb5\xd0\xb4\xd1\x83\xd0\xbf\xd1\x80\xd0\xb5\xd0\xb6\xd0\xb4\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x8f)\n";
                std::cout << "    5 = ERROR (\xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd0\xbe\xd1\x88\xd0\xb8\xd0\xb1\xd0\xba\xd0\xb8)\n";
            } else {
                std::cout << "  Log level:\n";
                std::cout << "    1 = TRACE (very verbose)\n";
                std::cout << "    2 = DEBUG\n";
                std::cout << "    3 = INFO  (recommended)\n";
                std::cout << "    4 = WARN\n";
                std::cout << "    5 = ERROR\n";
            }
            {
                i32 logChoice = readInt(ru ? "  \xd0\x92\xd1\x8b\xd0\xb1\xd0\xbe\xd1\x80 (1-5)" : "  Choice (1-5)", 3);
                if (logChoice == 1) cfg.logLevel = "TRACE";
                else if (logChoice == 2) cfg.logLevel = "DEBUG";
                else if (logChoice == 4) cfg.logLevel = "WARN";
                else if (logChoice == 5) cfg.logLevel = "ERROR";
                else cfg.logLevel = "INFO";
            }
            std::cout << "\n";
            break;
        }
        case 6: { // WIZARD_WHITELIST_V1
            printStep(6, 10, ru ? "\xd0\x91\xd0\xb5\xd0\xbb\xd1\x8b\xd0\xb9 \xd1\x81\xd0\xbf\xd0\xb8\xd1\x81\xd0\xbe\xd0\xba" : "Whitelist");
            if (ru) std::cout << "  \xd0\x91\xd0\xb5\xd0\xbb\xd1\x8b\xd0\xb9 \xd1\x81\xd0\xbf\xd0\xb8\xd1\x81\xd0\xbe\xd0\xba \xe2\x80\x94 \xd0\xbd\xd0\xb0 \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80 \xd1\x81\xd0\xbc\xd0\xbe\xd0\xb3\xd1\x83\xd1\x82 \xd0\xb7\xd0\xb0\xd0\xb9\xd1\x82\xd0\xb8 \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x83\xd0\xba\xd0\xb0\xd0\xb7\xd0\xb0\xd0\xbd\xd0\xbd\xd1\x8b\xd0\xb5 \xd0\xbd\xd0\xb8\xd0\xba\xd0\xb8.\n";
            else std::cout << "  Whitelist - only listed nicknames can join the server.\n";
            cfg.whiteList = readBool(ru ? "  \xd0\x92\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb8\xd1\x82\xd1\x8c \xd0\xb1\xd0\xb5\xd0\xbb\xd1\x8b\xd0\xb9 \xd1\x81\xd0\xbf\xd0\xb8\xd1\x81\xd0\xbe\xd0\xba y/n" : "  Enable whitelist y/n", false);
            if (cfg.whiteList) {
                std::string names = readLine(ru ? "  \xd0\x9d\xd0\xb8\xd0\xba\xd0\xb8 \xd1\x87\xd0\xb5\xd1\x80\xd0\xb5\xd0\xb7 \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x8f\xd1\x82\xd1\x83\xd1\x8e (Enter = \xd0\xbf\xd1\x83\xd1\x81\xd1\x82\xd0\xbe, \xd0\xb4\xd0\xbe\xd0\xb1\xd0\xb0\xd0\xb2\xd0\xb8\xd1\x82\xd1\x8c \xd0\xbf\xd0\xbe\xd0\xb7\xd0\xb6\xd0\xb5 \xd0\xba\xd0\xbe\xd0\xbc\xd0\xb0\xd0\xbd\xd0\xb4\xd0\xbe\xd0\xb9 whitelist add)" : "  Nicknames, comma-separated (Enter = none, add later via whitelist add)", "");
                std::stringstream ss(names);
                std::string tok;
                while (std::getline(ss, tok, ',')) {
                    tok.erase(0, tok.find_first_not_of(" \t\r\n"));
                    auto lastNs = tok.find_last_not_of(" \t\r\n");
                    if (lastNs != std::string::npos) tok.erase(lastNs + 1); else tok.clear();
                    if (!tok.empty()) wizardWhitelistNames.push_back(tok);
                }
                if (!wizardWhitelistNames.empty()) {
                    std::cout << (ru ? "  \xd0\x94\xd0\xbe\xd0\xb1\xd0\xb0\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xbe \xd0\xb2 \xd1\x81\xd0\xbf\xd0\xb8\xd1\x81\xd0\xbe\xd0\xba: " : "  Added to list: ") << wizardWhitelistNames.size() << "\n";
                }
            }
            std::cout << "\n";
            break;
        }
        case 7: { // WIZARD_RCON_BOT_V1
            printStep(7, 10, "RCON");
            if (ru) std::cout << "  RCON \xe2\x80\x94 \xd1\x83\xd0\xb4\xd0\xb0\xd0\xbb\xd1\x91\xd0\xbd\xd0\xbd\xd0\xb0\xd1\x8f \xd0\xba\xd0\xbe\xd0\xbd\xd1\x81\xd0\xbe\xd0\xbb\xd1\x8c. \xd0\x9d\xd1\x83\xd0\xb6\xd0\xbd\xd0\xb0 \xd0\xb4\xd0\xbb\xd1\x8f Discord-\xd0\xb1\xd0\xbe\xd1\x82\xd0\xb0 \xd0\xb8 \xd0\xb2\xd0\xb5\xd0\xb1-\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd0\xb8.\n";
            else std::cout << "  RCON - remote console. Required for Discord bot and web panel.\n";
            cfg.enableRcon = readBool(ru ? "  \xd0\x92\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb8\xd1\x82\xd1\x8c RCON y/n" : "  Enable RCON y/n", false);
            if (cfg.enableRcon) {
                cfg.rconPort = readInt(ru ? "  \xd0\x9f\xd0\xbe\xd1\x80\xd1\x82 RCON" : "  RCON port", cfg.rconPort);
                // SINGLEPASS_V1: пароль RCON больше не спрашиваем: это внутренний токен,
                // который человек всё равно никогда не вводит руками.
                if (cfg.rconPassword.empty()) cfg.rconPassword = makeSecret(24);
                std::cout << (ru ? "  Пароль RCON сгенерирован автоматически и попадёт в .env — запоминать его не нужно.\n"
                                 : "  The RCON password was generated automatically and goes into .env - you never need it.\n");
                cfg.rconMaxClients = readInt(ru ? "  Макс. клиентов RCON" : "  Max RCON clients", cfg.rconMaxClients);
                cfg.rconLogCommands = readBool(ru ? "  Логировать команды RCON y/n" : "  Log RCON commands y/n", true);
            }
            std::cout << "\n";
            break;
        }
        case 8: { // WIZARD_RCON_BOT_V1: Discord
            if (!cfg.enableRcon) break;
            printStep(8, 10, ru ? "Discord-\xd0\xb1\xd0\xbe\xd1\x82" : "Discord bot");
            if (ru) {
                std::cout << "  !! \xd0\x9d\xd0\x90 WINDOWS \xd0\xb1\xd0\xbe\xd1\x82 \xd0\x90\xd0\x92\xd0\xa2\xd0\x9e\xd0\x9c\xd0\x90\xd0\xa2\xd0\x98\xd0\xa7\xd0\x95\xd0\xa1\xd0\x9a\xd0\x98 \xd0\x9d\xd0\x95 \xd0\x97\xd0\x90\xd0\x9f\xd0\xa3\xd0\xa1\xd0\xa2\xd0\x98\xd0\xa2\xd0\xa1\xd0\xaf.\n";
                std::cout << "     \xd0\x94\xd0\xbb\xd1\x8f \xd0\xb0\xd0\xb2\xd1\x82\xd0\xbe\xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba\xd0\xb0: VPS/Linux + pm2.\n";
            } else std::cout << "  !! On WINDOWS the bot won't start automatically. Use VPS/Linux + pm2.\n";
            wizardBotSetup = readBool(ru ? "  \xd0\x9d\xd0\xb0\xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xb8\xd1\x82\xd1\x8c Discord-\xd0\xb1\xd0\xbe\xd1\x82 y/n" : "  Set up Discord bot y/n", false);
            if (wizardBotSetup) {
                std::cout << (ru ? "  \xd0\x9e\xd1\x82\xd0\xba\xd1\x80\xd0\xbe\xd0\xb9\xd1\x82\xd0\xb5 https://discord.com/developers/applications\n" : "  Open https://discord.com/developers/applications\n");
                wizardBotToken    = readLine("  Bot Token", "");
                wizardBotClientId = readLine("  Application ID", "");
                wizardBotGuildId  = readLine("  Guild/Server ID", "");
                if (wizardBotToken.empty() || wizardBotClientId.empty()) {
                    std::cout << (ru ? "  Token \xd0\xbf\xd1\x83\xd1\x81\xd1\x82 \xe2\x80\x94 \xd0\xbd\xd0\xb0\xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xb9\xd0\xba\xd0\xb0 \xd0\xbf\xd1\x80\xd0\xbe\xd0\xbf\xd1\x83\xd1\x89\xd0\xb5\xd0\xbd\xd0\xb0.\n" : "  Token empty - skipped.\n");
                    wizardBotSetup = false;
                }
            }
            std::cout << "\n";
            break;
        }
        case 9: { // WIZARD_WEB_V1: web panel
            if (!cfg.enableRcon) break;
            printStep(9, 10, ru ? "\xd0\x92\xd0\xb5\xd0\xb1-\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c" : "Web panel");
            if (ru) {
                std::cout << "  \xd0\x92\xd0\xb5\xd0\xb1-\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c RCON \xe2\x80\x94 \xd0\xb0\xd0\xb4\xd0\xbc\xd0\xb8\xd0\xbd\xd0\xba\xd0\xb0 \xd0\xb2 \xd0\xb1\xd1\x80\xd0\xb0\xd1\x83\xd0\xb7\xd0\xb5\xd1\x80\xd0\xb5.\n";
                std::cout << "  \xd0\x94\xd0\xbe\xd1\x81\xd1\x82\xd1\x83\xd0\xbf\xd0\xbd\xd0\xb0 \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x81 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb9 \xd0\xbc\xd0\xb0\xd1\x88\xd0\xb8\xd0\xbd\xd1\x8b (127.0.0.1).\n";
                std::cout << "  \xd0\x9d\xd0\xb0 VPS: SSH-\xd1\x82\xd1\x83\xd0\xbd\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c ssh -L <\xd0\xbf\xd0\xbe\xd1\x80\xd1\x82>:127.0.0.1:<\xd0\xbf\xd0\xbe\xd1\x80\xd1\x82> user@host\n";
            } else {
                std::cout << "  RCON web panel - browser admin UI, http://127.0.0.1:<port> only.\n";
                std::cout << "  On VPS: SSH tunnel ssh -L <port>:127.0.0.1:<port> user@host\n";
            }
            wizardWebEnabled = readBool(ru ? "  \xd0\x92\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb8\xd1\x82\xd1\x8c \xd0\xb2\xd0\xb5\xd0\xb1-\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c y/n" : "  Enable web panel y/n", true);
            if (wizardWebEnabled) {
                // WIZARD_WEBHOST_V3: bind address for the panel (webpanel.js reads WEB_HOST)
                std::cout << (ru ? "  \xd0\x9a\xd1\x82\xd0\xbe \xd1\x81\xd0\xbc\xd0\xbe\xd0\xb6\xd0\xb5\xd1\x82 \xd0\xbe\xd1\x82\xd0\xba\xd1\x80\xd1\x8b\xd1\x82\xd1\x8c \xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c \xd0\xb2 \xd0\xb1\xd1\x80\xd0\xb0\xd1\x83\xd0\xb7\xd0\xb5\xd1\x80\xd0\xb5:\n" : "  Who will be able to open the panel in a browser:\n");
                std::cout << (ru ? "    1) 127.0.0.1 \xe2\x80\x94 \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x8d\xd1\x82\xd0\xb0 \xd0\xbc\xd0\xb0\xd1\x88\xd0\xb8\xd0\xbd\xd0\xb0 (\xd0\xb1\xd0\xb5\xd0\xb7\xd0\xbe\xd0\xbf\xd0\xb0\xd1\x81\xd0\xbd\xd0\xbe, \xd0\xb8\xd0\xb7\xd0\xb2\xd0\xbd\xd0\xb5 \xe2\x80\x94 \xd1\x87\xd0\xb5\xd1\x80\xd0\xb5\xd0\xb7 SSH-\xd1\x82\xd1\x83\xd0\xbd\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c)\n" : "    1) 127.0.0.1 - this machine only (safe; remote access via SSH tunnel)\n");
                std::cout << (ru ? "    2) 0.0.0.0   \xe2\x80\x94 \xd0\xbb\xd1\x8e\xd0\xb1\xd0\xbe\xd0\xb9 IP: \xd0\xbb\xd0\xbe\xd0\xba\xd0\xb0\xd0\xbb\xd0\xba\xd0\xb0, \xd0\xb4\xd1\x80\xd1\x83\xd0\xb3\xd0\xbe\xd0\xb9 \xd0\x9f\xd0\x9a, \xd0\xb8\xd0\xbd\xd1\x82\xd0\xb5\xd1\x80\xd0\xbd\xd0\xb5\xd1\x82 (\xd0\xbd\xd1\x83\xd0\xb6\xd0\xb5\xd0\xbd \xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c!)\n" : "    2) 0.0.0.0   - any IP: LAN, another PC, the internet (password required!)\n");
                const i32 webHostChoice = readInt((ru ? "  \xd0\x92\xd1\x8b\xd0\xb1\xd0\xbe\xd1\x80" : "  Choice"), 1);
                wizardWebHost = (webHostChoice == 2) ? "0.0.0.0" : "127.0.0.1";
                std::cout << (ru ? "  \xd0\x9d\xd1\x83\xd0\xb6\xd0\xb5\xd0\xbd \xd0\xba\xd0\xbe\xd0\xbd\xd0\xba\xd1\x80\xd0\xb5\xd1\x82\xd0\xbd\xd1\x8b\xd0\xb9 IP \xd0\xb8\xd0\xbd\xd1\x82\xd0\xb5\xd1\x80\xd1\x84\xd0\xb5\xd0\xb9\xd1\x81\xd0\xb0? \xd0\x92\xd0\xbf\xd0\xb8\xd1\x88\xd0\xb8\xd1\x82\xd0\xb5 \xd0\xb5\xd0\xb3\xd0\xbe \xd0\xbf\xd0\xbe\xd1\x82\xd0\xbe\xd0\xbc \xd0\xb2 DiscrordBotRcon/.env \xd0\xb2 WEB_HOST.\n" : "  Need a specific interface IP? Put it into WEB_HOST in DiscrordBotRcon/.env later.\n");
                wizardWebPort = readInt((ru ? "  \xd0\x9f\xd0\xbe\xd1\x80\xd1\x82 \xd0\xb2\xd0\xb5\xd0\xb1-\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd0\xb8" : "  Web panel port"), 3000);
                std::cout << (ru ? "  \xd0\x9f\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c \xd0\xbd\xd1\x83\xd0\xb6\xd0\xb5\xd0\xbd \xd0\xb4\xd0\xbb\xd1\x8f \xd0\xb2\xd1\x85\xd0\xbe\xd0\xb4\xd0\xb0 \xd0\xb2 \xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c \xd0\xb2 \xd0\xb1\xd1\x80\xd0\xb0\xd1\x83\xd0\xb7\xd0\xb5\xd1\x80\xd0\xb5. \xd0\x9f\xd1\x83\xd1\x81\xd1\x82\xd0\xbe = \xd0\xb2\xd1\x85\xd0\xbe\xd0\xb4 \xd0\xb1\xd0\xb5\xd0\xb7 \xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8f.\n" : "  The password is used to log into the panel in a browser. Empty = no login required.\n");
                wizardWebPassword = readLine((ru ? "  \xd0\x9f\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c \xd0\xb2\xd0\xb5\xd0\xb1-\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd0\xb8 (Enter = \xd0\xb1\xd0\xb5\xd0\xb7 \xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8f)" : "  Web panel password (Enter = none)"), "");
                if (wizardWebPassword.empty() && wizardWebHost == "0.0.0.0") {
                    std::cout << (ru ? "  !! \xd0\x9f\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c \xd0\xbe\xd1\x82\xd0\xba\xd1\x80\xd1\x8b\xd1\x82\xd0\xb0 \xd0\xb2 \xd1\x81\xd0\xb5\xd1\x82\xd1\x8c \xd0\xb1\xd0\xb5\xd0\xb7 \xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8f \xe2\x80\x94 \xd1\x8d\xd1\x82\xd0\xbe \xd0\xbf\xd0\xbe\xd0\xbb\xd0\xbd\xd1\x8b\xd0\xb9 \xd0\xb4\xd0\xbe\xd1\x81\xd1\x82\xd1\x83\xd0\xbf \xd0\xba RCON \xd0\xb4\xd0\xbb\xd1\x8f \xd0\xbb\xd1\x8e\xd0\xb1\xd0\xbe\xd0\xb3\xd0\xbe.\n" : "  !! The panel is exposed to the network without a password - that is full RCON access for anyone.\n");
                    wizardWebPassword = readLine((ru ? "  \xd0\x97\xd0\xb0\xd0\xb4\xd0\xb0\xd0\xb9\xd1\x82\xd0\xb5 \xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c \xd0\xb2\xd0\xb5\xd0\xb1-\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd0\xb8" : "  Set a web panel password"), "");
                }
                if (wizardWebPassword.empty()) {
                    std::cout << (ru ? "  ! \xd0\x91\xd0\xb5\xd0\xb7 \xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8f \xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c \xd0\xbe\xd1\x82\xd0\xba\xd1\x80\xd0\xbe\xd0\xb5\xd1\x82 \xd0\xbb\xd1\x8e\xd0\xb1\xd0\xbe\xd0\xb9, \xd1\x83 \xd0\xba\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xb5\xd1\x81\xd1\x82\xd1\x8c \xd0\xb4\xd0\xbe\xd1\x81\xd1\x82\xd1\x83\xd0\xbf \xd0\xba \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb9 \xd0\xbc\xd0\xb0\xd1\x88\xd0\xb8\xd0\xbd\xd0\xb5.\n" : "  ! Without a password anyone with access to this machine can open the panel.\n");
                }
                wizardWebSessionTtl = readInt((ru ? "  \xd0\x96\xd0\xb8\xd0\xb7\xd0\xbd\xd1\x8c \xd1\x81\xd0\xb5\xd1\x81\xd1\x81\xd0\xb8\xd0\xb8 \xd0\xbf\xd0\xbe\xd1\x81\xd0\xbb\xd0\xb5 \xd0\xb2\xd1\x85\xd0\xbe\xd0\xb4\xd0\xb0, \xd0\xbc\xd0\xb8\xd0\xbd\xd1\x83\xd1\x82 (720 = 12 \xd1\x87\xd0\xb0\xd1\x81\xd0\xbe\xd0\xb2)" : "  Session lifetime after login, minutes (720 = 12 hours)"), wizardWebSessionTtl);
                if (wizardWebSessionTtl <= 0) wizardWebSessionTtl = 720;
            }
            std::cout << "\n";
            break;
        }
        case 10: { // WIZARD_CONSOLE_V1: какая консоль открывается вместе с сервером
            printStep(10, 10, ru ? "\xd0\x9a\xd0\xbe\xd0\xbd\xd1\x81\xd0\xbe\xd0\xbb\xd1\x8c" : "Console");
            std::cout << (ru ? "    1) classic \xe2\x80\x94 \xd0\xbe\xd0\xba\xd0\xbd\xd0\xbe \xd0\xba\xd0\xbe\xd0\xbc\xd0\xb0\xd0\xbd\xd0\xb4\xd0\xbd\xd0\xbe\xd0\xb9 \xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xba\xd0\xb8 Windows\n"
                             : "    1) classic - the Windows command prompt window\n");
            std::cout << (ru ? "    2) gui     \xe2\x80\x94 \xd1\x81\xd0\xbe\xd0\xb1\xd1\x81\xd1\x82\xd0\xb2\xd0\xb5\xd0\xbd\xd0\xbd\xd0\xbe\xd0\xb5 \xd0\xbe\xd0\xba\xd0\xbd\xd0\xbe-\xd1\x82\xd0\xb5\xd1\x80\xd0\xbc\xd0\xb8\xd0\xbd\xd0\xb0\xd0\xbb Zevvoryn\n"
                             : "    2) gui     - Zevvoryn's own terminal window\n");
            // CLASSICNOW_V1: выбор применяется сразу, а не со следующего запуска.
            std::cout << (ru ? "    Сейчас ты в gui-окне: мастер всегда идёт в нём.\n"
                             : "    You are in the gui window now: the wizard always runs there.\n");
            std::cout << (ru ? "    Выберешь classic — сервер сразу переедет в терминал Windows.\n"
                             : "    Pick classic and the server moves to the Windows terminal right away.\n");
            const std::string consoleChoice = readLine(ru ? "  \xd0\x9a\xd0\xbe\xd0\xbd\xd1\x81\xd0\xbe\xd0\xbb\xd1\x8c" : "  Console",
                                                       cfg.consoleMode == "gui" ? "2" : "1");
            cfg.consoleMode = (consoleChoice == "2" || consoleChoice == "gui" || consoleChoice == "GUI") ? "gui" : "classic";
            std::cout << "\n";
            break;
        }
        default: break;
        }
        if (g_back) {
            // WIZARD_BACK_V2: с первого вопроса шага — на прошлый шаг, иначе — повтор текущего с начала
            if (g_prompts <= 1 && step > 1) --step;
            std::cout << (ru ? "  <<< \xd0\x9d\xd0\xb0\xd0\xb7\xd0\xb0\xd0\xb4\n\n" : "  <<< Back\n\n");
        } else {
            ++step;
        }
    }

    // AUTOSTARTPANEL_V1: если настроен бот или веб-панель — спросить про автозапуск вместе с exe
    if (wizardBotSetup || wizardWebEnabled) {
        std::cout << "  " << std::string(44, '-') << "\n";
        if (ru) {
            std::cout << "  \xd0\x90\xd0\xb2\xd1\x82\xd0\xbe\xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba: \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba\xd0\xb0\xd1\x82\xd1\x8c Discord-\xd0\xb1\xd0\xbe\xd1\x82\xd0\xb0/\xd0\xb2\xd0\xb5\xd0\xb1-\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd0\xb8 \xd0\xb0\xd0\xb2\xd1\x82\xd0\xbe\xd0\xbc\xd0\xb0\xd1\x82\xd0\xb8\xd1\x87\xd0\xb5\xd1\x81\xd0\xba\xd0\xb8, \xd0\xba\xd0\xbe\xd0\xb3\xd0\xb4\xd0\xb0 \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x89\xd0\xb5\xd0\xbd zevvoryn.exe (\xd0\xbe\xd1\x82\xd0\xb4\xd0\xb5\xd0\xbb\xd1\x8c\xd0\xbd\xd1\x8b\xd0\xbc \xd1\x84\xd0\xbe\xd0\xbd\xd0\xbe\xd0\xb2\xd1\x8b\xd0\xbc \xd0\xbf\xd1\x80\xd0\xbe\xd1\x86\xd0\xb5\xd1\x81\xd1\x81\xd0\xbe\xd0\xbc node.exe).\n";
        } else {
            std::cout << "  Auto-start: launch the Discord bot/web panel automatically when zevvoryn.exe starts (background node.exe process).\n";
        }
        cfg.autoStartPanel = readBool(ru ? "  \xd0\x90\xd0\xb2\xd1\x82\xd0\xbe\xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba \xd1\x81 zevvoryn.exe y/n" : "  Auto-start with zevvoryn.exe y/n", true);
        std::cout << "\n";
    }

    // ── Сохранение ──
    std::cout << "  " << std::string(44, '=') << "\n";
    std::cout << "  " << (ru ? "\xd0\xa1\xd0\xbe\xd1\x85\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5 \xd0\xba\xd0\xbe\xd0\xbd\xd1\x84\xd0\xb8\xd0\xb3\xd1\x83\xd1\x80\xd0\xb0\xd1\x86\xd0\xb8\xd0\xb8..." : "Saving configuration...") << "\n";

    cfg.saveTo("settings.properties");

    // WIZARD_WHITELIST_V1: write initial whitelist.txt entries collected in step 6
    if (!wizardWhitelistNames.empty()) {
        std::ofstream wl("whitelist.txt", std::ios::trunc);
        if (wl.is_open()) {
            wl << "# Belyj spisok igrokov (WHITELIST_V1). Odin nik v stroke.\n";
            wl << "# white-list=true v settings.properties.\n";
            for (auto& n : wizardWhitelistNames) wl << n << "\n";
            wl.close();
            std::cout << "  [OK] whitelist.txt " << (ru ? "\xd1\x81\xd0\xbe\xd0\xb7\xd0\xb4\xd0\xb0\xd0\xbd (" : "created (") << wizardWhitelistNames.size() << (ru ? " \xd0\xbd\xd0\xb8\xd0\xba\xd0\xbe\xd0\xb2)\n" : " nicknames)\n");
        }
    }

    // WIZARD_RCON_BOT_V1 / WIZARD_WEB_V1: write DiscrordBotRcon/.env
    if ((wizardBotSetup && !wizardBotToken.empty()) || wizardWebEnabled) {
        std::error_code ec;
        std::filesystem::create_directories("DiscrordBotRcon", ec);
        std::ofstream ev("DiscrordBotRcon/.env", std::ios::trunc);
        if (ev.is_open()) {
            // WIZARD_ENVDOC_V1: .env пишется с комментариями на языке, выбранном на шаге 1
            const std::string envRule(75, '=');
            ev << "# " << envRule << "\r\n";
            ev << "# " << (ru ? "\xd0\xa1\xd0\xbe\xd0\xb7\xd0\xb4\xd0\xb0\xd0\xbd\xd0\xbe \xd0\xbc\xd0\xb0\xd1\x81\xd1\x82\xd0\xb5\xd1\x80\xd0\xbe\xd0\xbc \xd1\x83\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xba\xd0\xb8 Zevvoryn. \xd0\x9c\xd0\xbe\xd0\xb6\xd0\xbd\xd0\xbe \xd0\xbf\xd1\x80\xd0\xb0\xd0\xb2\xd0\xb8\xd1\x82\xd1\x8c \xd0\xb2\xd1\x80\xd1\x83\xd1\x87\xd0\xbd\xd1\x83\xd1\x8e." : "Generated by the Zevvoryn setup wizard. Safe to edit by hand.") << "\r\n";
            ev << "# " << (ru ? "\xd0\x9d\xd0\xb5 \xd0\xbf\xd1\x83\xd0\xb1\xd0\xbb\xd0\xb8\xd0\xba\xd1\x83\xd0\xb9\xd1\x82\xd0\xb5 \xd1\x8d\xd1\x82\xd0\xbe\xd1\x82 \xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb: \xd0\xb2 \xd0\xbd\xd1\x91\xd0\xbc \xd1\x82\xd0\xbe\xd0\xba\xd0\xb5\xd0\xbd \xd0\xb1\xd0\xbe\xd1\x82\xd0\xb0 \xd0\xb8 \xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd0\xb8." : "Never publish this file: it contains the bot token and passwords.") << "\r\n";
            ev << "# " << envRule << "\r\n\r\n";
            ev << "# " << (ru ? "--- DISCORD ---" : "--- DISCORD ---") << "\r\n";
            ev << "# " << (ru ? "\xd0\xa2\xd0\xbe\xd0\xba\xd0\xb5\xd0\xbd \xd0\xb1\xd0\xbe\xd1\x82\xd0\xb0: Developer Portal -> \xd0\xb2\xd0\xb0\xd1\x88\xd0\xb5 \xd0\xbf\xd1\x80\xd0\xb8\xd0\xbb\xd0\xbe\xd0\xb6\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5 -> Bot -> Reset Token. \xd0\x9f\xd1\x83\xd1\x81\xd1\x82\xd0\xbe = \xd0\xb1\xd0\xbe\xd1\x82 \xd0\xbd\xd0\xb5 \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba\xd0\xb0\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f." : "Bot token: Developer Portal -> your application -> Bot -> Reset Token. Empty = bot disabled.") << "\r\n";
            ev << "DISCORD_TOKEN=" << wizardBotToken << "\r\n";
            ev << "# " << (ru ? "Application ID \xd0\xbf\xd1\x80\xd0\xb8\xd0\xbb\xd0\xbe\xd0\xb6\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x8f (\xd0\xbd\xd0\xb5 \xd0\xb2\xd0\xb0\xd1\x88 \xd0\xb0\xd0\xba\xd0\xba\xd0\xb0\xd1\x83\xd0\xbd\xd1\x82). \xd0\x9d\xd1\x83\xd0\xb6\xd0\xb5\xd0\xbd \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd0\xb4\xd0\xbb\xd1\x8f \xd1\x80\xd0\xb5\xd0\xb3\xd0\xb8\xd1\x81\xd1\x82\xd1\x80\xd0\xb0\xd1\x86\xd0\xb8\xd0\xb8 \xd1\x81\xd0\xbb\xd1\x8d\xd1\x88-\xd0\xba\xd0\xbe\xd0\xbc\xd0\xb0\xd0\xbd\xd0\xb4." : "Application ID of the bot app (not your account). Used only to register slash commands.") << "\r\n";
            ev << "CLIENT_ID=" << wizardBotClientId << "\r\n";
            ev << "# " << (ru ? "ID \xd0\xb2\xd0\xb0\xd1\x88\xd0\xb5\xd0\xb3\xd0\xbe Discord-\xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80\xd0\xb0 (\"guild\" = \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80). \xd0\xa0\xd0\xb5\xd0\xb6\xd0\xb8\xd0\xbc \xd1\x80\xd0\xb0\xd0\xb7\xd1\x80\xd0\xb0\xd0\xb1\xd0\xbe\xd1\x82\xd1\x87\xd0\xb8\xd0\xba\xd0\xb0 -> \xd0\x9f\xd0\x9a\xd0\x9c \xd0\xbf\xd0\xbe \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80\xd1\x83 -> \xd0\x9a\xd0\xbe\xd0\xbf\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c ID." : "Your Discord server ID (\"guild\" = server). Developer Mode -> right-click server -> Copy Server ID.") << "\r\n";
            ev << "GUILD_ID=" << wizardBotGuildId << "\r\n\r\n";
            ev << "# " << (ru ? "--- \xd0\x9f\xd0\xa0\xd0\x90\xd0\x92\xd0\x90 \xd0\x94\xd0\x9e\xd0\xa1\xd0\xa2\xd0\xa3\xd0\x9f\xd0\x90 ---" : "--- PERMISSIONS ---") << "\r\n";
            ev << "# " << (ru ? "ID \xd0\xa0\xd0\x9e\xd0\x9b\xd0\x98 (\xd0\xbd\xd0\xb5 \xd1\x87\xd0\xb5\xd0\xbb\xd0\xbe\xd0\xb2\xd0\xb5\xd0\xba\xd0\xb0), \xd0\xba\xd0\xbe\xd1\x82\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb9 \xd1\x80\xd0\xb0\xd0\xb7\xd1\x80\xd0\xb5\xd1\x88\xd0\xb5\xd0\xbd\xd1\x8b \xd0\xba\xd0\xbe\xd0\xbc\xd0\xb0\xd0\xbd\xd0\xb4\xd1\x8b. \xd0\x9f\xd1\x83\xd1\x81\xd1\x82\xd0\xbe = \xd0\xb1\xd0\xb5\xd0\xb7 \xd0\xbf\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb5\xd1\x80\xd0\xba\xd0\xb8 \xd1\x80\xd0\xbe\xd0\xbb\xd0\xb8." : "ID of the ROLE (not a user) allowed to run commands. Empty = no role check.") << "\r\n";
            ev << "ADMIN_ROLE_ID=\r\n";
            ev << "# " << (ru ? "ID \xd1\x82\xd0\xb5\xd0\xba\xd1\x81\xd1\x82\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xba\xd0\xb0\xd0\xbd\xd0\xb0\xd0\xbb\xd0\xb0 \xd0\xb4\xd0\xbb\xd1\x8f \xd0\xba\xd0\xbe\xd0\xbc\xd0\xb0\xd0\xbd\xd0\xb4. \xd0\x9f\xd1\x83\xd1\x81\xd1\x82\xd0\xbe = \xd0\xbb\xd1\x8e\xd0\xb1\xd0\xbe\xd0\xb9 \xd0\xba\xd0\xb0\xd0\xbd\xd0\xb0\xd0\xbb." : "Text channel ID where commands are accepted. Empty = any channel.") << "\r\n";
            ev << "COMMAND_CHANNEL_ID=\r\n\r\n";
            ev << "# " << (ru ? "--- RCON ---" : "--- RCON ---") << "\r\n";
            ev << "# " << (ru ? "\xd0\x90\xd0\xb4\xd1\x80\xd0\xb5\xd1\x81 \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80\xd0\xb0. \xd0\x91\xd0\xbe\xd1\x82 \xd1\x80\xd1\x8f\xd0\xb4\xd0\xbe\xd0\xbc \xd1\x81 \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80\xd0\xbe\xd0\xbc = 127.0.0.1." : "Server address. Bot on the same machine as the server = 127.0.0.1.") << "\r\n";
            ev << "RCON_HOST=127.0.0.1\r\n";
            ev << "# " << (ru ? "\xd0\x9f\xd0\xbe\xd1\x80\xd1\x82 \xd0\xb8 \xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c \xd0\xb2\xd0\xb7\xd1\x8f\xd1\x82\xd1\x8b \xd0\xb8\xd0\xb7 settings.properties (rcon.port / rcon.password) \xe2\x80\x94 \xd0\xb4\xd0\xbe\xd0\xbb\xd0\xb6\xd0\xbd\xd1\x8b \xd1\x81\xd0\xbe\xd0\xb2\xd0\xbf\xd0\xb0\xd0\xb4\xd0\xb0\xd1\x82\xd1\x8c." : "Port and password come from settings.properties (rcon.port / rcon.password) - they must match.") << "\r\n";
            ev << "RCON_PORT=" << cfg.rconPort << "\r\n";
            ev << "RCON_PASSWORD=" << cfg.rconPassword << "\r\n";
            ev << "# " << (ru ? "\xd0\xa1\xd0\xba\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd0\xb6\xd0\xb4\xd0\xb0\xd1\x82\xd1\x8c \xd0\xbe\xd1\x82\xd0\xb2\xd0\xb5\xd1\x82 RCON, \xd0\xbc\xd0\xb8\xd0\xbb\xd0\xbb\xd0\xb8\xd1\x81\xd0\xb5\xd0\xba\xd1\x83\xd0\xbd\xd0\xb4 (10000 = 10 \xd1\x81\xd0\xb5\xd0\xba)." : "How long to wait for an RCON reply, milliseconds (10000 = 10 s).") << "\r\n";
            ev << "RCON_TIMEOUT_MS=10000\r\n\r\n";
            ev << "# " << (ru ? "--- \xd0\x92\xd0\x95\xd0\x91-\xd0\x9f\xd0\x90\xd0\x9d\xd0\x95\xd0\x9b\xd0\xac ---" : "--- WEB PANEL ---") << "\r\n";
            ev << "# " << (ru ? "\xd0\x9f\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c \xd0\xbf\xd0\xbe\xd0\xba\xd0\xb0\xd0\xb7\xd1\x8b\xd0\xb2\xd0\xb0\xd0\xb5\xd1\x82: \xd0\x9f\xd0\xbe\xd0\xb4\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb5\xd0\xbd\xd0\xbe / \xd0\x98\xd0\xb3\xd1\x80\xd0\xbe\xd0\xba\xd0\xb8 \xd0\xbe\xd0\xbd\xd0\xbb\xd0\xb0\xd0\xb9\xd0\xbd / Whitelist / \xd0\x9f\xd0\xb8\xd0\xbd\xd0\xb3 WS." : "The panel shows: Connected / Players online / Whitelist / WS ping.") << "\r\n";
            ev << "# " << (ru ? "\xd0\x94\xd0\xbe\xd1\x81\xd1\x82\xd1\x83\xd0\xbf\xd0\xbd\xd0\xb0 \xd0\xbf\xd0\xbe \xd0\xb0\xd0\xb4\xd1\x80\xd0\xb5\xd1\x81\xd1\x83 http://<WEB_HOST>:<WEB_PORT>. \xd0\xa3\xd0\xb4\xd0\xb0\xd0\xbb\xd1\x91\xd0\xbd\xd0\xbd\xd0\xbe: ssh -L 3000:127.0.0.1:3000 user@host" : "Reachable at http://<WEB_HOST>:<WEB_PORT>. Tunnel option: ssh -L 3000:127.0.0.1:3000 user@host") << "\r\n";
            ev << "# " << (ru ? "\xd0\x97\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba\xd0\xb0\xd1\x82\xd1\x8c \xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c \xd0\xb2\xd0\xbc\xd0\xb5\xd1\x81\xd1\x82\xd0\xb5 \xd1\x81 \xd0\xb1\xd0\xbe\xd1\x82\xd0\xbe\xd0\xbc: true / false." : "Start the panel together with the bot: true / false.") << "\r\n";
            ev << (wizardWebEnabled ? "WEB_ENABLED=true\r\n" : "WEB_ENABLED=false\r\n");
            ev << (ru ? "# \xd0\x90\xd0\xb4\xd1\x80\xd0\xb5\xd1\x81, \xd0\xbd\xd0\xb0 \xd0\xba\xd0\xbe\xd1\x82\xd0\xbe\xd1\x80\xd0\xbe\xd0\xbc \xd1\x81\xd0\xbb\xd1\x83\xd1\x88\xd0\xb0\xd0\xb5\xd1\x82 \xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c: 127.0.0.1 = \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x8d\xd1\x82\xd0\xb0 \xd0\xbc\xd0\xb0\xd1\x88\xd0\xb8\xd0\xbd\xd0\xb0, 0.0.0.0 = \xd0\xbb\xd1\x8e\xd0\xb1\xd0\xbe\xd0\xb9 IP.\r\n" : "# Address the panel listens on: 127.0.0.1 = this machine only, 0.0.0.0 = any IP.\r\n");
            ev << "WEB_HOST=" << wizardWebHost << "\r\n";
            ev << (ru ? "# \xd0\x9f\xd0\xbe\xd1\x80\xd1\x82 \xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd0\xb8: \xd0\xbe\xd1\x82\xd0\xba\xd1\x80\xd1\x8b\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c http://<\xd0\xb0\xd0\xb4\xd1\x80\xd0\xb5\xd1\x81>:<\xd0\xbf\xd0\xbe\xd1\x80\xd1\x82>\r\n" : "# Panel port: open http://<host>:<port>\r\n");
            ev << "WEB_PORT=" << wizardWebPort << "\r\n";
            ev << "# " << (ru ? "\xd0\x9f\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c \xd0\xb4\xd0\xbb\xd1\x8f \xd0\xb2\xd1\x85\xd0\xbe\xd0\xb4\xd0\xb0 \xd0\xb2 \xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c. \xd0\x9f\xd1\x83\xd1\x81\xd1\x82\xd0\xbe = \xd0\xb1\xd0\xb5\xd0\xb7 \xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8f (\xd0\xbd\xd0\xb5 \xd1\x80\xd0\xb5\xd0\xba\xd0\xbe\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb4\xd1\x83\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f)." : "Login password for the panel. Empty = no password (not recommended).") << "\r\n";
            ev << "WEB_PASSWORD=" << wizardWebPassword << "\r\n";
            ev << "# " << (ru ? "\xd0\x96\xd0\xb8\xd0\xb7\xd0\xbd\xd1\x8c \xd1\x81\xd0\xb5\xd1\x81\xd1\x81\xd0\xb8\xd0\xb8 \xd0\xbf\xd0\xbe\xd1\x81\xd0\xbb\xd0\xb5 \xd0\xb2\xd1\x85\xd0\xbe\xd0\xb4\xd0\xb0, \xd0\xbc\xd0\xb8\xd0\xbd\xd1\x83\xd1\x82 (720 = 12 \xd1\x87\xd0\xb0\xd1\x81\xd0\xbe\xd0\xb2). \xd0\xa0\xd0\xb5\xd1\x81\xd1\x82\xd0\xb0\xd1\x80\xd1\x82 \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80\xd0\xb0 \xd1\x80\xd0\xb0\xd0\xb7\xd0\xbb\xd0\xbe\xd0\xb3\xd0\xb8\xd0\xbd\xd0\xb8\xd0\xb2\xd0\xb0\xd0\xb5\xd1\x82 \xd0\xb2\xd1\x81\xd0\xb5\xd1\x85." : "Session lifetime after login, minutes (720 = 12 hours). A server restart logs everyone out.") << "\r\n";
            ev << "WEB_SESSION_TTL_MIN=" << wizardWebSessionTtl << "\r\n";
            ev << (ru ? "# \xd0\xaf\xd0\xb7\xd1\x8b\xd0\xba \xd0\xb8\xd0\xbd\xd1\x82\xd0\xb5\xd1\x80\xd1\x84\xd0\xb5\xd0\xb9\xd1\x81\xd0\xb0 \xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd0\xb8 \xd0\xbf\xd0\xbe \xd1\x83\xd0\xbc\xd0\xbe\xd0\xbb\xd1\x87\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x8e: ru \xd0\xb8\xd0\xbb\xd0\xb8 en (\xd0\xb2 \xd0\xb1\xd1\x80\xd0\xb0\xd1\x83\xd0\xb7\xd0\xb5\xd1\x80\xd0\xb5 \xd0\xb5\xd1\x81\xd1\x82\xd1\x8c \xd0\xbf\xd0\xb5\xd1\x80\xd0\xb5\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb0\xd1\x82\xd0\xb5\xd0\xbb\xd1\x8c).\r\n" : "# Default panel UI language: ru or en (there is a switch in the browser).\r\n");
            ev << "WEB_LANG=" << (ru ? "ru" : "en") << "\r\n";
            ev.close();
            std::cout << "  [OK] DiscrordBotRcon/.env " << (ru ? "\xd1\x81\xd0\xbe\xd0\xb7\xd0\xb4\xd0\xb0\xd0\xbd" : "created") << "\n";
            if (wizardBotSetup && !wizardBotToken.empty()) {
                std::cout << (ru ? "  \xd0\x91\xd0\xbe\xd1\x82: cd DiscrordBotRcon && npm install && npm run deploy && npm start\n"
                                 : "  Bot: cd DiscrordBotRcon && npm install && npm run deploy && npm start\n");
            }
            if (wizardWebEnabled) {
                std::cout << (ru ? "  \xd0\x92\xd0\xb5\xd0\xb1: cd DiscrordBotRcon && npm install && npm start (\xd0\xb2\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb5\xd0\xbd\xd0\xb0 \xd0\xbf\xd0\xbe \xd1\x83\xd0\xbc\xd0\xbe\xd0\xbb\xd1\x87\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x8e)\n"
                                 : "  Web: cd DiscrordBotRcon && npm install && npm start (auto-starts by default)\n");
                std::cout << "  URL: http://" << (wizardWebHost == "0.0.0.0" ? std::string("127.0.0.1") : wizardWebHost)
                          << ":" << wizardWebPort
                          << (wizardWebHost == "0.0.0.0" ? (ru ? "  (\xd0\xb0 \xd1\x82\xd0\xb0\xd0\xba\xd0\xb6\xd0\xb5 \xd0\xbf\xd0\xbe IP \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb9 \xd0\xbc\xd0\xb0\xd1\x88\xd0\xb8\xd0\xbd\xd1\x8b \xd0\xb8\xd0\xb7 \xd1\x81\xd0\xb5\xd1\x82\xd0\xb8)" : "  (and from the network by this machine IP)") : "") << "\n";
            }
        }
    }
    std::cout << "  [OK] settings.properties " << (ru ? "\xd1\x81\xd0\xbe\xd0\xb7\xd0\xb4\xd0\xb0\xd0\xbd" : "created") << "\n";
    std::cout << "\n";

    // ── Итог ──
    std::cout << "\033[36m\033[1m";
    int w = 58;
    std::cout << boxLine(w) << "\n";
    std::cout << boxRow(ru ? "\xd0\xa3\xd0\xa1\xd0\xa2\xd0\x90\xd0\x9d\xd0\x9e\xd0\x92\xd0\x9a\xd0\x90 \xd0\x97\xd0\x90\xd0\x92\xd0\x95\xd0\xa0\xd0\xa8\xd0\x95\xd0\x9d\xd0\x90!" : "SETUP COMPLETE!", w) << "\n";
    std::cout << boxBottom(w) << "\n";
    std::cout << "\033[0m";
    std::cout << "  MOTD:       " << cfg.motd << "\n";
    std::cout << "  " << (ru ? "\xd0\x9f\xd0\xbe\xd1\x80\xd1\x82:" : "Port:     ") << "     " << cfg.port << "\n";
    std::cout << "  " << (ru ? "\xd0\x9c\xd0\xb8\xd1\x80:" : "World:    ") << "    " << cfg.generator << " (seed: " << cfg.levelSeed << ")\n";
    std::cout << "  " << (ru ? "\xd0\xa0\xd0\xb5\xd0\xb6\xd0\xb8\xd0\xbc:" : "Gamemode: ") << " " << cfg.gamemode << "\n";
    std::cout << "  " << (ru ? "\xd0\x98\xd0\xb3\xd1\x80\xd0\xbe\xd0\xba\xd0\xbe\xd0\xb2:" : "Players:  ") << " " << cfg.maxPlayers << (ru ? " \xd0\xbc\xd0\xb0\xd0\xba\xd1\x81." : " max") << "\n";
    std::cout << "  " << (ru ? "\xd0\xa1\xd0\xbb\xd0\xbe\xd0\xb6\xd0\xbd\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c:" : "Difficulty: ") << " " << cfg.difficulty << "\n";
    std::cout << "\n";
    std::cout << "  " << (ru ? "\xd0\x9d\xd0\xb0\xd0\xb6\xd0\xbc\xd0\xb8\xd1\x82\xd0\xb5 Enter \xd0\xb4\xd0\xbb\xd1\x8f \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba\xd0\xb0..." : "Press Enter to start the server...") << "\n";
    std::cin.get();

    return cfg;
}

inline bool needsSetup(const std::filesystem::path& configPath) {
    return !std::filesystem::exists(configPath);
}

} // namespace setup
} // namespace nc
