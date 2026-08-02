#pragma once

#include "types.hpp"
#include "log.hpp"
#include <fstream>
#include <filesystem>
#include <iostream>
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
inline constexpr const char* NC_VERSION = "0.1.0";
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
    bool pvp                    = true;
    i32 difficulty              = 2;
    bool showCoordinates        = true;
    i32 spawnProtection         = 16;
    std::string ops             = ""; // OPS_V1
    bool allowFlight            = false;

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

    // ── Логирование ──
    std::string logLevel        = "INFO";

    // —— RCON (RCON_V1) ——
    bool enableRcon          = false;
    i32  rconPort            = 25575;
    std::string rconPassword;
    i32  rconMaxClients      = 4;
    bool rconLogCommands     = false; // RCONQUIET_V1: панель опрашивает сервер постоянно — по умолчанию молчим

    // AUTOSTARTPANEL_V1: автозапуск Discord-бота/веб-панели (DiscordBotRcon) вместе с zevvoryn.exe
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
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
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
        else if (lk == "pvp")                   pvp = toBool(val);
        else if (lk == "difficulty")            { difficulty = toInt(val, difficulty); if (difficulty < 0) difficulty = 0; if (difficulty > 3) difficulty = 3; }
        else if (lk == "show-coordinates")      showCoordinates = toBool(val);
        else if (lk == "spawn-protection")      spawnProtection = toInt(val, spawnProtection);
        else if (lk == "ops")                   ops = toLower(val); // OPS_V1
        else if (lk == "allow-flight")          allowFlight = toBool(val);
        else if (lk == "xbox-auth")             xboxAuth = toBool(val);
        else if (lk == "white-list")            whiteList = toBool(val);
        else if (lk == "server-ip")             serverIp = val;
        else if (lk == "enable-ipv6")           enableIpv6 = toBool(val);
        else if (lk == "max-packet-size")       maxPacketSize = toInt(val, maxPacketSize);
        else if (lk == "compression-threshold") compressionThreshold = toInt(val, compressionThreshold);
        else if (lk == "auto-save")             autoSave = toBool(val);
        else if (lk == "auto-save-interval")    autoSaveInterval = toInt(val, autoSaveInterval);
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
        std::transform(s.begin(), s.end(), s.begin(), ::toupper);
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
    void saveTo(const std::filesystem::path& path) const {
        bool ru = (language == "rus");

        std::ofstream out(path);
        out << "# ============================================\n";
        out << (ru ? "# \xd0\x9a\xd0\xbe\xd0\xbd\xd1\x84\xd0\xb8\xd0\xb3\xd1\x83\xd1\x80\xd0\xb0\xd1\x86\xd0\xb8\xd1\x8f \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80\xd0\xb0 Zevvoryn\n" : "# Zevvoryn Server Configuration\n");
        out << (ru ? "# \xd0\xa1\xd0\xbe\xd0\xb7\xd0\xb4\xd0\xb0\xd0\xbd\xd0\xbe \xd0\xbc\xd0\xb0\xd1\x81\xd1\x82\xd0\xb5\xd1\x80\xd0\xbe\xd0\xbc \xd1\x83\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xba\xd0\xb8\n" : "# Generated by Setup Wizard\n");
        out << "# ============================================\n";
        out << "\n";

        out << (ru ? "# \xd0\xaf\xd0\xb7\xd1\x8b\xd0\xba \xd0\xb2\xd1\x8b\xd0\xb2\xd0\xbe\xd0\xb4\xd0\xb0 \xd0\xb2 \xd0\xba\xd0\xbe\xd0\xbd\xd1\x81\xd0\xbe\xd0\xbb\xd0\xb8\n" : "# Language for server console output\n");
        out << (ru ? "# \xd0\x92\xd0\xb0\xd1\x80\xd0\xb8\xd0\xb0\xd0\xbd\xd1\x82\xd1\x8b: eng, rus\n" : "# Options: eng, rus\n");
        out << "language=" << language << "\n";
        out << "\n";

        out << (ru ? "# === \xd0\xa1\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80 ===\n" : "# === Server ===\n");
        out << (ru ? "# MOTD \xe2\x80\x94 \xd1\x82\xd0\xb5\xd0\xba\xd1\x81\xd1\x82 \xd0\xbe\xd1\x82\xd0\xbe\xd0\xb1\xd1\x80\xd0\xb0\xd0\xb6\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x8f \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80\xd0\xb0 \xd0\xb2 \xd1\x81\xd0\xbf\xd0\xb8\xd1\x81\xd0\xba\xd0\xb5 \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80\xd0\xbe\xd0\xb2 (Message Of The Day)\n" : "# MOTD \xe2\x80\x94 server name shown in the Minecraft server list (Message Of The Day)\n");
        out << "motd=" << motd << "\n";
        out << "brand=" << brand << "\n"; // BRAND_V1
        out << (ru ? "# \xd0\x92\xd1\x82\xd0\xbe\xd1\x80\xd0\xb0\xd1\x8f \xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xba\xd0\xb0 MOTD (\xd0\xbe\xd0\xbf\xd1\x86\xd0\xb8\xd0\xbe\xd0\xbd\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe)\n" : "# Second line of the MOTD (optional)\n");
        out << "sub-motd=" << subMotd << "\n";
        out << (ru ? "# \xd0\x9f\xd0\xbe\xd1\x80\xd1\x82 \xd0\xb4\xd0\xbb\xd1\x8f \xd0\xbf\xd0\xbe\xd0\xb4\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x8f Java-\xd0\xba\xd0\xbb\xd0\xb8\xd0\xb5\xd0\xbd\xd1\x82\xd0\xbe\xd0\xb2 (TCP)\n" : "# Listening port for Java clients (TCP)\n");
        out << "server-port=" << port << "\n";
        if (portV6 > 0) {
            out << (ru ? "# \xd0\x9f\xd0\xbe\xd1\x80\xd1\x82 \xd0\xb4\xd0\xbb\xd1\x8f IPv6 (\xd0\xbe\xd0\xbf\xd1\x86\xd0\xb8\xd0\xbe\xd0\xbd\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe)\n" : "# IPv6 listening port (optional)\n");
            out << "server-portv6=" << portV6 << "\n";
        }
        out << (ru ? "# \xd0\x9c\xd0\xb0\xd0\xba\xd1\x81\xd0\xb8\xd0\xbc\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe\xd0\xb5 \xd0\xba\xd0\xbe\xd0\xbb\xd0\xb8\xd1\x87\xd0\xb5\xd1\x81\xd1\x82\xd0\xb2\xd0\xbe \xd0\xb8\xd0\xb3\xd1\x80\xd0\xbe\xd0\xba\xd0\xbe\xd0\xb2 \xd0\xbd\xd0\xb0 \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80\xd0\xb5\n" : "# Maximum number of players allowed on the server\n");
        out << "max-players=" << maxPlayers << "\n";
        out << (ru ? "# \xd0\x94\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c \xd0\xbf\xd1\x80\xd0\xbe\xd1\x80\xd0\xb8\xd1\x81\xd0\xbe\xd0\xb2\xd0\xba\xd0\xb8 (\xd0\xb2 \xd1\x87\xd0\xb0\xd0\xbd\xd0\xba\xd0\xb0\xd1\x85)\n" : "# View distance in chunks\n");
        out << "view-distance=" << viewDistance << "\n";
        out << (ru ? "# \xd0\x94\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c \xd1\x81\xd0\xb8\xd0\xbc\xd1\x83\xd0\xbb\xd1\x8f\xd1\x86\xd0\xb8\xd0\xb8 \xd1\x8d\xd0\xbd\xd1\x82\xd0\xb8\xd1\x82\xd0\xb8 (\xd0\xbd\xd0\xb8\xd0\xb6\xd0\xb5 = \xd0\xbc\xd0\xb5\xd0\xbd\xd1\x8c\xd1\x88\xd0\xb5 CPU)\n" : "# Simulation distance (lower = less CPU)\n");
        out << "simulation-distance=" << simulationDistance << "\n";
        out << (ru ? "\x23\x20\xd0\x9b\xd0\xb8\xd0\xbc\xd0\xb8\xd1\x82\x20\xd0\x9e\xd0\x97\xd0\xa3\x20\xd0\xb2\x20\xd0\x93\xd0\x91\x20\x28\x30\x20\x3d\x20\xd0\xb1\xd0\xb5\xd0\xb7\x20\xd0\xbb\xd0\xb8\xd0\xbc\xd0\xb8\xd1\x82\xd0\xb0\x20\x2f\x20\xd0\xb0\xd0\xb2\xd1\x82\xd0\xbe\x29\n" : "# RAM limit in GB (0 = no limit / auto)\n");
        out << "max-ram-gb=" << maxRamGb << "\n";
        out << (ru ? "\x23\x20\xd0\xaf\xd0\xb4\xd1\x80\xd0\xb0\x20\x43\x50\x55\x20\xd0\xb4\xd0\xbb\xd1\x8f\x20\xd0\xb3\xd0\xb5\xd0\xbd\xd0\xb5\xd1\x80\xd0\xb0\xd1\x86\xd0\xb8\xd0\xb8\x20\xd0\xbc\xd0\xb8\xd1\x80\xd0\xb0\x20\x28\x30\x20\x3d\x20\xd0\xb0\xd0\xb2\xd1\x82\xd0\xbe\x29\n" : "# CPU cores for world generation (0 = auto)\n");
        out << "max-cores=" << maxCores << "\n";
        out << "\n";

        out << (ru ? "# === \xd0\x9c\xd0\xb8\xd1\x80 ===\n" : "# === World ===\n");
        out << (ru ? "# \xd0\x98\xd0\xbc\xd1\x8f \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb8 \xd1\x81 \xd0\xbc\xd0\xb8\xd1\x80\xd0\xbe\xd0\xbc\n" : "# World folder name\n");
        out << "level-name=" << levelName << "\n";
        out << (ru ? "# \xd0\x93\xd0\xb5\xd0\xbd\xd0\xb5\xd1\x80\xd0\xb0\xd1\x82\xd0\xbe\xd1\x80: DEFAULT (\xd0\xbe\xd0\xb1\xd1\x8b\xd1\x87\xd0\xbd\xd1\x8b\xd0\xb9), FLAT (\xd0\xbf\xd0\xbb\xd0\xbe\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9), VOID (\xd0\xbf\xd1\x83\xd1\x81\xd1\x82\xd0\xbe\xd0\xb9)\n" : "# Generator: DEFAULT (normal), FLAT (superflat), VOID (empty)\n");
        out << "generator=" << generator << "\n";
        out << (ru ? "# \xd0\xa1\xd0\xb8\xd0\xb4 \xd0\xbc\xd0\xb8\xd1\x80\xd0\xb0 (0 = \xd1\x81\xd0\xbb\xd1\x83\xd1\x87\xd0\xb0\xd0\xb9\xd0\xbd\xd0\xbe\xd0\xb5)\n" : "# World seed (0 = random)\n");
        out << "level-seed=" << levelSeed << "\n";
        // DIMTOGGLE_V1
        out << (ru ? "# Создавать мир Ада (Nether). false = измерения нет, порталы не зажигаются\n"
                   : "# Generate the Nether. false = no such dimension, portals stay dead\n");
        out << "enable-nether=" << boolToStr(enableNether) << "\n";
        out << (ru ? "# Создавать мир Энда (End). false = измерения нет, око Эндера не сработает\n"
                   : "# Generate the End. false = no such dimension, the eye of ender does nothing\n");
        out << "enable-end=" << boolToStr(enableEnd) << "\n";
        out << "\n";

        out << (ru ? "# === \xd0\x93\xd0\xb5\xd0\xb9\xd0\xbc\xd0\xbf\xd0\xbb\xd0\xb5\xd0\xb9 ===\n" : "# === Gameplay ===\n");
        out << (ru ? "# \xd0\xa0\xd0\xb5\xd0\xb6\xd0\xb8\xd0\xbc \xd0\xbf\xd0\xbe \xd1\x83\xd0\xbc\xd0\xbe\xd0\xbb\xd1\x87\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x8e: survival, creative, adventure, spectator\n" : "# Default gamemode: survival, creative, adventure, spectator\n");
        out << "gamemode=" << gamemode << "\n";
        out << (ru ? "# \xd0\x9f\xd1\x80\xd0\xb8\xd0\xbd\xd1\x83\xd0\xb4\xd0\xb8\xd1\x82\xd0\xb5\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x80\xd0\xb5\xd1\x82\xd0\xb8\xd1\x82\xd1\x8c \xd1\x80\xd0\xb5\xd0\xb6\xd0\xb8\xd0\xbc \xd0\xb2\xd1\x81\xd0\xb5\xd0\xbc \xd0\xb8\xd0\xb3\xd1\x80\xd0\xbe\xd0\xba\xd0\xb0\xd0\xbc (\xd0\xb8\xd0\xb3\xd0\xbd\xd0\xbe\xd1\x80\xd0\xb8\xd1\x80\xd1\x83\xd0\xb5\xd1\x82 \xd0\xb2\xd1\x8b\xd0\xb1\xd0\xbe\xd1\x80 \xd0\xb8\xd0\xb3\xd1\x80\xd0\xbe\xd0\xba\xd0\xb0)\n" : "# Force gamemode on all players (overrides individual choice)\n");
        out << "force-gamemode=" << boolToStr(forceGamemode) << "\n";
        out << (ru ? "# \xd0\xa0\xd0\xb0\xd0\xb7\xd1\x80\xd0\xb5\xd1\x88\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5 \xd0\xbf\xd0\xbe \xd0\xb1\xd0\xb8\xd1\x82\xd1\x8c\xd0\xb5 \xd0\xbc\xd0\xb5\xd0\xb6\xd0\xb4\xd1\x83 \xd0\xb8\xd0\xb3\xd1\x80\xd0\xbe\xd0\xba\xd0\xb0\xd0\xbc\xd0\xb8\n" : "# Allow players to fight each other\n");
        out << "pvp=" << boolToStr(pvp) << "\n";
        out << (ru ? "# \xd0\xa1\xd0\xbb\xd0\xbe\xd0\xb6\xd0\xbd\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c: 0=\xd0\x9c\xd0\xb8\xd1\x80\xd0\xbd\xd1\x8b\xd0\xb9, 1=\xd0\x9b\xd0\xb5\xd0\xb3\xd0\xba\xd0\xbe, 2=\xd0\x9d\xd0\xbe\xd1\x80\xd0\xbc\xd0\xb0, 3=\xd0\xa1\xd0\xbb\xd0\xbe\xd0\xb6\xd0\xbd\xd0\xbe\n" : "# Difficulty: 0=Peaceful, 1=Easy, 2=Normal, 3=Hard\n");
        out << "difficulty=" << difficulty << "\n";
        out << (ru ? "# \xd0\x9f\xd0\xbe\xd0\xba\xd0\xb0\xd0\xb7\xd1\x8b\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c \xd0\xba\xd0\xbe\xd0\xbe\xd1\x80\xd0\xb4\xd0\xb8\xd0\xbd\xd0\xb0\xd1\x82\xd1\x8b XYZ \xd0\xb2 \xd0\xb8\xd0\xb3\xd1\x80\xd0\xb5\n" : "# Show XYZ coordinates in game\n");
        out << "show-coordinates=" << boolToStr(showCoordinates) << "\n";
        out << (ru ? "# \xd0\xa0\xd0\xb0\xd0\xb4\xd0\xb8\xd1\x83\xd1\x81 \xd0\xb7\xd0\xb0\xd1\x89\xd0\xb8\xd1\x82\xd1\x8b \xd1\x81\xd0\xbf\xd0\xb0\xd0\xb2\xd0\xbd\xd0\xb0 (\xd0\xb2 \xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba\xd0\xb0\xd1\x85, 0=\xd0\xb2\xd1\x8b\xd0\xba\xd0\xbb)\n" : "# Spawn protection radius (in blocks, 0=off)\n");
        out << "spawn-protection=" << spawnProtection << "\n";
        out << (ru ? "# \xd0\x9e\xd0\xbf\xd0\xb5\xd1\x80\xd0\xb0\xd1\x82\xd0\xbe\xd1\x80\xd1\x8b (\xd1\x87\xd0\xb5\xd1\x80\xd0\xb5\xd0\xb7 \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x8f\xd1\x82\xd1\x83\xd1\x8e): \xd0\xbe\xd0\xb1\xd1\x85\xd0\xbe\xd0\xb4\xd1\x8f\xd1\x82 \xd0\xb7\xd0\xb0\xd1\x89\xd0\xb8\xd1\x82\xd1\x83 \xd1\x81\xd0\xbf\xd0\xb0\xd0\xb2\xd0\xbd\xd0\xb0, \xd0\xbc\xd0\xbe\xd0\xb3\xd1\x83\xd1\x82 /tp /gamemode /time \xd0\xb8 \xd1\x82.\xd0\xb4.\n" : "# Server operators (comma-separated): bypass spawn protection, can use admin commands\n");
        out << "ops=" << ops << "\n";
        out << (ru ? "# \xd0\xa0\xd0\xb0\xd0\xb7\xd1\x80\xd0\xb5\xd1\x88\xd0\xb8\xd1\x82\xd1\x8c \xd0\xbf\xd0\xbe\xd0\xbb\xd1\x91\xd1\x82 \xd0\xb2 \xd1\x80\xd0\xb5\xd0\xb6\xd0\xb8\xd0\xbc\xd0\xb5 \xd0\xb2\xd1\x8b\xd0\xb6\xd0\xb8\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x8f\n" : "# Allow flying in survival mode\n");
        out << "allow-flight=" << boolToStr(allowFlight) << "\n";
        out << "\n";

        out << (ru ? "# === \xd0\x90\xd1\x83\xd1\x82\xd0\xb5\xd0\xbd\xd1\x82\xd0\xb8\xd1\x84\xd0\xb8\xd0\xba\xd0\xb0\xd1\x86\xd0\xb8\xd1\x8f ===\n" : "# === Authentication ===\n");
        out << (ru ? "# \xd0\xa2\xd1\x80\xd0\xb5\xd0\xb1\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c \xd0\xb0\xd1\x83\xd1\x82\xd0\xb5\xd0\xbd\xd1\x82\xd0\xb8\xd1\x84\xd0\xb8\xd0\xba\xd0\xb0\xd1\x86\xd0\xb8\xd1\x8e \xd0\xbe\xd1\x82 Microsoft/Xbox (\xd0\xb2\xd1\x8b\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb8\xd1\x82\xd1\x8c \xd0\xb4\xd0\xbb\xd1\x8f \xd0\xbf\xd0\xb8\xd1\x80\xd0\xb0\xd1\x82\xd0\xbe\xd0\xba\xd0\xb8\xd1\x85 \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80\xd0\xbe\xd0\xb2)\n" : "# Require Microsoft/Xbox authentication (disable for cracked servers)\n");
        out << "xbox-auth=" << boolToStr(xboxAuth) << "\n";
        out << (ru ? "# \xd0\xa2\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd0\xb4\xd0\xbb\xd1\x8f \xd1\x80\xd0\xb0\xd0\xb7\xd1\x80\xd0\xb5\xd1\x88\xd1\x91\xd0\xbd\xd0\xbd\xd1\x8b\xd1\x85 \xd0\xb8\xd0\xb3\xd1\x80\xd0\xbe\xd0\xba\xd0\xbe\xd0\xb2 (\xd0\xb2\xd0\xb0\xd0\xb9\xd1\x82)\n" : "# Only allow whitelisted players (whitelist)\n");
        out << "white-list=" << boolToStr(whiteList) << "\n";
        out << "\n";

        out << (ru ? "# === \xd0\xa1\xd0\xb5\xd1\x82\xd1\x8c ===\n" : "# === Network ===\n");
        out << (ru ? "# \xd0\x90\xd0\xb4\xd1\x80\xd0\xb5\xd1\x81 \xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd1\x8f\xd0\xb7\xd0\xba\xd0\xb8 (\xd0\xbe\xd1\x81\xd1\x82\xd0\xb0\xd0\xb2\xd1\x8c\xd1\x82\xd0\xb5 \xd0\xbf\xd1\x83\xd1\x81\xd1\x82\xd1\x8b\xd0\xbc \xd0\xb4\xd0\xbb\xd1\x8f \xd0\xb2\xd1\x81\xd0\xb5\xd1\x85 \xd0\xb8\xd0\xbd\xd1\x82\xd0\xb5\xd1\x80\xd1\x84\xd0\xb5\xd0\xb9\xd1\x81\xd0\xbe\xd0\xb2)\n" : "# Bind address (leave empty for all interfaces)\n");
        out << "server-ip=" << serverIp << "\n";
        out << (ru ? "# \xd0\x92\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb8\xd1\x82\xd1\x8c \xd0\xbf\xd0\xbe\xd0\xb4\xd0\xb4\xd0\xb5\xd1\x80\xd0\xb6\xd0\xba\xd1\x83 IPv6\n" : "# Enable IPv6 support\n");
        out << "enable-ipv6=" << boolToStr(enableIpv6) << "\n";
        out << (ru ? "# \xd0\x9c\xd0\xb0\xd0\xba\xd1\x81\xd0\xb8\xd0\xbc\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd1\x8b\xd0\xb9 \xd1\x80\xd0\xb0\xd0\xb7\xd0\xbc\xd0\xb5\xd1\x80 \xd0\xbf\xd0\xb0\xd0\xba\xd0\xb5\xd1\x82\xd0\xb0 (\xd0\xb2 \xd0\xb1\xd0\xb0\xd0\xb9\xd1\x82\xd0\xb0\xd1\x85)\n" : "# Maximum packet size (in bytes)\n");
        out << "max-packet-size=" << maxPacketSize << "\n";
        out << (ru ? "# \xd0\xa1\xd0\xb6\xd0\xb8\xd0\xbc\xd0\xb0\xd1\x82\xd1\x8c \xd0\xbf\xd0\xb0\xd0\xba\xd0\xb5\xd1\x82\xd1\x8b \xd0\xb1\xd0\xbe\xd0\xbb\xd1\x8c\xd1\x88\xd0\xb5 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb3\xd0\xbe \xd1\x80\xd0\xb0\xd0\xb7\xd0\xbc\xd0\xb5\xd1\x80\xd0\xb0 (\xd0\xb2 \xd0\xb1\xd0\xb0\xd0\xb9\xd1\x82\xd0\xb0\xd1\x85)\n" : "# Compress packets larger than this size (in bytes)\n");
        out << "compression-threshold=" << compressionThreshold << "\n";
        out << "\n";

        out << (ru ? "# === \xd0\x90\xd0\xb2\xd1\x82\xd0\xbe\xd1\x81\xd0\xbe\xd1\x85\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5 ===\n" : "# === Auto-Save ===\n");
        out << (ru ? "# \xd0\x90\xd0\xb2\xd1\x82\xd0\xbe\xd0\xbc\xd0\xb0\xd1\x82\xd0\xb8\xd1\x87\xd0\xb5\xd1\x81\xd0\xba\xd0\xb8 \xd1\x81\xd0\xbe\xd1\x85\xd1\x80\xd0\xb0\xd0\xbd\xd1\x8f\xd1\x82\xd1\x8c \xd0\xbc\xd0\xb8\xd1\x80 \xd0\xbf\xd0\xbe \xd1\x80\xd0\xb0\xd1\x81\xd0\xbf\xd0\xb8\xd1\x81\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x8e\n" : "# Automatically save the world periodically\n");
        out << "auto-save=" << boolToStr(autoSave) << "\n";
        out << (ru ? "# \xd0\x98\xd0\xbd\xd1\x82\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb0\xd0\xbb \xd1\x81\xd0\xbe\xd1\x85\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x8f (\xd0\xb2 \xd1\x81\xd0\xb5\xd0\xba\xd1\x83\xd0\xbd\xd0\xb4\xd0\xb0\xd1\x85, \xd0\xbc\xd0\xb8\xd0\xbd\xd0\xb8\xd0\xbc\xd1\x83\xd0\xbc 30)\n" : "# Save interval in seconds (minimum 30)\n");
        out << "auto-save-interval=" << autoSaveInterval << "\n";
        out << "\n";

        out << (ru ? "# === \xd0\x9b\xd0\xbe\xd0\xb3\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xb8\xd0\xb5 ===\n" : "# === Logging ===\n");
        out << (ru ? "# \xd0\xa3\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb5\xd0\xbd\xd1\x8c \xd0\xbb\xd0\xbe\xd0\xb3\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2: TRACE, DEBUG, INFO, WARN, ERROR\n" : "# Console log level: TRACE, DEBUG, INFO, WARN, ERROR\n");
        out << "log-level=" << logLevel << "\n";
        out << "\n";
        // RCON_V1
        out << "# === RCON (nuzhen dlya Discord-bota i veb-paneli) ===\n";
        out << "enable-rcon=" << (enableRcon ? "true" : "false") << "\n";
        out << "rcon.port=" << rconPort << "\n";
        out << "rcon.password=" << rconPassword << "\n";
        out << "rcon.max-clients=" << rconMaxClients << "\n";
        out << "rcon.log-commands=" << (rconLogCommands ? "true" : "false") << "\n";
        // AUTOSTARTPANEL_V1
        out << (ru ? "# \xd0\x90\xd0\xb2\xd1\x82\xd0\xbe\xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba Discord-\xd0\xb1\xd0\xbe\xd1\x82\xd0\xb0/\xd0\xb2\xd0\xb5\xd0\xb1-\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd0\xb8 \xd0\xb2\xd0\xbc\xd0\xb5\xd1\x81\xd1\x82\xd0\xb5 \xd1\x81 \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80\xd0\xbe\xd0\xbc (Windows: node.exe \xd0\xb2 \xd1\x84\xd0\xbe\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xbc \xd0\xbe\xd0\xba\xd0\xbd\xd0\xb5)\n" : "# Auto-start Discord bot/web panel together with the server (Windows: background node.exe)\n");
        out << "auto-start-panel=" << (autoStartPanel ? "true" : "false") << "\n";
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

inline std::string readLine(const std::string& prompt, const std::string& defaultVal) {
    if (g_back) return defaultVal; // WIZARD_BACK_V1
    ++g_prompts;
    std::cout << prompt << " [" << defaultVal << "]: ";
    std::string input;
    std::getline(std::cin, input);
    if (isBackInput(input)) { g_back = true; return defaultVal; }
    const std::string val = input.empty() ? defaultVal : input;
    nc::log::rawLine(prompt + " [" + defaultVal + "]: " + val); // LOGBANNER_V1: ответы мастера в лог
    return val;
}

inline i32 readInt(const std::string& prompt, i32 defaultVal) {
    if (g_back) return defaultVal; // WIZARD_BACK_V1
    ++g_prompts;
    std::cout << prompt << " [" << defaultVal << "]: ";
    std::string input;
    std::getline(std::cin, input);
    if (isBackInput(input)) { g_back = true; return defaultVal; }
    i32 val = defaultVal;
    if (!input.empty()) { try { val = static_cast<i32>(std::stol(input)); } catch (...) {} }
    nc::log::rawLine(prompt + " [" + std::to_string(defaultVal) + "]: " + std::to_string(val)); // LOGBANNER_V1
    return val;
}

inline i64 readInt64(const std::string& prompt, i64 defaultVal) {
    if (g_back) return defaultVal; // WIZARD_BACK_V1
    ++g_prompts;
    std::cout << prompt << " [" << defaultVal << "]: ";
    std::string input;
    std::getline(std::cin, input);
    if (isBackInput(input)) { g_back = true; return defaultVal; }
    i64 val = defaultVal;
    if (!input.empty()) { try { val = std::stoll(input); } catch (...) {} }
    nc::log::rawLine(prompt + " [" + std::to_string(defaultVal) + "]: " + std::to_string(val)); // LOGBANNER_V1
    return val;
}

inline bool readBool(const std::string& prompt, bool defaultVal) {
    if (g_back) return defaultVal; // WIZARD_BACK_V1
    ++g_prompts;
    std::string def = defaultVal ? "y" : "n";
    std::cout << prompt << " [" << def << "]: ";
    std::string input;
    std::getline(std::cin, input);
    if (isBackInput(input)) { g_back = true; return defaultVal; }
    const bool val = input.empty() ? defaultVal : (input[0] == 'y' || input[0] == 'Y');
    nc::log::rawLine(prompt + " [" + def + "]: " + (val ? "y" : "n")); // LOGBANNER_V1
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
    static const char* kArt[] = { // BIGBANNER_V1
        "  ███████╗███████╗██╗   ██╗██╗   ██╗ ██████╗ ██████╗ ██╗   ██╗███╗   ██╗",
        "  ╚══███╔╝██╔════╝██║   ██║██║   ██║██╔═══██╗██╔══██╗╚██╗ ██╔╝████╗  ██║",
        "    ███╔╝ █████╗  ██║   ██║██║   ██║██║   ██║██████╔╝ ╚████╔╝ ██╔██╗ ██║",
        "   ███╔╝  ██╔══╝  ╚██╗ ██╔╝╚██╗ ██╔╝██║   ██║██╔══██╗  ╚██╔╝  ██║╚██╗██║",
        "  ███████╗███████╗ ╚████╔╝  ╚████╔╝ ╚██████╔╝██║  ██║   ██║   ██║ ╚████║",
        "  ╚══════╝╚══════╝  ╚═══╝    ╚═══╝   ╚═════╝ ╚═╝  ╚═╝   ╚═╝   ╚═╝  ╚═══╝",
    };
    std::cout << "\n";
    std::cout << "\033[32m\033[1m";
    nc::log::rawLine(""); // LOGBANNER_V1: баннер мастера установки тоже пишется в лог
    for (const char* a : kArt) { std::cout << a << "\n"; nc::log::rawLine(a); }
    std::cout << "\033[0m\n";
    std::cout << "\033[36m\033[1m";
    int w = 58;
    const std::string kRows[] = {
        boxLine(w),
        boxRow("ZEVVORYN SERVER", w),
        boxRow("Minecraft Java Edition 1.21.1", w),
        boxRow("C++20 Native \xe2\x94\x82 No Java \xe2\x94\x82 No Wrappers", w),
        boxBottom(w),
    };
    for (const auto& r : kRows) { std::cout << r << "\n"; nc::log::rawLine(r); }
    std::cout << "\033[0m\n";
    nc::log::rawLine(""); // LOGBANNER_V1
}

inline void printStep(int num, int total, const std::string& title) {
    std::cout << "  [" << num << "/" << total << "] " << title << "\n";
    std::cout << "  " << std::string(44, '-') << "\n";
    nc::log::rawLine("  [" + std::to_string(num) + "/" + std::to_string(total) + "] " + title); // LOGBANNER_V1
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

// SINGLEPASS_V1: если DiscordBotRcon/.env уже существует, подменяем в нём строку
// RCON_PASSWORD, чтобы панель и сервер никогда не разъехались по токену.
inline void syncEnvRconPassword(const std::string& password) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path envPath = fs::path("DiscordBotRcon") / ".env";
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

    printBanner();
    std::cout << "  \xd0\x9f\xd0\xbe\xd0\xb4\xd1\x81\xd0\xba\xd0\xb0\xd0\xb7\xd0\xba\xd0\xb0\x3a\x20\xd0\xb2\xd0\xb2\xd0\xb5\xd0\xb4\xd0\xb8\xd1\x82\xd0\xb5\x20\x62\x20\x28\xd0\xb8\xd0\xbb\xd0\xb8\x20\"\xd0\xbd\xd0\xb0\xd0\xb7\xd0\xb0\xd0\xb4\"\x2c\x20\x62\x61\x63\x6b\x29\x2c\x20\xd1\x87\xd1\x82\xd0\xbe\xd0\xb1\xd1\x8b\x20\xd0\xb2\xd0\xb5\xd1\x80\xd0\xbd\xd1\x83\xd1\x82\xd1\x8c\xd1\x81\xd1\x8f\x20\xd0\xbd\xd0\xb0\x20\xd1\x88\xd0\xb0\xd0\xb3\x20\xd0\xbd\xd0\xb0\xd0\xb7\xd0\xb0\xd0\xb4\n";
    std::cout << "  Hint: type b (or \"back\") to return to the previous step\n\n";

    bool ru = true;
    int step = 1;
    while (step <= 9) { // WIZARD_RCON_BOT_V1 WIZARD_WEB_V1 WIZARD_WHITELIST_V1
        g_back = false;
        g_prompts = 0;
        switch (step) {
        case 1: {
            // ── Шаг 1: Язык (первый, чтобы знать остальные) ──
            printStep(1, 9, "Language / \xd0\xaf\xd0\xb7\xd1\x8b\xd0\xba");
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
            printStep(2, 9, ru ? "\xd0\xa1\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80" : "Server");
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
            printStep(3, 9, ru ? "\xd0\x9c\xd0\xb8\xd1\x80" : "World");
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
            printStep(4, 9, ru ? "\xd0\x93\xd0\xb5\xd0\xb9\xd0\xbc\xd0\xbf\xd0\xbb\xd0\xb5\xd0\xb9" : "Gameplay");
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
            printStep(5, 9, ru ? "\xd0\x94\xd0\xbe\xd0\xbf\xd0\xbe\xd0\xbb\xd0\xbd\xd0\xb8\xd1\x82\xd0\xb5\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe" : "Advanced");
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
            printStep(6, 9, ru ? "\xd0\x91\xd0\xb5\xd0\xbb\xd1\x8b\xd0\xb9 \xd1\x81\xd0\xbf\xd0\xb8\xd1\x81\xd0\xbe\xd0\xba" : "Whitelist");
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
            printStep(7, 9, "RCON");
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
            printStep(8, 9, ru ? "Discord-\xd0\xb1\xd0\xbe\xd1\x82" : "Discord bot");
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
            printStep(9, 9, ru ? "\xd0\x92\xd0\xb5\xd0\xb1-\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c" : "Web panel");
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
                std::cout << (ru ? "  \xd0\x9d\xd1\x83\xd0\xb6\xd0\xb5\xd0\xbd \xd0\xba\xd0\xbe\xd0\xbd\xd0\xba\xd1\x80\xd0\xb5\xd1\x82\xd0\xbd\xd1\x8b\xd0\xb9 IP \xd0\xb8\xd0\xbd\xd1\x82\xd0\xb5\xd1\x80\xd1\x84\xd0\xb5\xd0\xb9\xd1\x81\xd0\xb0? \xd0\x92\xd0\xbf\xd0\xb8\xd1\x88\xd0\xb8\xd1\x82\xd0\xb5 \xd0\xb5\xd0\xb3\xd0\xbe \xd0\xbf\xd0\xbe\xd1\x82\xd0\xbe\xd0\xbc \xd0\xb2 DiscordBotRcon/.env \xd0\xb2 WEB_HOST.\n" : "  Need a specific interface IP? Put it into WEB_HOST in DiscordBotRcon/.env later.\n");
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

    // WIZARD_RCON_BOT_V1 / WIZARD_WEB_V1: write DiscordBotRcon/.env
    if ((wizardBotSetup && !wizardBotToken.empty()) || wizardWebEnabled) {
        std::error_code ec;
        std::filesystem::create_directories("DiscordBotRcon", ec);
        std::ofstream ev("DiscordBotRcon/.env", std::ios::trunc);
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
            std::cout << "  [OK] DiscordBotRcon/.env " << (ru ? "\xd1\x81\xd0\xbe\xd0\xb7\xd0\xb4\xd0\xb0\xd0\xbd" : "created") << "\n";
            if (wizardBotSetup && !wizardBotToken.empty()) {
                std::cout << (ru ? "  \xd0\x91\xd0\xbe\xd1\x82: cd DiscordBotRcon && npm install && npm run deploy && npm start\n"
                                 : "  Bot: cd DiscordBotRcon && npm install && npm run deploy && npm start\n");
            }
            if (wizardWebEnabled) {
                std::cout << (ru ? "  \xd0\x92\xd0\xb5\xd0\xb1: cd DiscordBotRcon && npm install && npm start (\xd0\xb2\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb5\xd0\xbd\xd0\xb0 \xd0\xbf\xd0\xbe \xd1\x83\xd0\xbc\xd0\xbe\xd0\xbb\xd1\x87\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x8e)\n"
                                 : "  Web: cd DiscordBotRcon && npm install && npm start (auto-starts by default)\n");
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
