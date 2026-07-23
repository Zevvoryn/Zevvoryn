#include "server.hpp"
#include "log.hpp"
#include "../registries/registry.hpp"
#include "../utils/nbt.hpp"

#include <nlohmann/json.hpp>
#include <format>
#include <thread>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <chrono> // VIEWDIST_V1
#include <algorithm> // CLIENT_BATCH_V1
#include <utility> // CLIENT_BATCH_V1
#include <sstream> // CMDS_V1
#include <cctype> // OPS_V1
#include "item_blocks.gen.hpp" // BLOCKS_V2
#include "icon.gen.hpp" // ICON_V1: встроенная иконка сервера + папка icon_Server
#include "../crypto/mc_crypto.hpp" // ONLINE_V1
#include <random> // SPAWN_V1

namespace nc {

// TPS20_V1: Windows default timer resolution is ~15.6ms, so sleep(1ms) sleeps ~15ms
// and the tick loop yields ~16 TPS instead of 20. timeBeginPeriod(1) raises it to 1ms.
// (MSVC links winmm via the pragma below; with MinGW add -lwinmm.)
#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned __stdcall timeBeginPeriod(unsigned);
extern "C" __declspec(dllimport) unsigned __stdcall timeEndPeriod(unsigned);
#pragma comment(lib, "winmm.lib")
#endif

// TPS_BOSS_V1: one stable Java Boss Event id shared by all players.
static constexpr UUID TPS_BOSSBAR_ID{0x5A6576766F72796EULL, 0x5450534D6F6E6974ULL};
static i32 tpsBossColor(f32 tps) {
    if (tps >= 19.5f) return 3; // GREEN
    if (tps >= 18.0f) return 4; // YELLOW
    if (tps >= 15.0f) return 2; // RED
    return 5;                   // PURPLE
}
static std::string tpsBossTitle(f32 tps) {
    const f32 load = std::clamp(100.0f - tps * 5.0f, 0.0f, 100.0f);
    return std::format("TPS {:.2f} | load {:.0f}%", tps, load);
}
// Java 1.21.1 serializes chat components as unnamed network NBT, not JSON strings.
static void writeTextComponent(net::Buffer& out, std::string_view text) {
    out.writeByte(0x08); // TAG_String root
    out.writeU16(static_cast<u16>(text.size()));
    out.writeBytes(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

// ONLINE_V1: 32-char hex (Mojang UUID without dashes) -> UUID
static UUID uuidFromHex(const std::string& hex) {
    auto val = [](char c) -> u64 {
        if (c >= '0' && c <= '9') return (u64)(c - '0');
        if (c >= 'a' && c <= 'f') return (u64)(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return (u64)(c - 'A' + 10);
        return 0;
    };
    std::string h;
    for (char c : hex) if (c != '-') h.push_back(c);
    while (h.size() < 32) h.insert(h.begin(), '0');
    UUID u{0, 0};
    for (int i = 0; i < 16; ++i) u.mostSignificant = (u.mostSignificant << 4) | val(h[(size_t)i]);
    for (int i = 16; i < 32; ++i) u.leastSignificant = (u.leastSignificant << 4) | val(h[(size_t)i]);
    return u;
}

NetherCraftServer::NetherCraftServer() {
    registries::RegistryManager::instance().loadDefaults();
}

NetherCraftServer::~NetherCraftServer() {
    stop();
}

bool NetherCraftServer::start(const std::string& configPath) {
    config_ = ServerConfig::loadFrom(configPath);
    return startCommon();
}

bool NetherCraftServer::startWithConfig(const ServerConfig& cfg) {
    config_ = cfg;
    return startCommon();
}

// SPAWN_V1: мировая точка спавна (общая), персистится в world/spawn.dat
static i32 g_spawnX = 0, g_spawnY = -60, g_spawnZ = 0; // FLATVANILLA_V1: суперфлэт как в ванилле — ноги игрока на Y=-60

// WEATHER_SYNC_V1: единое состояние погоды для всех клиентов.
// 0 = ясно, 1 = дождь, 2 = гроза.
static i32 g_weather = 0;
static void sendWeatherState(const std::shared_ptr<entity::Player>& player) {
    if (!player || !player->isAlive()) return;
    auto sendEvent = [&](u8 eventId, f32 value) {
        net::Buffer event;
        event.writeByte(eventId);
        event.writeF32(value);
        player->getConnection()->sendPacket(0x22,
            std::vector<u8>(event.writtenSpan().begin(), event.writtenSpan().end()));
    };
    sendEvent(g_weather == 0 ? 1 : 2, 0.0f); // stop/start raining
    sendEvent(7, g_weather == 0 ? 0.0f : 1.0f); // rain level
    sendEvent(8, g_weather == 2 ? 1.0f : 0.0f); // thunder level
}

static void writeWorldSpawn(i32 x, i32 y, i32 z) {
    std::error_code ec; std::filesystem::create_directories("world", ec);
    std::ofstream f("world/spawn.dat", std::ios::trunc);
    if (f) f << x << " " << y << " " << z << "\n";
}
static bool readWorldSpawn(i32& x, i32& y, i32& z) {
    std::ifstream f("world/spawn.dat");
    return bool(f >> x >> y >> z);
}

bool NetherCraftServer::startCommon() {
    auto startTime = std::chrono::steady_clock::now();

    // ── Баннер запуска ──
    std::cout << "\n";
    std::cout << "\033[36m[Starting server...]\033[0m\n\n";
    log::rawLine("[Starting server...]"); // LOGBANNER_V1: баннер запуска дублируется в файл лога
    log::rawLine("");

    std::cout << "\033[32m\033[1m";
    std::cout << "  ███████╗███████╗██╗   ██╗██╗   ██╗ ██████╗ ██████╗ ██╗   ██╗███╗   ██╗\n";
    std::cout << "  ╚══███╔╝██╔════╝██║   ██║██║   ██║██╔═══██╗██╔══██╗╚██╗ ██╔╝████╗  ██║\n";
    std::cout << "    ███╔╝ █████╗  ██║   ██║██║   ██║██║   ██║██████╔╝ ╚████╔╝ ██╔██╗ ██║\n";
    std::cout << "   ███╔╝  ██╔══╝  ╚██╗ ██╔╝╚██╗ ██╔╝██║   ██║██╔══██╗  ╚██╔╝  ██║╚██╗██║\n";
    std::cout << "  ███████╗███████╗ ╚████╔╝  ╚████╔╝ ╚██████╔╝██║  ██║   ██║   ██║ ╚████║\n";
    std::cout << "  ╚══════╝╚══════╝  ╚═══╝    ╚═══╝   ╚═════╝ ╚═╝  ╚═╝   ╚═╝   ╚═╝  ╚═══╝\n";
    std::cout << "\033[0m";
    { // LOGBANNER_V1: ASCII-баннер в файл лога
        static const char* kBannerArt[] = {
            "  ███████╗███████╗██╗   ██╗██╗   ██╗ ██████╗ ██████╗ ██╗   ██╗███╗   ██╗",
            "  ╚══███╔╝██╔════╝██║   ██║██║   ██║██╔═══██╗██╔══██╗╚██╗ ██╔╝████╗  ██║",
            "    ███╔╝ █████╗  ██║   ██║██║   ██║██║   ██║██████╔╝ ╚████╔╝ ██╔██╗ ██║",
            "   ███╔╝  ██╔══╝  ╚██╗ ██╔╝╚██╗ ██╔╝██║   ██║██╔══██╗  ╚██╔╝  ██║╚██╗██║",
            "  ███████╗███████╗ ╚████╔╝  ╚████╔╝ ╚██████╔╝██║  ██║   ██║   ██║ ╚████║",
            "  ╚══════╝╚══════╝  ╚═══╝    ╚═══╝   ╚═════╝ ╚═╝  ╚═╝   ╚═╝   ╚═╝  ╚═══╝",
        };
        for (const char* artLn : kBannerArt) log::rawLine(artLn);
        log::rawLine("");
    }

    std::cout << "\n";
    const char* tl = "\xe2\x95\x94"; // ╔
    const char* tr = "\xe2\x95\x97"; // ╗
    const char* bl = "\xe2\x95\x9a"; // ╚
    const char* br = "\xe2\x95\x9d"; // ╝
    const char* h  = "\xe2\x95\x90"; // ═
    const char* v  = "\xe2\x95\x91"; // ║
    int bw = 52; // ширина внутренней области
    auto line = [&](const char* l, const char* r) {
        std::string s = "  "; s += l;
        for (int i = 0; i < bw; ++i) s += h;
        s += r;
        std::cout << s << "\n";
        log::rawLine(s); // LOGBANNER_V1
    };
    auto row = [&](const std::string& content, int displayWidth) {
        (void)displayWidth; // WIZARD_UI_V1: считаем реальную ширину по UTF-8
        int cols = 0;
        for (unsigned char uch : content) if ((uch & 0xC0) != 0x80) ++cols;
        int pad = bw - cols;
        if (pad < 0) pad = 0;
        int leftPad = pad / 2;
        int rightPad = pad - leftPad;
        std::string s = "  "; s += v;
        s.append(static_cast<size_t>(leftPad), ' ');
        s += content;
        s.append(static_cast<size_t>(rightPad), ' ');
        s += v;
        std::cout << s << "\n";
        log::rawLine(s); // LOGBANNER_V1
    };
    std::cout << "\033[36m\033[1m";
    line(tl, tr);
    row("Minecraft Java Edition 1.21.1", 30);
    row("High Performance C++20", 22);
    row("No JVM | No JNI | No Wrappers", 30);
    line(bl, br);
    std::cout << "\033[0m\n";

    // Создаём структуру директорий
    std::filesystem::create_directories(config_.levelName);
    std::filesystem::create_directories("logs");
    log::initFileLog(config_.language); // LOGNAME_V1: logs/log-ДД.ММ.ГГ.log (eng: ММ.ДД.ГГ), 15 последних
    log::setLevelFromString(config_.logLevel); // LOGLEVEL_V1: apply configured verbosity (default INFO) so startup logs are visible

    NC_INFO("Server", "Loading server configuration");

    // Определяем язык
    if (config_.language == "rus") {
        NC_INFO("Server", "Выбран русский (rus) как основной язык");
    } else {
        NC_INFO("Server", "Language set to English (eng)");
    }

    NC_INFO("Server", "Starting Minecraft: Java Edition server v1.21.1");
    NC_INFO("Server", "Online mode: {}", config_.xboxAuth ? "enabled" : "disabled");
    // ONLINE_V1: online-mode uses RSA/AES + Mojang sessionserver
    if (config_.xboxAuth) {
        if (crypto::ServerKey::instance().valid()) {
            NC_INFO("Server", "Mojang authentication enabled.");
        } else {
            NC_ERROR("Server", "Online mode requested but crypto init failed; running offline");
            config_.xboxAuth = false;
        }
    }
    if (false) { // AUTHWARN_V1 (dead: superseded by ONLINE_V1)
        NC_WARN("Server", "Онлайн режим (Mojang auth) пока не реализован: нужны RSA/AES-шифрование и session-сервер Mojang. Сервер работает как offline.");
    }

    // PERF_TUNE_V1: cap chunk-gen worker threads from config (max-cores; 0 = auto).
    if (config_.maxCores > 0) world_.setGenThreadCount(static_cast<unsigned>(config_.maxCores));

    i32 radius = config_.viewDistance / 16 + 1;
    const bool ru = (config_.language == "rus"); // SPAWN_V1
    const bool isDefault = (config_.generator == "DEFAULT" || config_.generator == "default"); // WORLDGEN_V1
    const bool worldExists = std::filesystem::exists("world/world.dat"); // SEED_V1

    // SEED_V1: сид 0 = случайный; фиксируем и переиспользуем (для существующего мира читаем world/seed.dat)
    if (worldExists) {
        i64 savedSeed = 0; std::ifstream sf("world/seed.dat");
        if (sf >> savedSeed) config_.levelSeed = savedSeed;
    } else {
        if (config_.levelSeed == 0) {
            std::random_device rd;
            std::mt19937_64 g(((u64)rd() << 32) ^ (u64)rd());
            config_.levelSeed = (i64)g();
            if (config_.levelSeed == 0) config_.levelSeed = 1;
        }
        std::error_code ec; std::filesystem::create_directories("world", ec);
        std::ofstream sf("world/seed.dat", std::ios::trunc);
        if (sf) sf << config_.levelSeed << "\n";
    }
    world_.setLanguageRu(ru); // LANGFIX_V1: язык логов модуля мира
    if (ru) NC_INFO("Server", "Сид мира: {}", (long long)config_.levelSeed); // SPAWN_FIX_V1
    else    NC_INFO("Server", "World seed: {}", (long long)config_.levelSeed);

    // WORLDSAVE_V1: сначала пробуем загрузить сохранённый мир
    if (world_.startBackgroundLoad("world/world.dat")) { // FASTBOOT_V1: header on main thread, bodies stream in background
        if (isDefault) world_.initDefaultGenerator(config_.levelSeed, ru); // WORLDGEN_V1
        else world_.initFlatGenerator(); // FLATWORLD_V1: новые чанки генерятся на лету
        if (ru) NC_INFO("Server", "Мир загружается с диска в фоне (старт не ждёт)...");
        else    NC_INFO("Server", "World loading from disk in background (startup won't block)...");
        if (!readWorldSpawn(g_spawnX, g_spawnY, g_spawnZ) && isDefault) { // SPAWN_V1
            world_.findWorldSpawn(config_.levelSeed, g_spawnX, g_spawnY, g_spawnZ);
            writeWorldSpawn(g_spawnX, g_spawnY, g_spawnZ);
        }
    } else {
        if (isDefault) { // WORLDGEN_V1
            world_.generateDefault(config_.levelSeed, 0, 0, radius, ru);
        } else {
            world_.generateFlat(config_.levelSeed, 0, 0, radius);
        }
        if (isDefault) world_.findWorldSpawn(config_.levelSeed, g_spawnX, g_spawnY, g_spawnZ); // SPAWN_V1
        else { g_spawnX = 0; g_spawnY = -60; g_spawnZ = 0; } // FLATVANILLA_V1
        writeWorldSpawn(g_spawnX, g_spawnY, g_spawnZ);
        world_.saveToDisk("world/world.dat");
        if (ru) NC_INFO("Server", "Новый мир создан и сохранён в world/world.dat");
        else    NC_INFO("Server", "New world created and saved to world/world.dat");
        if (ru) NC_INFO("Server", "Мировой спавн: {} {} {}", g_spawnX, g_spawnY, g_spawnZ);
        else    NC_INFO("Server", "World spawn: {} {} {}", g_spawnX, g_spawnY, g_spawnZ);
    }

    network_.onConnection([this](auto conn) { onPlayerConnect(conn); });
    network_.onDisconnect([this](auto conn) { onPlayerDisconnect(conn); });
    network_.onPacket([this](auto conn, auto& data, auto id) { onPacketReceived(conn, data, id); });

    if (!network_.start(static_cast<u16>(config_.port))) {
        NC_FATAL("Server", "Failed to start server on port {}", config_.port);
        return false;
    }

    // LANGFIX_V1: строка о запуске — на выбранном языке (раньше была всегда по-русски из network/server.cpp)
    if (config_.language == "rus") NC_INFO("Server", "Сервер запущен на порту {}", config_.port);
    else                           NC_INFO("Server", "Server started on port {}", config_.port);

    { // ICON_V1: иконка сервера — встроенная по умолчанию, icon_Server/icon.png её заменяет
        std::string iconNote;
        iconFavicon_ = nc::icon::loadServerIconFavicon(config_.language == "rus", iconNote);
        if (!iconNote.empty()) NC_INFO("Server", "{}", iconNote);
    }

    std::string lang = (config_.language == "rus") ? "Русский (rus)" : "English (eng)";

    // Информация о сервере (как PMMP)
    std::cout << "\n";
    NC_INFO("Server", "Network interface listening on 0.0.0.0:{}", config_.port);
    NC_INFO("Server", "Default gamemode: {}", config_.gamemode);
    NC_INFO("Server", "Difficulty: {} ({})", config_.difficulty,
        config_.difficulty == 0 ? "Peaceful" :
        config_.difficulty == 1 ? "Easy" :
        config_.difficulty == 2 ? "Normal" : "Hard");
    NC_INFO("Server", "Language: {}", lang);
    NC_INFO("Server", "View distance: {} chunks", config_.viewDistance);
    NC_INFO("Server", "Max players: {}", config_.maxPlayers);
    std::cout << "\n";

    // Ссылки
    std::cout << "\033[36m";
    std::cout << "  - GitHub:     https://github.com/Zevvoryn/Zevvoryn\n";
    std::cout << "\033[0m\n";

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();
    double seconds = elapsed / 1000.0;

    if (config_.language == "rus") {
        NC_INFO("Server", "Done! (\xd0\xb7\xd0\xb0 {:.2f} \xd1\x81\xd0\xb5\xd0\xba.) \xd0\xa2\xd0\xb8\xd0\xbf \xe2\x80\x9chelp\xe2\x80\x9d \xd0\xb4\xd0\xbb\xd1\x8f \xd1\x81\xd0\xbf\xd1\x80\xd0\xb0\xd0\xb2\xd0\xba\xd0\xb8.", seconds);
    } else {
        NC_INFO("Server", "Done! ({}s) Type 'help' for a list of commands.", seconds);
    }
    std::cout << "\n";

    return true;
}

void NetherCraftServer::stop() {
    // WORLDSAVE_V1: сохраняем мир и игроков при остановке
    if (world_.getLoadedChunkCount() > 0) {
        world_.saveToDisk("world/world.dat");
        for (auto& p : getAllPlayersCopy()) savePlayerData(p);
        if (config_.language == "rus") NC_INFO("Server", "Мир сохранён в world/world.dat"); // SPAWN_FIX_V1
        else NC_INFO("Server", "World saved to world/world.dat");
    }
    network_.stop();
}

void NetherCraftServer::queueConsoleCommand(std::string command) {
    std::lock_guard lock(consoleMutex_);
    if (!command.empty()) consoleCommands_.push_back(std::move(command));
}

void NetherCraftServer::run() {
    // TPS20_V1: drift-free tick scheduler + high-resolution timer on Windows.
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    using namespace std::chrono;
    const auto tickDuration = milliseconds(50);
    auto nextTick = steady_clock::now() + tickDuration;

    while (network_.isRunning()) {
        tick();

        nextTick += tickDuration; // fixed 50ms step so accumulated drift does not grow
        const auto now = steady_clock::now();
        if (now < nextTick) {
            // TPS20_V2: hybrid wait for a rock-solid 20.00 TPS. sleep_until alone
            // overshoots by up to the timer granularity (~1ms) each tick, dragging
            // the measured rate down to ~19.96. Sleep until ~1.2ms before the
            // deadline, then busy-spin the remainder for sub-ms accuracy.
            const auto spinFrom = nextTick - microseconds(1200);
            if (now < spinFrom) std::this_thread::sleep_until(spinFrom);
            while (steady_clock::now() < nextTick) { /* spin */ }
        } else if (now - nextTick > seconds(1)) {
            // Fell far behind (long generation stall) - resync to avoid a tick storm.
            nextTick = now + tickDuration;
        }
    }

#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

// ============================================================
// Сетевые события
// ============================================================

void NetherCraftServer::onPlayerConnect(std::shared_ptr<net::Connection> conn) {
    u64 eid = nextEntityId_++;
    auto player = std::make_shared<entity::Player>(eid, conn);
    conn->setData<entity::Player>(player);

    std::lock_guard lock(playersMutex_);
    players_[conn->getId()] = player;
}

void NetherCraftServer::onPlayerDisconnect(std::shared_ptr<net::Connection> conn) {
    auto player = conn->getData<entity::Player>();
    if (player && !player->getName().empty()) {
        NC_DEBUG("Server", "{} disconnected", player->getName());
        savePlayerData(player); // WORLDSAVE_V1
    }

    if (player) {
        broadcastPlayerRemove(player); // MP_V1: убрать сущность игрока у остальных
    }

    std::lock_guard lock(playersMutex_);
    players_.erase(conn->getId());
    network_.removeConnection(conn->getId());
}

void NetherCraftServer::onPacketReceived(std::shared_ptr<net::Connection> conn, net::Buffer& data, i32 wireId) {
    auto player = conn->getData<entity::Player>();
    if (!player) return;

    auto state = conn->getConnectionState();
    // FLATWORLD_V1: уб����ан лог-спам pkt wireId

    switch (state) {
        case ConnectionState::Handshaking:
            handleHandshake(player, data);
            break;
        case ConnectionState::Status:
            handleStatus(player, data, wireId);
            break;
        case ConnectionState::Login:
            handleLogin(player, data, wireId);
            break;
        case ConnectionState::Configuration:
            handleConfiguration(player, data, wireId);
            break;
        case ConnectionState::Play:
            handlePlay(player, data, wireId);
            break;
    }
}

// ============================================================
// Handshake
// ============================================================

void NetherCraftServer::handleHandshake(std::shared_ptr<entity::Player> player, net::Buffer& data) {
    i32 protocolVersion = data.readVarInt();
    std::string serverAddress = data.readString();
    u16 serverPort = data.readU16();
    i32 nextState = data.readVarInt();

    // nextState: 1=Status, 2=Login — показываем только Login
    if (nextState == 2) {
        NC_DEBUG("Server", "Handshake: protocol={}, login", protocolVersion);
    }

    if (protocolVersion != 767) {
        if (config_.language == "rus") NC_WARN("Server", "Неподдерживаемая версия: {}", protocolVersion);
        else NC_WARN("Server", "Unsupported protocol version: {}", protocolVersion); // LANGFIX_V1
        net::Buffer discBuf;
        nlohmann::json discJson;
        if (config_.language == "rus") discJson["text"] = std::format("Поддерживается только 1.21.1 (767). Версия: {}", protocolVersion);
        else discJson["text"] = std::format("Only 1.21.1 (767) is supported. Your version: {}", protocolVersion); // LANGFIX_V1
        discBuf.writeString(discJson.dump());
        player->getConnection()->sendPacket(0x00, std::vector<u8>(discBuf.writtenSpan().begin(), discBuf.writtenSpan().end()));
        return;
    }

    if (nextState == 1) {
        player->getConnection()->setConnectionState(ConnectionState::Status);
    } else if (nextState == 2) {
        player->getConnection()->setConnectionState(ConnectionState::Login);
    }
}

// ============================================================
// Status (Server List Ping)
// ============================================================

void NetherCraftServer::handleStatus(std::shared_ptr<entity::Player> player, net::Buffer& data, i32 wireId) {
    if (wireId == 0x00) {
        // Status Request — отправляем JSON ответ
        sendStatusResponse(player);
    } else if (wireId == 0x01) {
        // Ping — эхо обратно
        if (data.readableBytes() >= 8) {
            i64 payload = data.readI64();
            net::Buffer pongBuf;
            pongBuf.writeI64(payload);
            player->getConnection()->sendPacket(0x01, std::vector<u8>(pongBuf.writtenSpan().begin(), pongBuf.writtenSpan().end()));
        } else {
            // Пустой ping — отправляем 0
            net::Buffer pongBuf;
            pongBuf.writeI64(0);
            player->getConnection()->sendPacket(0x01, std::vector<u8>(pongBuf.writtenSpan().begin(), pongBuf.writtenSpan().end()));
        }
    }
}

void NetherCraftServer::sendStatusResponse(std::shared_ptr<entity::Player> player) {
    // Считаем только реальных игр��ков (в Play), а не все соединения
    int onlineCount = 0;
    {
        std::lock_guard lock(playersMutex_);
        for (auto& [id, p] : players_) {
            if (p->isAlive() && p->getState() == entity::PlayerState::Play) {
                onlineCount++;
            }
        }
    }

    nlohmann::json response;
    response["version"]["name"] = "1.21.1";
    response["version"]["protocol"] = 767;
    response["players"]["max"] = config_.maxPlayers;
    response["players"]["online"] = onlineCount;
    response["players"]["sample"] = nlohmann::json::array();
    response["description"]["text"] = config_.motd;
    if (!iconFavicon_.empty()) response["favicon"] = iconFavicon_; // ICON_V1: иконка 64x64 в списке серверов

    net::Buffer buf;
    buf.writeString(response.dump());
    player->getConnection()->sendPacket(0x00, std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
}

// ============================================================
// Login
// ============================================================

// SKIN_V1: write the game-profile "textures" property (skin + cape) into a Player
// Info Update / Login Success entry. Writes 0 properties if the player has none.
static void writeProfileProperties(net::Buffer& buf, const entity::Player& p) {
    if (p.texturesValue.empty()) { buf.writeVarInt(0); return; }
    buf.writeVarInt(1);
    buf.writeString("textures");
    buf.writeString(p.texturesValue);
    if (!p.texturesSignature.empty()) { buf.writeBool(true); buf.writeString(p.texturesSignature); }
    else buf.writeBool(false);
}

// SKIN_OFFLINE_V1: look up an offline player's premium skin by username via the
// public Mojang API (name -> uuid -> signed profile) and store it on the player.
// Best-effort and blocking; if the name isn't premium or the network fails we
// simply keep the default Steve/Alex skin.
static void fetchOfflineSkin(std::shared_ptr<entity::Player> player) {
    if (!player) return;
    const std::string name = player->getName();
    auto idResp = crypto::httpsGet("api.mojang.com", "/users/profiles/minecraft/" + name);
    if (!idResp) return;
    std::string uuidHex;
    try {
        auto j = nlohmann::json::parse(*idResp);
        uuidHex = j.value("id", std::string());
    } catch (...) { return; }
    if (uuidHex.empty()) return;
    auto prof = crypto::httpsGet("sessionserver.mojang.com",
        "/session/minecraft/profile/" + uuidHex + "?unsigned=false");
    if (!prof) return;
    try {
        auto j = nlohmann::json::parse(*prof);
        if (j.contains("properties") && j["properties"].is_array()) {
            for (auto& pr : j["properties"]) {
                if (pr.value("name", std::string()) == "textures") {
                    player->texturesValue     = pr.value("value", std::string());
                    player->texturesSignature = pr.value("signature", std::string());
                }
            }
        }
    } catch (...) { return; }
    if (!player->texturesValue.empty())
        NC_DEBUG("Server", "{}: offline skin fetched", name);
}

void NetherCraftServer::handleLogin(std::shared_ptr<entity::Player> player, net::Buffer& data, i32 wireId) {
    NC_DEBUG("Server", "Login packet: wireId=0x{:02X}, readable={}", wireId, data.readableBytes());
    if (wireId == 0x00) {
        // Login Start: string name + UUID
        std::string name = data.readString();
        UUID uuid = data.readUUID();

        player->setName(name);
        player->setUuid(uuid);
        player->setState(entity::PlayerState::Login);
        NC_DEBUG("Server", "Login: {}", name);

        // MP_DUP_V1: защита от входа под уже занятым ником — киваем старую сессию
        {
            const u64 myConnId = player->getConnection()->getId();
            auto existing = getAllPlayersCopy();
            for (auto& other : existing) {
                if (!other || !other->getConnection()) continue;
                if (other->getConnection()->getId() == myConnId) continue; // не себя
                if (other->getName() == name) {
                    NC_INFO("Server", "MP_DUP_V1: nik {} uzhe zanyat -> kick staroy sessii", name);
                    other->sendSystemMessage("Â§cВы вошли с другого места");
                    other->getConnection()->close();
                }
            }
        }

        // ZLIB_V1: включаем сетевое сжатие (Set Compression, Login clientbound 0x03) до Login Success
        // ONLINE_V1: for online-mode begin the encryption handshake instead of an instant LoginSuccess
        if (config_.xboxAuth && crypto::ServerKey::instance().valid()) {
            player->encVerifyToken = crypto::randomBytes(4);
            const auto& pub = crypto::ServerKey::instance().publicKeyDer();
            net::Buffer enc;
            enc.writeString("");
            enc.writeVarInt(static_cast<i32>(pub.size()));
            enc.writeBytes(std::span<const u8>(pub));
            enc.writeVarInt(static_cast<i32>(player->encVerifyToken.size()));
            enc.writeBytes(std::span<const u8>(player->encVerifyToken));
            enc.writeBool(true); // Should Authenticate (1.20.5+/767)
            player->getConnection()->sendPacket(0x01,
                std::vector<u8>(enc.writtenSpan().begin(), enc.writtenSpan().end()));
            NC_DEBUG("Server", "{}: EncryptionRequest sent, awaiting response", name);
            return;
        }

        if (config_.compressionThreshold >= 0) {
            net::Buffer cbuf;
            cbuf.writeVarInt(config_.compressionThreshold);
            player->getConnection()->sendPacket(0x03,
                std::vector<u8>(cbuf.writtenSpan().begin(), cbuf.writtenSpan().end()));
            player->getConnection()->enableCompression(config_.compressionThreshold);
        }

        // SKIN_OFFLINE_V1: offline (cracked) players have no Mojang session, so
        // fetch their premium skin/cape by name and relay it like an online player.
        fetchOfflineSkin(player);

        sendLoginSuccess(player);
    } else if (wireId == 0x01) {
        // ONLINE_V1: Encryption Response - decrypt shared secret, verify session with Mojang
        i32 ssLen = data.readVarInt();
        std::vector<u8> encSecret = data.readBytes(static_cast<size_t>(ssLen));
        i32 vtLen = data.readVarInt();
        std::vector<u8> encToken = data.readBytes(static_cast<size_t>(vtLen));

        auto& key = crypto::ServerKey::instance();
        std::vector<u8> secret = key.decrypt(std::span<const u8>(encSecret));
        std::vector<u8> token  = key.decrypt(std::span<const u8>(encToken));

        if (secret.size() != 16 || token != player->encVerifyToken) {
            NC_WARN("Server", "{}: encryption handshake failed (bad secret/token)", player->getName());
            player->getConnection()->close();
            return;
        }

        // Enable AES on the socket (both directions) before any further packets.
        player->getConnection()->enableEncryption(std::span<const u8>(secret));

        // Mojang server hash + session verification via sessionserver.mojang.com.
        std::string hash = crypto::mcServerHash(std::span<const u8>(),
            std::span<const u8>(secret), std::span<const u8>(key.publicKeyDer()));
        std::optional<std::string> resp = crypto::hasJoined(player->getName(), hash);
        if (!resp) {
            NC_WARN("Server", "{}: Mojang auth failed (session not verified)", player->getName());
            net::Buffer disc;
            nlohmann::json dj; dj["text"] = "Online-mode: session could not be verified";
            disc.writeString(dj.dump());
            player->getConnection()->sendPacket(0x00,
                std::vector<u8>(disc.writtenSpan().begin(), disc.writtenSpan().end()));
            player->getConnection()->close();
            return;
        }

        std::string texValue, texSig, realName = player->getName();
        UUID realUuid = player->getUuid();
        try {
            auto j = nlohmann::json::parse(*resp);
            if (j.contains("id"))   realUuid = uuidFromHex(j["id"].get<std::string>());
            if (j.contains("name")) realName = j["name"].get<std::string>();
            if (j.contains("properties") && j["properties"].is_array()) {
                for (auto& pr : j["properties"]) {
                    if (pr.value("name", std::string()) == "textures") {
                        texValue = pr.value("value", std::string());
                        texSig   = pr.value("signature", std::string());
                    }
                }
            }
        } catch (...) {
            NC_WARN("Server", "{}: failed to parse session response", player->getName());
        }

        player->setUuid(realUuid);
        player->setName(realName);
        player->texturesValue     = texValue; // SKIN_V1: keep skin/cape for multiplayer relay
        player->texturesSignature = texSig;
        NC_DEBUG("Server", "{} authenticated (online-mode)", realName);

        if (config_.compressionThreshold >= 0) {
            net::Buffer cbuf;
            cbuf.writeVarInt(config_.compressionThreshold);
            player->getConnection()->sendPacket(0x03,
                std::vector<u8>(cbuf.writtenSpan().begin(), cbuf.writtenSpan().end()));
            player->getConnection()->enableCompression(config_.compressionThreshold);
        }

        // LoginSuccess with textures (player skin), matching the offline wire format.
        {
            net::Buffer buf;
            buf.writeUUID(player->getUuid());
            buf.writeString(player->getName());
            if (!texValue.empty()) {
                buf.writeVarInt(1);
                buf.writeString("textures");
                buf.writeString(texValue);
                if (!texSig.empty()) { buf.writeBool(true); buf.writeString(texSig); }
                else buf.writeBool(false);
            } else {
                buf.writeVarInt(0);
            }
            buf.writeBool(true); // strict error handling (matches sendLoginSuccess)
            player->getConnection()->sendPacket(0x02,
                std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
        }
    } else if (wireId == 0x03) {
        // Login Acknowledged — клиент переключился в Configuration
        NC_DEBUG("Server", "{} acknowledged login, switching to Configuration", player->getName());
        player->getConnection()->setConnectionState(ConnectionState::Configuration);
        player->setState(entity::PlayerState::Configuration);

        // Send feature flags + select_known_packs (client responds, then we send registries)
        sendInitialConfig(player);
    }
}

void NetherCraftServer::sendLoginSuccess(std::shared_ptr<entity::Player> player) {
    net::Buffer buf;
    buf.writeUUID(player->getUuid());
    buf.writeString(player->getName());

    // SKIN_V1/SKIN_OFFLINE_V1: include textures so offline players show their skin.
    writeProfileProperties(buf, *player);

    // 1.20.2+: Strict Error Handling (boolean) — ОБЯЗАТЕЛЕН
    buf.writeBool(true); // strict: ошибки протокола видны сразу, а не вечный Joining world

    // 0x02 = Login Success
    player->getConnection()->sendPacket(0x02, std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
}

// ============================================================
// Configuration (1.21.1)
// ============================================================

void NetherCraftServer::handleConfiguration(std::shared_ptr<entity::Player> player, net::Buffer& data, i32 wireId) {
    auto remaining = data.readableBytes();
    (void)remaining; // FLATWORLD_V1: убран лог-спам Config recv

    if (wireId == 0x03) {
        // Finish Configuration Acknowledged → Play
        NC_DEBUG("Server", "Игрок {} → Play", player->getName());
        player->getConnection()->setConnectionState(ConnectionState::Play);
        player->setState(entity::PlayerState::Play);
        NC_DEBUG("Server", "{} -> Play", player->getName());
        auto tPlay0 = std::chrono::steady_clock::now(); // VIEWDIST_V1: тайминг входа
        auto playMs = [&tPlay0]() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tPlay0).count();
        };

        player->setPosition(g_spawnX + 0.5, (f64)g_spawnY, g_spawnZ + 0.5); // SPAWN_V1: по умолчанию — мировой спавн
        loadPlayerData(player); // WORLDSAVE_V1: восстановить позицию игрока с диска

        sendJoinPlay(player);
        sendSpawnPosition(player);
        sendPlayerAbilities(player);
        sendPlayerPositionAndLook(player);

        { // LOADFIX_V2: Game Event 13 — start waiting for level chunks (без него клиент висит на Loading terrain до таймаута)
            net::Buffer geBuf;
            geBuf.writeByte(13);
            geBuf.writeF32(0.0f);
            player->getConnection()->sendPacket(0x22, std::vector<u8>(geBuf.writtenSpan().begin(), geBuf.writtenSpan().end()));
        }
        { // DEATHSCREEN_V1: Game Event 11 (immediate respawn) = 0 — принудительно выключаем
          // doImmediateRespawn, чтобы клиент всегда показывал экран смерти с кнопкой «Возродиться»
            net::Buffer irBuf;
            irBuf.writeByte(11);
            irBuf.writeF32(0.0f);
            player->getConnection()->sendPacket(0x22, std::vector<u8>(irBuf.writtenSpan().begin(), irBuf.writtenSpan().end()));
        }
        sendTimeUpdate(player);

        // PlayerInfoUpdate (0x3E) — add player to tab list
        {
            net::Buffer infoBuf;
            // Action bitmask: ADD_PLAYER(0x01) | UPDATE_GAME_MODE(0x04) | UPDATE_LISTED(0x08) | UPDATE_LATENCY(0x10) = 0x1D
            infoBuf.writeVarInt(0x1D);
            // Player count
            infoBuf.writeVarInt(1);
            // UUID
            infoBuf.writeUUID(player->getUuid());
            // game_profile: name
            infoBuf.writeString(player->getName());
            // game_profile: textures property (skin + cape) — SKIN_V1
            writeProfileProperties(infoBuf, *player);
            // INVENTORY_V3/GM3_FIX: реальный режим игрока в таб-листе.
            // Клиентский AbstractClientPlayer.isSpectator() читает режим ИМЕННО отсюда (PlayerInfo),
            // а не из login/GameEvent. Если слать жёстко 1 (creative), то в ГМ3 noPhysics=false и сквозь блоки не пройти.
            infoBuf.writeVarInt(player->gameMode);
            // listed: true
            infoBuf.writeVarInt(1);
            // latency: 0
            infoBuf.writeVarInt(0);
            player->getConnection()->sendPacket(0x3E,
                std::vector<u8>(infoBuf.writtenSpan().begin(), infoBuf.writtenSpan().end()));
        }

        { // CMDTREE_V2: полное дерево команд с аргумент����и
            net::Buffer cb;
            cb.writeVarInt(38); // GMSHORT_V1 // коли��ество узлов
            cb.writeByte(0x00); cb.writeVarInt(14); cb.writeVarInt(1); cb.writeVarInt(2); cb.writeVarInt(3); cb.writeVarInt(5); cb.writeVarInt(10); cb.writeVarInt(17); cb.writeVarInt(21); cb.writeVarInt(23); cb.writeVarInt(24); cb.writeVarInt(27); cb.writeVarInt(30); cb.writeVarInt(31); cb.writeVarInt(32); cb.writeVarInt(33); // [0] root (GMSHORT_V1: +gm0..gm3)
            cb.writeByte(0x05); cb.writeVarInt(0); cb.writeString("help"); // [1] help
            cb.writeByte(0x05); cb.writeVarInt(0); cb.writeString("list"); // [2] list
            cb.writeByte(0x01); cb.writeVarInt(1); cb.writeVarInt(4); cb.writeString("tp"); // [3] tp
            cb.writeByte(0x06); cb.writeVarInt(0); cb.writeString("pos"); cb.writeVarInt(10); // [4] pos
            cb.writeByte(0x01); cb.writeVarInt(8); cb.writeVarInt(6); cb.writeVarInt(7); cb.writeVarInt(8); cb.writeVarInt(9); cb.writeVarInt(34); cb.writeVarInt(35); cb.writeVarInt(36); cb.writeVarInt(37); cb.writeString("gamemode"); // [5] gamemode (GMSHORT_V1: +цифры 0-3)
            cb.writeByte(0x05); cb.writeVarInt(1); cb.writeVarInt(29); cb.writeString("survival"); // [6] survival (GMSHORT_V1: +[29] ник)
            cb.writeByte(0x05); cb.writeVarInt(1); cb.writeVarInt(29); cb.writeString("creative"); // [7] creative
            cb.writeByte(0x05); cb.writeVarInt(1); cb.writeVarInt(29); cb.writeString("adventure"); // [8] adventure
            cb.writeByte(0x05); cb.writeVarInt(1); cb.writeVarInt(29); cb.writeString("spectator"); // [9] spectator
            cb.writeByte(0x01); cb.writeVarInt(1); cb.writeVarInt(11); cb.writeString("time"); // [10] time
            cb.writeByte(0x01); cb.writeVarInt(5); cb.writeVarInt(12); cb.writeVarInt(13); cb.writeVarInt(14); cb.writeVarInt(15); cb.writeVarInt(16); cb.writeString("set"); // [11] set
            cb.writeByte(0x05); cb.writeVarInt(0); cb.writeString("day"); // [12] day
            cb.writeByte(0x05); cb.writeVarInt(0); cb.writeString("noon"); // [13] noon
            cb.writeByte(0x05); cb.writeVarInt(0); cb.writeString("night"); // [14] night
            cb.writeByte(0x05); cb.writeVarInt(0); cb.writeString("midnight"); // [15] midnight
            cb.writeByte(0x06); cb.writeVarInt(0); cb.writeString("ticks"); cb.writeVarInt(3); cb.writeByte(0); // [16] ticks
            cb.writeByte(0x01); cb.writeVarInt(3); cb.writeVarInt(18); cb.writeVarInt(19); cb.writeVarInt(20); cb.writeString("weather"); // [17] weather
            cb.writeByte(0x05); cb.writeVarInt(0); cb.writeString("clear"); // [18] clear
            cb.writeByte(0x05); cb.writeVarInt(0); cb.writeString("rain"); // [19] rain
            cb.writeByte(0x05); cb.writeVarInt(0); cb.writeString("thunder"); // [20] thunder
            cb.writeByte(0x01); cb.writeVarInt(1); cb.writeVarInt(22); cb.writeString("say"); // [21] say
            cb.writeByte(0x06); cb.writeVarInt(0); cb.writeString("msg"); cb.writeVarInt(5); cb.writeVarInt(2); // [22] msg
            cb.writeByte(0x05); cb.writeVarInt(0); cb.writeString("spawn"); // [23] spawn
            cb.writeByte(0x01); cb.writeVarInt(1); cb.writeVarInt(25); cb.writeString("setblock"); // [24] setblock
            cb.writeByte(0x02); cb.writeVarInt(1); cb.writeVarInt(26); cb.writeString("pos"); cb.writeVarInt(8); // [25] pos
            cb.writeByte(0x06); cb.writeVarInt(0); cb.writeString("block"); cb.writeVarInt(5); cb.writeVarInt(0); // [26] block
            cb.writeByte(0x01); cb.writeVarInt(1); cb.writeVarInt(28); cb.writeString("kick"); // [27] kick
            cb.writeByte(0x06); cb.writeVarInt(0); cb.writeString("player"); cb.writeVarInt(5); cb.writeVarInt(0); // [28] player
            cb.writeByte(0x06); cb.writeVarInt(0); cb.writeString("player"); cb.writeVarInt(5); cb.writeVarInt(0); // [29] GMSHORT_V1: ник цели для gamemode/gm0-3
            cb.writeByte(0x05); cb.writeVarInt(1); cb.writeVarInt(29); cb.writeString("gm0"); // [30] GMSHORT_V1
            cb.writeByte(0x05); cb.writeVarInt(1); cb.writeVarInt(29); cb.writeString("gm1"); // [31] GMSHORT_V1
            cb.writeByte(0x05); cb.writeVarInt(1); cb.writeVarInt(29); cb.writeString("gm2"); // [32] GMSHORT_V1
            cb.writeByte(0x05); cb.writeVarInt(1); cb.writeVarInt(29); cb.writeString("gm3"); // [33] GMSHORT_V1
            cb.writeByte(0x05); cb.writeVarInt(1); cb.writeVarInt(29); cb.writeString("0"); // [34] GMSHORT_V1
            cb.writeByte(0x05); cb.writeVarInt(1); cb.writeVarInt(29); cb.writeString("1"); // [35] GMSHORT_V1
            cb.writeByte(0x05); cb.writeVarInt(1); cb.writeVarInt(29); cb.writeString("2"); // [36] GMSHORT_V1
            cb.writeByte(0x05); cb.writeVarInt(1); cb.writeVarInt(29); cb.writeString("3"); // [37] GMSHORT_V1
            cb.writeVarInt(0); // индекс root
            player->getConnection()->sendPacket(0x11, std::vector<u8>(cb.writtenSpan().begin(), cb.writtenSpan().end()));
            NC_DEBUG("Server", "  Command tree sent: 38 nodes (+{} ms)", playMs());
        }
        { // ENTEVT_V1: уровень прав оператора (F3+F4 переключатель режимов)
            const std::string& _nm = player->getName();
            const std::string& _ov = config_.ops;
            bool opLvl = _ov.empty();
            if (!opLvl) {
                std::string _lo; _lo.reserve(_nm.size());
                for (char c : _nm) _lo.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(c))));
                size_t _p = 0;
                while (_p <= _ov.size()) {
                    size_t _cm = _ov.find(',', _p);
                    if (_cm == std::string::npos) _cm = _ov.size();
                    std::string _tk = _ov.substr(_p, _cm - _p);
                    size_t _a = _tk.find_first_not_of(" \t");
                    size_t _b = _tk.find_last_not_of(" \t");
                    if (_a != std::string::npos && _tk.substr(_a, _b - _a + 1) == _lo) { opLvl = true; break; }
                    _p = _cm + 1;
                }
            }
            net::Buffer ev;
            ev.writeI32(static_cast<i32>(player->getEntityId()));
            ev.writeByte(opLvl ? 28 : 24); // Entity Event: 24 + op level (0..4)
            player->getConnection()->sendPacket(0x1F, std::vector<u8>(ev.writtenSpan().begin(), ev.writtenSpan().end()));
        }

        // UpdateViewPosition (0x54) — tell client chunk center
        {
            net::Buffer viewBuf;
            viewBuf.writeVarInt(static_cast<i32>(std::floor(player->getX() / 16.0))); // FLATWORLD_V1
            viewBuf.writeVarInt(static_cast<i32>(std::floor(player->getZ() / 16.0)));
            player->getConnection()->sendPacket(0x54,
                std::vector<u8>(viewBuf.writtenSpan().begin(), viewBuf.writtenSpan().end()));
        }

        // UpdateViewDistance (0x55) — tell client view distance
        {
            net::Buffer vdBuf;
            vdBuf.writeVarInt(config_.viewDistance); // VIEWDIST_V1: реальный view distance
            player->getConnection()->sendPacket(0x55,
                std::vector<u8>(vdBuf.writtenSpan().begin(), vdBuf.writtenSpan().end()));
        }

        { // FLATWORLD_V1: чанки вокруг реальной позиции, радиус из view distance
            i32 pcx = static_cast<i32>(std::floor(player->getX() / 16.0));
            i32 pcz = static_cast<i32>(std::floor(player->getZ() / 16.0));
            player->setViewCenter(pcx, pcz);
            i32 r = config_.viewDistance; // VIEWDIST_V1: полный радиус, чтоб�� край мира не торчал до тумана
            if (r < 2) r = 2;
            sendChunksAround(player, pcx, pcz, r, 24); // PERF_ASYNC_V2: larger initial burst (was 9)
        }

        { // INVENTORY_V3: заливаем весь инвентарь клиенту при входе (иначе вещи не появляются после перезахода)
            net::Buffer inv;
            inv.writeByte(0);            // containerId = 0 (окно инвентаря игрока)
            inv.writeVarInt(1);          // state id
            inv.writeVarInt(entity::Player::INV_SIZE); // 46 слотов
            for (int i = 0; i < entity::Player::INV_SIZE; ++i) {
                const i32 id = player->invItemId[i];
                const i32 cnt = player->invCount[i];
                if (cnt > 0 && id > 0) { inv.writeVarInt(cnt); inv.writeVarInt(id); inv.writeVarInt(0); inv.writeVarInt(0); }
                else { inv.writeVarInt(0); }
            }
            inv.writeVarInt(0);          // предмет в курсоре: пусто
            player->getConnection()->sendPacket(0x13, std::vector<u8>(inv.writtenSpan().begin(), inv.writtenSpan().end()));
            net::Buffer carried; // выб��анн��й слот хотбара
            carried.writeByte((i8)(player->heldSlot >= 0 && player->heldSlot < 9 ? player->heldSlot : 0));
            player->getConnection()->sendPacket(0x53, std::vector<u8>(carried.writtenSpan().begin(), carried.writtenSpan().end()));
        }

        onPlayerEnterPlay(player); // MP_V1: пок��зать игрока другим и других — ему
    } else if (wireId == 0x07) {
        // select_known_packs from client → NOW send registries + tags + finish
        NC_DEBUG("Server", "Client sent select_known_packs (0x07), sending registries...");
        sendRegistryData(player);
        sendConfigurationFinish(player);
    } else if (wireId == 0x00) {
        // ClientInformation (Configuration) — locale, view distance, chat mode, etc.
        data.readString();      // locale (e.g. "ru_ru")
        data.readVarInt();      // view distance
        data.readVarInt();      // chat mode
        data.readBool();        // chat colors
        player->displayedSkinParts = static_cast<u8>(data.readByte()); // SKIN_V1: cape/layers bitmask
        data.readByte();        // main hand (0=left, 1=right)
        data.readBool();        // enable text filtering
        data.readBool();        // allow server listings
        NC_DEBUG("Server", "ClientInformation received from {}", player->getName());
    } else if (wireId == 0x02) {
        // PluginMessage (Configuration) — channel + data
        data.readString();      // channel (e.g. "minecraft:brand")
        size_t rem = data.readableBytes();
        if (rem > 0) data.readBytes(rem);
        NC_DEBUG("Server", "PluginMessage (config) from {}", player->getName());
    } else {
        NC_INFO("Server", "UNHANDLED Config wireId=0x{:02X}", wireId);
    }
}

void NetherCraftServer::sendInitialConfig(std::shared_ptr<entity::Player> player) {
    // Minestom order: brand → select_known_packs → [wait client] → registries → tags → finish
    // We skip brand (client already sent it).

    // 1. Feature Flags (0x0C)
    {
        net::Buffer buf;
        buf.writeVarInt(1);
        buf.writeString("minecraft:vanilla");
        player->getConnection()->sendPacket(0x0C,
            std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
        NC_DEBUG("Server", "Sent Feature Flags to {}", player->getName());
    }

    // 2. Select Known Packs (0x0E) — "minecraft:core:1.21.1"
    {
        net::Buffer buf;
        buf.writeVarInt(1); // 1 pack
        buf.writeString("minecraft");   // namespace
        buf.writeString("core");        // id
        buf.writeString("1.21.1");      // version
        player->getConnection()->sendPacket(0x0E,
            std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
        NC_DEBUG("Server", "Sent Select Known Packs (minecraft:core:1.21.1) to {}", player->getName());
    }
    // Client will respond with wireId=0x07 → handleConfiguration sends registries + finish
}

void NetherCraftServer::sendRegistryData(std::shared_ptr<entity::Player> player) {
    // REGISTRY_FULL_V2: полные ванильные списки записей 1.21.1.
    // Все записи с has_data=0 - клиент грузит данные из known pack
    // minecraft:core:1.21.1, который он подтвердил через select_known_packs.
    // Частичные реестры ломают Fabric-клиенты (Failed to load registries).
    auto sendRegistry = [&](const char* registryName,
                            std::initializer_list<const char*> entries) {
        net::Buffer p;
        p.writeString(registryName);
        p.writeVarInt(static_cast<i32>(entries.size()));
        for (const char* e : entries) {
            std::string full = std::string("minecraft:") + e;
            p.writeString(full);
            p.writeByte(0); // has_data = false (данные из known pack)
        }
        player->getConnection()->sendPacket(0x07,
            std::vector<u8>(p.writtenSpan().begin(), p.writtenSpan().end()));
        NC_DEBUG("Server", "Registry {} sent ({} entries, known-pack)",
            registryName, entries.size());
    };

    sendRegistry("minecraft:dimension_type", {
        "overworld", "overworld_caves", "the_end", "the_nether"
    });

    sendRegistry("minecraft:worldgen/biome", {
        "badlands", "bamboo_jungle", "basalt_deltas", "beach", "birch_forest", "cherry_grove",
        "cold_ocean", "crimson_forest", "dark_forest", "deep_cold_ocean", "deep_dark",
        "deep_frozen_ocean", "deep_lukewarm_ocean", "deep_ocean", "desert", "dripstone_caves",
        "end_barrens", "end_highlands", "end_midlands", "eroded_badlands", "flower_forest",
        "forest", "frozen_ocean", "frozen_peaks", "frozen_river", "grove", "ice_spikes",
        "jagged_peaks", "jungle", "lukewarm_ocean", "lush_caves", "mangrove_swamp", "meadow",
        "mushroom_fields", "nether_wastes", "ocean", "old_growth_birch_forest",
        "old_growth_pine_taiga", "old_growth_spruce_taiga", "plains", "river", "savanna",
        "savanna_plateau", "small_end_islands", "snowy_beach", "snowy_plains", "snowy_slopes",
        "snowy_taiga", "soul_sand_valley", "sparse_jungle", "stony_peaks", "stony_shore",
        "sunflower_plains", "swamp", "taiga", "the_end", "the_void", "warm_ocean",
        "warped_forest", "windswept_forest", "windswept_gravelly_hills", "windswept_hills",
        "windswept_savanna", "wooded_badlands"
    });

    sendRegistry("minecraft:chat_type", {
        "chat", "emote_command", "msg_command_incoming", "msg_command_outgoing", "say_command",
        "team_msg_command_incoming", "team_msg_command_outgoing"
    });

    sendRegistry("minecraft:damage_type", {
        "arrow", "bad_respawn_point", "cactus", "campfire", "cramming", "dragon_breath",
        "drown", "dry_out", "explosion", "fall", "falling_anvil", "falling_block",
        "falling_stalactite", "fireball", "fireworks", "fly_into_wall", "freeze", "generic",
        "generic_kill", "hot_floor", "in_fire", "in_wall", "indirect_magic", "lava",
        "lightning_bolt", "magic", "mob_attack", "mob_attack_no_aggro", "mob_projectile",
        "on_fire", "out_of_world", "outside_border", "player_attack", "player_explosion",
        "sonic_boom", "spit", "stalagmite", "starve", "sting", "sweet_berry_bush", "thorns",
        "thrown", "trident", "unattributed_fireball", "wind_charge", "wither", "wither_skull"
    });

    sendRegistry("minecraft:trim_pattern", {
        "bolt", "coast", "dune", "eye", "flow", "host", "raiser", "rib", "sentry", "shaper",
        "silence", "snout", "spire", "tide", "vex", "ward", "wayfinder", "wild"
    });

    sendRegistry("minecraft:trim_material", {
        "amethyst", "copper", "diamond", "emerald", "gold", "iron", "lapis", "netherite",
        "quartz", "redstone"
    });

    sendRegistry("minecraft:banner_pattern", {
        "base", "border", "bricks", "circle", "creeper", "cross", "curly_border",
        "diagonal_left", "diagonal_right", "diagonal_up_left", "diagonal_up_right", "flow",
        "flower", "globe", "gradient", "gradient_up", "guster", "half_horizontal",
        "half_horizontal_bottom", "half_vertical", "half_vertical_right", "mojang", "piglin",
        "rhombus", "skull", "small_stripes", "square_bottom_left", "square_bottom_right",
        "square_top_left", "square_top_right", "straight_cross", "stripe_bottom",
        "stripe_center", "stripe_downleft", "stripe_downright", "stripe_left", "stripe_middle",
        "stripe_right", "stripe_top", "triangle_bottom", "triangle_top", "triangles_bottom",
        "triangles_top"
    });

    sendRegistry("minecraft:enchantment", {
        "aqua_affinity", "bane_of_arthropods", "binding_curse", "blast_protection", "breach",
        "channeling", "density", "depth_strider", "efficiency", "feather_falling",
        "fire_aspect", "fire_protection", "flame", "fortune", "frost_walker", "impaling",
        "infinity", "knockback", "looting", "loyalty", "luck_of_the_sea", "lure", "mending",
        "multishot", "piercing", "power", "projectile_protection", "protection", "punch",
        "quick_charge", "respiration", "riptide", "sharpness", "silk_touch", "smite",
        "soul_speed", "sweeping_edge", "swift_sneak", "thorns", "unbreaking",
        "vanishing_curse", "wind_burst"
    });

    sendRegistry("minecraft:jukebox_song", {
        "11", "13", "5", "blocks", "cat", "chirp", "creator", "creator_music_box", "far",
        "mall", "mellohi", "otherside", "pigstep", "precipice", "relic", "stal", "strad",
        "wait", "ward"
    });

    sendRegistry("minecraft:painting_variant", {
        "alban", "aztec", "aztec2", "backyard", "baroque", "bomb", "bouquet", "burning_skull",
        "bust", "cavebird", "changing", "cotan", "courbet", "creebet", "donkey_kong", "earth",
        "endboss", "fern", "fighters", "finding", "fire", "graham", "humble", "kebab",
        "lowmist", "match", "meditative", "orb", "owlemons", "passage", "pigscene", "plant",
        "pointer", "pond", "pool", "prairie_ride", "sea", "skeleton", "skull_and_roses",
        "stage", "sunflowers", "sunset", "tides", "unpacked", "void", "wanderer", "wasteland",
        "water", "wind", "wither"
    });

    sendRegistry("minecraft:wolf_variant", {
        "ashen", "black", "chestnut", "pale", "rusty", "snowy", "spotted", "striped", "woods"
    });

    // Tags (0x0D) - пусто (валидно)
    {
        net::Buffer buf;
        buf.writeVarInt(0); // 0 tag types
        player->getConnection()->sendPacket(0x0D,
            std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
        NC_DEBUG("Server", "Sent Tags to {}", player->getName());
    }

    NC_DEBUG("Server", "Sent all registries to {}", player->getName());
}

void NetherCraftServer::sendConfigurationFinish(std::shared_ptr<entity::Player> player) {
    NC_DEBUG("Server", ">>> Sending Finish Configuration (0x03) to {}", player->getName());
    player->getConnection()->sendPacket(0x03, std::vector<u8>{});
    NC_DEBUG("Server", ">>> Finish Configuration sent OK");
}

// ============================================================
// Play — Join Game (1.21.1)
// ============================================================

void NetherCraftServer::sendJoinPlay(std::shared_ptr<entity::Player> player) {
    net::Buffer buf;

    // PLAYERSTATE_V2: only brand-new players take the configured default mode.
    if (!std::filesystem::exists("world/playerdata/" + player->getName() + ".txt"))
        player->gameMode = (config_.gamemode == "creative") ? 1 : (config_.gamemode == "adventure") ? 2 : (config_.gamemode == "spectator") ? 3 : 0;

    // Entity ID
    buf.writeI32(static_cast<i32>(player->getEntityId()));

    // Is Hardcore
    buf.writeBool(false);

    // World Names count + names
    buf.writeVarInt(1);
    buf.writeString("minecraft:overworld");

    // Max players
    buf.writeVarInt(config_.maxPlayers);

    // VIEWDIST_V1: реальный view distance из конфига (было 2 — отсюда туман в 2 чанках)
    buf.writeVarInt(config_.viewDistance);

    // Simulation distance
    buf.writeVarInt(config_.viewDistance);

    // Reduced debug info
    buf.writeBool(false);

    // Enable respawn screen
    buf.writeBool(false);

    // Do limited crafting
    buf.writeBool(false);

    // === CommonPlayerSpawnInfo ===

    // Dimension type: Holder via holderRegistry = VarInt(registry index)
    // Index 0 = overworld (order matches registry data we sent)
    buf.writeVarInt(0);

    // Dimension name
    buf.writeString("minecraft:overworld");

    // Hashed seed
    buf.writeI64(config_.levelSeed);

    // GameType: byte (i8) — из конфига игрока (GM_JOIN_V1)
    buf.writeByte((i8)player->gameMode);

    // PreviousGameType: byte (i8), -1=none
    buf.writeByte(-1);

    // Is debug
    buf.writeBool(false);

    // Is flat
    buf.writeBool(true);

    // Death location: optional (VarInt 0=absent)
    buf.writeVarInt(0);

    // Portal cooldown: VarInt (STANDALONE)
    buf.writeVarInt(0);

    // Enforces secure chat
    buf.writeBool(false);

    auto payload = std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end());

    // Hex dump Login packet
    std::string hex;
    for (size_t i = 0; i < payload.size(); ++i) {
        hex += std::format("{:02X} ", payload[i]);
    }
    (void)hex; // LOGQUIET_V1

    player->getConnection()->sendPacket(0x2B, payload);
}

// ============================================================
// Play — Spawn Position
// ============================================================

// ============================================================
// BLOCKS_V1: в��помогательные функции для блоков
// ============================================================
static void decodeBlockPos(u64 v, i32& x, i32& y, i32& z) {
    x = static_cast<i32>(static_cast<i64>(v) >> 38);
    y = static_cast<i32>((static_cast<i64>(v) << 52) >> 52);
    z = static_cast<i32>((static_cast<i64>(v) << 26) >> 38);
}

static i64 g_timeOfDay = 6000; // CMDS_V1: текущее время мира (для /time и Set Time)

// OPS_V1: проверка оператора (ops= в settings.properties, через запятую, без учёта регистра)
static bool isOpName(const std::string& opsCsv, const std::string& name) {
    if (opsCsv.empty()) return false;
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) lower.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(c))));
    size_t pos = 0;
    while (pos <= opsCsv.size()) {
        size_t comma = opsCsv.find(',', pos);
        if (comma == std::string::npos) comma = opsCsv.size();
        std::string tok = opsCsv.substr(pos, comma - pos);
        size_t a = tok.find_first_not_of(" \t");
        size_t b = tok.find_last_not_of(" \t");
        if (a != std::string::npos && tok.substr(a, b - a + 1) == lower) return true;
        pos = comma + 1;
    }
    return false;
}

// BLOCKS_V2: ванильный item id -> block state id (см. item_blocks.gen.hpp)
static i32 itemToBlockState(i32 itemId) {
    return gen::itemToBlockState(itemId);
}

// CHEST_V1: ключ позиции блока для карты контейнеров сундуков.
static u64 chestPosKey(i32 x, i32 y, i32 z) {
    return ((static_cast<u64>(static_cast<u32>(x)) & 0x3FFFFFFULL) << 38) |
           ((static_cast<u64>(static_cast<u32>(z)) & 0x3FFFFFFULL) << 12) |
           (static_cast<u64>(static_cast<u32>(y)) & 0xFFFULL);
}

// CHEST_V1: диапазоны block state сундуков 1.21.1.
static bool isChestBlockState(i32 st)   { return st >= 2954 && st <= 2977; } // chest
static bool isTrappedChestState(i32 st) { return st >= 9119 && st <= 9142; } // trapped_chest
static bool isEnderChestState(i32 st)   { return st >= 7513 && st <= 7520; } // ender_chest

// CHEST_V1: facing-блоки ставим «лицом к игроку» по yaw.
// Формулы стейтов 1.21.1: chest/trapped_chest = base + facing*6 (type=single, waterlogged=false);
// ender_chest/furnace = base + facing*2. Порядок facing: north=0, south=1, west=2, east=3.
static i32 orientBlockForPlacement(i32 defState, f32 yaw) {
    f32 y = std::fmod(yaw, 360.0f);
    if (y < 0.0f) y += 360.0f;
    const i32 dir = static_cast<i32>(std::floor(y / 90.0f + 0.5f)) & 3; // куда смотрит игрок: 0=юг 1=зап��д 2=север 3=восток
    static const i32 facingIdx[4] = {0, 3, 1, 2}; // направление игрока -> facing блока (навстречу игроку)
    const i32 f = facingIdx[dir];
    switch (defState) {
        case 2955: return 2955 + f * 6; // chest
        case 9120: return 9120 + f * 6; // trapped_chest
        case 7514: return 7514 + f * 2; // ender_chest
        case 4295: return 4295 + f * 2; // furnace
        default:   return defState;
    }
}

// CHEST_V2: обратное преобразование ключа позиции (см. chestPosKey).
static void decodeChestPosKey(u64 key, i32& x, i32& y, i32& z) {
    x = static_cast<i32>(static_cast<i64>(key) >> 38);
    z = static_cast<i32>(static_cast<i64>(key) << 26 >> 38);
    y = static_cast<i32>(static_cast<i64>(key) << 52 >> 52);
}

// CHEST_V2: реестровые id блоков 1.21.1 для Block Action 0x08 (клиент сверяет id с блоком в мире).
static i32 blockRegistryIdForState(i32 st) {
    if (isChestBlockState(st))   return 177; // minecraft:chest
    if (isEnderChestState(st))   return 344; // minecraft:ender_chest
    if (isTrappedChestState(st)) return 411; // minecraft:trapped_chest
    return 0;
}

// CHEST_V2: смещение «по часовой стрелке» от facing сундука (north=0,south=1,west=2,east=3).
// У LEFT-��оловины двойного сундука партнёр стоит по часовой, у RIGHT — против.
static void chestCwOffset(i32 f, i32& dx, i32& dz) {
    switch (f) {
        case 0:  dx = 1;  dz = 0;  break; // north -> east
        case 1:  dx = -1; dz = 0;  break; // south -> west
        case 2:  dx = 0;  dz = -1; break; // west -> north
        default: dx = 0;  dz = 1;  break; // east -> south
    }
}

// COMBAT_V2: базовый урон и скорость а��аки по item id 1.21.1.
// Инструменты занимают непрерывный блок id 818..847 (проверено по registries 1.21.1):
// {wooden,stone,golden,iron,diamond,netherite} x {sword,shovel,pickaxe,axe,hoe}
// ARMOR_V1: очки брони и toughness за предмет (id брони 1.21.1: порядок шлем/нагрудник/штаны/боты).
static void armorStats(i32 itemId, f32& armor, f32& toughness) {
    static const f32 kLeather[4]   = {1, 3, 2, 1};
    static const f32 kChain[4]     = {2, 5, 4, 1};
    static const f32 kIron[4]      = {2, 6, 5, 2};
    static const f32 kDiamond[4]   = {3, 8, 6, 3};
    static const f32 kGold[4]      = {2, 5, 3, 1};
    static const f32 kNetherite[4] = {3, 8, 6, 3};
    if (itemId >= 856 && itemId <= 859) armor += kLeather[itemId - 856];        // leather 856-859
    else if (itemId >= 860 && itemId <= 863) armor += kChain[itemId - 860];     // chainmail 860-863
    else if (itemId >= 864 && itemId <= 867) armor += kIron[itemId - 864];      // iron 864-867
    else if (itemId >= 868 && itemId <= 871) { armor += kDiamond[itemId - 868]; toughness += 2.0f; } // diamond 868-871
    else if (itemId >= 872 && itemId <= 875) armor += kGold[itemId - 872];      // golden 872-875
    else if (itemId >= 876 && itemId <= 879) { armor += kNetherite[itemId - 876]; toughness += 3.0f; } // netherite 876-879
}

static void weaponStats(i32 itemId, f32& damage, f32& attackSpeed) {
    damage = 1.0f;      // кулак
    attackSpeed = 4.0f; // кулак — почти без кулдауна
    if (itemId < 818 || itemId > 847) return;
    const i32 tier = (itemId - 818) / 5; // 0=wood 1=stone 2=gold 3=iron 4=diamond 5=netherite
    const i32 kind = (itemId - 818) % 5; // 0=sword 1=shovel 2=pickaxe 3=axe 4=hoe
    static const f32 kSwordDmg[6]  = {4.0f, 5.0f, 4.0f, 6.0f, 7.0f, 8.0f};
    static const f32 kShovelDmg[6] = {2.5f, 3.5f, 2.5f, 4.5f, 5.5f, 6.5f};
    static const f32 kPickDmg[6]   = {2.0f, 3.0f, 2.0f, 4.0f, 5.0f, 6.0f};
    static const f32 kAxeDmg[6]    = {7.0f, 9.0f, 7.0f, 9.0f, 9.0f, 10.0f};
    static const f32 kAxeSpd[6]    = {0.8f, 0.8f, 1.0f, 0.9f, 1.0f, 1.0f};
    static const f32 kHoeSpd[6]    = {1.0f, 2.0f, 1.0f, 3.0f, 4.0f, 4.0f};
    switch (kind) {
        case 0:  damage = kSwordDmg[tier];  attackSpeed = 1.6f; break;
        case 1:  damage = kShovelDmg[tier]; attackSpeed = 1.0f; break;
        case 2:  damage = kPickDmg[tier];   attackSpeed = 1.2f; break;
        case 3:  damage = kAxeDmg[tier];    attackSpeed = kAxeSpd[tier]; break;
        default: damage = 1.0f;             attackSpeed = kHoeSpd[tier]; break;
    }
}

// EQUIP_V1: обратный маппинг blockState -> itemId (для Set Equipment 0x5b)
static i32 stateToItem(i32 blockState) {
    static const std::unordered_map<i32, i32> rev = [] {
        std::unordered_map<i32, i32> m;
        for (const auto& kv : gen::itemStateMap()) m.emplace(kv.second, kv.first);
        return m;
    }();
    if (blockState < 0) return -1;
    auto it = rev.find(blockState);
    return it == rev.end() ? -1 : it->second;
}

// EQUIP_V1: собрать тело пакета Set Equipment (0x5b) для основной руки
static void buildEquipmentPacket(net::Buffer& eq, i32 eid, i32 heldState) {
    eq.writeVarInt(eid);
    eq.writeByte(0x00); // слот 0 = MAINHAND, старший бит снят -> единстве��ная запись
    const i32 itemId = stateToItem(heldState);
    if (itemId > 0) {
        eq.writeVarInt(1);      // Item Count
        eq.writeVarInt(itemId); // Item ID
        eq.writeVarInt(0);      // компонентов добавить
        eq.writeVarInt(0);      // компон��нтов удалит��
    } else {
        eq.writeVarInt(0);      // пустой слот (рука пуста)
    }
}

// PLAYER_VIS_V1: полный набор экип��ровки (рука, оффхенд, вся броня) из инвентаря игрока
static void buildFullEquipmentPacket(net::Buffer& eq, i32 eid, const entity::Player& p) {
    eq.writeVarInt(eid);
    const int mainInv = (p.heldSlot >= 0 && p.heldSlot < 9) ? (36 + p.heldSlot) : 36;
    const int invSlots[6] = { mainInv, 45, 8, 7, 6, 5 }; // рука, оффхенд, ботинки, поножи, нагрудник, шлем
    const int ordinals[6] = { 0, 1, 2, 3, 4, 5 };        // MAINHAND,OFFHAND,FEET,LEGS,CHEST,HEAD
    for (int i = 0; i < 6; ++i) {
        const bool more = (i != 5);
        eq.writeByte(static_cast<u8>(more ? (ordinals[i] | 0x80) : ordinals[i]));
        const int s = invSlots[i];
        const i32 id  = (s >= 0 && s < entity::Player::INV_SIZE) ? p.invItemId[s] : 0;
        const i32 cnt = (s >= 0 && s < entity::Player::INV_SIZE) ? p.invCount[s]  : 0;
        if (id > 0 && cnt > 0) {
            eq.writeVarInt(cnt);
            eq.writeVarInt(id);
            eq.writeVarInt(0);
            eq.writeVarInt(0);
        } else {
            eq.writeVarInt(0);
        }
    }
}

// PLAYER_VIS_V1: метаданные сущно��ти игрока (общие флаги + поза) для приседа/спринта
// SKIN_V1: + displayed skin parts (index 17) so capes/layers render on the body.
static void buildEntityMeta(net::Buffer& m, i32 eid, bool sneaking, bool sprinting, u8 skinParts) {
    m.writeVarInt(eid);
    u8 flags = 0;
    if (sneaking)  flags |= 0x02; // присед
    if (sprinting) flags |= 0x08; // спринт
    m.writeByte(0x00);            // индекс 0: общий флаг-байт
    m.writeVarInt(0);             // сериализатор BYTE
    m.writeByte(flags);
    m.writeByte(0x06);            // индекс 6: поза
    m.writeVarInt(21);            // сериализатор POSE
    m.writeVarInt(sneaking ? 5 : 0); // CROUCHING=5, STANDING=0
    m.writeByte(0x11);            // индекс 17: displayed skin parts (SKIN_V1)
    m.writeVarInt(0);             // сериализатор BYTE
    m.writeByte(skinParts);       // маска слоёв (плащ/куртка/рукава/штанины/шляпа)
    m.writeByte(0xFF);            // конец метаданных
}

void NetherCraftServer::sendSpawnPosition(std::shared_ptr<entity::Player> player) {
    net::Buffer buf;
    buf.writePosition(BlockPos{g_spawnX, g_spawnY, g_spawnZ}); // SPAWN_V1: общий мировой спавн
    buf.writeF32(0.0f); // angle = f32, НЕ u8!
    player->getConnection()->sendPacket(0x56, std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
}

// ============================================================
// Play — Player Abilities
// ============================================================

void NetherCraftServer::sendPlayerAbilities(std::shared_ptr<entity::Player> player) {
    net::Buffer buf;
    const int gm = player->gameMode; // GM_JOIN_V1: флаги сп��собностей по режиму
    buf.writeByte((u8)(gm == 1 ? (0x01 | 0x04 | 0x08) : (gm == 3 ? (0x01 | 0x02 | 0x04) : 0x00))); // GM3_FIX: наблюдатель=неуязвимость+полёт (без instabuild), креатив=неуязвимость
    buf.writeF32(0.05f);
    buf.writeF32(0.1f);
    player->getConnection()->sendPacket(0x38, std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
}

// ============================================================
// Play — Position and Look
// ============================================================

void NetherCraftServer::sendPlayerPositionAndLook(std::shared_ptr<entity::Player> player) {
    net::Buffer buf;
    buf.writeF64(player->getX());
    buf.writeF64(player->getY());
    buf.writeF64(player->getZ());
    buf.writeF32(player->get_yaw()); // PLAYERSTATE_V2: restore camera yaw
    buf.writeF32(player->get_pitch()); // PLAYERSTATE_V2: restore camera pitch
    buf.writeByte(0x00); // Flags
    buf.writeVarInt(1);  // Teleport ID
    player->getConnection()->sendPacket(0x40, std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
}

// ============================================================
// Play — Time Update
// ============================================================

void NetherCraftServer::sendTimeUpdate(std::shared_ptr<entity::Player> player) {
    net::Buffer buf;
    buf.writeI64(g_timeOfDay); // CMDS_V1
    buf.writeI64(g_timeOfDay);
    player->getConnection()->sendPacket(0x64, std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
}

// ============================================================
// Play — Send Chunks
// ============================================================

void NetherCraftServer::sendChunksAround(std::shared_ptr<entity::Player> player, i32 centerX, i32 centerZ, i32 radius, i32 maxChunks) { // CLIENT_BATCH_V1
    auto tChunks0 = std::chrono::steady_clock::now(); // VIEWDIST_V1
    // PERF_CHUNK_V1: build a center-out spiral directly. The old implementation
    // allocated every missing coordinate and sorted up to (2R+1)^2 entries per batch.
    std::vector<std::pair<i32, i32>> coords;
    coords.reserve(static_cast<size_t>(2 * radius + 1) * static_cast<size_t>(2 * radius + 1));
    auto appendIfMissing = [&](i32 cx, i32 cz) {
        if (!player->hasSeenChunk(cx, cz)) coords.emplace_back(cx, cz);
    };
    appendIfMissing(centerX, centerZ);
    for (i32 ring = 1; ring <= radius; ++ring) {
        const i32 minX = centerX - ring, maxX = centerX + ring;
        const i32 minZ = centerZ - ring, maxZ = centerZ + ring;
        for (i32 x = minX; x <= maxX; ++x) appendIfMissing(x, minZ);
        for (i32 z = minZ + 1; z <= maxZ; ++z) appendIfMissing(maxX, z);
        for (i32 x = maxX - 1; x >= minX; --x) appendIfMissing(x, maxZ);
        for (i32 z = maxZ - 1; z > minZ; --z) appendIfMissing(minX, z);
    }
    if (coords.empty()) return; // нечего слать - не спамим пустыми пачками
    if (maxChunks < 1) maxChunks = 1;

    // Chunk Batch Start (0x0D, empty)
    player->getConnection()->sendPacket(0x0D, std::vector<u8>{});

    i32 batchSize = 0;
    i32 syncBudget = 3; // PERF_ASYNC_V1: max blocking generations per batch (guarantees forward progress)
    for (const auto& cc : coords) {
        const i32 cx = cc.first;
        const i32 cz = cc.second; {
            if (batchSize >= maxChunks) { world_.requestChunkAsync(cx, cz); continue; } // PERF_ASYNC_V1: prefetch rest of view into pool // CLIENT_BATCH_V1: остальное - следующей пачкой
            // PERF_ASYNC_V1: prefer a live chunk, then one the background pool finished.
            auto chunk = world_.getChunk(cx, cz);
            if (!chunk) chunk = world_.takeReadyChunk(cx, cz);
            if (!chunk) {
                if (world_.isChunkPending(cx, cz)) continue; // generating in background - send it next batch
                if (syncBudget > 0) {
                    chunk = world_.getOrGenerateChunk(cx, cz); // FLATWORLD_V1: bounded blocking so player always sees ground
                    --syncBudget;
                } else {
                    world_.requestChunkAsync(cx, cz); // hand off to pool; retried on next Chunk Batch Received
                    continue;
                }
            }
            if (!chunk) continue;

            net::Buffer packetPayload;
            packetPayload.writeI32(cx);
            packetPayload.writeI32(cz);

            // Heightmaps (anonymousNbt — TAG_Compound with NO root name)
            {
                nbt::TagWriter heightmaps;
                heightmaps.beginRootCompound();
                // LIGHT_V1 + FLATVANILLA_V1: флэт-трава теперь на Y=-61 -> heightmap -61+1-(-64) = 4;
                // для DEFAULT-генератора оставляем прежнюю константу 68
                const u64 hmVal = (config_.generator == "DEFAULT" || config_.generator == "default") ? 68u : 4u;
                std::vector<i64> hm(37, 0);
                {
                    u64 packedHm = 0;
                    for (int hi = 0; hi < 7; ++hi) packedHm |= static_cast<u64>(hmVal) << (9 * hi);
                    for (auto& hl : hm) hl = static_cast<i64>(packedHm);
                }
                heightmaps.writeLongArray(hm, "MOTION_BLOCKING");
                heightmaps.writeLongArray(hm, "WORLD_SURFACE");
                heightmaps.endCompound();
                auto hmapNbt = heightmaps.toVector();
                packetPayload.writeBytes(std::span<const u8>(hmapNbt));
            }

            // Chunk sections data (length-prefixed)
            net::Buffer chunkBuf;
            chunk->writeTo(chunkBuf);
            packetPayload.writeVarInt(static_cast<i32>(chunkBuf.writtenBytes()));
            packetPayload.writeBytes(chunkBuf.writtenSpan());

            // Block entities count (empty)
            packetPayload.writeVarInt(0);

            // LIGHT_V1: полный дневной свет (skylight 15) во всех 26 секциях (24 + 2 граничные)
            packetPayload.writeVarInt(1);      // sky light mask: 1 long
            packetPayload.writeI64(0x3FFFFFF); // биты 0..25 = все секции
            packetPayload.writeVarInt(0);      // block light mask: пусто
            packetPayload.writeVarInt(0);      // empty sky light mask: пусто
            packetPayload.writeVarInt(1);      // empty block light mask: 1 long
            packetPayload.writeI64(0x3FFFFFF); // block light = 0 везде
            packetPayload.writeVarInt(26);     // 26 массивов skylight
            {
                static const std::vector<u8> fullLight(2048, 0xFF); // 4096 значений по 15
                for (int li = 0; li < 26; ++li) {
                    packetPayload.writeVarInt(2048);
                    packetPayload.writeBytes(std::span<const u8>(fullLight));
                }
            }
            packetPayload.writeVarInt(0);      // block light arrays: пусто

            player->getConnection()->sendPacket(0x27,
                std::vector<u8>(packetPayload.writtenSpan().begin(), packetPayload.writtenSpan().end()));
            player->markChunkSeen(cx, cz); // LIGHT_V1
            batchSize++;
        }
    }

    // Chunk Batch Finished (0x0C, VarInt batchSize)
    {
        net::Buffer batchBuf;
        batchBuf.writeVarInt(batchSize);
        player->getConnection()->sendPacket(0x0C,
            std::vector<u8>(batchBuf.writtenSpan().begin(), batchBuf.writtenSpan().end()));
    }

    player->setViewCenter(centerX, centerZ);
    if (batchSize > 0) {
        auto chunkMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tChunks0).count();
        (void)batchSize; (void)chunkMs; // LOGQUIET_V1
    }

    // Send KeepAlive immediately after chunks
    sendKeepAlive(player);
}

// ============================================================
// Play — Keep Alive
// ============================================================

void NetherCraftServer::sendKeepAlive(std::shared_ptr<entity::Player> player) {
    auto now = std::chrono::steady_clock::now();
    i64 id = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    net::Buffer buf;
    buf.writeI64(id);
    player->getConnection()->sendPacket(0x26, std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
    pendingKeepAlives_.push_back(id);
}

// ============================================================
// Play — Обработка пакетов от клиента
// ============================================================

// ============================================================
// MP_V1: Мультиплеер — видимость и синхронизация игроков между собой
// ============================================================

// Ванильный entity-type id игрока �� протоколе 1.21.1 (поле type в Spawn Entity 0x01).
// ВНИМАНИЕ: если чужой игрок отрисуется как ДРУГОЙ моб — поправь это число
// (значение protocol_id для minecraft:player из реестр�� entity_type 1.21.1).
static constexpr i32 MP_PLAYER_ENTITY_TYPE = 128;

void NetherCraftServer::broadcastToOthers(const std::shared_ptr<entity::Player>& except, i32 packetId, const std::vector<u8>& payload) {
    auto all = getAllPlayersCopy();
    for (auto& p : all) {
        if (p.get() == except.get()) continue;
        if (p->getState() != entity::PlayerState::Play) continue; // не слать Play-пакеты тем, кто ещё в Configuration
        if (!p->isAlive()) continue;
        p->getConnection()->sendPacket(packetId, payload);
    }
}

// PLAYER_VIS_V2: спектатора (GM3) видят только другие спектаторы; обычным игрокам его сущность не спавним (как в ванилле)
static bool isSpectatorHiddenFrom(const entity::Player& target, const entity::Player& viewer) {
    return target.gameMode == 3 && viewer.gameMode != 3;
}

void NetherCraftServer::spawnPlayerFor(const std::shared_ptr<entity::Player>& viewer, const std::shared_ptr<entity::Player>& target) {
    if (!viewer || !target || viewer.get() == target.get()) return;
    if (!viewer->isAlive() || viewer->getState() != entity::PlayerState::Play) return;

    const i32 eid = static_cast<i32>(target->getEntityId());
    const Angle yaw = Angle::fromDegrees(target->get_yaw());
    const Angle pitch = Angle::fromDegrees(target->get_pitch());

    // 1) Player Info Update (0x3E) — добавить в tab list (иначе сущность-игрок не отр��суется и будет без ника)
    {
        net::Buffer info;
        info.writeVarInt(0x1D); // MP_DUP_V1 fix: ADD_PLAYER | UPDATE_GAME_MODE | UPDATE_LISTED | UPDATE_LATENCY (было 0x0D -> лишний байт latency -> краш клиента)
        info.writeVarInt(1);    // count
        info.writeUUID(target->getUuid());
        info.writeString(target->getName());
        writeProfileProperties(info, *target); // SKIN_V1: skin + cape (online & offline)
        info.writeVarInt(target->gameMode);
        info.writeVarInt(1);    // listed = true
        info.writeVarInt(0);    // latency
        viewer->getConnection()->sendPacket(0x3E, std::vector<u8>(info.writtenSpan().begin(), info.writtenSpan().end()));
    }

    // PLAYER_VIS_V2: спектатор остаётся �� таб-листе, но сущность-игрока не спавним не-спектаторам (как в ванилле)
    if (isSpectatorHiddenFrom(*target, *viewer)) return;

    // 2) Spawn Entity (0x01) — type = игрок
    {
        net::Buffer sp;
        sp.writeVarInt(eid);
        sp.writeUUID(target->getUuid());
        sp.writeVarInt(MP_PLAYER_ENTITY_TYPE);
        sp.writeF64(target->getX());
        sp.writeF64(target->getY());
        sp.writeF64(target->getZ());
        sp.writeByte(pitch.value); // Pitch (Angle)
        sp.writeByte(yaw.value);   // Yaw (Angle)
        sp.writeByte(yaw.value);   // Head Yaw (Angle)
        sp.writeVarInt(0);         // Data
        sp.writeI16(0);            // Velocity X
        sp.writeI16(0);            // Velocity Y
        sp.writeI16(0);            // Velocity Z
        viewer->getConnection()->sendPacket(0x01, std::vector<u8>(sp.writtenSpan().begin(), sp.writtenSpan().end()));
    }

    // 3) Entity Head Rotation (0x48)
    {
        net::Buffer hr;
        hr.writeVarInt(eid);
        hr.writeByte(yaw.value);
        viewer->getConnection()->sendPacket(0x48, std::vector<u8>(hr.writtenSpan().begin(), hr.writtenSpan().end()));
    }

    // EQUIP_V1: показать зрител�� предмет в руке target
    sendPlayerEquipment(viewer, target);

    // PLAYER_VIS_V1: синхронизир��вать позу/присед/спринт target для зрителя
    {
        net::Buffer m;
        buildEntityMeta(m, static_cast<i32>(target->getEntityId()), target->sneaking, target->sprinting, target->displayedSkinParts);
        viewer->getConnection()->sendPacket(0x58, std::vector<u8>(m.writtenSpan().begin(), m.writtenSpan().end()));
    }
}

// PLAYER_VIS_V2: убрать сущность target у конкретного зрителя (в таб-листе игрок остаётся)
void NetherCraftServer::despawnPlayerFor(const std::shared_ptr<entity::Player>& viewer, const std::shared_ptr<entity::Player>& target) {
    if (!viewer || !target || viewer.get() == target.get()) return;
    if (!viewer->isAlive() || viewer->getState() != entity::PlayerState::Play) return;
    net::Buffer b;
    b.writeVarInt(1);
    b.writeVarInt(static_cast<i32>(target->getEntityId()));
    viewer->getConnection()->sendPacket(0x42, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()));
}

// PLAYER_VIS_V2: при смене спект��торства пересчитать видимость сущности в обе стороны
void NetherCraftServer::refreshSpectatorVisibility(const std::shared_ptr<entity::Player>& player, bool wasSpectator, bool isSpectator) {
    if (wasSpectator == isSpectator) return; // спектаторство не менялось — ничего не делаем
    auto all = getAllPlayersCopy();
    for (auto& other : all) {
        if (!other || other.get() == player.get()) continue;
        if (!other->isAlive() || other->getState() != entity::PlayerState::Play) continue;
        if (other->gameMode != 3) {
            // player как ЦЕЛЬ для обычного зрителя other
            if (isSpectator) despawnPlayerFor(other, player); // стал спектатором -> скрыть от в��живания
            else             spawnPlayerFor(other, player);   // вышел из спектатора -> показать
        } else {
            // player как ЗРИТЕЛЬ для спектатора other (спектаторы видят всех)
            if (isSpectator) spawnPlayerFor(player, other);   // стал спектатором -> увидеть других спектаторов
            else             despawnPlayerFor(player, other); // вышел -> скрыть чужих спектаторов
        }
    }
}

// EQUIP_V1: отправить одному зрителю текущий предмет в руке target
void NetherCraftServer::sendPlayerEquipment(const std::shared_ptr<entity::Player>& viewer, const std::shared_ptr<entity::Player>& target) {
    if (!viewer || !target) return;
    if (!viewer->isAlive() || viewer->getState() != entity::PlayerState::Play) return;
    net::Buffer eq;
    buildFullEquipmentPacket(eq, static_cast<i32>(target->getEntityId()), *target);
    viewer->getConnection()->sendPacket(0x5b, std::vector<u8>(eq.writtenSpan().begin(), eq.writtenSpan().end()));
}

// EQUIP_V1: разослать остальным изменившийся предмет в руке игрока
void NetherCraftServer::broadcastHeldEquipment(const std::shared_ptr<entity::Player>& player) {
    if (!player) return;
    net::Buffer eq;
    buildFullEquipmentPacket(eq, static_cast<i32>(player->getEntityId()), *player);
    broadcastToOthers(player, 0x5b, std::vector<u8>(eq.writtenSpan().begin(), eq.writtenSpan().end()));
}

// PLAYER_VIS_V1: разослать метаданные (присед/спринт/поза) остальным игрокам
void NetherCraftServer::broadcastEntityMeta(const std::shared_ptr<entity::Player>& player) {
    if (!player) return;
    net::Buffer m;
    buildEntityMeta(m, static_cast<i32>(player->getEntityId()), player->sneaking, player->sprinting, player->displayedSkinParts);
    broadcastToOthers(player, 0x58, std::vector<u8>(m.writtenSpan().begin(), m.writtenSpan().end()));
}

void NetherCraftServer::onPlayerEnterPlay(const std::shared_ptr<entity::Player>& player) {
    if (!player) return;
    // Синхронизировать точку отсчёта delta-move с текущей позицией
    player->mpLastX = player->getX();
    player->mpLastY = player->getY();
    player->mpLastZ = player->getZ();

    auto all = getAllPlayersCopy();
    for (auto& other : all) {
        if (other.get() == player.get()) continue;
        if (other->getState() != entity::PlayerState::Play) continue;
        if (!other->isAlive()) continue;
        spawnPlayerFor(player, other); // показать уже находящегося в игре — новичку
        spawnPlayerFor(other, player); // показать новичка — тому, кто уже в игре
    }
    // SELFSKIN_V1: отправить игроку метаданные ЕГО СОБСТВЕННОЙ сущности (index 17).
    // Клиент рендерит свою модель в F5 по synched-metadata, а не по локальным опциям,
    // поэтому без это��о пакета плащ/шапка/слои на себе не видны (в одиночке их синкает встроенный сервер).
    {
        net::Buffer sm;
        buildEntityMeta(sm, static_cast<i32>(player->getEntityId()), player->sneaking, player->sprinting, player->displayedSkinParts);
        player->getConnection()->sendPacket(0x58, std::vector<u8>(sm.writtenSpan().begin(), sm.writtenSpan().end()));
    }
    // ENTITIES_V1: показать новичку уже заспавненные не-игроковые сущности
    {
        std::vector<SpawnedEntity> copy;
        { std::lock_guard lk(entitiesMutex_); copy = entities_; }
        for (auto& e : copy) {
            net::Buffer sp;
            sp.writeVarInt(e.eid);
            sp.writeUUID(UUID{0, static_cast<u64>(e.eid)});
            sp.writeVarInt(e.typeId);
            sp.writeF64(e.x); sp.writeF64(e.y); sp.writeF64(e.z);
            sp.writeByte(0); sp.writeByte(0); sp.writeByte(0);
            sp.writeVarInt(0);
            sp.writeI16(0); sp.writeI16(0); sp.writeI16(0);
            player->getConnection()->sendPacket(0x01, std::vector<u8>(sp.writtenSpan().begin(), sp.writtenSpan().end()));
        }
    }
    // WEATHER_SYNC_V1: новичок получает то же состояние погоды, что и остальные.
    sendWeatherState(player);
    NC_DEBUG("Server", "MP_V1: {} viden ostalnym igrokam", player->getName());
}

void NetherCraftServer::applyEnvironmentalDamage(const std::shared_ptr<entity::Player>& player,
                                                   f32 damage, i32 damageTypeId,
                                                   const std::string& deathMessage) {
    if (!player || !player->isAlive() || player->dead || damage <= 0.0f) return;
    if (player->gameMode == 1 || player->gameMode == 3) return;

    player->health = std::max(0.0f, player->health - damage);
    const i32 eid = static_cast<i32>(player->getEntityId());

    net::Buffer damageEvent;
    damageEvent.writeVarInt(eid);
    damageEvent.writeVarInt(damageTypeId);
    damageEvent.writeVarInt(0); // source cause absent
    damageEvent.writeVarInt(0); // direct source absent
    damageEvent.writeBool(false);
    auto damageBytes = std::vector<u8>(damageEvent.writtenSpan().begin(), damageEvent.writtenSpan().end());
    for (auto& target : getAllPlayersCopy()) {
        if (target && target->isAlive() && target->getState() == entity::PlayerState::Play)
            target->getConnection()->sendPacket(0x1A, damageBytes);
    }

    net::Buffer health;
    health.writeF32(player->health);
    health.writeVarInt(20);
    health.writeF32(5.0f);
    player->getConnection()->sendPacket(0x5D,
        std::vector<u8>(health.writtenSpan().begin(), health.writtenSpan().end()));

    if (player->health <= 0.0f) {
        player->dead = true;
        net::Buffer death;
        death.writeVarInt(eid);
        writeTextComponent(death, deathMessage);
        player->getConnection()->sendPacket(0x3C,
            std::vector<u8>(death.writtenSpan().begin(), death.writtenSpan().end()));
        for (auto& target : getAllPlayersCopy())
            if (target && target->isAlive()) target->sendSystemMessage(std::string("§7") + deathMessage);
    }
}

void NetherCraftServer::applyFallDamage(const std::shared_ptr<entity::Player>& player,
                                         f64 newY, bool newOnGround) {
    if (!player) return;
    // EGG_V1: пасхалка глубоко в пустоте (Y < -1000), срабатывает один раз за погружение
    if (newY < -1000.0 && !player->eggDeepTold) {
        player->eggDeepTold = true;
        const bool eggRu = (config_.language == "rus");
        player->sendSystemMessage(eggRu ? "§5§oТы слышишь шёпот из пустоты..." : "§5§oYou hear whispers from the void...");
        player->sendSystemMessage(eggRu ? "§8Z E V V O R Y N   §5В И Д И Т   Т Е Б Я" : "§8Z E V V O R Y N   §5S E E S   Y O U");
        broadcastBlockSound("minecraft:ambient.cave", static_cast<i32>(player->getX()),
            static_cast<i32>(newY), static_cast<i32>(player->getZ()), 1.0f, 0.5f);
    } else if (newY > -900.0 && player->eggDeepTold) {
        player->eggDeepTold = false; // поднялся выше — пасхалка перезаряжена
    }
    if (player->gameMode == 1 || player->gameMode == 3 || player->dead) {
        player->fallPeakY = newY;
        player->fallWasOnGround = newOnGround;
        return;
    }

    if (newOnGround) {
        if (!player->fallWasOnGround) {
            const f64 fallDistance = player->fallPeakY - newY;
            const i32 damage = static_cast<i32>(std::floor(fallDistance - 3.0));
            if (damage > 0) {
                applyEnvironmentalDamage(player, static_cast<f32>(damage), 9,
                    std::format("{} разбился при падении", player->getName()));
            }
        }
        player->fallPeakY = newY;
    } else if (player->fallWasOnGround || newY > player->fallPeakY) {
        player->fallPeakY = newY;
    }
    player->fallWasOnGround = newOnGround;
}

void NetherCraftServer::broadcastPlayerMovement(const std::shared_ptr<entity::Player>& player, bool posChanged, bool rotChanged) {
    if (!player || player->getState() != entity::PlayerState::Play) return;
    const i32 eid = static_cast<i32>(player->getEntityId());
    const Angle yaw = Angle::fromDegrees(player->get_yaw());
    const Angle pitch = Angle::fromDegrees(player->get_pitch());
    const bool onGround = player->isOnGround();

    const f64 dx = player->getX() - player->mpLastX;
    const f64 dy = player->getY() - player->mpLastY;
    const f64 dz = player->getZ() - player->mpLastZ;
    const bool bigJump = std::fabs(dx) >= 7.9 || std::fabs(dy) >= 7.9 || std::fabs(dz) >= 7.9;

    if (posChanged && bigJump) {
        // Teleport Entity (0x70) — абсолютная позиция, когда delta не влезает в short
        net::Buffer b;
        b.writeVarInt(eid);
        b.writeF64(player->getX());
        b.writeF64(player->getY());
        b.writeF64(player->getZ());
        b.writeByte(yaw.value);
        b.writeByte(pitch.value);
        b.writeBool(onGround);
        broadcastToOthers(player, 0x70, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()));
        player->mpLastX = player->getX();
        player->mpLastY = player->getY();
        player->mpLastZ = player->getZ();
    } else if (posChanged && rotChanged) {
        // Update Entity Position and Rotation (0x2F)
        const i16 ddx = static_cast<i16>(dx * 4096.0);
        const i16 ddy = static_cast<i16>(dy * 4096.0);
        const i16 ddz = static_cast<i16>(dz * 4096.0);
        net::Buffer b;
        b.writeVarInt(eid);
        b.writeI16(ddx);
        b.writeI16(ddy);
        b.writeI16(ddz);
        b.writeByte(yaw.value);
        b.writeByte(pitch.value);
        b.writeBool(onGround);
        broadcastToOthers(player, 0x2F, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()));
        player->mpLastX += static_cast<f64>(ddx) / 4096.0;
        player->mpLastY += static_cast<f64>(ddy) / 4096.0;
        player->mpLastZ += static_cast<f64>(ddz) / 4096.0;
    } else if (posChanged) {
        // Update Entity Position (0x2E)
        const i16 ddx = static_cast<i16>(dx * 4096.0);
        const i16 ddy = static_cast<i16>(dy * 4096.0);
        const i16 ddz = static_cast<i16>(dz * 4096.0);
        net::Buffer b;
        b.writeVarInt(eid);
        b.writeI16(ddx);
        b.writeI16(ddy);
        b.writeI16(ddz);
        b.writeBool(onGround);
        broadcastToOthers(player, 0x2E, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()));
        player->mpLastX += static_cast<f64>(ddx) / 4096.0;
        player->mpLastY += static_cast<f64>(ddy) / 4096.0;
        player->mpLastZ += static_cast<f64>(ddz) / 4096.0;
    } else if (rotChanged) {
        // Update Entity Rotation (0x30)
        net::Buffer b;
        b.writeVarInt(eid);
        b.writeByte(yaw.value);
        b.writeByte(pitch.value);
        b.writeBool(onGround);
        broadcastToOthers(player, 0x30, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()));
    }

    if (rotChanged) {
        // Entity Head Rotation (0x48)
        net::Buffer h;
        h.writeVarInt(eid);
        h.writeByte(yaw.value);
        broadcastToOthers(player, 0x48, std::vector<u8>(h.writtenSpan().begin(), h.writtenSpan().end()));
    }
}

void NetherCraftServer::broadcastPlayerRemove(const std::shared_ptr<entity::Player>& player) {
    if (!player) return;
    const i32 eid = static_cast<i32>(player->getEntityId());
    // Remove Entities (0x42)
    {
        net::Buffer b;
        b.writeVarInt(1);
        b.writeVarInt(eid);
        broadcastToOthers(player, 0x42, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()));
    }
    // Player Info Remove (0x3D)
    {
        net::Buffer b;
        b.writeVarInt(1);
        b.writeUUID(player->getUuid());
        broadcastToOthers(player, 0x3D, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()));
    }
}

void NetherCraftServer::handlePlay(std::shared_ptr<entity::Player> player, net::Buffer& data, i32 wireId) {
    // FLATWORLD_V1: убран лог-спам Play packet
    switch (wireId) {
        case 0x00: { // ESSENTIALS_V1: Teleport Confirm — клиент подтвердил телепорт
            (void)data.readVarInt(); // teleportId
            break;
        }
        case 0x03: { // ESSENTIALS_V1: Message Acknowledgement — читаем счётчик подтверждённых сообщений
            (void)data.readVarInt(); // count
            break;
        }
        case 0x09: { // ESSENTIALS_V1 + COMBAT_V1: Client Command (0 = respawn, 1 = request stats)
            i32 actionId = data.readVarInt();
            if (actionId == 0) {
                // COMBAT_V1: полноценный респавн после смерти — Respawn (0x47) + сброс здоровья.
                player->health = 20.0f;
                player->dead = false;
                player->fallPeakY = static_cast<f64>(g_spawnY);
                player->fallWasOnGround = true;
                {
                    net::Buffer rs; // CommonPlayerSpawnInfo + data kept
                    rs.writeVarInt(0);                      // dimension type (holder index 0 = overworld)
                    rs.writeString("minecraft:overworld"); // dimension name
                    rs.writeI64(config_.levelSeed);         // hashed seed
                    rs.writeByte((i8)player->gameMode);     // game mode
                    rs.writeByte(-1);                       // previous game mode
                    rs.writeBool(false);                    // is debug
                    rs.writeBool(true);                     // is flat
                    rs.writeBool(false);                    // has death location
                    rs.writeVarInt(0);                      // portal cooldown
                    rs.writeByte(0x03);                     // data kept: attributes + metadata
                    player->getConnection()->sendPacket(0x47, std::vector<u8>(rs.writtenSpan().begin(), rs.writtenSpan().end()));
                }
                { // RESPAWN_V2: Game Event 13 «start waiting for level chunks» — без него клиент висит на «Loading terrain…» до таймаута
                    net::Buffer ge;
                    ge.writeByte(13);
                    ge.writeF32(0.0f);
                    player->getConnection()->sendPacket(0x22, std::vector<u8>(ge.writtenSpan().begin(), ge.writtenSpan().end()));
                }
                player->clearSeenChunks(); // RESPAWN_V2: клиент выбросил чанки после Respawn — отправим заново
                player->openWindowId = 0;  // RESPAWN_V2: окно контейнера на клиенте закрыто после смерти
                player->setPosition(g_spawnX + 0.5, (f64)g_spawnY, g_spawnZ + 0.5);
                sendPlayerPositionAndLook(player);
                { net::Buffer uh; uh.writeF32(20.0f); uh.writeVarInt(20); uh.writeF32(5.0f);
                  player->getConnection()->sendPacket(0x5D, std::vector<u8>(uh.writtenSpan().begin(), uh.writtenSpan().end())); }
                sendTimeUpdate(player); // TIMESYNC_V1: Respawn сбрасывает время суток у клиента — синхронизируем заново
                streamChunks(player);
            }
            break;
        }
        case 0x21: { // ESSENTIALS_V1: Ping Request (play) -> Ping Response (clientbound 0x36)
            i64 pingId = data.readI64();
            net::Buffer pr;
            pr.writeI64(pingId);
            player->getConnection()->sendPacket(0x36, std::vector<u8>(pr.writtenSpan().begin(), pr.writtenSpan().end()));
            break;
        }
        case 0x23: { // ESSENTIALS_V1: Player Abilities — клиент тогглит полёт (0x02 = is flying)
            u8 abilityFlags = data.readByte();
            (void)abilityFlags; // полёт в креативе клиент выполняет локально; читаем байт, чт��бы не сломать парсер
            break;
        }
        case 0x0A: { // SELFSKIN_V1: Client Information (play) — клиент сменил настройки (в т.ч. слои скина)
            data.readString();               // locale
            (void)data.readByte();           // view distance (i8)
            (void)data.readVarInt();         // chat flags
            (void)data.readBool();           // chat colors
            player->displayedSkinParts = static_cast<u8>(data.readByte()); // маска слоёв (плащ/куртка/рукава/штанин��/шляпа)
            (void)data.readVarInt();         // main hand
            (void)data.readBool();           // text filtering
            (void)data.readBool();           // server listing
            // обновить свою модель (self) + разослать остальным
            {
                net::Buffer sm;
                buildEntityMeta(sm, static_cast<i32>(player->getEntityId()), player->sneaking, player->sprinting, player->displayedSkinParts);
                player->getConnection()->sendPacket(0x58, std::vector<u8>(sm.writtenSpan().begin(), sm.writtenSpan().end()));
            }
            broadcastEntityMeta(player);
            break;
        }
        case 0x16: { // COMBAT_V1: Use Entity — PvP-удар / интеракт по сущности
            i32 targetEid = data.readVarInt();
            i32 interactType = data.readVarInt(); // 0=interact, 1=attack, 2=interact_at
            if (interactType == 2) { (void)data.readF32(); (void)data.readF32(); (void)data.readF32(); }
            if (interactType == 0 || interactType == 2) { (void)data.readVarInt(); } // рука
            (void)data.readBool(); // sneaking
            if (interactType != 1) break;      // обрабатываем только атаку
            if (player->gameMode == 3) break;  // спектатор не бьёт
            std::shared_ptr<entity::Player> victim;
            for (auto& p : getAllPlayersCopy()) {
                if (p && p.get() != player.get() && static_cast<i32>(p->getEntityId()) == targetEid) { victim = p; break; }
            }
            if (!victim) {
                // ENTHIT_V1: раньше здесь сразу был break, поэтому /summon-сущности
                // визуально появлялись, но сервер никогда не обрабатывал удар по ним.
                bool removed = false;
                {
                    std::lock_guard lk(entitiesMutex_);
                    auto it = std::find_if(entities_.begin(), entities_.end(),
                        [&](const SpawnedEntity& entity) { return entity.eid == targetEid; });
                    if (it != entities_.end()) {
                        entities_.erase(it);
                        removed = true;
                    }
                }
                if (removed) {
                    net::Buffer hurt;
                    hurt.writeVarInt(targetEid);
                    hurt.writeF32(0.0f);
                    net::Buffer remove;
                    remove.writeVarInt(1);
                    remove.writeVarInt(targetEid);
                    auto hurtBytes = std::vector<u8>(hurt.writtenSpan().begin(), hurt.writtenSpan().end());
                    auto removeBytes = std::vector<u8>(remove.writtenSpan().begin(), remove.writtenSpan().end());
                    for (auto& target : getAllPlayersCopy()) {
                        if (!target || !target->isAlive() || target->getState() != entity::PlayerState::Play) continue;
                        target->getConnection()->sendPacket(0x24, hurtBytes);
                        target->getConnection()->sendPacket(0x42, removeBytes);
                    }
                }
                break;
            }
            if (!victim->isAlive() || victim->getState() != entity::PlayerState::Play) break;
            if (victim->dead) break;
            if (victim->gameMode == 1 || victim->gameMode == 3) break; // креатив/спектатор неуязвимы
            const i32 vEid = static_cast<i32>(victim->getEntityId());
            // COMBAT_V2: урон зависит от предмета в руке + ванильный кулдаун атаки
            f32 baseDmg = 1.0f, atkSpeed = 4.0f;
            const i32 heldItem = (player->heldSlot < 9) ? player->invItemId[36 + player->heldSlot] : 0;
            weaponStats(heldItem, baseDmg, atkSpeed);
            const i64 nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            f32 charge = static_cast<f32>(nowMs - player->lastAttackMs) * atkSpeed / 1000.0f;
            if (charge > 1.0f) charge = 1.0f;
            if (charge < 0.0f) charge = 0.0f;
            player->lastAttackMs = nowMs;
            // IFRAME_V1: ванильные i-frames — 0.5 с неуязвимости после полученного урона.
            // Закликивание больше не работает: чаще 2 раз/с урон не проходит.
            if (nowMs - victim->lastHurtMs < 500) break;
            f32 dmg = baseDmg * (0.2f + 0.8f * charge * charge); // недозаряженный удар слабее (как в ваниле)
            // CRIT_V1: крит как в ваниле — атакующий падает (не на земле, ниже пика падения), не спринтует, удар заряжен
            const bool crit = !player->isOnGround() && player->getY() < player->fallPeakY && !player->sprinting && charge > 0.848f;
            if (crit) dmg *= 1.5f;
            // SHIELD_V1: поднятый щит блокирует удар из передней полусферы
            bool blocked = false;
            if (victim->usingShield && dmg > 0.0f) {
                const f64 adx = player->getX() - victim->getX();
                const f64 adz = player->getZ() - victim->getZ();
                const f64 vyawR = static_cast<f64>(victim->get_yaw()) * 3.14159265358979323846 / 180.0;
                const f64 lookX = -std::sin(vyawR), lookZ = std::cos(vyawR); // куда смотрит жертва
                const f64 alen = std::sqrt(adx * adx + adz * adz);
                if (alen > 1e-4 && (adx / alen) * lookX + (adz / alen) * lookZ > 0.0) {
                    blocked = true;
                    dmg = 0.0f;
                    broadcastBlockSound("minecraft:item.shield.block",
                        static_cast<i32>(std::floor(victim->getX())), static_cast<i32>(std::floor(victim->getY())),
                        static_cast<i32>(std::floor(victim->getZ())), 1.0f, 1.0f);
                }
            }
            // ARMOR_V1: броня жертвы уменьшает урон (ванильная формула; слоты 5-8 = шлем..боты)
            if (!blocked && dmg > 0.0f) {
                f32 armor = 0.0f, tough = 0.0f;
                for (int as = 5; as <= 8; ++as)
                    if (victim->invCount[as] > 0) armorStats(victim->invItemId[as], armor, tough);
                if (armor > 0.0f) {
                    f32 red = armor - dmg / (2.0f + tough / 4.0f);
                    if (red < armor / 5.0f) red = armor / 5.0f;
                    if (red > 20.0f) red = 20.0f;
                    dmg *= 1.0f - red / 25.0f;
                }
            }
            if (!blocked) victim->lastHurtMs = nowMs; // IFRAME_V1: заблокированный щитом удар i-frames не даёт
            victim->health -= dmg;
            if (victim->health < 0.0f) victim->health = 0.0f;
            const f64 ddx = victim->getX() - player->getX();
            const f64 ddz = victim->getZ() - player->getZ();
            const f32 hurtYaw = static_cast<f32>(std::atan2(ddz, ddx) * 180.0 / 3.14159265358979323846 - 90.0);
            auto sendAll = [&](i32 pid, const net::Buffer& b) {
                auto v = std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end());
                for (auto& p : getAllPlayersCopy())
                    if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                        p->getConnection()->sendPacket(pid, v);
            };
            if (!blocked) { net::Buffer h; h.writeVarInt(vEid); h.writeF32(hurtYaw); sendAll(0x24, h); } // Hurt Animation (SHIELD_V1: при блоке не мигаем)
            if (crit && !blocked) { // CRIT_V1: анимация крита (Entity Animation 0x03, anim 4)
                net::Buffer ca; ca.writeVarInt(vEid); ca.writeByte(4); sendAll(0x03, ca);
            }
            if (!blocked) { // SHIELD_V1: заблокированный удар — без damage event
                net::Buffer d; // Damage Event (0x1A)
                d.writeVarInt(vEid);
                d.writeVarInt(32); // player_attack в minecraft:damage_type
                d.writeVarInt(static_cast<i32>(player->getEntityId()) + 1); // sourceCauseId (+1: 0=нет)
                d.writeVarInt(static_cast<i32>(player->getEntityId()) + 1); // sourceDirectId
                d.writeBool(false); // без исходной позиции
                sendAll(0x1A, d);
            }
            {
                f64 len = std::sqrt(ddx * ddx + ddz * ddz); if (len < 1e-4) len = 1.0;
                const f64 kb = blocked ? 0.2 : 0.4; // SHIELD_V1: при блоке кнокбек слабее
                net::Buffer kbuf; // кнокбек (Entity Velocity 0x5A)
                kbuf.writeVarInt(vEid);
                kbuf.writeI16(static_cast<i16>((ddx / len) * kb * 8000.0));
                kbuf.writeI16(static_cast<i16>(0.36 * 8000.0));
                kbuf.writeI16(static_cast<i16>((ddz / len) * kb * 8000.0));
                sendAll(0x5A, kbuf);
            }
            if (victim->health <= 0.0f && !victim->dead) {
                victim->dead = true;
                { net::Buffer uh; uh.writeF32(0.0f); uh.writeVarInt(20); uh.writeF32(5.0f);
                  victim->getConnection()->sendPacket(0x5D, std::vector<u8>(uh.writtenSpan().begin(), uh.writtenSpan().end())); }
                { net::Buffer dc; dc.writeVarInt(vEid); // Death Combat Event (0x3C)
                  writeTextComponent(dc, std::format("{} был убит игроком {}", victim->getName(), player->getName()));
                  victim->getConnection()->sendPacket(0x3C, std::vector<u8>(dc.writtenSpan().begin(), dc.writtenSpan().end())); }
                for (auto& p : getAllPlayersCopy())
                    if (p && p->isAlive())
                        p->sendSystemMessage(std::format("§e{} §7был убит игроком §e{}", victim->getName(), player->getName()));
            } else {
                net::Buffer uh; uh.writeF32(victim->health); uh.writeVarInt(20); uh.writeF32(5.0f);
                victim->getConnection()->sendPacket(0x5D, std::vector<u8>(uh.writtenSpan().begin(), uh.writtenSpan().end()));
            }
            break;
        }
        case 0x08: { // Chunk Batch Received - CLIENT_BATCH_V1: по протоколу это FLOAT, не VarInt
            f32 desired = data.readF32(); // желаемое число чанков за тик
            // CHUNK_THROUGHPUT_V3: honor the client's pacing request for fast streaming.
            i32 budget = static_cast<i32>(desired * 6.0f); // PERF_ASYNC_V2: was *5
            if (budget < 8) budget = 8;    // PERF_ASYNC_V2: higher floor (was 4)
            if (budget > 64) budget = 64;  // PERF_ASYNC_V2: higher ceiling (was 24)
            i32 vcx = player->getViewCenterX();
            i32 vcz = player->getViewCenterZ();
            i32 vr = config_.viewDistance;
            if (vr < 2) vr = 2;
            sendChunksAround(player, vcx, vcz, vr, budget); // следующая пачка в темпе клиента
            break;
        }
        case 0x1A: { // Player Position
            f64 x = data.readF64();
            f64 y = data.readF64();
            f64 z = data.readF64();
            bool onGround = data.readBool();
            player->setPosition(x, y, z);
            player->setOnGround(onGround);
            applyFallDamage(player, y, onGround); // FALLDMG_V1
            broadcastPlayerMovement(player, true, false); // MP_V1: разослать движение остальным
            streamChunks(player); // FLATWORLD_V1: подгрузка чанков при движении
            break;
        }
        case 0x1B: { // Player Position And Rotation
            f64 x = data.readF64();
            f64 y = data.readF64();
            f64 z = data.readF64();
            f32 yaw = data.readF32();
            f32 pitch = data.readF32();
            bool onGround = data.readBool();
            player->setPosition(x, y, z);
            player->setRotation(yaw, pitch);
            player->setOnGround(onGround);
            applyFallDamage(player, y, onGround); // FALLDMG_V1
            broadcastPlayerMovement(player, true, true); // MP_V1: разослать движение+поворот остальным
            streamChunks(player); // FLATWORLD_V1: подгрузка чанков при движении
            break;
        }
        case 0x1C: { // Player Rotation
            f32 yaw = data.readF32();
            f32 pitch = data.readF32();
            bool onGround = data.readBool();
            player->setRotation(yaw, pitch);
            player->setOnGround(onGround);
            broadcastPlayerMovement(player, false, true); // MP_V1: разослать поворот остальным
            break;
        }
        case 0x1D: { // Player Movement (flying)
            bool onGround = data.readBool();
            player->setOnGround(onGround);
            applyFallDamage(player, player->getY(), onGround); // FALLDMG_V1
            break;
        }
        case 0x36: { // PLAYER_VIS_V1: Swing Arm -> анимация взмаха остальным
            i32 hand = data.readVarInt(); // 0 = основная рука, 1 = вторая
            net::Buffer an;
            an.writeVarInt(static_cast<i32>(player->getEntityId()));
            an.writeByte(static_cast<u8>(hand == 1 ? 3 : 0)); // 0=SWING_MAIN_HAND, 3=SWING_OFF_HAND
            broadcastToOthers(player, 0x03, std::vector<u8>(an.writtenSpan().begin(), an.writtenSpan().end()));
            break;
        }
        case 0x25: { // PLAYER_VIS_V1: Player Command (присед/спринт)
            (void)data.readVarInt(); // id сущности
            i32 action = data.readVarInt();
            (void)data.readVarInt(); // доп. данные
            switch (action) {
                case 0: player->sneaking = true;  break; // PRESS_SHIFT_KEY
                case 1: player->sneaking = false; break; // RELEASE_SHIFT_KEY
                case 3: player->sprinting = true; break; // START_SPRINTING
                case 4: player->sprinting = false; break; // STOP_SPRINTING
                default: break;
            }
            broadcastEntityMeta(player);
            break;
        }
        case 0x18: { // Keep Alive
            data.readI64(); // ID
            break;
        }
        case 0x06: { // Chat Message
            std::string message = data.readString();
            // Пропускаем timestamp, salt, signature, message count, ack
            // CHATLOG_V2: чат игроков вообще не логируем — ни в файл, ни в консоль (просьба владельца)

            // CHAT_V2: р��ссылка через System Chat (0x6C, NBT text) через sendSystemMessage.
            // Было: пакет 0x3F (= player_look_at) с JSON -> клиент падал на декоде.
            const std::string chatLine = std::format("<{}> {}", player->getName(), message);
            auto allPlayers = getAllPlayersCopy();
            for (auto& p : allPlayers) {
                if (p->isAlive() && p->getState() == entity::PlayerState::Play) {
                    p->sendSystemMessage(chatLine);
                }
            }
            break;
        }
        case 0x04: { // CMDS_V1: Serverbound Command
            std::string command = data.readString();
            // CMDLOG_QUIET_V1: не засоряем консоль каждой введённой командой.
            std::istringstream iss(command);
            std::vector<std::string> args;
            std::string tok;
            while (iss >> tok) args.push_back(tok);
            const std::string cmd = args.empty() ? std::string() : args[0];
            const bool op = config_.ops.empty() || isOpName(config_.ops, player->getName()); // OPS_V1: пока ops пуст — можно всем
            auto needOp = [&]() { player->sendSystemMessage("§cНужны права оператора (ops= в settings.properties)"); };
            auto broadcastBlock = [&](i32 sx, i32 sy, i32 sz, i32 st) {
                world_.setBlock(sx, sy, sz, st);
                net::Buffer bu;
                bu.writePosition(BlockPos{sx, sy, sz});
                bu.writeVarInt(st);
                auto vec = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
                auto allPlayers = getAllPlayersCopy();
                for (auto& p : allPlayers) if (p->isAlive()) p->getConnection()->sendPacket(0x09, vec);
            };

            if (cmd == "help") {
                player->sendSystemMessage("§6/help /tps /list /spawn /setworldspawn [x y z] /tp <x y z> /gamemode <режим|0-3> [ник]");
                player->sendSystemMessage("§6/gm0 /gm1 /gm2 /gm3 [ник] — быстрая смена режима"); // GMSHORT_V1
                player->sendSystemMessage("§6/time set <day|night|тики> /weather <clear|rain|thunder>");
                player->sendSystemMessage("§6/say <текст> /setblock <x y z блок> /kick <ник>");
                player->sendSystemMessage("§6/summon <pig|zombie|cow|sheep|creeper|skeleton|id> [кол-во] /killall");
            } else if (cmd == "tps") {
                // TPS_BOSS_V1: port of the Bedrock overlay toggle to Java Boss Event packets.
                player->tpsBossbarEnabled = !player->tpsBossbarEnabled;
                if (player->tpsBossbarEnabled) {
                    sendTpsBossbar(player, true);
                    player->sendSystemMessage("§aTPS bossbar включён");
                } else {
                    removeTpsBossbar(player);
                    player->sendSystemMessage("§eTPS bossbar выключен");
                }
            } else if (cmd == "list") {
                auto allPlayers = getAllPlayersCopy();
                std::string names;
                for (auto& p : allPlayers) { if (!names.empty()) names += ", "; names += p->getName(); }
                player->sendSystemMessage(std::format("§6Онлайн: {} — {}", allPlayers.size(), names));
            } else if (cmd == "spawn") {
                player->setPosition(g_spawnX + 0.5, (f64)g_spawnY, g_spawnZ + 0.5); // SPAWN_V1
                sendPlayerPositionAndLook(player);
                player->sendSystemMessage("§aТелепорт на с��авн");
            } else if (cmd == "setworldspawn") {
                if (!op) { needOp(); break; }
                i32 nx = g_spawnX, ny = g_spawnY, nz = g_spawnZ; bool okc = true;
                if (args.size() >= 4) {
                    try { nx = (i32)std::stol(args[1]); ny = (i32)std::stol(args[2]); nz = (i32)std::stol(args[3]); }
                    catch (...) { okc = false; }
                } else {
                    nx = (i32)std::floor(player->getX());
                    ny = (i32)std::floor(player->getY());
                    nz = (i32)std::floor(player->getZ());
                }
                if (!okc) { player->sendSystemMessage("§cКоординаты — целые числа"); break; }
                g_spawnX = nx; g_spawnY = ny; g_spawnZ = nz;
                writeWorldSpawn(g_spawnX, g_spawnY, g_spawnZ); // SPAWN_V1
                auto allSp = getAllPlayersCopy();
                for (auto& p : allSp) if (p->isAlive()) sendSpawnPosition(p);
                player->sendSystemMessage(std::format("§aМировой спавн установлен: {} {} {}", g_spawnX, g_spawnY, g_spawnZ));
            } else if (cmd == "tp") {
                if (!op) { needOp(); break; }
                if (args.size() >= 4) {
                    auto coord = [](const std::string& s, f64 cur) -> f64 {
                        if (!s.empty() && s[0] == '~') return s.size() > 1 ? cur + std::stod(s.substr(1)) : cur;
                        return std::stod(s);
                    };
                    try {
                        f64 nx = coord(args[1], player->getX());
                        f64 ny = coord(args[2], player->getY());
                        f64 nz = coord(args[3], player->getZ());
                        if (nx < -30000000.0 || nx > 30000000.0 || nz < -30000000.0 || nz > 30000000.0 || ny < -63.0 || ny > 319.0) {
                            player->sendSystemMessage(config_.language == "rus" ? "§cТП вне границ: X/Z ±30000000, Y -63..319" : "§cTeleport out of bounds: X/Z ±30000000, Y -63..319");
                        } else {
                        player->setPosition(nx, ny, nz);
                        sendPlayerPositionAndLook(player);
                        if (config_.language == "rus")
                            player->sendSystemMessage(std::format("§aТ��лепорт: {:.1f} {:.1f} {:.1f}", nx, ny, nz));
                        else
                            player->sendSystemMessage(std::format("§aTeleported: {:.1f} {:.1f} {:.1f}", nx, ny, nz));
                        }
                    } catch (...) { player->sendSystemMessage("§cКоординаты — числа или ~"); }
                } else player->sendSystemMessage("§cИспользование: /tp <x> <y> <z>");
            } else if (cmd == "gamemode" || cmd == "gm0" || cmd == "gm1" || cmd == "gm2" || cmd == "gm3") { // GMSHORT_V1: /gm0../gm3 + смена режима другому игроку
                if (!op) { needOp(); break; }
                i32 mode = -1;
                size_t targetArg = 1; // у /gmN ник — первый аргумент
                if (cmd != "gamemode") {
                    mode = cmd[2] - '0';
                } else {
                    targetArg = 2; // у /gamemode ник — второй аргумент
                    if (args.size() >= 2) {
                        const std::string& mm = args[1];
                        if (mm == "survival" || mm == "0") mode = 0;
                        else if (mm == "creative" || mm == "1") mode = 1;
                        else if (mm == "adventure" || mm == "2") mode = 2;
                        else if (mm == "spectator" || mm == "3") mode = 3;
                    }
                }
                if (mode < 0) { player->sendSystemMessage("§cИспользование: /gamemode <режим|0-3> [ник] или /gm0../gm3 [ник]"); break; }
                std::shared_ptr<entity::Player> target = player;
                if (args.size() > targetArg) { // GMSHORT_V1: цель — другой игрок
                    target = nullptr;
                    std::string want = args[targetArg];
                    for (auto& wc : want) wc = static_cast<char>(::tolower(static_cast<unsigned char>(wc)));
                    for (auto& p : getAllPlayersCopy()) {
                        if (!p || !p->isAlive() || p->getState() != entity::PlayerState::Play) continue;
                        std::string nm = p->getName();
                        for (auto& mc : nm) mc = static_cast<char>(::tolower(static_cast<unsigned char>(mc)));
                        if (nm == want) { target = p; break; }
                    }
                    if (!target) { player->sendSystemMessage(std::format("§cИгрок {} не найден", args[targetArg])); break; }
                }
                {
                    const i32 prevMode = target->gameMode; // PLAYER_VIS_V2
                    target->gameMode = mode; // GM_V1
                    net::Buffer ge;
                    ge.writeByte(3); // событие 3 — смена игрового режима
                    ge.writeF32(static_cast<f32>(mode));
                    target->getConnection()->sendPacket(0x22, std::vector<u8>(ge.writtenSpan().begin(), ge.writtenSpan().end()));
                    net::Buffer ab;
                    ab.writeByte(mode == 1 ? (0x01 | 0x04 | 0x08) : (mode == 3 ? (0x01 | 0x02 | 0x04) : 0x00)); // GM3_FIX
                    ab.writeF32(0.05f);
                    ab.writeF32(0.1f);
                    target->getConnection()->sendPacket(0x38, std::vector<u8>(ab.writtenSpan().begin(), ab.writtenSpan().end()));
                    { // GM3_FIX: обновляем режим в таб-листе (PlayerInfo UPDATE_GAME_MODE),
                      // иначе клиентский isSpectator() не сработает и ГМ3 не проходит сквозь блоки
                        net::Buffer gi;
                        gi.writeByte(0x04); // EnumSet из 6 действий -> 1 байт; бит 2 = UPDATE_GAME_MODE
                        gi.writeVarInt(1);  // 1 запись
                        gi.writeUUID(target->getUuid());
                        gi.writeVarInt(mode); // новый режим
                        auto giv = std::vector<u8>(gi.writtenSpan().begin(), gi.writtenSpan().end());
                        for (auto& pp : getAllPlayersCopy()) if (pp && pp->isAlive()) pp->getConnection()->sendPacket(0x3E, giv);
                    }
                    refreshSpectatorVisibility(target, prevMode == 3, mode == 3); // PLAYER_VIS_V2
                    static const char* kGmNames[4] = {"survival", "creative", "adventure", "spectator"};
                    target->sendSystemMessage(std::format("§aРежим игры: {}", kGmNames[mode]));
                    if (target != player) player->sendSystemMessage(std::format("§aРежим игры {} установлен для {}", kGmNames[mode], target->getName())); // GMSHORT_V1
                }
            } else if (cmd == "time") {
                if (!op) { needOp(); break; }
                i64 t = -1;
                if (args.size() >= 3 && args[1] == "set") {
                    const std::string& v = args[2];
                    if (v == "day") t = 1000;
                    else if (v == "noon") t = 6000;
                    else if (v == "night") t = 13000;
                    else if (v == "midnight") t = 18000;
                    else { try { t = std::stoll(v); } catch (...) { t = -1; } }
                }
                if (t < 0) player->sendSystemMessage("§cИспользование: /time set <day|noon|night|midnight|тики>");
                else {
                    g_timeOfDay = t;
                    net::Buffer tb;
                    tb.writeI64(t);
                    tb.writeI64(t);
                    auto vec = std::vector<u8>(tb.writtenSpan().begin(), tb.writtenSpan().end());
                    auto allPlayers = getAllPlayersCopy();
                    for (auto& p : allPlayers) if (p->isAlive()) p->getConnection()->sendPacket(0x64, vec);
                    player->sendSystemMessage(std::format("§aВремя: {}", t));
                }
            } else if (cmd == "weather") {
                if (!op) { needOp(); break; }
                const std::string w = args.size() >= 2 ? args[1] : std::string();
                if (w != "clear" && w != "rain" && w != "thunder") player->sendSystemMessage("§cИспользование: /weather <clear|rain|thunder>");
                else {
                    // WEATHER_SYNC_V1: состояние хранится сервером и одинаково рассылается всем.
                    g_weather = (w == "clear") ? 0 : ((w == "thunder") ? 2 : 1);
                    for (auto& target : getAllPlayersCopy()) sendWeatherState(target);
                    player->sendSystemMessage("§aПогода изменена");
                }
            } else if (cmd == "say") {
                size_t sp = command.find(' ');
                std::string msg = sp == std::string::npos ? std::string() : command.substr(sp + 1);
                if (msg.empty()) player->sendSystemMessage("§cИспользование: /say <текст>");
                else {
                    auto allPlayers = getAllPlayersCopy();
                    for (auto& p : allPlayers) if (p->isAlive()) p->sendSystemMessage(std::format("§d[{}] {}", player->getName(), msg));
                }
            } else if (cmd == "setblock") {
                if (!op) { needOp(); break; }
                if (args.size() >= 5) {
                    try {
                        i32 sx = static_cast<i32>(std::stol(args[1]));
                        i32 sy = static_cast<i32>(std::stol(args[2]));
                        i32 sz = static_cast<i32>(std::stol(args[3]));
                        i32 st = gen::blockNameToState(args[4]);
                        if (st < 0) player->sendSystemMessage("§cНеизвестный блок (полный список — после tools/gen_item_blocks.py)");
                        else if (sy < world::CHUNK_HEIGHT_MIN || sy >= world::CHUNK_HEIGHT_MAX) player->sendSystemMessage("§cY вне границ мира (-64..319)"); // HEIGHT_V2
                        else {
                            broadcastBlock(sx, sy, sz, st);
                            const i32 up2 = gen::doorUpperState(st); // DOORS_V1
                            if (up2 >= 0 && sy + 1 < world::CHUNK_HEIGHT_MAX) broadcastBlock(sx, sy + 1, sz, up2); // HEIGHT_V2
                            player->sendSystemMessage("§aБлок установлен");
                        }
                    } catch (...) { player->sendSystemMessage("§cКоординаты — целые числа"); }
                } else player->sendSystemMessage("§cИспользование: /setblock <x> <y> <z> <блок>");
            } else if (cmd == "kick") {
                if (!op) { needOp(); break; }
                if (args.size() >= 2) {
                    bool found = false;
                    auto allPlayers = getAllPlayersCopy();
                    for (auto& p : allPlayers) {
                        if (p->getName() == args[1]) {
                            p->sendSystemMessage("§cВы были кикнуты с сервера");
                            p->getConnection()->close();
                            found = true;
                        }
                    }
                    player->sendSystemMessage(found ? "§aИгрок кикнут" : "§cИгрок не найден");
                } else player->sendSystemMessage("§cИспользование: /kick <ник>");
            } else if (cmd == "summon") { // ENTITIES_V1: заспавнить не-игроковую сущность
                if (!op) { needOp(); break; }
                i32 typeId = -1;
                if (args.size() >= 2) {
                    static const std::unordered_map<std::string, i32> entityTypes = {
                        {"pig", 10}, {"cow", 9}, {"sheep", 12}, {"zombie", 54},
                        {"skeleton", 51}, {"creeper", 50}
                    };
                    auto named = entityTypes.find(args[1]);
                    if (named != entityTypes.end()) typeId = named->second;
                    else { try { typeId = static_cast<i32>(std::stol(args[1])); } catch (...) {} }
                }
                i32 count = 1;
                if (args.size() >= 3) { try { count = static_cast<i32>(std::stol(args[2])); } catch (...) {} }
                count = std::clamp(count, 1, 50);
                if (typeId < 0) {
                    player->sendSystemMessage("§cИспользование: /summon <pig|zombie|cow|sheep|creeper|skeleton|id> [кол-во]");
                    break;
                }
                const f64 baseX = player->getX(), baseY = player->getY(), baseZ = player->getZ();
                for (i32 n = 0; n < count; ++n) {
                    const f64 sx = baseX + static_cast<f64>((n % 5) - 2);
                    const f64 sz = baseZ + static_cast<f64>((n / 5) - 2);
                    i32 eid = static_cast<i32>(nextEntityId_++);
                    { std::lock_guard lk(entitiesMutex_); entities_.push_back({eid, typeId, sx, baseY, sz}); }
                    net::Buffer spawn;
                    spawn.writeVarInt(eid);
                    spawn.writeUUID(UUID{static_cast<u64>(eid), 0xE0000000ULL + static_cast<u64>(eid)});
                    spawn.writeVarInt(typeId);
                    spawn.writeF64(sx); spawn.writeF64(baseY); spawn.writeF64(sz);
                    spawn.writeByte(0); spawn.writeByte(0); spawn.writeByte(0);
                    spawn.writeVarInt(0);
                    spawn.writeI16(0); spawn.writeI16(0); spawn.writeI16(0);
                    auto bytes = std::vector<u8>(spawn.writtenSpan().begin(), spawn.writtenSpan().end());
                    for (auto& target : getAllPlayersCopy())
                        if (target && target->isAlive() && target->getState() == entity::PlayerState::Play)
                            target->getConnection()->sendPacket(0x01, bytes);
                }
                player->sendSystemMessage(std::format("§aЗаспавнено сущностей: {} (тип {})", count, typeId));
            } else if (cmd == "killall") { // ENTITIES_V1: удалить все заспавненные сущности
                if (!op) { needOp(); break; }
                std::vector<SpawnedEntity> copy;
                { std::lock_guard lk(entitiesMutex_); copy = entities_; entities_.clear(); }
                if (!copy.empty()) {
                    net::Buffer rm; rm.writeVarInt(static_cast<i32>(copy.size()));
                    for (auto& e : copy) rm.writeVarInt(e.eid);
                    auto v = std::vector<u8>(rm.writtenSpan().begin(), rm.writtenSpan().end());
                    for (auto& p : getAllPlayersCopy()) if (p && p->isAlive() && p->getState() == entity::PlayerState::Play) p->getConnection()->sendPacket(0x42, v);
                }
                player->sendSystemMessage(std::format("§aУдалено сущностей: {}", copy.size()));
            } else {
                player->sendSystemMessage("§cНеизвестная команда — /help");
            }
            break;
        }
        case 0x24: { // BLOCKS_V1: Player Action (копание/ломание)
            i32 status = data.readVarInt();
            u64 posRaw = data.readU64();
            (void)data.readByte(); // face
            i32 seq = data.readVarInt();
            i32 bx, by, bz;
            decodeBlockPos(posRaw, bx, by, bz);
            if (status == 5 && player->usingShield) { // SHIELD_V1: Release Use Item — щит опущен
                player->usingShield = false;
                broadcastHandState(player);
            }
            const bool creative = (player->gameMode == 1); // GM_V1: по-игроковому, а не из конфига
            const bool wantsBreak = player->gameMode != 3 && ((creative && status == 0) || (!creative && status == 2));
            if (wantsBreak && by >= world::CHUNK_HEIGHT_MIN && by < world::CHUNK_HEIGHT_MAX) { // HEIGHT_V2: vanilla world is -64..319
                const i32 ax = bx < 0 ? -bx : bx;
                const i32 az = bz < 0 ? -bz : bz;
                const bool prot = config_.spawnProtection > 0 && ax <= config_.spawnProtection && az <= config_.spawnProtection && !isOpName(config_.ops, player->getName()); // OPS_V1
                if (prot) {
                    net::Buffer bu;
                    bu.writePosition(BlockPos{bx, by, bz});
                    bu.writeVarInt(world_.getBlock(bx, by, bz)); // откатываем на клиенте
                    player->getConnection()->sendPacket(0x09, std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end()));
                    player->sendSystemMessage("§cСпавн ��ащищён — здесь ломать нельзя");
                } else if (by == world::CHUNK_HEIGHT_MIN && world_.getBlock(bx, by, bz) == 79) {
                    // BEDROCK_PROTECT_V1: exact lower border cannot be mined in survival or creative.
                    net::Buffer bu;
                    bu.writePosition(BlockPos{bx, by, bz});
                    bu.writeVarInt(79);
                    player->getConnection()->sendPacket(0x09, std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end()));
                    player->sendSystemMessage(config_.language == "rus" ? "§cНижний бедрок на высоте -64 нельзя сло��ать" : "§cThe bottom bedrock at Y=-64 cannot be broken");
                } else {
                    const i32 oldState = world_.getBlock(bx, by, bz); // BREAKFX_V1
                    world_.setBlock(bx, by, bz, 0);
                    net::Buffer bu;
                    bu.writePosition(BlockPos{bx, by, bz});
                    bu.writeVarInt(0);
                    auto vec = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
                    auto allPlayers = getAllPlayersCopy();
                    for (auto& p : allPlayers) if (p->isAlive()) p->getConnection()->sendPacket(0x09, vec);
                    if (oldState > 0) { // BREAKFX_V1: частицы + звук ломания (Level Event 2001)
                        net::Buffer fx;
                        fx.writeI32(2001);
                        fx.writePosition(BlockPos{bx, by, bz});
                        fx.writeI32(oldState);
                        fx.writeBool(false);
                        auto fxv = std::vector<u8>(fx.writtenSpan().begin(), fx.writtenSpan().end());
                        for (auto& p : allPlayers) if (p->isAlive()) p->getConnection()->sendPacket(0x28, fxv);
                    }
                    if (isChestBlockState(oldState) || isTrappedChestState(oldState) || isEnderChestState(oldState)) { // CHEST_V1/CHEST_V2: ��ундук сломан — чистим контейнер и закрываем окна зрителей (в т.ч. эндер-сундук)
                        const u64 key = chestPosKey(bx, by, bz);
                        const bool wasEnder = isEnderChestState(oldState);
                        if (!wasEnder) { std::lock_guard lock(chestsMutex_); chests_.erase(key); }
                        for (auto& p : allPlayers) {
                            if (p->isAlive() && p->openWindowId != 0 && p->openIsEnder == wasEnder && (p->openContainerKey == key || (p->openIsDouble && p->openContainerKey2 == key))) { // CHEST_V3: закрываем и зрителей второй половины
                                net::Buffer cw;
                                cw.writeByte((u8)p->openWindowId);
                                p->getConnection()->sendPacket(0x12, std::vector<u8>(cw.writtenSpan().begin(), cw.writtenSpan().end())); // clientbound Close Container
                                p->openWindowId = 0;
                                p->openIsDouble = false; p->openContainerKey2 = 0; // CHEST_V3
                                p->cursorItemId = 0; p->cursorCount = 0;
                            }
                        }
                        if (!wasEnder) { // CHEST_V2: если сломана половина ��войного сундука — партнёр снова одиночный
                            const i32 base = isChestBlockState(oldState) ? 2954 : 9119;
                            const i32 f = (oldState - base) / 6;
                            const i32 t = ((oldState - base) % 6) / 2; // 0=single, 1=left, 2=right
                            if (t != 0) {
                                i32 dx = 0, dz = 0;
                                chestCwOffset(f, dx, dz);
                                if (t == 2) { dx = -dx; dz = -dz; } // у правой половины партнёр против часовой
                                const i32 nx = bx + dx, nz = bz + dz;
                                const i32 nSt = world_.getBlock(nx, by, nz);
                                const i32 nBase = isChestBlockState(nSt) ? 2954 : (isTrappedChestState(nSt) ? 9119 : -1);
                                if (nBase == base && (nSt - nBase) / 6 == f) {
                                    const i32 single = nBase + f * 6 + 1; // type=single, не waterlogged
                                    world_.setBlock(nx, by, nz, single);
                                    net::Buffer nb;
                                    nb.writePosition(BlockPos{nx, by, nz});
                                    nb.writeVarInt(single);
                                    auto nv = std::vector<u8>(nb.writtenSpan().begin(), nb.writtenSpan().end());
                                    for (auto& p : allPlayers) if (p->isAlive()) p->getConnection()->sendPacket(0x09, nv);
                                }
                            }
                        }
                    }
                }
            }
            {
                net::Buffer ab;
                ab.writeVarInt(seq);
                player->getConnection()->sendPacket(0x05, std::vector<u8>(ab.writtenSpan().begin(), ab.writtenSpan().end()));
            }
            break;
        }
        case 0x2F: { // BLOCKS_V1: Set Carried Item (выбранный слот хотбара)
            i16 slot = data.readI16();
            if (slot >= 0 && slot < 9) player->heldSlot = slot;
            if (player->usingShield && player->usingShieldHand == 0) { player->usingShield = false; broadcastHandState(player); } // SHIELD_V1: смена слота опускает щит
            broadcastHeldEquipment(player); // EQUIP_V1: новый предмет в руке -> остальным
            break;
        }
        case 0x32: { // BLOCKS_V1: Set Creative Mode Slot
            i16 slot = data.readI16();
            i32 count = data.readVarInt();
            i32 state = -1;
            i32 itemId = 0;
            if (count > 0) {
                itemId = data.readVarInt();
                state = itemToBlockState(itemId);
            }
            // INVENTORY_V3: пишем ВЕСЬ инвентарь (рюкзак+хотбар+броня+оффхенд — все 46 слотов окна игрока)
            if (slot >= 0 && slot < entity::Player::INV_SIZE) {
                player->invItemId[slot] = (count > 0) ? itemId : 0;
                player->invCount[slot]  = (count > 0) ? count : 0;
            }
            if (slot >= 36 && slot <= 44) player->hotbarBlockState[slot - 36] = state;
            broadcastHeldEquipment(player); // EQUIP_V1: содержимое хотбара изменилось -> остальным
            break; // хвост с компонентами предмета игнорируем
        }
        case 0x0E: { // INVENTORY_CLICK_V1: Window Click — базовая работа с курсором в окне игрока
            u8 windowId = data.readByte();
            (void)data.readVarInt();   // state id
            i16 slot = data.readI16();
            u8 button = data.readByte();
            i32 mode = data.readVarInt();
            // хвост (changedSlots + cursorItem) игнорируем — у каждого пакета свой буфер;
            // сервер авторитетно пересинкает инвентарь ниже.
            // CHEST_V1: клики в открытом окне сундука (generic_9x3):
            // слоты 0..26 — контейнер, 27..62 — инвентарь игрока (слоты окна игрока 9..44).
            if (player->openWindowId != 0 && windowId == (u8)player->openWindowId) {
                const int CONT = player->openIsDouble ? 54 : 27; // CHEST_V3: двойной сундук = 54 слота контейнера
                {
                    std::lock_guard lock(chestsMutex_);
                    if (!player->openIsEnder) { // CHEST_V3: создаём оба контейнера заранее, чтобы указатели не инвалидировались
                        (void)chests_[player->openContainerKey];
                        if (player->openIsDouble) (void)chests_[player->openContainerKey2];
                    }
                    ChestData* chest = player->openIsEnder ? nullptr : &chests_[player->openContainerKey];
                    ChestData* chest2 = (!player->openIsEnder && player->openIsDouble) ? &chests_[player->openContainerKey2] : nullptr; // CHEST_V3
                    auto idRef = [&](int s) -> i32& {
                        if (s < 27) return player->openIsEnder ? player->enderItemId[s] : chest->itemId[s];
                        if (s < CONT) return chest2->itemId[s - 27]; // CHEST_V3: вторая половина
                        return player->invItemId[s - CONT + 9];
                    };
                    auto cntRef = [&](int s) -> i32& {
                        if (s < 27) return player->openIsEnder ? player->enderCount[s] : chest->count[s];
                        if (s < CONT) return chest2->count[s - 27]; // CHEST_V3
                        return player->invCount[s - CONT + 9];
                    };
                    if (slot >= 0 && slot < CONT + 36) {
                        i32& sId = idRef(slot); i32& sCnt = cntRef(slot);
                        i32& cId = player->cursorItemId; i32& cCnt = player->cursorCount;
                        if (mode == 0 && button == 0) { // левый клик — весь стак
                            if (cCnt == 0 && sCnt > 0) { cId = sId; cCnt = sCnt; sId = 0; sCnt = 0; }
                            else if (cCnt > 0 && sCnt == 0) { sId = cId; sCnt = cCnt; cId = 0; cCnt = 0; }
                            else if (cCnt > 0 && sCnt > 0) {
                                if (sId == cId) { i32 room = 64 - sCnt; i32 mv = cCnt < room ? cCnt : room; sCnt += mv; cCnt -= mv; if (cCnt == 0) cId = 0; }
                                else { i32 ti = sId, tc = sCnt; sId = cId; sCnt = cCnt; cId = ti; cCnt = tc; }
                            }
                        } else if (mode == 0 && button == 1) { // правый клик — половина / по одному
                            if (cCnt == 0 && sCnt > 0) { i32 half = (sCnt + 1) / 2; cId = sId; cCnt = half; sCnt -= half; if (sCnt == 0) sId = 0; }
                            else if (cCnt > 0 && (sCnt == 0 || sId == cId)) { if (sCnt == 0) sId = cId; if (sCnt < 64) { sCnt++; cCnt--; if (cCnt == 0) cId = 0; } }
                            else if (cCnt > 0 && sCnt > 0 && sId != cId) { i32 ti = sId, tc = sCnt; sId = cId; sCnt = cCnt; cId = ti; cCnt = tc; }
                        } else if (mode == 1 && sCnt > 0) { // shift-клик — быстрый перенос контейнер <-> инвентарь
                            const int dstBegin = slot < CONT ? CONT : 0;
                            const int dstEnd   = slot < CONT ? CONT + 36 : CONT;
                            for (int d = dstBegin; d < dstEnd && sCnt > 0; ++d) { // сначала докладываем в существующие стаки
                                if (cntRef(d) > 0 && idRef(d) == sId && cntRef(d) < 64) {
                                    i32 room = 64 - cntRef(d); i32 mv = sCnt < room ? sCnt : room;
                                    cntRef(d) += mv; sCnt -= mv;
                                }
                            }
                            for (int d = dstBegin; d < dstEnd && sCnt > 0; ++d) { // затем в пустые слоты
                                if (cntRef(d) == 0) { idRef(d) = sId; cntRef(d) = sCnt; sCnt = 0; }
                            }
                            if (sCnt == 0) sId = 0;
                        }
                    }
                }
                // хотбар мог измениться — обновляем блок-стейты и снаряжение
                for (int i = 0; i < 9; ++i) {
                    const i32 hid = player->invItemId[36 + i]; const i32 hcnt = player->invCount[36 + i];
                    player->hotbarBlockState[i] = (hcnt > 0 && hid > 0) ? itemToBlockState(hid) : -1;
                }
                broadcastHeldEquipment(player);
                sendContainerContent(player); // авторитетная пересинхронизация окна
                if (!player->openIsEnder) { // другие зрители того же сундука
                    for (auto& p : getAllPlayersCopy())
                        if (p != player && p->isAlive() && p->openWindowId != 0 && !p->openIsEnder && p->openContainerKey == player->openContainerKey)
                            sendContainerContent(p);
                }
                break;
            }
            const bool creative = (player->gameMode == 1);
            auto resync = [&]() {
                net::Buffer inv;
                inv.writeByte((i8)windowId);
                inv.writeVarInt(1);
                inv.writeVarInt(entity::Player::INV_SIZE);
                for (int i = 0; i < entity::Player::INV_SIZE; ++i) {
                    const i32 id = player->invItemId[i];
                    const i32 cnt = player->invCount[i];
                    if (cnt > 0 && id > 0) { inv.writeVarInt(cnt); inv.writeVarInt(id); inv.writeVarInt(0); inv.writeVarInt(0); }
                    else { inv.writeVarInt(0); }
                }
                if (player->cursorCount > 0 && player->cursorItemId > 0) { inv.writeVarInt(player->cursorCount); inv.writeVarInt(player->cursorItemId); inv.writeVarInt(0); inv.writeVarInt(0); }
                else inv.writeVarInt(0);
                player->getConnection()->sendPacket(0x13, std::vector<u8>(inv.writtenSpan().begin(), inv.writtenSpan().end()));
            };
            // В креативе окно управляется Set Creative Slot; реальную логику курсора применяем
            // только к окну игрок�� (windowId 0) в выживании, режим normal-click (mode 0).
            if (!creative && windowId == 0 && mode == 0 && slot >= 0 && slot < entity::Player::INV_SIZE) {
                i32& sId = player->invItemId[slot]; i32& sCnt = player->invCount[slot];
                i32& cId = player->cursorItemId;   i32& cCnt = player->cursorCount;
                if (button == 0) { // левый клик — весь стак
                    if (cCnt == 0 && sCnt > 0) { cId = sId; cCnt = sCnt; sId = 0; sCnt = 0; }
                    else if (cCnt > 0 && sCnt == 0) { sId = cId; sCnt = cCnt; cId = 0; cCnt = 0; }
                    else if (cCnt > 0 && sCnt > 0) {
                        if (sId == cId) { i32 room = 64 - sCnt; i32 mv = cCnt < room ? cCnt : room; sCnt += mv; cCnt -= mv; if (cCnt == 0) cId = 0; }
                        else { i32 ti = sId, tc = sCnt; sId = cId; sCnt = cCnt; cId = ti; cCnt = tc; }
                    }
                } else if (button == 1) { // правый клик — половина / по одному
                    if (cCnt == 0 && sCnt > 0) { i32 half = (sCnt + 1) / 2; cId = sId; cCnt = half; sCnt -= half; if (sCnt == 0) sId = 0; }
                    else if (cCnt > 0 && (sCnt == 0 || sId == cId)) { if (sCnt == 0) sId = cId; if (sCnt < 64) { sCnt++; cCnt--; if (cCnt == 0) cId = 0; } }
                    else if (cCnt > 0 && sCnt > 0 && sId != cId) { i32 ti = sId, tc = sCnt; sId = cId; sCnt = cCnt; cId = ti; cCnt = tc; }
                }
            }
            resync();
            break;
        }
        case 0x0F: { // INVENTORY_CLICK_V1: Close Window — сбросить курсор
            (void)data.readByte(); // window id
            player->cursorItemId = 0; player->cursorCount = 0;
            handleChestWindowClosed(player); // CHEST_V2: анимация крышки + звук закрытия (сбрасывает openWindowId)
            break;
        }
        case 0x10: { // INVENTORY_CLICK_V1: Set Slot State (бандлы) — принимаем и игнорируем
            (void)data.readVarInt(); // slot id
            (void)data.readVarInt(); // window id
            (void)data.readBool();   // state
            break;
        }
        case 0x38: { // BLOCKS_V1: Use Item On (установка блока)
            (void)data.readVarInt(); // рука
            u64 posRaw = data.readU64();
            i32 face = data.readVarInt();
            (void)data.readF32(); (void)data.readF32(); (void)data.readF32(); // курсор
            (void)data.readBool(); // внутри блока
            i32 seq = data.readVarInt();
            i32 bx, by, bz;
            decodeBlockPos(posRaw, bx, by, bz);
            { // CHEST_V1: правый клик по сундуку открывает контейнер (кроме приседа и наблюдателя)
                const i32 clicked = world_.getBlock(bx, by, bz);
                const bool ender = isEnderChestState(clicked);
                if ((isChestBlockState(clicked) || isTrappedChestState(clicked) || ender) &&
                    player->gameMode != 3 && !player->sneaking) {
                    openChestFor(player, bx, by, bz, ender);
                    net::Buffer ab;
                    ab.writeVarInt(seq);
                    player->getConnection()->sendPacket(0x05, std::vector<u8>(ab.writtenSpan().begin(), ab.writtenSpan().end()));
                    break;
                }
            }
            i32 tx = bx, ty = by, tz = bz;
            switch (face) { case 0: --ty; break; case 1: ++ty; break; case 2: --tz; break; case 3: ++tz; break; case 4: --tx; break; case 5: ++tx; break; default: break; }
            if (player->gameMode != 3 && ty >= world::CHUNK_HEIGHT_MIN && ty < world::CHUNK_HEIGHT_MAX) { // SPECTATOR_V1 + HEIGHT_V2
                const i32 ax = tx < 0 ? -tx : tx;
                const i32 az = tz < 0 ? -tz : tz;
                const bool prot = config_.spawnProtection > 0 && ax <= config_.spawnProtection && az <= config_.spawnProtection && !isOpName(config_.ops, player->getName()); // OPS_V1
                i32 state = player->hotbarBlockState[player->heldSlot >= 0 && player->heldSlot < 9 ? player->heldSlot : 0];
                state = orientBlockForPlacement(state, player->get_yaw()); // CHEST_V1: сундуки/печки ставятся лицом к игроку
                const i32 upperState = gen::doorUpperState(state);
                // ANTISUFFOCATE_V2: a player occupies a 0.6 x 1.8 block hitbox.
                auto playerOccupies = [&](i32 ox, i32 oy, i32 oz) {
                    for (const auto& other : getAllPlayersCopy()) {
                        if (!other || !other->isAlive()) continue;
                        const f64 minX = other->getX() - 0.3, maxX = other->getX() + 0.3;
                        const f64 minY = other->getY(),       maxY = other->getY() + 1.8;
                        const f64 minZ = other->getZ() - 0.3, maxZ = other->getZ() + 0.3;
                        if (minX < ox + 1.0 && maxX > ox && minY < oy + 1.0 && maxY > oy && minZ < oz + 1.0 && maxZ > oz)
                            return true;
                    }
                    return false;
                };
                const bool playerBlocked = state > 0 &&
                    (playerOccupies(tx, ty, tz) || (upperState >= 0 && playerOccupies(tx, ty + 1, tz)));
                if (prot) {
                    net::Buffer bu;
                    bu.writePosition(BlockPos{tx, ty, tz});
                    bu.writeVarInt(world_.getBlock(tx, ty, tz));
                    player->getConnection()->sendPacket(0x09, std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end()));
                    player->sendSystemMessage("§cСпавн защищён — здесь строить нельзя");
                } else if (playerBlocked) {
                    net::Buffer bu;
                    bu.writePosition(BlockPos{tx, ty, tz});
                    bu.writeVarInt(world_.getBlock(tx, ty, tz));
                    player->getConnection()->sendPacket(0x09, std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end()));
                } else if (state > 0) {
                    world_.setBlock(tx, ty, tz, state);
                    net::Buffer bu;
                    bu.writePosition(BlockPos{tx, ty, tz});
                    bu.writeVarInt(state);
                    auto vec = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
                    auto allPlayers = getAllPlayersCopy();
                    for (auto& p : allPlayers) if (p->isAlive()) p->getConnection()->sendPacket(0x09, vec);
                    { // CHEST_V2: слияние двух соседних одиночных сундуков в двойной
                        const i32 base = isChestBlockState(state) ? 2954 : (isTrappedChestState(state) ? 9119 : -1);
                        if (base >= 0) {
                            const i32 f = (state - base) / 6;
                            i32 dx = 0, dz = 0;
                            chestCwOffset(f, dx, dz);
                            const i32 single = base + f * 6 + 1; // одиночный того же вида и facing
                            i32 selfSt = 0, nbSt = 0, nx = 0, nz = 0;
                            if (world_.getBlock(tx + dx, ty, tz + dz) == single) {        // сосед по ч��совой — мы LEFT, он RIGHT
                                selfSt = single + 2; nbSt = single + 4; nx = tx + dx; nz = tz + dz;
                            } else if (world_.getBlock(tx - dx, ty, tz - dz) == single) { // сосед против часовой — мы RIGHT, он LEFT
                                selfSt = single + 4; nbSt = single + 2; nx = tx - dx; nz = tz - dz;
                            }
                            if (selfSt > 0) {
                                world_.setBlock(tx, ty, tz, selfSt);
                                world_.setBlock(nx, ty, nz, nbSt);
                                net::Buffer b1; b1.writePosition(BlockPos{tx, ty, tz}); b1.writeVarInt(selfSt);
                                net::Buffer b2; b2.writePosition(BlockPos{nx, ty, nz}); b2.writeVarInt(nbSt);
                                auto v1 = std::vector<u8>(b1.writtenSpan().begin(), b1.writtenSpan().end());
                                auto v2 = std::vector<u8>(b2.writtenSpan().begin(), b2.writtenSpan().end());
                                for (auto& p : allPlayers) if (p->isAlive()) { p->getConnection()->sendPacket(0x09, v1); p->getConnection()->sendPacket(0x09, v2); }
                            }
                        }
                    }
                    { // DOORS_V1: верхняя половина двери
                        const i32 upSt = gen::doorUpperState(state);
                        if (upSt >= 0 && ty + 1 < world::CHUNK_HEIGHT_MAX) {
                            world_.setBlock(tx, ty + 1, tz, upSt);
                            net::Buffer du;
                            du.writePosition(BlockPos{tx, ty + 1, tz});
                            du.writeVarInt(upSt);
                            auto duv = std::vector<u8>(du.writtenSpan().begin(), du.writtenSpan().end());
                            for (auto& pd : allPlayers) if (pd->isAlive()) pd->getConnection()->sendPacket(0x09, duv);
                        }
                    }
                } else {
                    // предмет не блок или блока нет в реестре — мягкий откат клиентского предикта
                    net::Buffer bu;
                    bu.writePosition(BlockPos{tx, ty, tz});
                    bu.writeVarInt(world_.getBlock(tx, ty, tz));
                    player->getConnection()->sendPacket(0x09, std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end()));
                }
            }
            {
                net::Buffer ab;
                ab.writeVarInt(seq);
                player->getConnection()->sendPacket(0x05, std::vector<u8>(ab.writtenSpan().begin(), ab.writtenSpan().end()));
            }
            break;
        }
        case 0x39: { // LIQUID_V1: Use Item (в т.ч. вёдра с водой/лавой)
            const i32 useHand = data.readVarInt(); // SHIELD_V1: 0 = основная рука, 1 = оффхенд
            i32 seq = data.readVarInt();
            { // SHIELD_V1: поднятие щита — активная фаза видна другим + включает блокирование
                const bool handOk = (player->heldSlot >= 0 && player->heldSlot < 9);
                const i32 handItem = (useHand == 1) ? player->invItemId[45] : (handOk ? player->invItemId[36 + player->heldSlot] : 0);
                const i32 handCnt  = (useHand == 1) ? player->invCount[45]  : (handOk ? player->invCount[36 + player->heldSlot] : 0);
                if (handItem == 1162 && handCnt > 0) { // 1162 = minecraft:shield (1.21.1)
                    player->usingShield = true;
                    player->usingShieldHand = useHand;
                    broadcastHandState(player); // метаданные 0x58: рука занята щитом
                }
            }
            const i32 held = (player->heldSlot >= 0 && player->heldSlot < 9) ? player->hotbarBlockState[player->heldSlot] : -1;
            if (held == 80 || held == 96) { // вода / лава: простой рейтрейс взгляд��
                const f64 yawR = static_cast<f64>(player->get_yaw()) * 3.1415926535897932 / 180.0;
                const f64 pitR = static_cast<f64>(player->get_pitch()) * 3.1415926535897932 / 180.0;
                const f64 ddx = -std::sin(yawR) * std::cos(pitR);
                const f64 ddy = -std::sin(pitR);
                const f64 ddz = std::cos(yawR) * std::cos(pitR);
                i32 lx = 0, ly = -1000, lz = 0;
                for (f64 tt = 0.5; tt <= 5.0; tt += 0.1) {
                    const i32 cx = static_cast<i32>(std::floor(player->getX() + ddx * tt));
                    const i32 cy = static_cast<i32>(std::floor(player->getY() + 1.62 + ddy * tt));
                    const i32 cz = static_cast<i32>(std::floor(player->getZ() + ddz * tt));
                    if (cy < world::CHUNK_HEIGHT_MIN || cy >= world::CHUNK_HEIGHT_MAX) break; // HEIGHT_V2
                    if (world_.getBlock(cx, cy, cz) != 0) break;
                    lx = cx; ly = cy; lz = cz;
                }
                if (ly > -1000) {
                    const i32 ax = lx < 0 ? -lx : lx;
                    const i32 az = lz < 0 ? -lz : lz;
                    const bool prot = config_.spawnProtection > 0 && ax <= config_.spawnProtection && az <= config_.spawnProtection && !isOpName(config_.ops, player->getName()); // OPS_V1
                    if (prot) player->sendSystemMessage("§cСпавн защищён — здесь строить нельзя");
                    else {
                        world_.setBlock(lx, ly, lz, held);
                        net::Buffer bu;
                        bu.writePosition(BlockPos{lx, ly, lz});
                        bu.writeVarInt(held);
                        auto vec = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
                        auto allPlayers = getAllPlayersCopy();
                        for (auto& p : allPlayers) if (p->isAlive()) p->getConnection()->sendPacket(0x09, vec);
                    }
                }
            }
            {
                net::Buffer ab;
                ab.writeVarInt(seq);
                player->getConnection()->sendPacket(0x05, std::vector<u8>(ab.writtenSpan().begin(), ab.writtenSpan().end()));
            }
            break;
        }
        default:
            break;
    }
}

// ============================================================
// Утилиты
// ============================================================

std::vector<std::shared_ptr<entity::Player>> NetherCraftServer::getAllPlayersCopy() {
    std::lock_guard lock(playersMutex_);
    std::vector<std::shared_ptr<entity::Player>> result;
    result.reserve(players_.size());
    for (auto& [id, p] : players_) {
        result.push_back(p);
    }
    return result;
}

// ============================================================
// CHEST_V1: серверные контейнеры сундуков
// ============================================================

// CHEST_V1: Open Screen 0x33 (generic_9x3) + полная синхронизация содержимого.
void NetherCraftServer::openChestFor(const std::shared_ptr<entity::Player>& player, i32 bx, i32 by, i32 bz, bool isEnder) {
    if (!player || !player->isAlive()) return;
    const u64 key = chestPosKey(bx, by, bz);
    // CHEST_V3: определяем двойной сундук и ключи половин (ваниль: слоты 0-26 = правая половина, 27-53 = левая)
    bool isDouble = false;
    u64 key1 = key, key2 = 0;
    i32 bx2 = bx, bz2 = bz;
    const i32 st0 = world_.getBlock(bx, by, bz);
    if (!isEnder && (isChestBlockState(st0) || isTrappedChestState(st0))) {
        const i32 base = isChestBlockState(st0) ? 2954 : 9119;
        const i32 f = (st0 - base) / 6;
        const i32 t = ((st0 - base) % 6) / 2; // 0=single, 1=left, 2=right
        if (t != 0) {
            i32 dx = 0, dz = 0;
            chestCwOffset(f, dx, dz);
            if (t == 2) { dx = -dx; dz = -dz; }
            const i32 nSt = world_.getBlock(bx + dx, by, bz + dz);
            const i32 nBase = isChestBlockState(nSt) ? 2954 : (isTrappedChestState(nSt) ? 9119 : -1);
            if (nBase == base && (nSt - nBase) / 6 == f) {
                isDouble = true;
                bx2 = bx + dx; bz2 = bz + dz;
                const u64 partnerKey = chestPosKey(bx2, by, bz2);
                if (t == 1) { key1 = partnerKey; key2 = key; } // кликнули левую — первая половина (слоты 0-26) у правой
                else        { key1 = key;        key2 = partnerKey; }
            }
        }
    }
    if (!isEnder) { // контейнеры создаются при первом открытии
        std::lock_guard lock(chestsMutex_);
        (void)chests_[key1];
        if (isDouble) (void)chests_[key2];
    }
    const i32 wid = player->nextWindowId++;
    if (player->nextWindowId > 99) player->nextWindowId = 1;
    player->openWindowId = wid;
    player->openContainerKey = key1;
    player->openContainerKey2 = isDouble ? key2 : 0; // CHEST_V3
    player->openIsDouble = isDouble;                 // CHEST_V3
    player->openIsEnder = isEnder;
    net::Buffer buf;
    buf.writeVarInt(wid);
    buf.writeVarInt(isDouble ? 5 : 2); // CHEST_V3: generic_9x6 для двойного, иначе generic_9x3
    writeTextComponent(buf, isEnder ? "Эндер-сундук" : (isDouble ? "Большой сундук" : "Сундук"));
    player->getConnection()->sendPacket(0x33, std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
    sendContainerContent(player);
    player->openBlockState = world_.getBlock(bx, by, bz); // CHEST_V2: запоминаем блок для анимации
    const int viewers = countChestViewers(player->openContainerKey, isEnder);
    broadcastChestLid(bx, by, bz, player->openBlockState, viewers);
    if (isDouble) broadcastChestLid(bx2, by, bz2, world_.getBlock(bx2, by, bz2), viewers); // CHEST_V3: крышка второй половины
    if (viewers == 1) // CHEST_V2: первый зритель — звук открытия
        broadcastBlockSound(isEnder ? "minecraft:block.ender_chest.open" : "minecraft:block.chest.open", bx, by, bz, 0.5f, 0.95f);
}

// CHEST_V2: сколько игроков сейчас держат открытым сундук с данным ключом позиции.
int NetherCraftServer::countChestViewers(u64 key, bool ender) {
    int n = 0;
    for (auto& p : getAllPlayersCopy())
        if (p && p->isAlive() && p->openWindowId != 0 && p->openIsEnder == ender && p->openContainerKey == key) ++n;
    return n;
}

// CHEST_V2: анимация крышки сундука — Block Action 0x08 всем игрокам.
void NetherCraftServer::broadcastChestLid(i32 bx, i32 by, i32 bz, i32 blockState, i32 viewers) {
    const i32 blockId = blockRegistryIdForState(blockState);
    if (blockId <= 0) return;
    net::Buffer ba;
    ba.writePosition(BlockPos{bx, by, bz});
    ba.writeByte(1);                       // action 1: обновить число зрителей сундука
    ba.writeByte(static_cast<u8>(viewers)); // 0 зрит��лей = крышка закрывается
    ba.writeVarInt(blockId);               // реестровый id блока (не state!)
    auto vec = std::vector<u8>(ba.writtenSpan().begin(), ba.writtenSpan().end());
    for (auto& p : getAllPlayersCopy()) if (p && p->isAlive()) p->getConnection()->sendPacket(0x08, vec);
}

// CHEST_V2: именованный звук — Sound Effect 0x68 (inline sound event) всем игрокам.
void NetherCraftServer::broadcastBlockSound(const char* name, i32 bx, i32 by, i32 bz, f32 volume, f32 pitch) {
    net::Buffer s;
    s.writeVarInt(0);         // 0 = inline sound event (не из реестра)
    s.writeString(name);      // identifier звука
    s.writeBool(false);       // нет fixed range
    s.writeVarInt(4);         // категория: blocks
    s.writeI32(bx * 8 + 4);   // fixed-point *8, центр блока
    s.writeI32(by * 8 + 4);
    s.writeI32(bz * 8 + 4);
    s.writeF32(volume);
    s.writeF32(pitch);
    s.writeI64(0);            // seed
    auto vec = std::vector<u8>(s.writtenSpan().begin(), s.writtenSpan().end());
    for (auto& p : getAllPlayersCopy()) if (p && p->isAlive()) p->getConnection()->sendPacket(0x68, vec);
}

// SHIELD_V1: метаданные «руки заняты» (Set Entity Metadata 0x58, index 8, byte) —
// активная фаза щита видна остальным игрокам.
void NetherCraftServer::broadcastHandState(const std::shared_ptr<entity::Player>& player) {
    if (!player) return;
    net::Buffer m;
    m.writeVarInt(static_cast<i32>(player->getEntityId()));
    m.writeByte(8);   // index 8: LivingEntity hand states
    m.writeVarInt(0); // сериализатор 0: byte
    m.writeByte(player->usingShield ? static_cast<u8>(0x01 | (player->usingShieldHand == 1 ? 0x02 : 0x00)) : 0x00); // bit0 = active, bit1 = offhand
    m.writeByte(0xFF); // конец метаданных
    auto v = std::vector<u8>(m.writtenSpan().begin(), m.writtenSpan().end());
    for (auto& p : getAllPlayersCopy())
        if (p && p != player && p->isAlive() && p->getState() == entity::PlayerState::Play)
            p->getConnection()->sendPacket(0x58, v);
}

// CHEST_V2: игрок закрыл окно сундука — обновить анимацию крышки и звук закрытия.
void NetherCraftServer::handleChestWindowClosed(const std::shared_ptr<entity::Player>& player) {
    if (!player || player->openWindowId == 0) return;
    const u64 key = player->openContainerKey;
    const u64 key2 = player->openIsDouble ? player->openContainerKey2 : 0; // CHEST_V3
    const bool ender = player->openIsEnder;
    player->openWindowId = 0;
    player->openIsDouble = false;     // CHEST_V3
    player->openContainerKey2 = 0;    // CHEST_V3
    i32 bx = 0, by = 0, bz = 0;
    decodeChestPosKey(key, bx, by, bz);
    const i32 st = world_.getBlock(bx, by, bz);
    if (!isChestBlockState(st) && !isTrappedChestState(st) && !isEnderChestState(st)) return; // блока уже нет
    const int viewers = countChestViewers(key, ender);
    broadcastChestLid(bx, by, bz, st, viewers);
    if (key2 != 0) { // CHEST_V3: крышка второй половины двойного сундука
        i32 qx = 0, qy = 0, qz = 0;
        decodeChestPosKey(key2, qx, qy, qz);
        const i32 st2 = world_.getBlock(qx, qy, qz);
        if (isChestBlockState(st2) || isTrappedChestState(st2)) broadcastChestLid(qx, qy, qz, st2, viewers);
    }
    if (viewers == 0) // последний зритель ушёл — звук закрытия
        broadcastBlockSound(ender ? "minecraft:block.ender_chest.close" : "minecraft:block.chest.close", bx, by, bz, 0.5f, 0.9f);
}

// CHEST_V1: Container Set Content 0x13 для открытого окна сундука (27 + 36 слотов).
void NetherCraftServer::sendContainerContent(const std::shared_ptr<entity::Player>& player) {
    if (!player || !player->isAlive() || player->openWindowId == 0) return;
    const int cont = player->openIsDouble ? 54 : 27; // CHEST_V3: двойной сундук = 54 слота
    i32 ids[54] = {}; i32 cnts[54] = {};
    if (player->openIsEnder) {
        for (int i = 0; i < 27; ++i) { ids[i] = player->enderItemId[i]; cnts[i] = player->enderCount[i]; }
    } else {
        std::lock_guard lock(chestsMutex_);
        auto& c = chests_[player->openContainerKey];
        for (int i = 0; i < 27; ++i) { ids[i] = c.itemId[i]; cnts[i] = c.count[i]; }
        if (player->openIsDouble) { // CHEST_V3: вторая половина — слоты 27-53
            auto& c2 = chests_[player->openContainerKey2];
            for (int i = 0; i < 27; ++i) { ids[27 + i] = c2.itemId[i]; cnts[27 + i] = c2.count[i]; }
        }
    }
    net::Buffer inv;
    inv.writeByte(static_cast<u8>(player->openWindowId));
    inv.writeVarInt(1); // state id
    inv.writeVarInt(cont + 36);
    auto writeSlot = [&](i32 id, i32 cnt) {
        if (cnt > 0 && id > 0) { inv.writeVarInt(cnt); inv.writeVarInt(id); inv.writeVarInt(0); inv.writeVarInt(0); }
        else inv.writeVarInt(0);
    };
    for (int i = 0; i < cont; ++i) writeSlot(ids[i], cnts[i]); // CHEST_V3
    for (int i = 9; i < 45; ++i) writeSlot(player->invItemId[i], player->invCount[i]); // рюкзак + хотбар
    if (player->cursorCount > 0 && player->cursorItemId > 0) { inv.writeVarInt(player->cursorCount); inv.writeVarInt(player->cursorItemId); inv.writeVarInt(0); inv.writeVarInt(0); }
    else inv.writeVarInt(0);
    player->getConnection()->sendPacket(0x13, std::vector<u8>(inv.writtenSpan().begin(), inv.writtenSpan().end()));
}

// ============================================================
// Tick loop
// ============================================================

// ============================================================
// WORLDSAVE_V1: данные игроков (world/playerdata/<ник>.txt).
// Удалить игрока = удалить его файл в этой папке.
// ============================================================

void NetherCraftServer::savePlayerData(std::shared_ptr<entity::Player> player) {
    if (!player || player->getName().empty()) return;
    std::error_code ec;
    std::filesystem::create_directories("world/playerdata", ec);
    std::ofstream f("world/playerdata/" + player->getName() + ".txt", std::ios::trunc);
    if (!f) return;
    f << player->getX() << " " << player->getY() << " " << player->getZ()
      << " " << player->get_yaw() << " " << player->get_pitch() << " " << player->gameMode;
    for (i32 slotState : player->hotbarBlockState) f << " " << slotState;
    f << "\n"; // INVENTORY_SAVE_V1: nine hotbar block states
    // INVENTORY_V3: полный инвентарь (выбранный слот + 46 слотов пар itemId count)
    f << "INV3 " << player->heldSlot;
    for (int i = 0; i < entity::Player::INV_SIZE; ++i) f << " " << player->invItemId[i] << " " << player->invCount[i];
    f << "\n";
}

void NetherCraftServer::loadPlayerData(std::shared_ptr<entity::Player> player) {
    if (!player || player->getName().empty()) return;
    std::ifstream f("world/playerdata/" + player->getName() + ".txt");
    if (!f) return;
    f64 x = 0, y = 0, z = 0;
    f32 yaw = 0, pitch = 0; i32 savedMode = -1;
    if (f >> x >> y >> z >> yaw >> pitch) {
        if ((f >> savedMode) && savedMode >= 0 && savedMode <= 3) player->gameMode = savedMode;
        for (int slot = 0; slot < 9; ++slot) { i32 savedState = -1; if (!(f >> savedState)) break; player->hotbarBlockState[slot] = savedState; }
        // INVENTORY_V3: читаем полный инвентарь, если он записан (обратная совместимость со старым форматом)
        std::string invTag;
        if ((f >> invTag) && invTag == "INV3") {
            i32 hs = 0;
            if ((f >> hs) && hs >= 0 && hs < 9) player->heldSlot = hs;
            for (int i = 0; i < entity::Player::INV_SIZE; ++i) {
                i32 id = 0, cnt = 0;
                if (!(f >> id >> cnt)) break;
                player->invItemId[i] = id;
                player->invCount[i] = cnt;
            }
            // восстановить block state хотбара из инвентаря (нужно для установки блоков)
            for (int i = 0; i < 9; ++i) {
                const i32 id = player->invItemId[36 + i];
                const i32 cnt = player->invCount[36 + i];
                player->hotbarBlockState[i] = (cnt > 0 && id > 0) ? itemToBlockState(id) : -1;
            }
        }
        player->setPosition(x, y, z); // INVENTORY_SAVE_V1
        player->setRotation(yaw, pitch);
        NC_DEBUG("Server", "Позиция игрока {} восстановлена: {:.1f} {:.1f} {:.1f}", player->getName(), x, y, z);
    }
}

// FLATWORLD_V1: при пересечении границы чанка шлём новый центр + чанки вокруг
void NetherCraftServer::streamChunks(std::shared_ptr<entity::Player> player) {
    i32 cx = static_cast<i32>(std::floor(player->getX() / 16.0));
    i32 cz = static_cast<i32>(std::floor(player->getZ() / 16.0));
    if (cx == player->getViewCenterX() && cz == player->getViewCenterZ()) return;
    player->setViewCenter(cx, cz);

    net::Buffer viewBuf;
    viewBuf.writeVarInt(cx);
    viewBuf.writeVarInt(cz);
    player->getConnection()->sendPacket(0x54,
        std::vector<u8>(viewBuf.writtenSpan().begin(), viewBuf.writtenSpan().end()));

    i32 r = config_.viewDistance; // VIEWDIST_V1: полный радиус, чтобы край мира не торчал до тумана
    if (r < 2) r = 2;
    sendChunksAround(player, cx, cz, r, 16); // PERF_ASYNC_V2: fill new view edge faster (was 9)

    // MEM_V1: unload chunks far from every player so RAM stays bounded as players
    // roam. Keep the view distance + a small margin around each player.
    {
        std::vector<std::pair<i32, i32>> centers;
        for (auto& p : getAllPlayersCopy()) {
            if (!p) continue;
            centers.emplace_back(p->getViewCenterX(), p->getViewCenterZ());
        }
        world_.pruneChunks(centers, r + 3);
    }
}

void NetherCraftServer::processConsoleCommands() {
    std::deque<std::string> commands;
    {
        std::lock_guard lock(consoleMutex_);
        commands.swap(consoleCommands_);
    }
    for (std::string command : commands) {
        if (!command.empty() && command[0] == '/') command.erase(0, 1);
        std::istringstream iss(command);
        std::string cmd;
        iss >> cmd;
        if (cmd.empty()) continue;
        if (cmd == "help") {
            NC_INFO("Console", "Commands: help, list, say <text>, save-all, stop");
        } else if (cmd == "list") {
            auto all = getAllPlayersCopy();
            std::string names;
            int count = 0;
            for (const auto& p : all) if (p && p->isAlive()) {
                if (!names.empty()) names += ", ";
                names += p->getName(); ++count;
            }
            NC_INFO("Console", "Players ({}): {}", count, names.empty() ? "none" : names);
        } else if (cmd == "say") {
            std::string text; std::getline(iss, text);
            if (!text.empty() && text[0] == ' ') text.erase(0, 1);
            if (text.empty()) { NC_INFO("Console", "Usage: say <text>"); continue; }
            for (const auto& p : getAllPlayersCopy()) if (p && p->isAlive())
                p->sendSystemMessage(std::format("§d[Console] {}", text));
            NC_INFO("Console", "[Console] {}", text);
        } else if (cmd == "save-all" || cmd == "save") {
            world_.saveToDisk("world/world.dat");
            for (const auto& p : getAllPlayersCopy()) if (p) savePlayerData(p);
            NC_INFO("Console", "World and player data saved");
        } else if (cmd == "stop") {
            NC_INFO("Console", "Stopping server...");
            stop();
        } else {
            NC_WARN("Console", "Unknown command '{}'. Type help.", cmd);
        }
    }
}

void NetherCraftServer::tick() {
    processConsoleCommands(); // CONSOLE_V2: all world/network operations remain on the tick thread
    // FASTBOOT_V1: install chunks streamed from disk by the background loader (main thread only).
    world_.drainLoadedChunks();
    {
        static bool s_worldLoadLogged = false;
        if (!s_worldLoadLogged && world_.isBackgroundLoadDone()) {
            s_worldLoadLogged = true;
            if (config_.language == "rus") NC_INFO("Server", "Мир догружен с диска: {} чанков", world_.getLoadedChunkCount());
            else NC_INFO("Server", "World finished loading from disk: {} chunks", world_.getLoadedChunkCount()); // LANGFIX_V1
        }
    }
    tickCounter_++;
    updateTpsBossbar();

    // TIMESYNC_V1: время суток тикает на сервере и рассылается раз в секунду —
    // иначе после смерти/респавна день и ночь у игроков расходятся
    if (tickCounter_ % 20 == 0) {
        g_timeOfDay = (g_timeOfDay + 20) % 24000;
        for (auto& p : getAllPlayersCopy())
            if (p && p->isAlive() && p->getState() == entity::PlayerState::Play) sendTimeUpdate(p);
    }

    if (tickCounter_ % 300 == 0) {
        tickKeepAlive();
    }

    // WORLDSAVE_V1: автосохранение каждые 5 минут (6000 тиков)
    if (tickCounter_ % 6000 == 0) {
        world_.saveToDisk("world/world.dat");
        for (auto& p : getAllPlayersCopy()) savePlayerData(p);
        if (config_.language == "rus") NC_INFO("Server", "Автосохранение: мир + игроки");
        else NC_INFO("Server", "Auto-save: world + players"); // LANGFIX_V1
    }
}

void NetherCraftServer::sendTpsBossbar(const std::shared_ptr<entity::Player>& player, bool add) {
    if (!player || !player->isAlive() || player->getState() != entity::PlayerState::Play) return;
    const f32 progress = std::clamp(tps_ / 20.0f, 0.0f, 1.0f);
    const std::string title = tpsBossTitle(tps_);

    if (add || !player->tpsBossbarShown) {
        // Clientbound Boss Event 0x0A, operation ADD=0.
        net::Buffer out;
        out.writeUUID(TPS_BOSSBAR_ID);
        out.writeVarInt(0);
        writeTextComponent(out, title); // Component NBT
        out.writeF32(progress);
        out.writeVarInt(tpsBossColor(tps_)); // BossBarColor
        out.writeVarInt(0);         // BossBarOverlay.PROGRESS
        out.writeByte(0);           // no darken screen / music / fog
        player->getConnection()->sendPacket(0x0A, std::vector<u8>(out.writtenSpan().begin(), out.writtenSpan().end()));
        player->tpsBossbarShown = true;
        return;
    }

    // UPDATE_NAME=3 and UPDATE_PROGRESS=2 are small packets, sent once per second.
    net::Buffer name;
    name.writeUUID(TPS_BOSSBAR_ID);
    name.writeVarInt(3);
    writeTextComponent(name, title);
    player->getConnection()->sendPacket(0x0A, std::vector<u8>(name.writtenSpan().begin(), name.writtenSpan().end()));
    net::Buffer value;
    value.writeUUID(TPS_BOSSBAR_ID);
    value.writeVarInt(2);
    value.writeF32(progress);
    player->getConnection()->sendPacket(0x0A, std::vector<u8>(value.writtenSpan().begin(), value.writtenSpan().end()));
}

void NetherCraftServer::removeTpsBossbar(const std::shared_ptr<entity::Player>& player) {
    if (!player || !player->isAlive() || !player->tpsBossbarShown) return;
    net::Buffer out;
    out.writeUUID(TPS_BOSSBAR_ID);
    out.writeVarInt(1); // ClientboundBossEventPacket.OperationType.REMOVE
    player->getConnection()->sendPacket(0x0A, std::vector<u8>(out.writtenSpan().begin(), out.writtenSpan().end()));
    player->tpsBossbarShown = false;
}

void NetherCraftServer::updateTpsBossbar() {
    const auto now = std::chrono::steady_clock::now();
    if (tpsSampleStart_.time_since_epoch().count() == 0) tpsSampleStart_ = now;
    ++tpsSampleTicks_;
    const f64 seconds = std::chrono::duration<f64>(now - tpsSampleStart_).count();
    if (seconds < 1.0) return;

    const f32 instant = std::clamp(static_cast<f32>(tpsSampleTicks_ / seconds), 0.0f, 20.0f);
    // TPS20_V2: the hybrid-spin scheduler holds a true 20/s, so snap tiny
    // measurement jitter (>=19.95) to a clean 20.00; genuine stalls fall well
    // below 19.95 and are still shown via the smoothing branch.
    if (instant >= 19.95f) tps_ = 20.0f;
    else tps_ = tps_ * 0.5f + instant * 0.5f;
    tpsSampleTicks_ = 0;
    tpsSampleStart_ = now;
    for (auto& player : getAllPlayersCopy()) {
        if (player && player->tpsBossbarEnabled) sendTpsBossbar(player, false);
    }
}

void NetherCraftServer::tickKeepAlive() {
    auto allPlayers = getAllPlayersCopy();
    for (auto& p : allPlayers) {
        if (p->isAlive()) {
            sendKeepAlive(p);
        }
    }
}

} // namespace nc