#pragma once

#include "types.hpp"
#include "log.hpp"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
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

// ============================================================
// ServerConfig — читает/пишет settings.properties (key=value)
// Формат совместим с PMCpp / PocketMine style
// ============================================================
struct ServerConfig {
    // ── Сервер ──
    std::string language        = "rus";
    std::string motd            = "Zevvoryn Server";
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

inline ServerConfig runWizard() { // WIZARD_BACK_V1
    ServerConfig cfg;

    printBanner();
    std::cout << "  \xd0\x9f\xd0\xbe\xd0\xb4\xd1\x81\xd0\xba\xd0\xb0\xd0\xb7\xd0\xba\xd0\xb0\x3a\x20\xd0\xb2\xd0\xb2\xd0\xb5\xd0\xb4\xd0\xb8\xd1\x82\xd0\xb5\x20\x62\x20\x28\xd0\xb8\xd0\xbb\xd0\xb8\x20\"\xd0\xbd\xd0\xb0\xd0\xb7\xd0\xb0\xd0\xb4\"\x2c\x20\x62\x61\x63\x6b\x29\x2c\x20\xd1\x87\xd1\x82\xd0\xbe\xd0\xb1\xd1\x8b\x20\xd0\xb2\xd0\xb5\xd1\x80\xd0\xbd\xd1\x83\xd1\x82\xd1\x8c\xd1\x81\xd1\x8f\x20\xd0\xbd\xd0\xb0\x20\xd1\x88\xd0\xb0\xd0\xb3\x20\xd0\xbd\xd0\xb0\xd0\xb7\xd0\xb0\xd0\xb4\n";
    std::cout << "  Hint: type b (or \"back\") to return to the previous step\n\n";

    bool ru = true;
    int step = 1;
    while (step <= 5) {
        g_back = false;
        g_prompts = 0;
        switch (step) {
        case 1: {
            // ── Шаг 1: Язык (первый, чтобы знать остальные) ──
            printStep(1, 5, "Language / \xd0\xaf\xd0\xb7\xd1\x8b\xd0\xba");
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
            printStep(2, 5, ru ? "\xd0\xa1\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80" : "Server");
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
            printStep(3, 5, ru ? "\xd0\x9c\xd0\xb8\xd1\x80" : "World");
            std::cout << "    1) DEFAULT" << (ru ? " (\xd0\xbe\xd0\xb1\xd1\x8b\xd1\x87\xd0\xbd\xd1\x8b\xd0\xb9 \xe2\x80\x94 \xd0\xb1\xd0\xb8\xd0\xbe\xd0\xbc\xd1\x8b, \xd1\x80\xd0\xb5\xd0\xbb\xd1\x8c\xd0\xb5\xd1\x84, \xd1\x80\xd0\xb5\xd0\xba\xd0\xbe\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb4\xd1\x83\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f)" : " (normal - biomes & terrain, recommended)") << "\n";
            std::cout << "    2) FLAT   " << (ru ? "(\xd0\xbf\xd0\xbb\xd0\xbe\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9 \xd0\xbc\xd0\xb8\xd1\x80)" : "(superflat)") << "\n";
            std::string genChoice = readLine(ru ? "  \xd0\x93\xd0\xb5\xd0\xbd\xd0\xb5\xd1\x80\xd0\xb0\xd1\x82\xd0\xbe\xd1\x80" : "  Generator", "1");
            cfg.generator = (genChoice == "2") ? "FLAT" : "DEFAULT"; // WIZARD_WORLDTYPE_V1: DEFAULT теперь выбор по умолчанию (Enter)
            cfg.levelSeed = readInt64(ru ? "  \xd0\xa1\xd0\xb8\xd0\xb4 (0 = \xd1\x81\xd0\xbb\xd1\x83\xd1\x87\xd0\xb0\xd0\xb9\xd0\xbd\xd0\xbe\xd0\xb5)" : "  Seed (0 = random)", cfg.levelSeed);
            std::cout << "\n";
            break;
        }
        case 4: {
            // ── Шаг 4: Геймплей ──
            printStep(4, 5, ru ? "\xd0\x93\xd0\xb5\xd0\xb9\xd0\xbc\xd0\xbf\xd0\xbb\xd0\xb5\xd0\xb9" : "Gameplay");
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
            printStep(5, 5, ru ? "\xd0\x94\xd0\xbe\xd0\xbf\xd0\xbe\xd0\xbb\xd0\xbd\xd0\xb8\xd1\x82\xd0\xb5\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe" : "Advanced");
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

    // ── Сохранение ──
    std::cout << "  " << std::string(44, '=') << "\n";
    std::cout << "  " << (ru ? "\xd0\xa1\xd0\xbe\xd1\x85\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5 \xd0\xba\xd0\xbe\xd0\xbd\xd1\x84\xd0\xb8\xd0\xb3\xd1\x83\xd1\x80\xd0\xb0\xd1\x86\xd0\xb8\xd0\xb8..." : "Saving configuration...") << "\n";

    cfg.saveTo("settings.properties");
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
