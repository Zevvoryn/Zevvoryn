#include "server.hpp"
#include "log.hpp"
#include "command_registry.hpp" // PLUGINCMD_V1
#include "op_manager.hpp"       // OPMGR_V1: ops.json + /op + /deop
#include "ban_manager.hpp"      // BANMGR_V1: banned-players.json + /ban + /pardon
#include "i18n.hpp"            // I18N_V1: ответы команд на языке клиента
#include "../registries/registry.hpp"
#include "../utils/nbt.hpp"
#include "../world/anvil.hpp" // ANVIL_CONVERT_V1

#include <nlohmann/json.hpp>
#include <set>
#include <array>
#include <vector>
#include <cstdlib>
#include <format>
#include <thread>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <chrono> // VIEWDIST_V1
#include <algorithm> // CLIENT_BATCH_V1
#include <functional> // FLUID_V2: рекурсивный slope-поиск
#include <utility> // CLIENT_BATCH_V1
#include <sstream> // CMDS_V1
#include <cctype> // OPS_V1
#include <cstdio> // OPMGR_V1: snprintf для UUID
#include <ctime> // CLEANEXIT_V2: время в last-exit.txt
#include "item_blocks.gen.hpp" // BLOCKS_V2
#include "items.gen.hpp" // GIVECMD_V1
#include "icon.gen.hpp" // ICON_V1: встроенная иконка сервера + папка icon_Server
#include "spawn.gen.hpp" // SPAWNCFG_V1: папка spawn + spawn.properties
#include "tab.gen.hpp" // TABSERVER_V1: настраиваемый header/footer таб-листа + папка tab_Server
#include "../crypto/mc_crypto.hpp" // ONLINE_V1
#include <random> // SPAWN_V1
#include "crash_context.hpp" // CRASHCTX_V1

namespace nc {

// STARTPROF_V1: сколько времени съел каждый этап запуска. Пишется в DEBUG, чтобы не
// мешать обычному логу: log-level=DEBUG в settings.properties — и таблица появится.
namespace {
struct StartupProfile {
    using clock = std::chrono::steady_clock;
    clock::time_point mark = clock::now();
    std::vector<std::pair<std::string, long long>> phases;
    void reset() { phases.clear(); mark = clock::now(); }
    void lap(const char* name) {
        const auto now = clock::now();
        phases.emplace_back(name, std::chrono::duration_cast<std::chrono::milliseconds>(now - mark).count());
        mark = now;
    }
    void dump(long long totalMs) const {
        auto row = [](const std::string& name, long long ms) {
            std::string dots = name;
            while (dots.size() < 18) dots.push_back('.');
            NC_DEBUG("Startup", "{} {} ms", dots, ms);
        };
        for (const auto& ph : phases) row(ph.first, ph.second);
        row("Total", totalMs);
    }
};
StartupProfile g_startProfile;

// PATHU8_V3: path::string() returns the ANSI form on Windows, so a folder like
// C:/Users/<cyrillic name> turns into question marks in the UTF-8 console.
inline std::string pathU8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
} // namespace

// TPS20_V1: Windows default timer resolution is ~15.6ms, so sleep(1ms) sleeps ~15ms
// and the tick loop yields ~16 TPS instead of 20. timeBeginPeriod(1) raises it to 1ms.
// (MSVC links winmm via the pragma below; with MinGW add -lwinmm.)
#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned __stdcall timeBeginPeriod(unsigned);
extern "C" __declspec(dllimport) unsigned __stdcall timeEndPeriod(unsigned);
#pragma comment(lib, "winmm.lib")
// TPS20_V3: флаги высокоточного waitable-таймера — на случай старого SDK, где они не объявлены.
#ifndef CREATE_WAITABLE_TIMER_MANUAL_RESET
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
// SCHEDFIX_V1: поднять приоритет тик-потока, чтобы Windows не вытеснял его на ~квант (~13мс).
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread(void);
extern "C" __declspec(dllimport) int __stdcall SetThreadPriority(void*, int);
// HUD_V1: psapi.h's K32GetProcessMemoryInfo is re-exported straight from
// kernel32.dll (no separate psapi.lib to link, same no-extra-lib style as winmm above).
#include <psapi.h>
#endif

// TPS_BOSS_V1: one stable Java Boss Event id shared by all players.
static constexpr UUID TPS_BOSSBAR_ID{0x5A6576766F72796EULL, 0x5450534D6F6E6974ULL};
// HUD_V1: real thresholds as requested — green 19-20, yellow 15-19, red below 15.
static i32 tpsBossColor(f32 tps) {
    if (tps >= 19.0f) return 3; // GREEN
    if (tps >= 15.0f) return 4; // YELLOW
    return 2;                   // RED
}
static std::string tpsBossTitle(f32 tps, f32 ramMb, f32 cpuPercent) {
    return std::format("TPS {:.2f} | RAM {:.0f} MB | CPU {:.0f}%", tps, ramMb, cpuPercent);
}
// Java 1.21.1 serializes chat components as unnamed network NBT, not JSON strings.
static void writeTextComponent(net::Buffer& out, std::string_view text) {
    out.writeByte(0x08); // TAG_String root
    out.writeU16(static_cast<u16>(text.size()));
    out.writeBytes(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

// XP_BOTTLE_V2: MSVC needs declarations before early call sites in this file.
static i32 xpNeededForNextLevel(i32 level);
static void syncExperienceBar(const std::shared_ptr<entity::Player>& player);
static void grantExperience(const std::shared_ptr<entity::Player>& player, i32 amount);

// MINIEDIT_V3: styled custom_name Component for Builder's Wand.
static void writeYellowWandName(net::Buffer& out) {
    auto nbtString = [&](std::string_view key, std::string_view value) {
        out.writeByte(0x08);
        out.writeU16(static_cast<u16>(key.size()));
        out.writeBytes(std::span<const u8>(reinterpret_cast<const u8*>(key.data()), key.size()));
        out.writeU16(static_cast<u16>(value.size()));
        out.writeBytes(std::span<const u8>(reinterpret_cast<const u8*>(value.data()), value.size()));
    };
    out.writeByte(0x0A); // unnamed TAG_Compound
    nbtString("text", "Builder's Wand");
    nbtString("color", "yellow");
    out.writeByte(0x01); // TAG_Byte italic=false
    out.writeU16(6);
    out.writeBytes(std::span<const u8>(reinterpret_cast<const u8*>("italic"), 6));
    out.writeByte(0);
    out.writeByte(0); // TAG_End
}

static void writeInventoryStack(net::Buffer& out, i32 itemId, i32 count, bool namedWand = false) {
    if (count <= 0 || itemId <= 0) { out.writeVarInt(0); return; }
    out.writeVarInt(count); out.writeVarInt(itemId);
    if (namedWand && itemId == 821) {
        out.writeVarInt(1); out.writeVarInt(0); // added, removed components
        out.writeVarInt(5); writeYellowWandName(out);
    } else {
        out.writeVarInt(0); out.writeVarInt(0);
    }
}

// ALLPACKETS_V3: generic minecraft:custom_name component (plain text, no styling) --
// used by Edit Book (0x14) and Rename Item (0x2A), which are real per-slot renames
// even though we do not model a full lectern/anvil UI.
static void writeCustomNameComponent(net::Buffer& out, std::string_view text) {
    auto nbtString = [&](std::string_view key, std::string_view value) {
        out.writeByte(0x08);
        out.writeU16(static_cast<u16>(key.size()));
        out.writeBytes(std::span<const u8>(reinterpret_cast<const u8*>(key.data()), key.size()));
        out.writeU16(static_cast<u16>(value.size()));
        out.writeBytes(std::span<const u8>(reinterpret_cast<const u8*>(value.data()), value.size()));
    };
    out.writeByte(0x0A); // unnamed TAG_Compound
    nbtString("text", text);
    out.writeByte(0); // TAG_End
}

static void sendItemSlotWithName(const std::shared_ptr<entity::Player>& player, i32 slotIdx) {
    if (!player || !player->getConnection()) return;
    net::Buffer pk;
    pk.writeByte(0); pk.writeVarInt(0); pk.writeI16(static_cast<i16>(slotIdx));
    const i32 cnt = player->invCount[slotIdx];
    pk.writeVarInt(cnt);
    if (cnt > 0) {
        pk.writeVarInt(player->invItemId[slotIdx]);
        if (!player->invCustomName[slotIdx].empty()) {
            pk.writeVarInt(1); pk.writeVarInt(0); // one added component, no removals
            pk.writeVarInt(5);                     // minecraft:custom_name
            writeCustomNameComponent(pk, player->invCustomName[slotIdx]);
        } else {
            pk.writeVarInt(0); pk.writeVarInt(0);
        }
    }
    player->getConnection()->sendPacket(0x15, std::vector<u8>(pk.writtenSpan().begin(), pk.writtenSpan().end()));
}

// ALLPACKETS_V3: block-entity/command-minecart text the server genuinely stores.
// Hoisted out of their old per-case `static` locals so Query Block NBT (0x01)
// can honestly read back what Update Command Block (0x30) / Update Sign (0x35)
// actually saved, instead of always answering empty.
static std::mutex g_cmdBlockMutex;
static std::unordered_map<u64, std::string> g_cmdBlockText;
static std::mutex g_signMutex;
static std::unordered_map<u64, std::array<std::string, 4>> g_signText;
static std::mutex g_cmdMinecartMutex;
static std::unordered_map<i32, std::string> g_cmdMinecartText;

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

NetherCraftServer::NetherCraftServer() : miniEdit_(world_) {
    registries::RegistryManager::instance().loadDefaults();

    // PLUGINCMD_V1: these four used to be hand-added byte-for-byte into the
    // Commands packet one at a time. Now they just register like any future
    // plugin command would — proof that the generic path works end-to-end.
    // Their real execution logic still lives in the big in-game `cmd == "..."`
    // chain below (handler left null here); this registration only exists so
    // the console and the client-side command tree learn about them
    // automatically, with zero manual node-index math.
    auto& reg = nc::cmd::CommandRegistry::instance();
    reg.registerCommand({"tps", true, "core", "/tps", nullptr});
    reg.registerCommand({"summon", true, "core", "/summon <pig|zombie|cow|sheep|creeper|skeleton|id> [count]", nullptr});
    reg.registerCommand({"give", true, "core", "/give [player] <id|name> [count]", nullptr}); // GIVECMD_V1
    reg.registerCommand({"killall", true, "core", "/killall", nullptr});
    reg.registerCommand({"crash", true, "core", "/crash", nullptr});
    reg.registerCommand({"stop", true, "core", "/stop", nullptr});     // STOPCMD_V1
    reg.registerCommand({"reload", true, "core", "/reload", nullptr}); // SOFTRELOAD_V1
    // MINIEDIT_V1: double-slash commands arrive with one leading slash.
    // Direct pos aliases are also advertised because the owner requested /pso and /pos2.
    // CMDTREE_V2: здесь перечислены ТОЛЬКО те команды, у которых уже есть живая
    // реализация в ветках `cmd == "..."` ниже. Дерево команд клиента
    // строится из этого реестра, поэтому пустых пунктов в нём быть не должно:
    // всё остальное (/kill, /clear, /effect, /xp, /msg, /ban и т.д.) регистрируется
    // в startCommon() вместе с настоящим handler'ом.
    for (const char* name : {"help", "list", "time", "weather", "gamemode", "gm0", "gm1", "gm2", "gm3",
                             "say", "kick", "setblock", "skin", "nether", "end", "overworld",
                             "spawn", "setworldspawn", "setspawn", "locate", "mob", "save", "save-all", "tps",
                             "summon", "give", "killall", "crash", "stop", "reload",
                             "warprandomtick", "whitelist", "import-vanilla", "export-vanilla"})
        reg.registerCommand({name, true, "core", std::string("/") + name, nullptr});
    for (const char* name : {"/wand", "/pos1", "/pos2", "/pso", "/set", "/replace", "/copy", "/paste", "/rotate",
                             "edit", "we", "wand", "set", "replace", "copy", "paste", "rotate",
                             "undo", "redo", "pos1", "pos2", "pso"})
        reg.registerCommand({name, true, "miniedit", "Built-in MiniEdit", nullptr});
}

NetherCraftServer::~NetherCraftServer() {
    stop();
}

bool NetherCraftServer::start(const std::string& configPath) {
    configPath_ = configPath; // SOFTRELOAD_V1
    g_startProfile.reset(); // STARTPROF_V1
    config_ = ServerConfig::loadFrom(configPath);
    // SINGLEPASS_V1: старые конфиги и ручные правки могут оставить rcon.password пустым —
    // раньше это тихо ломало RCON. Генерим токен сами, сохраняем в settings.properties
    // и тут же подменяем его в .env панели, чтобы стороны не разъехались.
    if (config_.enableRcon && config_.rconPassword.empty()) {
        config_.rconPassword = setup::makeSecret(24); // SINGLEPASS_V1: хелперы живут в nc::setup
        config_.saveTo(configPath);
        setup::syncEnvRconPassword(config_.rconPassword);
        if (config_.language == "rus")
            NC_INFO("Server", "Пароль RCON был пуст — сгенерирован автоматически и записан в settings.properties и .env");
        else
            NC_INFO("Server", "RCON password was empty - generated automatically and written to settings.properties and .env");
    }
    return startCommon();
}

bool NetherCraftServer::startWithConfig(const ServerConfig& cfg) {
    config_ = cfg;
    return startCommon();
}

// SPAWN_V1: мировая точка спавна (общая), персистится в world/spawn.dat
static i32 g_spawnX = 0, g_spawnY = 4, g_spawnZ = 0; // FLATNATIVE_V1: родная высота — ноги игрока на Y=4 (трава на Y=3)

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

// CLEANEXIT_V2: logs/last-exit.txt теперь понятен человеку, открывшему его вручную:
// первая строка — машинный статус (её ����������������итает сервер), ниже — пояснение и время обновления.
static void writeLastExitFile(const char* status, bool ru) {
    try {
        std::filesystem::create_directories("logs");
        std::ofstream f("logs/last-exit.txt", std::ios::trunc);
        char ts[32] = "?";
        std::time_t now = std::time(nullptr);
        if (std::tm* tmv = std::localtime(&now)) std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tmv);
        const bool clean = std::string(status) == "clean";
        f << status << "\n\n";
        if (ru) {
            f << "# Служебный файл сервера Zevvoryn: как завершился последний сеанс.\n";
            f << "# Первую строку читает сам сервер при старте — не редактируй этот файл.\n";
            f << "#\n";
            f << "#   running = сервер работает прямо сейчас, ЛИБО был закрыт аварийно\n";
            f << "#             (краш, диспетчер задач, принудительное закрытие окна) — см. logs/crash-last.txt\n";
            f << "#   clean   = сервер остановлен корректно командой stop, всё сохранено\n";
            f << "#\n";
            f << "# Обновлено: " << ts << (clean ? " (корректная остановка через stop)" : " (запуск сервера)") << "\n";
        } else {
            f << "# Zevvoryn server status file: how the last session ended.\n";
            f << "# The first line is read by the server on startup - do not edit this file.\n";
            f << "#\n";
            f << "#   running = the server is running right now, OR it was terminated\n";
            f << "#             abnormally (crash, task manager, force-closed window) - see logs/crash-last.txt\n";
            f << "#   clean   = the server was stopped properly with the stop command\n";
            f << "#\n";
            f << "# Updated: " << ts << (clean ? " (clean stop)" : " (server startup)") << "\n";
        }
    } catch (...) {}
}

bool NetherCraftServer::startCommon() {
    auto startTime = std::chrono::steady_clock::now();

    // ── OPMGR_V1: операторы (ops.json) ──────────────────────────────────
    {
        std::error_code __ec;
        nc::OpManager::instance().init(std::filesystem::current_path(__ec));
        nc::OpManager::instance().importLegacyCsv(config_.ops); // миграция ops= из settings.properties
        nc::BanManager::instance().init(std::filesystem::current_path(__ec)); // BANMGR_V1

        // Мгновенно перерисовываем интерфейс игрока при /op и /deop:
        // Entity Event 24+level переключает доступность команд и F3+F4.
        nc::OpManager::instance().setRefreshHook([this](const std::string& who, int lvl) {
            for (auto& p : getAllPlayersCopy()) {
                if (!p || !p->isAlive() || p->getName() != who) continue;
                if (p->getState() != entity::PlayerState::Play) continue;
                net::Buffer ev;
                ev.writeI32(static_cast<i32>(p->getEntityId()));
                ev.writeByte(static_cast<u8>(24 + std::clamp(lvl, 0, 4)));
                p->getConnection()->sendPacket(0x1F, std::vector<u8>(ev.writtenSpan().begin(), ev.writtenSpan().end()));
                p->sendSystemMessage(lvl > 0
                    ? std::format("§aВы получили права оператора (уровень {})", lvl)
                    : std::string("§cВы больше не оператор"));
            }
        });

        static bool __opCmdsRegistered = false;
        if (!__opCmdsRegistered) {
            __opCmdsRegistered = true;
            auto& __reg = nc::cmd::CommandRegistry::instance();

            // /op <player> [level] — level 1..4, по умолчанию 4 (полный оператор)
            __reg.registerCommand({"op", true, "core", "/op <player> [1-4]", [this](nc::cmd::CommandContext& ctx) {
                if (ctx.args.size() < 2) { ctx.reply("§cUsage: /op <player> [1-4]"); return; }
                const std::string who = ctx.args[1];
                int lvl = 4; // OPMGR_V1: ванильный дефолт
                if (ctx.args.size() >= 3) { try { lvl = std::stoi(ctx.args[2]); } catch (...) { lvl = 4; } }
                if (lvl < 1 || lvl > 4) { ctx.reply("§cУровень прав должен быть от 1 до 4"); return; }
                std::string uuidStr;
                for (auto& p : getAllPlayersCopy()) {
                    if (!p || p->getName() != who) continue;
                    const auto& u = p->getUuid();
                    char b[40];
                    std::snprintf(b, sizeof(b), "%08x-%04x-%04x-%04x-%012llx",
                        static_cast<unsigned>(u.mostSignificant >> 32),
                        static_cast<unsigned>((u.mostSignificant >> 16) & 0xffff),
                        static_cast<unsigned>(u.mostSignificant & 0xffff),
                        static_cast<unsigned>((u.leastSignificant >> 48) & 0xffff),
                        static_cast<unsigned long long>(u.leastSignificant & 0xffffffffffffull));
                    uuidStr = b;
                    break;
                }
                const bool changed = nc::OpManager::instance().addOp(who, uuidStr, lvl, false);
                ctx.reply(changed ? std::format("§a{} теперь оператор (уровень {})", who, lvl)
                                  : std::format("§e{} уже оператор уровня {}", who, lvl));
                NC_INFO("Ops", "op {} level {} (by {})", who, lvl, ctx.isConsole ? "console" : ctx.playerName);
            }});

            // /deop <player>
            __reg.registerCommand({"deop", true, "core", "/deop <player>", [](nc::cmd::CommandContext& ctx) {
                if (ctx.args.size() < 2) { ctx.reply("§cUsage: /deop <player>"); return; }
                const std::string who = ctx.args[1];
                const bool removed = nc::OpManager::instance().removeOp(who);
                ctx.reply(removed ? std::format("§a{} больше не оператор", who)
                                  : std::format("§e{} и так не оператор", who));
                NC_INFO("Ops", "deop {} (by {})", who, ctx.isConsole ? "console" : ctx.playerName);
            }});

            // ── CMDFULL_V1: ванильные команды с настоящей логикой ───────────────────
            // Работают из чата, из консоли и через RCON и автоматически попадают
            // в дерево команд клиента (пакет 0x11).
            // I18N_V1: ответ печатается на языке того, кому он адресован:
            //   игрок → его locale из ClientInformation, консоль/RCON → language= из конфига.
            using nc::i18n::Lang;
            auto __findPlayer = [this](const std::string& want) -> std::shared_ptr<entity::Player> {
                std::string lw = want;
                for (auto& c : lw) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                for (auto& p : getAllPlayersCopy()) {
                    if (!p || !p->isAlive() || p->getState() != entity::PlayerState::Play) continue;
                    std::string nm = p->getName();
                    for (auto& c : nm) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                    if (nm == lw) return p;
                }
                return nullptr;
            };
            auto __target = [__findPlayer](nc::cmd::CommandContext& ctx, size_t idx) -> std::shared_ptr<entity::Player> {
                if (ctx.args.size() > idx) return __findPlayer(ctx.args[idx]);
                if (!ctx.isConsole && !ctx.playerName.empty()) return __findPlayer(ctx.playerName);
                return nullptr;
            };
            // Язык автора команды.
            auto __lang = [this, __findPlayer](const nc::cmd::CommandContext& ctx) -> Lang {
                if (!ctx.isConsole && !ctx.playerName.empty()) {
                    if (auto p = __findPlayer(ctx.playerName)) return nc::i18n::langFromLocale(p->clientLocale);
                }
                return config_.language == "rus" ? Lang::Ru : Lang::En;
            };

            // /kill [player]
            __reg.registerCommand({"kill", true, "core", "/kill [player]", [this, __target, __lang](nc::cmd::CommandContext& ctx) {
                const Lang lg = __lang(ctx);
                auto tgt = __target(ctx, 1);
                if (!tgt) { ctx.reply(std::string(nc::i18n::tr(lg, "kill.usage"))); return; }
                // Сообщение о смерти видит сам игрок — берём его язык.
                applyEnvironmentalDamage(tgt, 1000.0f, 9,
                                         nc::i18n::f(nc::i18n::langFromLocale(tgt->clientLocale), "kill.death", tgt->getName()));
                ctx.reply(nc::i18n::f(lg, "kill.ok", tgt->getName()));
            }});

            // /clear [player]
            __reg.registerCommand({"clear", true, "core", "/clear [player]", [this, __target, __lang](nc::cmd::CommandContext& ctx) {
                const Lang lg = __lang(ctx);
                auto tgt = __target(ctx, 1);
                if (!tgt) { ctx.reply(std::string(nc::i18n::tr(lg, "clear.usage"))); return; }
                i32 removed = 0;
                for (int s = 0; s < entity::Player::INV_SIZE; ++s) {
                    if (tgt->invItemId[s] != 0) removed += std::max(0, tgt->invCount[s]);
                    tgt->invItemId[s] = 0;
                    tgt->invCount[s] = 0;
                }
                for (int h = 0; h < 9; ++h) tgt->hotbarBlockState[h] = -1;
                tgt->cursorItemId = 0;
                tgt->cursorCount = 0;
                sendFullPlayerInventory(tgt);
                ctx.reply(nc::i18n::f(lg, "clear.ok", tgt->getName(), removed));
            }});

            // /effect give <player> <id> [sec] [amp] | /effect clear <player> [id]
            __reg.registerCommand({"effect", true, "core", "/effect give|clear <player> <id> [sec] [amp]", [this, __findPlayer, __lang](nc::cmd::CommandContext& ctx) {
                const Lang lg = __lang(ctx);
                if (ctx.args.size() < 3) { ctx.reply(std::string(nc::i18n::tr(lg, "effect.usage"))); return; }
                const std::string sub = ctx.args[1];
                auto tgt = __findPlayer(ctx.args[2]);
                if (!tgt) { ctx.reply(nc::i18n::f(lg, "player.notfound", ctx.args[2])); return; }
                auto num = [](const std::string& s, i32 def) { try { return static_cast<i32>(std::stol(s)); } catch (...) { return def; } };
                if (sub == "give") {
                    if (ctx.args.size() < 4) { ctx.reply(std::string(nc::i18n::tr(lg, "effect.usage.give"))); return; }
                    const i32 id  = num(ctx.args[3], -1);
                    const i32 sec = ctx.args.size() > 4 ? std::clamp(num(ctx.args[4], 30), 1, 100000) : 30;
                    const i32 amp = ctx.args.size() > 5 ? std::clamp(num(ctx.args[5], 0), 0, 255) : 0;
                    if (id < 0) { ctx.reply(std::string(nc::i18n::tr(lg, "effect.badid"))); return; }
                    addPlayerEffect(tgt, id, amp, sec * 20);
                    ctx.reply(nc::i18n::f(lg, "effect.given", id, amp + 1, sec, tgt->getName()));
                } else if (sub == "clear") {
                    if (ctx.args.size() > 3) {
                        const i32 id = num(ctx.args[3], -1);
                        removePlayerEffect(tgt, id);
                        ctx.reply(nc::i18n::f(lg, "effect.removed", id, tgt->getName()));
                    } else {
                        std::vector<i32> ids;
                        for (const auto& e : tgt->effects) ids.push_back(e.id);
                        for (i32 id : ids) removePlayerEffect(tgt, id);
                        ctx.reply(nc::i18n::f(lg, "effect.removed.all", ids.size(), tgt->getName()));
                    }
                } else {
                    ctx.reply(std::string(nc::i18n::tr(lg, "effect.usage")));
                }
            }});

            // /xp add|set <player> <amount> [levels]
            __reg.registerCommand({"xp", true, "core", "/xp add|set <player> <amount> [levels]", [__findPlayer, __lang](nc::cmd::CommandContext& ctx) {
                const Lang lg = __lang(ctx);
                if (ctx.args.size() < 4) { ctx.reply(std::string(nc::i18n::tr(lg, "xp.usage"))); return; }
                const std::string sub = ctx.args[1];
                auto tgt = __findPlayer(ctx.args[2]);
                if (!tgt) { ctx.reply(nc::i18n::f(lg, "player.notfound", ctx.args[2])); return; }
                i32 amount = 0;
                try { amount = static_cast<i32>(std::stol(ctx.args[3])); } catch (...) { ctx.reply(std::string(nc::i18n::tr(lg, "arg.number"))); return; }
                const bool levels = ctx.args.size() > 4 && (ctx.args[4] == "levels" || ctx.args[4] == "l");
                if (sub == "set") {
                    tgt->totalExperience = 0;
                    tgt->experienceLevel = 0;
                    tgt->experienceProgress = 0.0f;
                    syncExperienceBar(tgt);
                }
                if (levels) {
                    tgt->experienceLevel = std::max(0, tgt->experienceLevel + amount);
                    syncExperienceBar(tgt);
                } else if (amount > 0) {
                    grantExperience(tgt, amount);
                } else {
                    syncExperienceBar(tgt);
                }
                ctx.reply(nc::i18n::f(lg, "xp.ok", tgt->getName(), tgt->experienceLevel, tgt->totalExperience));
            }});

            // /msg | /tell | /w <player> <text>
            auto __whisper = [__findPlayer, __lang](nc::cmd::CommandContext& ctx) {
                const Lang lg = __lang(ctx);
                if (ctx.args.size() < 3) { ctx.reply(std::string(nc::i18n::tr(lg, "msg.usage"))); return; }
                auto tgt = __findPlayer(ctx.args[1]);
                if (!tgt) { ctx.reply(nc::i18n::f(lg, "player.notfound", ctx.args[1])); return; }
                std::string text;
                for (size_t i2 = 2; i2 < ctx.args.size(); ++i2) { if (!text.empty()) text += ' '; text += ctx.args[i2]; }
                const std::string from = ctx.isConsole ? std::string("Console") : ctx.playerName;
                tgt->sendSystemMessage(nc::i18n::f(nc::i18n::langFromLocale(tgt->clientLocale), "msg.in", from, text));
                ctx.reply(nc::i18n::f(lg, "msg.out", tgt->getName(), text));
            };
            for (const char* alias : {"msg", "tell", "w"})
                __reg.registerCommand({alias, false, "core", "/msg <player> <text>", __whisper});

            // /me <action>
            __reg.registerCommand({"me", false, "core", "/me <action>", [this, __lang](nc::cmd::CommandContext& ctx) {
                if (ctx.args.size() < 2) { ctx.reply(std::string(nc::i18n::tr(__lang(ctx), "me.usage"))); return; }
                std::string text;
                for (size_t i2 = 1; i2 < ctx.args.size(); ++i2) { if (!text.empty()) text += ' '; text += ctx.args[i2]; }
                const std::string who = ctx.isConsole ? std::string("Console") : ctx.playerName;
                const std::string line = std::format("§d* {} {}", who, text);
                for (auto& p : getAllPlayersCopy())
                    if (p && p->isAlive() && p->getState() == entity::PlayerState::Play) p->sendSystemMessage(line);
                NC_INFO("Chat", "* {} {}", who, text);
            }});

            // /seed
            __reg.registerCommand({"seed", false, "core", "/seed", [this, __lang](nc::cmd::CommandContext& ctx) {
                ctx.reply(nc::i18n::f(__lang(ctx), "seed.ok", config_.levelSeed));
            }});

            // /difficulty [peaceful|easy|normal|hard|0-3]
            __reg.registerCommand({"difficulty", true, "core", "/difficulty <peaceful|easy|normal|hard>", [this, __lang](nc::cmd::CommandContext& ctx) {
                static const char* kNames[4] = {"peaceful", "easy", "normal", "hard"};
                const Lang lg = __lang(ctx);
                if (config_.difficultyLocked) { // ALLPACKETS_V3: Lock Difficulty (0x19) now actually gates this
                    ctx.reply(std::string(nc::i18n::tr(lg, "diff.locked")));
                    return;
                }
                if (ctx.args.size() < 2) {
                    ctx.reply(nc::i18n::f(lg, "diff.current", kNames[std::clamp(config_.difficulty, 0, 3)], config_.difficulty));
                    return;
                }
                std::string v = ctx.args[1];
                for (auto& c : v) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                i32 d = -1;
                for (i32 k = 0; k < 4; ++k) if (v == kNames[k] || v == std::to_string(k)) d = k;
                if (d < 0) { ctx.reply(std::string(nc::i18n::tr(lg, "diff.usage"))); return; }
                config_.difficulty = d;
                net::Buffer df;
                df.writeByte(static_cast<i8>(d));
                df.writeBool(false);
                const auto bytes = std::vector<u8>(df.writtenSpan().begin(), df.writtenSpan().end());
                for (auto& p : getAllPlayersCopy())
                    if (p && p->isAlive() && p->getState() == entity::PlayerState::Play) p->getConnection()->sendPacket(0x0B, bytes);
                ctx.reply(nc::i18n::f(lg, "diff.set", kNames[d]));
                NC_INFO("Server", "Difficulty set to {} ({})", kNames[d], d);
            }});

            // /ban <player> [reason]
            __reg.registerCommand({"ban", true, "core", "/ban <player> [reason]", [__findPlayer, __lang](nc::cmd::CommandContext& ctx) {
                const Lang lg = __lang(ctx);
                if (ctx.args.size() < 2) { ctx.reply(std::string(nc::i18n::tr(lg, "ban.usage"))); return; }
                const std::string who = ctx.args[1];
                std::string reason;
                for (size_t i2 = 2; i2 < ctx.args.size(); ++i2) { if (!reason.empty()) reason += ' '; reason += ctx.args[i2]; }
                const std::string by = ctx.isConsole ? std::string("Console") : ctx.playerName;
                if (!nc::BanManager::instance().ban(who, {}, by, reason)) {
                    ctx.reply(nc::i18n::f(lg, "ban.already", who));
                    return;
                }
                if (auto tgt = __findPlayer(who)) {
                    const Lang tl = nc::i18n::langFromLocale(tgt->clientLocale);
                    tgt->kick(reason.empty() ? std::string(nc::i18n::tr(tl, "ban.kick"))
                                             : nc::i18n::f(tl, "ban.kick.reason", reason));
                }
                ctx.reply(nc::i18n::f(lg, "ban.ok", who, reason.empty() ? std::string() : (" — " + reason)));
                NC_INFO("Bans", "ban {} by {} ({})", who, by, reason.empty() ? "no reason" : reason);
            }});

            // /pardon (/unban) <player>
            auto __pardon = [__lang](nc::cmd::CommandContext& ctx) {
                const Lang lg = __lang(ctx);
                if (ctx.args.size() < 2) { ctx.reply(std::string(nc::i18n::tr(lg, "pardon.usage"))); return; }
                const bool ok = nc::BanManager::instance().pardon(ctx.args[1]);
                ctx.reply(nc::i18n::f(lg, ok ? "pardon.ok" : "pardon.not", ctx.args[1]));
            };
            __reg.registerCommand({"pardon", true, "core", "/pardon <player>", __pardon});
            __reg.registerCommand({"unban", true, "core", "/unban <player>", __pardon});

            // /banlist
            __reg.registerCommand({"banlist", true, "core", "/banlist", [__lang](nc::cmd::CommandContext& ctx) {
                const Lang lg = __lang(ctx);
                const auto all = nc::BanManager::instance().list();
                if (all.empty()) { ctx.reply(std::string(nc::i18n::tr(lg, "banlist.empty"))); return; }
                std::string line = nc::i18n::f(lg, "banlist.head", all.size());
                for (size_t i2 = 0; i2 < all.size(); ++i2)
                    line += all[i2].name + (i2 + 1 < all.size() ? ", " : "");
                ctx.reply(line);
            }});

            // /tp <player> <x y z> | /tp <player> <target>
            __reg.registerCommand({"tp", true, "core", "/tp <player> <x y z | target>", [this, __findPlayer, __lang](nc::cmd::CommandContext& ctx) {
                const Lang lg = __lang(ctx);
                if (ctx.args.size() < 3) { ctx.reply(std::string(nc::i18n::tr(lg, "tp.usage"))); return; }
                auto who = __findPlayer(ctx.args[1]);
                if (!who) { ctx.reply(nc::i18n::f(lg, "player.notfound", ctx.args[1])); return; }
                if (ctx.args.size() >= 5) {
                    try {
                        const f64 nx = std::stod(ctx.args[2]);
                        const f64 ny = std::stod(ctx.args[3]);
                        const f64 nz = std::stod(ctx.args[4]);
                        if (nx < -29999984.0 || nx > 29999984.0 || nz < -29999984.0 || nz > 29999984.0 || ny < -63.0 || ny > 319.0) {
                            ctx.reply(std::string(nc::i18n::tr(lg, "tp.oob")));
                            return;
                        }
                        teleportSafe(who, nx, ny, nz, false);
                        ctx.reply(nc::i18n::f(lg, "tp.coords", who->getName(), nx, ny, nz));
                    } catch (...) { ctx.reply(std::string(nc::i18n::tr(lg, "tp.nan"))); }
                    return;
                }
                auto dst = __findPlayer(ctx.args[2]);
                if (!dst) { ctx.reply(nc::i18n::f(lg, "player.notfound", ctx.args[2])); return; }
                teleportSafe(who, dst->getX(), dst->getY(), dst->getZ(), false);
                ctx.reply(nc::i18n::f(lg, "tp.player", who->getName(), dst->getName()));
            }});

            // /commands
            __reg.registerCommand({"commands", false, "core", "/commands", [__lang](nc::cmd::CommandContext& ctx) {
                const auto all = nc::cmd::CommandRegistry::instance().all();
                std::string line = nc::i18n::f(__lang(ctx), "cmds.head", all.size());
                for (size_t i2 = 0; i2 < all.size(); ++i2)
                    line += all[i2].name + (i2 + 1 < all.size() ? ", " : "");
                ctx.reply(line);
            }});

            // /ops — показать текущий список операторов
            __reg.registerCommand({"ops", true, "core", "/ops", [](nc::cmd::CommandContext& ctx) {
                const auto all = nc::OpManager::instance().list();
                if (all.empty()) { ctx.reply("§eops.json пуст"); return; }
                std::string line = std::format("§aОператоров: {} — ", all.size());
                for (size_t i = 0; i < all.size(); ++i)
                    line += std::format("{}({}){}", all[i].name, all[i].level, i + 1 < all.size() ? ", " : "");
                ctx.reply(line);
            }});
        }
        NC_INFO("Ops", "ops.json: {} operator(s) loaded from {}",
                nc::OpManager::instance().list().size(), pathU8(nc::OpManager::instance().filePath()));
    }

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
            "    ███╔╝ █���███╗  ██║   ██║██║   ██║██║   ██║██████╔╝ ╚████╔╝ ██╔██╗ ██║",
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

    { // LANGFILE_V1 + LANGCHECK_V1: lang/eng.json, lang/rus.json, lang/Help.json
        const auto langReport = nc::i18n::loadLangPacks("lang");
        const bool ruLangLog = (config_.language == "rus");
        for (const auto& bad : langReport.invalid) {
            if (ruLangLog)
                NC_WARN("Lang", "Языковой файл {} повреждён и пропущен.", bad);
            else
                NC_WARN("Lang", "Language {} is invalid and was skipped.", bad);
        }
        const size_t langCount = langReport.codes.size();
        const bool listLangs = (langCount <= 6); // long lists stay a single line
        if (ruLangLog)
            NC_INFO("Lang", "Загружено языков: {}{}", langCount, listLangs ? ":" : "");
        else
            NC_INFO("Lang", "Loaded {} language(s){}", langCount, listLangs ? ":" : "");
        if (listLangs) {
            for (const auto& code : langReport.codes) NC_INFO("Lang", " - {}", code);
        }
    }

    { // CLEANEXIT_V1: сообщаем, как завершился прошлый сеанс, и ставим маркер «running»
        const bool ruExit = (config_.language == "rus");
        std::string prevExit;
        try { std::ifstream f("logs/last-exit.txt"); std::getline(f, prevExit); } catch (...) {}
        if (prevExit == "clean") {
            NC_INFO("Server", ruExit ? "Прошлый сеанс завершён корректно (через stop)" : "Previous session ended cleanly (stop)");
        } else if (prevExit == "running") {
            NC_WARN("Server", ruExit ? "Прошлый сеанс завершился АВАРИЙНО (краш или принудительное закрытие) — подробности в logs/crash-last.txt" : "Previous session ended ABNORMALLY (crash or force close) - see logs/crash-last.txt");
        }
        writeLastExitFile("running", ruExit); // CLEANEXIT_V2
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
        NC_WARN("Server", "Онлайн режим (Mojang auth) пока не реализован: нужны RSA/AES-шифрование и session-сервер Mojang. С��р��ер работает как offline.");
    }

    g_startProfile.lap("Config"); // STARTPROF_V1
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

    { // DIMSAVE_V1: world/ = родитель, внутри overworld/ nether/ end/
        std::error_code dsec;
        std::filesystem::create_directories("world/overworld", dsec);
        std::filesystem::create_directories("world/nether", dsec);
        std::filesystem::create_directories("world/end", dsec);
        if (std::filesystem::exists("world/world.dat", dsec) &&
            !std::filesystem::exists("world/overworld/world.dat", dsec))
            std::filesystem::rename("world/world.dat", "world/overworld/world.dat", dsec);
    }
    // WORLDSAVE_V1: сначала пробуем загрузить сохранённый мир
    if (world_.startBackgroundLoad("world/overworld/world.dat")) { // FASTBOOT_V1: header on main thread, bodies stream in background
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
        else { g_spawnX = 0; g_spawnY = 4; g_spawnZ = 0; } // FLATNATIVE_V1
        writeWorldSpawn(g_spawnX, g_spawnY, g_spawnZ);
        saveWorlds(); // DIMSAVE_V1
        if (ru) NC_INFO("Server", "Новый мир создан и сохранён в world/world.dat");
        else    NC_INFO("Server", "New world created and saved to world/world.dat");
        if (ru) NC_INFO("Server", "Мировой спавн: {} {} {}", g_spawnX, g_spawnY, g_spawnZ);
        else    NC_INFO("Server", "World spawn: {} {} {}", g_spawnX, g_spawnY, g_spawnZ);
    }

    g_startProfile.lap("World snapshot"); // STARTPROF_V1
    prepareAllDimensions(); // WORLDPREP_V1: Ад и Энд готовы до приёма игроков
    g_startProfile.lap("Spawn prep"); // STARTPROF_V1

    network_.onConnection([this](auto conn) { onPlayerConnect(conn); });
    network_.onDisconnect([this](auto conn) { onPlayerDisconnect(conn); });
    network_.onPacket([this](auto conn, auto& data, auto id) { onPacketReceived(conn, data, id); });

    if (!network_.start(static_cast<u16>(config_.port))) {
        // PORTLOCK_V1: второй сервер на ТОМ ЖЕ порту не запускается. Другие порты — сколько угодно.
        if (ru) {
            NC_FATAL("Server", "Порт {} уже занят — сервер не запущен", config_.port);
            NC_FATAL("Server", "Скорее всего уже запущен другой сервер на этом же порту.");
            NC_FATAL("Server", "Закрой его или поменя�� port= в settings.properties (например {}).", config_.port + 1);
        } else {
            NC_FATAL("Server", "Port {} is already in use — server not started", config_.port);
            NC_FATAL("Server", "Another server is most likely already running on this port.");
            NC_FATAL("Server", "Close it or change port= in settings.properties (for example {}).", config_.port + 1);
        }
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
    // WHITELIST_V1
    {
        std::filesystem::path wlPath = "whitelist.txt";
        if (!configPath_.empty()) {
            auto cp = std::filesystem::path(configPath_);
            wlPath = cp.parent_path().empty() ? std::filesystem::path("whitelist.txt") : (cp.parent_path() / "whitelist.txt");
        }
        whitelist_.setPath(wlPath.string());
        whitelist_.load();
        NC_INFO("Server", "WHITELIST_V1: {} entries:{}", config_.whiteList ? "on" : "off", whitelist_.size());
    }

    // RCON_BRIDGE_V1: core/rcon.hpp::RconServer был полностью написан, но никогда не запускался —
    // enable-rcon в settings.properties молча ни на что не влиял. Запускаем его здесь и
    // мостим каждую RCON-команду на тик-поток через ту же очередь, что и консоль.
    if (config_.enableRcon) {
        bool rconStarted = rcon_.start(static_cast<u16>(config_.rconPort), config_.rconPassword,
            config_.rconMaxClients, [this](const std::string& cmd) -> std::string {
                // RCONQUIET_V2: в консоль попадает только то, что ввёл человек.
                // Команды моста (панель сама спрашивает list/stats/whitelist) идут в debug.
                if (config_.rconLogCommands) NC_DEBUG("RCON", "Executing command: {}", cmd);
                auto result = std::make_shared<std::promise<std::string>>();
                auto future = result->get_future();
                queueConsoleCommand(cmd, result);
                if (future.wait_for(std::chrono::seconds(5)) == std::future_status::timeout) {
                    return "Command timed out";
                }
                try { return future.get(); } catch (...) { return "RCON error"; }
            });
        if (!rconStarted) {
            if (config_.language == "rus") NC_WARN("Server", "RCON включён в конфиге, но не запустился (см. лог RCON выше)");
            else NC_WARN("Server", "RCON is enabled in config but failed to start (see RCON log above)");
        }
    }
    NC_INFO("Server", "Default gamemode: {}", config_.gamemode);
    NC_INFO("Server", "Difficulty: {} ({})", config_.difficulty,
        config_.difficulty == 0 ? "Peaceful" :
        config_.difficulty == 1 ? "Easy" :
        config_.difficulty == 2 ? "Normal" : "Hard");
    NC_INFO("Server", "Language: {}", lang);
    NC_INFO("Server", "View distance: {} chunks", config_.viewDistance);
    g_startProfile.lap("Network"); // STARTPROF_V1
    NC_INFO("Server", "Max players: {}", config_.maxPlayers);
    std::cout << "\n";

    // Ссылки
    std::cout << "\033[36m";
    std::cout << "  - GitHub:     https://github.com/Zevvoryn/Zevvoryn\n";
    std::cout << "\033[0m\n";

    g_startProfile.lap("Plugins"); // STARTPROF_V1
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();
    double seconds = elapsed / 1000.0;
    g_startProfile.dump(static_cast<long long>(elapsed)); // STARTPROF_V1

    if (config_.language == "rus") {
        NC_INFO("Server", "Done! (\xd0\xb7\xd0\xb0 {:.2f} \xd1\x81\xd0\xb5\xd0\xba.) \xd0\xa2\xd0\xb8\xd0\xbf \xe2\x80\x9chelp\xe2\x80\x9d \xd0\xb4\xd0\xbb\xd1\x8f \xd1\x81\xd0\xbf\xd1\x80\xd0\xb0\xd0\xb2\xd0\xba\xd0\xb8.", seconds);
    } else {
        NC_INFO("Server", "Done! ({}s) Type 'help' for a list of commands.", seconds);
    }
    std::cout << "\n";

    // CHATASYNC_V1: spin up the dedicated chat broadcast worker so player chat
    // never blocks the tick thread again.
    chatRunning_.store(true, std::memory_order_release);
    chatThread_ = std::thread([this] { chatWorkerLoop(); });

    return true;
}

// CHATASYNC_V1: producer side — called from the packet/tick thread. Just parks
// the finished line in the queue and wakes the worker; returns immediately.
void NetherCraftServer::enqueueChatBroadcast(std::string line) {
    {
        std::lock_guard<std::mutex> lk(chatMutex_);
        chatQueue_.push_back(std::move(line));
    }
    chatCv_.notify_one();
}

// CHATASYNC_V1: consumer side — runs on its own thread. Sends each chat line to
// every player here, off the tick loop, so a slow client can't stall the world.
void NetherCraftServer::chatWorkerLoop() {
    while (chatRunning_.load(std::memory_order_acquire)) {
        std::string line;
        {
            std::unique_lock<std::mutex> lk(chatMutex_);
            chatCv_.wait(lk, [this] {
                return !chatQueue_.empty() || !chatRunning_.load(std::memory_order_acquire);
            });
            if (!chatRunning_.load(std::memory_order_acquire) && chatQueue_.empty()) return;
            line = std::move(chatQueue_.front());
            chatQueue_.pop_front();
        }
        // getAllPlayersCopy() returns shared_ptr copies, so every player/connection
        // stays alive for the duration of this broadcast even if they disconnect.
        for (auto& p : getAllPlayersCopy()) {
            if (p && p->isAlive() && p->getState() == entity::PlayerState::Play) {
                p->sendSystemMessage(line);
            }
        }
    }
}

void NetherCraftServer::stop() {
    // STOPONCE_V1: stop() зовут из консольной команды stop, из main() после join
    // и из деструктора — мир сохранялся ТРИЖДЫ. Теперь работает только первый вызов.
    bool expectedStop = false;
    if (!stoppedOnce_.compare_exchange_strong(expectedStop, true)) return;
    NC_INFO("Server", "Stopping server..."); // STOPLOG_V1: печатается ровно один раз (после guard)
    // ASYNCSAVE_V1: дождаться фонового автосейва, чтобы не писать world.dat из двух потоков
    // KICKFIRST_V1: кикаем игроков ДО сохранения. До этого stop() шёл saveWorlds() (~10+ сек),
    // потом kick — но Windows даёт CTRL_CLOSE_EVENT всего ~5 сек: процесс убивался до кика.
    {
        const bool ruStop = (config_.language == "rus");
        for (auto& p : getAllPlayersCopy()) {
            if (p) p->kick(ruStop ? "§cСервер остановлен" : "§cServer closed");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // даём writer-потокам дослать Disconnect
    }
    // ASYNCSAVE_V1: дождаться фонового автосейва
    if (saveThread_.joinable()) saveThread_.join();
    // WORLDSAVE_V1 + KICKFIRST_V1: сохраняем мир после кика
    // WINSAVE_V1: надпись ИДЁТ ДО сейва. Раньше сообщение печаталось только ПОСЛЕ записи,
    // и если Windows убивал процесс по крестику (таймаут), игрок не видел ничего
    // и мир откатывался к прошлому автосейву.
    {
        const bool ruSave = (config_.language == "rus");
        const i32 chunkCount = static_cast<i32>(world_.getLoadedChunkCount());
        if (chunkCount > 0) {
            if (ruSave) NC_INFO("Server", "Сохранение мира... ({} чанков, не закрывайте окно)", chunkCount);
            else NC_INFO("Server", "Saving world... ({} chunks, do not close the window)", chunkCount);
            const auto tSave0 = std::chrono::steady_clock::now();
            saveWorlds(); // DIMSAVE_V1
            for (auto& p2 : getAllPlayersCopy()) savePlayerData(p2);
            const f64 saveMs = std::chrono::duration<f64, std::milli>(
                std::chrono::steady_clock::now() - tSave0).count();
            if (ruSave) NC_INFO("Server", "Мир сохранён в world/overworld/world.dat за {:.0f} мс", saveMs);
            else NC_INFO("Server", "World saved to world/overworld/world.dat in {:.0f} ms", saveMs);
        } else if (ruSave) {
            NC_WARN("Server", "Сохранять нечего: ни один чанк не загружен (старый world.dat остался как был)");
        } else {
            NC_WARN("Server", "Nothing to save: no chunks loaded (existing world.dat left untouched)");
        }
    }
    // STOPKICK_V1->KICKFIRST_V1: кик уже выше, запоминаем заголовок для истории маркеров
    // дослать Disconnect до закрытия сети — игрок видит «Сервер остановлен».
    {
        const bool ruStop = (config_.language == "rus");
        for (auto& p : getAllPlayersCopy()) {
            if (p) p->kick(ruStop ? "§cСервер остановлен" : "§cServer closed");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    network_.stop();
    rcon_.stop(); // RCON_BRIDGE_V1

    // CHATASYNC_V1: stop and join the chat worker cleanly so no send happens
    // against torn-down connections during shutdown.
    chatRunning_.store(false, std::memory_order_release);
    chatCv_.notify_all();
    if (chatThread_.joinable()) chatThread_.join();

    // CLEANEXIT_V1: пом��тка «завершились штатно» — читается на следующем старте
    writeLastExitFile("clean", config_.language == "rus"); // CLEANEXIT_V2
}

void NetherCraftServer::queueConsoleCommand(std::string command, std::shared_ptr<std::promise<std::string>> result) {
    std::lock_guard lock(consoleMutex_);
    if (!command.empty()) consoleCommands_.push_back(ConsoleCommandItem{std::move(command), std::move(result)});
}

void NetherCraftServer::run() {
    // TPS20_V1: drift-free tick scheduler + high-resolution timer on Windows.
#ifdef _WIN32
    timeBeginPeriod(1);
    // SCHEDFIX_V1: THREAD_PRIORITY_HIGHEST(=2). tick() делает ~0.1мс работы, но планировщик
    // раньше регулярно будил поток поздно (интервал 63мс вместо 50 → TPS 19.8).
    // SCHEDFIX_V2: TIME_CRITICAL(=15). Под штурмом 500 ботов работает ~1000 сетевых потоков,
    // и HIGHEST(=2) всё ещё вытеснялся IO-boost'нутыми потоками (гэпы 70-109мс в логе).
    // tick() занимает <2мс из 50мс бюджета, так что 15 безопасен.
    SetThreadPriority(GetCurrentThread(), 15);
    // TPS20_V3: главный фикс стабильных 63мс. Windows 11 может ИГНОРИРОВАТЬ timeBeginPeriod(1)
    // (power throttling), тогда sleep_until будит поток с шагом ~15.6мс -> интервал 50+13=63мс
    // при мгновенном tick() — ровно то, что видно в логе (62.7-63.8мс каждые 5с). Два фикса:
    // (1) явно запрещаем ОС игнорировать наш запрос разрешения таймера;
    // (2) ждём на высокоточном waitable-таймере (Win10 1803+), который даёт ~0.5мс точность
    //     НЕЗАВИСИМО от глобального разрешения системного таймера.
#ifdef PROCESS_POWER_THROTTLING_CURRENT_VERSION
    {
        PROCESS_POWER_THROTTLING_STATE ppts{};
        ppts.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        ppts.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        ppts.StateMask = 0; // 0 = НЕ игнорировать timeBeginPeriod этого процесса
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &ppts, sizeof(ppts));
    }
#endif
    HANDLE hrTimer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (!hrTimer) // старые Windows (<10 1803): обычный waitable-таймер, точность даст timeBeginPeriod(1)
        hrTimer = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_MANUAL_RESET, TIMER_ALL_ACCESS);
#endif
    using namespace std::chrono;
    const auto tickDuration = milliseconds(50);
    auto nextTick = steady_clock::now() + tickDuration;

    // TICKPROF_V1: замер интервала МЕЖДУ тиками — отделяет джиттер планировщика/ОС от медленного tick().
    auto _tp_prevStart = steady_clock::now();
    f64 _tp_gapMax = 0.0; int _tp_gapWin = 0;

    while (network_.isRunning()) {
        const auto _tp_now = steady_clock::now();
        const f64 _tp_gap = duration<f64, std::milli>(_tp_now - _tp_prevStart).count();
        _tp_prevStart = _tp_now;
        if (_tp_gap > _tp_gapMax) _tp_gapMax = _tp_gap;
        if (++_tp_gapWin >= 100) { // ~раз в 5с
            if (_tp_gapMax > 52.0)
                NC_DEBUG("TickProf", "макс. интервал МЕЖДУ тиками за 5с: {:.1f}мс (цель 50). Если tick() при этом быстрый — виноват планировщик/ОС, а не наш код", _tp_gapMax);
            _tp_gapMax = 0.0; _tp_gapWin = 0;
        }

        tick();

        nextTick += tickDuration; // fixed 50ms step so accumulated drift does not grow
        const auto now = steady_clock::now();
        if (now < nextTick) {
            // TPS20_V2: hybrid wait for a rock-solid 20.00 TPS. sleep_until alone
            // overshoots by up to the timer granularity (~1ms) each tick, dragging
            // the measured rate down to ~19.96. Sleep until ~1.2ms before the
            // deadline, then busy-spin the remainder for sub-ms accuracy.
            // SCHEDFIX_V1: раньше busy-spin был 1.2мс и жёг ядро (CPU ~57%% в соло),
            // делая поток мишенью для вытеснения. Стабильность тепер������ даёт приоритет
            // потока + timeBeginPeriod(1); микро-spin остаётся коротким (300мкс) для sub-ms точности.
            const auto spinFrom = nextTick - microseconds(300);
            if (now < spinFrom) {
#ifdef _WIN32
                // TPS20_V3: ждём на высокоточном таймере вместо sleep_until —
                // точность не зависит от глобального разрешения таймера (15.6мс).
                bool hrSlept = false;
                if (hrTimer) {
                    const long long remain100ns =
                        duration_cast<nanoseconds>(spinFrom - now).count() / 100;
                    if (remain100ns > 0) {
                        LARGE_INTEGER due; due.QuadPart = -remain100ns; // относительное время, шаг 100нс
                        if (SetWaitableTimer(hrTimer, &due, 0, nullptr, nullptr, FALSE)) {
                            WaitForSingleObject(hrTimer, 55); // страховочный таймаут < тика
                            hrSlept = true;
                        }
                    } else hrSlept = true; // осталось <100нс — сразу в микро-spin
                }
                if (!hrSlept) std::this_thread::sleep_until(spinFrom);
#else
                std::this_thread::sleep_until(spinFrom);
#endif
            }
            while (steady_clock::now() < nextTick) { /* tiny spin */ }
        } else if (now - nextTick > seconds(1)) {
            // Fell far behind (long generation stall) - resync to avoid a tick storm.
            nextTick = now + tickDuration;
        }
    }

#ifdef _WIN32
    if (hrTimer) CloseHandle(hrTimer); // TPS20_V3
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
        miniEdit_.removeSession(player->getEntityId()); // MINIEDIT_V1: clipboard/history are per connection
    }

    {
        std::lock_guard lock(playersMutex_);
        players_.erase(conn->getId());
    }
    network_.removeConnection(conn->getId());
    tabListDirty_.store(true, std::memory_order_relaxed); // STRESS_FIX_V1: раньше слали пакет мгновенно на каждый выход — шторм при churn
    // MEMFIX_V1: разорвать цикл shared_ptr Player <-> Connection.
    // Connection держит Player через userData_, Player держит Connection через connection_ —
    // без этого оба объекта (инвентарь, sentChunks_, буферы сокета) живут вечно после дисконнекта.
    conn->setData<entity::Player>(nullptr);
}

void NetherCraftServer::onPacketReceived(std::shared_ptr<net::Connection> conn, net::Buffer& data, i32 wireId) {
    auto player = conn->getData<entity::Player>();
    if (!player) return;

    auto state = conn->getConnectionState();
    // FLATWORLD_V1: убран лог-спам pkt wireId

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
    // Считаем только реальных игроков (в Play), а не все соединения
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

// SKIN_OFFLINE_V1 + SKINCMD_V1: «ник -> подписанные текстуры (скин+плащ)» через публичный
// Mojang API (name -> uuid -> signed profile). Блокирующе, best-effort: нет такого ника/нет сети — false.
static bool fetchSkinTextures(const std::string& name, std::string& outValue, std::string& outSig) {
    auto idResp = crypto::httpsGet("api.mojang.com", "/users/profiles/minecraft/" + name);
    if (!idResp) return false;
    std::string uuidHex;
    try {
        auto j = nlohmann::json::parse(*idResp);
        uuidHex = j.value("id", std::string());
    } catch (...) { return false; }
    if (uuidHex.empty()) return false;
    auto prof = crypto::httpsGet("sessionserver.mojang.com",
        "/session/minecraft/profile/" + uuidHex + "?unsigned=false");
    if (!prof) return false;
    try {
        auto j = nlohmann::json::parse(*prof);
        if (j.contains("properties") && j["properties"].is_array()) {
            for (auto& pr : j["properties"]) {
                if (pr.value("name", std::string()) == "textures") {
                    outValue = pr.value("value", std::string());
                    outSig   = pr.value("signature", std::string());
                }
            }
        }
    } catch (...) { return false; }
    return !outValue.empty();
}

// SKIN_OFFLINE_V1: offline (cracked) players have no Mojang session, so fetch
// their premium skin by their own username; non-premium names keep Steve/Alex.
static void fetchOfflineSkin(std::shared_ptr<entity::Player> player) {
    if (!player) return;
    if (fetchSkinTextures(player->getName(), player->texturesValue, player->texturesSignature))
        NC_DEBUG("Server", "{}: offline skin fetched", player->getName());
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

        // MAXPLAYERS_V1: лимит max-players раньше ВООБЩЕ не проверялся: в стресс-тесте на сервер
        // с лимитом 80 зашли все 500 ботов («Зашло 500/500, ошибок входа: 0»). Раздача
        // чанков на 500 сокетов забивала очереди отправки (до 8МБ на клиента) и память —
        // отсюда массовые 10054 и вероятный краш по памяти. Теперь лишних отшиваем на логине.
        {
            i32 online = 0;
            for (auto& p : getAllPlayersCopy()) {
                if (!p || !p->getConnection()) continue;
                auto st = p->getConnection()->getConnectionState();
                if (st == ConnectionState::Login || st == ConnectionState::Configuration || st == ConnectionState::Play) ++online;
            }
            if (online > config_.maxPlayers) { // счётчик включает это соединение, поэтому строго «>»
                net::Buffer db; // Login Disconnect (0x00): причина — JSON-строка
                // MAXPLAYERS_V2: текст по языку из конфига (rus/eng)
                db.writeString(config_.language == "rus"
                    ? "{\"text\":\"Сервер заполнен\",\"color\":\"red\"}"
                    : "{\"text\":\"Server is full\",\"color\":\"red\"}");
                player->getConnection()->sendPacket(0x00,
                    std::vector<u8>(db.writtenSpan().begin(), db.writtenSpan().end()));
                // GRACECLOSE_V1: раньше close() закрывал сокет до того, как поток-писатель
                // успевал отправить пакет — клиент видел «не удалось подключиться к серверу»
                // вместо причины. Теперь сокет закроется ПОСЛЕ отправки Login Disconnect.
                player->getConnection()->closeAfterFlush();
                NC_DEBUG("Server", "MAXPLAYERS_V1: {} otklonen, online {}/{}", name, online - 1, config_.maxPlayers);
                return;
            }
        }

        // BANMGR_V1: бан проверяется раньше белого списка
        if (auto __ban = nc::BanManager::instance().find(name)) {
            net::Buffer db;
            const auto __blang = nc::i18n::langFromLocale(player->clientLocale);
            std::string __why = __ban->reason.empty()
                                    ? std::string(nc::i18n::tr(__blang, "ban.kick"))
                                    : nc::i18n::f(__blang, "ban.kick.reason", __ban->reason);
            std::string __esc;
            for (char c : __why) { if (c == '"' || c == '\\') __esc.push_back('\\'); __esc.push_back(c); }
            db.writeString("{\"text\":\"" + __esc + "\",\"color\":\"red\"}");
            player->getConnection()->sendPacket(0x00, std::vector<u8>(db.writtenSpan().begin(), db.writtenSpan().end()));
            player->getConnection()->closeAfterFlush();
            NC_INFO("Server", "BANMGR_V1: {} is banned ({})", name, __why);
            return;
        }

        // WHITELIST_LOGIN_V1
        if (config_.whiteList) {
            bool isOp = false;
            if (!config_.ops.empty()) {
                std::string lname;
                for (char c : name) lname.push_back((char)::tolower((unsigned char)c));
                std::string csv = config_.ops;
                size_t pos = 0;
                while (pos <= csv.size()) {
                    size_t cm = csv.find(',', pos);
                    if (cm == std::string::npos) cm = csv.size();
                    std::string tok = csv.substr(pos, cm - pos);
                    auto a = tok.find_first_not_of(" \t");
                    auto b = tok.find_last_not_of(" \t");
                    if (a != std::string::npos && tok.substr(a, b - a + 1) == lname) { isOp = true; break; }
                    pos = cm + 1;
                }
            }
            if (!isOp && !whitelist_.allowed(name)) {
                net::Buffer db;
                db.writeString(config_.language == "rus"
                    ? "{\"text\":\"\xd0\x92\xd0\xb0\xd1\x81 \xd0\xbd\xd0\xb5\xd1\x82 \xd0\xb2 \xd0\xb1\xd0\xb5\xd0\xbb\xd0\xbe\xd0\xbc \xd1\x81\xd0\xbf\xd0\xb8\xd1\x81\xd0\xba\xd0\xb5\",\"color\":\"red\"}"
                    : "{\"text\":\"You are not white-listed on this server\",\"color\":\"red\"}");
                player->getConnection()->sendPacket(0x00, std::vector<u8>(db.writtenSpan().begin(), db.writtenSpan().end()));
                player->getConnection()->closeAfterFlush();
                NC_INFO("Server", "WHITELIST_LOGIN_V1: {} not in whitelist", name);
                return;
            }
        }

        // MP_DUP_V1: защита от входа под уже занятым ником — киваем старую сессию
        {
            const u64 myConnId = player->getConnection()->getId();
            auto existing = getAllPlayersCopy();
            for (auto& other : existing) {
                if (!other || !other->getConnection()) continue;
                if (other->getConnection()->getId() == myConnId) continue; // не себя
                if (other->getName() == name) {
                    NC_INFO("Server", "MP_DUP_V1: nik {} uzhe zanyat -> kick staroy sessii", name);
                    other->sendSystemMessage("§cВы вошли с другого места");
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

        // SKINPERSIST_V1: выбранный через /skin вид хранится в playerdata.
        // Грузим ДО Login Success, иначе при перезаходе Login Success снова отдаёт скин по нику.
        loadPlayerData(player);
        // SKIN_OFFLINE_V1: если игрок ещё не выбирал /skin, подхватываем скин по собственному нику.
        if (player->texturesValue.empty()) fetchOfflineSkin(player);

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
    } else if (wireId == 0x02) {
        // ALLPACKETS_V1: Login Plugin Response — принимаем без ошибки
        NC_DEBUG("Server", "Login Plugin Response from {} (ignored)", player->getName());
    } else if (wireId == 0x03) {
        // Login Acknowledged — кли��нт пер��ключился в Configuration
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
        { // DIFFICULTY_V1: без Change Difficulty клиент пишет "Local Difficulty: ??"
            net::Buffer df;
            df.writeByte(static_cast<i8>(config_.difficulty));
            df.writeBool(false);
            player->getConnection()->sendPacket(0x0B, std::vector<u8>(df.writtenSpan().begin(), df.writtenSpan().end()));
        }
        sendPlayerAbilities(player);
        sendPlayerPositionAndLook(player);
        syncExperienceBar(player);

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

        { // CMDTREE_V3: дерево строится из таблицы, новая команда появляется у клиента сама
            struct TreeCmd { std::string name; bool args; std::vector<std::string> subs; };
            std::vector<TreeCmd> cmds = {
                {"help", false, {}}, {"list", false, {}}, {"tps", false, {}},
                {"tp", true, {}}, {"spawn", false, {}}, {"setworldspawn", false, {}},
                {"setspawn", true, {}}, // SPAWNCMD_V1
                {"gamemode", true, {"survival","creative","adventure","spectator","0","1","2","3"}},
                {"gm0", true, {}}, {"gm1", true, {}}, {"gm2", true, {}}, {"gm3", true, {}},
                {"time", true, {"day","noon","night","midnight","set"}},
                {"weather", true, {"clear","rain","thunder"}},
                {"say", true, {}}, {"msg", true, {}}, {"kick", true, {}},
                {"setblock", true, {}}, {"skin", true, {}},
                {"nether", false, {}}, {"end", false, {}}, {"overworld", false, {}},
                {"summon", true, {}}, {"killall", false, {}},
                {"warprandomtick", true, {}}, {"save-all", false, {}},
                {"stop", false, {}}, {"reload", false, {}}, {"crash", false, {}},
            };
            for (const auto& __pc : nc::cmd::CommandRegistry::instance().all()) { // PLUGINCMD_V1: всё, что зарегистрировано, попадает в дерево
                bool __known = false;
                for (const auto& __c : cmds) if (__c.name == __pc.name) { __known = true; break; }
                if (!__known) cmds.push_back(TreeCmd{__pc.name, true, {}});
            }
            std::vector<i32> litIdx(cmds.size(), 0), argIdx(cmds.size(), -1);
            std::vector<std::vector<i32>> subIdx(cmds.size()), subArgIdx(cmds.size());
            i32 __next = 1;
            for (size_t i = 0; i < cmds.size(); ++i) {
                litIdx[i] = __next++;
                for (size_t s = 0; s < cmds[i].subs.size(); ++s) {
                    subIdx[i].push_back(__next++);
                    subArgIdx[i].push_back(cmds[i].args ? __next++ : -1);
                }
                if (cmds[i].args) argIdx[i] = __next++;
            }
            net::Buffer cb;
            cb.writeVarInt(__next);
            cb.writeByte(0x00); cb.writeVarInt(static_cast<i32>(cmds.size()));
            for (size_t i = 0; i < cmds.size(); ++i) cb.writeVarInt(litIdx[i]);
            auto __greedy = [&](const char* nm) { cb.writeByte(0x06); cb.writeVarInt(0); cb.writeString(nm); cb.writeVarInt(5); cb.writeVarInt(2); };
            for (size_t i = 0; i < cmds.size(); ++i) {
                const i32 __cc = static_cast<i32>(subIdx[i].size()) + (argIdx[i] >= 0 ? 1 : 0);
                cb.writeByte(0x05); cb.writeVarInt(__cc);
                for (i32 __ci : subIdx[i]) cb.writeVarInt(__ci);
                if (argIdx[i] >= 0) cb.writeVarInt(argIdx[i]);
                cb.writeString(cmds[i].name);
                for (size_t s = 0; s < cmds[i].subs.size(); ++s) {
                    cb.writeByte(0x05); cb.writeVarInt(subArgIdx[i][s] >= 0 ? 1 : 0);
                    if (subArgIdx[i][s] >= 0) cb.writeVarInt(subArgIdx[i][s]);
                    cb.writeString(cmds[i].subs[s]);
                    if (subArgIdx[i][s] >= 0) __greedy("args");
                }
                if (argIdx[i] >= 0) __greedy("args");
            }
            cb.writeVarInt(0);
            player->getConnection()->sendPacket(0x11, std::vector<u8>(cb.writtenSpan().begin(), cb.writtenSpan().end()));
            NC_DEBUG("Server", "  Command tree sent: {} nodes, {} commands (+{} ms)", __next, cmds.size(), playMs());
        }
        { // ENTEVT_V1 / OPMGR_V1: уровень прав оператора (F3+F4, доступность команд)
            const int _lvl = nc::opLevelOf(config_.ops, player->getName());
            net::Buffer ev;
            ev.writeI32(static_cast<i32>(player->getEntityId()));
            ev.writeByte(static_cast<u8>(24 + _lvl)); // Entity Event: 24 + op level (0..4)
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
            i32 r = config_.viewDistance; // VIEWDIST_V1: полный радиус, чтобы край мира не торчал до тумана
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
                writeInventoryStack(inv, id, cnt, player->builderWandOwned && id == 821);
            }
            inv.writeVarInt(0);          // предмет в курсоре: пусто
            player->getConnection()->sendPacket(0x13, std::vector<u8>(inv.writtenSpan().begin(), inv.writtenSpan().end()));
            net::Buffer carried; // выбранный слот хотбара
            carried.writeByte((i8)(player->heldSlot >= 0 && player->heldSlot < 9 ? player->heldSlot : 0));
            player->getConnection()->sendPacket(0x53, std::vector<u8>(carried.writtenSpan().begin(), carried.writtenSpan().end()));
        }

        { // VANILLA_JOIN_V8: ванильный клиент получает эти пакеты при входе в мир,
          // без каких-либо команд — просто как часть join-последовательности.
            { // Set Simulation Distance (0x62)
                net::Buffer sd;
                sd.writeVarInt(config_.simulationDistance);
                player->getConnection()->sendPacket(0x62, std::vector<u8>(sd.writtenSpan().begin(), sd.writtenSpan().end()));
            }
            { // Initialize World Border (0x25)
                net::Buffer wb;
                wb.writeF64(0.0); wb.writeF64(0.0);          // center x/z
                wb.writeF64(59999968.0); wb.writeF64(59999968.0); // old/new diameter (vanilla default)
                wb.writeVarLong(0);                          // speed (ms to reach new diameter)
                wb.writeVarInt(29999984);                    // portal teleport boundary
                wb.writeVarInt(5);                           // warning blocks
                wb.writeVarInt(15);                          // warning time (seconds)
                player->getConnection()->sendPacket(0x25, std::vector<u8>(wb.writtenSpan().begin(), wb.writtenSpan().end()));
            }
            { // Server Data (0x4B): MOTD component + optional icon (absent)
                net::Buffer sdata;
                writeTextComponent(sdata, config_.motd);
                sdata.writeBool(false);
                player->getConnection()->sendPacket(0x4B, std::vector<u8>(sdata.writtenSpan().begin(), sdata.writtenSpan().end()));
            }
            { // Update Tags (0x78): no extra play-state tag overrides
                net::Buffer tg;
                tg.writeVarInt(0);
                player->getConnection()->sendPacket(0x78, std::vector<u8>(tg.writtenSpan().begin(), tg.writtenSpan().end()));
            }
            { // Update Advancements (0x74): reset the client tree, we track no advancements
                net::Buffer adv;
                adv.writeBool(true); // reset/clear
                adv.writeVarInt(0);  // added advancements
                adv.writeVarInt(0);  // removed identifiers
                adv.writeVarInt(0);  // progress entries
                player->getConnection()->sendPacket(0x74, std::vector<u8>(adv.writtenSpan().begin(), adv.writtenSpan().end()));
            }
        }

        player->playReady = true; // JOINSAFE_V1: с этой секунды клиент готов к мировым пакетам
        onPlayerEnterPlay(player); // MP_V1: показать игрока другим и других — ему
    } else if (wireId == 0x07) {
        // select_known_packs from client → NOW send registries + tags + finish
        NC_DEBUG("Server", "Client sent select_known_packs (0x07), sending registries...");
        sendRegistryData(player);
        sendConfigurationFinish(player);
    } else if (wireId == 0x00) {
        // ClientInformation (Configuration) — locale, view distance, chat mode, etc.
        player->clientLocale = data.readString(); // I18N_V1: язык клиента ("ru_ru", "en_us", ...)
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
    } else if (wireId == 0x01) {
        // ALLPACKETS_V1: Cookie Response (config) — куки не запрашиваем
        NC_DEBUG("Server", "Cookie Response (config) from {} (ignored)", player->getName());
    } else if (wireId == 0x04) {
        // ALLPACKETS_V1: Keep Alive (config) — config keep-alive мы не инициируем
    } else if (wireId == 0x05) {
        // ALLPACKETS_V1: Pong (config) — config ping мы не шлём
    } else if (wireId == 0x06) {
        // ALLPACKETS_V1: Resource Pack Response (config) — ресурспак не навязываем
    } else {
        NC_INFO("Server", "UNHANDLED Config wireId=0x{:02X}", wireId);
    }
}

void NetherCraftServer::sendInitialConfig(std::shared_ptr<entity::Player> player) {
    // Minestom order: brand → select_known_packs → [wait client] → registries → tags → finish

    // 0. BRAND_V1: бренд сервера (Clientbound Plugin Message 0x01, канал minecraft:brand).
    // Без него в F3 второй строкой пишется "null" server. Мы читали бренд клиента, но свой не шлали.
    {
        net::Buffer buf;
        buf.writeString("minecraft:brand");
        buf.writeString(config_.brand.empty() ? std::string("Zevvoryn") : config_.brand);
        player->getConnection()->sendPacket(0x01,
            std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
        NC_DEBUG("Server", "Sent server brand to {}", player->getName());
    }

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

    { // DIMHEIGHT_V1: shlem SVOI dimension_type vmesto vanilnyh iz known pack.
      // U vanilnogo the_nether min_y=0 height=128, u the_end min_y=0 height=256,
      // a my vsegda shlem 24 sekcii ot Y=-64 -> klient chital ih so sdvigom i risoval
      // pol Ada/Enda na Y~70. Teper u vseh izmereniy min_y=-64, height=384.
        struct DimDef {
            const char* name; f32 ambient; bool bedWorks; f64 coordScale; const char* effects;
            bool hasCeiling; bool hasRaids; bool hasSkylight; const char* infiniburn;
            i32 monsterBlockLight; bool natural; bool piglinSafe; bool respawnAnchor;
            bool ultrawarm; i64 fixedTime; bool hasFixedTime;
        };
        static const DimDef kDims[4] = {
            {"overworld",       0.0f, true,  1.0, "minecraft:overworld", false, true,  true,
             "#minecraft:infiniburn_overworld",  0, true,  false, false, false,     0, false},
            {"overworld_caves", 0.0f, true,  1.0, "minecraft:overworld", true,  true,  true,
             "#minecraft:infiniburn_overworld",  0, true,  false, false, false,     0, false},
            {"the_end",         0.0f, false, 1.0, "minecraft:the_end",   false, true,  false,
             "#minecraft:infiniburn_end",        0, false, false, false, false,  6000, true},
            {"the_nether",      0.1f, false, 8.0, "minecraft:the_nether", true, false, false,
             "#minecraft:infiniburn_nether",    15, false, true,  true,  true,  18000, true},
        };
        net::Buffer dp;
        dp.writeString("minecraft:dimension_type");
        dp.writeVarInt(4);
        for (const auto& d : kDims) {
            dp.writeString(std::string("minecraft:") + d.name);
            dp.writeByte(1); // has_data = true
            nbt::TagWriter w;
            w.beginRootCompound();
            w.writeFloat(d.ambient, "ambient_light");
            w.writeByte(d.bedWorks ? 1 : 0, "bed_works");
            w.writeDouble(d.coordScale, "coordinate_scale");
            w.writeString(d.effects, "effects");
            if (d.hasFixedTime) w.writeLong(d.fixedTime, "fixed_time");
            w.writeByte(d.hasCeiling ? 1 : 0, "has_ceiling");
            w.writeByte(d.hasRaids ? 1 : 0, "has_raids");
            w.writeByte(d.hasSkylight ? 1 : 0, "has_skylight");
            w.writeInt(384, "height");
            w.writeString(d.infiniburn, "infiniburn");
            w.writeInt(384, "logical_height");
            w.writeInt(-64, "min_y");
            w.writeInt(d.monsterBlockLight, "monster_spawn_block_light_limit");
            w.writeInt(7, "monster_spawn_light_level");
            w.writeByte(d.natural ? 1 : 0, "natural");
            w.writeByte(d.piglinSafe ? 1 : 0, "piglin_safe");
            w.writeByte(d.respawnAnchor ? 1 : 0, "respawn_anchor_works");
            w.writeByte(d.ultrawarm ? 1 : 0, "ultrawarm");
            w.endCompound();
            auto dimNbt = w.toVector();
            dp.writeBytes(std::span<const u8>(dimNbt));
        }
        player->getConnection()->sendPacket(0x07,
            std::vector<u8>(dp.writtenSpan().begin(), dp.writtenSpan().end()));
        NC_DEBUG("Server", "Registry minecraft:dimension_type sent (4 entries, DIMHEIGHT_V1 min_y=-64 height=384)");
    }

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

    // FLUIDTAGS_V1: теги НЕ могут быть пустыми. Ванильный клиент проверяет
    // FluidState.is(FluidTags.WATER/LAVA) для pushing, swimming и jumpInLiquid.
    // BuiltInRegistries.FLUID в 1.21.1 (Fluids.java):
    // 0=empty, 1=flowing_water, 2=water, 3=flowing_lava, 4=lava.
    {
        net::Buffer buf;
        buf.writeVarInt(1);                    // one registry with tags
        buf.writeString("minecraft:fluid");   // registry key
        buf.writeVarInt(2);                    // two fluid tags

        buf.writeString("minecraft:water");
        buf.writeVarInt(2);
        buf.writeVarInt(1);                    // minecraft:flowing_water
        buf.writeVarInt(2);                    // minecraft:water

        buf.writeString("minecraft:lava");
        buf.writeVarInt(2);
        buf.writeVarInt(3);                    // minecraft:flowing_lava
        buf.writeVarInt(4);                    // minecraft:lava

        player->getConnection()->sendPacket(0x0D,
            std::vector<u8>(buf.writtenSpan().begin(), buf.writtenSpan().end()));
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

    player->dimension = 0; // MULTIWORLD_V1: the join info below describes the overworld

    // Entity ID
    buf.writeI32(static_cast<i32>(player->getEntityId()));

    // Is Hardcore
    buf.writeBool(false);

    // World Names count + names (MULTIWORLD_V1: client must know every level id up front,
    // otherwise a Respawn into the nether/end is rejected)
    buf.writeVarInt(3);
    buf.writeString("minecraft:overworld");
    buf.writeString("minecraft:the_nether");
    buf.writeString("minecraft:the_end");

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
// BLOCKS_V1: вспомогательные функции для блоков
// ============================================================
static void decodeBlockPos(u64 v, i32& x, i32& y, i32& z) {
    x = static_cast<i32>(static_cast<i64>(v) >> 38);
    y = static_cast<i32>((static_cast<i64>(v) << 52) >> 52);
    z = static_cast<i32>((static_cast<i64>(v) << 26) >> 38);
}

static i64 g_timeOfDay = 6000; // CMDS_V1: текущее время мира (для /time и Set Time)
static f64 g_timeOfDayAccum = 0.0; // DAYNIGHT_LENGTH_V1: дробный остаток тиков между секундами

// OPS_V1: проверка оператора (ops= в settings.properties, через запятую, без учёта регистра)
static bool isOpName(const std::string& opsCsv, const std::string& name) {
    // OPMGR_V1: ops.json is the primary source of truth; the legacy
    // ops= CSV in settings.properties keeps working as a fallback.
    if (nc::OpManager::instance().isOp(name)) return true;
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

// CHEST_V1: ди��пазоны block state сундуков 1.21.1.
static bool isChestBlockState(i32 st)   { return st >= 2954 && st <= 2977; } // chest
static bool isTrappedChestState(i32 st) { return st >= 9119 && st <= 9142; } // trapped_chest
static bool isEnderChestState(i32 st)   { return st >= 7513 && st <= 7520; } // ender_chest

// CHEST_V1: facing-блоки ставим «лицом к игроку» по yaw.
// Формулы стейтов 1.21.1: chest/trapped_chest = base + facing*6 (type=single, waterlogged=false);
// ender_chest/furnace = base + facing*2. Порядок facing: north=0, south=1, west=2, east=3.
static i32 orientBlockForPlacement(i32 defState, f32 yaw) {
    f32 y = std::fmod(yaw, 360.0f);
    if (y < 0.0f) y += 360.0f;
    const i32 dir = static_cast<i32>(std::floor(y / 90.0f + 0.5f)) & 3; // куда смотрит игрок: 0=юг 1=запад 2=север 3=восток
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

// CHEST_V2: смещение «по часово�� стрелке» от facing сундука (north=0,south=1,west=2,east=3).
// У LEFT-половины двойного сундука партнёр стоит по часовой, у RIGHT — против.
static void chestCwOffset(i32 f, i32& dx, i32& dz) {
    switch (f) {
        case 0:  dx = 1;  dz = 0;  break; // north -> east
        case 1:  dx = -1; dz = 0;  break; // south -> west
        case 2:  dx = 0;  dz = -1; break; // west -> north
        default: dx = 0;  dz = 1;  break; // east -> south
    }
}

// COMBAT_V2: базовый урон и скорость атаки по item id 1.21.1.
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

// ============================================================
// MINIEDIT_V1: built-in, dependency-free cuboid editor.
// Commands are a thin adapter; all world mutations/history live in miniedit.hpp.
// ============================================================

bool NetherCraftServer::isMiniEditWandHeld(const std::shared_ptr<entity::Player>& player) const {
    if (!player || player->heldSlot < 0 || player->heldSlot > 8) return false;
    const i32 slot = 36 + player->heldSlot;
    return player->invCount[slot] > 0 && player->invItemId[slot] == 821 &&
           (player->builderWandOwned || miniEdit_.wandEnabled(player->getEntityId()));
}

static void tntCountDecrement(std::atomic<i64>& counter); // ANTILAG_TNT_V1: defined below, used by publishMiniEditChanges

void NetherCraftServer::publishMiniEditChanges(std::span<const miniedit::BlockChange> changes, bool schedulePhysics) {
    struct SectionBatch { i32 sx=0, sy=0, sz=0; std::vector<std::pair<u16,i32>> blocks; };
    for (const auto& c : changes) { // ANTILAG_TNT_V1: keep the stationary TNT counter in sync
        if (c.before == 2095 && c.after != 2095) tntCountDecrement(tntBlockCount_);
        else if (c.before != 2095 && c.after == 2095) tntBlockCount_.fetch_add(1, std::memory_order_relaxed);
    }
    constexpr size_t kWindow = 65'536; // RAM_BATCH_V1: never mirror an 8M edit in one giant map
    const auto fluidState=[](i32 state){ return state>=80 && state<=111; };
    const auto fallingState=[](i32 state){
        return state==112 || state==117 || state==118 || state==9107 || state==9111 ||
               state==9115 || (state>=12744 && state<=12759);
    };
    for (size_t base=0; base<changes.size(); base+=kWindow) {
        const size_t end=std::min(changes.size(),base+kWindow);
        std::map<i64,SectionBatch> batches;
        for (size_t i=base;i<end;++i) {
            const auto& c=changes[i];
            const i32 sx=c.pos.x>>4, sy=c.pos.y>>4, sz=c.pos.z>>4;
            const i64 sectionPos=((static_cast<i64>(sx)&0x3FFFFFLL)<<42) |
                                 ((static_cast<i64>(sz)&0x3FFFFFLL)<<20) |
                                 (static_cast<i64>(sy)&0xFFFFFLL);
            auto& batch=batches[sectionPos]; batch.sx=sx; batch.sy=sy; batch.sz=sz;
            const u16 relative=static_cast<u16>(((c.pos.x&15)<<8)|((c.pos.z&15)<<4)|(c.pos.y&15));
            batch.blocks.emplace_back(relative,c.after);
            if (schedulePhysics) {
                if (c.after==0 || fluidState(c.before) || fluidState(c.after))
                    scheduleFluidNeighbors(c.pos.x,c.pos.y,c.pos.z);
                if ((c.after >= 2391 && c.after < 2872) || c.after == 2872)
                    scheduleFireUpdate(c.pos.x,c.pos.y,c.pos.z,1);
                if (c.after==0 || fallingState(c.after)) {
                    scheduleFallingBlockUpdate(c.pos.x,c.pos.y,c.pos.z,2);
                    scheduleFallingBlockUpdate(c.pos.x,c.pos.y+1,c.pos.z,2);
                    scheduleFallingColumnCascade(c.pos.x, c.pos.y + 1, c.pos.z, 2);
                }
            }
        }
        std::vector<MiniEditPacket> packets; packets.reserve(batches.size());
        for (const auto& [sectionPos,batch]:batches) {
            net::Buffer out(16+batch.blocks.size()*5); out.writeI64(sectionPos);
            out.writeVarInt(static_cast<i32>(batch.blocks.size()));
            for (const auto& [relative,state]:batch.blocks)
                out.writeVarLong((static_cast<i64>(state)<<12)|relative);
            packets.push_back({batch.sx,batch.sz,std::vector<u8>(out.writtenSpan().begin(),out.writtenSpan().end())});
        }
        std::lock_guard lock(miniEditPacketsMutex_);
        for (auto& packet:packets) miniEditPackets_.push_back(std::move(packet));
    }
}

void NetherCraftServer::flushMiniEditPackets() {
    // FASTASYNC_V1: cap network work per tick. A million-block edit may create
    // hundreds of section packets, but never monopolizes one server tick.
    std::vector<MiniEditPacket> batch;
    batch.reserve(64);
    {
        std::lock_guard lock(miniEditPacketsMutex_);
        while (!miniEditPackets_.empty() && batch.size() < 64) {
            batch.push_back(std::move(miniEditPackets_.front()));
            miniEditPackets_.pop_front();
        }
    }
    if (batch.empty()) return;
    const auto players = getAllPlayersCopy();
    for (const auto& packet : batch) for (const auto& p : players) {
        if (!p || !p->isAlive() || p->getState() != entity::PlayerState::Play) continue;
        const i32 pcx = static_cast<i32>(std::floor(p->getX())) >> 4;
        const i32 pcz = static_cast<i32>(std::floor(p->getZ())) >> 4;
        if (std::abs(pcx - packet.sx) <= config_.viewDistance + 1 &&
            std::abs(pcz - packet.sz) <= config_.viewDistance + 1)
            // WORLD_SYNC_V1: block-state packets are authoritative and must
            // never be shed like movement/particles under network pressure.
            p->getConnection()->sendPacket(0x49, packet.payload, false);
    }
}

void NetherCraftServer::sendMiniEditOutline(const std::shared_ptr<entity::Player>& player,
                                             const miniedit::Selection& selection) {
    if (!player || !selection.complete() || !player->getConnection()) return;
    const BlockPos lo = selection.min(), hiBlock = selection.max();
    const f64 x0 = lo.x, y0 = lo.y, z0 = lo.z;
    const f64 x1 = static_cast<f64>(hiBlock.x) + 1.0;
    const f64 y1 = static_cast<f64>(hiBlock.y) + 1.0;
    const f64 z1 = static_cast<f64>(hiBlock.z) + 1.0;

    auto particle = [&](f64 x, f64 y, f64 z, f32 xd, f32 yd, f32 zd, i32 count) {
        net::Buffer b(64);
        b.writeBool(true);                    // override limiter
        b.writeF64(x); b.writeF64(y); b.writeF64(z);
        b.writeF32(xd); b.writeF32(yd); b.writeF32(zd);
        b.writeF32(0); b.writeI32(count);
        b.writeVarInt(13);                    // minecraft:dust, registry id 13
        b.writeF32(1.0f); b.writeF32(0.0f); b.writeF32(0.0f); // RGB red
        b.writeF32(1.65f);                    // bright, readable dust scale
        player->getConnection()->sendPacket(0x29,
            std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()), true);
    };
    auto edge = [&](f64 ax, f64 ay, f64 az, f64 bx, f64 by, f64 bz) {
        const f64 length = std::sqrt((bx-ax)*(bx-ax) + (by-ay)*(by-ay) + (bz-az)*(bz-az));
        // A handful of dense particle clouds per edge is much brighter and uses
        // fewer packets than one packet for every faint point (old V10 path).
        const i32 parts = std::clamp(static_cast<i32>(std::ceil(length / 10.0)), 1, 8);
        for (i32 i = 0; i < parts; ++i) {
            const f64 ta = static_cast<f64>(i) / parts, tb = static_cast<f64>(i + 1) / parts;
            const f64 mx = ax + (bx-ax) * (ta + tb) * 0.5;
            const f64 my = ay + (by-ay) * (ta + tb) * 0.5;
            const f64 mz = az + (bz-az) * (ta + tb) * 0.5;
            const f32 xd = static_cast<f32>(std::abs((bx-ax) / parts) / 5.0);
            const f32 yd = static_cast<f32>(std::abs((by-ay) / parts) / 5.0);
            const f32 zd = static_cast<f32>(std::abs((bz-az) / parts) / 5.0);
            particle(mx, my, mz, xd, yd, zd, std::clamp(static_cast<i32>(std::ceil(length * 2.0 / parts)), 12, 32));
        }
    };
    // Twelve edges only: a sparse wireframe, never a filled particle cuboid.
    for (f64 y : {y0, y1}) {
        edge(x0,y,z0, x1,y,z0); edge(x0,y,z1, x1,y,z1);
        edge(x0,y,z0, x0,y,z1); edge(x1,y,z0, x1,y,z1);
    }
    edge(x0,y0,z0, x0,y1,z0); edge(x1,y0,z0, x1,y1,z0);
    edge(x0,y0,z1, x0,y1,z1); edge(x1,y0,z1, x1,y1,z1);
}

void NetherCraftServer::tickMiniEditVisuals() {
    if ((tickCounter_ % 20) != 0) return; // refresh transient particles once per second
    const auto visuals = miniEdit_.visuals();
    if (visuals.empty()) return;
    const auto players = getAllPlayersCopy();
    for (const auto& visual : visuals)
        for (const auto& p : players)
            if (p && p->getEntityId() == visual.playerKey) {
                sendMiniEditOutline(p, visual.selection);
                break;
            }
}

bool NetherCraftServer::handleMiniEditCommand(const std::shared_ptr<entity::Player>& player,
                                               const std::string& command, bool isOp) {
    std::istringstream input(command);
    std::vector<std::string> words;
    for (std::string word; input >> word;) words.push_back(std::move(word));
    if (words.empty()) return false;

    std::string sub;
    size_t arg = 1;
    if (!words[0].empty() && words[0][0] == '/') {
        sub = words[0];
        while (!sub.empty() && sub.front() == '/') sub.erase(sub.begin());
    } else if (words[0] == "edit" || words[0] == "we") {
        if (words.size() >= 2) { sub = words[1]; arg = 2; }
        else sub = "help";
    } else if (words[0] == "wand" || words[0] == "set" || words[0] == "replace" ||
               words[0] == "copy" || words[0] == "paste" || words[0] == "rotate" ||
               words[0] == "undo" || words[0] == "redo" || words[0] == "pos1" ||
               words[0] == "pos2" || words[0] == "pso") {
        sub = words[0];
    } else return false;

    for (char& c : sub) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (sub == "pso") sub = "pos1"; // convenience alias for the common typo requested by owner
    const bool miniRu = player->clientLocale.rfind("ru", 0) == 0;
    if (!isOp) {
        player->sendSystemMessage(miniRu ? "§cMiniEdit доступен только операторам" : "§cMiniEdit is available to operators only");
        return true;
    }


    auto mini = [&](std::string_view en, std::string_view ru) { return std::string(miniRu ? ru : en); };
    auto miniAction = [&](std::string_view a) {
        if (!miniRu) return std::string(a);
        if (a == "set") return std::string("установлено");
        if (a == "replace") return std::string("заменено");
        if (a == "copy") return std::string("скопировано");
        if (a == "paste") return std::string("вставлено");
        if (a == "undo") return std::string("отменено");
        if (a == "redo") return std::string("повторено");
        return std::string(a);
    };
    const u64 key = player->getEntityId();
    const BlockPos playerPos{static_cast<i32>(std::floor(player->getX())),
                             static_cast<i32>(std::floor(player->getY())),
                             static_cast<i32>(std::floor(player->getZ()))};
    miniedit::EditHooks hooks;
    hooks.publishChanges = [this](std::span<const miniedit::BlockChange> changes) {
        publishMiniEditChanges(changes);
    };
    auto resolveBlock = [](std::string token) -> std::optional<i32> {
        for (char& c : token) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        try {
            size_t used = 0;
            const long long id = std::stoll(token, &used);
            if (used == token.size() && id >= 0 && id <= 26683) return static_cast<i32>(id);
        } catch (...) {}
        constexpr std::string_view prefix = "minecraft:";
        if (token.starts_with(prefix)) token.erase(0, prefix.size());
        const i32 state = gen::blockNameToState(token);
        if (state >= 0) return state;
        return std::nullopt;
    };
    auto report = [&](const miniedit::EditResult& result, std::string_view action) {
        if (!result.ok) {
            std::string msg = result.message;
            if (miniRu && msg == "clipboard is empty") msg = "буфер обмена пуст";
            player->sendSystemMessage("§cMiniEdit: " + msg);
        } else if (result.affected > 0)
            player->sendSystemMessage(std::format("§aMiniEdit {}: {} {}", miniAction(action), result.affected, mini("blocks", "блоков")));
        else player->sendSystemMessage("§aMiniEdit: " + (result.message.empty() ? miniAction(action) : result.message));
    };

    if (sub == "help") {
        player->sendSystemMessage(miniRu ? "§6MiniEdit: //wand //pos1 //pos2 //set <блок> //replace <из> <в>"
                                         : "§6MiniEdit: //wand //pos1 //pos2 //set <block> //replace <from> <to>");
        player->sendSystemMessage(miniRu ? "§6//copy //rotate <90|180|270> //paste /undo /redo; алиасы: /edit, /we"
                                         : "§6//copy //rotate <90|180|270> //paste /undo /redo; aliases: /edit, /we");
    } else if (sub == "wand") {
        miniEdit_.setWandEnabled(key);
        const i32 slotIndex = 36 + std::clamp(player->heldSlot, 0, 8);
        player->invItemId[slotIndex] = 821; player->invCount[slotIndex] = 1;
        player->builderWandOwned = true;
        player->hotbarBlockState[slotIndex - 36] = -1;
        net::Buffer slot;
        slot.writeByte(0); slot.writeVarInt(0); slot.writeI16(static_cast<i16>(slotIndex));
        slot.writeVarInt(1); slot.writeVarInt(821);
        slot.writeVarInt(1); slot.writeVarInt(0); // one added component, no removals
        slot.writeVarInt(5);                      // minecraft:custom_name DataComponentType
        writeYellowWandName(slot);
        player->getConnection()->sendPacket(0x15, std::vector<u8>(slot.writtenSpan().begin(), slot.writtenSpan().end()));
        broadcastHeldEquipment(player);
        player->sendSystemMessage(miniRu ? "§aMiniEdit жезл: ЛКМ=pos1, ПКМ=pos2"
                                         : "§aMiniEdit wand: left click=pos1, right click=pos2");
    } else if (sub == "pos1" || sub == "pos2") {
        const int which = sub == "pos1" ? 1 : 2;
        const auto selection = miniEdit_.setPosition(key, which, playerPos);
        player->sendSystemMessage(std::format("§dMiniEdit pos{}: {} {} {}{}", which, playerPos.x, playerPos.y, playerPos.z,
            selection.complete() ? std::format(" ({} {})", selection.volume(), mini("blocks", "блоков")) : std::string()));
        if (selection.complete()) sendMiniEditOutline(player, selection);
    } else if (sub == "set") {
        if (words.size() <= arg) player->sendSystemMessage("§cИспользование: //set <block|state-id>");
        else if (auto state = resolveBlock(words[arg])) report(miniEdit_.set(key, *state, hooks), "set");
        else player->sendSystemMessage("§cНеизвестный блок или state ID");
    } else if (sub == "replace") {
        if (words.size() <= arg + 1) player->sendSystemMessage("§cИспользование: //replace <from> <to>");
        else {
            auto from = resolveBlock(words[arg]), to = resolveBlock(words[arg + 1]);
            if (!from || !to) player->sendSystemMessage("§cНеизвестный блок или state ID");
            else report(miniEdit_.replace(key, *from, *to, hooks), "replace");
        }
    } else if (sub == "copy") {
        report(miniEdit_.copy(key, playerPos), "copy");
    } else if (sub == "paste") {
        report(miniEdit_.paste(key, playerPos, hooks), "paste");
    } else if (sub == "rotate") {
        try { // MINIEDIT_ROTATE_V2: bare //rotate means 90 degrees, like WorldEdit
            const auto rotRes = miniEdit_.rotate(key, words.size() > arg ? std::stoi(words[arg]) : 90);
            if (!rotRes.ok) {
                const std::string why = (miniRu && rotRes.message.find("clipboard is empty") != std::string::npos)
                    ? "буфер пуст: выделите область (pos1/pos2), затем выполните //copy"
                    : (miniRu ? "не удалось повернуть" : rotRes.message);
                player->sendSystemMessage("§cMiniEdit: " + why);
            } else player->sendSystemMessage("§aMiniEdit " + rotRes.message +
                                          (miniRu ? " — выполните //paste для вставки повёрнутой копии"
                                                  : " — //paste to place the rotated copy"));
        }
             catch (...) { player->sendSystemMessage("§cПоворот: 90, 180 или 270"); }
    } else if (sub == "undo") {
        report(miniEdit_.undo(key, hooks), "undo");
    } else if (sub == "redo") {
        report(miniEdit_.redo(key, hooks), "redo");
    } else {
        player->sendSystemMessage("§cНеизвестная MiniEdit-команда. //wand или /edit help");
    }
    return true;
}

// ============================================================
// ITEMDROP_V1: выпавшие предметы. Сущность minecraft:item (id 60 в реестре 1.21.1),
// вид предмета задаёт метадата index 8 (сериализатор 7 = Slot). Физика — в tickItemDrops():
// гравитация, торможение, пол/стены; подбор = Take Item Entity (0x6F) + Remove Entities (0x42).
// DROPENTITY_FIX_V1: id 60 — это minecraft:item_frame (скрин показал рамки),
// а id 59 — настоящий minecraft:item в raw entity registry протокола 1.21.1.
static constexpr i32 ITEM_ENTITY_TYPE = 58;
static constexpr i32 FALLING_BLOCK_ENTITY_TYPE = 40;

static bool isFallingBlockState(i32 s) {
    return s == 112 || s == 113 || s == 117 || s == 118 || s == 119 || // sand/suspicious/red/gravel
           s == 7416 ||                                           // dragon egg (5 tick delay below)
           (s >= 9107 && s <= 9118) ||                            // three anvils, four facings each
           (s >= 12744 && s <= 12759);                            // concrete powders
}

static i32 bubbleColumnPush(const world::World& world, i32 x, i32 y, i32 z) {
    const i32 state = world.getBlock(x, y, z);
    if (state != 12960 && state != 12961) return 0;
    i32 baseY = y - 1;
    while (baseY >= world::CHUNK_HEIGHT_MIN) {
        const i32 below = world.getBlock(x, baseY, z);
        if (below == 12960 || below == 12961) { --baseY; continue; }
        if (below == 5850) return 1;   // soul_sand: upward column
        if (below == 12543) return -1; // magma_block: drag down
        return 0;
    }
    return 0;
}

static bool isAnvilState(i32 s) { return s >= 9107 && s <= 9118; }
static bool isConcretePowderState(i32 s) { return s >= 12744 && s <= 12759; }
static bool isWaterBlockState(i32 s) { return s >= 80 && s <= 95; }

// PLACE_V2: name of a block state, empty when the id is unknown.
static std::string_view stateNameOf(i32 s) {
    const auto* bs = registries::RegistryManager::instance().blockStates().getById(s);
    return bs ? std::string_view(bs->name) : std::string_view();
}

static bool stateHasProp(i32 s, const char* key, const char* value) {
    const auto* bs = registries::RegistryManager::instance().blockStates().getById(s);
    if (!bs) return false;
    auto it = bs->properties.find(key);
    return it != bs->properties.end() && it->second == value;
}

// PLACE_V2: Block#canBeReplaced - what a newly placed block may overwrite.
// Everything else must reject the placement, otherwise you can stack a door
// on a door and the client ends up showing a world the server does not have.
static bool isReplaceableState(i32 s) {
    if (s <= 0) return true;                    // air
    if (s >= 80 && s <= 111) return true;       // water / lava
    const std::string_view n = stateNameOf(s);
    if (n.empty()) return false;
    static const char* kReplaceable[] = {
        "minecraft:cave_air", "minecraft:void_air", "minecraft:short_grass", "minecraft:tall_grass",
        "minecraft:fern", "minecraft:large_fern", "minecraft:dead_bush", "minecraft:seagrass",
        "minecraft:tall_seagrass", "minecraft:fire", "minecraft:soul_fire", "minecraft:snow",
        "minecraft:vine", "minecraft:glow_lichen", "minecraft:light", "minecraft:structure_void",
        "minecraft:warped_roots", "minecraft:crimson_roots", "minecraft:nether_sprouts",
        "minecraft:hanging_roots", "minecraft:moss_carpet", "minecraft:bubble_column",
    };
    for (const char* r : kReplaceable) if (n == r) return true;
    return false;
}

// PLACE_V2: doors and tall plants are two stacked blocks; beds are two blocks
// side by side. Breaking one half has to take the other half with it.
static bool doubleBlockPartner(i32 s, i32& dx, i32& dy, i32& dz) {
    dx = 0; dy = 0; dz = 0;
    if (stateHasProp(s, "half", "upper")) { dy = -1; return true; }
    if (stateHasProp(s, "half", "lower")) { dy = 1; return true; }
    const auto* bs = registries::RegistryManager::instance().blockStates().getById(s);
    if (!bs) return false;
    auto partIt = bs->properties.find("part");
    auto faceIt = bs->properties.find("facing");
    if (partIt == bs->properties.end() || faceIt == bs->properties.end()) return false;
    i32 fx = 0, fz = 0;
    if (faceIt->second == "north") fz = -1;
    else if (faceIt->second == "south") fz = 1;
    else if (faceIt->second == "west") fx = -1;
    else if (faceIt->second == "east") fx = 1;
    else return false;
    const bool head = partIt->second == "head";
    dx = head ? -fx : fx;
    dz = head ? -fz : fz;
    return dx != 0 || dz != 0;
}

static bool isFireBlockState(i32 s) {
    const std::string_view n = stateNameOf(s);
    return n == "minecraft:fire" || n == "minecraft:soul_fire";
}

// PARTICLE_V2: blocks that must NOT play the block-break particle/sound effect.
// Fire is the obvious one - vanilla plays a small extinguish puff instead of
// exploding into "fire block" particles.
static bool isParticlelessBreakState(i32 s) {
    if (s <= 0) return true;
    if (s >= 80 && s <= 111) return true; // water / lava
    const std::string_view n = stateNameOf(s);
    if (n.empty()) return true;
    static const char* kSilent[] = {
        "minecraft:fire", "minecraft:soul_fire", "minecraft:barrier", "minecraft:light",
        "minecraft:structure_void", "minecraft:nether_portal", "minecraft:end_portal",
        "minecraft:end_gateway", "minecraft:cave_air", "minecraft:void_air",
        "minecraft:bubble_column", "minecraft:moving_piston", "minecraft:water", "minecraft:lava",
    };
    for (const char* q : kSilent) if (n == q) return true;
    return false;
}
static i32 concreteFromPowder(i32 s) { return 12728 + (s - 12744); }

// ConcretePowderBlock.shouldSolidify/touchesLiquid (1.21.1): the fluid already
// occupying the target counts, as do water neighbours on the four sides and
// above. Water only below does not harden a stationary powder block; it falls
// into that water first and FallingBlockEntity then detects direct contact.
static bool concretePowderShouldSolidify(const world::World& world, i32 x, i32 y, i32 z,
                                         i32 oldStateAtTarget) {
    if (isWaterBlockState(oldStateAtTarget)) return true;
    return isWaterBlockState(world.getBlock(x + 1, y, z)) ||
           isWaterBlockState(world.getBlock(x - 1, y, z)) ||
           isWaterBlockState(world.getBlock(x, y, z + 1)) ||
           isWaterBlockState(world.getBlock(x, y, z - 1)) ||
           isWaterBlockState(world.getBlock(x, y + 1, z));
}

// ConcretePowderBlock.updateShape for water introduced after the powder was
// already placed (bucket, /setblock, or another direct world mutation).
void NetherCraftServer::solidifyConcretePowderAround(i32 x, i32 y, i32 z) {
    static constexpr i32 DX[7] = {0, 1, -1, 0, 0, 0, 0};
    static constexpr i32 DY[7] = {0, 0, 0, 0, 0, 1, -1};
    static constexpr i32 DZ[7] = {0, 0, 0, 1, -1, 0, 0};
    for (i32 i = 0; i < 7; ++i) {
        const i32 px = x + DX[i], py = y + DY[i], pz = z + DZ[i];
        const i32 powder = world_.getBlock(px, py, pz);
        if (!isConcretePowderState(powder) ||
            !concretePowderShouldSolidify(world_, px, py, pz, powder)) continue;
        const i32 concrete = concreteFromPowder(powder);
        world_.setBlock(px, py, pz, concrete);
        scheduleFluidNeighbors(px, py, pz);
        scheduleFluidUpdate(px, py, pz, 1);
        scheduleFallingBlockUpdate(px, py + 1, pz, 2);
        scheduleFallingColumnCascade(px, py + 1, pz, 2);
        net::Buffer bu;
        bu.writePosition(BlockPos{px, py, pz});
        bu.writeVarInt(concrete);
        const auto bytes = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
        for (auto& player : getAllPlayersCopy())
            if (player && player->isAlive() && player->getState() == entity::PlayerState::Play)
                player->getConnection()->sendPacket(0x09, bytes);
    }
}

static bool isFreeForFalling(i32 s) {
        if (s <= 0 || (s >= 80 && s <= 111) || s == 12960 || s == 12961 || s == 2391 || s == 2872) return true;
    const auto* st = registries::RegistryManager::instance().blockStates().getById(s);
    if (!st) return false;
    const std::string& n = st->name;
    auto has = [&](std::string_view v) { return n.find(v) != std::string::npos; };
    return has("_sapling") || has("_flower") || has("_tulip") || has("_mushroom") ||
           has("_torch") || has("_rail") || has("_carpet") || has("_button") ||
           has("_pressure_plate") || has("_banner") || has("_coral") ||
           n == "minecraft:short_grass" || n == "minecraft:tall_grass" ||
           n == "minecraft:fern" || n == "minecraft:large_fern" ||
           n == "minecraft:dead_bush" || n == "minecraft:cobweb" ||
           n == "minecraft:vine" || n == "minecraft:glow_lichen" ||
           n == "minecraft:snow" || n == "minecraft:seagrass" ||
           n == "minecraft:tall_seagrass" || n == "minecraft:kelp" ||
           n == "minecraft:kelp_plant" || n == "minecraft:sugar_cane";
}

static void sendFallingBlockSpawnTo(const std::shared_ptr<entity::Player>& viewer,
                                    i32 eid, i32 state, f64 x, f64 y, f64 z,
                                    f64 vx, f64 vy, f64 vz) {
    if (!viewer || !viewer->isAlive() || viewer->getState() != entity::PlayerState::Play) return;
    net::Buffer sp;
    sp.writeVarInt(eid);
    sp.writeUUID(UUID{static_cast<u64>(eid), 0xF0000000ULL + static_cast<u64>(eid)});
    sp.writeVarInt(FALLING_BLOCK_ENTITY_TYPE);
    sp.writeF64(x); sp.writeF64(y); sp.writeF64(z);
    sp.writeByte(0); sp.writeByte(0); sp.writeByte(0);
    sp.writeVarInt(state); // ClientboundAddEntityPacket data = rendered block state
    auto vel = [](f64 v) { return static_cast<i16>(std::clamp(v * 8000.0, -32000.0, 32000.0)); };
    sp.writeI16(vel(vx)); sp.writeI16(vel(vy)); sp.writeI16(vel(vz));
    viewer->getConnection()->sendPacket(0x01,
        std::vector<u8>(sp.writtenSpan().begin(), sp.writtenSpan().end()));
}

static void sendItemDropSpawnTo(const std::shared_ptr<entity::Player>& viewer, i32 eid, i32 itemId, i32 count, f64 x, f64 y, f64 z, f64 vx, f64 vy, f64 vz) {
    if (!viewer || !viewer->isAlive() || viewer->getState() != entity::PlayerState::Play) return;
    net::Buffer sp;
    sp.writeVarInt(eid);
    sp.writeUUID(UUID{static_cast<u64>(eid), 0xD0000000ULL + static_cast<u64>(eid)});
    sp.writeVarInt(ITEM_ENTITY_TYPE);
    sp.writeF64(x); sp.writeF64(y); sp.writeF64(z);
    sp.writeByte(0); sp.writeByte(0); sp.writeByte(0); // pitch / yaw / head yaw
    sp.writeVarInt(0); // data
    auto vel = [](f64 v) { return static_cast<i16>(std::clamp(v * 8000.0, -32000.0, 32000.0)); };
    sp.writeI16(vel(vx)); sp.writeI16(vel(vy)); sp.writeI16(vel(vz));
    viewer->getConnection()->sendPacket(0x01, std::vector<u8>(sp.writtenSpan().begin(), sp.writtenSpan().end()));
    net::Buffer meta;
    meta.writeVarInt(eid);
    meta.writeByte(8);   // Item entity: index 8 = сам предмет
    meta.writeVarInt(7); // сериализатор 7: Slot
    meta.writeVarInt(count);
    meta.writeVarInt(itemId);
    meta.writeVarInt(0); meta.writeVarInt(0); // компонентов: +0 / -0
    meta.writeByte(0xFF); // конец метадаты
    viewer->getConnection()->sendPacket(0x58, std::vector<u8>(meta.writtenSpan().begin(), meta.writtenSpan().end()));
}

// ITEMDROP_V1: заспавнить предмет в мире и показать в��ем
void NetherCraftServer::spawnItemDrop(f64 x, f64 y, f64 z, i32 itemId, i32 count, f64 vx, f64 vy, f64 vz, i32 pickupDelay) {
    if (itemId <= 0 || count <= 0) return;
    const i32 eid = static_cast<i32>(nextEntityId_++);
    {
        std::lock_guard lk(itemDropsMutex_);
        if (itemDrops_.size() >= 2000) return; // предохранитель от лавины предметов
        itemDrops_.push_back({eid, itemId, count, x, y, z, vx, vy, vz, 0, pickupDelay});
    }
    for (auto& p : getAllPlayersCopy()) sendItemDropSpawnTo(p, eid, itemId, count, x, y, z, vx, vy, vz);
}

static i32 itemMaxStackSize(i32 itemId) {
    if (itemId == 909 || itemId == 910 || itemId == 911) return 1; // filled buckets
    if (itemId == 908) return 16;                                  // empty bucket
    return 64;
}

// ITEMDROP_V1: положить предмет игроку: сначала докладываем в существующие стаки,
// потом в пустые слоты; хотбар приоритетнее рюкзака. Изменённые слоты шлём клиенту (Set Slot 0x15).
// Возвращает, сколько штук поместилось.
static i32 giveItemToPlayer(const std::shared_ptr<entity::Player>& p, i32 itemId, i32 count) {
    i32 remaining = count;
    std::vector<i32> changed;
    const i32 maxStack = itemMaxStackSize(itemId);
    auto trySlot = [&](i32 s, bool mergeOnly) {
        if (remaining <= 0) return;
        if (mergeOnly) {
            if (p->invItemId[s] != itemId || p->invCount[s] <= 0 || p->invCount[s] >= maxStack) return;
        } else {
            if (p->invCount[s] > 0) return;
            p->invItemId[s] = itemId;
        }
        const i32 add = std::min(remaining, maxStack - p->invCount[s]);
        p->invCount[s] += add;
        remaining -= add;
        changed.push_back(s);
    };
    for (i32 pass = 0; pass < 2; ++pass) {
        for (i32 sl = 36; sl <= 44; ++sl) trySlot(sl, pass == 0);
        for (i32 sl = 9; sl <= 35; ++sl) trySlot(sl, pass == 0);
    }
    for (i32 sl : changed) {
        if (sl >= 36 && sl <= 44) p->hotbarBlockState[sl - 36] = itemToBlockState(p->invItemId[sl]); // синк с постановкой блоков
        net::Buffer sb;
        sb.writeByte(0);   // окно 0 — инвентарь игрока
        sb.writeVarInt(0); // state id
        sb.writeI16(static_cast<i16>(sl));
        sb.writeVarInt(p->invCount[sl]);
        if (p->invCount[sl] > 0) { sb.writeVarInt(p->invItemId[sl]); sb.writeVarInt(0); sb.writeVarInt(0); }
        p->getConnection()->sendPacket(0x15, std::vector<u8>(sb.writtenSpan().begin(), sb.writtenSpan().end()));
    }
    return count - remaining;
}

// ITEMDROP_V1: физика и подбор — раз в тик, из tick-потока.
void NetherCraftServer::tickItemDrops() {
    std::vector<ItemDrop> drops;
    { std::lock_guard lk(itemDropsMutex_); drops = itemDrops_; }
    if (drops.empty()) return;
    auto players = getAllPlayersCopy();
    std::vector<i32> removed;
    std::unordered_map<i32, ItemDrop> updated;

    // PHYS_V2: классифика��ия блока по stateId 1.21.1 (см. core/item_blocks.gen.hpp).
    auto blockAt = [&](f64 X, f64 Y, f64 Z) {
        return world_.getBlock(static_cast<i32>(std::floor(X)), static_cast<i32>(std::floor(Y)), static_cast<i32>(std::floor(Z)));
    };
    auto isWaterState = [](i32 s) { return s >= 80 && s <= 95; };   // water + уровни
    auto isLavaState  = [](i32 s) { return s >= 96 && s <= 111; };  // lava + уровни
    auto isCobweb     = [](i32 s) { return s == 2004; };
    auto isSolidState = [&](i32 s) {
        if (s <= 0) return false;                                   // воздух
        if (isWaterState(s) || isLavaState(s) || isCobweb(s)) return false; // сквозь жидкости/паутину
        if (s == 12960 || s == 12961) return false;                 // bubble_column
        if (s == 22318) return false;                              // POWDER_SNOW_V1: предметы проваливаются сквозь рыхлый снег
        return true;
    };
    auto solidAt = [&](f64 X, f64 Y, f64 Z) { return isSolidState(blockAt(X, Y, Z)); };
    auto frictionBelow = [&](f64 X, f64 Y, f64 Z) -> f64 {          // трение верхней грани блока под предметом
        const i32 s = blockAt(X, Y - 0.05, Z);
        if (s == 5780 || s == 10746 || s == 12941) return 0.98;     // ice / packed_ice / blue_ice — скользко
        if (s == 10364) return 0.8;                                 // slime_block
        if (s == 19445) return 0.85;                                // honey_block
        return 0.6;                                                 // обычное трение
    };

    auto sendAllPlay = [&](i32 packetId, const net::Buffer& b) {
        auto v = std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end());
        for (auto& pl : players)
            if (pl && pl->isAlive() && pl->getState() == entity::PlayerState::Play) pl->getConnection()->sendPacket(packetId, v);
    };
    auto sendMeta = [&](const ItemDrop& d) {
        net::Buffer meta; meta.writeVarInt(d.eid); meta.writeByte(8); meta.writeVarInt(7);
        meta.writeVarInt(d.count); meta.writeVarInt(d.itemId); meta.writeVarInt(0); meta.writeVarInt(0);
        meta.writeByte(0xFF);
        sendAllPlay(0x58, meta);
    };
    auto sendRemove = [&](i32 eid) {
        net::Buffer rm; rm.writeVarInt(1); rm.writeVarInt(eid);
        sendAllPlay(0x42, rm);
    };
    auto sendTeleport = [&](const ItemDrop& d) {
        net::Buffer tp; tp.writeVarInt(d.eid);
        tp.writeF64(d.x); tp.writeF64(d.y); tp.writeF64(d.z);
        tp.writeByte(0); tp.writeByte(0);
        tp.writeBool(d.vy == 0.0);
        sendAllPlay(0x70, tp);
    };

    const size_t n = drops.size();
    std::vector<char> gone(n, 0), movedFlag(n, 0), countChanged(n, 0);

    // 1) возраст + деспавн (5 минут, как в ванилле)
    for (size_t i = 0; i < n; ++i) {
        ItemDrop& d = drops[i];
        ++d.age;
        if (d.pickupDelay > 0) --d.pickupDelay;
        if (d.age > 6000) { sendRemove(d.eid); removed.push_back(d.eid); gone[i] = 1; }
    }

    // 2) движение: гравитация 0.04, вода/лава/паутина, трение об пол, сопротивление воздуха
    for (size_t i = 0; i < n; ++i) {
        if (gone[i]) continue;
        ItemDrop& d = drops[i];
        const i32 here = blockAt(d.x, d.y + 0.125, d.z);            // ~центр хитбокса предмета (0.25 высотой)
        const bool inWater = isWaterState(here);
        const bool inLava  = isLavaState(here);
        const bool inWeb   = isCobweb(here);
        const bool inSnow  = (here == 22318);                      // POWDER_SNOW_V1
        const i32 bubble = bubbleColumnPush(world_, static_cast<i32>(std::floor(d.x)),
                                             static_cast<i32>(std::floor(d.y + 0.125)),
                                             static_cast<i32>(std::floor(d.z)));

        if (bubble != 0) {                                         // BubbleColumnBlock.entityInside
            const i32 above = blockAt(d.x, d.y + 1.0, d.z);
            const bool surface = above != 12960 && above != 12961;
            if (bubble < 0) d.vy = std::max(surface ? -0.9 : -0.3, d.vy - 0.03);
            else d.vy = std::min(surface ? 1.8 : 0.7, d.vy + (surface ? 0.1 : 0.06));
        } else if (inWeb) {                                        // паутина: почти стоп + очень медленное опускание
            d.vx *= 0.25; d.vz *= 0.25; d.vy *= 0.05; d.vy -= 0.005;
        } else if (inWater) {                                      // ItemEntity.setUnderwaterMovement()
            d.vx *= 0.99; d.vz *= 0.99;
            if (d.vy < 0.06) d.vy += 0.0005;
        } else if (inLava) {                                       // ItemEntity.setUnderLavaMovement()
            d.vx *= 0.95; d.vz *= 0.95;
            if (d.vy < 0.06) d.vy += 0.0005;
        } else if (inSnow) {                                       // рыхлый снег: предмет медленно проваливается
            d.vy -= 0.01; d.vx *= 0.7; d.vy *= 0.7; d.vz *= 0.7;
        } else {
            d.vy -= 0.04;                                          // обычная гравитация
        }

        f64 nx = d.x + d.vx, ny = d.y + d.vy, nz = d.z + d.vz;
        bool onGround = false;
        if (d.vy <= 0.0 && solidAt(nx, ny - 0.001, nz)) {          // приземлился — ставим на верх блока
            ny = std::floor(ny - 0.001) + 1.0; d.vy = 0.0; onGround = true;
        }
        if (d.vy > 0.0 && solidAt(nx, ny + 0.25, nz)) { ny = d.y; d.vy = 0.0; }             // упёрся в потолок
        if (d.vx != 0.0 && solidAt(nx + (d.vx > 0 ? 0.13 : -0.13), ny + 0.05, d.z)) { nx = d.x; d.vx = 0.0; } // стена X
        if (d.vz != 0.0 && solidAt(d.x, ny + 0.05, nz + (d.vz > 0 ? 0.13 : -0.13))) { nz = d.z; d.vz = 0.0; } // стена Z

        if (onGround) {
            const f64 fr = frictionBelow(nx, ny, nz) * 0.98;
            d.vx *= fr; d.vz *= fr;
            d.vy *= 0.98;
        } else if (!inWeb && !inSnow) {
            // ItemEntity.tick(): после move всегда multiply(f, 0.98, f),
            // в воздухе f=0.98; water/lava damping уже применён выше и затем тоже получает этот шаг.
            d.vx *= 0.98; d.vy *= 0.98; d.vz *= 0.98;
        }
        if (std::abs(d.vx) < 1e-3) d.vx = 0.0;
        if (std::abs(d.vz) < 1e-3) d.vz = 0.0;
        if (onGround && std::abs(d.vy) < 1e-3) d.vy = 0.0;

        movedFlag[i] = (std::abs(nx - d.x) + std::abs(ny - d.y) + std::abs(nz - d.z) > 1e-4) ? 1 : 0;
        d.x = nx; d.y = ny; d.z = nz;
    }

    // 3) слияние близких стаков одного типа (как в ванилле объединяются лежащие предметы)
    for (size_t i = 0; i < n; ++i) {
        if (gone[i]) continue;
        for (size_t j = i + 1; j < n; ++j) {
            if (gone[j]) continue;
            ItemDrop& a = drops[i]; ItemDrop& b = drops[j];
            const i32 maxStack = itemMaxStackSize(a.itemId);
            if (a.itemId != b.itemId || a.count >= maxStack || b.count >= maxStack ||
                a.count + b.count > maxStack) continue;
            const f64 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            if (dx * dx + dz * dz > 0.5 || std::abs(dy) > 0.5) continue;
            a.count += b.count; countChanged[i] = 1;               // ��ыбивает более старый предмет (i)
            sendRemove(b.eid); removed.push_back(b.eid); gone[j] = 1;
        }
    }

    // 4) подбор игроками (живой, не спектатор, радиус ~1.2 блока по горизонтали)
    for (size_t i = 0; i < n; ++i) {
        if (gone[i]) continue;
        ItemDrop& d = drops[i];
        bool taken = false;
        if (d.pickupDelay <= 0) {
            for (auto& pl : players) {
                if (!pl || !pl->isAlive() || pl->getState() != entity::PlayerState::Play || pl->gameMode == 3 || pl->dead) continue;
                const f64 dx = pl->getX() - d.x, dy = pl->getY() - d.y, dz = pl->getZ() - d.z;
                if (dx * dx + dz * dz > 1.44 || dy < -2.0 || dy > 1.0) continue;
                const i32 added = giveItemToPlayer(pl, d.itemId, d.count);
                if (added <= 0) continue;                          // инвентарь полный — предмет остаётся лежать
                if (added >= d.count) {
                    net::Buffer ti; ti.writeVarInt(d.eid); ti.writeVarInt(static_cast<i32>(pl->getEntityId())); ti.writeVarInt(d.count);
                    sendAllPlay(0x6F, ti);                          // Take Item Entity: анимация всасывания
                    sendRemove(d.eid); removed.push_back(d.eid); gone[i] = 1; taken = true;
                } else {                                            // поместилась только часть — остаток лежит дальше
                    d.count -= added; countChanged[i] = 1;
                }
                break;
            }
        }
        if (taken) continue;
        if (countChanged[i]) sendMeta(d);
        if (movedFlag[i]) sendTeleport(d);
        updated.emplace(d.eid, d);
    }

    { // пишем результат назад; предметы, добавленные другими потоками за время тика, не трогаем
        std::lock_guard lk(itemDropsMutex_);
        std::erase_if(itemDrops_, [&](const ItemDrop& d) { return std::find(removed.begin(), removed.end(), d.eid) != removed.end(); });
        for (auto& d : itemDrops_) { auto it = updated.find(d.eid); if (it != updated.end()) d = it->second; }
    }
}

// ============================================================
// FALLING_V1: port of FallingBlock/FallingBlockEntity (Java 1.21.1).
// Scheduled after two ticks; gravity 0.04, drag 0.98, entity type 40.
// ============================================================
// ============================================================
// DIMPHYS_V1: физика больше не прибита гвоздями к оверворлду. В ключ очереди
// добавлены 2 бита измерения (0 = оверворлд, 1 = Ад, 2 = Энд); координаты
// сжаты до 25 бит (±16.7М — шире границы мира в 30М/2). Измерение берётся из
// g_dimCtx: поток игрока выставляет его по player->dimension, тик-пот��к — по текущему миру.
// ============================================================
thread_local i32 g_dimCtx = 0;
struct DimCtxScope {
    i32 prev;
    explicit DimCtxScope(i32 d) : prev(g_dimCtx) { g_dimCtx = d; }
    ~DimCtxScope() { g_dimCtx = prev; }
};
static inline u64 dimPackKey(i32 x, i32 y, i32 z) {
    const u64 ux = static_cast<u64>(static_cast<u32>(x)) & 0x1FFFFFFull; // 25 бит
    const u64 uz = static_cast<u64>(static_cast<u32>(z)) & 0x1FFFFFFull; // 25 бит
    const u64 uy = static_cast<u64>(static_cast<u32>(y)) & 0xFFFull;     // 12 бит
    const u64 ud = static_cast<u64>(static_cast<u32>(g_dimCtx)) & 0x3ull;
    return (ud << 62) | (ux << 37) | (uz << 12) | uy;
}
static inline void dimUnpackKey(u64 k, i32& x, i32& y, i32& z) {
    auto sext = [](u64 v, int bits) -> i32 { const u64 m = 1ull << (bits - 1); return static_cast<i32>((v ^ m) - m); };
    x = sext((k >> 37) & 0x1FFFFFFull, 25);
    z = sext((k >> 12) & 0x1FFFFFFull, 25);
    y = sext(k & 0xFFFull, 12);
}
static inline i32 dimOfKey(u64 k) { return static_cast<i32>((k >> 62) & 0x3ull); }

static inline u64 fallingKey(i32 x, i32 y, i32 z) { return dimPackKey(x, y, z); }
static inline void fallingUnkey(u64 k, i32& x, i32& y, i32& z) { dimUnpackKey(k, x, y, z); }

void NetherCraftServer::scheduleFallingBlockUpdate(i32 x, i32 y, i32 z, i32 delay) {
    if (y < world::CHUNK_HEIGHT_MIN || y >= world::CHUNK_HEIGHT_MAX) return;
    if (!isFallingBlockState(world_.getBlock(x, y, z))) return;
    if (world_.getBlock(x, y, z) == 7416) delay = std::max(delay, 5); // DragonEggBlock
    const u64 k = fallingKey(x, y, z);
    const i32 due = tickCounter_ + std::max(1, delay);
    std::lock_guard lk(fallingMutex_);
    auto it = fallingDue_.find(k);
    if (it != fallingDue_.end() && it->second <= due) return;
    fallingDue_[k] = due;
    fallingQueue_.emplace(due, k);
}

void NetherCraftServer::scheduleFallingColumnCascade(i32 x, i32 y, i32 z, i32 firstDelay) {
    i32 wave = 0;
    for (i32 sy = y; sy < world::CHUNK_HEIGHT_MAX && wave < 16; ++sy) {
        if (!isFallingBlockState(world_.getBlock(x, sy, z))) continue;
        scheduleFallingBlockUpdate(x, sy, z, firstDelay + wave * 30);
        ++wave;
    }
}

void NetherCraftServer::tickFallingBlocks() {
    auto players = getAllPlayersCopy();
    auto sendAll = [&](i32 packetId, const net::Buffer& b) {
        const auto v = std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end());
        for (auto& p : players)
            if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                p->getConnection()->sendPacket(packetId, v);
    };
    auto blockUpdate = [&](i32 x, i32 y, i32 z, i32 state) {
        net::Buffer bu; bu.writePosition(BlockPos{x, y, z}); bu.writeVarInt(state); sendAll(0x09, bu);
    };
    auto removeEntity = [&](i32 eid) {
        net::Buffer rm; rm.writeVarInt(1); rm.writeVarInt(eid); sendAll(0x42, rm);
    };

    std::vector<u64> due;
    {
        std::lock_guard lk(fallingMutex_);
        while (!fallingQueue_.empty() && fallingQueue_.begin()->first <= tickCounter_ && due.size() < 4096) {
            const auto [when, key] = *fallingQueue_.begin();
            fallingQueue_.erase(fallingQueue_.begin());
            auto it = fallingDue_.find(key);
            if (it == fallingDue_.end() || it->second != when) continue;
            fallingDue_.erase(it); due.push_back(key);
        }
    }

    for (u64 key : due) {
        i32 x, y, z; fallingUnkey(key, x, y, z);
        const i32 dimIndex = dimOfKey(key); // DIMPHYS_V1
        if ((dimIndex == 1 && !netherReady_) || (dimIndex == 2 && !endReady_)) continue;
        world::World& world_ = worldFor(dimIndex); // C4458: осознанное перекрытие члена
        DimCtxScope dimScope(dimIndex);
        const i32 state = world_.getBlock(x, y, z);
        if (!isFallingBlockState(state) || y <= world::CHUNK_HEIGHT_MIN ||
            !isFreeForFalling(world_.getBlock(x, y - 1, z))) continue;
        world_.setBlock(x, y, z, 0); // no waterlogged states yet: legacy fluid is air
        scheduleFluidNeighbors(x, y, z);
        blockUpdate(x, y, z, 0);
        const i32 eid = static_cast<i32>(nextEntityId_++);
        FallingBlockMotion f{eid, state, x + 0.5, static_cast<f64>(y), z + 0.5,
                             0.0, 0.0, 0.0, 0, static_cast<f64>(y)};
        f.dim = dimIndex; // DIMPHYS_V1
        { std::lock_guard lk(fallingMutex_); fallingBlocks_.push_back(f); }
        for (auto& p : players) sendFallingBlockSpawnTo(p, eid, state, f.x, f.y, f.z, 0.0, 0.0, 0.0);
        // FALLING_CHAIN_V2: do not release the next block while this entity is
        // still in flight. Otherwise several blocks can land in one cell and
        // turn into drops instead of rebuilding a vertical column.
    }

    std::vector<FallingBlockMotion> moving;
    { std::lock_guard lk(fallingMutex_); moving = fallingBlocks_; }
    if (moving.empty()) return;
    std::unordered_map<i32, FallingBlockMotion> updated;
    std::vector<i32> removed;
    for (auto& f : moving) {
        ++f.time;
        world::World& world_ = worldFor(f.dim); // DIMPHYS_V1 (C4458)
        DimCtxScope dimScope(f.dim);
        const f64 oldY = f.y;
        f.vy -= 0.04;
        f.x += f.vx; f.y += f.vy; f.z += f.vz;
        i32 bx = static_cast<i32>(std::floor(f.x));
        i32 by = static_cast<i32>(std::floor(f.y));
        i32 bz = static_cast<i32>(std::floor(f.z));
        const i32 bubble = bubbleColumnPush(world_, bx, by, bz);
        if (bubble < 0) f.vy = std::max(-0.3, f.vy - 0.03);
        else if (bubble > 0) f.vy = std::min(0.7, f.vy + 0.06);
        bool waterHit = false;
        if (isConcretePowderState(f.state)) {
            const i32 lo = static_cast<i32>(std::floor(std::min(oldY, f.y)));
            const i32 hi = static_cast<i32>(std::floor(std::max(oldY, f.y)));
            for (i32 yy = lo; yy <= hi; ++yy) {
                const i32 ws = world_.getBlock(bx, yy, bz);
                if (ws >= 80 && ws <= 95) { by = yy; f.y = static_cast<f64>(yy); waterHit = true; break; }
            }
        }
        bool ground = false;
        if (!waterHit && f.vy <= 0.0) {
            // Swept vertical collision: FallingBlockEntity must not tunnel through a
            // one-block floor when one tick crosses its top face.
            const i32 topSupport = static_cast<i32>(std::floor(oldY - 1.0e-6));
            const i32 lowSupport = static_cast<i32>(std::floor(f.y - 1.0e-6));
            for (i32 sy = topSupport; sy >= lowSupport; --sy) {
                if (!isFreeForFalling(world_.getBlock(bx, sy, bz))) {
                    by = sy + 1; f.y = static_cast<f64>(by); ground = true; break;
                }
            }
        }
        bool finish = false;
        if (ground || waterHit) {
            if (ground) { by = static_cast<i32>(std::floor(f.y)); f.y = static_cast<f64>(by); }
            const bool solidify = isConcretePowderState(f.state) &&
                (waterHit || (ground && concretePowderShouldSolidify(world_, bx, by, bz,
                                                                      world_.getBlock(bx, by, bz))));
            i32 placeState = solidify ? concreteFromPowder(f.state) : f.state;
            const bool replaceable = isFreeForFalling(world_.getBlock(bx, by, bz));
            const bool supported = waterHit || !isFreeForFalling(world_.getBlock(bx, by - 1, bz));
            if (replaceable && supported && by >= world::CHUNK_HEIGHT_MIN && by < world::CHUNK_HEIGHT_MAX) {
                world_.setBlock(bx, by, bz, placeState);
                scheduleFluidNeighbors(bx, by, bz);
                blockUpdate(bx, by, bz, placeState);
                if (isAnvilState(f.state)) {
                    const f32 damage = static_cast<f32>(std::min(40.0, std::max(0.0, std::ceil(f.startY - f.y - 1.0) * 2.0)));
                    if (damage > 0.0f) for (auto& p : players) {
                        if (!p || !p->isAlive() || p->dead || p->gameMode == 1 || p->gameMode == 3) continue;
                        if (std::abs(p->getX() - f.x) <= 0.8 && std::abs(p->getZ() - f.z) <= 0.8 &&
                            p->getY() < f.y + 1.0 && p->getY() + 1.8 > f.y)
                            applyEnvironmentalDamage(p, damage, 10, std::format("{} был раздавлен наковальней", p->getName()));
                    }
                }
            } else {
                const i32 item = stateToItem(f.state);
                if (item > 0) spawnItemDrop(f.x, f.y + 0.25, f.z, item, 1, 0.0, 0.1, 0.0, 10);
            }
            removeEntity(f.eid); removed.push_back(f.eid); finish = true;
            // FALLING_CHAIN_V3: queue the remaining column above with a safe
            // stagger, then let the immediate next landing tighten the timing.
            scheduleFallingColumnCascade(bx, by + 1, bz, 2);
        } else if (f.time > 600 || f.y < world::CHUNK_HEIGHT_MIN - 64) {
            const i32 item = stateToItem(f.state);
            if (item > 0) spawnItemDrop(f.x, f.y, f.z, item, 1, 0.0, 0.1, 0.0, 10);
            removeEntity(f.eid); removed.push_back(f.eid); finish = true;
        }
        if (finish) continue;
        f.vx *= 0.98; f.vy *= 0.98; f.vz *= 0.98;
        net::Buffer tp; tp.writeVarInt(f.eid); tp.writeF64(f.x); tp.writeF64(f.y); tp.writeF64(f.z);
        tp.writeByte(0); tp.writeByte(0); tp.writeBool(false); sendAll(0x70, tp);
        updated.emplace(f.eid, f);
    }
    {
        std::lock_guard lk(fallingMutex_);
        std::erase_if(fallingBlocks_, [&](const FallingBlockMotion& f) {
            return std::find(removed.begin(), removed.end(), f.eid) != removed.end();
        });
        for (auto& f : fallingBlocks_) { auto it = updated.find(f.eid); if (it != updated.end()) f = it->second; }
    }
}

// ============================================================
// TNT_V1 / EXPLOSION_V1: PrimedTnt tick + vanilla-shaped ray explosion.
// ============================================================
// ANTILAG_TNT_V1: combined world-wide TNT cap (active primed entities + stationary
// placed-but-unignited blocks). Counting starts from server boot, not a full world scan.
static constexpr i64 kMaxTntTotal = 8'000'000;
static void tntCountDecrement(std::atomic<i64>& counter) {
    i64 cur = counter.load(std::memory_order_relaxed);
    while (cur > 0 && !counter.compare_exchange_weak(cur, cur - 1, std::memory_order_relaxed)) {}
}

i64 NetherCraftServer::tntTotalCount() {
    size_t active = 0;
    { std::lock_guard lk(primedTntMutex_); active = primedTnt_.size(); }
    return tntBlockCount_.load(std::memory_order_relaxed) + static_cast<i64>(active);
}

void NetherCraftServer::spawnPrimedTnt(f64 x, f64 y, f64 z, i32 ownerEid, i32 fuse, f64 launchImpulse) {
    { std::lock_guard lk(primedTntMutex_); if (primedTnt_.size() >= 4096) return; }
    const i32 eid = static_cast<i32>(nextEntityId_++);
    const f64 angle = std::fmod(static_cast<f64>(eid) * 2.399963229728653, 6.283185307179586);
    const u32 scatter = static_cast<u32>(eid * 747796405u + 2891336453u);
    const f64 horizontal = launchImpulse > 0.0 ? launchImpulse * (0.55 + (scatter & 255u) / 512.0) : 0.02;
    const f64 vertical = launchImpulse > 0.0 ? 0.25 + ((scatter >> 8) & 255u) / 255.0 * launchImpulse : 0.2;
    PrimedTntMotion t{eid, ownerEid, x, y, z, -std::sin(angle) * horizontal, vertical,
                      -std::cos(angle) * horizontal, std::max(1, fuse)};
    { std::lock_guard lk(primedTntMutex_); primedTnt_.push_back(t); }
    net::Buffer sp;
    sp.writeVarInt(eid);
    sp.writeUUID(UUID{static_cast<u64>(eid), 0x71000000ULL + static_cast<u64>(eid)});
    sp.writeVarInt(106); // minecraft:tnt, protocol registry 1.21.1
    sp.writeF64(x); sp.writeF64(y); sp.writeF64(z);
    sp.writeByte(0); sp.writeByte(0); sp.writeByte(0);
    sp.writeVarInt(0);
    auto vel = [](f64 v) { return static_cast<i16>(std::clamp(v * 8000.0, -32000.0, 32000.0)); };
    sp.writeI16(vel(t.vx)); sp.writeI16(vel(t.vy)); sp.writeI16(vel(t.vz));
    const auto bytes = std::vector<u8>(sp.writtenSpan().begin(), sp.writtenSpan().end());
    for (auto& p : getAllPlayersCopy())
        if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
            p->getConnection()->sendPacket(0x01, bytes);
}

bool NetherCraftServer::primeTntBlock(i32 x, i32 y, i32 z, i32 ownerEid, i32 fuse) {
    if (world_.getBlock(x, y, z) != 2095) return false;
    world_.setBlock(x, y, z, 0);
    tntCountDecrement(tntBlockCount_); // ANTILAG_TNT_V1: stationary TNT ignited into a primed entity
    scheduleFluidNeighbors(x, y, z);
    scheduleFallingBlockUpdate(x, y + 1, z, 2);
    scheduleFallingColumnCascade(x, y + 1, z, 2);
    net::Buffer bu; bu.writePosition(BlockPos{x, y, z}); bu.writeVarInt(0);
    const auto bytes = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
    for (auto& p : getAllPlayersCopy())
        if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
            p->getConnection()->sendPacket(0x09, bytes);
    spawnPrimedTnt(x + 0.5, static_cast<f64>(y), z + 0.5, ownerEid, fuse);
    broadcastBlockSound("minecraft:entity.tnt.primed", x, y, z, 1.0f, 1.0f);
    return true;
}

void NetherCraftServer::explodeAt(f64 x, f64 y, f64 z, f32 radius, i32 sourceEid, i32 ownerEid) {
    std::unordered_set<u64> affected;
    // EXPLOSION_FAST_V2: cache the few touched columns instead of taking the
    // world's chunk-map mutex for every 0.3-block ray sample.
    std::unordered_map<u64, std::shared_ptr<world::ChunkColumn>> chunkCache;
    auto chunkKey = [](i32 cx, i32 cz) -> u64 {
        return (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cz);
    };
    auto cachedChunk = [&](i32 bx, i32 bz) -> std::shared_ptr<world::ChunkColumn> {
        const i32 cx = bx >> 4, cz = bz >> 4; const u64 key = chunkKey(cx, cz);
        auto it = chunkCache.find(key);
        if (it != chunkCache.end()) return it->second;
        auto chunk = world_.getChunk(cx, cz);
        chunkCache.emplace(key, chunk);
        return chunk;
    };
    auto readBlock = [&](i32 bx, i32 by, i32 bz) -> i32 {
        auto chunk = cachedChunk(bx, bz);
        return chunk ? chunk->getBlock(bx, by, bz) : 0;
    };
    auto resistance = [](i32 state) -> f32 {
        if (state <= 0) return 0.0f;
        if (state == 79 || state == 2354 || state == 26573) return 3600000.0f; // bedrock/obsidian/reinforced
        if (state >= 80 && state <= 111) return 100.0f;                       // fluids shield rays
        if (state == 2095) return 0.0f;                                      // TNT chains easily
        if (state == 1 || state == 14 || state == 6537) return 6.0f;         // stone families
        return 0.8f;
    };
    // Explosion.explode(): rays from the surface of a 16^3 cube, 0.3-block steps.
    for (i32 ix = 0; ix < 16; ++ix) for (i32 iy = 0; iy < 16; ++iy) for (i32 iz = 0; iz < 16; ++iz) {
        if (ix != 0 && ix != 15 && iy != 0 && iy != 15 && iz != 0 && iz != 15) continue;
        f64 dx = static_cast<f64>(ix) / 15.0 * 2.0 - 1.0;
        f64 dy = static_cast<f64>(iy) / 15.0 * 2.0 - 1.0;
        f64 dz = static_cast<f64>(iz) / 15.0 * 2.0 - 1.0;
        const f64 len = std::sqrt(dx * dx + dy * dy + dz * dz);
        dx /= len; dy /= len; dz /= len;
        u32 h = static_cast<u32>((ix * 73428767) ^ (iy * 912931) ^ (iz * 19349663) ^ sourceEid);
        const f32 random01 = static_cast<f32>(h & 0xFFFFu) / 65535.0f;
        f32 strength = radius * (0.7f + random01 * 0.6f);
        f64 px = x, py = y, pz = z;
        while (strength > 0.0f) {
            const i32 bx = static_cast<i32>(std::floor(px));
            const i32 by = static_cast<i32>(std::floor(py));
            const i32 bz = static_cast<i32>(std::floor(pz));
            if (by < world::CHUNK_HEIGHT_MIN || by >= world::CHUNK_HEIGHT_MAX) break;
            const i32 state = readBlock(bx, by, bz);
            if (state > 0) strength -= (resistance(state) + 0.3f) * 0.3f;
            if (strength > 0.0f && state > 0 && resistance(state) < 1000000.0f)
                affected.insert(fallingKey(bx, by, bz));
            px += dx * 0.3; py += dy * 0.3; pz += dz * 0.3;
            strength -= 0.22500001f;
        }
    }

    // EXPLOSION_V3: the ray lattice must not miss point-blank blocks or TNT.
    const i32 ecx=static_cast<i32>(std::floor(x));
    const i32 ecy=static_cast<i32>(std::floor(y));
    const i32 ecz=static_cast<i32>(std::floor(z));
    static constexpr i32 pointBlankR = 2; // TNT_EXPLODE_V4: primed TNT may drift ~1 block before fuse ends
    for(i32 ox=-pointBlankR;ox<=pointBlankR;++ox) for(i32 oy=-pointBlankR;oy<=pointBlankR;++oy) for(i32 oz=-pointBlankR;oz<=pointBlankR;++oz)
        if(readBlock(ecx+ox,ecy+oy,ecz+oz)>0)
            affected.insert(fallingKey(ecx+ox,ecy+oy,ecz+oz));
    const i32 scanR=static_cast<i32>(std::ceil(radius));
    for(i32 ox=-scanR;ox<=scanR;++ox) for(i32 oy=-scanR;oy<=scanR;++oy) for(i32 oz=-scanR;oz<=scanR;++oz) {
        if(ox*ox+oy*oy+oz*oz>scanR*scanR) continue;
        if(readBlock(ecx+ox,ecy+oy,ecz+oz)==2095)
            affected.insert(fallingKey(ecx+ox,ecy+oy,ecz+oz));
    }

    std::vector<std::array<i32, 3>> chainedTnt;
    std::vector<miniedit::BlockChange> destroyed;
    destroyed.reserve(affected.size());
    std::unordered_map<i32, i32> aggregatedDrops;
    auto players = getAllPlayersCopy();
    for (u64 key : affected) {
        i32 bx, by, bz; fallingUnkey(key, bx, by, bz);
        const i32 old = readBlock(bx, by, bz);
        if (old <= 0 || old == 79 || old == 2354 || old == 26573) continue;
        if (old == 2095) chainedTnt.push_back({bx, by, bz});
        auto chunk = cachedChunk(bx, bz);
        if (!chunk) continue;
        chunk->setBlock(bx, by, bz, 0);
        destroyed.push_back({BlockPos{bx, by, bz}, old, 0});
        if (old != 2095 && ((static_cast<u32>(key) ^ static_cast<u32>(sourceEid)) % 3u) == 0u) {
            const i32 item = stateToItem(old);
            if (item > 0 && (aggregatedDrops.contains(item) || aggregatedDrops.size() < 32))
                aggregatedDrops[item] = std::min(64, aggregatedDrops[item] + 1);
        }
    }
    if (!destroyed.empty()) publishMiniEditChanges(destroyed); // one section packet per section
    i32 dropOrdinal = 0;
    for (const auto& [item, count] : aggregatedDrops) {
        const f64 angle = dropOrdinal++ * 2.399963229728653;
        spawnItemDrop(x + std::cos(angle) * 0.35, y + 0.35, z + std::sin(angle) * 0.35,
                      item, count, std::cos(angle) * 0.05, 0.12, std::sin(angle) * 0.05, 10);
    }
    if (chainedTnt.size() >= 16) {
        // Keep a bounded amount of the vanilla spectacle: up to 75 boundary
        // TNTs are launched with deterministic-random impulse/fuse while the
        // connected interior uses the anti-lag collapse.
        std::unordered_set<u64> chainSet;
        chainSet.reserve(chainedTnt.size() * 2);
        for (const auto& p : chainedTnt) chainSet.insert(fallingKey(p[0],p[1],p[2]));
        static constexpr i32 edgeDirs[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        std::vector<std::array<i32,3>> flyers;
        flyers.reserve(chainedTnt.size());
        for (const auto& p : chainedTnt) {
            bool edge = false;
            for (const auto& d : edgeDirs)
                if (!chainSet.contains(fallingKey(p[0]+d[0],p[1]+d[1],p[2]+d[2]))) { edge=true; break; }
            if (edge) flyers.push_back(p);
        }
        auto randomScore = [&](const auto& p) {
            u64 v = fallingKey(p[0],p[1],p[2]) ^ static_cast<u64>(sourceEid);
            v ^= v >> 30; v *= 0xbf58476d1ce4e5b9ULL; v ^= v >> 27;
            v *= 0x94d049bb133111ebULL; return v ^ (v >> 31);
        };
        std::sort(flyers.begin(), flyers.end(), [&](const auto& a, const auto& b) {
            return randomScore(a) < randomScore(b);
        });
        const size_t flyCount = std::min<size_t>(75, flyers.size());
        for (size_t i = 0; i < flyCount; ++i) {
            const auto& p = flyers[i];
            const i32 fuse = 25 + static_cast<i32>((fallingKey(p[0],p[1],p[2]) ^ sourceEid) % 36);
            spawnPrimedTnt(p[0]+0.5, static_cast<f64>(p[1]), p[2]+0.5, ownerEid, fuse, 0.65);
        }
        startBulkTntCollapse(chainedTnt, ownerEid);
    }
    else for (const auto& p : chainedTnt) {
        const i32 fuse = 10 + static_cast<i32>((fallingKey(p[0], p[1], p[2]) ^ sourceEid) % 21);
        spawnPrimedTnt(p[0] + 0.5, static_cast<f64>(p[1]), p[2] + 0.5, ownerEid, fuse);
    }

    // Vanilla-shaped entity damage with block exposure samples. Walls now absorb
    // damage/knockback instead of the old unconditional line-of-sight value 1.
    const f64 diameter = static_cast<f64>(radius) * 2.0;
    for (auto& p : players) {
        if (!p || !p->isAlive() || p->dead || p->gameMode == 1 || p->gameMode == 3) continue;
        const f64 dx = p->getX() - x, dy = (p->getY() + 0.9) - y, dz = p->getZ() - z;
        const f64 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > diameter || dist < 1.0e-6) continue;
        i32 clearSamples = 0;
        for (f64 oy : {0.1,0.6,1.1,1.7}) {
            bool clear = true;
            const f64 tx=p->getX(), ty=p->getY()+oy, tz=p->getZ();
            const i32 steps = std::max(2, static_cast<i32>(std::ceil(dist * 2.0)));
            for (i32 step=1; step<steps; ++step) {
                const f64 q=static_cast<f64>(step)/steps;
                const i32 st=readBlock(static_cast<i32>(std::floor(x+(tx-x)*q)),
                                       static_cast<i32>(std::floor(y+(ty-y)*q)),
                                       static_cast<i32>(std::floor(z+(tz-z)*q)));
                if (st>0 && !(st>=80 && st<=111)) { clear=false; break; }
            }
            if (clear) ++clearSamples;
        }
        const f64 exposure = clearSamples / 4.0;
        const f64 impact = std::max(0.0, 1.0 - dist / diameter) * exposure;
        if (impact <= 0.0) continue;
        const f32 damage = static_cast<f32>(std::floor((impact * impact + impact) * 0.5 * 7.0 * diameter + 1.0));
        applyEnvironmentalDamage(p, damage, 7, std::format("{} взорвался", p->getName()));
        if (!p->dead) {
            const f64 kb = impact / dist;
            auto q = [](f64 v) { return static_cast<i16>(std::clamp(v * 8000.0, -32000.0, 32000.0)); };
            net::Buffer mv; mv.writeVarInt(static_cast<i32>(p->getEntityId()));
            mv.writeI16(q(dx * kb)); mv.writeI16(q(dy * kb)); mv.writeI16(q(dz * kb));
            p->getConnection()->sendPacket(0x5A, std::vector<u8>(mv.writtenSpan().begin(), mv.writtenSpan().end()));
        }
    }

    // EXPLOSION_PHYSICS_V2: the pressure wave now acts on loose server-side
    // entities too. Items, falling blocks and projectiles no longer ignore an
    // explosion next to them; distance falloff and a speed cap keep this cheap
    // and prevent a large TNT wall from creating unbounded velocities.
    auto addShockwave = [&](f64 ex, f64 ey, f64 ez, i32 salt,
                            f64& vx, f64& vy, f64& vz, f64 scale) {
        f64 dx = ex - x, dy = ey - y, dz = ez - z;
        f64 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist >= diameter) return;
        if (dist < 1.0e-5) {
            const f64 angle = std::fmod(static_cast<f64>(salt) * 2.399963229728653,
                                        6.283185307179586);
            dx = std::cos(angle); dz = std::sin(angle); dy = 0.35; dist = 1.0;
        }
        const f64 impulse = (1.0 - dist / diameter) * scale;
        vx = std::clamp(vx + dx / dist * impulse, -2.5, 2.5);
        vy = std::clamp(vy + dy / dist * impulse + impulse * 0.12, -2.5, 2.5);
        vz = std::clamp(vz + dz / dist * impulse, -2.5, 2.5);
    };
    {
        std::lock_guard lk(itemDropsMutex_);
        for (auto& item : itemDrops_)
            addShockwave(item.x, item.y + 0.125, item.z, item.eid,
                         item.vx, item.vy, item.vz, 0.85);
    }
    {
        std::lock_guard lk(fallingMutex_);
        for (auto& block : fallingBlocks_)
            addShockwave(block.x, block.y + 0.49, block.z, block.eid,
                         block.vx, block.vy, block.vz, 0.70);
    }
    {
        std::lock_guard lk(projectilesMutex_);
        for (auto& projectile : projectiles_)
            addShockwave(projectile.x, projectile.y, projectile.z, projectile.eid,
                         projectile.vx, projectile.vy, projectile.vz, 0.55);
    }
    broadcastBlockSound("minecraft:entity.generic.explode", static_cast<i32>(std::floor(x)),
                        static_cast<i32>(std::floor(y)), static_cast<i32>(std::floor(z)), 4.0f, 1.0f);
}

void NetherCraftServer::startBulkTntCollapse(std::span<const std::array<i32, 3>> seeds, i32 ownerEid) {
    BulkTntJob job; job.ownerEid = ownerEid;
    for (const auto& p : seeds) {
        job.frontier.push_back({p[0], p[1], p[2]});
        const u64 column = (static_cast<u64>(static_cast<u32>(p[0])) << 32) | static_cast<u32>(p[2]);
        auto [it, inserted] = job.columnFloor.emplace(column, p[1]);
        if (!inserted) it->second = std::min(it->second, p[1]);
    }
    if (!job.frontier.empty()) bulkTntJobs_.push_back(std::move(job));
}

void NetherCraftServer::tickBulkTntCollapse() {
    if (bulkTntJobs_.empty()) return;
    { // Do not let authoritative collision run far ahead of client rendering.
        std::lock_guard lock(miniEditPacketsMutex_);
        if (miniEditPackets_.size() > 256) return;
    }
    constexpr size_t kNodeBudget = 25'000; // bounded anti-lag work, ~80 ticks for 2M dense TNT
    auto& job = bulkTntJobs_.front();
    std::unordered_map<u64, std::shared_ptr<world::ChunkColumn>> chunks;
    auto chunkAt = [&](i32 x, i32 z) {
        const i32 cx = x >> 4, cz = z >> 4;
        const u64 key = (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cz);
        auto it = chunks.find(key); if (it != chunks.end()) return it->second;
        auto c = world_.getChunk(cx, cz); chunks.emplace(key, c); return c;
    };
    static constexpr i32 dirs[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    std::vector<miniedit::BlockChange> changes; changes.reserve(kNodeBudget);
    size_t nodes = 0;
    while (nodes++ < kNodeBudget && !job.frontier.empty()) {
        const BlockPos p = job.frontier.front(); job.frontier.pop_front();
        for (const auto& d : dirs) {
            BlockPos n{p.x+d[0], p.y+d[1], p.z+d[2]};
            if (n.y < world::CHUNK_HEIGHT_MIN || n.y >= world::CHUNK_HEIGHT_MAX) continue;
            auto chunk = chunkAt(n.x,n.z);
            if (!chunk || chunk->getBlock(n.x,n.y,n.z) != 2095) continue;
            chunk->setBlock(n.x,n.y,n.z,0);
            changes.push_back({n,2095,0}); job.frontier.push_back(n); ++job.removed;
            job.touchedChunks.insert((static_cast<u64>(static_cast<u32>(n.x >> 4)) << 32) |
                                     static_cast<u32>(n.z >> 4));
            const u64 column = (static_cast<u64>(static_cast<u32>(n.x)) << 32) | static_cast<u32>(n.z);
            auto [it, inserted] = job.columnFloor.emplace(column, n.y);
            if (!inserted) it->second = std::min(it->second, n.y);
        }
    }
    // A compressed TNT chain must still make a real crater. Start one bounded
    // downward walker per TNT column; it stops at bedrock/another protected
    // block, so even a tall mass reaches the world's real floor without a giant
    // synchronous bounding-box scan.
    if (job.frontier.empty() && !job.scanComplete) {
        for (const auto& [column, floorY] : job.columnFloor) {
            const i32 cx = static_cast<i32>(column >> 32);
            const i32 cz = static_cast<i32>(column & 0xFFFFFFFFu);
            if (floorY > world::CHUNK_HEIGHT_MIN) job.crater.push_back({cx, floorY - 1, cz});
        }
        job.scanComplete = true;
    }
    size_t craterBudget = kNodeBudget;
    while (craterBudget-- > 0 && !job.crater.empty()) {
        const BlockPos p = job.crater.front(); job.crater.pop_front();
        auto chunk = chunkAt(p.x,p.z); if (!chunk) continue;
        const i32 old = chunk->getBlock(p.x,p.y,p.z);
        if (old == 79 || old == 2354 || old == 26573) continue;
        if (old > 0) {
            chunk->setBlock(p.x,p.y,p.z,0);
            changes.push_back({p,old,0});
            job.touchedChunks.insert((static_cast<u64>(static_cast<u32>(p.x >> 4)) << 32) |
                                     static_cast<u32>(p.z >> 4));
        }
        if (p.y > world::CHUNK_HEIGHT_MIN) job.crater.push_back({p.x,p.y - 1,p.z});
    }
    if (!changes.empty()) publishMiniEditChanges(changes, false);
    if (job.frontier.empty() && job.crater.empty() && job.scanComplete) {
        for (u64 chunkKey : job.touchedChunks)
            if (tntLightResyncQueued_.insert(chunkKey).second) tntLightResync_.push_back(chunkKey);
        bulkTntJobs_.pop_front();
    }
}

void NetherCraftServer::tickTntLightResync() {
    for (i32 budget = 0; budget < 4 && !tntLightResync_.empty(); ++budget) {
        const u64 key = tntLightResync_.front(); tntLightResync_.pop_front();
        tntLightResyncQueued_.erase(key);
        const i32 cx = static_cast<i32>(key >> 32), cz = static_cast<i32>(key & 0xFFFFFFFFu);
        for (const auto& p : getAllPlayersCopy()) {
            if (!p || !p->isAlive() || p->getState() != entity::PlayerState::Play) continue;
            const i32 pcx = static_cast<i32>(std::floor(p->getX())) >> 4;
            const i32 pcz = static_cast<i32>(std::floor(p->getZ())) >> 4;
            if (std::abs(pcx-cx) > config_.viewDistance+1 || std::abs(pcz-cz) > config_.viewDistance+1) continue;
            p->forgetChunk(cx,cz);
            sendChunksAround(p,cx,cz,0,1); // full Chunk Data + Light removes stale shadows
        }
    }
}

void NetherCraftServer::tickPrimedTnt() {
    std::vector<PrimedTntMotion> moving;
    { std::lock_guard lk(primedTntMutex_); moving = primedTnt_; }
    if (moving.empty()) return;
    auto players = getAllPlayersCopy();
    std::vector<i32> removed;
    std::unordered_map<i32, PrimedTntMotion> updated;
    i32 explosionsThisTick = 0;
    for (auto& t : moving) {
        t.vy -= 0.04;
        f64 nx = t.x + t.vx, ny = t.y + t.vy, nz = t.z + t.vz;
        const i32 bodyY = static_cast<i32>(std::floor(t.y + 0.49));
        if (!isFreeForFalling(world_.getBlock(static_cast<i32>(std::floor(nx)), bodyY,
                                               static_cast<i32>(std::floor(t.z))))) {
            nx=t.x; t.vx *= -0.45;
        }
        if (!isFreeForFalling(world_.getBlock(static_cast<i32>(std::floor(nx)), bodyY,
                                               static_cast<i32>(std::floor(nz))))) {
            nz=t.z; t.vz *= -0.45;
        }
        bool ground = false;
        if (t.vy <= 0.0) {
            const i32 bx = static_cast<i32>(std::floor(nx));
            const i32 bz = static_cast<i32>(std::floor(nz));
            const i32 below = static_cast<i32>(std::floor(ny - 1.0e-6));
            if (!isFreeForFalling(world_.getBlock(bx, below, bz))) {
                ny = below + 1.0; ground = true;
            }
        }
        t.x = nx; t.y = ny; t.z = nz;
        t.vx *= 0.98; t.vy *= 0.98; t.vz *= 0.98;
        if (ground) { t.vx *= 0.7; t.vz *= 0.7; t.vy *= -0.5; }
        const i32 medium = world_.getBlock(static_cast<i32>(std::floor(t.x)),
                                           static_cast<i32>(std::floor(t.y+0.45)),
                                           static_cast<i32>(std::floor(t.z)));
        const i32 bubble = bubbleColumnPush(world_, static_cast<i32>(std::floor(t.x)),
                                             static_cast<i32>(std::floor(t.y+0.45)),
                                             static_cast<i32>(std::floor(t.z)));
        if (bubble != 0) {
            if (bubble < 0) t.vy = std::max(-0.3, t.vy - 0.03);
            else t.vy = std::min(0.7, t.vy + 0.06);
            t.vx *= 0.90; t.vz *= 0.90;
        } else if (medium>=80 && medium<=95) { // water: strong drag + slight buoyancy
            t.vx*=0.80; t.vz*=0.80; t.vy=t.vy*0.80+0.025;
        } else if (medium>=96 && medium<=111) { // lava: denser/slower
            t.vx*=0.50; t.vz*=0.50; t.vy=t.vy*0.50+0.015;
        }
        --t.fuse;
        if (t.fuse <= 0) {
            if (explosionsThisTick >= 4) { t.fuse = 1; updated.emplace(t.eid, t); continue; }
            ++explosionsThisTick;
            net::Buffer rm; rm.writeVarInt(1); rm.writeVarInt(t.eid);
            const auto bytes = std::vector<u8>(rm.writtenSpan().begin(), rm.writtenSpan().end());
            for (auto& p : players) if (p && p->isAlive()) p->getConnection()->sendPacket(0x42, bytes);
            removed.push_back(t.eid);
            explodeAt(t.x, t.y + 0.0625, t.z, 4.0f, t.eid, t.ownerEid);
            continue;
        }
        net::Buffer tp; tp.writeVarInt(t.eid); tp.writeF64(t.x); tp.writeF64(t.y); tp.writeF64(t.z);
        tp.writeByte(0); tp.writeByte(0); tp.writeBool(ground);
        const auto bytes = std::vector<u8>(tp.writtenSpan().begin(), tp.writtenSpan().end());
        for (auto& p : players) if (p && p->isAlive()) p->getConnection()->sendPacket(0x70, bytes);
        updated.emplace(t.eid, t);
    }
    std::lock_guard lk(primedTntMutex_);
    std::erase_if(primedTnt_, [&](const PrimedTntMotion& t) {
        return std::find(removed.begin(), removed.end(), t.eid) != removed.end();
    });
    for (auto& t : primedTnt_) { auto it = updated.find(t.eid); if (it != updated.end()) t = it->second; }
}

// ============================================================
// VEHICLE_PHYSICS_V1: boats + minecarts.
// Boats are client-authoritative, exactly like vanilla: the riding client sends
// Move Vehicle (0x1E) and the server rebroadcasts Teleport Entity (0x70).
// Minecarts are server-side and follow rails by probing neighbouring rail
// blocks instead of decoding a rail shape out of the block state: the generated
// state table in this project has gaps that do not match vanilla state counts,
// so shape bits cannot be trusted here (see PHYSICS_PROGRESS.txt).
// Powered rails are treated as plain rails on purpose - redstone is out of scope.
// ============================================================
namespace {

constexpr i32 kBoatTypeId = 10;
constexpr i32 kMinecartTypeId = 69;

// Ranges stop at the next block listed in item_blocks.gen.hpp, so a neighbouring
// block can never be mistaken for a rail.
inline bool isVehicleRailState(i32 s) {
    return (s >= 4663 && s < 4693)   // rail
        || (s >= 1957 && s < 1981)   // powered_rail
        || (s >= 1981 && s < 1998)   // detector_rail
        || (s >= 9333 && s < 9345);  // activator_rail
}

inline bool isVehicleWaterState(i32 s) { return s >= 80 && s <= 95; }

inline i32 boatVariantForItem(i32 item) {
    switch (item) {
        case 774: return 0; // oak
        case 776: return 1; // spruce
        case 778: return 2; // birch
        case 780: return 3; // jungle
        case 782: return 4; // acacia
        case 784: return 5; // cherry
        case 786: return 6; // dark oak
        case 788: return 7; // mangrove
        case 790: return 8; // bamboo raft
        default: return -1;
    }
}

constexpr i32 kChestBoatTypeId = 17; // VEHICLE_FIX_V2

inline i32 chestBoatVariantForItem(i32 item) {
    switch (item) {
        case 775: return 0; case 777: return 1; case 779: return 2; case 781: return 3;
        case 783: return 4; case 785: return 5; case 787: return 6; case 789: return 7;
        case 791: return 8; default: return -1;
    }
}

// RAIL_SHAPE_V1: 1.21.1 rail state layout, taken from the vanilla property order.
//   rail                       : SHAPE(10) x WATERLOGGED(2)              -> shape stride 2
//   powered/detector/activator : SHAPE(6) x POWERED(2) x WATERLOGGED(2)  -> shape stride 4
// Shape order: 0 north_south, 1 east_west, 2 asc_east, 3 asc_west,
//              4 asc_north, 5 asc_south, 6 south_east, 7 south_west, 8 north_west, 9 north_east.
struct RailFamily { i32 base; i32 stride; i32 shapes; };

inline bool railFamilyOf(i32 s, RailFamily& out) {
    if (s >= 4663 && s < 4683) { out = RailFamily{4663, 2, 10}; return true; }
    if (s >= 1957 && s < 1981) { out = RailFamily{1957, 4, 6};  return true; }
    if (s >= 1981 && s < 1998) { out = RailFamily{1981, 4, 6};  return true; }
    if (s >= 9333 && s < 9345) { out = RailFamily{9333, 4, 6};  return true; }
    return false;
}

inline i32 railShapeIndex(i32 s) {
    RailFamily f{};
    if (!railFamilyOf(s, f)) return 0;
    return ((s - f.base) / f.stride) % f.shapes;
}

inline i32 railWithShape(i32 s, i32 shape) {
    RailFamily f{};
    if (!railFamilyOf(s, f)) return s;
    if (shape < 0 || shape >= f.shapes) return s;
    const i32 cur = ((s - f.base) / f.stride) % f.shapes;
    return s + (shape - cur) * f.stride;
}

// Two exits per rail shape: {dx, dz, dyUp}.
inline void railExits(i32 shape, i32 out[2][3]) {
    auto set = [&](i32 i, i32 dx, i32 dz, i32 dy) { out[i][0] = dx; out[i][1] = dz; out[i][2] = dy; };
    switch (shape) {
        case 1:  set(0,  1, 0, 0); set(1, -1, 0, 0); break; // east_west
        case 2:  set(0,  1, 0, 1); set(1, -1, 0, 0); break; // ascending_east
        case 3:  set(0, -1, 0, 1); set(1,  1, 0, 0); break; // ascending_west
        case 4:  set(0, 0, -1, 1); set(1, 0,  1, 0); break; // ascending_north
        case 5:  set(0, 0,  1, 1); set(1, 0, -1, 0); break; // ascending_south
        case 6:  set(0, 0,  1, 0); set(1,  1, 0, 0); break; // south_east
        case 7:  set(0, 0,  1, 0); set(1, -1, 0, 0); break; // south_west
        case 8:  set(0, 0, -1, 0); set(1, -1, 0, 0); break; // north_west
        case 9:  set(0, 0, -1, 0); set(1,  1, 0, 0); break; // north_east
        default: set(0, 0, -1, 0); set(1, 0,  1, 0); break; // north_south
    }
}

inline i32 floorToInt(f64 v) { return static_cast<i32>(std::floor(v)); }

inline u8 yawToAngleByte(f32 yaw) {
    return static_cast<u8>(static_cast<i32>(yaw * 256.0f / 360.0f) & 0xFF);
}

} // namespace

void NetherCraftServer::spawnVehicle(f64 x, f64 y, f64 z, f32 yaw, i32 typeId, i32 itemId, i32 variant) {
    { std::lock_guard lk(vehiclesMutex_); if (vehicles_.size() >= 4096) return; }
    const i32 eid = static_cast<i32>(nextEntityId_++);
    VehicleMotion v{};
    v.eid = eid; v.typeId = typeId; v.itemId = itemId; v.variant = variant;
    v.x = x; v.y = y; v.z = z; v.yaw = yaw;
    v.vx = 0.0; v.vy = 0.0; v.vz = 0.0;
    v.dirX = 0; v.dirZ = 0; v.speed = 0.0;
    v.passengerEid = 0; v.onGround = false;
    { std::lock_guard lk(vehiclesMutex_); vehicles_.push_back(v); }
    const u8 yawByte = yawToAngleByte(yaw);
    net::Buffer sp;
    sp.writeVarInt(eid);
    sp.writeUUID(UUID{static_cast<u64>(eid), 0x76000000ULL + static_cast<u64>(eid)});
    sp.writeVarInt(typeId);
    sp.writeF64(x); sp.writeF64(y); sp.writeF64(z);
    sp.writeByte(0); sp.writeByte(yawByte); sp.writeByte(yawByte);
    sp.writeVarInt(0);
    sp.writeI16(0); sp.writeI16(0); sp.writeI16(0);
    const auto bytes = std::vector<u8>(sp.writtenSpan().begin(), sp.writtenSpan().end());
    std::vector<u8> metaBytes;
    if ((typeId == kBoatTypeId || typeId == kChestBoatTypeId) && variant > 0) {
        net::Buffer md;
        md.writeVarInt(eid);
        md.writeByte(11); md.writeVarInt(1); md.writeVarInt(variant); // Boat type (VarInt serializer)
        md.writeByte(0xFF);
        metaBytes.assign(md.writtenSpan().begin(), md.writtenSpan().end());
    }
    for (auto& p : getAllPlayersCopy()) {
        if (!p || !p->isAlive() || p->getState() != entity::PlayerState::Play) continue;
        p->getConnection()->sendPacket(0x01, bytes);
        if (!metaBytes.empty()) p->getConnection()->sendPacket(0x58, metaBytes);
    }
}

void NetherCraftServer::broadcastVehiclePassengers(i32 vehicleEid, i32 passengerEid) {
    net::Buffer sp;
    sp.writeVarInt(vehicleEid);
    if (passengerEid != 0) { sp.writeVarInt(1); sp.writeVarInt(passengerEid); }
    else sp.writeVarInt(0);
    const auto bytes = std::vector<u8>(sp.writtenSpan().begin(), sp.writtenSpan().end());
    for (auto& p : getAllPlayersCopy())
        if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
            p->getConnection()->sendPacket(0x5F, bytes);
}

bool NetherCraftServer::vehicleInteract(const std::shared_ptr<entity::Player>& player, i32 vehicleEid) {
    if (!player) return false;
    const i32 pEid = static_cast<i32>(player->getEntityId());
    f64 vx = 0.0, vy = 0.0, vz = 0.0;
    std::vector<i32> leftBehind;
    {
        std::lock_guard lk(vehiclesMutex_);
        auto it = std::find_if(vehicles_.begin(), vehicles_.end(),
            [&](const VehicleMotion& v) { return v.eid == vehicleEid; });
        if (it == vehicles_.end()) return false;
        if (it->passengerEid != 0 && it->passengerEid != pEid) return true; // seat taken
        for (auto& other : vehicles_)
            if (other.eid != vehicleEid && other.passengerEid == pEid) { other.passengerEid = 0; leftBehind.push_back(other.eid); }
        it->passengerEid = pEid;
        vx = it->x; vy = it->y; vz = it->z;
    }
    player->ridingVehicleEid = vehicleEid;
    player->vehicleForward = 0.0f;
    player->vehicleSideways = 0.0f;
    player->setPosition(vx, vy, vz);
    for (i32 old : leftBehind) broadcastVehiclePassengers(old, 0);
    broadcastVehiclePassengers(vehicleEid, pEid);
    return true;
}

bool NetherCraftServer::vehicleAttack(const std::shared_ptr<entity::Player>& player, i32 vehicleEid) {
    i32 itemId = 0, passenger = 0;
    f64 x = 0.0, y = 0.0, z = 0.0;
    bool survived = false;
    {
        std::lock_guard lk(vehiclesMutex_);
        auto it = std::find_if(vehicles_.begin(), vehicles_.end(),
            [&](const VehicleMotion& v) { return v.eid == vehicleEid; });
        if (it == vehicles_.end()) return false;
        // VEHICLE_FIX_V2: like vanilla, a boat needs several bare-handed hits.
        const bool boatLike = (it->typeId == kBoatTypeId || it->typeId == kChestBoatTypeId);
        const bool instant = (player && player->gameMode == 1);
        if (boatLike && !instant && ++it->hits < 3) survived = true;
        if (!survived) {
            itemId = it->itemId; passenger = it->passengerEid;
            x = it->x; y = it->y; z = it->z;
            vehicles_.erase(it);
        }
    }
    if (survived) {
        net::Buffer hb; hb.writeVarInt(vehicleEid); hb.writeF32(0.0f);
        const auto hbytes = std::vector<u8>(hb.writtenSpan().begin(), hb.writtenSpan().end());
        for (auto& p : getAllPlayersCopy())
            if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                p->getConnection()->sendPacket(0x24, hbytes);
        return true;
    }
    if (passenger != 0) {
        for (auto& p : getAllPlayersCopy())
            if (p && static_cast<i32>(p->getEntityId()) == passenger) { p->ridingVehicleEid = 0; break; }
        broadcastVehiclePassengers(vehicleEid, 0);
    }
    net::Buffer rm; rm.writeVarInt(1); rm.writeVarInt(vehicleEid);
    const auto bytes = std::vector<u8>(rm.writtenSpan().begin(), rm.writtenSpan().end());
    for (auto& p : getAllPlayersCopy())
        if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
            p->getConnection()->sendPacket(0x42, bytes);
    if (itemId > 0 && (!player || player->gameMode != 1))
        spawnItemDrop(x, y + 0.25, z, itemId, 1, 0.0, 0.1, 0.0);
    return true;
}

void NetherCraftServer::vehicleDismount(const std::shared_ptr<entity::Player>& player) {
    if (!player || player->ridingVehicleEid == 0) return;
    const i32 vehicleEid = player->ridingVehicleEid;
    const i32 pEid = static_cast<i32>(player->getEntityId());
    f64 x = player->getX(), y = player->getY(), z = player->getZ();
    {
        std::lock_guard lk(vehiclesMutex_);
        for (auto& v : vehicles_)
            if (v.eid == vehicleEid && v.passengerEid == pEid) { v.passengerEid = 0; x = v.x; y = v.y; z = v.z; }
    }
    player->ridingVehicleEid = 0;
    player->vehicleForward = 0.0f;
    player->vehicleSideways = 0.0f;
    player->setPosition(x, y + 1.0, z);          // VEHICLE_FIX_V2: was landing a block below
    broadcastVehiclePassengers(vehicleEid, 0);
    sendPlayerPositionAndLook(player);           // force the client onto the same spot
}

void NetherCraftServer::handleVehicleMove(const std::shared_ptr<entity::Player>& player, f64 x, f64 y, f64 z, f32 yaw) {
    if (!player || player->ridingVehicleEid == 0) return;
    const i32 vehicleEid = player->ridingVehicleEid;
    const i32 pEid = static_cast<i32>(player->getEntityId());
    bool moved = false;
    {
        std::lock_guard lk(vehiclesMutex_);
        for (auto& v : vehicles_) {
            if (v.eid != vehicleEid || v.passengerEid != pEid) continue;
            // sanity clamp: ignore absurd jumps instead of trusting the client blindly
            if (std::abs(x - v.x) > 32.0 || std::abs(z - v.z) > 32.0 || std::abs(y - v.y) > 32.0) return;
            if (y < static_cast<f64>(world::CHUNK_HEIGHT_MIN) - 64.0 || y > static_cast<f64>(world::CHUNK_HEIGHT_MAX) + 64.0) return;
            v.vx = x - v.x; v.vy = y - v.y; v.vz = z - v.z;
            v.x = x; v.y = y; v.z = z; v.yaw = yaw;
            moved = true;
            break;
        }
    }
    if (!moved) return;
    player->setPosition(x, y, z);
    net::Buffer tp;
    tp.writeVarInt(vehicleEid);
    tp.writeF64(x); tp.writeF64(y); tp.writeF64(z);
    tp.writeByte(yawToAngleByte(yaw)); tp.writeByte(0); tp.writeBool(true);
    broadcastToOthers(player, 0x70, std::vector<u8>(tp.writtenSpan().begin(), tp.writtenSpan().end()), true);
}

bool NetherCraftServer::placeVehicleItem(const std::shared_ptr<entity::Player>& player, i32 itemId, i32 bx, i32 by, i32 bz, i32 face) {
    if (!player || player->gameMode == 3) return false;
    i32 boatVariant = boatVariantForItem(itemId);
    i32 boatType = kBoatTypeId;
    if (boatVariant < 0) { // VEHICLE_FIX_V2: chest boats are a separate entity type
        const i32 cv = chestBoatVariantForItem(itemId);
        if (cv >= 0) { boatVariant = cv; boatType = kChestBoatTypeId; }
    }
    const bool minecart = (itemId == 766);
    if (!minecart && boatVariant < 0) return false;
    i32 tx = bx, ty = by, tz = bz;
    switch (face) { case 0: --ty; break; case 1: ++ty; break; case 2: --tz; break;
                    case 3: ++tz; break; case 4: --tx; break; case 5: ++tx; break; default: break; }
    if (minecart) {
        i32 rx = bx, ry = by, rz = bz;
        if (!isVehicleRailState(world_.getBlock(rx, ry, rz))) {
            rx = tx; ry = ty; rz = tz;
            if (!isVehicleRailState(world_.getBlock(rx, ry, rz))) {
                player->sendSystemMessage(config_.language == "rus"
                    ? "§cВагонетку можно поставить только на рельсы"
                    : "§cA minecart can only be placed on rails");
                return false;
            }
        }
        f32 cartYaw = player->get_yaw(); // MINECART_YAW_V1: вагонетка встаёт вдоль рельсы, а не по взгляду
        {
            const i32 railSt = world_.getBlock(rx, ry, rz);
            RailFamily cf{};
            if (railFamilyOf(railSt, cf)) {
                switch (railShapeIndex(railSt)) {
                    case 0: case 4: case 5: cartYaw = 0.0f;   break; // north-south + подъёмы север/юг
                    case 1: case 2: case 3: cartYaw = 90.0f;  break; // east-west + подъёмы восток/запад
                    case 6: cartYaw = 45.0f;  break;                 // south-east
                    case 7: cartYaw = 135.0f; break;                 // south-west
                    case 8: cartYaw = 225.0f; break;                 // north-west
                    case 9: cartYaw = 315.0f; break;                 // north-east
                    default: break;
                }
            }
        }
        spawnVehicle(rx + 0.5, ry + 0.0625, rz + 0.5, cartYaw, kMinecartTypeId, itemId, 0);
        broadcastBlockSound("minecraft:entity.minecart.riding", rx, ry, rz, 0.5f, 1.0f);
        return true;
    }
    // VEHICLE_FIX_V2: clicking water puts the boat on the actual surface, even on a
    // raised water column where the clicked face points at air.
    i32 wx = bx, wy = by, wz = bz;
    if (!isVehicleWaterState(world_.getBlock(wx, wy, wz))) { wx = tx; wy = ty; wz = tz; }
    if (!isVehicleWaterState(world_.getBlock(wx, wy, wz)) &&
        wy - 1 >= world::CHUNK_HEIGHT_MIN && isVehicleWaterState(world_.getBlock(wx, wy - 1, wz))) --wy;
    if (isVehicleWaterState(world_.getBlock(wx, wy, wz))) {
        i32 guard = 0;
        while (guard++ < 512 && wy + 1 < world::CHUNK_HEIGHT_MAX &&
               isVehicleWaterState(world_.getBlock(wx, wy + 1, wz))) ++wy;
        spawnVehicle(wx + 0.5, wy + 0.9, wz + 0.5, player->get_yaw(), boatType, itemId, boatVariant);
        return true;
    }
    if (ty < world::CHUNK_HEIGHT_MIN || ty >= world::CHUNK_HEIGHT_MAX) return false;
    if (!isFreeForFalling(world_.getBlock(tx, ty, tz))) return false;
    spawnVehicle(tx + 0.5, static_cast<f64>(ty), tz + 0.5,
                 player->get_yaw(), boatType, itemId, boatVariant);
    return true;
}

// RAIL_SHAPE_V1: choose the shape a freshly placed rail should take, vanilla-style.
i32 NetherCraftServer::computeRailState(i32 x, i32 y, i32 z, i32 state, f32 placerYaw) {
    RailFamily fam{};
    if (!railFamilyOf(state, fam)) return state;
    const bool curves = (fam.shapes == 10);
    const i32 dDx[4] = {0, 0, 1, -1};   // north, south, east, west
    const i32 dDz[4] = {-1, 1, 0, 0};
    bool conn[4] = {false, false, false, false};
    bool up[4]   = {false, false, false, false};
    // RAIL_SHAPE_V2: раньше соединением считались ЛЮБЫЕ рельсы рядом, а ванильный клиент
    // считает только те, что УЖЕ смотрят на нас; прямые powered/detector/activator гнуться
    // не умеют (6 форм вместо 10). Из-за расхождения клиент откатывал свою ставку —
    // отсюда «серв хочет так, а кладёт так» и «энерго ставятся с 3 раза».
    bool face[4]  = {false, false, false, false};
    bool rigid[4] = {false, false, false, false};
    auto pointsBack = [&](i32 nshape, i32 d) {
        i32 ex[2][3];
        railExits(nshape, ex);
        for (i32 i = 0; i < 2; ++i)
            if (ex[i][0] == -dDx[d] && ex[i][1] == -dDz[d]) return true;
        return false;
    };
    // RAIL_SHAPE_V3: V2 резал слишком много (сосед не смотрит на нас — связи нет), сервер перебивал
    // клиентскую форму и это выглядело как откат. Ванилла ��роще: соединяемся, если сосед уже
    // смотрит на нас ЛИБО он гибкий (10 форм) и у него ещё есть свободный выход.
    auto busyExits = [&](i32 nx2, i32 ny2, i32 nz2, i32 nshape) {
        i32 ex[2][3];
        railExits(nshape, ex);
        i32 busy = 0;
        for (i32 i = 0; i < 2; ++i) {
            const i32 ox = nx2 + ex[i][0], oz = nz2 + ex[i][1];
            for (i32 dy2 = 1; dy2 >= -1; --dy2) {
                const i32 oy = ny2 + dy2;
                if (oy < world::CHUNK_HEIGHT_MIN || oy >= world::CHUNK_HEIGHT_MAX) continue;
                if (isVehicleRailState(world_.getBlock(ox, oy, oz))) { ++busy; break; }
            }
        }
        return busy;
    };
    for (i32 d = 0; d < 4; ++d) {
        const i32 nx = x + dDx[d], nz = z + dDz[d];
        i32 ny = y;
        i32 ns = world_.getBlock(nx, y, nz);
        bool found = false, isUp = false;
        if (isVehicleRailState(ns)) found = true;
        else if (y + 1 < world::CHUNK_HEIGHT_MAX && isVehicleRailState(ns = world_.getBlock(nx, y + 1, nz))) { found = true; isUp = true; ny = y + 1; }
        else if (y - 1 >= world::CHUNK_HEIGHT_MIN && isVehicleRailState(ns = world_.getBlock(nx, y - 1, nz))) { found = true; ny = y - 1; }
        if (!found) continue;
        conn[d] = true;
        up[d] = isUp;
        RailFamily nf{};
        railFamilyOf(ns, nf);
        const i32 nshape = railShapeIndex(ns);
        if (pointsBack(nshape, d)) face[d] = true;
        else if (nf.shapes == 6) rigid[d] = true;
        else if (busyExits(nx, ny, nz, nshape) >= 2) rigid[d] = true;
    }
    {
        i32 nFace = 0;
        for (i32 d = 0; d < 4; ++d) if (face[d]) ++nFace;
        if (nFace >= 2) { for (i32 d = 0; d < 4; ++d) if (!face[d]) conn[d] = false; }
        else { for (i32 d = 0; d < 4; ++d) if (rigid[d] && !face[d]) conn[d] = false; }
    }
    const bool n = conn[0], so = conn[1], e = conn[2], w = conn[3];
    i32 shape;
    if (n && so) shape = 0;
    else if (e && w) shape = 1;
    else if (curves && so && e) shape = 6;
    else if (curves && so && w) shape = 7;
    else if (curves && n && w) shape = 8;
    else if (curves && n && e) shape = 9;
    else if (n || so) shape = 0;
    else if (e || w) shape = 1;
    else {
        f32 yaw = placerYaw;
        while (yaw < 0.0f) yaw += 360.0f;
        while (yaw >= 360.0f) yaw -= 360.0f;
        const i32 q = static_cast<i32>((yaw / 90.0f) + 0.5f) & 3; // 0 south, 1 west, 2 north, 3 east
        shape = (q == 0 || q == 2) ? 0 : 1;
    }
    if (shape == 0) { if (up[0] && !up[1]) shape = 4; else if (up[1] && !up[0]) shape = 5; }
    else if (shape == 1) { if (up[2] && !up[3]) shape = 2; else if (up[3] && !up[2]) shape = 3; }
    return railWithShape(state, shape);
}

// RAIL_SHAPE_V1: re-bend the rails around a change so tracks actually connect,
// and re-send the touched blocks because the client predicted its own shape.
void NetherCraftServer::updateRailShapesAround(i32 x, i32 y, i32 z) {
    auto players = getAllPlayersCopy();
    auto send = [&](i32 nx, i32 ny, i32 nz, i32 st) {
        net::Buffer bu;
        bu.writePosition(BlockPos{nx, ny, nz});
        bu.writeVarInt(st);
        const auto bytes = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
        for (auto& p : players)
            if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                p->getConnection()->sendPacket(0x09, bytes);
    };
    const i32 self = world_.getBlock(x, y, z);
    if (isVehicleRailState(self)) send(x, y, z, self);
    const i32 dx[4] = {1, -1, 0, 0};
    const i32 dz[4] = {0, 0, 1, -1};
    for (i32 d = 0; d < 4; ++d) {
        for (i32 dy = -1; dy <= 1; ++dy) {
            const i32 nx = x + dx[d], ny = y + dy, nz = z + dz[d];
            if (ny < world::CHUNK_HEIGHT_MIN || ny >= world::CHUNK_HEIGHT_MAX) continue;
            const i32 old = world_.getBlock(nx, ny, nz);
            if (!isVehicleRailState(old)) continue;
            const i32 fresh = computeRailState(nx, ny, nz, old, 0.0f);
            if (fresh == old) continue;
            world_.setBlock(nx, ny, nz, fresh);
            send(nx, ny, nz, fresh);
        }
    }
}

void NetherCraftServer::tickVehicles() {
    std::vector<VehicleMotion> moving;
    { std::lock_guard lk(vehiclesMutex_); moving = vehicles_; }
    if (moving.empty()) return;
    auto players = getAllPlayersCopy();
    auto findRider = [&](i32 eid) -> std::shared_ptr<entity::Player> {
        for (auto& p : players)
            if (p && p->isAlive() && p->getState() == entity::PlayerState::Play &&
                static_cast<i32>(p->getEntityId()) == eid) return p;
        return nullptr;
    };
    std::unordered_map<i32, VehicleMotion> updated;
    std::vector<i32> emptied;
    for (auto& v : moving) {
        std::shared_ptr<entity::Player> rider;
        if (v.passengerEid != 0) {
            rider = findRider(v.passengerEid);
            if (!rider || rider->ridingVehicleEid != v.eid) {
                v.passengerEid = 0; emptied.push_back(v.eid); rider.reset();
            }
        }
        const bool boat = (v.typeId == kBoatTypeId || v.typeId == kChestBoatTypeId);
        // A ridden boat is driven by its client; nothing to simulate here.
        if (boat && rider) { updated.emplace(v.eid, v); continue; }
        const bool idle = (v.speed == 0.0 && v.vx == 0.0 && v.vy == 0.0 && v.vz == 0.0);
        if (boat) {
            const i32 medium = world_.getBlock(floorToInt(v.x), floorToInt(v.y + 0.35), floorToInt(v.z));
            if (isVehicleWaterState(medium)) { v.vy = v.vy * 0.6 + 0.02; if (v.vy > 0.1) v.vy = 0.1; }
            else v.vy -= 0.04;
            v.vx *= 0.9; v.vz *= 0.9;
            f64 ny = v.y + v.vy;
            const i32 below = floorToInt(ny - 1.0e-6);
            if (v.vy <= 0.0 && !isFreeForFalling(world_.getBlock(floorToInt(v.x), below, floorToInt(v.z)))) {
                ny = below + 1.0; v.vy = 0.0; v.onGround = true;
            } else v.onGround = false;
            v.y = ny; v.x += v.vx; v.z += v.vz;
            if (std::abs(v.vx) < 1.0e-4) v.vx = 0.0;
            if (std::abs(v.vz) < 1.0e-4) v.vz = 0.0;
            if (v.onGround && std::abs(v.vy) < 1.0e-4) v.vy = 0.0;
        } else {
            const i32 bx = floorToInt(v.x), bz = floorToInt(v.z);
            i32 railY = floorToInt(v.y);
            bool onRail = isVehicleRailState(world_.getBlock(bx, railY, bz));
            if (!onRail) { railY = floorToInt(v.y - 0.25); onRail = isVehicleRailState(world_.getBlock(bx, railY, bz)); }
            if (!onRail) {
                v.vy -= 0.04;
                f64 ny = v.y + v.vy;
                const i32 below = floorToInt(ny - 1.0e-6);
                if (v.vy <= 0.0 && !isFreeForFalling(world_.getBlock(bx, below, bz))) {
                    ny = below + 1.0; v.vy = 0.0; v.onGround = true;
                } else v.onGround = false;
                v.y = ny; v.x += v.vx; v.z += v.vz;
                v.vx *= 0.92; v.vz *= 0.92;
                v.speed = std::sqrt(v.vx * v.vx + v.vz * v.vz);
                if (v.speed < 0.002) { v.speed = 0.0; v.vx = 0.0; v.vz = 0.0; }
            } else {
                if (rider) {
                    const f64 fwd = static_cast<f64>(rider->vehicleForward);
                    if (std::abs(fwd) > 0.1 && v.dirX == 0 && v.dirZ == 0) {
                        const f64 yr = static_cast<f64>(rider->get_yaw()) * 0.017453292519943295;
                        const f64 lookX = -std::sin(yr), lookZ = std::cos(yr);
                        i32 sx = 0, sz = 0;
                        if (std::abs(lookX) >= std::abs(lookZ)) sx = lookX > 0.0 ? 1 : -1;
                        else sz = lookZ > 0.0 ? 1 : -1;
                        if (fwd < 0.0) { sx = -sx; sz = -sz; }
                        i32 sExits[2][3];
                        railExits(railShapeIndex(world_.getBlock(bx, railY, bz)), sExits);
                        i32 sPick = 0; f64 sBest = -2.0;
                        for (i32 c = 0; c < 2; ++c) {
                            const f64 sc = static_cast<f64>(sExits[c][0] * sx + sExits[c][1] * sz);
                            if (sc > sBest) { sBest = sc; sPick = c; }
                        }
                        v.dirX = sExits[sPick][0]; v.dirZ = sExits[sPick][1]; v.speed = 0.04;
                    } else if (std::abs(fwd) > 0.1) {
                        v.speed = std::min(0.4, v.speed + 0.012);
                    } else {
                        v.speed *= 0.96;
                    }
                } else {
                    v.speed *= 0.985;
                }
                if (v.speed < 0.002) { v.speed = 0.0; v.dirX = 0; v.dirZ = 0; }
                if (v.speed > 0.0) {
                    // RAIL_SHAPE_V1: exits come from the rail shape, so curves and slopes work.
                    i32 exits[2][3];
                    railExits(railShapeIndex(world_.getBlock(bx, railY, bz)), exits);
                    if (rider && rider->vehicleForward < -0.1f && v.speed < 0.12) { v.dirX = -v.dirX; v.dirZ = -v.dirZ; }
                    i32 pick = 0; f64 bestScore = -2.0;
                    for (i32 c = 0; c < 2; ++c) {
                        const f64 score = static_cast<f64>(exits[c][0] * v.dirX + exits[c][1] * v.dirZ);
                        if (score > bestScore) { bestScore = score; pick = c; }
                    }
                    const i32 ndx = exits[pick][0], ndz = exits[pick][1], nUp = exits[pick][2];
                    i32 nextY = railY + nUp;
                    bool nextRail = isVehicleRailState(world_.getBlock(bx + ndx, nextY, bz + ndz));
                    if (!nextRail && nUp == 0 && railY - 1 >= world::CHUNK_HEIGHT_MIN &&
                        isVehicleRailState(world_.getBlock(bx + ndx, railY - 1, bz + ndz))) {
                        nextY = railY - 1; nextRail = true; // the neighbour rail climbs towards us
                    }
                    if (!nextRail) {
                        v.speed = 0.0; v.dirX = 0; v.dirZ = 0;
                        v.x = bx + 0.5; v.z = bz + 0.5; v.y = railY + 0.0625;
                        v.vx = 0.0; v.vy = 0.0; v.vz = 0.0;
                    } else {
                        v.dirX = ndx; v.dirZ = ndz;
                        const f64 targetX = (bx + ndx) + 0.5;
                        const f64 targetZ = (bz + ndz) + 0.5;
                        const f64 targetY = nextY + 0.0625;
                        const f64 ddx = targetX - v.x, ddz = targetZ - v.z, ddy = targetY - v.y;
                        const f64 dist = std::sqrt(ddx * ddx + ddz * ddz);
                        if (dist <= v.speed || dist < 1.0e-6) { v.x = targetX; v.z = targetZ; v.y = targetY; }
                        else { const f64 k = v.speed / dist; v.x += ddx * k; v.z += ddz * k; v.y += ddy * k; }
                        if (nextY > railY) v.speed = std::max(0.0, v.speed - 0.012);
                        else if (nextY < railY) v.speed = std::min(0.4, v.speed + 0.008);
                        v.vx = static_cast<f64>(v.dirX) * v.speed;
                        v.vz = static_cast<f64>(v.dirZ) * v.speed;
                        v.vy = 0.0;
                    }
                } else {
                    v.x = bx + 0.5; v.z = bz + 0.5; v.y = railY + 0.0625;
                    v.vx = 0.0; v.vy = 0.0; v.vz = 0.0;
                }
                v.onGround = true;
                if (v.dirX != 0) v.yaw = v.dirX > 0 ? -90.0f : 90.0f;
                else if (v.dirZ != 0) v.yaw = v.dirZ > 0 ? 0.0f : 180.0f;
            }
        }
        const bool stillIdle = (v.speed == 0.0 && v.vx == 0.0 && v.vy == 0.0 && v.vz == 0.0);
        if (idle && stillIdle && !rider) { updated.emplace(v.eid, v); continue; } // ANTILAG: silent parked vehicles
        if (rider) rider->setPosition(v.x, v.y, v.z);
        net::Buffer tp;
        tp.writeVarInt(v.eid);
        tp.writeF64(v.x); tp.writeF64(v.y); tp.writeF64(v.z);
        tp.writeByte(yawToAngleByte(v.yaw)); tp.writeByte(0); tp.writeBool(v.onGround);
        const auto bytes = std::vector<u8>(tp.writtenSpan().begin(), tp.writtenSpan().end());
        for (auto& p : players)
            if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                p->getConnection()->sendPacket(0x70, bytes);
        // Vanilla vehicle clients also expect Move Vehicle while seated; this is emitted
        // from the actual boat/minecart simulation, not a command.
        if (rider) {
            net::Buffer vm;
            vm.writeF64(v.x); vm.writeF64(v.y); vm.writeF64(v.z);
            vm.writeF32(v.yaw); vm.writeF32(0.0f);
            rider->getConnection()->sendPacket(0x31, std::vector<u8>(vm.writtenSpan().begin(), vm.writtenSpan().end()));
        }
        updated.emplace(v.eid, v);
    }
    for (i32 eid : emptied) broadcastVehiclePassengers(eid, 0);
    std::lock_guard lk(vehiclesMutex_);
    for (auto& v : vehicles_) {
        auto it = updated.find(v.eid);
        if (it != updated.end()) v = it->second;
    }
}

// ============================================================
// PROJECTILE_V2: common throwable raycast for pearls/snowballs/eggs.
// ============================================================
void NetherCraftServer::spawnThrowableProjectile(const std::shared_ptr<entity::Player>& owner, f32 yaw, f32 pitch,
                                                 i32 entityTypeId, i32 kind) {
    if (!owner) return;
    const f64 yr = static_cast<f64>(yaw) * 0.017453292519943295;
    const f64 pr = static_cast<f64>(pitch) * 0.017453292519943295;
    const f64 dirX = -std::sin(yr) * std::cos(pr);
    const f64 dirY = -std::sin(pr);
    const f64 dirZ =  std::cos(yr) * std::cos(pr);
    // THROW_SPEED_V1: vanilla Projectile.spawnProjectileFromRotation uses power=1.5 for
    // snowball/egg/ender pearl but only power=0.7 for the experience bottle (heavier arc).
    // Everything previously flew at a uniform 1.5, so bottles were noticeably faster/flatter
    // than vanilla.
    const f64 speed = (kind == 4) ? 0.7 : 1.5;
    const f64 vx = dirX * speed;
    const f64 vy = dirY * speed;
    const f64 vz = dirZ * speed;
    // PROJECTILE_SPAWN_V3: ordinary throwables start farther in front of the eyes,
    // so point-blank throws do not instantly clip into the owner-side space.
    const f64 spawnX = owner->getX() + dirX * 0.65;
    const f64 spawnY = owner->getY() + 1.52 - 0.05 + dirY * 0.30;
    const f64 spawnZ = owner->getZ() + dirZ * 0.65;
    const i32 eid = static_cast<i32>(nextEntityId_++);
    ProjectileMotion q{eid, entityTypeId, kind, static_cast<i32>(owner->getEntityId()), spawnX,
                       spawnY, spawnZ, vx, vy, vz, 0};
    { std::lock_guard lk(projectilesMutex_); projectiles_.push_back(q); }
    net::Buffer sp;
    sp.writeVarInt(eid); sp.writeUUID(UUID{static_cast<u64>(eid), 0x72000000ULL + static_cast<u64>(eid)});
    sp.writeVarInt(entityTypeId); sp.writeF64(q.x); sp.writeF64(q.y); sp.writeF64(q.z);
    sp.writeByte(0); sp.writeByte(0); sp.writeByte(0); sp.writeVarInt(q.ownerEid);
    auto vel = [](f64 v) { return static_cast<i16>(std::clamp(v * 8000.0, -32000.0, 32000.0)); };
    sp.writeI16(vel(vx)); sp.writeI16(vel(vy)); sp.writeI16(vel(vz));
    const auto bytes = std::vector<u8>(sp.writtenSpan().begin(), sp.writtenSpan().end());
    const auto viewers = getAllPlayersCopy();
    for (auto& p : viewers) if (p && p->isAlive()) p->getConnection()->sendPacket(0x01, bytes);
    // THROW_ITEM_META_V1: ThrowableItemProjectile.handleEntityEvent(3) renders its break
    // particles from the entity's own DATA_ITEM_STACK metadata (index 8, Slot). Snowball and
    // egg use that generic path and stayed invisible without this packet; ender pearl uses a
    // separate portal effect and experience bottle hardcodes INSTANT_EFFECT particles, so
    // neither strictly needed it, but sending it for every throwable matches vanilla state.
    // ENTITY_ID_FIX_V2: id реестра идут в порядке объявления в EntityType.java, а не по алфавиту.
    // Проверено по всем 82 известным netId мобов: snowball = 97.
    const i32 throwItemId = entityTypeId == 97 ? 912 : (entityTypeId == 28 ? 927 : (entityTypeId == 37 ? 1088 : 993));
    net::Buffer meta;
    meta.writeVarInt(eid);
    meta.writeByte(8);   // ThrowableItemProjectile: index 8 = сам предмет
    meta.writeVarInt(7); // сериализатор 7: Slot
    meta.writeVarInt(1);
    meta.writeVarInt(throwItemId);
    meta.writeVarInt(0); meta.writeVarInt(0); // компонентов: +0 / -0
    meta.writeByte(0xFF); // конец метадаты
    const auto metaBytes = std::vector<u8>(meta.writtenSpan().begin(), meta.writtenSpan().end());
    for (auto& p : viewers) if (p && p->isAlive()) p->getConnection()->sendPacket(0x58, metaBytes);
}

void NetherCraftServer::spawnEnderPearl(const std::shared_ptr<entity::Player>& owner, f32 yaw, f32 pitch) {
    spawnThrowableProjectile(owner, yaw, pitch, 32, 1);
}

void NetherCraftServer::tickProjectiles() {
    std::vector<ProjectileMotion> moving;
    { std::lock_guard lk(projectilesMutex_); moving = projectiles_; }
    if (moving.empty()) return;
    auto players = getAllPlayersCopy();
    std::vector<i32> removed;
    std::unordered_map<i32, ProjectileMotion> updated;
    for (auto& q : moving) {
        ++q.age;
        std::shared_ptr<entity::Player> owner;
        for (auto& p : players) if (p && static_cast<i32>(p->getEntityId()) == q.ownerEid) { owner = p; break; }
        if (!owner || owner->dead || !owner->isAlive() || q.age > 1200) {
            removed.push_back(q.eid);
        } else {
            const f64 ox = q.x, oy = q.y, oz = q.z;
            const f64 nx = ox + q.vx, ny = oy + q.vy, nz = oz + q.vz;
            const i32 steps = std::max(1, static_cast<i32>(std::ceil(std::sqrt(q.vx*q.vx + q.vy*q.vy + q.vz*q.vz) / 0.1)));
            bool hit = false; f64 hx = nx, hy = ny, hz = nz;
            std::shared_ptr<entity::Player> hitPlayer;
            // THROW_RACE_FIX_V1 (REVERTED): delaying block-hit eligibility until age>=4
            // was meant to give the client time to receive Spawn Entity before Entity
            // Status(3), but it back-fired for the common case this was supposed to help:
            // a point-blank throw straight down/into a wall. Blocking collision checks for
            // the first several ticks does not pause the projectile -- it keeps moving and
            // can tunnel clean through a thin floor/wall during the ignored ticks, so the
            // hit only registers far past the intended surface (underground, or dozens of
            // blocks away once it finally hits something else). That matches the reports:
            // particles appearing far from the throw, or not at all when the hit ends up
            // inside solid ground out of view. Ender pearl (kind==1) never had this gate and
            // never had this problem, which is the strongest evidence the gate itself, not
            // network timing, was the bug. Removing it restores physically correct collision
            // at the true first point of contact for every throwable, same as pearl.
            const bool blockHitAllowed = true;
            for (i32 i = 1; i <= steps; ++i) {
                const f64 a = static_cast<f64>(i) / steps;
                const f64 sx = ox + (nx - ox) * a, sy = oy + (ny - oy) * a, sz = oz + (nz - oz) * a;
                const i32 state = world_.getBlock(static_cast<i32>(std::floor(sx)), static_cast<i32>(std::floor(sy)), static_cast<i32>(std::floor(sz)));
                if (blockHitAllowed && state > 0 && !(state >= 80 && state <= 111) && !isFreeForFalling(state)) {
                    const f64 prev = static_cast<f64>(std::max(0, i - 1)) / steps;
                    hx = ox + (nx - ox) * prev; hy = oy + (ny - oy) * prev; hz = oz + (nz - oz) * prev;
                    hit = true; break;
                }
                if (q.age > 4) for (auto& target : players) {
                    if (!target || target.get() == owner.get() || target->dead) continue;
                    const f64 dx = target->getX() - sx, dz = target->getZ() - sz;
                    if (dx*dx + dz*dz <= 0.45*0.45 && sy >= target->getY() && sy <= target->getY()+1.8) {
                        hx = sx; hy = sy; hz = sz; hit = true; hitPlayer = target; break;
                    }
                }
                if (hit) break;
            }
            if (hit && q.kind == 1) {
                owner->setPosition(hx, hy, hz);
                owner->fallPeakY = hy; owner->fallWasOnGround = false;
                sendPlayerPositionAndLook(owner);
                applyEnvironmentalDamage(owner, 5.0f, 9, std::format("{} ударился об землю", owner->getName()));
                broadcastBlockSound("minecraft:entity.player.teleport", static_cast<i32>(std::floor(hx)),
                                    static_cast<i32>(std::floor(hy)), static_cast<i32>(std::floor(hz)), 1.0f, 1.0f);
                removed.push_back(q.eid);
            } else if (hit && (q.kind == 2 || q.kind == 3 || q.kind == 4)) {
                if (q.kind == 2 && hitPlayer) {
                    // SNOWBALL_V1: vanilla damage matters mostly for blazes; players get 0.
                    // The current server keeps projectile combat server-authoritative without
                    // introducing a new damage source just for harmless throwables.
                }
                if (q.kind == 4) {
                    // XP_ORB_V1: spawn real, visible experience orb entities at the impact
                    // point instead of an invisible instant grant. Orbs float toward the
                    // nearest player and are split into vanilla-sized chunks, matching
                    // net.minecraft.world.entity.ExperienceOrb.award.
                    i32 remainingXp = 3 + (std::rand() % 5) + (std::rand() % 5); // vanilla bottle: 3-13 xp
                    static const i32 kOrbSizes[] = {2477, 1237, 617, 307, 149, 73, 37, 17, 7, 3, 1};
                    while (remainingXp > 0) {
                        i32 orbAmount = remainingXp;
                        for (i32 size : kOrbSizes) { if (remainingXp >= size) { orbAmount = size; break; } }
                        spawnExperienceOrb(hx, hy + 0.1, hz, orbAmount);
                        remainingXp -= orbAmount;
                    }
                    broadcastBlockSound("minecraft:entity.experience_bottle.throw",
                                        static_cast<i32>(std::floor(hx)),
                                        static_cast<i32>(std::floor(hy)),
                                        static_cast<i32>(std::floor(hz)), 0.7f, 1.0f);
                }
                net::Buffer ev; ev.writeI32(q.eid); ev.writeByte(3);
                const auto evBytes = std::vector<u8>(ev.writtenSpan().begin(), ev.writtenSpan().end());
                i32 sentTo = 0;
                for (auto& p : players) if (p && p->isAlive()) { p->getConnection()->sendPacket(0x1F, evBytes); ++sentTo; }
                (void)sentTo;
                removed.push_back(q.eid);
            } else if (!hit) {
                q.x = nx; q.y = ny; q.z = nz;
                const i32 fluid = world_.getBlock(static_cast<i32>(std::floor(q.x)), static_cast<i32>(std::floor(q.y)), static_cast<i32>(std::floor(q.z)));
                const f64 drag = (fluid >= 80 && fluid <= 95) ? 0.8 : 0.99;
                q.vx *= drag; q.vy = q.vy * drag - 0.03; q.vz *= drag;
                const i32 bubble = bubbleColumnPush(world_, static_cast<i32>(std::floor(q.x)),
                                                     static_cast<i32>(std::floor(q.y)),
                                                     static_cast<i32>(std::floor(q.z)));
                if (bubble < 0) q.vy = std::max(-0.3, q.vy - 0.03);
                else if (bubble > 0) q.vy = std::min(0.7, q.vy + 0.06);

                // PEARL_V2: vanilla ServerEntity tracks throwable motion with a
                // Set Entity Motion packet plus a fixed-point relative move.
                // Absolute Teleport Entity every tick fought the client's own
                // interpolation and made pearls visibly snap/jitter.
                auto velocity = [](f64 value) {
                    return static_cast<i16>(std::clamp(value * 8000.0, -31200.0, 31200.0));
                };
                net::Buffer motion;
                motion.writeVarInt(q.eid);
                motion.writeI16(velocity(q.vx)); motion.writeI16(velocity(q.vy)); motion.writeI16(velocity(q.vz));
                const auto motionBytes = std::vector<u8>(motion.writtenSpan().begin(), motion.writtenSpan().end());

                // Difference of rounded absolute fixed-point coordinates avoids
                // accumulating one-unit rounding drift over a long flight.
                auto relative = [](f64 from, f64 to) {
                    const i64 a = static_cast<i64>(std::llround(from * 4096.0));
                    const i64 b = static_cast<i64>(std::llround(to * 4096.0));
                    return static_cast<i16>(std::clamp<i64>(b - a, -32768, 32767));
                };
                net::Buffer move;
                move.writeVarInt(q.eid);
                move.writeI16(relative(ox, q.x)); move.writeI16(relative(oy, q.y)); move.writeI16(relative(oz, q.z));
                move.writeBool(false);
                const auto moveBytes = std::vector<u8>(move.writtenSpan().begin(), move.writtenSpan().end());
                for (auto& p : players) if (p && p->isAlive()) {
                    p->getConnection()->sendPacket(0x5A, motionBytes);
                    p->getConnection()->sendPacket(0x2E, moveBytes);
                }
                updated.emplace(q.eid, q);
            }
        }
        if (std::find(removed.begin(), removed.end(), q.eid) != removed.end()) {
            net::Buffer rm; rm.writeVarInt(1); rm.writeVarInt(q.eid);
            const auto bytes = std::vector<u8>(rm.writtenSpan().begin(), rm.writtenSpan().end());
            for (auto& p : players) if (p && p->isAlive()) p->getConnection()->sendPacket(0x42, bytes);
        }
    }
    std::lock_guard lk(projectilesMutex_);
    std::erase_if(projectiles_, [&](const ProjectileMotion& q) {
        return std::find(removed.begin(), removed.end(), q.eid) != removed.end();
    });
    for (auto& q : projectiles_) { auto it = updated.find(q.eid); if (it != updated.end()) q = it->second; }
}

void NetherCraftServer::spawnExperienceOrb(f64 x, f64 y, f64 z, i32 amount) {
    // XP_ORB_V1: dedicated Spawn Experience Orb packet (0x02) confirmed against the
    // protocol 767 (1.21.1) play.toClient mapping: entityId varint, x/y/z f64, count i16.
    if (amount <= 0) return;
    const i32 eid = static_cast<i32>(nextEntityId_++);
    const f64 vx = (static_cast<f64>(std::rand() % 200) / 100.0 - 1.0) * 0.02;
    const f64 vy = 0.1 + (static_cast<f64>(std::rand() % 100) / 100.0) * 0.1;
    const f64 vz = (static_cast<f64>(std::rand() % 200) / 100.0 - 1.0) * 0.02;
    ExperienceOrb orb{eid, x, y, z, vx, vy, vz, amount, 0};
    { std::lock_guard lk(xpOrbsMutex_); xpOrbs_.push_back(orb); }
    net::Buffer sp;
    sp.writeVarInt(eid);
    sp.writeF64(x); sp.writeF64(y); sp.writeF64(z);
    sp.writeI16(static_cast<i16>(std::clamp(amount, 0, 32767)));
    const auto bytes = std::vector<u8>(sp.writtenSpan().begin(), sp.writtenSpan().end());
    for (auto& p : getAllPlayersCopy()) if (p && p->isAlive()) p->getConnection()->sendPacket(0x02, bytes);
}

void NetherCraftServer::tickExperienceOrbs() {
    std::vector<ExperienceOrb> orbs;
    { std::lock_guard lk(xpOrbsMutex_); orbs = xpOrbs_; }
    if (orbs.empty()) return;
    auto players = getAllPlayersCopy();
    std::vector<i32> removed;
    std::unordered_map<i32, ExperienceOrb> updated;
    for (auto& o : orbs) {
        ++o.age;
        if (o.age > 6000) { removed.push_back(o.eid); continue; } // vanilla despawns orbs after 5 minutes
        std::shared_ptr<entity::Player> nearest;
        f64 nearestDistSq = 8.0 * 8.0; // vanilla XP_RANGE = 8 blocks
        for (auto& p : players) {
            if (!p || !p->isAlive() || p->dead || p->gameMode == 3) continue;
            const f64 dx = p->getX() - o.x, dy = (p->getY() + 1.0) - o.y, dz = p->getZ() - o.z;
            const f64 d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < nearestDistSq) { nearestDistSq = d2; nearest = p; }
        }
        const i32 fluid = world_.getBlock(static_cast<i32>(std::floor(o.x)), static_cast<i32>(std::floor(o.y)), static_cast<i32>(std::floor(o.z)));
        const bool inWater = (fluid >= 80 && fluid <= 95);
        o.vy -= inWater ? 0.005 : 0.03;
        const f64 drag = inWater ? 0.8 : 0.98;
        if (nearest) {
            const f64 dist = std::sqrt(nearestDistSq);
            if (dist < 8.0 && dist > 1e-4) {
                const f64 dx = nearest->getX() - o.x, dy = (nearest->getY() + 1.0) - o.y, dz = nearest->getZ() - o.z;
                const f64 pull = (1.0 - dist / 8.0) * (1.0 - dist / 8.0) * 0.1;
                o.vx += (dx / dist) * pull;
                o.vy += (dy / dist) * pull;
                o.vz += (dz / dist) * pull;
            }
        }
        const f64 ox = o.x, oy = o.y, oz = o.z;
        f64 nx = ox + o.vx, ny = oy + o.vy, nz = oz + o.vz;
        const i32 belowState = world_.getBlock(static_cast<i32>(std::floor(nx)), static_cast<i32>(std::floor(ny - 0.05)), static_cast<i32>(std::floor(nz)));
        bool onGround = false;
        if (o.vy < 0 && belowState > 0 && !(belowState >= 80 && belowState <= 111) && !isFreeForFalling(belowState)) {
            ny = std::min(oy, std::floor(oy) + 0.0625); // rest just above the block below, never move up through it
            o.vy = 0;
            onGround = true;
        }
        o.vx *= drag; o.vz *= drag;
        if (!onGround) o.vy *= drag;
        bool picked = false;
        if (nearest) {
            const f64 dx = nearest->getX() - nx, dy = (nearest->getY() + 1.0) - ny, dz = nearest->getZ() - nz;
            if (dx * dx + dy * dy + dz * dz <= 1.0) { // vanilla pickup radius: 1 block
                grantExperience(nearest, o.amount);
                net::Buffer ti;
                ti.writeVarInt(o.eid); ti.writeVarInt(static_cast<i32>(nearest->getEntityId())); ti.writeVarInt(o.amount);
                const auto tiBytes = std::vector<u8>(ti.writtenSpan().begin(), ti.writtenSpan().end());
                for (auto& p : players) if (p && p->isAlive()) p->getConnection()->sendPacket(0x6F, tiBytes);
                removed.push_back(o.eid);
                picked = true;
            }
        }
        if (picked) continue;
        o.x = nx; o.y = ny; o.z = nz;
        auto velocity = [](f64 value) { return static_cast<i16>(std::clamp(value * 8000.0, -31200.0, 31200.0)); };
        net::Buffer motion;
        motion.writeVarInt(o.eid);
        motion.writeI16(velocity(o.vx)); motion.writeI16(velocity(o.vy)); motion.writeI16(velocity(o.vz));
        const auto motionBytes = std::vector<u8>(motion.writtenSpan().begin(), motion.writtenSpan().end());
        auto relative = [](f64 from, f64 to) {
            const i64 a = static_cast<i64>(std::llround(from * 4096.0));
            const i64 b = static_cast<i64>(std::llround(to * 4096.0));
            return static_cast<i16>(std::clamp<i64>(b - a, -32768, 32767));
        };
        net::Buffer move;
        move.writeVarInt(o.eid);
        move.writeI16(relative(ox, o.x)); move.writeI16(relative(oy, o.y)); move.writeI16(relative(oz, o.z));
        move.writeBool(false);
        const auto moveBytes = std::vector<u8>(move.writtenSpan().begin(), move.writtenSpan().end());
        for (auto& p : players) if (p && p->isAlive()) {
            p->getConnection()->sendPacket(0x5A, motionBytes);
            p->getConnection()->sendPacket(0x2E, moveBytes);
        }
        updated.emplace(o.eid, o);
    }
    for (i32 eid : removed) {
        net::Buffer rm; rm.writeVarInt(1); rm.writeVarInt(eid);
        const auto bytes = std::vector<u8>(rm.writtenSpan().begin(), rm.writtenSpan().end());
        for (auto& p : players) if (p && p->isAlive()) p->getConnection()->sendPacket(0x42, bytes);
    }
    std::lock_guard lk(xpOrbsMutex_);
    std::erase_if(xpOrbs_, [&](const ExperienceOrb& o) {
        return std::find(removed.begin(), removed.end(), o.eid) != removed.end();
    });
    for (auto& o : xpOrbs_) { auto it = updated.find(o.eid); if (it != updated.end()) o = it->second; }
}

// EQUIP_V1: собрать тело пакета Set Equipment (0x5b) для основной руки
static void buildEquipmentPacket(net::Buffer& eq, i32 eid, i32 heldState) {
    eq.writeVarInt(eid);
    eq.writeByte(0x00); // слот 0 = MAINHAND, старший бит снят -> единственная запись
    const i32 itemId = stateToItem(heldState);
    if (itemId > 0) {
        eq.writeVarInt(1);      // Item Count
        eq.writeVarInt(itemId); // Item ID
        eq.writeVarInt(0);      // компонентов добавить
        eq.writeVarInt(0);      // компонентов удалить
    } else {
        eq.writeVarInt(0);      // пустой слот (рука пуста)
    }
}

// PLAYER_VIS_V1: полный набор экипировки (рука, оффхенд, вся броня) из инвентаря игрока
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

// PLAYER_VIS_V1: метаданные сущности игрока (общие флаги + поза) для приседа/спринта
// SKIN_V1: + displayed skin parts (index 17) so capes/layers render on the body.
static void buildEntityMeta(net::Buffer& m, i32 eid, bool sneaking, bool sprinting,
                            u8 skinParts, bool onFire, bool elytraFlying = false) {
    m.writeVarInt(eid);
    u8 flags = 0;
    if (onFire)    flags |= 0x01; // LAVAFIRE_V1: DATA_SHARED_FLAGS.ON_FIRE
    if (sneaking)  flags |= 0x02; // присед
    if (sprinting) flags |= 0x08; // спринт
    if (elytraFlying) flags |= 0x80; // DATA_SHARED_FLAGS.FALL_FLYING
    m.writeByte(0x00);            // индекс 0: общий флаг-байт
    m.writeVarInt(0);             // сериализатор BYTE
    m.writeByte(flags);
    m.writeByte(0x06);            // индекс 6: поза
    m.writeVarInt(21);            // сериализатор POSE
    m.writeVarInt(elytraFlying ? 1 : (sneaking ? 5 : 0)); // FALL_FLYING / CROUCHING / STANDING
    m.writeByte(0x11);            // индекс 17: displayed skin parts (SKIN_V1)
    m.writeVarInt(0);             // сериализатор BYTE
    m.writeByte(skinParts);       // маска слоёв (плащ/куртка/рукава/штанины/шляпа)
    m.writeByte(0xFF);            // конец метаданных
}

static i32 xpNeededForNextLevel(i32 level) {
    if (level >= 30) return 112 + (level - 30) * 9;
    if (level >= 15) return 37 + (level - 15) * 5;
    return 7 + level * 2;
}

static void syncExperienceBar(const std::shared_ptr<entity::Player>& player) {
    if (!player || !player->getConnection()) return;
    net::Buffer xp;
    xp.writeF32(std::clamp(player->experienceProgress, 0.0f, 1.0f));
    xp.writeVarInt(std::max(0, player->experienceLevel));
    xp.writeVarInt(std::max(0, player->totalExperience));
    player->getConnection()->sendPacket(0x5C, std::vector<u8>(xp.writtenSpan().begin(), xp.writtenSpan().end()));
}

static void grantExperience(const std::shared_ptr<entity::Player>& player, i32 amount) {
    if (!player || amount <= 0) return;
    player->totalExperience = std::max(0, player->totalExperience + amount);
    i32 left = amount;
    while (left > 0) {
        const i32 need = std::max(1, xpNeededForNextLevel(player->experienceLevel));
        const i32 room = std::max(1, static_cast<i32>(std::ceil((1.0f - player->experienceProgress) * need - 1e-4f)));
        const i32 take = std::min(left, room);
        player->experienceProgress += static_cast<f32>(take) / static_cast<f32>(need);
        left -= take;
        while (player->experienceProgress >= 0.9999f) {
            player->experienceProgress -= 1.0f;
            ++player->experienceLevel;
        }
    }
    syncExperienceBar(player);
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
    // PROTOCOL767_GUARD_V1: 0x38 is not a valid clientbound Player Abilities
    // packet for the 1.21.1 client. Sending it disconnects the client with
    // "Received unknown packet id 38". Do not emit a guessed packet ID.
    // Game Event already communicates the current gamemode; survival/adventure
    // work with their default abilities. Creative/spectator ability syncing is
    // intentionally deferred until the authoritative 767 packet mapping is added.
    (void)player;
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
    buf.writeVarInt(player->nextTeleportId()); // TPFIX_V2: была константа 1 на все телепорты
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

// ============================================================
// MULTIWORLD_V1: dimensions. 0 = overworld, 1 = nether, 2 = end.
// We send the dimension_type registry as {overworld, overworld_caves, the_end, the_nether},
// so the holder indices are 0, 2 and 3.
// ============================================================
namespace {
i32 dimTypeIndex(i32 dim) { return dim == 1 ? 3 : (dim == 2 ? 2 : 0); }
const char* dimIdName(i32 dim) {
    return dim == 1 ? "minecraft:the_nether" : (dim == 2 ? "minecraft:the_end" : "minecraft:overworld");
}
} // namespace

world::World& NetherCraftServer::worldFor(i32 dim) {
    if (dim == 1) return nether_;
    if (dim == 2) return end_;
    return world_;
}

world::World& NetherCraftServer::worldOf(const std::shared_ptr<entity::Player>& player) {
    return worldFor(player ? player->dimension : 0);
}

void NetherCraftServer::saveWorlds() { // DIMSAVE_V1: у каждого измерения своя папка
    world_.saveToDisk("world/overworld/world.dat");
    if (netherReady_) nether_.saveToDisk("world/nether/nether.dat");
    if (endReady_) end_.saveToDisk("world/end/end.dat");
}

// SPAWNCFG_V1: обратный отсчёт /spawn. Настройки берутся из spawn/spawn.properties
// в момент вызова команды, поэтому правка файла работает без перезапуска сервера.
void NetherCraftServer::tickSpawnWarmups() {
    if (spawnWarmups_.empty()) return;
    const auto all = getAllPlayersCopy();
    for (std::size_t i = 0; i < spawnWarmups_.size();) {
        SpawnWarmup& w = spawnWarmups_[i];
        std::shared_ptr<entity::Player> pl;
        for (const auto& p : all) if (p && p->getName() == w.name) { pl = p; break; }
        if (!pl || !pl->isAlive() || pl->getState() != entity::PlayerState::Play) {
            spawnWarmups_.erase(spawnWarmups_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const auto lang = w.forceLang ? (w.forceRu ? nc::i18n::Lang::Ru : nc::i18n::Lang::En)
                                      : nc::i18n::langFromLocale(pl->clientLocale);
        if (w.standStill) {
            const f64 dx = pl->getX() - w.sx, dy = pl->getY() - w.sy, dz = pl->getZ() - w.sz;
            if (dx * dx + dy * dy + dz * dz > 0.35 * 0.35) {
                pl->sendSystemMessage(w.textCancelled.empty()
                    ? std::string(nc::i18n::tr(lang, "spawn.cancelled"))
                    : w.color + w.textCancelled);
                spawnWarmups_.erase(spawnWarmups_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        }
        if (w.ticksLeft > 0) --w.ticksLeft;
        if (w.ticksLeft <= 0) {
            if (pl->dimension != 0) travelToDimension(pl, 0, g_spawnX + 0.5, (f64)g_spawnY, g_spawnZ + 0.5);
            else teleportSafe(pl, g_spawnX + 0.5, (f64)g_spawnY, g_spawnZ + 0.5, true);
            pl->sendSystemMessage(w.textDone.empty()
                ? std::string(nc::i18n::tr(lang, "spawn.tp"))
                : w.color + w.textDone);
            spawnWarmups_.erase(spawnWarmups_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const i32 secLeft = (w.ticksLeft + 19) / 20;
        if (secLeft != w.lastShown) {
            w.lastShown = secLeft;
            pl->sendSystemMessage(w.textCountdown.empty()
                ? w.color + nc::i18n::f(lang, "spawn.countdown", secLeft)
                : w.color + nc::spawncfg::spawnFillSeconds(w.textCountdown, secLeft));
        }
        ++i;
    }
}

// WORLDPREP_V1: ванильный старт готовит ��егион спавна ДО того, как пустит игроков, и пишет проценты.
// Раньше Ад и Энд генерировались лениво — прямо в тике, в момент первого
// прохода в портал, отчего TPS проваливался до 2.32. Теперь эта работа сделана до старта тиков.
void NetherCraftServer::prepareAllDimensions() {
    const bool ru = (config_.language == "rus");
    struct PrepStage { i32 dim; i32 centerX; i32 centerZ; i32 radius; const char* nameRu; const char* nameEn; };
    static const PrepStage kStages[2] = {
        {1, 0, 0, 3, "Ад", "Nether"},
        {2, 6, 0, 4, "Энд", "End"},
    };

    if (ru) NC_INFO("Server", "Подготовка измерений...");
    else    NC_INFO("Server", "Preparing dimensions...");
    if (ru) NC_INFO("Server", "  Обычный мир: 100% (готов)");
    else    NC_INFO("Server", "  Overworld: 100% (ready)");

    bool generatedAny = false; // DIMLOAD_V1
    for (const auto& st : kStages) {
        const char* name = ru ? st.nameRu : st.nameEn;
        // DIMTOGGLE_V1: измерение выключено в конфиге — не создаём и не грузим.
        if ((st.dim == 1 && !config_.enableNether) || (st.dim == 2 && !config_.enableEnd)) {
            const char* key = (st.dim == 1) ? "nether" : "end";
            if (ru) NC_INFO("Server", "  {}: выключен в settings.properties (enable-{}=false)", name, key);
            else    NC_INFO("Server", "  {}: disabled in settings.properties (enable-{}=false)", name, key);
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const char* datPath = (st.dim == 1) ? "world/nether/nether.dat" : "world/end/end.dat";
        std::error_code dec;
        const bool fromDisk = std::filesystem::exists(datPath, dec); // DIMLOAD_V1
        ensureDimensionReady(st.dim);
        world::World& w = worldFor(st.dim);
        if (fromDisk) {
            // DIMLOAD_V1: измерение уже сохранено. Фоновая загрузка ставит колонны
            // в очередь, а разбирает её drainLoadedChunks() в тике — но тиков ещё нет.
            // Раньше из-за этого старт генерил измерение заново и затирал сохранённое.
            for (int guard = 0; guard < 30000 && !w.isBackgroundLoadDone(); ++guard) {
                w.drainLoadedChunks();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            w.drainLoadedChunks();
            const auto lms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (ru) NC_INFO("Server", "  {}: 100% — загружен с диска за {} мс (генерация не нужна)", name, (long long)lms);
            else    NC_INFO("Server", "  {}: 100% — loaded from disk in {} ms (no generation needed)", name, (long long)lms);
            g_startProfile.lap(st.dim == 1 ? "Nether" : "End"); // STARTPROF_V1
            continue;
        }
        generatedAny = true; // DIMLOAD_V1
        const i32 side = st.radius * 2 + 1;
        const i32 total = side * side;
        i32 done = 0;
        i32 shown = 0;
        for (i32 cx = st.centerX - st.radius; cx <= st.centerX + st.radius; ++cx) {
            for (i32 cz = st.centerZ - st.radius; cz <= st.centerZ + st.radius; ++cz) {
                w.getOrGenerateChunk(cx, cz);
                ++done;
                const i32 pct = done * 100 / total;
                if (pct >= shown + 10) {
                    shown = pct - (pct % 10);
                    NC_INFO("Server", "  {}: {}%", name, shown);
                }
            }
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (ru) NC_INFO("Server", "  {}: 100% — готово, {} чанков за {} мс", name, total, (long long)ms);
        else    NC_INFO("Server", "  {}: 100% — done, {} chunks in {} ms", name, total, (long long)ms);
        g_startProfile.lap(st.dim == 1 ? "Nether" : "End"); // STARTPROF_V1
    }

    if (generatedAny) saveWorlds(); // WORLDPREP_V1 / DIMLOAD_V1: только если что-то действительно создали
    if (ru) NC_INFO("Server", "Все измерения готовы");
    else    NC_INFO("Server", "All dimensions ready");
}

void NetherCraftServer::ensureDimensionReady(i32 dim) {
    // DIMTOGGLE_V1: выключенное измерение не создаём никогда.
    if ((dim == 1 && !config_.enableNether) || (dim == 2 && !config_.enableEnd)) return;
    const bool ru = (config_.language == "rus");
    // DIMGEN_V1: Ад и Энд больше не плоские пресеты — у каждого свой шумовой генератор.
    if (dim == 1 && !netherReady_) {
        nether_.setLanguageRu(ru);
        nether_.initNetherGenerator(config_.levelSeed);
        if (!nether_.startBackgroundLoad("world/nether/nether.dat")) // DIMSAVE_V1
            nether_.generateDimSpawn(0, 0, 2);
        netherReady_ = true;
    } else if (dim == 2 && !endReady_) {
        end_.setLanguageRu(ru);
        end_.initEndGenerator(config_.levelSeed);
        if (!end_.startBackgroundLoad("world/end/end.dat")) // DIMSAVE_V1
            end_.generateDimSpawn(6, 0, 2);
        endReady_ = true;
    }
}


// ============================================================
// PORTAL_V2 / ENDPORTAL_V1: рабочие порталы.
// Рамка Ада проверяется БЕЗ УГЛОВ (как в ванилле): нужны только боковые
// столбы и пол/потолок напротив внутреннего проёма. Внутренний размер 2..21 x 3..21.
// ============================================================
namespace {
struct PortalBlocks {
    i32 obsidian = -1, portalX = -1, portalZ = -1, endPortal = -1;
    i32 frameNoEye[4] = {-1, -1, -1, -1};
    i32 frameEye[4]   = {-1, -1, -1, -1};
};
const PortalBlocks& portalBlocks() {
    static const PortalBlocks pb = [] {
        auto& reg = registries::RegistryManager::instance();
        PortalBlocks p;
        // PORTALFIX_V1: главный баг порталов. Раньше все ID тянулись только через
        // getBlockStateId(name, props). Реестр (registry.cpp / registerStateVariants)
        // не регистрировал property-варианты ни для nether_portal, ни для
        // end_portal_frame, поэтому оба запроса всегда возвращали nullopt ->
        // portalX/portalZ/frame* оставались -1 -> tryLightNetherPortal() выходил
        // на первой же с��роке, а tryPlaceEnderEye() не находил ни одной рамки.
        // Снаружи это выглядело как "огниво ставит огонь, портал не зажигается"
        // и "глаза не вставляются в рамку".
        // Теперь ID берутся от дефолтного состояния блока (оно в реестре есть
        // всегда) со сдвигом по ванильному порядку состояний:
        //   nether_portal    default = axis=x, +1 = axis=z
        //   end_portal_frame default = eye=false,north; +1..+3 = south/west/east;
        //                    четвёрка eye=true идёт ПЕРЕД ними (bool: true, false) => -4.
        auto def = [&](const char* n) { return reg.getBlockStateId(n).value_or(-1); };
        p.obsidian  = def("minecraft:obsidian");
        const i32 npDef = def("minecraft:nether_portal");
        p.portalX   = reg.getBlockStateId("minecraft:nether_portal", {{"axis", "x"}}).value_or(npDef);
        p.portalZ   = reg.getBlockStateId("minecraft:nether_portal", {{"axis", "z"}}).value_or(npDef >= 0 ? npDef + 1 : -1);
        p.endPortal = def("minecraft:end_portal");
        static const char* kFace[4] = {"north", "south", "west", "east"};
        const i32 frDef = def("minecraft:end_portal_frame");
        for (int i = 0; i < 4; ++i) {
            p.frameNoEye[i] = reg.getBlockStateId("minecraft:end_portal_frame", {{"eye", "false"}, {"facing", kFace[i]}})
                                 .value_or(frDef >= 0 ? frDef + i : -1);
            p.frameEye[i]   = reg.getBlockStateId("minecraft:end_portal_frame", {{"eye", "true"},  {"facing", kFace[i]}})
                                 .value_or(frDef >= 4 ? frDef - 4 + i : -1);
        }
        return p;
    }();
    return pb;
}
} // namespace

bool NetherCraftServer::tryLightNetherPortal(i32 dim, i32 bx, i32 by, i32 bz, bool dryRun) {
    const auto& P = portalBlocks();
    if (P.obsidian < 0 || P.portalX < 0 || P.portalZ < 0) return false;
    world::World& w = worldFor(dim);
    auto isFrame = [&](i32 x, i32 y, i32 z) { return w.getBlock(x, y, z) == P.obsidian; };
    auto isInner = [&](i32 x, i32 y, i32 z) {
        const i32 s = w.getBlock(x, y, z);
        // PORTALFIX_V1: cave_air / void_air / soul_fire тоже считаются пустотой.
        return s == 0 || s == 12958 || s == 12959 || s == 2391 || s == 2872 ||
               s == P.portalX || s == P.portalZ; // воздух / огонь / уже портал
    };
    if (!isInner(bx, by, bz)) return false;

    for (int axis = 0; axis < 2; ++axis) {
        const i32 dx = (axis == 0) ? 1 : 0;
        const i32 dz = (axis == 0) ? 0 : 1;

        i32 left = 0;
        while (left < 21 && isInner(bx - dx * (left + 1), by, bz - dz * (left + 1))) ++left;
        if (!isFrame(bx - dx * (left + 1), by, bz - dz * (left + 1))) continue;
        i32 right = 0;
        while (right < 21 && isInner(bx + dx * (right + 1), by, bz + dz * (right + 1))) ++right;
        if (!isFrame(bx + dx * (right + 1), by, bz + dz * (right + 1))) continue;

        const i32 width = left + right + 1;
        if (width < 2 || width > 21) continue;
        const i32 x0 = bx - dx * left, z0 = bz - dz * left; // первая внутренняя колонка

        i32 down = 0;
        while (down < 21 && isInner(bx, by - down - 1, bz)) ++down;
        i32 up = 0;
        while (up < 21 && isInner(bx, by + up + 1, bz)) ++up;
        const i32 yBottom = by - down, yTop = by + up;
        const i32 height = yTop - yBottom + 1;
        if (height < 3 || height > 21) continue;

        bool good = true;
        for (i32 i = 0; i < width && good; ++i) {
            const i32 x = x0 + dx * i, z = z0 + dz * i;
            if (!isFrame(x, yBottom - 1, z) || !isFrame(x, yTop + 1, z)) { good = false; break; }
            for (i32 y = yBottom; y <= yTop; ++y)
                if (!isInner(x, y, z)) { good = false; break; }
        }
        for (i32 y = yBottom; y <= yTop && good; ++y) {
            if (!isFrame(x0 - dx, y, z0 - dz)) good = false;
            if (!isFrame(x0 + dx * width, y, z0 + dz * width)) good = false;
        }
        if (!good) continue; // углы намеренно не проверяются
        // DIMTOGGLE_V3: the caller only wanted to know whether a real frame is here.
        if (dryRun) return true;

        const i32 portalState = (axis == 0) ? P.portalX : P.portalZ;
        for (i32 i = 0; i < width; ++i)
            for (i32 y = yBottom; y <= yTop; ++y) {
                const i32 x = x0 + dx * i, z = z0 + dz * i;
                w.setBlock(x, y, z, portalState);
                broadcastBlockIn(dim, x, y, z, portalState);
            }
        return true;
    }
    return false;
}

bool NetherCraftServer::tryPlaceEnderEye(i32 dim, i32 bx, i32 by, i32 bz) {
    const auto& P = portalBlocks();
    world::World& w = worldFor(dim);
    const i32 s = w.getBlock(bx, by, bz);
    int idx = -1;
    for (int i = 0; i < 4; ++i) if (P.frameNoEye[i] >= 0 && P.frameNoEye[i] == s) idx = i;
    if (idx < 0 || P.frameEye[idx] < 0) return false;
    w.setBlock(bx, by, bz, P.frameEye[idx]);
    broadcastBlockIn(dim, bx, by, bz, P.frameEye[idx]);
    tryCompleteEndPortal(dim, bx, by, bz);
    return true;
}

void NetherCraftServer::tryCompleteEndPortal(i32 dim, i32 bx, i32 by, i32 bz) {
    const auto& P = portalBlocks();
    if (P.endPortal < 0) return;
    world::World& w = worldFor(dim);
    auto hasEye = [&](i32 x, i32 z) {
        const i32 st = w.getBlock(x, by, z);
        for (int i = 0; i < 4; ++i) if (P.frameEye[i] >= 0 && P.frameEye[i] == st) return true;
        return false;
    };
    for (i32 ox = -2; ox <= 2; ++ox) {
        for (i32 oz = -2; oz <= 2; ++oz) {
            const i32 cx = bx + ox, cz = bz + oz;
            bool ring = true;
            for (i32 i = -1; i <= 1 && ring; ++i) {
                if (!hasEye(cx + i, cz - 2) || !hasEye(cx + i, cz + 2) ||
                    !hasEye(cx - 2, cz + i) || !hasEye(cx + 2, cz + i)) ring = false;
            }
            if (!ring) continue;
            for (i32 i = -1; i <= 1; ++i)
                for (i32 j = -1; j <= 1; ++j) {
                    w.setBlock(cx + i, by, cz + j, P.endPortal);
                    broadcastBlockIn(dim, cx + i, by, cz + j, P.endPortal);
                }
            return;
        }
    }
}

bool NetherCraftServer::findOrCreateNetherPortal(i32 dim, i32 cx, i32 cy, i32 cz, f64& outX, f64& outY, f64& outZ) {
    const auto& P = portalBlocks();
    if (P.obsidian < 0 || P.portalX < 0) return false;
    world::World& w = worldFor(dim);
    const i32 yMin = (dim == 1) ? 6 : 2;
    const i32 yMax = (dim == 1) ? 118 : 200;
    cy = std::clamp(cy, yMin, yMax);

    // генерим землю вокруг цели — иначе искать нечего
    const i32 ccx = cx >> 4, ccz = cz >> 4;
    for (i32 ax = ccx - 1; ax <= ccx + 1; ++ax)
        for (i32 az = ccz - 1; az <= ccz + 1; ++az) w.getOrGenerateChunk(ax, az);

    // 1) готовый портал поблизости
    {
        i64 best = -1; i32 fx = 0, fy = 0, fz = 0;
        const i32 y0 = std::max(yMin, cy - 20), y1 = std::min(yMax, cy + 20);
        for (i32 x = cx - 16; x <= cx + 16; ++x)
            for (i32 z = cz - 16; z <= cz + 16; ++z)
                for (i32 y = y0; y <= y1; ++y) {
                    const i32 st = w.getBlock(x, y, z);
                    if (st != P.portalX && st != P.portalZ) continue;
                    const i64 d = (i64)(x - cx) * (x - cx) + (i64)(z - cz) * (z - cz) + (i64)(y - cy) * (y - cy);
                    if (best < 0 || d < best) { best = d; fx = x; fy = y; fz = z; }
                }
        if (best >= 0) { outX = fx + 0.5; outY = (f64)fy; outZ = fz + 0.5; return true; }
    }

    // 2) ищем площадку и строим новый (внутренний проём 2x3)
    auto solidFloor = [&](i32 x, i32 y, i32 z) {
        const i32 s = w.getBlock(x, y, z);
        return s != 0 && !(s >= 80 && s <= 111); // не воздух и не жидкость
    };
    i32 baseY = -1;
    for (i32 y = std::min(yMax, cy + 24); y > yMin; --y) {
        bool clear = true;
        for (i32 h = 0; h < 4 && clear; ++h) if (w.getBlock(cx, y + h, cz) != 0) clear = false;
        if (clear && solidFloor(cx, y - 1, cz)) { baseY = y; break; }
    }
    if (baseY < 0) baseY = std::clamp(cy, yMin + 1, yMax - 6);

    const i32 x0 = cx, z0 = cz;
    for (i32 i = -1; i <= 2; ++i)
        for (i32 j = -1; j <= 1; ++j) {
            w.setBlock(x0 + i, baseY - 1, z0 + j, P.obsidian);           // опора
            broadcastBlockIn(dim, x0 + i, baseY - 1, z0 + j, P.obsidian);
            for (i32 y = baseY; y <= baseY + 4; ++y) {
                w.setBlock(x0 + i, y, z0 + j, 0);                        // расчистка
                broadcastBlockIn(dim, x0 + i, y, z0 + j, 0);
            }
        }
    for (i32 i = -1; i <= 2; ++i) {
        w.setBlock(x0 + i, baseY - 1, z0, P.obsidian);
        w.setBlock(x0 + i, baseY + 3, z0, P.obsidian);
        broadcastBlockIn(dim, x0 + i, baseY + 3, z0, P.obsidian);
    }
    for (i32 y = baseY; y <= baseY + 2; ++y) {
        w.setBlock(x0 - 1, y, z0, P.obsidian); broadcastBlockIn(dim, x0 - 1, y, z0, P.obsidian);
        w.setBlock(x0 + 2, y, z0, P.obsidian); broadcastBlockIn(dim, x0 + 2, y, z0, P.obsidian);
    }
    for (i32 i = 0; i <= 1; ++i)
        for (i32 y = baseY; y <= baseY + 2; ++y) {
            w.setBlock(x0 + i, y, z0, P.portalX);
            broadcastBlockIn(dim, x0 + i, y, z0, P.portalX);
        }
    outX = x0 + 0.5; outY = (f64)baseY; outZ = z0 + 0.5;
    return true;
}

void NetherCraftServer::buildEndSpawnPlatform() { // ванильная обсидиановая платформа 5x5 на (100, 49, 0)
    const auto& P = portalBlocks();
    if (P.obsidian < 0) return;
    world::World& w = worldFor(2);
    for (i32 acx = (98 >> 4) - 1; acx <= (102 >> 4) + 1; ++acx)
        for (i32 acz = -1; acz <= 1; ++acz) w.getOrGenerateChunk(acx, acz);
    for (i32 x = 98; x <= 102; ++x)
        for (i32 z = -2; z <= 2; ++z) {
            w.setBlock(x, 48, z, P.obsidian);
            for (i32 y = 49; y <= 52; ++y) w.setBlock(x, y, z, 0);
        }
}

// Переход между измерениями: 80 тиков в выживании, мгновенно в творчестве,
// масштаб 1:8, ��ул��аун 300 тиков (PORTAL_V1 поля игрока уже были заведены).
// PORTALBREAK_V1: в ванильной игре портал гаснет целиком, если сломать любой его блок
// или блок рамки. Раньше сносился только один блок, остальные висели в воздухе.
void NetherCraftServer::extinguishPortalNear(i32 dim, i32 bx, i32 by, i32 bz, i32 brokenState) {
    const auto& P = portalBlocks();
    if (P.portalX < 0 || P.portalZ < 0) return;
    const bool wasPortal = (brokenState == P.portalX || brokenState == P.portalZ);
    const bool wasFrame  = (P.obsidian >= 0 && brokenState == P.obsidian);
    if (!wasPortal && !wasFrame) return;
    world::World& w = worldFor(dim);
    struct PBPos { i32 x, y, z; };
    std::vector<PBPos> stack;
    auto push = [&](i32 x, i32 y, i32 z) {
        if (y < world::CHUNK_HEIGHT_MIN || y >= world::CHUNK_HEIGHT_MAX) return;
        const i32 s = w.getBlock(x, y, z);
        if (s == P.portalX || s == P.portalZ) stack.push_back(PBPos{x, y, z});
    };
    push(bx + 1, by, bz); push(bx - 1, by, bz);
    push(bx, by + 1, bz); push(bx, by - 1, bz);
    push(bx, by, bz + 1); push(bx, by, bz - 1);
    i32 removed = 0;
    while (!stack.empty() && removed < 512) {
        const PBPos c = stack.back();
        stack.pop_back();
        const i32 s = w.getBlock(c.x, c.y, c.z);
        if (s != P.portalX && s != P.portalZ) continue;
        broadcastBlockIn(dim, c.x, c.y, c.z, 0);
        ++removed;
        push(c.x + 1, c.y, c.z); push(c.x - 1, c.y, c.z);
        push(c.x, c.y + 1, c.z); push(c.x, c.y - 1, c.z);
        push(c.x, c.y, c.z + 1); push(c.x, c.y, c.z - 1);
    }
}

void NetherCraftServer::tickPortals() {
    const auto& P = portalBlocks();
    for (auto& p : getAllPlayersCopy()) {
        if (!p || p->getState() != entity::PlayerState::Play || !p->isAlive()) continue;
        // DIMTOGGLE_V1: измерение выключили, а игрок сохранён в нём — возвращаем домой.
        if ((p->dimension == 1 && !config_.enableNether) || (p->dimension == 2 && !config_.enableEnd)) {
            if (travelToDimension(p, 0, g_spawnX + 0.5, (f64)g_spawnY, g_spawnZ + 0.5)) {
                p->setPosition(g_spawnX + 0.5, (f64)g_spawnY, g_spawnZ + 0.5);
                p->fallPeakY = (f64)g_spawnY; p->fallWasOnGround = true; p->setOnGround(true);
                sendPlayerPositionAndLook(p);
                const bool rl = (p->clientLocale.rfind("ru", 0) == 0);
                p->sendSystemMessage(rl ? "§eЭто измерение выключено на сервере — вы возвращены в обычный мир"
                                        : "§eThat dimension is disabled on this server - you were moved back to the overworld");
            }
            continue;
        }
        if (p->portalCooldownTicks > 0) { --p->portalCooldownTicks; p->portalTimeTicks = 0; continue; }
        world::World& w = worldOf(p);
        const i32 bx = static_cast<i32>(std::floor(p->getX()));
        const i32 by = static_cast<i32>(std::floor(p->getY() + 0.2));
        const i32 bz = static_cast<i32>(std::floor(p->getZ()));
        const i32 st = w.getBlock(bx, by, bz);
        const bool inNether = (P.portalX >= 0 && (st == P.portalX || st == P.portalZ));
        const bool inEnd    = (P.endPortal >= 0 && st == P.endPortal);
        if (!inNether && !inEnd) { p->portalTimeTicks = 0; continue; }

        const i32 need = inEnd ? 1 : (p->gameMode == 1 ? 1 : 80);
        if (++p->portalTimeTicks < need) continue;
        p->portalTimeTicks = 0;
        p->portalCooldownTicks = 300;

        if (inEnd) {
            if (p->dimension == 2) {
                travelToDimension(p, 0, g_spawnX + 0.5, (f64)g_spawnY, g_spawnZ + 0.5);
            } else {
                ensureDimensionReady(2);
                buildEndSpawnPlatform();
                if (travelToDimension(p, 2, 100.5, 49.0, 0.5)) {
                    p->setPosition(100.5, 49.0, 0.5);
                    p->fallPeakY = 49.0; p->fallWasOnGround = true; p->setOnGround(true);
                    sendPlayerPositionAndLook(p);
                }
            }
            p->portalCooldownTicks = 300;
            continue;
        }

        const i32 dst = (p->dimension == 1) ? 0 : 1;
        if (dst == 1 && !config_.enableNether) continue; // DIMTOGGLE_V1: портал в выключенный Ад мёртв
        ensureDimensionReady(dst);
        const f64 scale = (dst == 1) ? 0.125 : 8.0;
        const i32 tx = static_cast<i32>(std::floor(p->getX() * scale));
        const i32 tz = static_cast<i32>(std::floor(p->getZ() * scale));
        const i32 ty = static_cast<i32>(std::floor(p->getY()));
        f64 ox = 0.0, oy = 0.0, oz = 0.0;
        if (!findOrCreateNetherPortal(dst, tx, ty, tz, ox, oy, oz)) continue;
        if (travelToDimension(p, dst, ox, oy, oz)) {
            p->setPosition(ox, oy, oz);
            p->fallPeakY = oy; p->fallWasOnGround = true; p->setOnGround(true);
            sendPlayerPositionAndLook(p);
        }
        p->portalCooldownTicks = 300;
    }
}

bool NetherCraftServer::travelToDimension(const std::shared_ptr<entity::Player>& player, i32 dim, f64 tx, f64 ty, f64 tz) {
    // DIMTOGGLE_V1: в выключенный мир не пускаем никого и ничем.
    if ((dim == 1 && !config_.enableNether) || (dim == 2 && !config_.enableEnd)) return false;
    if (!player || player->getState() != entity::PlayerState::Play) return false;
    if (dim < 0 || dim > 2 || player->dimension == dim) return false;
    if (player->ridingVehicleEid != 0) vehicleDismount(player);
    ensureDimensionReady(dim);
    player->dimension = dim;
    player->clearSeenChunks(); // the client throws away its chunks on Respawn
    { // Respawn (0x47): same player, new dimension, keep attributes + metadata
        net::Buffer rs;
        rs.writeVarInt(dimTypeIndex(dim));
        rs.writeString(dimIdName(dim));
        rs.writeI64(config_.levelSeed);
        rs.writeByte((i8)player->gameMode);
        rs.writeByte(-1);
        rs.writeBool(false); // is debug
        rs.writeBool(dim == 0); // is flat (DIMGEN_V1: nether/end are generated now)
        rs.writeBool(false); // has death location
        rs.writeVarInt(0);   // portal cooldown
        rs.writeByte(0x03);  // data kept: attributes + metadata
        player->getConnection()->sendPacket(0x47, std::vector<u8>(rs.writtenSpan().begin(), rs.writtenSpan().end()));
    }
    { // Game Event 13 "start waiting for level chunks", otherwise the client hangs on Loading terrain
        net::Buffer ge;
        ge.writeByte(13);
        ge.writeF32(0.0f);
        player->getConnection()->sendPacket(0x22, std::vector<u8>(ge.writtenSpan().begin(), ge.writtenSpan().end()));
    }
    const i32 ccx = static_cast<i32>(std::floor(tx / 16.0));
    const i32 ccz = static_cast<i32>(std::floor(tz / 16.0));
    world::World& dw = worldFor(dim);
    dw.getOrGenerateChunk(ccx, ccz); // VOIDFIX_V1: земля должна существовать ДО прибытия игрока
    { // VOIDFIX_V2: садимся на РЕАЛЬНУЮ поверхность, а не на прикинутую высоту
        const i32 sbx = static_cast<i32>(std::floor(tx));
        const i32 sbz = static_cast<i32>(std::floor(tz));
        bool landed = false;
        for (i32 yy = 250; yy >= world::CHUNK_HEIGHT_MIN; --yy) {
            const i32 st = dw.getBlock(sbx, yy, sbz);
            if (st != 0 && st != 12958 && st != 12959) { ty = static_cast<f64>(yy + 1); landed = true; break; }
        }
        if (!landed && dim != 0) ty = 4.0; // VOIDFIX_V3: плоский Ад/Энд — пол Y=0..3, стоим на 4
    }
    player->setPosition(tx, ty, tz);
    sendCenterChunk(player, ccx, ccz); // TPFIX_V2: та не дыра была и на переходе между измерениями
    sendPlayerPositionAndLook(player);
    sendChunksAround(player, ccx, ccz, config_.viewDistance, 24);
    // INVDIM_V1: Ад/Энд шли через Respawn (0x47) без досылки инвентаря — вещи пропадали
    sendFullPlayerInventory(player);
    pruneAllWorlds(); // MEM_V2
    // VOIDFIX_V1: без сброса падения игрок прилетает с накопленным fallPeakY и ловит урон
    player->fallPeakY = ty;
    player->fallWasOnGround = true;
    player->setOnGround(true);
    return true;
}

// INVDIM_V1: после Respawn (0x47) клиент стирает свой контейнер инвентаря. Сервер обязан
// дослать Set Container Content (0x13) + выбранный слот (0x53), иначе вещи визуально пропадают.
void NetherCraftServer::sendFullPlayerInventory(const std::shared_ptr<entity::Player>& player) {
    if (!player || !player->getConnection()) return;
    net::Buffer inv;
    inv.writeByte(0);            // containerId = 0 (окно инвентаря игрока)
    inv.writeVarInt(1);          // state id
    inv.writeVarInt(entity::Player::INV_SIZE);
    for (int i = 0; i < entity::Player::INV_SIZE; ++i)
        writeInventoryStack(inv, player->invItemId[i], player->invCount[i], player->builderWandOwned && player->invItemId[i] == 821);
    inv.writeVarInt(0);          // курсор: пусто
    player->getConnection()->sendPacket(0x13, std::vector<u8>(inv.writtenSpan().begin(), inv.writtenSpan().end()));
    net::Buffer carried;
    carried.writeByte((i8)(player->heldSlot >= 0 && player->heldSlot < 9 ? player->heldSlot : 0));
    player->getConnection()->sendPacket(0x53, std::vector<u8>(carried.writtenSpan().begin(), carried.writtenSpan().end()));
}

// MEM_V2: раньше чистился ТОЛЬКО оверворлд (world_), да и то лишь при пересечении
// границы чанка. Ад и Энд после первого же визита висели в ОЗУ целиком и навсегда.
void NetherCraftServer::pruneAllWorlds() {
    i32 r = config_.viewDistance;
    if (r < 2) r = 2;
    // MEM_V4: было r+3 (лишние кольца в ОЗУ), потом r+1.
    // CPU_V1: r+1 давал thrash — краевые чанки выгружались и тут же генерировались заново
    // при каждом шаге назад-вперёд (шум считался дважды). r+2 — баланс.
    const i32 keep = r + 2;
    std::vector<std::pair<i32, i32>> centers[3];
    for (auto& p : getAllPlayersCopy()) {
        if (!p) continue;
        const i32 d = (p->dimension >= 0 && p->dimension <= 2) ? p->dimension : 0;
        centers[d].emplace_back(p->getViewCenterX(), p->getViewCenterZ());
    }
    for (i32 d = 0; d < 3; ++d) {
        if (d == 1 && !netherReady_) continue;
        if (d == 2 && !endReady_) continue;
        auto& c = centers[d];
        // Измерение без игроков сжимаем до спавна; грязные чанки pruneChunks не трогает.
        if (c.empty()) { std::vector<std::pair<i32, i32>> only{{0, 0}}; worldFor(d).pruneChunks(only, 2); }
        else worldFor(d).pruneChunks(c, keep);
    }
}

// TPFIX_V2: Set Center Chunk (0x54). Клиент хранит чанки в квадрате вокруг своего центра
// и МОЛЧА выбрасывает всё, что пришло за его пределы. Любой телепорт дальше view
// distance обязан сначала двинуть центр, и только потом слать чанки.
void NetherCraftServer::sendCenterChunk(const std::shared_ptr<entity::Player>& player, i32 cx, i32 cz) {
    if (!player || !player->getConnection()) return;
    net::Buffer vb;
    vb.writeVarInt(cx);
    vb.writeVarInt(cz);
    player->getConnection()->sendPacket(0x54, std::vector<u8>(vb.writtenSpan().begin(), vb.writtenSpan().end()));
    player->setViewCenter(cx, cz);
}

// TPFIX_V1: /tp and /spawn used to only move coords: no chunk gen, no chunk
// resend, no fall reset. Far teleports dropped the player through ungenerated
// ground. Same guard as travelToDimension, but inside one dimension.
bool NetherCraftServer::teleportSafe(const std::shared_ptr<entity::Player>& player, f64 tx, f64 ty, f64 tz, bool snapToSurface) {
    if (!player) return false;
    const i32 dim = player->dimension;
    world::World& w = worldFor(dim);
    const i32 ccx = static_cast<i32>(std::floor(tx / 16.0));
    const i32 ccz = static_cast<i32>(std::floor(tz / 16.0));
    w.getOrGenerateChunk(ccx, ccz);
    const i32 sbx = static_cast<i32>(std::floor(tx));
    const i32 sbz = static_cast<i32>(std::floor(tz));
    auto isSolid = [](i32 st) { return st != 0 && st != 12958 && st != 12959; };
    if (snapToSurface) {
        bool landed = false;
        for (i32 yy = 250; yy >= world::CHUNK_HEIGHT_MIN; --yy) {
            if (isSolid(w.getBlock(sbx, yy, sbz))) { ty = static_cast<f64>(yy + 1); landed = true; break; }
        }
        if (!landed) ty = (dim != 0) ? 4.0 : ty;
    } else {
        // TPFIX_V1: explicit Y is respected, but never below the floor of a
        // freshly generated column, otherwise the player keeps falling.
        i32 floorY = world::CHUNK_HEIGHT_MIN;
        bool foundFloor = false;
        for (i32 yy = static_cast<i32>(std::floor(ty)); yy >= world::CHUNK_HEIGHT_MIN; --yy) {
            if (isSolid(w.getBlock(sbx, yy, sbz))) { floorY = yy + 1; foundFloor = true; break; }
        }
        // TPFIX_V2: раньше при ненайденном поле floorY оставался -64, кламп не срабатывал
        // и игрок улетал в пустоту. Нет пола под точкой — ищем сверху.
        if (!foundFloor) {
            for (i32 yy = 250; yy >= world::CHUNK_HEIGHT_MIN; --yy) {
                if (isSolid(w.getBlock(sbx, yy, sbz))) { floorY = yy + 1; foundFloor = true; break; }
            }
            if (!foundFloor) floorY = (dim != 0) ? 4 : 64; // плоский Ад/Энд — пол Y=0..3
            ty = static_cast<f64>(floorY);
        }
        if (ty < static_cast<f64>(floorY)) ty = static_cast<f64>(floorY);
    }
    player->setPosition(tx, ty, tz);
    // TPFIX_V2: ГЛАВНОЕ. Без Set Center Chunk (0x54) клиент выбрасывает все чанки,
    // ��ришедшие дальше view distance от его старого центра: после дальнего /tp была
    // пустота без коллизий. А sendChunksAround в конце сам ставит setViewCenter, поэтому
    // streamChunks делал early-return и 0x54 не уходил уже никогда.
    player->clearSeenChunks();
    sendCenterChunk(player, ccx, ccz);
    sendPlayerPositionAndLook(player); // ровно один раз: два телепорта подряд давали резиновую ленту
    sendChunksAround(player, ccx, ccz, config_.viewDistance, 24);
    sendFullPlayerInventory(player); // INVDIM_V1: страховка от пропажи вещей после дальнего телепорта
    pruneAllWorlds();                // MEM_V2: старая округа больше не висит в ОЗУ до пер��сечения чанка
    player->fallPeakY = ty;
    player->fallWasOnGround = true;
    player->setOnGround(true);
    return true;
}

void NetherCraftServer::sendChunksAround(std::shared_ptr<entity::Player> player, i32 centerX, i32 centerZ, i32 radius, i32 maxChunks) { // CLIENT_BATCH_V1
    world::World& world_ = worldOf(player); g_dimCtx = player ? player->dimension : 0; // DIMPHYS_V1 // MULTIWORLD_V1: stream the player's own dimension
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
                // LIGHT_V1 + FLATNATIVE_V1: флэт-трава на Y=3 -> heightmap 3+1-(-64) = 68 (совпадает с DEFAULT)
                const u64 hmVal = 68u;
                std::vector<i64> hm(37, 0);
                {
                    u64 packedHm = 0;
                    for (int hi = 0; hi < 7; ++hi) packedHm |= static_cast<u64>(hmVal) << (9 * hi);
                    for (auto& hl : hm) hl = static_cast<i64>(packedHm);
                }
                heightmaps.writeLongArray(hm, "MOTION_BLOCKING");
                heightmaps.writeLongArray(hm, "WORLD_SURFACE");
                heightmaps.writeLongArray(hm, "OCEAN_FLOOR");               // HEIGHTMAP_V1
                heightmaps.writeLongArray(hm, "MOTION_BLOCKING_NO_LEAVES"); // HEIGHTMAP_V1
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

            // NETHERLIGHT_V1: в Аду/Энде has_skylight=false — клиент выкидывает skylight
            // и рисует чёрную тьму (Client Light: 0). Туда шлём block light 15.
            {
                const bool noSky = (player->dimension != 0);
                static const std::vector<u8> fullLight(2048, 0xFF); // 4096 значений по 15
                if (noSky) {
                    packetPayload.writeVarInt(0);      // sky light mask: пусто
                    packetPayload.writeVarInt(1);      // block light mask: 1 long
                    packetPayload.writeI64(0x3FFFFFF);
                    packetPayload.writeVarInt(1);      // empty sky light mask: всё
                    packetPayload.writeI64(0x3FFFFFF);
                    packetPayload.writeVarInt(0);      // empty block light mask: пусто
                    packetPayload.writeVarInt(0);      // sky light arrays: пусто
                    packetPayload.writeVarInt(26);     // 26 массивов block light
                    for (int li = 0; li < 26; ++li) {
                        packetPayload.writeVarInt(2048);
                        packetPayload.writeBytes(std::span<const u8>(fullLight));
                    }
                } else {
                    packetPayload.writeVarInt(1);      // sky light mask: 1 long
                    packetPayload.writeI64(0x3FFFFFF); // биты 0..25 = все секции
                    packetPayload.writeVarInt(0);      // block light mask: пусто
                    packetPayload.writeVarInt(0);      // empty sky light mask: пусто
                    packetPayload.writeVarInt(1);      // empty block light mask: 1 long
                    packetPayload.writeI64(0x3FFFFFF);
                    packetPayload.writeVarInt(26);     // 26 массивов skylight
                    for (int li = 0; li < 26; ++li) {
                        packetPayload.writeVarInt(2048);
                        packetPayload.writeBytes(std::span<const u8>(fullLight));
                    }
                    packetPayload.writeVarInt(0);      // block light arrays: пусто
                }
            }

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
    // KEEPALIVE_TIMEOUT_V1: храним ожидание ответа на самом игроке (без утекающего общего вектора)
    player->pendingKeepAliveId = id;
    player->awaitingKeepAlive = true;
    player->keepAliveSentAtMs = id; // id уже есть метка в мс с того же часового источника
}

// ============================================================
// Play — Обработка пакетов от клиента
// ============================================================

// ============================================================
// MP_V1: Мультиплеер — видимость и синхронизация игроков между собой
// ============================================================

// Ванильный entity-type id игрока в протоколе 1.21.1 (поле type в Spawn Entity 0x01).
// ВНИМАНИЕ: если чужой игрок отрисуется как ДРУГОЙ моб — поправь это число
// (значение protocol_id для minecraft:player из реестра entity_type 1.21.1).
static constexpr i32 MP_PLAYER_ENTITY_TYPE = 128;


// CHUNKVIS_V1: расстояние между игроками в ЧАНКАХ (метрика Чебышёва — как у view distance)
static i32 mpChunkDistance(const entity::Player& a, const entity::Player& b) {
    const i32 acx = static_cast<i32>(std::floor(a.getX() / 16.0));
    const i32 acz = static_cast<i32>(std::floor(a.getZ() / 16.0));
    const i32 bcx = static_cast<i32>(std::floor(b.getX() / 16.0));
    const i32 bcz = static_cast<i32>(std::floor(b.getZ() / 16.0));
    return std::max(std::abs(acx - bcx), std::abs(acz - bcz));
}

// CHUNKVIS_V1: радиус видимости сущностей в чанках — привязан к view-distance из конфига
// (раньше был жёсткий лимит 128 БЛОКОВ = 8 чанков независимо от настроек)
static i32 entityViewRadiusChunks(i32 viewDistance) {
    return viewDistance < 2 ? 2 : viewDistance;
}

void NetherCraftServer::broadcastToOthers(const std::shared_ptr<entity::Player>& except, i32 packetId, const std::vector<u8>& payload, bool droppable) {
    auto all = getAllPlayersCopy();
    // ENTITY_CULL_V1: droppable-пакеты (движение/мета/анимация) шлём только игрокам в радиусе
    // видимости — раньше шли ВСЕМ (O(n^2) шторм → переполнение write-очереди → close → WinError 10054).
    // спавн/despawn/remove (droppable=false) по-прежнему доходят до всех, чтобы не было призраков.
    // CHUNKVIS_V1: лимит теперь в ЧАНКАХ по view-distance (при 16 чанках = 256 блоков)
    const i32 vr = entityViewRadiusChunks(config_.viewDistance);
    const u64 exEid = except->getEntityId();
    for (auto& p : all) {
        if (p.get() == except.get()) continue;
        if (p->getState() != entity::PlayerState::Play || !p->playReady) continue; // JOINSAFE_V1: и тем, кто ещё не получил мир
        if (!p->isAlive()) continue;
        if (droppable) {
            if (mpChunkDistance(*p, *except) > vr) continue; // CHUNKVIS_V1: вне радиуса (в чанках) — не шлём движение/мету
            if (!p->hasVisibleEntity(exEid)) continue; // CHUNKVIS_V1: сущность не заспавнена у зрителя
        }
        p->getConnection()->sendPacket(packetId, payload, droppable);
    }
}

// PLAYER_VIS_V2: спектатора (GM3) видят только другие спектаторы; обычным игрокам его сущность не спавним (как в ванилле)
static bool isSpectatorHiddenFrom(const entity::Player& target, const entity::Player& viewer) {
    return target.gameMode == 3 && viewer.gameMode != 3;
}

// CHUNKVIS_V1: таб-лист отдельно от спавна сущности — в тебе видны ВСЕ, а сущность спавнится только в радиусе
static void mpSendPlayerInfo(const std::shared_ptr<entity::Player>& viewer, const std::shared_ptr<entity::Player>& target) {
    net::Buffer info;
    info.writeVarInt(0x1D); // MP_DUP_V1 fix: ADD_PLAYER | UPDATE_GAME_MODE | UPDATE_LISTED | UPDATE_LATENCY
    info.writeVarInt(1);    // count
    info.writeUUID(target->getUuid());
    info.writeString(target->getName());
    writeProfileProperties(info, *target); // SKIN_V1: skin + cape (online & offline)
    info.writeVarInt(target->gameMode);
    info.writeVarInt(1);    // listed = true
    info.writeVarInt(0);    // latency
    viewer->getConnection()->sendPacket(0x3E, std::vector<u8>(info.writtenSpan().begin(), info.writtenSpan().end()));
}

void NetherCraftServer::spawnPlayerFor(const std::shared_ptr<entity::Player>& viewer, const std::shared_ptr<entity::Player>& target) {
    if (!viewer || !target || viewer.get() == target.get()) return;
    if (!viewer->isAlive() || !viewer->playReady || viewer->getState() != entity::PlayerState::Play) return; // JOINSAFE_V1

    const i32 eid = static_cast<i32>(target->getEntityId());
    const Angle yaw = Angle::fromDegrees(target->get_yaw());
    const Angle pitch = Angle::fromDegrees(target->get_pitch());

    // CHUNKVIS_V1: сущность уже заспавнена у этого зрителя — не дублируем пакеты
    if (viewer->hasVisibleEntity(target->getEntityId())) return;

    // CHUNKVIS_V1: вне радиуса (в чанках) или спектатор — сущность НЕ спавним, но в таб-лист
    // добавляем. Появится автоматом при сближении (пересчёт в broadcastPlayerMovement).
    if (mpChunkDistance(*viewer, *target) > entityViewRadiusChunks(config_.viewDistance)
        || isSpectatorHiddenFrom(*target, *viewer)) {
        mpSendPlayerInfo(viewer, target);
        return;
    }
    viewer->addVisibleEntity(target->getEntityId());

    // 1) Player Info Update (0x3E) — tab list (иначе сущность не отрисуется и будет без ника)
    mpSendPlayerInfo(viewer, target);

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

    // EQUIP_V1: показать зрителю предмет в руке target
    sendPlayerEquipment(viewer, target);

    // PLAYER_VIS_V1: синхронизировать позу/присед/спринт target для зрителя
    {
        net::Buffer m;
        buildEntityMeta(m, static_cast<i32>(target->getEntityId()), target->sneaking, target->sprinting,
            target->displayedSkinParts, target->remainingFireTicks > 0, target->elytraFlying);
        viewer->getConnection()->sendPacket(0x58, std::vector<u8>(m.writtenSpan().begin(), m.writtenSpan().end()));
    }

    // SHIELD_V2: новый зритель должен видеть поднятый щит target
    if (target->usingShield) {
        net::Buffer hsm;
        hsm.writeVarInt(static_cast<i32>(target->getEntityId()));
        hsm.writeByte(8);   // index 8: LivingEntity hand states
        hsm.writeVarInt(0); // сериализатор 0: byte
        hsm.writeByte(static_cast<u8>(0x01 | (target->usingShieldHand == 1 ? 0x02 : 0x00)));
        hsm.writeByte(0xFF);
        viewer->getConnection()->sendPacket(0x58, std::vector<u8>(hsm.writtenSpan().begin(), hsm.writtenSpan().end()));
    }
}

// PLAYER_VIS_V2: убрать сущность target у конкретного зрителя (в таб-листе игрок остаётся)
void NetherCraftServer::despawnPlayerFor(const std::shared_ptr<entity::Player>& viewer, const std::shared_ptr<entity::Player>& target) {
    if (!viewer || !target || viewer.get() == target.get()) return;
    if (!viewer->isAlive() || !viewer->playReady || viewer->getState() != entity::PlayerState::Play) return; // JOINSAFE_V1
    if (!viewer->removeVisibleEntity(target->getEntityId())) return; // CHUNKVIS_V1: и так не был виден — не шлём Remove
    net::Buffer b;
    b.writeVarInt(1);
    b.writeVarInt(static_cast<i32>(target->getEntityId()));
    viewer->getConnection()->sendPacket(0x42, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()));
}

// PLAYER_VIS_V2: при смене спектаторства пересчитать видимость сущности в обе стороны
// CONSOLE_V3: единая смена режима игры — используется командой /gamemode (и /gm0..3) и консолью
void NetherCraftServer::applyGameMode(const std::shared_ptr<entity::Player>& target, i32 mode) {
    if (!target || mode < 0 || mode > 3) return;
    const i32 prevMode = target->gameMode; // PLAYER_VIS_V2
    target->gameMode = mode; // GM_V1
    net::Buffer ge;
    ge.writeByte(3); // событие 3 — смена игрового режима
    ge.writeF32(static_cast<f32>(mode));
    target->getConnection()->sendPacket(0x22, std::vector<u8>(ge.writtenSpan().begin(), ge.writtenSpan().end()));
    // PROTOCOL767_GUARD_V1: never send the invalid 0x38 ability packet.
    sendPlayerAbilities(target);
    { // GM3_FIX: обновляем режим в таб-листе (PlayerInfo UPDATE_GAME_MODE)
        net::Buffer gi;
        gi.writeByte(0x04); // EnumSet из 6 действий -> 1 байт; бит 2 = UPDATE_GAME_MODE
        gi.writeVarInt(1);  // 1 запись
        gi.writeUUID(target->getUuid());
        gi.writeVarInt(mode); // новый режим
        auto giv = std::vector<u8>(gi.writtenSpan().begin(), gi.writtenSpan().end());
        for (auto& pp : getAllPlayersCopy()) if (pp && pp->isAlive()) pp->getConnection()->sendPacket(0x3E, giv);
    }
    refreshSpectatorVisibility(target, prevMode == 3, mode == 3); // PLAYER_VIS_V2
    { // PERMLEVEL_V2: пересылаем уровень прав — клиент мог проигнорировать событие на входе,
      // из-за чего F3+F4 ругался «недостаточно полномочий»
        net::Buffer ev;
        ev.writeI32(static_cast<i32>(target->getEntityId()));
        ev.writeByte(static_cast<u8>(24 + nc::opLevelOf(config_.ops, target->getName())));
        target->getConnection()->sendPacket(0x1F, std::vector<u8>(ev.writtenSpan().begin(), ev.writtenSpan().end()));
    }
}

void NetherCraftServer::refreshSpectatorVisibility(const std::shared_ptr<entity::Player>& player, bool wasSpectator, bool isSpectator) {
    if (wasSpectator == isSpectator) return; // спектаторство не менялось — ничего не делаем
    auto all = getAllPlayersCopy();
    for (auto& other : all) {
        if (!other || other.get() == player.get()) continue;
        if (!other->isAlive() || other->getState() != entity::PlayerState::Play) continue;
        if (other->gameMode != 3) {
            // player как ЦЕЛЬ для обычного зрителя other
            if (isSpectator) despawnPlayerFor(other, player); // стал спектатором -> скрыть от выживания
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
    if (!viewer->isAlive() || !viewer->playReady || viewer->getState() != entity::PlayerState::Play) return; // JOINSAFE_V1
    net::Buffer eq;
    buildFullEquipmentPacket(eq, static_cast<i32>(target->getEntityId()), *target);
    viewer->getConnection()->sendPacket(0x5b, std::vector<u8>(eq.writtenSpan().begin(), eq.writtenSpan().end()));
}

// EQUIP_V1: разослать остальным изменившийся предмет в руке игрока
void NetherCraftServer::broadcastHeldEquipment(const std::shared_ptr<entity::Player>& player) {
    if (!player) return;
    net::Buffer eq;
    buildFullEquipmentPacket(eq, static_cast<i32>(player->getEntityId()), *player);
    broadcastToOthers(player, 0x5b, std::vector<u8>(eq.writtenSpan().begin(), eq.writtenSpan().end()), true);
}

// PLAYER_VIS_V1: разослать метаданные (присед/спринт/поза) остальным игрокам
void NetherCraftServer::broadcastEntityMeta(const std::shared_ptr<entity::Player>& player) {
    if (!player) return;
    net::Buffer m;
    buildEntityMeta(m, static_cast<i32>(player->getEntityId()), player->sneaking, player->sprinting,
        player->displayedSkinParts, player->remainingFireTicks > 0, player->elytraFlying);
    // ELYTRA_FIX_V3: the owner must receive the same state transition too.
    // Without the clearing update after landing/removing the elytra, a client keeps
    // Pose.FALL_FLYING but loses shared flag 7 and deliberately renders crawling.
    const auto bytes = std::vector<u8>(m.writtenSpan().begin(), m.writtenSpan().end());
    for (auto& viewer : getAllPlayersCopy())
        if (viewer && viewer->isAlive() && viewer->playReady && viewer->getState() == entity::PlayerState::Play && viewer->getConnection()) // JOINSAFE_V1
            viewer->getConnection()->sendPacket(0x58, bytes);
}

void NetherCraftServer::onPlayerEnterPlay(const std::shared_ptr<entity::Player>& player) {
    if (!player) return;
    player->tabHeaderFooterSent = false;
    // Синхронизировать точку отсчёта delta-move с текущей позицией
    player->mpLastX = player->getX();
    player->mpLastY = player->getY();
    player->mpLastZ = player->getZ();
    // CHUNKVIS_V2: запомнить стартовый чанк для пересчёта видимости
    player->mpVisChunkX = static_cast<i32>(std::floor(player->getX() / 16.0));
    player->mpVisChunkZ = static_cast<i32>(std::floor(player->getZ() / 16.0));

    auto all = getAllPlayersCopy();
    for (auto& other : all) {
        if (other.get() == player.get()) continue;
        if (other->getState() != entity::PlayerState::Play) continue;
        if (!other->isAlive()) continue;
        spawnPlayerFor(player, other); // п��казать уже находящегося в игре — новичку
        spawnPlayerFor(other, player); // показать новичка — тому, кто уже в игре
    }
    // SELFSKIN_V1: отправить игроку метаданные ЕГО СОБСТВЕННОЙ сущности (index 17).
    // Клиент рендерит свою модель в F5 по synched-metadata, а не по локальным опциям,
    // поэтому без этого пакета плащ/шапка/слои на себе не видны (в одиночке их синкает встроенный сервер).
    {
        net::Buffer sm;
        buildEntityMeta(sm, static_cast<i32>(player->getEntityId()), player->sneaking, player->sprinting,
            player->displayedSkinParts, player->remainingFireTicks > 0, player->elytraFlying);
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
    sendMobsTo(player); // MOBS_V1: показать уже живущих мобов
    // ITEMDROP_V1: показать новичку предметы, уже лежащие в мире
    {
        std::vector<ItemDrop> copy;
        { std::lock_guard lk(itemDropsMutex_); copy = itemDrops_; }
        for (auto& d : copy) sendItemDropSpawnTo(player, d.eid, d.itemId, d.count, d.x, d.y, d.z, 0.0, 0.0, 0.0);
    }
    // FALLING_V1: поздно вошедший клиент должен увидеть уже падающие блоки.
    {
        std::vector<FallingBlockMotion> copy;
        { std::lock_guard lk(fallingMutex_); copy = fallingBlocks_; }
        for (auto& f : copy)
            sendFallingBlockSpawnTo(player, f.eid, f.state, f.x, f.y, f.z, f.vx, f.vy, f.vz);
    }
    // TNT_V1 / PROJECTILE_V1: replay active dynamic entities to late joiners.
    {
        std::vector<PrimedTntMotion> copy;
        { std::lock_guard lk(primedTntMutex_); copy = primedTnt_; }
        for (const auto& t : copy) {
            net::Buffer sp; sp.writeVarInt(t.eid);
            sp.writeUUID(UUID{static_cast<u64>(t.eid), 0x71000000ULL + static_cast<u64>(t.eid)});
            sp.writeVarInt(106); sp.writeF64(t.x); sp.writeF64(t.y); sp.writeF64(t.z); // ENTITY_ID_FIX_V2: tnt = 106
            sp.writeByte(0); sp.writeByte(0); sp.writeByte(0); sp.writeVarInt(0);
            auto vel = [](f64 v) { return static_cast<i16>(std::clamp(v * 8000.0, -32000.0, 32000.0)); };
            sp.writeI16(vel(t.vx)); sp.writeI16(vel(t.vy)); sp.writeI16(vel(t.vz));
            player->getConnection()->sendPacket(0x01, std::vector<u8>(sp.writtenSpan().begin(), sp.writtenSpan().end()));
        }
    }
    {
        std::vector<ProjectileMotion> copy;
        { std::lock_guard lk(projectilesMutex_); copy = projectiles_; }
        for (const auto& q : copy) {
            net::Buffer sp; sp.writeVarInt(q.eid);
            sp.writeUUID(UUID{static_cast<u64>(q.eid), 0x72000000ULL + static_cast<u64>(q.eid)});
            sp.writeVarInt(q.typeId); sp.writeF64(q.x); sp.writeF64(q.y); sp.writeF64(q.z);
            sp.writeByte(0); sp.writeByte(0); sp.writeByte(0); sp.writeVarInt(q.ownerEid);
            auto vel = [](f64 v) { return static_cast<i16>(std::clamp(v * 8000.0, -32000.0, 32000.0)); };
            sp.writeI16(vel(q.vx)); sp.writeI16(vel(q.vy)); sp.writeI16(vel(q.vz));
            player->getConnection()->sendPacket(0x01, std::vector<u8>(sp.writtenSpan().begin(), sp.writtenSpan().end()));
        }
    }
    // WEATHER_SYNC_V1: новичок получает то же состояние погоды, что и остальные.
    sendWeatherState(player);
    if (player->tpsBossbarEnabled) sendTpsBossbar(player, true); // TPS_BOSS_V3: duplicate reconnect restores the bar immediately
    tabListDirty_.store(true, std::memory_order_relaxed); // STRESS_FIX_V1: раньше слали пакет мгновенно на каждый вход — шторм при массовом join
    NC_DEBUG("Server", "MP_V1: {} viden ostalnym igrokam", player->getName());
}

// ============================================================
// TABLIST_COUNT_V1: header/footer таб-листа с числом игроков онлайн (RU/EN).
// Ванильный клиент рисует в списке максимум 80 ников (4 колонки по 20), поэтому реальное
// число онлайн всегда показываем в header/footer пакетом 0x6D, его список ников клиент не ограничивает.
// ============================================================
void NetherCraftServer::broadcastTabListHeaderFooter() {
    // TABLIST_OPT_V1: троттлинг до 1 раза/с. Косметический header/footer не нужен ��а 20 Гц,
    // а при churn (join/leave каждый тик) рассылка на всех = O(n^2) шторм
    // (профайлер поймал фазу tabList-broadcast как виновника). Перевзводим dirty, чтобы не потерять апдейт.
    static u64 s_lastBcastTick = 0;
    static bool s_bcastInit = false;
    if (s_bcastInit && tickCounter_ - s_lastBcastTick < 20) {
        tabListDirty_.store(true, std::memory_order_relaxed);
        return;
    }
    s_lastBcastTick = tickCounter_;
    s_bcastInit = true;

    auto all = getAllPlayersCopy();
    size_t online = 0;
    for (auto& p : all) {
        if (p && p->isAlive() && p->getState() == entity::PlayerState::Play) ++online;
    }
    const bool ru = (config_.language == "rus");
    // TABLIST_FIX_V1: раньше кириллица бралась через \u-эскейпы в ОБЫЧНОМ (без u8-префикса) строковом литерале,
    // переданном в std::format: MSVC без /utf-8 конвертирует \u-эскейпы в обычной строке в execution
    // charset (ANSI/CP1251), А НЕ в UTF-8 — те же самые "Игроков" уходили на провод как байты CP1251,
    // а это невалидный UTF-8 для NBT-строки — именно это валило клиент с "Failed to decode packet
    // 'clientbound/minecraft:tab_list'" (то же самое объяснялось раньше с иконкой сервера, см. ICON_V3).
    // u8"..." гарантированно даёт реальные UTF-8 байты независимо от флагов компилятора.
    auto u8s = [](const char8_t* s) { return std::string(reinterpret_cast<const char*>(s)); };

    // TABSERVER_V1: настройки (имя/онлайн/свои строки/полное отключение) читаются из папки tab_Server —
    // см. core/tab.gen.hpp. Файл читается заново на каждый вызов (он маленький, а сам вызов не на каждый тик),
    // поэтому правки в tab_Server/tab.properties подхватываются без перезапуска сервера.
    // TABLIST_OPT_V1: раньше конфиг читался с ДИСКА на КАЖДЫЙ вызов (I/O на тик-потоке —
    // отсюда спайк 30.5мс, тик #996). Кэшируем и перечитываем не чаще раза в 5с (100 тиков),
    // правки в tab_Server/tab.properties всё равно подхватываются без перезапуска.
    static nc::tab::TabConfig s_tabCfg;
    static bool s_tabCfgInit = false;
    static u64 s_tabCfgTick = 0;
    if (!s_tabCfgInit || tickCounter_ - s_tabCfgTick >= 100) {
        s_tabCfg = nc::tab::loadTabConfig(ru);
        s_tabCfgInit = true;
        s_tabCfgTick = tickCounter_;
    }
    const nc::tab::TabConfig& tabCfg = s_tabCfg;
    if (tabCfg.disable) {
        // Пользователь попросил ванильный таб-лист без кастомной шапки/подвала — просто ничего не шлём,
        // клиент сам нарисует обычный список ников без header/footer.
        return;
    }

    // TABENC_V2: MSVC без /utf-8 читает исходник как ANSI, поэтому u8"..." кодирует уже-UTF-8
    // байты ВТОРОЙ раз — отсюда кракозябры в табе. Обычные литералы уходят байт-в-байт
    // (в чате кириллица же нормальная), поэтому здесь используем именно их.
    (void)u8s;
    const std::string colorB = "§b";
    const std::string colorGray = "§7";
    const std::string colorWhite = "§f";

    std::string header = colorB + tabCfg.name;
    if (tabCfg.showOnline) {
        header += "\n" + colorGray
            + std::string(ru ? "Игроков онлайн: " : "Players online: ") // TABENC_V2
            + colorWhite + std::to_string(online) + "/" + std::to_string(config_.maxPlayers); // TABCOUNT_V1: N/N (онлайн/макс)
    }

    std::string footer;
    if (online > 80) {
        footer += colorGray
            + std::string(ru ? "Показаны первые 80 из " : "Showing the first 80 of ") // TABENC_V2
            + std::to_string(online);
    }
    for (const auto& extra : tabCfg.extraLines) {
        if (!footer.empty()) footer += "\n";
        footer += extra;
    }

    auto writeNbtText = [](net::Buffer& buf, const std::string& text) {
        buf.writeByte(0x08); // TAG_String
        buf.writeU16(static_cast<u16>(text.size()));
        buf.writeBytes(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
    };

    net::Buffer buf;
    writeNbtText(buf, header);
    writeNbtText(buf, footer);
    std::vector<u8> payload(buf.writtenSpan().begin(), buf.writtenSpan().end());
    // TABLIST_V2: even with unchanged payload, a freshly joined or duplicate-login
    // player must receive header/footer again; otherwise it looks "disabled".
    static std::vector<u8> s_lastPayload;
    const bool payloadChanged = payload != s_lastPayload;
    if (payloadChanged) s_lastPayload = payload;
    for (auto& p : all) {
        if (p && p->isAlive() && p->getState() == entity::PlayerState::Play) {
            if (payloadChanged || !p->tabHeaderFooterSent) {
                p->getConnection()->sendPacket(0x6D, payload);
                p->tabHeaderFooterSent = true;
            }
        }
    }
}

void NetherCraftServer::applyEnvironmentalDamage(const std::shared_ptr<entity::Player>& player,
                                                   f32 damage, i32 damageTypeId,
                                                   const std::string& deathMessage) {
    if (!player || !player->isAlive() || damage <= 0.0f) return;
    // DEATH_TICK_GUARD_V1: после смерти больше не считаем и не рассылаем damage/hurt.
    if (player->dead || player->health <= 0.0f) return;
    if (player->gameMode == 1 || player->gameMode == 3) return;
    if (player->respawnInvulnerabilityTicks > 0) return; // RESPAWN_INVULN_V1

    // EFFECTS_APPLY_V1: Сопротивление (id 10) режет входящий урон на 20% за уровень.
    const i32 resistAmp = playerEffectAmplifier(player, 10);
    if (resistAmp >= 0) {
        damage *= std::max(0.0f, 1.0f - 0.2f * static_cast<f32>(resistAmp + 1));
        if (damage <= 0.0f) return;
    }

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
        // Ставим terminal state ДО сетевых пакетов: параллельный/следующий env tick
        // уже не сможет отправить второй Damage Event или второй death packet.
        player->dead = true;
        player->health = 0.0f;
        player->usingShield = false;
        player->lavaHurtCooldown = 0;
        player->remainingFireTicks = 0;
        player->airSupply = 300;
        player->ticksFrozen = 0;

        // Снять ON_FIRE сразу, иначе death-screen продолжает рисовать горящую модель.
        if (player->fireFlagSynced) {
            net::Buffer fm;
            fm.writeVarInt(eid);
            fm.writeByte(0); fm.writeVarInt(0);
            u8 flags = 0;
            if (player->sneaking) flags |= 0x02;
            if (player->sprinting) flags |= 0x08;
            fm.writeByte(flags);
            fm.writeByte(0xFF);
            auto fv = std::vector<u8>(fm.writtenSpan().begin(), fm.writtenSpan().end());
            for (auto& target : getAllPlayersCopy())
                if (target && target->isAlive() && target->getState() == entity::PlayerState::Play)
                    target->getConnection()->sendPacket(0x58, fv);
            player->fireFlagSynced = false;
        }

        net::Buffer death;
        death.writeVarInt(eid);
        writeTextComponent(death, deathMessage);
        player->getConnection()->sendPacket(0x3C,
            std::vector<u8>(death.writtenSpan().begin(), death.writtenSpan().end()));
        for (auto& target : getAllPlayersCopy())
            if (target && target->isAlive()) target->sendSystemMessage(std::string("§7") + deathMessage);
        for (auto& pp : getAllPlayersCopy()) despawnPlayerFor(pp, player); // DEATHVIS_V1: труп не должен стоять столбом у других
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
    if (player->gameMode == 1 || player->gameMode == 3 || player->dead ||
        player->respawnInvulnerabilityTicks > 0) {
        player->fallPeakY = newY;
        player->fallWasOnGround = newOnGround;
        return;
    }

    if (newOnGround) {
        // ELYTRA_LANDING_V1: landing exits the fall-flying pose before any regular
        // fall-damage calculation. The glide itself has already dissipated the fall.
        const bool landedWhileGliding = player->elytraFlying;
        if (landedWhileGliding) { player->elytraFlying = false; broadcastEntityMeta(player); }
        if (!player->fallWasOnGround && !landedWhileGliding) {
            const f64 fallDistance = player->fallPeakY - newY;
            // FARMLAND_TRAMPLE_V1: landing on farmland from more than 0.5 blocks tramples it
            // back into dirt (sneaking players are exempt, matching vanilla FarmBlock.fallOn).
            // Any crop growing on top is destroyed and drops seeds/produce based on how grown
            // it was.
            if (fallDistance > 0.5 && !player->sneaking) {
                const i32 tpx = static_cast<i32>(std::floor(player->getX()));
                const i32 tpz = static_cast<i32>(std::floor(player->getZ()));
                const i32 tfy = static_cast<i32>(std::floor(newY - 0.1));
                const i32 farmState = world_.getBlock(tpx, tfy, tpz);
                if (farmState >= 4286 && farmState <= 4293) {
                    struct FarmCrop { i32 base; i32 maxAge; i32 seedItem; i32 productItem; };
                    static const FarmCrop kFarmCrops[] = {
                        {4278, 7, 853, 854},    // wheat: wheat_seeds -> wheat
                        {8595, 7, 1097, 1097},  // carrots: carrot is both seed & product
                        {8603, 7, 1098, 1098},  // potatoes: potato is both seed & product
                        {12509, 3, 1155, 1154}, // beetroots: beetroot_seeds -> beetroot
                    };
                    const i32 cropState = world_.getBlock(tpx, tfy + 1, tpz);
                    for (const auto& c : kFarmCrops) {
                        if (cropState < c.base || cropState > c.base + c.maxAge) continue;
                        const i32 age = cropState - c.base;
                        world_.setBlock(tpx, tfy + 1, tpz, 0); // crop -> air
                        net::Buffer cb; cb.writePosition(BlockPos{tpx, tfy + 1, tpz}); cb.writeVarInt(0);
                        const auto cbBytes = std::vector<u8>(cb.writtenSpan().begin(), cb.writtenSpan().end());
                        for (auto& pl : getAllPlayersCopy())
                            if (pl && pl->isAlive() && pl->getState() == entity::PlayerState::Play)
                                pl->getConnection()->sendPacket(0x09, cbBytes);
                        const f64 dropX = tpx + 0.5, dropY = tfy + 1.1, dropZ = tpz + 0.5;
                        if (age >= c.maxAge) {
                            spawnItemDrop(dropX, dropY, dropZ, c.productItem, 1, 0.0, 0.15, 0.0);
                            spawnItemDrop(dropX, dropY, dropZ, c.seedItem, 1 + (std::rand() % 4), 0.0, 0.15, 0.0);
                        } else {
                            spawnItemDrop(dropX, dropY, dropZ, c.seedItem, 1, 0.0, 0.15, 0.0);
                        }
                        break;
                    }
                    world_.setBlock(tpx, tfy, tpz, 10); // farmland -> dirt
                    net::Buffer fb; fb.writePosition(BlockPos{tpx, tfy, tpz}); fb.writeVarInt(10);
                    const auto fbBytes = std::vector<u8>(fb.writtenSpan().begin(), fb.writtenSpan().end());
                    for (auto& pl : getAllPlayersCopy())
                        if (pl && pl->isAlive() && pl->getState() == entity::PlayerState::Play)
                            pl->getConnection()->sendPacket(0x09, fbBytes);
                }
            }
            if (fallDistance > 3.0) {
                // FALLSOFT_V1: блок приз��мления влияет на урон (см. core/item_blocks.gen.hpp)
                const i32 px = static_cast<i32>(std::floor(player->getX()));
                const i32 pz = static_cast<i32>(std::floor(player->getZ()));
                const i32 feet   = world_.getBlock(px, static_cast<i32>(std::floor(newY - 0.2)), pz);
                const i32 inside = world_.getBlock(px, static_cast<i32>(std::floor(newY + 0.1)), pz);
                auto isWater = [](i32 s) { return (s >= 80 && s <= 95) || s == 12960 || s == 12961; };
                const bool soft = isWater(feet) || isWater(inside)  // вода полностью гасит урон
                    || feet == 2004  || inside == 2004             // cobweb
                    || feet == 22318 || inside == 22318            // powder_snow
                    || feet == 18575 || inside == 18575            // sweet_berry_bush
                    || feet == 19445;                              // honey_block
                f64 dmgF = fallDistance - 3.0;
                if (feet == 10726) dmgF *= 0.2;                    // hay_block: -80% урона
                if (feet == 10364) {                               // BOUNCE_V1: slime_block — отскок без урона
                    const f64 up = std::min(fallDistance * 0.2, 2.5);
                    auto q = [](f64 v) { return static_cast<i16>(std::clamp(v * 8000.0, -32000.0, 32000.0)); };
                    net::Buffer vb; vb.writeVarInt(static_cast<i32>(player->getEntityId()));
                    vb.writeI16(0); vb.writeI16(q(up)); vb.writeI16(0);
                    player->getConnection()->sendPacket(0x5A, std::vector<u8>(vb.writtenSpan().begin(), vb.writtenSpan().end()));
                    dmgF = 0.0;
                } else if (soft) {
                    dmgF = 0.0;
                }
                // LivingEntity.calculateFallDamage uses Mth.ceil, not floor.
                // This matters for hay and other fractional multipliers.
                const i32 damage = static_cast<i32>(std::ceil(dmgF));
                if (damage > 0) {
                    applyEnvironmentalDamage(player, static_cast<f32>(damage), 9,
                        std::format("{} разбился при падении", player->getName()));
                }
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

    // CHUNKVIS_V1/V2: пересчёт видимости — только при ПЕРЕСЕЧЕНИИ границы чанка.
    // V1 гонял цикл по всем игрокам на КАЖДОМ пакете движения: 500 ботов x 20 пак/с x 500
    // проверок = ~5 млн итераций/с + копия списка игроков на каждый пакет — именно это
    // убило сервер под 500 ботами. Внутри чанка видимости меняться не может (метрика в чанках).
    if (posChanged) {
        const i32 pcx = static_cast<i32>(std::floor(player->getX() / 16.0));
        const i32 pcz = static_cast<i32>(std::floor(player->getZ() / 16.0));
        if (pcx != player->mpVisChunkX || pcz != player->mpVisChunkZ) {
            player->mpVisChunkX = pcx;
            player->mpVisChunkZ = pcz;
            const i32 vr = entityViewRadiusChunks(config_.viewDistance);
            for (auto& other : getAllPlayersCopy()) {
                if (!other || other.get() == player.get()) continue;
                if (!other->isAlive() || other->getState() != entity::PlayerState::Play) continue;
                if (mpChunkDistance(*other, *player) <= vr) {
                    spawnPlayerFor(other, player);
                    spawnPlayerFor(player, other);
                } else {
                    despawnPlayerFor(other, player);
                    despawnPlayerFor(player, other);
                }
            }
        }
    }
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
        broadcastToOthers(player, 0x70, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()), true);
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
        broadcastToOthers(player, 0x2F, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()), true);
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
        broadcastToOthers(player, 0x2E, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()), true);
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
        broadcastToOthers(player, 0x30, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()), true);
    }

    if (rotChanged) {
        // Entity Head Rotation (0x48)
        net::Buffer h;
        h.writeVarInt(eid);
        h.writeByte(yaw.value);
        broadcastToOthers(player, 0x48, std::vector<u8>(h.writtenSpan().begin(), h.writtenSpan().end()), true);
    }
}

void NetherCraftServer::broadcastPlayerRemove(const std::shared_ptr<entity::Player>& player) {
    if (!player) return;
    const i32 eid = static_cast<i32>(player->getEntityId());
    // CHUNKVIS_V1: вычистить eid из сетов видимости всех (иначе копятся мёртвые записи)
    for (auto& p : getAllPlayersCopy()) if (p) p->removeVisibleEntity(player->getEntityId());
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
    // MULTIWORLD_V1: shadow the member so every player-driven block edit below
    // lands in the dimension that player is actually standing in.
    world::World& world_ = worldOf(player); g_dimCtx = player ? player->dimension : 0; // DIMPHYS_V1
    // FLATWORLD_V1: убран лог-спам Play packet
    switch (wireId) {
        case 0x00: { // ESSENTIALS_V1: Teleport Confirm — клиент подтвердил телепорт
            const i32 confirmedTpId = data.readVarInt(); // TPFIX_V2: id больше не выбрасываем
            if (player->awaitingTeleport && confirmedTpId == player->pendingTeleportId) player->awaitingTeleport = false;
            break;
        }
        case 0x03: { // ESSENTIALS_V1: Message Acknowledgement — читаем счётчик подтверждённых сообщений
            (void)data.readVarInt(); // count
            break;
        }
        case 0x09: { // ESSENTIALS_V1 + COMBAT_V1: Client Command (0 = respawn, 1 = request stats)
            i32 actionId = data.readVarInt();
            if (actionId == 1) { // STATS_V8: Award Statistics (0x04) — ответ на открытие экрана
                // статистики в меню паузы. Сервер не ведёт счётчики — отдаём пустой
                // корректный список, чтобы экран открывался, а не вис на загрузке.
                net::Buffer st;
                st.writeVarInt(0);
                player->getConnection()->sendPacket(0x04, std::vector<u8>(st.writtenSpan().begin(), st.writtenSpan().end()));
            }
            if (actionId == 0) {
                // COMBAT_V1: полноценный респавн после смерти — Respawn (0x47) + сброс здоровья.
                player->health = 20.0f;
                player->dead = false;
                // RESPAWN_INVULN_V1: ванильное окно защиты после настоящего
                // death-respawn. RespawnPacket для смены скина ниже его не выдаёт.
                player->respawnInvulnerabilityTicks = 60;
                player->lastHurtMs = 0;
                player->lastDamageTaken = 0.0f;
                player->lavaHurtCooldown = 0;
                player->fallPeakY = static_cast<f64>(g_spawnY);
                player->fallWasOnGround = true;
                if (player->dimension != 0) { // DIMRESPAWN_V1: died in the Nether/End -> back to the overworld
                    player->dimension = 0;
                    player->portalCooldownTicks = 300;
                    player->portalTimeTicks = 0;
                    player->clearSeenChunks();
                    g_dimCtx = 0;
                }
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
                { // RESPAWN_V2: Game Event 13 «start waiting for level chunks» — без него клиент висит на «Loading terrain» до таймаута
                    net::Buffer ge;
                    ge.writeByte(13);
                    ge.writeF32(0.0f);
                    player->getConnection()->sendPacket(0x22, std::vector<u8>(ge.writtenSpan().begin(), ge.writtenSpan().end()));
                }
                { // TITLES_V8: Clear Titles (0x0F) — ванильный респавн сбрасывает титры
                    net::Buffer ct;
                    ct.writeBool(true); // reset
                    player->getConnection()->sendPacket(0x0F, std::vector<u8>(ct.writtenSpan().begin(), ct.writtenSpan().end()));
                }
                if (player->inCombat) { // COMBAT_V8: End Combat Event (0x3A) после смерти/респавна
                    net::Buffer ec;
                    ec.writeVarInt(0);
                    player->getConnection()->sendPacket(0x3A, std::vector<u8>(ec.writtenSpan().begin(), ec.writtenSpan().end()));
                    player->inCombat = false;
                    player->combatStartMs = 0;
                }
                player->clearSeenChunks(); // RESPAWN_V2: клиент выбросил чанки после Respawn — отправим заново
                player->openWindowId = 0;  // RESPAWN_V2: окно контейнера на клиенте закрыто после смерти
                player->setPosition(g_spawnX + 0.5, (f64)g_spawnY, g_spawnZ + 0.5);
                sendPlayerPositionAndLook(player);
                { net::Buffer uh; uh.writeF32(20.0f); uh.writeVarInt(20); uh.writeF32(5.0f);
                  player->getConnection()->sendPacket(0x5D, std::vector<u8>(uh.writtenSpan().begin(), uh.writtenSpan().end())); }
                syncExperienceBar(player);
                sendTimeUpdate(player); // TIMESYNC_V1: Respawn сбрасывает время суток у клиента — синхронизируем заново
                streamChunks(player);
                player->clearVisibleEntities(); // CHUNKVIS_V1: клиент выбрасывает сущности после Respawn
                for (auto& pp : getAllPlayersCopy()) { spawnPlayerFor(pp, player); spawnPlayerFor(player, pp); } // DEATHVIS_V1 + CHUNKVIS_V1
                { // INVRESPAWN_V1: клиент прячет вещи после смерти — заливаем инвентарь заново
                    net::Buffer inv;
                    inv.writeByte(0);            // containerId = 0 (окно инвентаря игрока)
                    inv.writeVarInt(1);          // state id
                    inv.writeVarInt(entity::Player::INV_SIZE);
                    for (int i = 0; i < entity::Player::INV_SIZE; ++i) {
                        const i32 id = player->invItemId[i];
                        const i32 cnt = player->invCount[i];
                        if (cnt > 0 && id > 0) { inv.writeVarInt(cnt); inv.writeVarInt(id); inv.writeVarInt(0); inv.writeVarInt(0); }
                        else { inv.writeVarInt(0); }
                    }
                    inv.writeVarInt(0);          // предмет в курсоре: пусто
                    player->getConnection()->sendPacket(0x13, std::vector<u8>(inv.writtenSpan().begin(), inv.writtenSpan().end()));
                }
                { // PERMLEVEL_V2: после респавна пересылаем уровень прав (фикс F3+F4 «недостаточно полномочий»)
                    net::Buffer ev;
                    ev.writeI32(static_cast<i32>(player->getEntityId()));
                    ev.writeByte(static_cast<u8>(24 + nc::opLevelOf(config_.ops, player->getName())));
                    player->getConnection()->sendPacket(0x1F, std::vector<u8>(ev.writtenSpan().begin(), ev.writtenSpan().end()));
                }
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
            (void)abilityFlags; // полёт в креативе клиент выполняет локально; читаем байт, чтобы не сломать парсер
            break;
        }
        case 0x0A: { // SELFSKIN_V1: Client Information (play) — клиент сменил настройки (в т.ч. слои скина)
            player->clientLocale = data.readString(); // I18N_V1: клиент мог сменить язык в настройках
            (void)data.readByte();           // view distance (i8)
            (void)data.readVarInt();         // chat flags
            (void)data.readBool();           // chat colors
            player->displayedSkinParts = static_cast<u8>(data.readByte()); // маска слоёв (плащ/куртка/рукава/штанины/шляпа)
            (void)data.readVarInt();         // main hand
            (void)data.readBool();           // text filtering
            (void)data.readBool();           // server listing
            // обновить свою модель (self) + разослать остальным
            {
                net::Buffer sm;
                buildEntityMeta(sm, static_cast<i32>(player->getEntityId()), player->sneaking, player->sprinting,
                    player->displayedSkinParts, player->remainingFireTicks > 0, player->elytraFlying);
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
            // VEHICLE_PHYSICS_V1: interact seats the player, attack breaks the vehicle
            if ((interactType == 0 || interactType == 2) && player->gameMode != 3) {
                if (!vehicleInteract(player, targetEid)) mobInteract(player, targetEid); // MOBS_V1: молоко/стрижка
                break;
            }
            if (interactType != 1) break;      // обрабатываем только атаку
            if (vehicleAttack(player, targetEid)) break;
            if (mobAttack(player, targetEid)) break; // MOBS_V1: урон и дроп животным
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
            if (victim->respawnInvulnerabilityTicks > 0) break; // RESPAWN_INVULN_V1: PvP то��е блокируется
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
            f32 dmg = baseDmg * (0.2f + 0.8f * charge * charge); // недозаряженный удар слабее (как в ваниле)
            // CRIT_V1: крит как в ваниле — атакующий падает (не на земле, ниже пика падения), не спринтует, удар заряжен
            const bool crit = !player->isOnGround() && player->getY() < player->fallPeakY && !player->sprinting && charge > 0.848f;
            if (crit) dmg *= 1.5f;
            // SHIELD_V1: поднятый щит блокирует удар из передней полусферы
            bool blocked = false;
            // SHIELD_V2: щит блокирует только после ванильного прогрева 5 тиков (250 мс)
            if (victim->usingShield && nowMs - victim->shieldRaisedMs >= 250 && dmg > 0.0f) {
                const f64 adx = player->getX() - victim->getX();
                const f64 adz = player->getZ() - victim->getZ();
                const f64 vyawR = static_cast<f64>(victim->get_yaw()) * 3.14159265358979323846 / 180.0;
                const f64 lookX = -std::sin(vyawR), lookZ = std::cos(vyawR); // куда смотрит жертва
                const f64 alen = std::sqrt(adx * adx + adz * adz);
                if (alen > 1e-4 && (adx / alen) * lookX + (adz / alen) * lookZ > 0.0) {
                    blocked = true;
                    dmg = 0.0f;
                    // SHIELD_V2: топор отключает щит на 5 секунд (как в ванилле)
                    const bool axeHit = (heldItem >= 818 && heldItem <= 847 && (heldItem - 818) % 5 == 3);
                    if (axeHit) {
                        victim->usingShield = false;
                        victim->shieldDisabledUntilMs = nowMs + 5000;
                        broadcastHandState(victim);
                        broadcastBlockSound("minecraft:item.shield.break",
                            static_cast<i32>(std::floor(victim->getX())), static_cast<i32>(std::floor(victim->getY())),
                            static_cast<i32>(std::floor(victim->getZ())), 1.0f, 1.0f);
                    } else {
                        broadcastBlockSound("minecraft:item.shield.block",
                            static_cast<i32>(std::floor(victim->getX())), static_cast<i32>(std::floor(victim->getY())),
                            static_cast<i32>(std::floor(victim->getZ())), 1.0f, 1.0f);
                    }
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
            // IFRAME_V2 (COMBAT_V3): ванильные i-frames — 0.5 с, НО более сильный удар в окне
            // неуязвимости досчитывает разницу (как в ванилле) — джамп-крит теперь добивает
            // быстро, а закликивание слабыми ударами по-прежнему не работает.
            if (!blocked) {
                if (nowMs - victim->lastHurtMs < 500) {
                    if (dmg <= victim->lastDamageTaken) break; // слабее/равен прошлому — игнор
                    const f32 diff = dmg - victim->lastDamageTaken;
                    victim->lastDamageTaken = dmg; // запоминаем полный урон этого окна
                    dmg = diff;                    // применяем только разницу
                } else {
                    victim->lastHurtMs = nowMs;
                    victim->lastDamageTaken = dmg;
                }
            }
            if (!blocked) { // COMBAT_V8: Enter Combat Event (0x3B) — ванильный вход в бой
                if (!victim->inCombat) {
                    victim->inCombat = true;
                    victim->getConnection()->sendPacket(0x3B, std::vector<u8>{});
                }
                if (!player->inCombat) {
                    player->inCombat = true;
                    player->getConnection()->sendPacket(0x3B, std::vector<u8>{});
                }
                victim->combatStartMs = victim->combatStartMs ? victim->combatStartMs : nowMs;
                player->combatStartMs = player->combatStartMs ? player->combatStartMs : nowMs;
            }
            victim->health -= dmg;
            if (victim->health < 0.0f) victim->health = 0.0f;
            if (!blocked) { // SOUND_V8: Entity Sound Effect (0x67) — звук боли привязан к сущности
                net::Buffer es;
                es.writeVarInt(0);                                  // inline sound holder
                es.writeString("minecraft:entity.player.hurt");
                es.writeBool(false);                                // no fixed range
                es.writeVarInt(7);                                  // sound category: players (SoundSource ordinal 7)
                es.writeVarInt(vEid);
                es.writeF32(1.0f);
                es.writeF32(1.0f);
                es.writeI64(static_cast<i64>(nowMs));
                auto esv = std::vector<u8>(es.writtenSpan().begin(), es.writtenSpan().end());
                for (auto& p : getAllPlayersCopy())
                    if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                        p->getConnection()->sendPacket(0x67, esv);
            }
            if (victim->health <= 0.0f && victim->inCombat) { // COMBAT_V8: End Combat Event (0x3A)
                net::Buffer ec;
                ec.writeVarInt(static_cast<i32>(std::max<i64>(0, (nowMs - victim->combatStartMs) / 50)));
                victim->getConnection()->sendPacket(0x3A, std::vector<u8>(ec.writtenSpan().begin(), ec.writtenSpan().end()));
                victim->inCombat = false;
                victim->combatStartMs = 0;
            }
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
                const f64 kb = 0.4; // SHIELD_V1: при блоке кнокбек слабее
                net::Buffer kbuf; // кнокбек (Entity Velocity 0x5A)
                kbuf.writeVarInt(vEid);
                kbuf.writeI16(static_cast<i16>((ddx / len) * kb * 8000.0));
                kbuf.writeI16(static_cast<i16>(0.36 * 8000.0));
                kbuf.writeI16(static_cast<i16>((ddz / len) * kb * 8000.0));
                if (!blocked) sendAll(0x5A, kbuf); // SHIELD_V2: заблокированный удар не отбрасывает жертву
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
                for (auto& pp : getAllPlayersCopy()) despawnPlayerFor(pp, victim); // DEATHVIS_V1: труп не должен стоять столбом у других
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
            if (player->awaitingTeleport) break; // TPFIX_V2: до Confirm Teleport клиент шлёт СТАРУЮ позицию
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
            if (player->awaitingTeleport) break; // TPFIX_V2
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
            broadcastToOthers(player, 0x03, std::vector<u8>(an.writtenSpan().begin(), an.writtenSpan().end()), true);
            break;
        }
        case 0x25: { // PLAYER_VIS_V1: Player Command (присед/спринт)
            (void)data.readVarInt(); // id сущности
            i32 action = data.readVarInt();
            (void)data.readVarInt(); // доп. данные
            // ELYTRA_DEPLOY_V1: START_FALL_FLYING is action 8 in protocol 767.
            // Slot 6 is the chest armour slot; 773 is minecraft:elytra in the same
            // protocol item registry as the existing ender-pearl id 993.
            // ELYTRA_FIX_V2: the client renders FALL_FLYING as crawling when it
            // receives pose=FALL_FLYING without DATA_SHARED_FLAGS bit 7 in the same
            // authoritative update.  Unlike the earlier generic broadcast (which
            // intentionally omits its subject), send the accepted state to the pilot
            // as well as observers.  This mirrors vanilla's startFallFlying state.
            bool elytraStateChanged = false;
            if (action == 8 && player->gameMode != 3 && !player->isOnGround() &&
                player->invCount[6] > 0 && player->invItemId[6] == 773) {
                const i32 px = static_cast<i32>(std::floor(player->getX()));
                const i32 py = static_cast<i32>(std::floor(player->getY() + 0.8));
                const i32 pz = static_cast<i32>(std::floor(player->getZ()));
                const i32 bodyFluid = world_.getBlock(px, py, pz);
                const bool inWater = (bodyFluid >= 80 && bodyFluid <= 95) || bodyFluid == 12960 || bodyFluid == 12961;
                if (!inWater && !player->elytraFlying) {
                    player->elytraFlying = true;
                    player->fallPeakY = player->getY(); // prevent stale pre-glide fall distance
                    elytraStateChanged = true;
                }
            }
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
            i64 id = data.readI64();
            // KEEPALIVE_TIMEOUT_V1: подтверждаем, что клиент жив и отвечает на наш последний пакет
            if (id == player->pendingKeepAliveId) {
                player->awaitingKeepAlive = false;
                // PINGSTAT_V1: id keep-alive — это метка отправки в мс с того же часового
                // источника, так что разница с текущим временем и есть пинг.
                // Сглаживаем (2/3 старого + 1/3 нового), чтобы одиночный всплеск
                // не прыгал в консоли и панели.
                const i64 nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                const i32 rtt = static_cast<i32>(std::clamp<i64>(nowMs - id, 0, 60000));
                player->pingMs = (player->pingMs < 0) ? rtt
                                                      : static_cast<i32>((player->pingMs * 2 + rtt) / 3);
            }
            break;
        }
        case 0x06: { // Chat Message
            std::string message = data.readString();
            // Пропускаем timestamp, salt, signature, message count, ack
            // CHATLOG_V2: чат игроков вообще не логируем — ни в файл, ни в консоль (просьба владельца)

            // CHAT_V2: рассылка через System Chat (0x6C, NBT text) через sendSystemMessage.
            // Было: пакет 0x3F (= player_look_at) с JSON -> клиент падал на декоде.
            setCrashContext("core", "chat message", player->getName()); // CRASHCTX_V1
            // CHATASYNC_V1: was an inline loop of blocking sends to every player
            // right here on the packet/tick thread (laggy on a full server).
            // Now we just format the line and hand it to the chat worker thread.
            enqueueChatBroadcast(std::format("<{}> {}", player->getName(), message));
            clearCrashContext(); // CRASHCTX_V1
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
            // CRASHCTX_V1: record what's about to run on THIS connection's thread so
            // that if it crashes, the crash report can say exactly which command (and
            // whose) caused it — "source" defaults to core since there are no plugins
            // yet; once plugin commands exist they should tag themselves accordingly.
            setCrashContext("core", command.empty() ? "/(empty)" : ("/" + command), player->getName());
            const bool op = nc::opAllowed(config_.ops, player->getName()); // OPMGR_V1: ops.json -> ops= CSV -> bootstrap mode
            auto needOp = [&]() { player->sendSystemMessage("§cНужны права оператора (ops= в settings.properties)"); };
            auto broadcastBlock = [&](i32 sx, i32 sy, i32 sz, i32 st) {
                if (isConcretePowderState(st) &&
                    concretePowderShouldSolidify(world_, sx, sy, sz, world_.getBlock(sx, sy, sz)))
                    st = concreteFromPowder(st); // ConcretePowderBlock.getStateForPlacement/updateShape
                world_.setBlock(sx, sy, sz, st);
                scheduleFluidNeighbors(sx, sy, sz); // FLUID_V1: /setblock тоже триггерит поток
                scheduleFallingBlockUpdate(sx, sy, sz, 2);
                scheduleFallingBlockUpdate(sx, sy + 1, sz, 2);
                scheduleFallingColumnCascade(sx, sy + 1, sz, 2);
                net::Buffer bu;
                bu.writePosition(BlockPos{sx, sy, sz});
                bu.writeVarInt(st);
                auto vec = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
                auto allPlayers = getAllPlayersCopy();
                for (auto& p : allPlayers) if (p->isAlive()) p->getConnection()->sendPacket(0x09, vec);
                if (isWaterBlockState(st)) solidifyConcretePowderAround(sx, sy, sz);
            };

            if (handleMiniEditCommand(player, command, op)) {
                clearCrashContext();
                break;
            } else if (cmd == "crash") {
                // CRASHTEST_V1: intentional, on-purpose test crash (op-only) so the
                // owner can verify a real crash prints a visible error in the console
                // window instead of the window just silently vanishing. A plain
                // `throw` would NOT reach the console the way a real crash does —
                // Connection::processPacket() already wraps every packet handler in
                // try/catch(...) and would just log+disconnect this one player
                // instead of crashing anything. std::abort() bypasses that entirely:
                // it cannot be caught by any C++ try/catch, so this reliably takes
                // the whole process down on purpose, on demand, with no world-save
                // and no attempt to corrupt memory/data — it's just an honest,
                // deliberate "stop right now" for testing crash visibility.
                if (!op) { needOp(); break; }
                NC_ERROR("Server", "/crash from {}: intentional test crash (std::abort), no save, kicking {} player(s) first", player->getName(), getAllPlayersCopy().size());
                // KICKFIX_V1: process is about to die outright — give every connected
                // client a real Disconnect packet with a reason first, so their game
                // shows "you were disconnected: server crashed" instead of silently
                // hanging/timing out with no explanation.
                for (auto& p : getAllPlayersCopy()) if (p && p->isAlive()) p->kick("§cСервер аварийно остановлен (тест /crash)");
                std::abort();
            } else if (cmd == "stop") { // STOPCMD_V1: остановка из игры (только оператор)
                if (!op) { needOp(); break; }
                NC_INFO("Server", "/stop от {}: остана��ливаю сервер", player->getName());
                queueConsoleCommand("stop"); // выполнится на tick-потоке, а не на сетевом
            } else if (cmd == "reload") { // SOFTRELOAD_V1: мягкий рестарт из игры (только оператор)
                if (!op) { needOp(); break; }
                queueConsoleCommand("reload"); // выполнится на tick-потоке
            } else if (cmd == "help") {
                player->sendSystemMessage("§6/help /tps /list /spawn /setworldspawn [x y z] /tp <x y z> /gamemode <режим|0-3> [ник]");
                player->sendSystemMessage("§6/gm0 /gm1 /gm2 /gm3 [ник] — быстрая смена режима"); // GMSHORT_V1
                player->sendSystemMessage("§6/time set <day|night|тики> /weather <clear|rain|thunder>");
                player->sendSystemMessage("§6/say <текст> /setblock <x y z блок> /kick <ник>");
                player->sendSystemMessage("§6/skin <ник> — скин лицензионного игрока, /skin reset — вернуть свой"); // SKINCMD_V1
                player->sendSystemMessage("§6/summon <pig|zombie|cow|sheep|creeper|skeleton|id> [кол-во] /killall");
                player->sendSystemMessage("§c/stop — остановка, /reload — мягкий рестарт (только оператор)"); // STOPCMD_V1 + SOFTRELOAD_V1
                player->sendSystemMessage("§c/crash — намеренный тестовый краш сервера (только оператор)"); // CRASHTEST_V1
            } else if (cmd == "skin") { // SKINCMD_V1: скин любого лицензионного ника для пиратов в офлайн-моде
                if (args.size() < 2) { player->sendSystemMessage("§cИспользование: /skin <ник с лицензией> или /skin reset"); break; }
                const std::string want = args[1];
                std::string texVal, texSig;
                if (want == "reset") {
                    // вернуть скин по собственному нику (не премиум — Стив/Алекс)
                    fetchSkinTextures(player->getName(), texVal, texSig);
                } else if (!fetchSkinTextures(want, texVal, texSig)) {
                    player->sendSystemMessage(std::format("§cИгрок {} не найден на Mojang (нет лицензии или API недоступен)", want));
                    break;
                }
                player->texturesValue     = texVal;
                player->texturesSignature = texSig;
                { // всем: убрать запись из таба (клиент кеширует скин по uuid) и пересоздать сущность
                    net::Buffer rm;
                    rm.writeVarInt(1);
                    rm.writeUUID(player->getUuid());
                    auto rmv = std::vector<u8>(rm.writtenSpan().begin(), rm.writtenSpan().end());
                    for (auto& other : getAllPlayersCopy()) {
                        if (!other || !other->isAlive() || other->getState() != entity::PlayerState::Play) continue;
                        other->getConnection()->sendPacket(0x3D, rmv); // Player Info Remove
                        if (other.get() == player.get()) continue;
                        despawnPlayerFor(other, player);
                        spawnPlayerFor(other, player); // info-add с новыми текстурами + спавн сущности
                    }
                }
                // сам игрок увидит свой новый скин только после Respawn (data kept) — как у SkinsRestorer
                mpSendPlayerInfo(player, player); // вернуть себя в таб уже с новым скином
                {
                    net::Buffer rs;
                    rs.writeVarInt(dimTypeIndex(player->dimension));   // DIMRESPAWN_V1
                    rs.writeString(dimIdName(player->dimension));
                    rs.writeI64(config_.levelSeed);
                    rs.writeByte((i8)player->gameMode);
                    rs.writeByte(-1);
                    rs.writeBool(false);
                    rs.writeBool(player->dimension == 0);             // is flat
                    rs.writeBool(false);
                    rs.writeVarInt(0);
                    rs.writeByte(0x03);                     // data kept: attributes + metadata
                    player->getConnection()->sendPacket(0x47, std::vector<u8>(rs.writtenSpan().begin(), rs.writtenSpan().end()));
                }
                { net::Buffer ge; ge.writeByte(13); ge.writeF32(0.0f); // start waiting for level chunks
                  player->getConnection()->sendPacket(0x22, std::vector<u8>(ge.writtenSpan().begin(), ge.writtenSpan().end())); }
                player->clearSeenChunks();          // клиент выбросил чанки после Respawn
                sendPlayerPositionAndLook(player);  // остаёмся на том же месте — без телепорта на спавн
                { net::Buffer uh; uh.writeF32(player->health); uh.writeVarInt(20); uh.writeF32(5.0f);
                  player->getConnection()->sendPacket(0x5D, std::vector<u8>(uh.writtenSpan().begin(), uh.writtenSpan().end())); }
                syncExperienceBar(player);
                sendTimeUpdate(player);
                streamChunks(player);
                player->clearVisibleEntities();
                for (auto& pp : getAllPlayersCopy()) if (pp && pp.get() != player.get()) spawnPlayerFor(player, pp);
                { // инвентарь: клиент прячет вещи после Respawn — как INVRESPAWN_V1
                    net::Buffer inv;
                    inv.writeByte(0);
                    inv.writeVarInt(1);
                    inv.writeVarInt(entity::Player::INV_SIZE);
                    for (int i = 0; i < entity::Player::INV_SIZE; ++i) {
                        const i32 iid = player->invItemId[i];
                        const i32 icnt = player->invCount[i];
                        if (icnt > 0 && iid > 0) { inv.writeVarInt(icnt); inv.writeVarInt(iid); inv.writeVarInt(0); inv.writeVarInt(0); }
                        else { inv.writeVarInt(0); }
                    }
                    inv.writeVarInt(0);
                    player->getConnection()->sendPacket(0x13, std::vector<u8>(inv.writtenSpan().begin(), inv.writtenSpan().end()));
                }
                player->sendSystemMessage(want == "reset" ? std::string("§aСкин сброшен") : std::format("§aСкин игрока {} применён", want));
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
            } else if (cmd == "spawn") { // SPAWNCMD_V1 / SPAWNCFG_V1
                const auto __scfg = nc::spawncfg::loadSpawnConfig(config_.language == "rus");
                const bool __forceLang = (__scfg.lang == "ru" || __scfg.lang == "en");
                const auto __sl = __forceLang
                    ? (__scfg.lang == "ru" ? nc::i18n::Lang::Ru : nc::i18n::Lang::En)
                    : nc::i18n::langFromLocale(player->clientLocale);
                if (__scfg.disable) { player->sendSystemMessage(std::string(nc::i18n::tr(__sl, "spawn.disabled"))); break; }
                if (__scfg.warmup > 0) { // SPAWNCFG_V1: warmup=-1/0 — телепорт мгновенный
                    bool __already = false;
                    for (const auto& __w : spawnWarmups_) if (__w.name == player->getName()) { __already = true; break; }
                    if (__already) { player->sendSystemMessage(std::string(nc::i18n::tr(__sl, "spawn.already"))); break; }
                    SpawnWarmup __w;
                    __w.name = player->getName();
                    __w.ticksLeft = __scfg.warmup * 20;
                    __w.sx = player->getX(); __w.sy = player->getY(); __w.sz = player->getZ();
                    __w.standStill = __scfg.standStill;
                    __w.forceLang = __forceLang; __w.forceRu = (__scfg.lang == "ru");
                    __w.color = __scfg.color;
                    __w.textCountdown = __scfg.textCountdown;
                    __w.textDone = __scfg.textDone;
                    __w.textCancelled = __scfg.textCancelled;
                    const std::string __first = __scfg.textCountdown.empty()
                        ? __scfg.color + nc::i18n::f(__sl, "spawn.countdown", __scfg.warmup)
                        : __scfg.color + nc::spawncfg::spawnFillSeconds(__scfg.textCountdown, __scfg.warmup);
                    __w.lastShown = __scfg.warmup;
                    spawnWarmups_.push_back(std::move(__w));
                    player->sendSystemMessage(__first);
                    break;
                }
                if (player->dimension != 0) travelToDimension(player, 0, g_spawnX + 0.5, (f64)g_spawnY, g_spawnZ + 0.5); // SPAWNCMD_V1: спавн живёт в обычном мире
                else teleportSafe(player, g_spawnX + 0.5, (f64)g_spawnY, g_spawnZ + 0.5, true); // TPFIX_V1
                player->sendSystemMessage(__scfg.textDone.empty()
                    ? std::string(nc::i18n::tr(__sl, "spawn.tp"))
                    : __scfg.color + __scfg.textDone);
            } else if (cmd == "setworldspawn" || cmd == "setspawn") { // SPAWNCMD_V1: короткий алиас
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
                const auto __sl = nc::i18n::langFromLocale(player->clientLocale); // SPAWNCMD_V1
                if (!okc) { player->sendSystemMessage(std::string(nc::i18n::tr(__sl, "spawn.int"))); break; }
                g_spawnX = nx; g_spawnY = ny; g_spawnZ = nz;
                writeWorldSpawn(g_spawnX, g_spawnY, g_spawnZ); // SPAWN_V1
                auto allSp = getAllPlayersCopy();
                for (auto& p : allSp) if (p->isAlive()) sendSpawnPosition(p);
                player->sendSystemMessage(nc::i18n::f(__sl, "spawn.set", g_spawnX, g_spawnY, g_spawnZ)); // SPAWNCMD_V1
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
                        // TPFIX_V2: ванильный мировой барьер — 29 999 984, дальше клиент рендерит мусор
                        if (nx < -29999984.0 || nx > 29999984.0 || nz < -29999984.0 || nz > 29999984.0 || ny < -63.0 || ny > 319.0) {
                            player->sendSystemMessage(config_.language == "rus" ? "§cТП вне границ: X/Z ±29999984, Y -63..319" : "§cTeleport out of bounds: X/Z ±29999984, Y -63..319");
                        } else {
                        teleportSafe(player, nx, ny, nz, false); // TPFIX_V1
                        if (config_.language == "rus")
                            player->sendSystemMessage(std::format("§aТелепорт: {:.1f} {:.1f} {:.1f}", nx, ny, nz));
                        else
                            player->sendSystemMessage(std::format("§aTeleported: {:.1f} {:.1f} {:.1f}", nx, ny, nz));
                        }
                    } catch (...) { player->sendSystemMessage("§cКоординаты — числа или ~"); }
                } else player->sendSystemMessage("§cИспользование: /tp <x> <y> <z>");
            } else if (cmd == "mob") { // MOBS_ALL_V1: /mob <имя> [кол-во] — проверка любого моба
                const bool ruMob = (config_.language == "rus");
                if (!op) { needOp(); break; }
                if (args.size() < 2) {
                    player->sendSystemMessage(ruMob
                        ? std::format("§e/mob <��мя> [кол-во], типов всего: {} — например /mob zombie 3", entity::mobTypeCount())
                        : std::format("§e/mob <name> [count], types: {} — e.g. /mob zombie 3", entity::mobTypeCount()));
                } else {
                    const i32 mobIdx = entity::mobIndexByName(args[1].c_str());
                    if (mobIdx < 0) {
                        player->sendSystemMessage(ruMob
                            ? std::format("§cНет такого моба: {}", args[1])
                            : std::format("§cUnknown mob: {}", args[1]));
                    } else {
                        i32 mobCnt = 1;
                        if (args.size() >= 3) {
                            try { mobCnt = std::clamp(std::stoi(args[2]), 1, 20); } catch (...) { mobCnt = 1; }
                        }
                        spawnMobAt(mobIdx, player->getX(), player->getY(), player->getZ(), player->dimension, mobCnt);
                        player->sendSystemMessage(ruMob
                            ? std::format("§aСпавн: {} x{}", entity::mobDef(mobIdx).name, mobCnt)
                            : std::format("§aSpawned: {} x{}", entity::mobDef(mobIdx).name, mobCnt));
                    }
                }
            } else if (cmd == "locate") { // STRUCT_LOCATE_V1: проверка, что структуры есть, и куда идти
                const bool ruLang = (config_.language == "rus");
                if (args.size() < 2) {
                    player->sendSystemMessage((ruLang ? std::string("§e/locate <тип>: ") : std::string("§e/locate <type>: ")) + world::World::structureKeys());
                } else if (player->dimension != 0) {
                    player->sendSystemMessage(ruLang ? "§cСтруктуры пока есть т��лько в оверворлде" : "§cStructures exist only in the overworld for now");
                } else {
                    auto& lw = worldFor(player->dimension);
                    const i32 pcx = (i32)std::floor(player->getX() / 16.0);
                    const i32 pcz = (i32)std::floor(player->getZ() / 16.0);
                    i32 fx = 0, fz = 0;
                    std::string nru, nen;
                    if (lw.locateStructure(args[1], pcx, pcz, 256, fx, fz, nru, nen)) {
                        const f64 ddx = (f64)fx - player->getX();
                        const f64 ddz = (f64)fz - player->getZ();
                        const f64 dist = std::sqrt(ddx * ddx + ddz * ddz);
                        if (ruLang) player->sendSystemMessage(std::format("§a{}: X {} Z {} — {:.0f} блоков", nru, fx, fz, dist));
                        else        player->sendSystemMessage(std::format("§a{}: X {} Z {} ({:.0f} blocks)", nen, fx, fz, dist));
                    } else {
                        player->sendSystemMessage((ruLang ? std::string("§cНе нашлось в радиусе 256 чанков. Типы: ") : std::string("§cNothing within 256 chunks. Types: ")) + world::World::structureKeys());
                    }
                }
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
                    applyGameMode(target, mode); // CONSOLE_V3 + PERMLEVEL_V2: общая логика смены режима
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
            } else if (cmd == "nether" || cmd == "end" || cmd == "overworld") { // MULTIWORLD_V1
                if (!op) { needOp(); break; }
                const i32 target = (cmd == "nether") ? 1 : (cmd == "end") ? 2 : 0;
                // DIMTOGGLE_V1: мир выключен в конфиге — честно говорим об этом.
                if ((target == 1 && !config_.enableNether) || (target == 2 && !config_.enableEnd)) {
                    const bool rl = (player->clientLocale.rfind("ru", 0) == 0);
                    player->sendSystemMessage(rl
                        ? std::format("§cМир «{}» выключен на сервере (enable-{}=false в settings.properties)",
                                      target == 1 ? "Ад" : "Энд", target == 1 ? "nether" : "end")
                        : std::format("§cThe {} is disabled on this server (enable-{}=false in settings.properties)",
                                      target == 1 ? "Nether" : "End", target == 1 ? "nether" : "end"));
                    break;
                }
                if (player->dimension == target) {
                    player->sendSystemMessage("§eТы уже в этом измерении");
                    break;
                }
                f64 tx = player->getX(), tz = player->getZ(), ty = 5.0;
                if (target == 1 && player->dimension == 0) { tx /= 8.0; tz /= 8.0; }          // 1:8 как в ванилле
                else if (target == 0 && player->dimension == 1) { tx *= 8.0; tz *= 8.0; }
                if (target == 2) { tx = 0.5; tz = 0.5; }                                      // Энд: платформа у нуля
                if (target == 0) { // обычный мир может быть не плоским — ищем поверхность
                    i32 sxi = static_cast<i32>(std::floor(tx)), szi = static_cast<i32>(std::floor(tz));
                    auto scanSurface = [&](i32 bx, i32 bz) {
                        worldFor(0).getOrGenerateChunk(bx >> 4, bz >> 4); // VOIDFIX_V1: без этого скан читал пустой чанк
                        for (i32 yy = 250; yy > world::CHUNK_HEIGHT_MIN; --yy)
                            if (worldFor(0).getBlock(bx, yy, bz) != 0) return yy + 1;
                        return -1;
                    };
                    i32 sy = scanSurface(sxi, szi);
                    if (sy < 0) { // ничего твёрдого — возвращаем на спавн, а не в бездну
                        sxi = g_spawnX; szi = g_spawnZ;
                        tx = static_cast<f64>(g_spawnX) + 0.5; tz = static_cast<f64>(g_spawnZ) + 0.5;
                        sy = scanSurface(sxi, szi);
                        if (sy < 0) sy = g_spawnY;
                    }
                    ty = static_cast<f64>(sy);
                }
                if (travelToDimension(player, target, tx, ty, tz)) {
                    player->sendSystemMessage(std::format("§aИзмерение: {} ({:.0f}, {:.0f}, {:.0f})",
                        target == 1 ? "Ад" : target == 2 ? "Энд" : "Обычный мир", tx, ty, tz));
                } else {
                    player->sendSystemMessage("§cНе удалось сменить измерение");
                }
            } else if (cmd == "warprandomtick") { // RANDOMTICK_WARP_V1: тестовый хук для QA.
                // Мгновенно прогоняет N проходов tickRandomBlockUpdates() подряд,
                // без ожидания реального времени. Не трогает остальную часть
                // tick() (физику, сущностей, огонь, день/ночь) и не имеет отношения к
                // редстоуну/хорусу (они тут вообще не затрагиваются) — только растения/
                // опоры растений, то есть те же 3 случайные клетки на чанк-колонну, что и в обычном
                // тике, просто много раз подряд. Сделано для smoke_bot.py, чтобы можно было
                // честно пров��рять бамбук/естественный рост урожая без 27-минутного ожидания.
                if (!op) { needOp(); break; }
                i64 n = -1;
                if (args.size() >= 2) { try { n = std::stoll(args[1]); } catch (...) { n = -1; } }
                if (n <= 0) player->sendSystemMessage("§cИспользование: /warprandomtick <кол-во проходов, макс 1000>");
                else {
                    n = std::min<i64>(n, 1000);
                    // Packet handlers run on connection threads, while the world is
                    // owned by the tick thread.  Running this loop here races the
                    // normal tick (and simultaneous QA commands), corrupting the
                    // world/RNG state.  Queue it so processConsoleCommands() runs
                    // it serially on the one thread allowed to mutate the world.
                    queueConsoleCommand(std::format("warprandomtick {}", n));
                    player->sendSystemMessage(std::format("§aСлучайные тики поставлены в очередь: {}", n));
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
                            p->kick("§cВы были кикнуты с сервера"); // KICKFIX_V1: настоящий Disconnect, а не тихий close()
                            found = true;
                        }
                    }
                    player->sendSystemMessage(found ? "§aИгрок кикнут" : "§cИгрок не найден");
                } else player->sendSystemMessage("§cИспользов��ние: /kick <ник>");
            } else if (cmd == "summon") { // ENTITIES_V1: заспавнить не-игроковую сущность
                if (!op) { needOp(); break; }
                i32 typeId = -1;
                if (args.size() >= 2) {
                    static const std::unordered_map<std::string, i32> entityTypes = {
                        {"pig", 77}, {"cow", 22}, {"sheep", 87}, {"zombie", 124},   // ENTITY_ID_FIX_V2: сверено с netId в mobs.gen.hpp
                        {"skeleton", 91}, {"creeper", 23}, {"tnt", 106}                // 1.21.1 registry ids (ENTITY_ID_FIX_V2)
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
                    if (typeId == 106) { // ENTITY_ID_FIX_V2: tnt
                        spawnPrimedTnt(sx, baseY, sz, static_cast<i32>(player->getEntityId()), 80);
                        continue;
                    }
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
            } else if (cmd == "give") { // GIVECMD_V1: /give [ник] <id|имя> [кол-во]
                if (!op) { needOp(); break; }
                std::shared_ptr<entity::Player> target = player;
                std::size_t ai = 1;
                if (args.size() >= 3) {
                    for (auto& cand : getAllPlayersCopy())
                        if (cand && cand->getName() == args[1]) { target = cand; ai = 2; break; }
                }
                if (args.size() <= ai) {
                    player->sendSystemMessage("§cИспользование: /give [ник] <id|имя> [кол-во]");
                    break;
                }
                const i32 giveId = nc::gen::itemIdByName(args[ai]);
                if (giveId <= 0) {
                    player->sendSystemMessage(std::format("§cНеизвестный предмет: {}", args[ai]));
                    break;
                }
                i32 giveCount = 1;
                if (args.size() > ai + 1) { try { giveCount = static_cast<i32>(std::stol(args[ai + 1])); } catch (...) {} }
                giveCount = std::clamp(giveCount, 1, 6400);
                const i32 gave = giveItemToPlayer(target, giveId, giveCount);
                const std::string giveName = nc::gen::itemNameById(giveId);
                if (gave <= 0) {
                    player->sendSystemMessage("§cИнвентарь полон");
                } else {
                    player->sendSystemMessage(std::format("§aВыдано: {} x {} (id {}) -> {}", giveName, gave, giveId, target->getName()));
                    if (target != player)
                        target->sendSystemMessage(std::format("§aВы получили: {} x {}", giveName, gave));
                    if (gave < giveCount)
                        player->sendSystemMessage(std::format("§eНе поместилось: {}", giveCount - gave));
                }
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
            } else if (auto* pc = nc::cmd::CommandRegistry::instance().find(cmd); pc && pc->handler) {
                // PLUGINCMD_V1: generic fallback for commands core doesn't hardcode —
                // this is the hook that lets a plugin (donate, vip, whatever) work
                // without ever touching this switch statement.
                if (pc->opOnly && !op) { needOp(); }
                else {
                    nc::cmd::CommandContext ctx;
                    ctx.playerName = player->getName();
                    ctx.isConsole = false;
                    ctx.isOp = op;
                    ctx.args = args;
                    ctx.reply = [&player](const std::string& msg) { player->sendSystemMessage(msg); };
                    setCrashContext(pc->source, "/" + cmd, player->getName());
                    pc->handler(ctx);
                }
            } else {
                player->sendSystemMessage("§cНеизвестная команда — /help");
            }
            clearCrashContext(); // CRASHCTX_V1: command finished without crashing, nothing to blame it for anymore
            break;
        }
        case 0x24: { // BLOCKS_V1: Player Action (копание/ломание)
            i32 status = data.readVarInt();
            u64 posRaw = data.readU64();
            (void)data.readByte(); // face
            i32 seq = data.readVarInt();
            i32 bx, by, bz;
            decodeBlockPos(posRaw, bx, by, bz);
            // BLOCKACK_V2: acknowledge after the block updates below, not before them.
            // A premature ack lets the client briefly render its prediction (half-door / old
            // state) before the authoritative 0x09 packets arrive.
            if ((status == 0 || status == 1 || status == 2) && isMiniEditWandHeld(player)) {
                if (status == 0) {
                    const auto selection = miniEdit_.setPosition(player->getEntityId(), 1, BlockPos{bx, by, bz});
                    player->sendSystemMessage(std::format("§dMiniEdit pos1: {} {} {}{}", bx, by, bz,
                        selection.complete() ? std::format(" ({} блоков)", selection.volume()) : std::string()));
                    if (selection.complete()) sendMiniEditOutline(player, selection);
                }
                // Cancel both creative instant-break and survival finish-break.
                net::Buffer restore;
                restore.writePosition(BlockPos{bx, by, bz});
                restore.writeVarInt(world_.getBlock(bx, by, bz));
                player->getConnection()->sendPacket(0x09,
                    std::vector<u8>(restore.writtenSpan().begin(), restore.writtenSpan().end()));
                net::Buffer ack; ack.writeVarInt(seq);
                player->getConnection()->sendPacket(0x05,
                    std::vector<u8>(ack.writtenSpan().begin(), ack.writtenSpan().end()));
                break;
            }
            if (status == 5 && player->usingShield) { // SHIELD_V1: Release Use Item — щит опущен
                player->usingShield = false;
                broadcastHandState(player);
            }
            if (status == 3 || status == 4) { // ITEMDROP_V1: Q (4) / Ctrl+Q (3) — выброс предмета из руки
                const i32 slot = 36 + player->heldSlot;
                if (player->heldSlot < 9 && player->invCount[slot] > 0 && player->invItemId[slot] > 0) {
                    const i32 throwCnt = (status == 3) ? player->invCount[slot] : 1;
                    const i32 throwId = player->invItemId[slot];
                    player->invCount[slot] -= throwCnt;
                    if (player->invCount[slot] <= 0) { player->invCount[slot] = 0; player->invItemId[slot] = 0; player->hotbarBlockState[player->heldSlot] = -1; }
                    const f64 yawR = static_cast<f64>(player->get_yaw()) * 0.017453292519943295;
                    const f64 pitR = static_cast<f64>(player->get_pitch()) * 0.017453292519943295;
                    // бросаем по взгляду; задержка подбора 40 тиков — чтобы не всосать обратно мгновенно
                    spawnItemDrop(player->getX(), player->getY() + 1.3, player->getZ(), throwId, throwCnt,
                        -std::sin(yawR) * std::cos(pitR) * 0.3, -std::sin(pitR) * 0.3 + 0.1, std::cos(yawR) * std::cos(pitR) * 0.3, 40);
                }
            }
            if (status == 6) { // ALLPACKETS_V2: F - swap the main hand with the off hand
                const i32 mainSlot = 36 + std::clamp(player->heldSlot, 0, 8);
                std::swap(player->invItemId[mainSlot], player->invItemId[45]);
                std::swap(player->invCount[mainSlot], player->invCount[45]);
                if (player->heldSlot >= 0 && player->heldSlot < 9) player->hotbarBlockState[player->heldSlot] = -1;
                for (const i32 sSlot : {mainSlot, 45}) {
                    net::Buffer sw; sw.writeByte(0); sw.writeVarInt(0); sw.writeI16(static_cast<i16>(sSlot));
                    sw.writeVarInt(player->invCount[sSlot]);
                    if (player->invCount[sSlot] > 0) { sw.writeVarInt(player->invItemId[sSlot]); sw.writeVarInt(0); sw.writeVarInt(0); }
                    player->getConnection()->sendPacket(0x15, std::vector<u8>(sw.writtenSpan().begin(), sw.writtenSpan().end()));
                }
                broadcastHeldEquipment(player); // others must see the new item in hand
            }
            const bool creative = (player->gameMode == 1); // GM_V1: ��о-игроковому, а не из конфига
            const bool wantsBreak = player->gameMode != 3 && ((creative && status == 0) || (!creative && status == 2));
            if (wantsBreak && by >= world::CHUNK_HEIGHT_MIN && by < world::CHUNK_HEIGHT_MAX) { // HEIGHT_V2: vanilla world is -64..319
                const i32 ax = bx < 0 ? -bx : bx;
                const i32 az = bz < 0 ? -bz : bz;
                const bool prot = config_.spawnProtection > 0 && player->dimension == 0 && ax <= config_.spawnProtection && az <= config_.spawnProtection && !isOpName(config_.ops, player->getName()); // OPS_V1
                if (prot) {
                    net::Buffer bu;
                    bu.writePosition(BlockPos{bx, by, bz});
                    bu.writeVarInt(world_.getBlock(bx, by, bz)); // откатываем на клиенте
                    player->getConnection()->sendPacket(0x09, std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end()));
                    player->sendSystemMessage("§cСпавн защищён — здесь ломать нельзя");
                } else if (by == world::CHUNK_HEIGHT_MIN && world_.getBlock(bx, by, bz) == 79) {
                    // BEDROCK_PROTECT_V1: exact lower border cannot be mined in survival or creative.
                    net::Buffer bu;
                    bu.writePosition(BlockPos{bx, by, bz});
                    bu.writeVarInt(79);
                    player->getConnection()->sendPacket(0x09, std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end()));
                    player->sendSystemMessage(config_.language == "rus" ? "§cНижний бедрок на высоте -64 нельзя сломать" : "��cThe bottom bedrock at Y=-64 cannot be broken");
                } else {
                    const i32 oldState = world_.getBlock(bx, by, bz); // BREAKFX_V1
                    if (oldState == 2095) tntCountDecrement(tntBlockCount_); // ANTILAG_TNT_V1: stationary TNT broken
                    world_.setBlock(bx, by, bz, 0);
                    { // PLACE_V2: a door/tall plant/bed is two blocks - remove the other half too,
                      // instead of leaving a floating top half behind.
                        i32 hdx = 0, hdy = 0, hdz = 0;
                        const i32 exactDoorPartner = gen::doorPartnerState(oldState); // DOORS_V2
                        const bool isExactDoor = exactDoorPartner >= 0;
                        if (isExactDoor) {
                            // The known lower IDs are above their upper IDs in 1.21.1.
                            hdy = exactDoorPartner < oldState ? 1 : -1;
                        }
                        if (isExactDoor || doubleBlockPartner(oldState, hdx, hdy, hdz)) {
                            const i32 px = bx + hdx, py = by + hdy, pz = bz + hdz;
                            const i32 partner = world_.getBlock(px, py, pz);
                            if (partner > 0 && (isExactDoor ? partner == exactDoorPartner
                                                         : stateNameOf(partner) == stateNameOf(oldState))) {
                                world_.setBlock(px, py, pz, 0);
                                net::Buffer hb; hb.writePosition(BlockPos{px, py, pz}); hb.writeVarInt(0);
                                const auto hbv = std::vector<u8>(hb.writtenSpan().begin(), hb.writtenSpan().end());
                                for (auto& pl : getAllPlayersCopy())
                                    if (pl && pl->isAlive() && pl->getState() == entity::PlayerState::Play)
                                        pl->getConnection()->sendPacket(0x09, hbv);
                            }
                        }
                    }
                    extinguishPortalNear(player->dimension, bx, by, bz, oldState); // PORTALBREAK_V1
                    scheduleFluidNeighbors(bx, by, bz); // FLUID_V1: сломали блок — соседние жидкости могут потечь
                    scheduleFallingBlockUpdate(bx, by + 1, bz, 2); // FALLING_V1: освободили опору
                    scheduleFallingColumnCascade(bx, by + 1, bz, 2); // FALLING_CHAIN_V3
                    { // SUPPORT_BREAK_V1: breaking the soil under a planted crop must break the
                      // crop too (bug report: crops kept floating in place after their farmland
                      // or soul sand was mined out from under them).
                        const i32 abovePos = world_.getBlock(bx, by + 1, bz);
                        struct SupportCrop { i32 base; i32 maxAge; i32 seedItem; i32 productItem; };
                        static const SupportCrop kSupportCrops[] = {
                            {4278, 7, 853, 854},    // wheat
                            {8595, 7, 1097, 1097},  // carrots
                            {8603, 7, 1098, 1098},  // potatoes
                            {12509, 3, 1155, 1154}, // beetroots
                        };
                        bool supportBroke = false;
                        for (const auto& c : kSupportCrops) {
                            if (abovePos < c.base || abovePos > c.base + c.maxAge) continue;
                            const i32 age = abovePos - c.base;
                            world_.setBlock(bx, by + 1, bz, 0);
                            supportBroke = true;
                            if (age >= c.maxAge) {
                                spawnItemDrop(bx + 0.5, by + 1.1, bz + 0.5, c.productItem, 1, 0.0, 0.15, 0.0);
                                spawnItemDrop(bx + 0.5, by + 1.1, bz + 0.5, c.seedItem, 1 + (std::rand() % 4), 0.0, 0.15, 0.0);
                            } else {
                                spawnItemDrop(bx + 0.5, by + 1.1, bz + 0.5, c.seedItem, 1, 0.0, 0.15, 0.0);
                            }
                            break;
                        }
                        if (!supportBroke && abovePos >= 7385 && abovePos <= 7388) { // nether_wart
                            const i32 age = abovePos - 7385;
                            world_.setBlock(bx, by + 1, bz, 0);
                            supportBroke = true;
                            const i32 count = (age >= 3) ? (2 + std::rand() % 3) : 1;
                            spawnItemDrop(bx + 0.5, by + 1.1, bz + 0.5, 997, count, 0.0, 0.15, 0.0);
                        }
                        if (!supportBroke) {
                            const auto* aboveBs = registries::RegistryManager::instance().blockStates().getById(abovePos);
                            if (aboveBs) {
                                const std::string_view an = aboveBs->name;
                                if (an == "minecraft:pumpkin_stem" || an == "minecraft:melon_stem" ||
                                    an == "minecraft:attached_pumpkin_stem" || an == "minecraft:attached_melon_stem" ||
                                    an == "minecraft:torchflower_crop") {
                                    world_.setBlock(bx, by + 1, bz, 0);
                                    supportBroke = true;
                                }
                            }
                        }
                        if (supportBroke) {
                            net::Buffer scb; scb.writePosition(BlockPos{bx, by + 1, bz}); scb.writeVarInt(0);
                            const auto scbBytes = std::vector<u8>(scb.writtenSpan().begin(), scb.writtenSpan().end());
                            for (auto& pl : getAllPlayersCopy())
                                if (pl && pl->isAlive() && pl->getState() == entity::PlayerState::Play)
                                    pl->getConnection()->sendPacket(0x09, scbBytes);
                        }
                    }
                    net::Buffer bu;
                    bu.writePosition(BlockPos{bx, by, bz});
                    bu.writeVarInt(0);
                    auto vec = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
                    auto allPlayers = getAllPlayersCopy();
                    for (auto& p : allPlayers) if (p->isAlive()) p->getConnection()->sendPacket(0x09, vec);
                    const bool fxSilent = isParticlelessBreakState(oldState); // PARTICLE_V2
                    if (fxSilent && isFireBlockState(oldState)) {
                        // Vanilla: putting out fire is Level Event 1009 (extinguish puff + hiss),
                        // not the block-break effect that showered "fire" particles everywhere.
                        net::Buffer fe;
                        fe.writeI32(1009);
                        fe.writePosition(BlockPos{bx, by, bz});
                        fe.writeI32(0);
                        fe.writeBool(false);
                        auto fev = std::vector<u8>(fe.writtenSpan().begin(), fe.writtenSpan().end());
                        for (auto& p : allPlayers) if (p->isAlive()) p->getConnection()->sendPacket(0x28, fev);
                    }
                    if (oldState > 0 && !fxSilent) { // BREAKFX_V1: частицы + звук ломания (Level Event 2001)
                        net::Buffer fx;
                        fx.writeI32(2001);
                        fx.writePosition(BlockPos{bx, by, bz});
                        fx.writeI32(oldState);
                        fx.writeBool(false);
                        auto fxv = std::vector<u8>(fx.writtenSpan().begin(), fx.writtenSpan().end());
                        for (auto& p : allPlayers) if (p->isAlive()) p->getConnection()->sendPacket(0x28, fxv);
                    }
                    { // COCOA_BREAK_DROP_V1: mined cocoa pods drop 1 bean if unripe (age 0/1) or
                      // 3 beans once fully ripe (age 2), matching vanilla CocoaBlock loot table.
                        const auto* cocoaBs = registries::RegistryManager::instance().blockStates().getById(oldState);
                        if (cocoaBs && cocoaBs->name == "minecraft:cocoa") {
                            auto ageIt2 = cocoaBs->properties.find("age");
                            const i32 cocoaAge = ageIt2 != cocoaBs->properties.end() ? std::atoi(ageIt2->second.c_str()) : 0;
                            const i32 beanCount = cocoaAge >= 2 ? 3 : 1;
                            spawnItemDrop(bx + 0.5, by + 0.5, bz + 0.5, 921, beanCount, 0.0, 0.2, 0.0);
                        }
                    }
                    if (!creative && oldState > 0) { // ITEMDROP_V1: в выживании сломанный блок выпадает предметом
                        // DOORS_V3: the upper door half has no item mapping. Its partner is
                        // the lower state that does map to the door item, so breaking either
                        // half drops exactly one door in survival.
                        i32 dropId = stateToItem(oldState);
                        if (dropId <= 0) {
                            const i32 lowerDoor = gen::doorPartnerState(oldState);
                            if (lowerDoor > oldState) dropId = stateToItem(lowerDoor);
                        }
                        const bool isCocoaState = oldState >= 7419 && oldState <= 7430; // COCOA_BREAK_DROP_V1 already handled this above
                        if (dropId > 0 && !isCocoaState) {
                            const f64 jx = static_cast<f64>((bx * 73 + bz * 31 + by * 17) % 21 - 10) / 100.0; // лёгкий разброс
                            const f64 jz = static_cast<f64>((bx * 31 + bz * 73 + by * 41) % 21 - 10) / 100.0;
                            spawnItemDrop(bx + 0.5 + jx, by + 0.5, bz + 0.5 + jz, dropId, 1, jx, 0.2, jz);
                        }
                    }
                    if (isChestBlockState(oldState) || isTrappedChestState(oldState) || isEnderChestState(oldState)) { // CHEST_V1/CHEST_V2: сундук сломан — чистим к��нтейнер и закрываем окна зрителей (в т.ч. эндер-сундук)
                        const u64 key = chestPosKey(bx, by, bz);
                        const bool wasEnder = isEnderChestState(oldState);
                        if (!wasEnder) { // ITEMDROP_V1: содержимое сундука высыпается на пол
                            ChestData spilled{};
                            { std::lock_guard lock(chestsMutex_); auto itc = chests_.find(key); if (itc != chests_.end()) spilled = itc->second; chests_.erase(key); }
                            for (i32 ci = 0; ci < 27; ++ci)
                                if (spilled.count[ci] > 0 && spilled.itemId[ci] > 0)
                                    spawnItemDrop(bx + 0.5, by + 0.5, bz + 0.5, spilled.itemId[ci], spilled.count[ci],
                                        static_cast<f64>(ci % 5 - 2) * 0.04, 0.15, static_cast<f64>(ci / 5 - 2) * 0.04);
                        }
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
                        if (!wasEnder) { // CHEST_V2: если сломана половина двойного сундука — партнёр снова одиночный
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
            // только к окну игрока (windowId 0) в выживании, режим normal-click (mode 0).
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
            const i32 useHand = data.readVarInt(); // рука
            u64 posRaw = data.readU64();
            i32 face = data.readVarInt();
            (void)data.readF32(); (void)data.readF32(); (void)data.readF32(); // курсор
            (void)data.readBool(); // внутри блока
            i32 seq = data.readVarInt();
            i32 bx, by, bz;
            decodeBlockPos(posRaw, bx, by, bz);
            if (useHand == 0 && isMiniEditWandHeld(player)) {
                const auto selection = miniEdit_.setPosition(player->getEntityId(), 2, BlockPos{bx, by, bz});
                player->sendSystemMessage(std::format("§dMiniEdit pos2: {} {} {}{}", bx, by, bz,
                    selection.complete() ? std::format(" ({} блоков)", selection.volume()) : std::string()));
                if (selection.complete()) sendMiniEditOutline(player, selection);
                net::Buffer ack; ack.writeVarInt(seq);
                player->getConnection()->sendPacket(0x05,
                    std::vector<u8>(ack.writtenSpan().begin(), ack.writtenSpan().end()));
                break;
            }
            { // DOORS_V4: vanilla wooden door interaction. Earlier patches only placed
              // both halves; right-click still fell through to placement and never toggled it.
                const i32 doorState = world_.getBlock(bx, by, bz);
                if (gen::isWoodenDoorState(doorState) && player->gameMode != 3 && !player->sneaking) {
                    const i32 otherState = gen::doorPartnerState(doorState);
                    const i32 toggled = gen::doorToggleOpenState(doorState);
                    const i32 otherToggled = gen::doorToggleOpenState(otherState);
                    if (otherState >= 0 && toggled >= 0 && otherToggled >= 0) {
                        const bool clickedUpper = ((doorState - gen::doorStateRange(doorState)->first) & 15) < 8;
                        const i32 ox = bx, oy = by + (clickedUpper ? 1 : -1), oz = bz;
                        if (world_.getBlock(ox, oy, oz) == otherState) {
                            world_.setBlock(bx, by, bz, toggled);
                            world_.setBlock(ox, oy, oz, otherToggled);
                            auto players = getAllPlayersCopy();
                            for (const BlockPos q : {BlockPos{bx, by, bz}, BlockPos{ox, oy, oz}}) {
                                net::Buffer db; db.writePosition(q); db.writeVarInt(world_.getBlock(q.x, q.y, q.z));
                                const auto dv = std::vector<u8>(db.writtenSpan().begin(), db.writtenSpan().end());
                                for (auto& p : players) if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                                    p->getConnection()->sendPacket(0x09, dv);
                            }
                            broadcastBlockSound("minecraft:block.wooden_door.open", bx, by, bz, 1.0f, 1.0f);
                            net::Buffer da; da.writeVarInt(seq);
                            player->getConnection()->sendPacket(0x05, std::vector<u8>(da.writtenSpan().begin(), da.writtenSpan().end()));
                            break;
                        }
                    }
                }
            }
            { // VEHICLE_PHYSICS_V1: boat/minecart items spawn entities, not blocks
                const i32 vehHandSlot = useHand == 1 ? 45 : (36 + std::clamp(player->heldSlot, 0, 8));
                const i32 vehItem = player->invCount[vehHandSlot] > 0 ? player->invItemId[vehHandSlot] : 0;
                if (vehItem != 0 && placeVehicleItem(player, vehItem, bx, by, bz, face)) {
                    if (player->gameMode != 1 && --player->invCount[vehHandSlot] <= 0) {
                        player->invCount[vehHandSlot] = 0; player->invItemId[vehHandSlot] = 0;
                        if (vehHandSlot >= 36 && vehHandSlot <= 44) player->hotbarBlockState[vehHandSlot - 36] = -1;
                    }
                    net::Buffer vslot; vslot.writeByte(0); vslot.writeVarInt(0); vslot.writeI16(static_cast<i16>(vehHandSlot));
                    vslot.writeVarInt(player->invCount[vehHandSlot]);
                    if (player->invCount[vehHandSlot] > 0) { vslot.writeVarInt(player->invItemId[vehHandSlot]); vslot.writeVarInt(0); vslot.writeVarInt(0); }
                    player->getConnection()->sendPacket(0x15, std::vector<u8>(vslot.writtenSpan().begin(), vslot.writtenSpan().end()));
                    net::Buffer vack; vack.writeVarInt(seq);
                    player->getConnection()->sendPacket(0x05, std::vector<u8>(vack.writtenSpan().begin(), vack.writtenSpan().end()));
                    break;
                }
            }
            { // MOBS_ALL_V1: яйца спавна — ��аботают для всех мобов из таблицы
                const i32 eggHandSlot = useHand == 1 ? 45 : (36 + std::clamp(player->heldSlot, 0, 8));
                const i32 eggItem = player->invCount[eggHandSlot] > 0 ? player->invItemId[eggHandSlot] : 0;
                const i32 eggMob = entity::mobIndexBySpawnEgg(eggItem);
                if (eggMob >= 0) {
                    i32 sx = bx, sy = by, sz = bz;
                    switch (face) {
                        case 0: --sy; break;   case 1: ++sy; break;
                        case 2: --sz; break;   case 3: ++sz; break;
                        case 4: --sx; break;   default: ++sx; break;
                    }
                    spawnMobAt(eggMob, sx + 0.5, static_cast<f64>(sy), sz + 0.5, player->dimension, 1);
                    if (player->gameMode != 1 && --player->invCount[eggHandSlot] <= 0) {
                        player->invCount[eggHandSlot] = 0; player->invItemId[eggHandSlot] = 0;
                        if (eggHandSlot >= 36 && eggHandSlot <= 44) player->hotbarBlockState[eggHandSlot - 36] = -1;
                    }
                    net::Buffer eslot; eslot.writeByte(0); eslot.writeVarInt(0); eslot.writeI16(static_cast<i16>(eggHandSlot));
                    eslot.writeVarInt(player->invCount[eggHandSlot]);
                    if (player->invCount[eggHandSlot] > 0) { eslot.writeVarInt(player->invItemId[eggHandSlot]); eslot.writeVarInt(0); eslot.writeVarInt(0); }
                    player->getConnection()->sendPacket(0x15, std::vector<u8>(eslot.writtenSpan().begin(), eslot.writtenSpan().end()));
                    net::Buffer eack; eack.writeVarInt(seq);
                    player->getConnection()->sendPacket(0x05, std::vector<u8>(eack.writtenSpan().begin(), eack.writtenSpan().end()));
                    break;
                }
            }
            { // PORTAL_V2 / ENDPORTAL_V1: огниво зажигает портал, око Эндера вставляется в рамку
                const i32 ptHandSlot = useHand == 1 ? 45 : (36 + std::clamp(player->heldSlot, 0, 8));
                const i32 ptItem = player->invCount[ptHandSlot] > 0 ? player->invItemId[ptHandSlot] : 0;
                bool ptDone = false, ptConsume = false;
                const bool rlPt = (player->clientLocale.rfind("ru", 0) == 0); // DIMTOGGLE_V1
                // DIMTOGGLE_V2: warn only when a portal is really being built:
                // an eye into an End frame, or flint and steel on obsidian.
                // Setting grass on fire has nothing to do with the Nether.
                const i32 ptClicked = worldFor(player->dimension).getBlock(bx, by, bz);
                const bool ptEndFrame = (ptClicked == 7411); // end_portal_frame
                const bool ptObsidian = (ptClicked == 2354); // obsidian
                i32 ptFx = bx, ptFy = by, ptFz = bz; // DIMTOGGLE_V3: where the fire would go
                switch (face) {
                    case 0: --ptFy; break;   case 1: ++ptFy; break;
                    case 2: --ptFz; break;   case 3: ++ptFz; break;
                    case 4: --ptFx; break;   default: ++ptFx; break;
                }
                // Complain about the disabled Nether only when this click really would
                // have lit a portal, i.e. a valid obsidian frame is standing here.
                const bool ptDeadFrame = (ptItem == 798 && ptObsidian && !config_.enableNether) &&
                                         tryLightNetherPortal(player->dimension, ptFx, ptFy, ptFz, true);
                if (ptItem == 1006 && ptEndFrame && !config_.enableEnd) {
                    player->sendSystemMessage(rlPt ? "§cМир Энда выключен на сервере — портал не заработает"
                                                   : "§cThe End is disabled on this server - this portal will never work");
                } else if (ptDeadFrame) {
                    player->sendSystemMessage(rlPt ? "§cМир Ада выключен на сервере — портал не зажечь"
                                                   : "§cThe Nether is disabled on this server - the portal will not light");
                } else if (ptItem == 1006 && config_.enableEnd) { // ender_eye
                    ptDone = tryPlaceEnderEye(player->dimension, bx, by, bz);
                    ptConsume = ptDone;
                } else if (ptItem == 798 && config_.enableNether) { // flint_and_steel
                    i32 fx = bx, fy = by, fz = bz;
                    switch (face) {
                        case 0: --fy; break;   case 1: ++fy; break;
                        case 2: --fz; break;   case 3: ++fz; break;
                        case 4: --fx; break;   default: ++fx; break;
                    }
                    ptDone = tryLightNetherPortal(player->dimension, fx, fy, fz);
                }
                if (ptDone) {
                    if (ptConsume && player->gameMode != 1 && --player->invCount[ptHandSlot] <= 0) {
                        player->invCount[ptHandSlot] = 0; player->invItemId[ptHandSlot] = 0;
                        if (ptHandSlot >= 36 && ptHandSlot <= 44) player->hotbarBlockState[ptHandSlot - 36] = -1;
                    }
                    net::Buffer pslot; pslot.writeByte(0); pslot.writeVarInt(0); pslot.writeI16(static_cast<i16>(ptHandSlot));
                    pslot.writeVarInt(player->invCount[ptHandSlot]);
                    if (player->invCount[ptHandSlot] > 0) { pslot.writeVarInt(player->invItemId[ptHandSlot]); pslot.writeVarInt(0); pslot.writeVarInt(0); }
                    player->getConnection()->sendPacket(0x15, std::vector<u8>(pslot.writtenSpan().begin(), pslot.writtenSpan().end()));
                    net::Buffer pack; pack.writeVarInt(seq);
                    player->getConnection()->sendPacket(0x05, std::vector<u8>(pack.writtenSpan().begin(), pack.writtenSpan().end()));
                    break;
                }
            }
            { // BUCKET_REUSE_V2: Use Item On may handle both source pickup and placement.
                const i32 handSlot = useHand == 1 ? 45 : (36 + std::clamp(player->heldSlot, 0, 8));
                const i32 item = player->invCount[handSlot] > 0 ? player->invItemId[handSlot] : 0;
                if (item >= 908 && item <= 910 && player->gameMode != 3) {
                    i32 tx = bx, ty = by, tz = bz;
                    switch (face) { case 0: --ty; break; case 1: ++ty; break; case 2: --tz; break;
                                    case 3: ++tz; break; case 4: --tx; break; case 5: ++tx; break; default: break; }
                    bool changed = false;
                    if (item == 908 && (world_.getBlock(bx,by,bz) == 80 || world_.getBlock(bx,by,bz) == 96)) {
                        const i32 source = world_.getBlock(bx,by,bz);
                        world_.setBlock(bx,by,bz,0); scheduleFluidNeighbors(bx,by,bz);
                        player->invItemId[handSlot] = source == 80 ? 909 : 910;
                        player->invCount[handSlot] = 1; changed = true;
                        net::Buffer bu; bu.writePosition(BlockPos{bx,by,bz}); bu.writeVarInt(0);
                        const auto bytes=std::vector<u8>(bu.writtenSpan().begin(),bu.writtenSpan().end());
                        for(auto& p:getAllPlayersCopy()) if(p&&p->isAlive()) p->getConnection()->sendPacket(0x09,bytes);
                    } else if ((item == 909 || item == 910) && world_.getBlock(tx,ty,tz) == 0) {
                        const i32 fluid = item == 909 ? 80 : 96;
                        world_.setBlock(tx,ty,tz,fluid); scheduleFluidNeighbors(tx,ty,tz);
                        if (fluid == 80) solidifyConcretePowderAround(tx,ty,tz);
                        if (player->gameMode != 1) { player->invItemId[handSlot]=908; player->invCount[handSlot]=1; }
                        changed = true;
                        net::Buffer bu; bu.writePosition(BlockPos{tx,ty,tz}); bu.writeVarInt(fluid);
                        const auto bytes=std::vector<u8>(bu.writtenSpan().begin(),bu.writtenSpan().end());
                        for(auto& p:getAllPlayersCopy()) if(p&&p->isAlive()) p->getConnection()->sendPacket(0x09,bytes);
                    }
                    if (changed) {
                        if (handSlot >= 36 && handSlot <= 44) {
                            const i32 id=player->invItemId[handSlot];
                            player->hotbarBlockState[handSlot-36]=id==909?80:(id==910?96:-1);
                        }
                        net::Buffer slot; slot.writeByte(0); slot.writeVarInt(0); slot.writeI16(static_cast<i16>(handSlot));
                        slot.writeVarInt(player->invCount[handSlot]);
                        if(player->invCount[handSlot]>0){slot.writeVarInt(player->invItemId[handSlot]);slot.writeVarInt(0);slot.writeVarInt(0);}
                        player->getConnection()->sendPacket(0x15,std::vector<u8>(slot.writtenSpan().begin(),slot.writtenSpan().end()));
                        net::Buffer ab; ab.writeVarInt(seq);
                        player->getConnection()->sendPacket(0x05,std::vector<u8>(ab.writtenSpan().begin(),ab.writtenSpan().end()));
                        break;
                    }
                }
            }
            { // TNT_V1: flint-and-steel / fire charge primes the clicked TNT block.
                const i32 handSlot = useHand == 1 ? 45 : (36 + std::clamp(player->heldSlot, 0, 8));
                const i32 item = player->invCount[handSlot] > 0 ? player->invItemId[handSlot] : 0;
                if (world_.getBlock(bx, by, bz) == 2095 && (item == 798 || item == 1089) && player->gameMode != 3) {
                    const i32 ax = std::abs(bx - g_spawnX), az = std::abs(bz - g_spawnZ);
                    const bool prot = config_.spawnProtection > 0 && player->dimension == 0 && ax <= config_.spawnProtection && az <= config_.spawnProtection && !isOpName(config_.ops, player->getName());
                    if (!prot && primeTntBlock(bx, by, bz, static_cast<i32>(player->getEntityId()), 80)) {
                        if (item == 1089 && player->gameMode != 1 && --player->invCount[handSlot] <= 0) {
                            player->invCount[handSlot] = 0; player->invItemId[handSlot] = 0;
                            if (handSlot >= 36 && handSlot <= 44) player->hotbarBlockState[handSlot - 36] = -1;
                        }
                        net::Buffer slot; slot.writeByte(0); slot.writeVarInt(0); slot.writeI16(static_cast<i16>(handSlot));
                        slot.writeVarInt(player->invCount[handSlot]);
                        if (player->invCount[handSlot] > 0) { slot.writeVarInt(player->invItemId[handSlot]); slot.writeVarInt(0); slot.writeVarInt(0); }
                        player->getConnection()->sendPacket(0x15, std::vector<u8>(slot.writtenSpan().begin(), slot.writtenSpan().end()));
                    }
                    net::Buffer ab; ab.writeVarInt(seq);
                    player->getConnection()->sendPacket(0x05, std::vector<u8>(ab.writtenSpan().begin(), ab.writtenSpan().end()));
                    break;
                }
            }
            { // FIRE_PLACE_V1: non-TNT use places fire on the clicked face.
                const i32 handSlot = useHand == 1 ? 45 : (36 + std::clamp(player->heldSlot, 0, 8));
                const i32 item = player->invCount[handSlot] > 0 ? player->invItemId[handSlot] : 0;
                if ((item == 798 || item == 1089) && player->gameMode != 3) {
                    i32 fx = bx, fy = by, fz = bz;
                    switch (face) { case 0: --fy; break; case 1: ++fy; break; case 2: --fz; break;
                                    case 3: ++fz; break; case 4: --fx; break; case 5: ++fx; break; default: break; }
                    const bool prot = config_.spawnProtection > 0 && player->dimension == 0 &&
                        std::abs(fx - g_spawnX) <= config_.spawnProtection &&
                        std::abs(fz - g_spawnZ) <= config_.spawnProtection &&
                        !isOpName(config_.ops, player->getName());
                    if (!prot && fy >= world::CHUNK_HEIGHT_MIN && fy < world::CHUNK_HEIGHT_MAX &&
                        isFreeForFalling(world_.getBlock(fx, fy, fz))) {
                        const i32 support = world_.getBlock(fx, fy - 1, fz);
                        const i32 fireState = (support == 5850 || support == 5851) ? 2872 : 2391;
                        world_.setBlock(fx, fy, fz, fireState);
                        scheduleFluidNeighbors(fx, fy, fz);
                        scheduleFireUpdate(fx, fy, fz, 1);
                        net::Buffer bu; bu.writePosition(BlockPos{fx, fy, fz}); bu.writeVarInt(fireState);
                        const auto fireBytes = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
                        for (auto& p : getAllPlayersCopy())
                            if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                                p->getConnection()->sendPacket(0x09, fireBytes);
                        broadcastBlockSound("minecraft:item.flintandsteel.use", fx, fy, fz, 1.0f, 1.0f);
                        if (item == 1089 && player->gameMode != 1) {
                            if (--player->invCount[handSlot] <= 0) {
                                player->invCount[handSlot] = 0; player->invItemId[handSlot] = 0;
                                if (handSlot >= 36 && handSlot <= 44) player->hotbarBlockState[handSlot - 36] = -1;
                            }
                            net::Buffer slot; slot.writeByte(0); slot.writeVarInt(0); slot.writeI16(static_cast<i16>(handSlot));
                            slot.writeVarInt(player->invCount[handSlot]);
                            if (player->invCount[handSlot] > 0) { slot.writeVarInt(player->invItemId[handSlot]); slot.writeVarInt(0); slot.writeVarInt(0); }
                            player->getConnection()->sendPacket(0x15, std::vector<u8>(slot.writtenSpan().begin(), slot.writtenSpan().end()));
                        }
                    }
                    net::Buffer ab; ab.writeVarInt(seq);
                    player->getConnection()->sendPacket(0x05, std::vector<u8>(ab.writtenSpan().begin(), ab.writtenSpan().end()));
                    break;
                }
            }
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
            { // BONE_MEAL_V1: bone meal instantly grows supported crops (wheat/carrots/potatoes/beetroots/nether_wart)
                const i32 handSlot = useHand == 1 ? 45 : (36 + std::clamp(player->heldSlot, 0, 8));
                const i32 item = player->invCount[handSlot] > 0 ? player->invItemId[handSlot] : 0;
                if (item == 960 && player->gameMode != 3) { // 960 = minecraft:bone_meal
                    struct AgeCrop { i32 base; i32 maxAge; };
                    static const AgeCrop kCrops[] = {
                        {4278, 7},  // wheat
                        {8595, 7},  // carrots
                        {8603, 7},  // potatoes
                        {12509, 3}, // beetroots
                        {7385, 3},  // nether_wart
                    };
                    const i32 clicked = world_.getBlock(bx, by, bz);
                    i32 newState = -1;
                    for (const auto& c : kCrops) {
                        if (clicked < c.base || clicked > c.base + c.maxAge) continue;
                        const i32 age = clicked - c.base;
                        if (age >= c.maxAge) break; // уже выросло — костная мука не тратится (как в ваниле)
                        const i32 boost = 2 + (std::rand() % 4); // vanilla-style: +2..+5 стадий за раз
                        newState = c.base + std::min(age + boost, c.maxAge);
                        break;
                    }
                    if (newState < 0) { // BONEMEAL_STEM_TORCHFLOWER_V1: pumpkin/melon stems and
                        // torchflower_crop track age via a registry property, not a fixed offset,
                        // so bone meal silently did nothing on them before this fix.
                        const auto* stemBs = registries::RegistryManager::instance().blockStates().getById(clicked);
                        if (stemBs) {
                            const std::string_view sn = stemBs->name;
                            const bool isPumpkinStem = sn == "minecraft:pumpkin_stem";
                            const bool isMelonStem = sn == "minecraft:melon_stem";
                            const bool isTorchflowerCrop = sn == "minecraft:torchflower_crop";
                            if (isPumpkinStem || isMelonStem || isTorchflowerCrop) {
                                auto ageIt = stemBs->properties.find("age");
                                const i32 age = ageIt != stemBs->properties.end() ? std::atoi(ageIt->second.c_str()) : 0;
                                const char* stemName = isTorchflowerCrop ? "minecraft:torchflower_crop"
                                    : (isPumpkinStem ? "minecraft:pumpkin_stem" : "minecraft:melon_stem");
                                static const i32 kPumpkinMaxAgeBM = [] { i32 a = 0; while (registries::RegistryManager::instance().getBlockStateId(
                                    "minecraft:pumpkin_stem", {{"age", std::to_string(a + 1)}}).has_value()) ++a; return a; }();
                                static const i32 kMelonMaxAgeBM = [] { i32 a = 0; while (registries::RegistryManager::instance().getBlockStateId(
                                    "minecraft:melon_stem", {{"age", std::to_string(a + 1)}}).has_value()) ++a; return a; }();
                                static const i32 kTorchflowerMaxAgeBM = [] { i32 a = 0; while (registries::RegistryManager::instance().getBlockStateId(
                                    "minecraft:torchflower_crop", {{"age", std::to_string(a + 1)}}).has_value()) ++a; return a; }();
                                const i32 maxAge = isTorchflowerCrop ? kTorchflowerMaxAgeBM : (isPumpkinStem ? kPumpkinMaxAgeBM : kMelonMaxAgeBM);
                                if (age < maxAge) {
                                    const i32 boost = 2 + (std::rand() % 4);
                                    const i32 grownAge = std::min(age + boost, maxAge);
                                    if (isTorchflowerCrop && grownAge >= maxAge) {
                                        newState = 2076; // matured torchflower flower (see TORCHFLOWER_GROWTH_V1)
                                    } else {
                                        newState = registries::RegistryManager::instance().getBlockStateId(
                                            stemName, {{"age", std::to_string(grownAge)}}).value_or(-1);
                                    }
                                }
                            } else if (sn == "minecraft:sweet_berry_bush") {
                                // SWEET_BERRY_BONEMEAL_V1: same dynamic-maxAge pattern as the stems
                                // above, applied to the bush's own "age" property.
                                auto ageIt = stemBs->properties.find("age");
                                const i32 age = ageIt != stemBs->properties.end() ? std::atoi(ageIt->second.c_str()) : 0;
                                static const i32 kSweetBerryMaxAgeBM = [] { i32 a = 0; while (registries::RegistryManager::instance().getBlockStateId(
                                    "minecraft:sweet_berry_bush", {{"age", std::to_string(a + 1)}}).has_value()) ++a; return a; }();
                                if (age < kSweetBerryMaxAgeBM) {
                                    const i32 boost = 1 + (std::rand() % 2); // gentler boost fits the bush's short 0..3 age range
                                    const i32 grownAge = std::min(age + boost, kSweetBerryMaxAgeBM);
                                    newState = registries::RegistryManager::instance().getBlockStateId(
                                        "minecraft:sweet_berry_bush", {{"age", std::to_string(grownAge)}}).value_or(-1);
                                }
                            }
                        }
                    }
                    auto allPlayers = getAllPlayersCopy();
                    if (newState >= 0) {
                        world_.setBlock(bx, by, bz, newState);
                        net::Buffer bu; bu.writePosition(BlockPos{bx, by, bz}); bu.writeVarInt(newState);
                        const auto bytes = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
                        for (auto& p : allPlayers)
                            if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                                p->getConnection()->sendPacket(0x09, bytes);
                        net::Buffer fx; fx.writeI32(2005); fx.writePosition(BlockPos{bx, by, bz}); fx.writeI32(0); fx.writeBool(false); // LevelEvent 2005: bonemeal particles
                        const auto fxBytes = std::vector<u8>(fx.writtenSpan().begin(), fx.writtenSpan().end());
                        for (auto& p : allPlayers)
                            if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                                p->getConnection()->sendPacket(0x28, fxBytes);
                        if (player->gameMode != 1 && --player->invCount[handSlot] <= 0) {
                            player->invCount[handSlot] = 0; player->invItemId[handSlot] = 0;
                            if (handSlot >= 36 && handSlot <= 44) player->hotbarBlockState[handSlot - 36] = -1;
                        }
                        net::Buffer slot; slot.writeByte(0); slot.writeVarInt(0); slot.writeI16(static_cast<i16>(handSlot));
                        slot.writeVarInt(player->invCount[handSlot]);
                        if (player->invCount[handSlot] > 0) { slot.writeVarInt(player->invItemId[handSlot]); slot.writeVarInt(0); slot.writeVarInt(0); }
                        player->getConnection()->sendPacket(0x15, std::vector<u8>(slot.writtenSpan().begin(), slot.writtenSpan().end()));
                    }
                    net::Buffer ab; ab.writeVarInt(seq);
                    player->getConnection()->sendPacket(0x05, std::vector<u8>(ab.writtenSpan().begin(), ab.writtenSpan().end()));
                    break;
                }
            }
            { // HOE_TILL_V1: hoe tills dirt/grass_block/dirt_path into farmland, coarse_dirt into dirt
                const i32 handSlot = useHand == 1 ? 45 : (36 + std::clamp(player->heldSlot, 0, 8));
                const i32 item = player->invCount[handSlot] > 0 ? player->invItemId[handSlot] : 0;
                const bool isHoe = item >= 818 && item <= 847 && (item - 818) % 5 == 4;
                if (isHoe && face != 0 && player->gameMode != 3) {
                    const i32 clicked = world_.getBlock(bx, by, bz);
                    const i32 above = world_.getBlock(bx, by + 1, bz);
                    const bool aboveClear = above == 0 || above == 12958 || above == 12959;
                    i32 newState = -1;
                    if (aboveClear) {
                        const auto* bs = registries::RegistryManager::instance().blockStates().getById(clicked);
                        const std::string_view n = bs ? std::string_view(bs->name) : std::string_view{};
                        if (n == "minecraft:grass_block" || n == "minecraft:dirt" || n == "minecraft:dirt_path")
                            newState = 4286; // farmland, moisture 0
                        else if (n == "minecraft:coarse_dirt")
                            newState = 10; // dirt
                    }
                    if (newState >= 0) {
                        world_.setBlock(bx, by, bz, newState);
                        net::Buffer bu; bu.writePosition(BlockPos{bx, by, bz}); bu.writeVarInt(newState);
                        const auto bytes = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
                        for (auto& p : getAllPlayersCopy())
                            if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                                p->getConnection()->sendPacket(0x09, bytes);
                    }
                    net::Buffer ab; ab.writeVarInt(seq);
                    player->getConnection()->sendPacket(0x05, std::vector<u8>(ab.writtenSpan().begin(), ab.writtenSpan().end()));
                    break;
                }
            }
            { // SEED_PLANT_V1: right-click top of farmland with seeds/carrot/potato plants a
              // crop at age 0; nether wart plants the same way on soul sand/soul soil.
              // Fixes the client briefly showing the crop then reverting it: without this
              // handler the server never confirmed the placement with a Block Update (0x09),
              // so the client's predicted block got rolled back.
                const i32 handSlot = useHand == 1 ? 45 : (36 + std::clamp(player->heldSlot, 0, 8));
                const i32 item = player->invCount[handSlot] > 0 ? player->invItemId[handSlot] : 0;
                struct SeedCrop { i32 itemId; i32 cropBase; bool onFarmland; };
                static const SeedCrop kSeeds[] = {
                    {853, 4278, true},    // wheat_seeds -> wheat
                    {1097, 8595, true},   // carrot -> carrots
                    {1098, 8603, true},   // potato -> potatoes
                    {1155, 12509, true},  // beetroot_seeds -> beetroots
                    {997, 7385, false},   // nether_wart -> nether_wart (soul sand/soil)
                    {986, 6821, true},    // pumpkin_seeds -> pumpkin_stem (STEM_GROWTH_V1)
                    {987, 6829, true},    // melon_seeds -> melon_stem (STEM_GROWTH_V1)
                    {1152, 12495, true},  // torchflower_seeds -> torchflower_crop (TORCHFLOWER_GROWTH_V1)
                };
                const SeedCrop* seed = nullptr;
                for (const auto& s : kSeeds) if (s.itemId == item) { seed = &s; break; }
                if (seed && face == 1 && player->gameMode != 3) {
                    const i32 clicked = world_.getBlock(bx, by, bz);
                    const bool validSoil = seed->onFarmland
                        ? (clicked >= 4286 && clicked <= 4293)
                        : (clicked == 5850 || clicked == 5851); // soul_sand / soul_soil
                    const i32 above = world_.getBlock(bx, by + 1, bz);
                    const bool aboveClear = above == 0 || above == 12958 || above == 12959;
                    if (validSoil && aboveClear) {
                        world_.setBlock(bx, by + 1, bz, seed->cropBase);
                        net::Buffer bu; bu.writePosition(BlockPos{bx, by + 1, bz}); bu.writeVarInt(seed->cropBase);
                        const auto bytes = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
                        for (auto& p : getAllPlayersCopy())
                            if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                                p->getConnection()->sendPacket(0x09, bytes);
                        if (player->gameMode != 1 && --player->invCount[handSlot] <= 0) {
                            player->invCount[handSlot] = 0; player->invItemId[handSlot] = 0;
                            if (handSlot >= 36 && handSlot <= 44) player->hotbarBlockState[handSlot - 36] = -1;
                        }
                        net::Buffer slot; slot.writeByte(0); slot.writeVarInt(0); slot.writeI16(static_cast<i16>(handSlot));
                        slot.writeVarInt(player->invCount[handSlot]);
                        if (player->invCount[handSlot] > 0) { slot.writeVarInt(player->invItemId[handSlot]); slot.writeVarInt(0); slot.writeVarInt(0); }
                        player->getConnection()->sendPacket(0x15, std::vector<u8>(slot.writtenSpan().begin(), slot.writtenSpan().end()));
                    }
                    net::Buffer ab2; ab2.writeVarInt(seq);
                    player->getConnection()->sendPacket(0x05, std::vector<u8>(ab2.writtenSpan().begin(), ab2.writtenSpan().end()));
                    break;
                }
            }
            { // COMPOSTER_FILL_V1: right-click a composter with a compostable item raises its
              // "level" by 1 with an item-dependent chance, consuming one item either way
              // (matching vanilla ComposterBlock.compost). At level 8 (full) any further use
              // instead empties it back to level 0 and gives the player one bone meal.
              // Simplification/honesty note: only a small, high-confidence subset of vanilla's
              // compostable item table is covered here (seeds/nether wart at an approximate
              // 30%, raw carrot/potato at an approximate 85%); this is not the full vanilla
              // compost chance table, which spans dozens of items this server doesn't need to
              // fully model yet.
                const i32 clicked = world_.getBlock(bx, by, bz);
                if (clicked >= 19372 && clicked <= 19380) { // minecraft:composter, level 0..8
                    const i32 handSlot = useHand == 1 ? 45 : (36 + std::clamp(player->heldSlot, 0, 8));
                    const i32 item = player->invCount[handSlot] > 0 ? player->invItemId[handSlot] : 0;
                    struct CompostItem { i32 itemId; i32 percentChance; };
                    static const CompostItem kCompostables[] = {
                        {853, 30},   // wheat_seeds
                        {1155, 30},  // beetroot_seeds
                        {987, 30},   // melon_seeds
                        {986, 30},   // pumpkin_seeds
                        {997, 30},   // nether_wart
                        {1152, 30},  // torchflower_seeds
                        {1097, 85},  // carrot
                        {1098, 85},  // potato
                    };
                    const CompostItem* compost = nullptr;
                    for (const auto& c : kCompostables) if (c.itemId == item) { compost = &c; break; }
                    if (compost && player->gameMode != 3) {
                        const i32 level = clicked - 19372;
                        i32 newState = -1;
                        i32 giveBoneMeal = 0;
                        if (level >= 8) {
                            newState = 19372; // empty composter
                            giveBoneMeal = 1;
                        } else if (static_cast<i32>(std::rand() % 100) < compost->percentChance) {
                            newState = 19372 + level + 1;
                        }
                        if (newState >= 0) {
                            world_.setBlock(bx, by, bz, newState);
                            net::Buffer bu; bu.writePosition(BlockPos{bx, by, bz}); bu.writeVarInt(newState);
                            const auto bytes = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
                            for (auto& p : getAllPlayersCopy())
                                if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                                    p->getConnection()->sendPacket(0x09, bytes);
                        }
                        if (player->gameMode != 1 && --player->invCount[handSlot] <= 0) {
                            player->invCount[handSlot] = 0; player->invItemId[handSlot] = 0;
                            if (handSlot >= 36 && handSlot <= 44) player->hotbarBlockState[handSlot - 36] = -1;
                        }
                        if (giveBoneMeal > 0) {
                            i32 freeSlot = -1;
                            for (i32 s = 36; s <= 44; ++s) {
                                if (player->invItemId[s] == 960 && player->invCount[s] < 64) { freeSlot = s; break; }
                            }
                            if (freeSlot < 0) for (i32 s = 36; s <= 44; ++s) if (player->invCount[s] <= 0) { freeSlot = s; break; }
                            if (freeSlot >= 0) {
                                if (player->invCount[freeSlot] <= 0) player->invItemId[freeSlot] = 960;
                                player->invCount[freeSlot] += giveBoneMeal;
                                net::Buffer slot2; slot2.writeByte(0); slot2.writeVarInt(0); slot2.writeI16(static_cast<i16>(freeSlot));
                                slot2.writeVarInt(player->invCount[freeSlot]);
                                slot2.writeVarInt(player->invItemId[freeSlot]); slot2.writeVarInt(0); slot2.writeVarInt(0);
                                player->getConnection()->sendPacket(0x15, std::vector<u8>(slot2.writtenSpan().begin(), slot2.writtenSpan().end()));
                            }
                        }
                        net::Buffer slot; slot.writeByte(0); slot.writeVarInt(0); slot.writeI16(static_cast<i16>(handSlot));
                        slot.writeVarInt(player->invCount[handSlot]);
                        if (player->invCount[handSlot] > 0) { slot.writeVarInt(player->invItemId[handSlot]); slot.writeVarInt(0); slot.writeVarInt(0); }
                        player->getConnection()->sendPacket(0x15, std::vector<u8>(slot.writtenSpan().begin(), slot.writtenSpan().end()));
                        net::Buffer ab3; ab3.writeVarInt(seq);
                        player->getConnection()->sendPacket(0x05, std::vector<u8>(ab3.writtenSpan().begin(), ab3.writtenSpan().end()));
                        break;
                    }
                }
            }
            { // CAKE_EAT_V1: right-click a cake removes one bite (bites 0..6); one more
              // use after the last bite removes the block entirely. No hunger is restored:
              // this server has no food/hunger system anywhere in the codebase (verified:
              // no hunger/foodLevel fields exist at all), so this is honestly cosmetic
              // block-state progress only, not vanilla's real hunger-restoring effect.
                const i32 clickedCake = world_.getBlock(bx, by, bz);
                if (clickedCake >= 5874 && clickedCake <= 5880 && player->gameMode != 3) { // minecraft:cake, bites 0..6
                    const i32 newCakeState = (clickedCake >= 5880) ? 0 : (clickedCake + 1);
                    world_.setBlock(bx, by, bz, newCakeState);
                    net::Buffer bu; bu.writePosition(BlockPos{bx, by, bz}); bu.writeVarInt(newCakeState);
                    const auto bytes = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
                    for (auto& p : getAllPlayersCopy())
                        if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                            p->getConnection()->sendPacket(0x09, bytes);
                    broadcastBlockSound("minecraft:entity.generic.eat", bx, by, bz, 1.0f, 1.0f);
                    net::Buffer ab4; ab4.writeVarInt(seq);
                    player->getConnection()->sendPacket(0x05, std::vector<u8>(ab4.writtenSpan().begin(), ab4.writtenSpan().end()));
                    break;
                }
            }
            { // NOTE_BLOCK_CHIME_V1: right-click a note block plays its chime sound only.
              // Honesty note: this server does not model note_block's real instrument/note/
              // powered property schema (its id gap to the next block does not cleanly
              // divide into vanilla's 16 instruments x 25 notes x 2 powered combinations,
              // so guessing that encoding would be unsafe) -- no note cycling or block-state
              // change happens here, sound feedback only.
                auto& reg = registries::RegistryManager::instance();
                const auto* clickedNoteBs = reg.blockStates().getById(world_.getBlock(bx, by, bz));
                if (clickedNoteBs && clickedNoteBs->name == "minecraft:note_block" && player->gameMode != 3) {
                    broadcastBlockSound("minecraft:block.note_block.harp", bx, by, bz, 3.0f, 1.0f);
                    net::Buffer ab5; ab5.writeVarInt(seq);
                    player->getConnection()->sendPacket(0x05, std::vector<u8>(ab5.writtenSpan().begin(), ab5.writtenSpan().end()));
                    break;
                }
            }
            { // BELL_RING_V1: right-click a bell plays its ring sound. Matched purely by
              // block name: this server does not register bell's attachment/facing/powered
              // properties (id gap of 34 doesn't cleanly divide by vanilla's 4x4x2=32
              // combinations, the same kind of minor padding already seen and documented
              // for other blocks in this table), so no wobble direction or state change is
              // modeled here -- sound feedback only.
                auto& reg = registries::RegistryManager::instance();
                const auto* clickedBellBs = reg.blockStates().getById(world_.getBlock(bx, by, bz));
                if (clickedBellBs && clickedBellBs->name == "minecraft:bell" && player->gameMode != 3) {
                    broadcastBlockSound("minecraft:block.bell.use", bx, by, bz, 2.0f, 1.0f);
                    net::Buffer ab6; ab6.writeVarInt(seq);
                    player->getConnection()->sendPacket(0x05, std::vector<u8>(ab6.writtenSpan().begin(), ab6.writtenSpan().end()));
                    break;
                }
            }
            { // COCOA_PLANT_V1: right-click a side face of a jungle log/wood (stripped or not)
              // with cocoa beans in hand plants a cocoa pod at age=0, facing away from the log.
              // Real block/item IDs for THIS server's registry were verified against
              // core/item_blocks.gen.hpp (cocoa base state = 7419); numbers from vanilla's
              // global palette (e.g. ~5153) do NOT apply to this custom-ordered registry and
              // would silently corrupt an unrelated hanging-sign block if used directly.
                const i32 handSlot = useHand == 1 ? 45 : (36 + std::clamp(player->heldSlot, 0, 8));
                const i32 item = player->invCount[handSlot] > 0 ? player->invItemId[handSlot] : 0;
                static const char* kCocoaFaceDir[6] = {"", "", "north", "south", "west", "east"};
                if (item == 921 && face >= 2 && face <= 5 && player->gameMode != 3) { // 921 = minecraft:cocoa_beans
                    const i32 clicked = world_.getBlock(bx, by, bz);
                    const auto* logBs = registries::RegistryManager::instance().blockStates().getById(clicked);
                    const bool validLog = logBs && (logBs->name == "minecraft:jungle_log" || logBs->name == "minecraft:stripped_jungle_log" ||
                        logBs->name == "minecraft:jungle_wood" || logBs->name == "minecraft:stripped_jungle_wood");
                    i32 ctx = bx, cty = by, ctz = bz;
                    switch (face) { case 2: --ctz; break; case 3: ++ctz; break; case 4: --ctx; break; case 5: ++ctx; break; default: break; }
                    const i32 targetState = world_.getBlock(ctx, cty, ctz);
                    if (validLog && (targetState == 0 || targetState == 12958 || targetState == 12959)) { // air/cave_air/void_air
                        auto& reg = registries::RegistryManager::instance();
                        auto cocoaState = reg.getBlockStateId("minecraft:cocoa", {{"age", "0"}, {"facing", kCocoaFaceDir[face]}});
                        if (cocoaState) {
                            world_.setBlock(ctx, cty, ctz, *cocoaState);
                            net::Buffer bu; bu.writePosition(BlockPos{ctx, cty, ctz}); bu.writeVarInt(*cocoaState);
                            const auto bytes = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
                            for (auto& p : getAllPlayersCopy())
                                if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                                    p->getConnection()->sendPacket(0x09, bytes);
                            if (player->gameMode != 1 && --player->invCount[handSlot] <= 0) {
                                player->invCount[handSlot] = 0; player->invItemId[handSlot] = 0;
                                if (handSlot >= 36 && handSlot <= 44) player->hotbarBlockState[handSlot - 36] = -1;
                            }
                            net::Buffer slot; slot.writeByte(0); slot.writeVarInt(0); slot.writeI16(static_cast<i16>(handSlot));
                            slot.writeVarInt(player->invCount[handSlot]);
                            if (player->invCount[handSlot] > 0) { slot.writeVarInt(player->invItemId[handSlot]); slot.writeVarInt(0); slot.writeVarInt(0); }
                            player->getConnection()->sendPacket(0x15, std::vector<u8>(slot.writtenSpan().begin(), slot.writtenSpan().end()));
                        }
                    }
                    net::Buffer ab3; ab3.writeVarInt(seq);
                    player->getConnection()->sendPacket(0x05, std::vector<u8>(ab3.writtenSpan().begin(), ab3.writtenSpan().end()));
                    break;
                }
            }
            i32 tx = bx, ty = by, tz = bz;
            switch (face) { case 0: --ty; break; case 1: ++ty; break; case 2: --tz; break; case 3: ++tz; break; case 4: --tx; break; case 5: ++tx; break; default: break; }
            if (player->gameMode != 3 && ty >= world::CHUNK_HEIGHT_MIN && ty < world::CHUNK_HEIGHT_MAX) { // SPECTATOR_V1 + HEIGHT_V2
                const i32 ax = tx < 0 ? -tx : tx;
                const i32 az = tz < 0 ? -tz : tz;
                const bool prot = config_.spawnProtection > 0 && player->dimension == 0 && ax <= config_.spawnProtection && az <= config_.spawnProtection && !isOpName(config_.ops, player->getName()); // OPS_V1
                i32 state = player->hotbarBlockState[player->heldSlot >= 0 && player->heldSlot < 9 ? player->heldSlot : 0];
                state = orientBlockForPlacement(state, player->get_yaw());
                if (isVehicleRailState(state)) state = computeRailState(tx, ty, tz, state, player->get_yaw()); // RAIL_SHAPE_V1: rails bend to their neighbours   // CHEST_V1: сундуки/печки ставятся лицом к игроку
                if (isConcretePowderState(state) &&
                    concretePowderShouldSolidify(world_, tx, ty, tz, world_.getBlock(tx, ty, tz)))
                    state = concreteFromPowder(state); // vanilla getStateForPlacement: water target/side -> concrete
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
                // RAILPLACE_V1: у рельсов нет коллизии — ванилла спокойно даёт класть их под себя.
                // ANTISUFFOCATE_V2 резал их, и сервер откатывал блок: «рельсы не ставятся вблизи».
                const bool playerBlocked = state > 0 && !isVehicleRailState(state) &&
                    (playerOccupies(tx, ty, tz) || (upperState >= 0 && playerOccupies(tx, ty + 1, tz)));
                // PLACE_V2: vanilla only overwrites replaceable blocks (air, fluids, grass,
                // fire, snow layers...). Without this check a door could be placed straight
                // into another door and the client kept showing a block the server refused.
                const bool targetBlocked = state > 0 && !isReplaceableState(world_.getBlock(tx, ty, tz));
                const bool upperBlocked = state > 0 && upperState >= 0 &&
                    (ty + 1 >= world::CHUNK_HEIGHT_MAX || !isReplaceableState(world_.getBlock(tx, ty + 1, tz)));
                if (prot) {
                    net::Buffer bu;
                    bu.writePosition(BlockPos{tx, ty, tz});
                    bu.writeVarInt(world_.getBlock(tx, ty, tz));
                    player->getConnection()->sendPacket(0x09, std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end()));
                    player->sendSystemMessage("§cСпавн защищён — здесь строить нельзя");
                } else if (targetBlocked || upperBlocked) {
                    // Reject and repaint both cells so the client cannot keep a ghost block.
                    for (const i32 ry : {ty, ty + 1}) {
                        if (ry < world::CHUNK_HEIGHT_MIN || ry >= world::CHUNK_HEIGHT_MAX) continue;
                        net::Buffer rb;
                        rb.writePosition(BlockPos{tx, ry, tz});
                        rb.writeVarInt(world_.getBlock(tx, ry, tz));
                        player->getConnection()->sendPacket(0x09, std::vector<u8>(rb.writtenSpan().begin(), rb.writtenSpan().end()));
                    }
                } else if (playerBlocked) {
                    net::Buffer bu;
                    bu.writePosition(BlockPos{tx, ty, tz});
                    bu.writeVarInt(world_.getBlock(tx, ty, tz));
                    player->getConnection()->sendPacket(0x09, std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end()));
                } else if (state == 2095 && tntTotalCount() >= kMaxTntTotal) {
                    // ANTILAG_TNT_V1: deny new TNT block placement once the combined active+stationary cap is hit.
                    net::Buffer bu;
                    bu.writePosition(BlockPos{tx, ty, tz});
                    bu.writeVarInt(world_.getBlock(tx, ty, tz));
                    player->getConnection()->sendPacket(0x09, std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end()));
                    player->sendSystemMessage(config_.language == "rus" ? "§cДостигнут лимит TNT в мире (8 000 000)" : "§cWorld TNT limit reached (8,000,000)");
                } else if (state > 0) {
                    world_.setBlock(tx, ty, tz, state);
                    if (state == 2095) tntBlockCount_.fetch_add(1, std::memory_order_relaxed); // ANTILAG_TNT_V1
                    if (isVehicleRailState(state)) updateRailShapesAround(tx, ty, tz); // RAIL_SHAPE_V1
                    scheduleFluidNeighbors(tx, ty, tz); // FLUID_V1: поставили блок — рядом жидкости пересчитаются
                    scheduleFallingBlockUpdate(tx, ty, tz, 2);     // FALLING_V1
                    scheduleFallingBlockUpdate(tx, ty + 1, tz, 2);
                    if (player->gameMode != 1) { // SURVIVAL_ITEM_V1 + BUCKET_V1: выживание расходует предмет; ведро — в пустое ведро
                        const i32 hs2 = (player->heldSlot >= 0 && player->heldSlot < 9) ? player->heldSlot : 0;
                        const i32 sl2 = 36 + hs2;
                        const i32 curId = player->invItemId[sl2];
                        bool slotChanged = false;
                        if (curId == 909 || curId == 910 || curId == 911) { // BUCKET_V1: ведро воды/лавы/рыхлого снега → пустое ведро (908)
                            player->invItemId[sl2] = 908;
                            player->invCount[sl2] = 1;
                            player->hotbarBlockState[hs2] = -1;
                            slotChanged = true;
                        } else if (player->invCount[sl2] > 0) {
                            if (--player->invCount[sl2] <= 0) { player->invCount[sl2] = 0; player->invItemId[sl2] = 0; player->hotbarBlockState[hs2] = -1; }
                            slotChanged = true;
                        }
                        if (slotChanged) {
                            net::Buffer ds;
                            ds.writeByte(0); ds.writeVarInt(0); ds.writeI16(static_cast<i16>(sl2));
                            ds.writeVarInt(player->invCount[sl2]);
                            if (player->invCount[sl2] > 0) { ds.writeVarInt(player->invItemId[sl2]); ds.writeVarInt(0); ds.writeVarInt(0); }
                            player->getConnection()->sendPacket(0x15, std::vector<u8>(ds.writtenSpan().begin(), ds.writtenSpan().end()));
                        }
                    }
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
                            if (world_.getBlock(tx + dx, ty, tz + dz) == single) {        // сосед по часовой — мы LEFT, он RIGHT
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
                    { // PLACE_V2: the server may rotate or merge what was placed (stairs, chests,
                      // rails, concrete, doors). Send the final state back to the placer so the
                      // client stops showing its own guess of the orientation.
                        net::Buffer fsb;
                        fsb.writePosition(BlockPos{tx, ty, tz});
                        fsb.writeVarInt(world_.getBlock(tx, ty, tz));
                        player->getConnection()->sendPacket(0x09, std::vector<u8>(fsb.writtenSpan().begin(), fsb.writtenSpan().end()));
                    }
                    { // SIGN_V8: Open Sign Editor (0x34) — в ванилке только что поставленная
                      // табличка сразу открывает редактор текста у того, кто её поставил.
                        const auto* placedBs = registries::RegistryManager::instance().blockStates().getById(world_.getBlock(tx, ty, tz));
                        if (placedBs) {
                            const std::string_view pn = placedBs->name;
                            if (pn.find("_sign") != std::string_view::npos && pn.find("sign_") == std::string_view::npos) {
                                net::Buffer se;
                                se.writePosition(BlockPos{tx, ty, tz});
                                se.writeBool(true); // front text
                                player->getConnection()->sendPacket(0x34, std::vector<u8>(se.writtenSpan().begin(), se.writtenSpan().end()));
                            }
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
            const i32 seq = data.readVarInt();
            // BUCKET_V2: 1.21.1 ServerboundUseItemPacket несёт актуальный взгляд.
            // Старый код игнорировал эти поля и иногда raycast'ил по прошлому yaw/pitch.
            const f32 useYaw = data.readF32();
            const f32 usePitch = data.readF32();
            const bool mainHandOk = player->heldSlot >= 0 && player->heldSlot < 9;
            const i32 handSlot = (useHand == 1) ? 45 : (mainHandOk ? 36 + player->heldSlot : -1);
            const i32 handItem = handSlot >= 0 ? player->invItemId[handSlot] : 0;
            const i32 handCount = handSlot >= 0 ? player->invCount[handSlot] : 0;
            { // SHIELD_V1: поднятие щита — активная фаза видна другим + включает блокирование
                if (handItem == 1162 && handCount > 0) { // 1162 = minecraft:shield (1.21.1)
                    const i64 nowUseMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (nowUseMs >= player->shieldDisabledUntilMs) { // SHIELD_V2: в кд (после топора) щит не поднять
                        player->usingShield = true;
                        player->usingShieldHand = useHand;
                        player->shieldRaisedMs = nowUseMs; // SHIELD_V2: ванильный про��рев 5 тиков
                        broadcastHandState(player); // метаданные 0x58: рука занята щитом
                    }
                }
            }

            { // BOOK_V8: Open Book (0x32) — ванильное использование писаной книги
              // открывает экран книги у клиента (никаких команд).
                if (handCount > 0) {
                    const std::string useItemName = nc::gen::itemNameById(handItem);
                    if (useItemName == "minecraft:written_book") {
                        net::Buffer ob;
                        ob.writeVarInt(useHand == 1 ? 1 : 0);
                        player->getConnection()->sendPacket(0x32, std::vector<u8>(ob.writtenSpan().begin(), ob.writtenSpan().end()));
                    }
                }
            }

            auto sendHandSlot = [&]() {
                if (handSlot < 0) return;
                net::Buffer slot;
                slot.writeByte(0); slot.writeVarInt(0); slot.writeI16(static_cast<i16>(handSlot));
                slot.writeVarInt(player->invCount[handSlot]);
                if (player->invCount[handSlot] > 0) {
                    slot.writeVarInt(player->invItemId[handSlot]);
                    slot.writeVarInt(0); slot.writeVarInt(0);
                }
                player->getConnection()->sendPacket(0x15,
                    std::vector<u8>(slot.writtenSpan().begin(), slot.writtenSpan().end()));
            };
            auto setHandItem = [&](i32 id, i32 count) {
                if (handSlot < 0) return;
                player->invItemId[handSlot] = count > 0 ? id : 0;
                player->invCount[handSlot] = std::max(0, count);
                if (handSlot >= 36 && handSlot <= 44) {
                    const i32 hs = handSlot - 36;
                    player->hotbarBlockState[hs] = id == 909 ? 80 : (id == 910 ? 96 : (id == 911 ? 22318 : -1));
                }
            };
            auto insertFilledBucket = [&](i32 id) -> bool {
                // BUCKET_REALSTACK_V1: filled bucket никогда не merge'ится с уже
                // существующим filled bucket. Только отдельный серверный slot count=1.
                auto tryRange = [&](i32 first, i32 last) -> bool {
                    for (i32 slotIndex = first; slotIndex <= last; ++slotIndex) {
                        if (player->invCount[slotIndex] > 0) continue;
                        player->invItemId[slotIndex] = id;
                        player->invCount[slotIndex] = 1;
                        if (slotIndex >= 36 && slotIndex <= 44)
                            player->hotbarBlockState[slotIndex - 36] = id == 909 ? 80 : 96;
                        net::Buffer slot;
                        slot.writeByte(0); slot.writeVarInt(0); slot.writeI16(static_cast<i16>(slotIndex));
                        slot.writeVarInt(1); slot.writeVarInt(id); slot.writeVarInt(0); slot.writeVarInt(0);
                        player->getConnection()->sendPacket(0x15,
                            std::vector<u8>(slot.writtenSpan().begin(), slot.writtenSpan().end()));
                        return true;
                    }
                    return false;
                };
                return tryRange(36, 44) || tryRange(9, 35);
            };
            auto broadcastBlock = [&](i32 x, i32 y, i32 z, i32 state) {
                net::Buffer bu;
                bu.writePosition(BlockPos{x, y, z});
                bu.writeVarInt(state);
                auto bytes = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
                for (auto& p : getAllPlayersCopy())
                    if (p && p->isAlive() && p->getState() == entity::PlayerState::Play)
                        p->getConnection()->sendPacket(0x09, bytes);
            };
            auto acknowledgeUse = [&]() {
                net::Buffer ab; ab.writeVarInt(seq);
                player->getConnection()->sendPacket(0x05,
                    std::vector<u8>(ab.writtenSpan().begin(), ab.writtenSpan().end()));
            };
            auto broadcastSwing = [&]() {
                net::Buffer swing; swing.writeVarInt(static_cast<i32>(player->getEntityId())); swing.writeByte(0);
                const auto swingBytes = std::vector<u8>(swing.writtenSpan().begin(), swing.writtenSpan().end());
                for (auto& observer : getAllPlayersCopy())
                    if (observer && observer->isAlive() && observer->getState() == entity::PlayerState::Play)
                        observer->getConnection()->sendPacket(0x03, swingBytes);
            };

            // ELYTRA_ROCKET_V1: rocket boost is valid only while the server has
            // accepted fall-flying. A short forward/up impulse is sent as entity motion;
            // the normal client movement packets continue to provide the glide trajectory.
            // ELYTRA_ROCKET_V2: ракета работала на земле и без раскрытых элитр: флаг
            // elytraFlying сбрасывался только в tick(), так что между посадкой и тиком
            // на земле он ещё стоял в true, и клик по ракете давал подброс на 1-2 блока.
            // Теперь обязательно: летим, НЕ на земле и элитры реально надеты.
            // ELYTRA_ROCKET_V3: «ракета работает без элитр» — это КЛИЕНТСКАЯ предсказанная
            // тяга: клиент считает себя планирующим, потому что мы не сказали ему обратное.
            // На любой отказ по ракете шлём метаданные с снятым битом FALL_FLYING и позой
            // STANDING — клиент откатывает свою предсказанную тягу.
            if (handItem == 1112 && handCount > 0 &&
                !(player->elytraFlying && player->gameMode != 3 && !player->isOnGround() &&
                  player->invCount[6] > 0 && player->invItemId[6] == 773)) {
                if (player->elytraFlying) player->elytraFlying = false;
                broadcastEntityMeta(player);
            }
            if (handItem == 1112 && handCount > 0 && player->elytraFlying && player->gameMode != 3 &&
                !player->isOnGround() && player->invCount[6] > 0 && player->invItemId[6] == 773) {
                const f64 yr = static_cast<f64>(useYaw) * 0.017453292519943295;
                const f64 pr = static_cast<f64>(usePitch) * 0.017453292519943295;
                const f64 dx = -std::sin(yr) * std::cos(pr);
                const f64 dy = -std::sin(pr);
                const f64 dz =  std::cos(yr) * std::cos(pr);
                auto vel = [](f64 v) { return static_cast<i16>(std::clamp(v * 8000.0, -31200.0, 31200.0)); };
                net::Buffer motion;
                motion.writeVarInt(static_cast<i32>(player->getEntityId()));
                // Vanilla applies thrust for many ticks; this server has no rocket entity
                // simulation, so send one equivalent decisive impulse. A small positive floor
                // prevents level flight from immediately becoming a vertical dive.
                // ELYTRA_ROCKET_V2: было dy*2.4 + 0.65 с полом не ниже 0.35 — т.е. ВСЕГДА вверх,
                // даже когда смотришь горизонтально или вниз. Отсюда подброс на 1-2 блока
                // и невозможность ускориться в пике. Ваниль толкает СТРОГО по взгляду
                // (look * 1.5), без вертикального бонуса.
                motion.writeI16(vel(dx * 2.2));
                motion.writeI16(vel(dy * 2.2));
                motion.writeI16(vel(dz * 2.2));
                const auto motionBytes = std::vector<u8>(motion.writtenSpan().begin(), motion.writtenSpan().end());
                for (auto& observer : getAllPlayersCopy())
                    if (observer && observer->isAlive() && observer->getState() == entity::PlayerState::Play)
                        observer->getConnection()->sendPacket(0x5A, motionBytes);
                broadcastBlockSound("minecraft:entity.firework_rocket.launch",
                    static_cast<i32>(std::floor(player->getX())), static_cast<i32>(std::floor(player->getY())),
                    static_cast<i32>(std::floor(player->getZ())), 1.0f, 1.0f);
                broadcastSwing();
                if (player->gameMode != 1) setHandItem(handItem, handCount - 1);
                sendHandSlot();
                acknowledgeUse();
                break;
            }

            if (handItem == 993 && handCount > 0 && player->gameMode != 3) { // minecraft:ender_pearl
                if (player->enderPearlCooldownTicks > 0) {
                    // Still acknowledge the predicted use, but never consume or
                    // create a second pearl while the authoritative cooldown runs.
                    acknowledgeUse();
                    break;
                }
                player->enderPearlCooldownTicks = 20; // vanilla EnderpearlItem: addCooldown(item, 20)
                net::Buffer cooldown;
                cooldown.writeVarInt(993); // item registry id
                cooldown.writeVarInt(20);
                // Protocol 767: 0x16 is Cookie Request; Cooldown is 0x17.
                player->getConnection()->sendPacket(0x17,
                    std::vector<u8>(cooldown.writtenSpan().begin(), cooldown.writtenSpan().end()));
                spawnEnderPearl(player, useYaw, usePitch);
                // Vanilla LivingEntity.swing: broadcast arm animation for this accepted use only.
                broadcastSwing();
                broadcastBlockSound("minecraft:entity.ender_pearl.throw",
                                    static_cast<i32>(std::floor(player->getX())),
                                    static_cast<i32>(std::floor(player->getY())),
                                    static_cast<i32>(std::floor(player->getZ())), 0.5f, 0.9f);
                if (player->gameMode != 1) setHandItem(handItem, handCount - 1);
                sendHandSlot();
                acknowledgeUse();
                break;
            }

            if ((handItem == 912 || handItem == 927 || handItem == 1088) && handCount > 0 && player->gameMode != 3) { // snowball / egg / experience bottle
                const bool snowball = handItem == 912;
                const bool egg = handItem == 927;
                const bool xpBottle = handItem == 1088;
                spawnThrowableProjectile(player, useYaw, usePitch,
                                        snowball ? 97 : (egg ? 28 : 37), // ENTITY_ID_FIX_V2: snowball = 97
                                        snowball ? 2 : (egg ? 3 : 4));
                broadcastSwing();
                broadcastBlockSound(snowball ? "minecraft:entity.snowball.throw"
                                             : (egg ? "minecraft:entity.egg.throw" : "minecraft:entity.experience_bottle.throw"),
                                    static_cast<i32>(std::floor(player->getX())),
                                    static_cast<i32>(std::floor(player->getY())),
                                    static_cast<i32>(std::floor(player->getZ())), 0.5f, 0.9f);
                if (player->gameMode != 1) setHandItem(handItem, handCount - 1);
                sendHandSlot();
                acknowledgeUse();
                break;
            }

            const bool bucketUse = handCount > 0 &&
                (handItem == 908 || handItem == 909 || handItem == 910);
            if (bucketUse) {
                const f64 yawR = static_cast<f64>(useYaw) * 3.1415926535897932 / 180.0;
                const f64 pitR = static_cast<f64>(usePitch) * 3.1415926535897932 / 180.0;
                const f64 ddx = -std::sin(yawR) * std::cos(pitR);
                const f64 ddy = -std::sin(pitR);
                const f64 ddz = std::cos(yawR) * std::cos(pitR);

                i32 hitX = 0, hitY = -1000, hitZ = 0;
                i32 prevX = static_cast<i32>(std::floor(player->getX()));
                i32 prevY = static_cast<i32>(std::floor(player->getY() + 1.62));
                i32 prevZ = static_cast<i32>(std::floor(player->getZ()));
                i32 lastX = prevX, lastY = prevY, lastZ = prevZ;
                for (f64 tt = 0.0; tt <= 5.0; tt += 0.05) {
                    const i32 cx = static_cast<i32>(std::floor(player->getX() + ddx * tt));
                    const i32 cy = static_cast<i32>(std::floor(player->getY() + 1.62 + ddy * tt));
                    const i32 cz = static_cast<i32>(std::floor(player->getZ() + ddz * tt));
                    if (cy < world::CHUNK_HEIGHT_MIN || cy >= world::CHUNK_HEIGHT_MAX) break; // HEIGHT_V2
                    if (cx == lastX && cy == lastY && cz == lastZ) continue;
                    prevX = lastX; prevY = lastY; prevZ = lastZ;
                    lastX = cx; lastY = cy; lastZ = cz;
                    const i32 state = world_.getBlock(cx, cy, cz);
                    const bool fluid = (state >= 80 && state <= 111);
                    if (handItem == 908) {
                        // ClipContext.Fluid.SOURCE_ONLY: только state 80/96 можно забрать;
                        // текущая жидкость прозрачна для поиска источника, твёрдый блок останавливает луч.
                        if (state == 80 || state == 96) { hitX = cx; hitY = cy; hitZ = cz; break; }
                        if (state != 0 && !fluid) break;
                    } else {
                        // Filled bucket использует ClipContext.Fluid.NONE: жидкости не являются hit-shape.
                        if (state != 0 && !fluid) { hitX = prevX; hitY = prevY; hitZ = prevZ; break; }
                    }
                }

                bool success = false;
                if (hitY > -1000) {
                    const i32 ax = std::abs(hitX - g_spawnX);
                    const i32 az = std::abs(hitZ - g_spawnZ);
                    const bool prot = config_.spawnProtection > 0 && player->dimension == 0 && ax <= config_.spawnProtection && az <= config_.spawnProtection && !isOpName(config_.ops, player->getName()); // OPS_V1
                    if (prot) player->sendSystemMessage("§cСпавн защищён — здесь строить нельзя");
                    else if (handItem == 908) {
                        // BUCKET_V2: серверно-авторитетный pickup только source state.
                        const i32 source = world_.getBlock(hitX, hitY, hitZ);
                        if (source == 80 || source == 96) {
                            const i32 filled = source == 80 ? 909 : 910;
                            bool resultReserved = false;
                            if (player->gameMode != 1 && handCount == 1) {
                                setHandItem(filled, 1);
                                resultReserved = true;
                            } else if (player->gameMode == 1) {
                                // Не используем vanilla creative duplicate suppression:
                                // она удаляет источник без нового stack, если такой bucket уже есть.
                                resultReserved = insertFilledBucket(filled);
                            } else {
                                resultReserved = insertFilledBucket(filled);
                                if (resultReserved) setHandItem(908, handCount - 1);
                                else {
                                    // Survival + полный инвентарь: ItemUtils в vanilla бросает
                                    // настоящий ItemEntity. Источник можно забирать только после его создания.
                                    spawnItemDrop(player->getX(), player->getY() + 0.5, player->getZ(), filled, 1, 0.0, 0.1, 0.0, 10);
                                    setHandItem(908, handCount - 1);
                                    resultReserved = true;
                                }
                            }
                            if (resultReserved) {
                                world_.setBlock(hitX, hitY, hitZ, 0);
                                scheduleFluidNeighbors(hitX, hitY, hitZ);
                                scheduleFallingBlockUpdate(hitX, hitY + 1, hitZ, 2);
                                scheduleFallingColumnCascade(hitX, hitY + 1, hitZ, 2);
                                broadcastBlock(hitX, hitY, hitZ, 0);
                                broadcastBlockSound(source == 80 ? "minecraft:item.bucket.fill" : "minecraft:item.bucket.fill_lava",
                                    hitX, hitY, hitZ, 1.0f, 1.0f);
                                success = true;
                            }
                        }
                    } else if (world_.getBlock(hitX, hitY, hitZ) == 0) {
                        const i32 placed = handItem == 909 ? 80 : 96;
                        world_.setBlock(hitX, hitY, hitZ, placed);
                        scheduleFluidNeighbors(hitX, hitY, hitZ);
                        scheduleFallingBlockUpdate(hitX, hitY + 1, hitZ, 2);
                        broadcastBlock(hitX, hitY, hitZ, placed);
                        if (placed == 80) solidifyConcretePowderAround(hitX, hitY, hitZ);
                        if (player->gameMode != 1) setHandItem(908, 1);
                        broadcastBlockSound(placed == 80 ? "minecraft:item.bucket.empty" : "minecraft:item.bucket.empty_lava",
                            hitX, hitY, hitZ, 1.0f, 1.0f);
                        success = true;
                    }
                }

                // BUCKET_TX_V1: Set Slot отправляется и при успехе, и при отказе.
                // Это немедленно исправляет клиентский prediction вместо ожидания relog/инвентарного sync.
                sendHandSlot();
                if (!success && hitY > -1000)
                    broadcastBlock(hitX, hitY, hitZ, world_.getBlock(hitX, hitY, hitZ));
            }
            {
                net::Buffer ab;
                ab.writeVarInt(seq);
                player->getConnection()->sendPacket(0x05, std::vector<u8>(ab.writtenSpan().begin(), ab.writtenSpan().end()));
            }
            break;
        }
        // ALLPACKETS_V1: полное покрытие serverbound play-пакетов 1.21.1 (protocol 767).
        // Пакеты без своей подсистемы принимаются без ошибок (раньше молча падали в default).
        case 0x01: { // ALLPACKETS_V3: Query Block Entity NBT — реальный ответ данными,
            // которые сервер сам сохранил (командный блок / табличка); честно отвечает
            // "нет данных" (TAG_End) для любого другого блока вместо выдуманного NBT.
            const i32 qbTxn = data.readVarInt();
            const u64 qbPos = data.readU64();
            std::optional<std::string> qbCmdFound;
            std::optional<std::array<std::string, 4>> qbSignFound;
            {
                std::lock_guard<std::mutex> lk(g_cmdBlockMutex);
                auto it = g_cmdBlockText.find(qbPos);
                if (it != g_cmdBlockText.end()) qbCmdFound = it->second;
            }
            if (!qbCmdFound) {
                std::lock_guard<std::mutex> lk(g_signMutex);
                auto it = g_signText.find(qbPos);
                if (it != g_signText.end()) qbSignFound = it->second;
            }
            net::Buffer qr;
            qr.writeVarInt(qbTxn);
            if (qbCmdFound) {
                nbt::TagWriter qw;
                qw.beginRootCompound();
                qw.writeString(*qbCmdFound, "Command");
                qw.endCompound();
                qr.writeBytes(qw.getBuffer().writtenSpan());
            } else if (qbSignFound) {
                nbt::TagWriter qw;
                qw.beginRootCompound();
                qw.writeString((*qbSignFound)[0], "Text1");
                qw.writeString((*qbSignFound)[1], "Text2");
                qw.writeString((*qbSignFound)[2], "Text3");
                qw.writeString((*qbSignFound)[3], "Text4");
                qw.endCompound();
                qr.writeBytes(qw.getBuffer().writtenSpan());
            } else {
                qr.writeByte(0x00); // TAG_End — у нас действительно нет данных для этого блока
            }
            player->getConnection()->sendPacket(0x6E, std::vector<u8>(qr.writtenSpan().begin(), qr.writtenSpan().end()));
            break;
        }
        case 0x02: { // Change Difficulty — от клиента игнорируем (сложность задаёт сервер)
            break;
        }
        case 0x05: { // ALLPACKETS_V1: Signed Chat Command — исполняем тем же путём, ��то и 0x04
            std::string command = data.readString();
            net::Buffer cmdBuf;
            cmdBuf.writeString(command);
            handlePlay(player, cmdBuf, 0x04);
            break;
        }
        case 0x07: { // Chat Session Update — подпись чата не проверяем (offline)
            break;
        }
        case 0x0B: { // ALLPACKETS_V3: Command Suggestions Request — реальные подсказки
            // по именам зарегистрированных команд верхнего уровня; аргументы команд
            // (второе слово и далее) честно не доподсказываем — дерева аргументов нет.
            const i32 tcTxn = data.readVarInt();
            const std::string tcText = data.readString();
            std::vector<std::string> tcMatches;
            i32 tcStart = static_cast<i32>(tcText.size());
            i32 tcLen = 0;
            if (!tcText.empty() && tcText[0] == '/' && tcText.find(' ') == std::string::npos) {
                const std::string prefix = tcText.substr(1);
                std::string prefixLower = prefix;
                for (auto& c : prefixLower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                tcStart = 1;
                tcLen = static_cast<i32>(prefix.size());
                for (auto& def : nc::cmd::CommandRegistry::instance().all()) {
                    std::string nameLower = def.name;
                    for (auto& c : nameLower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                    if (nameLower.rfind(prefixLower, 0) == 0) tcMatches.push_back("/" + def.name);
                    if (tcMatches.size() >= 50) break;
                }
            }
            net::Buffer tr;
            tr.writeVarInt(tcTxn);
            tr.writeVarInt(tcStart);
            tr.writeVarInt(tcLen);
            tr.writeVarInt(static_cast<i32>(tcMatches.size()));
            for (auto& m : tcMatches) { tr.writeString(m); tr.writeBool(false); }
            player->getConnection()->sendPacket(0x10, std::vector<u8>(tr.writtenSpan().begin(), tr.writtenSpan().end()));
            break;
        }
        case 0x0C: { // Acknowledge Configuration — реконфигурацию не инициируем
            break;
        }
        case 0x0D: { // Enchant Item — столов зачарования нет
            break;
        }
        case 0x12: { // Plugin Message (custom payload) — каналы модов игнорируем
            break;
        }
        case 0x13: { // Debug Sample Subscription — телеметрию отладки не отдаём
            break;
        }
        case 0x14: { // ALLPACKETS_V3: Edit Book — реальное сохранение: заголовок книги
            // становится настоящим именем предмета в руке; полный текст страниц не храним —
            // его некуда рендерить без открытия книги клиентом.
            const i32 ebHand = data.readVarInt();
            const i32 ebPagesLen = data.readVarInt();
            std::vector<std::string> ebPages;
            for (i32 i = 0; i < ebPagesLen; ++i) {
                std::string page = data.readString();
                if (static_cast<i32>(ebPages.size()) < 100) ebPages.push_back(std::move(page));
            }
            const bool ebHasTitle = data.readBool();
            std::string ebTitle;
            if (ebHasTitle) ebTitle = data.readString();
            const i32 ebSlot = (ebHand == 1) ? 45 : (36 + std::clamp(player->heldSlot, 0, 8));
            if (ebHasTitle && !ebTitle.empty() && player->invItemId[ebSlot] > 0 && player->invCount[ebSlot] > 0) {
                player->invCustomName[ebSlot] = ebTitle;
                sendItemSlotWithName(player, ebSlot);
                broadcastHeldEquipment(player);
            }
            NC_DEBUG("Book", "{} wrote {} page(s){}", player->getName(), ebPages.size(),
                     ebHasTitle ? (" titled '" + ebTitle + "'") : std::string());
            break;
        }
        case 0x15: { // ALLPACKETS_V3: Query Entity NBT — реальный частичный NBT игрока;
            // мобы/предметы честно не отвечают — их состояние не хранится по entity id.
            const i32 qeTxn = data.readVarInt();
            const i32 qeEntityId = data.readVarInt();
            std::shared_ptr<entity::Player> qeFound;
            for (auto& p : getAllPlayersCopy()) {
                if (p && static_cast<i32>(p->getEntityId()) == qeEntityId) { qeFound = p; break; }
            }
            net::Buffer qr;
            qr.writeVarInt(qeTxn);
            if (qeFound) {
                nbt::TagWriter qw;
                qw.beginRootCompound();
                qw.writeString("minecraft:player", "id");
                qw.writeIntArray({
                    static_cast<i32>(qeFound->getUuid().mostSignificant >> 32),
                    static_cast<i32>(qeFound->getUuid().mostSignificant & 0xFFFFFFFFu),
                    static_cast<i32>(qeFound->getUuid().leastSignificant >> 32),
                    static_cast<i32>(qeFound->getUuid().leastSignificant & 0xFFFFFFFFu)
                }, "UUID");
                qw.writeDoubleList({qeFound->getX(), qeFound->getY(), qeFound->getZ()}, "Pos");
                qw.writeFloat(qeFound->health, "Health");
                qw.writeByte(qeFound->isOnGround() ? 1 : 0, "OnGround");
                qw.endCompound();
                qr.writeBytes(qw.getBuffer().writtenSpan());
            } else {
                qr.writeByte(0x00); // TAG_End — честно: эта сущность не отслеживается по id
            }
            player->getConnection()->sendPacket(0x6E, std::vector<u8>(qr.writtenSpan().begin(), qr.writtenSpan().end()));
            break;
        }
        case 0x17: { // Generate Structure (jigsaw) — генерацию структур не поддерживаем
            break;
        }
        case 0x19: { // ALLPACKETS_V3: Lock Difficulty — реальный флаг: пока включён,
            // /difficulty отказывает в изменении сложности (как в ванильном клиенте).
            const bool dlLocked = data.readBool();
            if (isOpName(config_.ops, player->getName())) {
                config_.difficultyLocked = dlLocked;
                const bool dlRu = (player->clientLocale.rfind("ru", 0) == 0);
                player->sendSystemMessage(dlRu
                    ? (dlLocked ? "§eСложность заблокирована." : "§eСложность разблокирована.")
                    : (dlLocked ? "§eDifficulty locked." : "§eDifficulty unlocked."));
            }
            break;
        }
        case 0x1E: { // VEHICLE_PHYSICS_V1: Move Vehicle - boats are client-authoritative
            const f64 mvx = data.readF64();
            const f64 mvy = data.readF64();
            const f64 mvz = data.readF64();
            const f32 mvYaw = data.readF32();
            (void)data.readF32(); // pitch
            handleVehicleMove(player, mvx, mvy, mvz, mvYaw);
            break;
        }
        case 0x1F: { // VEHICLE_PHYSICS_V1: Paddle Boat - paddling is mirrored to other clients
            const bool paddleLeft = data.readBool();
            const bool paddleRight = data.readBool();
            if (player->ridingVehicleEid != 0) {
                net::Buffer md;
                md.writeVarInt(player->ridingVehicleEid);
                md.writeByte(12); md.writeVarInt(8); md.writeBool(paddleLeft);  // DATA_ID_PADDLE_LEFT
                md.writeByte(13); md.writeVarInt(8); md.writeBool(paddleRight); // DATA_ID_PADDLE_RIGHT
                md.writeByte(0xFF);
                broadcastToOthers(player, 0x58, std::vector<u8>(md.writtenSpan().begin(), md.writtenSpan().end()), true);
            }
            break;
        }
        case 0x20: { // ALLPACKETS_V2: Pick Item - middle click moves an inventory slot into the hand
            const i32 pickSlot = data.readVarInt();
            if (pickSlot >= 0 && pickSlot < 46) {
                const i32 handSlot = 36 + std::clamp(player->heldSlot, 0, 8);
                if (pickSlot != handSlot) {
                    std::swap(player->invItemId[pickSlot], player->invItemId[handSlot]);
                    std::swap(player->invCount[pickSlot], player->invCount[handSlot]);
                    if (player->heldSlot >= 0 && player->heldSlot < 9) player->hotbarBlockState[player->heldSlot] = -1;
                    for (const i32 sSlot : {pickSlot, handSlot}) {
                        net::Buffer pk; pk.writeByte(0); pk.writeVarInt(0); pk.writeI16(static_cast<i16>(sSlot));
                        pk.writeVarInt(player->invCount[sSlot]);
                        if (player->invCount[sSlot] > 0) { pk.writeVarInt(player->invItemId[sSlot]); pk.writeVarInt(0); pk.writeVarInt(0); }
                        player->getConnection()->sendPacket(0x15, std::vector<u8>(pk.writtenSpan().begin(), pk.writtenSpan().end()));
                    }
                    broadcastHeldEquipment(player);
                }
            }
            break;
        }
        case 0x22: { // RECIPE_V8: Place Recipe — книги рецептов нет, поэтому сервер честно
            // отвечает Place Ghost Recipe (0x37), как ванильный сервер на запрос клиента.
            const i32 recipeWindowId = data.readVarInt();
            const std::string recipeId = data.readString();
            (void)data.readBool(); // makeAll (shift-click)
            if (!recipeId.empty()) {
                net::Buffer gr;
                gr.writeByte(static_cast<u8>(recipeWindowId & 0xFF));
                gr.writeString(recipeId);
                player->getConnection()->sendPacket(0x37, std::vector<u8>(gr.writtenSpan().begin(), gr.writtenSpan().end()));
            }
            break;
        }
        case 0x26: { // VEHICLE_PHYSICS_V1: Player Input (steer vehicle)
            const f32 steerSideways = data.readF32();
            const f32 steerForward = data.readF32();
            const u8 steerFlags = static_cast<u8>(data.readByte());
            player->vehicleSideways = steerSideways;
            player->vehicleForward = steerForward;
            if ((steerFlags & 0x02) != 0) vehicleDismount(player); // shift = leave the vehicle
            break;
        }
        case 0x27: { // Pong (ответ на play-ping) — принимаем, состояние не храним
            break;
        }
        case 0x28: { // ALLPACKETS_V3: Recipe Book Settings — реально храним состояние
            // кнопки/фильтра книги рецептов на игроке; самих рецептов/крафта по ним всё ещё нет.
            (void)data.readVarInt(); // bookId — у нас один общий стейт на игрока
            const bool rbOpen = data.readBool();
            const bool rbFilter = data.readBool();
            player->recipeBookOpen = rbOpen;
            player->recipeBookFilterActive = rbFilter;
            break;
        }
        case 0x29: { // ALLPACKETS_V3: Set Seen Recipe — реально запоминаем, что рецепт увиден
            const std::string rsRecipeId = data.readString();
            if (!rsRecipeId.empty()) player->seenRecipes.insert(rsRecipeId);
            break;
        }
        case 0x2A: { // ALLPACKETS_V3: Rename Item (anvil) — реальное переименование предмета
            // в руке через minecraft:custom_name; полноценной наковальни (XP, окно)
            // нет, поэтому переименование бесплатное и мгновенное.
            const std::string riName = data.readString();
            const i32 riSlot = 36 + std::clamp(player->heldSlot, 0, 8);
            if (player->invItemId[riSlot] > 0 && player->invCount[riSlot] > 0) {
                player->invCustomName[riSlot] = riName;
                sendItemSlotWithName(player, riSlot);
                broadcastHeldEquipment(player);
            }
            break;
        }
        case 0x2B: { // Resource Pack Response — ресурспак не навязываем
            break;
        }
        case 0x2C: { // Seen Advancements — вкладки достижений не ведём
            break;
        }
        case 0x2D: { // VILLAGER_TRADE_V1: Select Trade — игрок выбрал строку в окне жителя
            const i32 tradeIndex = data.readVarInt();
            villagerSelectTrade(player, tradeIndex);
            break;
        }
        case 0x2E: { // ALLPACKETS_V3: Set Beacon Effect — реально накладывает статус-эффект
            // на игрока через существующую систему EFFECTS_V2; настоящей пирамиды/
            // уровней маяка нет, поэтому применяем эффект уровня I сразу, без привязки к дальности.
            const bool beHasPrimary = data.readBool();
            i32 bePrimary = -1;
            if (beHasPrimary) bePrimary = data.readVarInt();
            const bool beHasSecondary = data.readBool();
            i32 beSecondary = -1;
            if (beHasSecondary) beSecondary = data.readVarInt();
            constexpr i32 kBeaconDurationTicks = 220; // ~длительность ванильного маяка уровня I
            if (bePrimary >= 0) addPlayerEffect(player, bePrimary, 0, kBeaconDurationTicks);
            if (beSecondary >= 0 && beSecondary != bePrimary) addPlayerEffect(player, beSecondary, 0, kBeaconDurationTicks);
            break;
        }
        case 0x30: { // ALLPACKETS_V2: Update Command Block - the GUI saved a command
            const u64 cbPos = data.readU64();
            const std::string cbCmd = data.readString();
            const i32 cbMode = data.readVarInt();
            const u8 cbFlags = static_cast<u8>(data.readByte());
            i32 cbx, cby, cbz;
            decodeBlockPos(cbPos, cbx, cby, cbz);
            {
                std::lock_guard<std::mutex> lk(g_cmdBlockMutex);
                if (cbCmd.empty()) g_cmdBlockText.erase(cbPos);
                else g_cmdBlockText[cbPos] = cbCmd;
            }
            NC_INFO("CmdBlock", "{} saved '{}' at {} {} {} (mode={}, flags={})", player->getName(), cbCmd,
                    cbx, cby, cbz, cbMode, static_cast<i32>(cbFlags));
            const bool ruCb = (player->clientLocale.rfind("ru", 0) == 0);
            player->sendSystemMessage(ruCb
                ? "§eКоманда сохранена в блоке. Запуск командных блоков ещё не сделан."
                : "§eCommand stored in the block. Running command blocks is not implemented yet.");
            break;
        }
        case 0x31: { // ALLPACKETS_V3: Update Command Block Minecart — реально сохраняем
            // команду по entityId (как и для обычного командного блока); запуск команд
            // из вагонетки по-прежнему не выполняется — это честно только хранение.
            const i32 cmEntityId = data.readVarInt();
            const std::string cmCmd = data.readString();
            (void)data.readBool(); // track output — некуда показать last output
            {
                std::lock_guard<std::mutex> lk(g_cmdMinecartMutex);
                if (cmCmd.empty()) g_cmdMinecartText.erase(cmEntityId);
                else g_cmdMinecartText[cmEntityId] = cmCmd;
            }
            const bool cmRu = (player->clientLocale.rfind("ru", 0) == 0);
            player->sendSystemMessage(cmRu
                ? "§eКоманда сохранена в вагонетке. Запуск командных блоков ещё не сделан."
                : "§eCommand stored in the minecart. Running command blocks is not implemented yet.");
            break;
        }
        case 0x33: { // Update Jigsaw Block — jigsaw-блоков нет
            break;
        }
        case 0x34: { // Update Structure Block — структурных блоков нет
            break;
        }
        case 0x35: { // ALLPACKETS_V2: Update Sign - keep the four lines the player typed
            const u64 signPos = data.readU64();
            const bool signFront = data.readBool();
            std::string signLines[4];
            for (auto& line : signLines) line = data.readString();
            i32 sgx, sgy, sgz;
            decodeBlockPos(signPos, sgx, sgy, sgz);
            {
                std::lock_guard<std::mutex> lk(g_signMutex);
                g_signText[signPos] = {signLines[0], signLines[1], signLines[2], signLines[3]};
            }
            NC_DEBUG("Sign", "{} wrote a sign at {} {} {} ({}): {} | {} | {} | {}", player->getName(),
                     sgx, sgy, sgz, signFront ? "front" : "back",
                     signLines[0], signLines[1], signLines[2], signLines[3]);
            break;
        }
        case 0x37: { // ALLPACKETS_V2: Spectate - jump to the picked player in spectator mode
            const u64 specHi = data.readU64();
            const u64 specLo = data.readU64();
            if (player->gameMode == 3) {
                for (const auto& target : getAllPlayersCopy()) {
                    if (!target || !target->isAlive() || target.get() == player.get()) continue;
                    if (target->getUuid().mostSignificant != specHi ||
                        target->getUuid().leastSignificant != specLo) continue;
                    player->setPosition(target->getX(), target->getY(), target->getZ());
                    sendPlayerPositionAndLook(player);
                    { // CAMERA_V8: Set Camera (0x52) — в ванилке спектатор смотрит глазами цели
                        net::Buffer cam;
                        cam.writeVarInt(static_cast<i32>(target->getEntityId()));
                        player->getConnection()->sendPacket(0x52, std::vector<u8>(cam.writtenSpan().begin(), cam.writtenSpan().end()));
                    }
                    break;
                }
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
    buf.writeVarInt(isDouble ? 5 : 2); // CHEST_V3: generic_9x6 ��ля двойного, иначе generic_9x3
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

// CHEST_V2: сколько игроков сейчас держат открытым сундук с данным ключо�� позиции.
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
    ba.writeByte(static_cast<u8>(viewers)); // 0 зрителей = крышка закрывается
    ba.writeVarInt(blockId);               // реестровый id блока (не state!)
    auto vec = std::vector<u8>(ba.writtenSpan().begin(), ba.writtenSpan().end());
    for (auto& p : getAllPlayersCopy()) if (p && p->isAlive()) p->getConnection()->sendPacket(0x08, vec);
}

// CHEST_V2: именованный звук — Sound Effect 0x68 (inline sound event) всем игрокам.
// PORTAL_V1: старые broadcastBlock — две локальные лямбды, зашитые на world_ (оверворлд)
// и рассылающие всем подряд. Порталу нужно ставить блоки в своём мире.
void NetherCraftServer::broadcastBlockIn(i32 dim, i32 x, i32 y, i32 z, i32 state) {
    if (y < world::CHUNK_HEIGHT_MIN || y >= world::CHUNK_HEIGHT_MAX) return;
    world::World& w = worldFor(dim);
    w.getOrGenerateChunk(x >> 4, z >> 4);
    w.setBlock(x, y, z, state);
    net::Buffer bu;
    bu.writePosition(BlockPos{x, y, z});
    bu.writeVarInt(state);
    auto vec = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
    for (auto& p : getAllPlayersCopy())
        if (p && p->isAlive() && p->playReady && p->dimension == dim && p->getState() == entity::PlayerState::Play) // JOINSAFE_V1
            p->getConnection()->sendPacket(0x09, vec);
}

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
    for (auto& p : getAllPlayersCopy()) // JOINSAFE_V1
        if (p && p->isAlive() && p->playReady && p->getState() == entity::PlayerState::Play) p->getConnection()->sendPacket(0x68, vec);
}

// SHIELD_V1: метаданные «руки заняты» (Set Entity Metadata 0x58, index 8, byte) —
// активная фаза щита в��дна остальным игрокам.
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
    if (!player) return;
    player->openMerchantEid = 0; // VILLAGER_TRADE_V1: окно торговли закрыто
    if (player->openWindowId == 0) return;
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
    if (key2 != 0) { // CHEST_V3: крышка в��орой половины двойного сундука
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

// ASYNCSAVE_V1: содержимое файла игрока формируем в строку на tick-потоке
// (микросекунды) — фоновый поток пишет готовый снапшот и не читает живые поля.
static std::string buildPlayerDataContent(const std::shared_ptr<entity::Player>& player) {
    std::ostringstream f;
    f << player->getX() << " " << player->getY() << " " << player->getZ()
      << " " << player->get_yaw() << " " << player->get_pitch() << " " << player->gameMode;
    for (i32 slotState : player->hotbarBlockState) f << " " << slotState;
    f << "\n"; // INVENTORY_SAVE_V1: nine hotbar block states
    // INVENTORY_V3
    f << "INV3 " << player->heldSlot;
    for (int i = 0; i < entity::Player::INV_SIZE; ++i) f << " " << player->invItemId[i] << " " << player->invCount[i];
    f << "\n";
    f << "MINIEDIT1 " << (player->builderWandOwned ? 1 : 0) << "\n";
    f << "TPSBAR1 " << (player->tpsBossbarEnabled ? 1 : 0) << "\n";
    // SKINPERSIST_V1: base64 textures/signature не содержит пробелов, поэтому формат безопасно расширяем.
    if (!player->texturesValue.empty()) f << "SKINV1 " << player->texturesValue << " " << player->texturesSignature << "\n";
    return f.str();
}

void NetherCraftServer::savePlayerData(std::shared_ptr<entity::Player> player) {
    if (!player || player->getName().empty()) return;
    std::error_code ec;
    std::filesystem::create_directories("world/playerdata", ec);
    const std::string path = "world/playerdata/" + player->getName() + ".txt";
    const std::string tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return;
    f << buildPlayerDataContent(player); // ASYNCSAVE_V1: общий сериализатор
    f.close();
    if (!f.good()) { std::filesystem::remove(tmp, ec); return; }
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
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
        // Optional tagged lines remain backwards compatible with old playerdata.
        std::string optionalTag;
        bool sawMiniEditTag = false;
        while (f >> optionalTag) {
            if (optionalTag == "MINIEDIT1") {
                i32 owned = 0; if (f >> owned) player->builderWandOwned = owned != 0;
                sawMiniEditTag = true;
            } else if (optionalTag == "TPSBAR1") {
                i32 enabled = 1; if (f >> enabled) player->tpsBossbarEnabled = enabled != 0;
            } else if (optionalTag == "SKINV1") {
                std::string savedTextures, savedSignature;
                if (f >> savedTextures) {
                    f >> savedSignature;
                    player->texturesValue = std::move(savedTextures);
                    player->texturesSignature = std::move(savedSignature);
                }
            }
        }
        // One-time migration for wands issued by V10.1/V10.2, before the marker existed.
        if (!sawMiniEditTag) for (int i = 0; i < entity::Player::INV_SIZE; ++i)
            if (player->invCount[i] > 0 && player->invItemId[i] == 821) { player->builderWandOwned = true; break; }
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
    const i32 oldCx = player->getViewCenterX();
    const i32 oldCz = player->getViewCenterZ();
    player->setViewCenter(cx, cz);

    { // UNLOAD_V8: Unload Chunk (0x21) — ванильное поведение: чанки, вышедшие
      // за пределы view distance при движении игрока, снимаются у клиента.
        i32 ur = config_.viewDistance;
        if (ur < 2) ur = 2;
        for (i32 ox = oldCx - ur; ox <= oldCx + ur; ++ox) {
            for (i32 oz = oldCz - ur; oz <= oldCz + ur; ++oz) {
                if (std::abs(ox - cx) <= ur && std::abs(oz - cz) <= ur) continue;
                if (!player->hasSeenChunk(ox, oz)) continue;
                net::Buffer uc;
                uc.writeI32(oz); // protocol order: chunkZ then chunkX
                uc.writeI32(ox);
                player->getConnection()->sendPacket(0x21, std::vector<u8>(uc.writtenSpan().begin(), uc.writtenSpan().end()));
                player->forgetChunk(ox, oz);
            }
        }
    }

    net::Buffer viewBuf;
    viewBuf.writeVarInt(cx);
    viewBuf.writeVarInt(cz);
    player->getConnection()->sendPacket(0x54,
        std::vector<u8>(viewBuf.writtenSpan().begin(), viewBuf.writtenSpan().end()));

    i32 r = config_.viewDistance; // VIEWDIST_V1: полный радиус, чтобы край мира не торчал до тумана
    if (r < 2) r = 2;
    sendChunksAround(player, cx, cz, r, 16); // PERF_ASYNC_V2: fill new view edge faster (was 9)

    // MEM_V1/MEM_V2: unload chunks far from every player so RAM stays bounded as
    // players roam — теперь во всех трёх измерениях, а не только в оверворлде,
    // и с учётом измерения каждого игрока.
    pruneAllWorlds();
}

// SOFTRELOAD_V1: мягкий рестарт без завершения процесса. Вызывать ТОЛЬКО на tick-потоке
// (через консольную очередь). Сеть, пул генерации и чат-поток НЕ пересоздаются —
// они без игрового состояния: нет ни deadlock'а, ни use-after-free, ни гонок потоков.
void NetherCraftServer::softReload() {
    const auto t0 = std::chrono::steady_clock::now();
    const bool ru = (config_.language == "rus");
    { // RELOADBANNER_V1: крупный баннер — р��старт должно быть видно в консоли
        const char* rbBar = "============================================================";
        std::cout << "\n\033[33m" << rbBar << "\n"
                  << (ru ? ">>>            МЯГКИЙ РЕСТАРТ СЕРВЕРА            <<<\n"
                         : ">>>            SOFT SERVER RESTART            <<<\n")
                  << (ru ? ">>>  сохраняю мир/игроков, кикаю всех, выгружаю чанки  <<<\n"
                         : ">>>  saving world/players, kicking everyone, unloading chunks  <<<\n")
                  << rbBar << "\033[0m\n" << std::flush;
        nc::log::rawLine(ru ? "==== МЯГКИЙ РЕСТАРТ СЕРВЕРА: сохраняю и перезагружаю ===="
                            : "==== SOFT SERVER RESTART: saving and reloading ====");
    }

    // 1) дождаться фонового автосейва и сохранить игроков ДО кика
    if (saveThread_.joinable()) saveThread_.join(); // ASYNCSAVE_V1
    auto all = getAllPlayersCopy();
    for (auto& p : all) savePlayerData(p);

    // 2) кикнуть всех с честным Disconnect-экраном
    for (auto& p : all) if (p && p->isAlive())
        p->kick(ru ? "§eСервер перезагружается..." : "§eServer is restarting...");

    // 3) сохранить мир СИНХРО��НО (внутри — атомарная подмена файла, мир не потеряется)
    saveWorlds(); // DIMSAVE_V1

    // 4) очистить серверное состояние (disconnect-коллбэки повторный erase переживут)
    {
        std::lock_guard lock(playersMutex_);
        players_.clear();
    }
    {
        std::lock_guard lk(entitiesMutex_);
        entities_.clear(); // мобы/сущности
    }
    {
        std::lock_guard lk(itemDropsMutex_);
        itemDrops_.clear(); // ITEMDROP_V1: выпавшие предметы очищаем при мягком рестарте
    }
    { std::lock_guard lk(primedTntMutex_); primedTnt_.clear(); }
    bulkTntJobs_.clear();
    tntLightResync_.clear();
    tntLightResyncQueued_.clear();
    { std::lock_guard lk(projectilesMutex_); projectiles_.clear(); }
    miniEdit_.clear();
    { std::lock_guard lk(miniEditPacketsMutex_); miniEditPackets_.clear(); }
    tabListDirty_.store(true, std::memory_order_relaxed);

    // 5) выгрузить все чанки и очереди генерации (RAM вернётся к стартовой)
    world_.reset();

    // 6) перечитать конфиг (язык, view-distance, max-players и т.д.)
    if (!configPath_.empty()) config_ = ServerConfig::loadFrom(configPath_);
    world_.setLanguageRu(config_.language == "rus");

    // 7) поднять мир обратно с только что сохранённого файла (в фоне, как при старте)
    if (!world_.startBackgroundLoad("world/overworld/world.dat")) {
        NC_WARN("Server", ru ? "��ягкий рестарт: world.dat не прочитан — чанки сгенерируются на лету" : "Soft reload: world.dat unreadable - chunks will regenerate on the fly");
    }

    const f64 reloadMs = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
    { // RELOADBANNER_V1: финальный баннер
        const bool ruRb = (config_.language == "rus");
        const char* rbBar = "============================================================";
        std::string rbDone = ruRb
            ? std::format(">>>   МЯГКИЙ РЕСТАРТ ЗАВЕРШЁН за {:.0f}мс   <<<", reloadMs)
            : std::format(">>>   SOFT RESTART DONE in {:.0f}ms   <<<", reloadMs);
        std::cout << "\n\033[32m" << rbBar << "\n" << rbDone << "\n"
                  << (ruRb ? ">>>   серв��р снова принимает иг��оков   <<<\n"
                           : ">>>   the server is accepting players again   <<<\n")
                  << rbBar << "\033[0m\n" << std::flush;
        nc::log::rawLine(rbDone);
    }
    if (config_.language == "rus") NC_INFO("Server", "Мягкий рестарт завершён за {:.0f}мс — сервер снова принимает игроков", reloadMs);
    else NC_INFO("Server", "Soft reload finished in {:.0f}ms - accepting players again", reloadMs);
}

// CONSOLEHUMAN_V1: команды, которым нужен живой игрок (позиция, инвентарь, измерение).
namespace {
bool isPlayerOnlyCommand(const std::string& c) {
    static const char* kList[] = {"tp", "spawn", "setworldspawn", "nether", "end", "overworld",
        "setblock", "skin", "summon", "killall", "tps", "msg", "warprandomtick",
        "wand", "pos1", "pos2", "pso", "set", "replace", "copy", "paste", "rotate", "undo", "redo", "we", "edit"};
    for (const char* n : kList) if (c == n) return true;
    return !c.empty() && c[0] == '/'; // //set и прочий MiniEdit
}
} // namespace

void NetherCraftServer::processConsoleCommands() {
    std::deque<ConsoleCommandItem> commands;
    {
        std::lock_guard lock(consoleMutex_);
        commands.swap(consoleCommands_);
    }
    for (ConsoleCommandItem& rconItem : commands) {
        // RCON_BRIDGE_V1: RAII гарант, чтобы даже ранние `continue;` ниже корректно
        // завершали перехват и отдавали ответ RCON-клиенту.
        std::string capturedOutput;
        struct RconCaptureGuard {
            std::shared_ptr<std::promise<std::string>> result;
            std::string* captured;
            bool active = false;
            ~RconCaptureGuard() {
                if (active) {
                    nc::log::endCapture();
                    if (result) { try { result->set_value(*captured); } catch (...) {} }
                }
            }
        } rconGuard{rconItem.result, &capturedOutput};
        // RCONQUIET_V1: silent=true — ответ уходит RCON-клиенту и НЕ печатается в консоли
        if (rconItem.result) { nc::log::beginCapture(&capturedOutput, true); rconGuard.active = true; }

        std::string command = rconItem.text;
        if (!command.empty() && command[0] == '/') command.erase(0, 1);
        std::istringstream iss(command);
        std::string cmd;
        iss >> cmd;
        if (cmd.empty()) continue;
        if (cmd == "help") {
            NC_INFO("Console", "Commands: help, list, say <text>, gamemode <mode> <player>, gm0..gm3 <player>, kick <player>, time set <t>, weather <clear|rain|thunder>, save-all, export-vanilla [dir], import-vanilla <dir>, crash, stop, reload"); // CONSOLE_V3 // ANVIL_CONVERT_V1 // CRASHTEST_V1
        } else if (cmd == "list") {
            auto all = getAllPlayersCopy();
            std::string names;
            int count = 0;
            for (const auto& p : all) if (p && p->isAlive()) {
                if (!names.empty()) names += ", ";
                // PINGSTAT_V1: рядом с ником сразу актуальный пинг (мс, -1 = ещё не замерен)
                names += std::format("{} ({}ms)", p->getName(), p->pingMs);
                ++count;
            }
            // Формат "Players (N): ..." нарочно не меняем — его парсит веб-панель.
            NC_INFO("Console", "Players ({}): {}", count, names.empty() ? "none" : names);
            NC_INFO("Console", "TPS {:.2f} | RAM {:.0f} MB | CPU {:.0f}% | slots {}/{}",
                    tps_, ramMb_, cpuPercent_, count, config_.maxPlayers);
        } else if (cmd == "tps" || cmd == "stats") {
            // PINGSTAT_V1 / WEBRES_V1: "tps" — читаемый отчёт для человека,
            // "stats" — одна JSON-строка для веб-панели. Цифры берём из тех же
            // счётчиков, что кормят игровой TPS-bossbar (sampleProcessStats()).
            const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
            auto all = getAllPlayersCopy();
            int online = 0, pinged = 0;
            i64 pingSum = 0;
            for (const auto& p : all) if (p && p->isAlive()) {
                ++online;
                if (p->pingMs >= 0) { pingSum += p->pingMs; ++pinged; }
            }
            if (cmd == "tps") {
                // PINGFMT_V1: пинг есть только у игроков, которым уже ушёл keep-alive.
                // Пока таких нет — пишем n/a, а не минус одну миллисекунду.
                const std::string avgPing = pinged
                    ? std::format("{}ms", static_cast<i64>(pingSum / pinged))
                    : std::string("n/a");
                NC_INFO("Console", "TPS {:.2f} | RAM {:.0f} MB | CPU {:.0f}% ({} cores) | players {}/{} | avg ping {}",
                        tps_, ramMb_, cpuPercent_, cores, online, config_.maxPlayers, avgPing);
            } else {
                std::string js = std::format(
                    "STATS {{\"tps\":{:.2f},\"ramMb\":{:.1f},\"cpu\":{:.1f},\"cores\":{},\"online\":{},\"max\":{},\"players\":[",
                    tps_, ramMb_, cpuPercent_, cores, online, config_.maxPlayers);
                bool firstPlayer = true;
                for (const auto& p : all) if (p && p->isAlive()) {
                    if (!firstPlayer) js += ",";
                    firstPlayer = false;
                    js += std::format("{{\"name\":\"{}\",\"ping\":{}}}", p->getName(), p->pingMs);
                }
                js += "]}";
                NC_INFO("Console", "{}", js);
            }
        } else if (cmd == "say") {
            std::string text; std::getline(iss, text);
            if (!text.empty() && text[0] == ' ') text.erase(0, 1);
            if (text.empty()) { NC_INFO("Console", "Usage: say <text>"); continue; }
            for (const auto& p : getAllPlayersCopy()) if (p && p->isAlive())
                p->sendSystemMessage(std::format("§d[Console] {}", text));
            NC_INFO("Console", "[Console] {}", text);
        } else if (cmd == "gamemode" || cmd == "gm0" || cmd == "gm1" || cmd == "gm2" || cmd == "gm3") { // CONSOLE_V3: смена режима из консоли
            i32 mode = -1;
            std::string nick;
            if (cmd != "gamemode") { mode = cmd[2] - '0'; iss >> nick; }
            else {
                std::string mm; iss >> mm >> nick;
                if (mm == "survival" || mm == "0") mode = 0;
                else if (mm == "creative" || mm == "1") mode = 1;
                else if (mm == "adventure" || mm == "2") mode = 2;
                else if (mm == "spectator" || mm == "3") mode = 3;
            }
            if (mode < 0 || nick.empty()) { NC_INFO("Console", "Usage: gamemode <survival|creative|adventure|spectator|0-3> <player> | gm0..gm3 <player>"); continue; }
            std::shared_ptr<entity::Player> target;
            std::string want = nick;
            for (auto& wc : want) wc = static_cast<char>(::tolower(static_cast<unsigned char>(wc)));
            for (auto& p : getAllPlayersCopy()) {
                if (!p || !p->isAlive() || p->getState() != entity::PlayerState::Play) continue;
                std::string nm = p->getName();
                for (auto& mc : nm) mc = static_cast<char>(::tolower(static_cast<unsigned char>(mc)));
                if (nm == want) { target = p; break; }
            }
            if (!target) { NC_INFO("Console", "Player '{}' not found", nick); continue; }
            applyGameMode(target, mode);
            static const char* kGmNames[4] = {"survival", "creative", "adventure", "spectator"};
            target->sendSystemMessage(std::format("§aРежим игры: {}", kGmNames[mode]));
            NC_INFO("Console", "Game mode {} set for {}", kGmNames[mode], target->getName());
        } else if (cmd == "kick") { // CONSOLE_V3
            std::string nick; iss >> nick;
            if (nick.empty()) { NC_INFO("Console", "Usage: kick <player>"); continue; }
            bool found = false;
            for (auto& p : getAllPlayersCopy()) {
                if (p && p->isAlive() && p->getName() == nick) {
                    p->kick("§cВы были кикнуты с сервера"); // KICKFIX_V1
                    found = true;
                }
            }
            if (found) NC_INFO("Console", "Player {} kicked", nick);
            else NC_INFO("Console", "Player '{}' not found", nick);
        } else if (cmd == "time") { // CONSOLE_V3
            std::string sub, v; iss >> sub >> v;
            i64 t = -1;
            if (sub == "set") {
                if (v == "day") t = 1000;
                else if (v == "noon") t = 6000;
                else if (v == "night") t = 13000;
                else if (v == "midnight") t = 18000;
                else { try { t = std::stoll(v); } catch (...) { t = -1; } }
            }
            if (t < 0) { NC_INFO("Console", "Usage: time set <day|noon|night|midnight|ticks>"); continue; }
            g_timeOfDay = t;
            net::Buffer tb;
            tb.writeI64(t);
            tb.writeI64(t);
            auto vec = std::vector<u8>(tb.writtenSpan().begin(), tb.writtenSpan().end());
            for (auto& p : getAllPlayersCopy()) if (p && p->isAlive() && p->getState() == entity::PlayerState::Play) p->getConnection()->sendPacket(0x64, vec);
            NC_INFO("Console", "Time set to {}", t);
        } else if (cmd == "warprandomtick") { // RANDOMTICK_WARP_V1: консольный вариант, см. версию из чата.
            std::string v; iss >> v;
            i64 n = -1;
            try { n = std::stoll(v); } catch (...) { n = -1; }
            if (n <= 0) { NC_INFO("Console", "Usage: warprandomtick <count, max 1000>"); continue; }
            n = std::min<i64>(n, 1000);
            for (i64 i = 0; i < n; ++i) tickRandomBlockUpdates();
            NC_INFO("Console", "Ran {} random-tick passes", n);
        } else if (cmd == "weather") { // CONSOLE_V3
            std::string w; iss >> w;
            if (w != "clear" && w != "rain" && w != "thunder") { NC_INFO("Console", "Usage: weather <clear|rain|thunder>"); continue; }
            g_weather = (w == "clear") ? 0 : ((w == "thunder") ? 2 : 1);
            for (auto& target : getAllPlayersCopy()) sendWeatherState(target);
            NC_INFO("Console", "Weather: {}", w);
        } else if (cmd == "save-all" || cmd == "save") {
            saveWorlds(); // DIMSAVE_V1
            for (const auto& p : getAllPlayersCopy()) if (p) savePlayerData(p);
            NC_INFO("Console", "World and player data saved");
        } else if (cmd == "export-vanilla") {
            // ANVIL_CONVERT_V1: export the live world to a vanilla-loadable
            // singleplayer/server save. Usage: export-vanilla [dir]
            std::string dir; std::getline(iss, dir);
            if (!dir.empty() && dir[0] == ' ') dir.erase(0, 1);
            if (dir.empty()) dir = "vanilla_export";
            std::string err;
            if (world::anvil::exportToVanilla(world_, dir, config_.levelName, config_.levelSeed, g_spawnX, g_spawnY, g_spawnZ, &err)) {
                NC_INFO("Console", "Exported world to '{}' (vanilla Anvil format)", dir);
            } else {
                NC_WARN("Console", "export-vanilla failed: {}", err);
            }
        } else if (cmd == "import-vanilla") {
            // ANVIL_CONVERT_V1: import a vanilla save's chunks into the live world.
            // Usage: import-vanilla <dir>
            std::string dir; std::getline(iss, dir);
            if (!dir.empty() && dir[0] == ' ') dir.erase(0, 1);
            if (dir.empty()) {
                NC_WARN("Console", "Usage: import-vanilla <dir>");
            } else {
                std::string err;
                if (world::anvil::importFromVanilla(world_, dir, &err)) {
                    NC_INFO("Console", "Imported vanilla world from '{}'", dir);
                } else {
                    NC_WARN("Console", "import-vanilla failed: {}", err);
                }
            }
        } else if (cmd == "stop") {
            stop(); // STOPLOG_V1: сообщение печатает сам stop()
        } else if (cmd == "whitelist") { // WHITELIST_CMD_V1
            std::string sub; iss >> sub;
            std::string arg; iss >> arg;
            if (sub.empty() || sub == "list") {
                auto ns = whitelist_.names();
                NC_INFO("Console", "whitelist: {} path={} entries={}", config_.whiteList ? "on" : "off", pathU8(std::filesystem::path(whitelist_.path())), ns.size());
                std::string row; int cnt = 0;
                for (auto& n : ns) { if (!row.empty()) row += ", "; row += n; if (++cnt >= 8) { NC_INFO("Console", "{}", row); row.clear(); cnt = 0; } }
                if (!row.empty()) NC_INFO("Console", "{}", row);
            } else if (sub == "on" || sub == "off") {
                config_.whiteList = (sub == "on");
                NC_INFO("Console", "whitelist {}", sub);
            } else if (sub == "add") {
                if (arg.empty()) NC_INFO("Console", "Usage: whitelist add <nik>");
                else if (whitelist_.add(arg)) NC_INFO("Console", "{} added", arg);
                else NC_WARN("Console", "{} already in list", arg);
            } else if (sub == "remove" || sub == "rm") {
                if (arg.empty()) { NC_INFO("Console", "Usage: whitelist remove <nik>"); }
                else if (!whitelist_.remove(arg)) { NC_WARN("Console", "{} not found", arg); }
                else {
                    NC_INFO("Console", "{} removed", arg);
                    if (config_.whiteList) {
                        for (auto& p : getAllPlayersCopy()) {
                            if (!p || !p->isAlive()) continue;
                            std::string a = p->getName(), b = arg;
                            std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                            std::transform(b.begin(), b.end(), b.begin(), ::tolower);
                            if (a == b) p->kick("\xc2\xa7" "c\xd0\xa3\xd0\xb1\xd1\x80\xd0\xb0\xd0\xbd \xd0\xb8\xd0\xb7 \xd0\xb1\xd0\xb5\xd0\xbb.\xd1\x81\xd0\xbf\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0");
                        }
                    }
                }
            } else if (sub == "reload") {
                whitelist_.load();
                NC_INFO("Console", "whitelist reloaded: {} entries", whitelist_.size());
            } else {
                NC_INFO("Console", "Usage: whitelist <list|on|off|add|remove|reload>");
            }
        } else if (cmd == "reload") { // SOFTRELOAD_V1
            softReload();
        } else if (cmd == "crash") { // CRASHTEST_V1: same intentional test crash, but from the console (server owner — no op check needed)
            setCrashContext("core", "crash (console)", "");
            NC_ERROR("Console", "crash: intentional test crash (std::abort), no save, kicking {} player(s) first", getAllPlayersCopy().size());
            for (auto& p : getAllPlayersCopy()) if (p && p->isAlive()) p->kick("§cСервер аварийно остановлен (тест /crash)");
            std::abort();
        } else if (cmd == "give") { // GIVECMD_V1: с консоли нужен явный ник
            std::string gWho, gWhat, gCnt;
            iss >> gWho >> gWhat >> gCnt;
            if (gWho.empty() || gWhat.empty()) {
                NC_WARN("Console", "Usage: give <player> <id|name> [count]");
            } else {
                std::shared_ptr<entity::Player> gTarget;
                for (auto& cand : getAllPlayersCopy())
                    if (cand && cand->getName() == gWho) { gTarget = cand; break; }
                const i32 gId = nc::gen::itemIdByName(gWhat);
                i32 gCount = 1;
                if (!gCnt.empty()) { try { gCount = static_cast<i32>(std::stol(gCnt)); } catch (...) {} }
                gCount = std::clamp(gCount, 1, 6400);
                if (!gTarget) NC_WARN("Console", "Player '{}' is not online", gWho);
                else if (gId <= 0) NC_WARN("Console", "Unknown item '{}'", gWhat);
                else {
                    const i32 gGave = giveItemToPlayer(gTarget, gId, gCount);
                    NC_INFO("Console", "Gave {} x {} (id {}) to {}", nc::gen::itemNameById(gId), gGave, gId, gWho);
                }
            }
        } else if (isPlayerOnlyCommand(cmd)) { // CONSOLEHUMAN_V1
            if (config_.language == "rus") NC_ERROR("Console", "\033[31mКоманду '/{}' может выполнить только игрок. Консоль не человек.\033[0m", cmd);
            else NC_ERROR("Console", "\033[31m'/{}' can only be run by a player. The console is not a human.\033[0m", cmd);
        } else if (auto* pc = nc::cmd::CommandRegistry::instance().find(cmd); pc && pc->handler) {
            // PLUGINCMD_V1: the console is the server owner, so it always counts as
            // "op" here — this is exactly the case the user asked about (an admin
            // typing a donation-plugin command straight into the console must not
            // get rejected just because core does not know that command by name).
            nc::cmd::CommandContext ctx;
            ctx.isConsole = true;
            ctx.isOp = true;
            ctx.args = {cmd};
            std::string __tok;
            while (iss >> __tok) ctx.args.push_back(__tok);
            ctx.reply = [](const std::string& msg) { NC_INFO("Console", "{}", msg); };
            NC_INFO("Console", "Running plugin command '{}' (source: {})", cmd, pc->source);
            pc->handler(ctx);
        } else {
            NC_WARN("Console", "Unknown command '{}'. Type help.", cmd);
        }
    }
}

// ============================================================
// FLUID_V1: динамика жидкостей (вода/лава) + образование камня/обсидиана.
// Pull-модель: клетка сама вычисляет своё состояние по соседям. Любое
// изменение блока планирует пересчёт соседей, поэтому поток и растекается,
// и отступает автоматически. Обрабатываем ограниченную пачку за тик.
// ============================================================
static inline u64 fluidKey(i32 x, i32 y, i32 z) { return dimPackKey(x, y, z); }       // DIMPHYS_V1
static inline void fluidUnkey(u64 k, i32& x, i32& y, i32& z) { dimUnpackKey(k, x, y, z); }

void NetherCraftServer::scheduleFluidUpdate(i32 x, i32 y, i32 z, i32 delay) {
    if (y < world::CHUNK_HEIGHT_MIN || y >= world::CHUNK_HEIGHT_MAX) return;
    const u64 k = fluidKey(x, y, z);
    if (delay < 0) {
        const i32 state = worldFor(g_dimCtx).getBlock(x, y, z); // DIMPHYS_V1
        delay = (state >= 96 && state <= 111) ? 30
              : (state >= 80 && state <= 95) ? 5 : 1;
    }
    const i32 due = tickCounter_ + std::max(1, delay);
    std::lock_guard lk(fluidMutex_);
    auto it = fluidDue_.find(k);
    if (it != fluidDue_.end() && it->second <= due) return;
    fluidDue_[k] = due;
    fluidQueue_.emplace(due, k);
}

void NetherCraftServer::scheduleFluidNeighbors(i32 x, i32 y, i32 z) {
    scheduleFluidUpdate(x, y, z);
    scheduleFluidUpdate(x + 1, y, z); scheduleFluidUpdate(x - 1, y, z);
    scheduleFluidUpdate(x, y, z + 1); scheduleFluidUpdate(x, y, z - 1);
    scheduleFluidUpdate(x, y + 1, z); scheduleFluidUpdate(x, y - 1, z);
}

void NetherCraftServer::tickFluids() { // DIMPHYS_V1: пачка раскладывается по измерениям
    std::vector<u64> collected;
    {
        std::lock_guard lk(fluidMutex_);
        if (fluidQueue_.empty()) return;
        constexpr size_t LIMIT = 4096; // ограничение на тик — защита от лага при больших разливах
        while (!fluidQueue_.empty() && collected.size() < LIMIT && fluidQueue_.begin()->first <= tickCounter_) {
            const auto [due, k] = *fluidQueue_.begin();
            fluidQueue_.erase(fluidQueue_.begin());
            const auto current = fluidDue_.find(k);
            if (current == fluidDue_.end() || current->second != due) continue; // устаревшая запись
            fluidDue_.erase(current);
            collected.push_back(k);
        }
    }
    if (collected.empty()) return;
    std::vector<u64> perDim[3];
    for (const u64 k : collected) perDim[dimOfKey(k) % 3].push_back(k);
    for (i32 d = 0; d < 3; ++d) {
        if (perDim[d].empty()) continue;
        if ((d == 1 && !netherReady_) || (d == 2 && !endReady_)) continue;
        tickFluidsIn(d, perDim[d]);
    }
}

void NetherCraftServer::tickFluidsIn(i32 dimIndex, const std::vector<u64>& batch) {
    world::World& world_ = worldFor(dimIndex); // C4458: осознанное перекрытие члена
    DimCtxScope dimScope(dimIndex);

    auto players = getAllPlayersCopy();
    auto broadcast = [&](i32 x, i32 y, i32 z, i32 st) {
        net::Buffer bu; bu.writePosition(BlockPos{x, y, z}); bu.writeVarInt(st);
        auto vec = std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end());
        for (auto& p : players)
            if (p && p->isAlive() && p->playReady && p->dimension == dimIndex && p->getState() == entity::PlayerState::Play) p->getConnection()->sendPacket(0x09, vec); // DIMSYNC_V1 / JOINSAFE_V1
    };

    // ==========================================================================
    // FLUID_V2: перенос ванильного FlowingFluid 1:1 (client+server: FlowingFluid.java,
    // WaterFluid.java, LavaFluid.java). Вода: dropOff=1, slopeFind=4, tick=5.
    // Лава(оверворлд): dropOff=2, slopeFind=2, tick=30.
    // Блок-стейт LiquidBlock.LEVEL = 8 - min(amount,8) + (falling?8:0); источник = level 0.
    // => water 80=источник, 81..87=поток(amount 7..1), 88=падающий; lava 96/97..103/104.
    // ==========================================================================
    auto sid     = [&](i32 X, i32 Y, i32 Z) { return world_.getBlock(X, Y, Z); };
    auto isWater = [](i32 s) { return s >= 80 && s <= 95; };
    auto isLava  = [](i32 s) { return s >= 96 && s <= 111; };
    auto stateInfo = [&](i32 s) -> const registries::BlockState* {
        return registries::RegistryManager::instance().blockStates().getById(s);
    };
    auto isNonMotionBlock = [&](i32 s) {
        if (s == 0 || isWater(s) || isLava(s)) return true;
        const auto* st = stateInfo(s);
        if (!st) return false;
        const std::string_view n = st->name;
        auto has = [&](std::string_view part) { return n.find(part) != std::string_view::npos; };
        auto ends = [&](std::string_view suffix) {
            return n.size() >= suffix.size() && n.substr(n.size() - suffix.size()) == suffix;
        };
        if (!st->isSolid()) return true;
        if (ends("_sapling") || ends("_tulip") || ends("_orchid") || ends("_torch") ||
            ends("_rail") || ends("_button") || ends("_pressure_plate") || ends("_carpet") ||
            ends("_coral") || ends("_coral_fan") || ends("_candle")) return true;
        return n == "minecraft:mangrove_propagule" || n == "minecraft:short_grass" ||
               n == "minecraft:tall_grass" || n == "minecraft:fern" || n == "minecraft:large_fern" ||
               n == "minecraft:dead_bush" || n == "minecraft:dandelion" || n == "minecraft:poppy" ||
               n == "minecraft:allium" || n == "minecraft:azure_bluet" || n == "minecraft:oxeye_daisy" ||
               n == "minecraft:cornflower" || n == "minecraft:lily_of_the_valley" ||
               n == "minecraft:wither_rose" || n == "minecraft:torchflower" ||
               n == "minecraft:sunflower" || n == "minecraft:lilac" || n == "minecraft:rose_bush" ||
               n == "minecraft:peony" || n == "minecraft:pink_petals" ||
               n == "minecraft:brown_mushroom" || n == "minecraft:red_mushroom" ||
               n == "minecraft:crimson_roots" || n == "minecraft:warped_roots" ||
               n == "minecraft:nether_sprouts" || n == "minecraft:seagrass" ||
               n == "minecraft:tall_seagrass" || n == "minecraft:kelp" || n == "minecraft:kelp_plant" ||
               n == "minecraft:vine" || n == "minecraft:glow_lichen" ||
               has("weeping_vines") || has("twisting_vines") ||
               n == "minecraft:wheat" || n == "minecraft:carrots" || n == "minecraft:potatoes" ||
               n == "minecraft:beetroots" || n == "minecraft:nether_wart" || n == "minecraft:cocoa" ||
               has("_stem") || n == "minecraft:redstone_wire" || n == "minecraft:tripwire" ||
               n == "minecraft:fire" || n == "minecraft:soul_fire" || n == "minecraft:cobweb" ||
               n == "minecraft:snow";
    };
    auto isSolid = [&](i32 s) {
        return !isNonMotionBlock(s);
    };
    auto sameFam = [&](i32 s, i32 b) { return b == 80 ? isWater(s) : isLava(s); };
    auto isSrc   = [&](i32 s, i32 b) { return s == b; };
    auto isFall  = [&](i32 s, i32 b) { return sameFam(s, b) && (s - b) >= 8; };
    auto amountOf = [&](i32 s, i32 b) -> i32 { // FluidState.getAmount (1..8)
        if (s == b) return 8;                   // источник
        const i32 lv = s - b;
        if (lv >= 8) return 8;                  // падающий столб
        return 8 - lv;                          // поток: level 1..7 -> amount 7..1
    };
    auto mkState = [&](i32 b, i32 amount, bool falling) -> i32 { // -> block-state (getLegacyLevel)
        if (!falling && amount >= 8) return b;  // источник
        const i32 lvl = 8 - std::min(amount, 8) + (falling ? 8 : 0);
        return b + lvl;
    };
    auto canHoldFluid = [&](i32 s, i32 /*b*/) {
        if (s == 0) return true;
        if (isWater(s) || isLava(s)) return false;
        const auto* st = stateInfo(s);
        if (!st) return false;
        const std::string_view n = st->name;
        auto has = [&](std::string_view part) { return n.find(part) != std::string_view::npos; };

        // FlowingFluid.canHoldFluid(): специальные vanilla-запреты.
        if (has("_door") || has("_sign") || n == "minecraft:ladder" ||
            n == "minecraft:sugar_cane" || n == "minecraft:bubble_column" ||
            n == "minecraft:nether_portal" || n == "minecraft:end_portal" ||
            n == "minecraft:end_gateway" || n == "minecraft:structure_void") return false;
        return isNonMotionBlock(s);
    };
    // FluidState.canBeReplacedWith: одно семейство напрямую не перезаписывается.
    auto canPassSide = [&](i32 s, i32 b) { return !sameFam(s, b) && canHoldFluid(s, b); };
    // isWaterHole: под клеткой можно провалиться (в��здух/та же жидкость) — клетка «над ямой».
    auto belowIsHole = [&](i32 X, i32 Y, i32 Z, i32 b) {
        const i32 bs = sid(X, Y - 1, Z);
        return sameFam(bs, b) || canHoldFluid(bs, b);
    };

    // getNewLiquid: пересчитать желаемую жидкость В ДАННОЙ клетке из соседей. 0 = пусто.
    auto getNewLiquid = [&](i32 X, i32 Y, i32 Z, i32 b) -> i32 {
        const i32 dropOff = (b == 80) ? 1 : 2;
        i32 maxAmount = 0, srcN = 0;
        auto scan = [&](i32 nx, i32 ny, i32 nz) {
            const i32 ns = sid(nx, ny, nz);
            if (!sameFam(ns, b)) return;
            if (isSrc(ns, b)) ++srcN;
            const i32 a = amountOf(ns, b);
            if (a > maxAmount) maxAmount = a;
        };
        scan(X + 1, Y, Z); scan(X - 1, Y, Z); scan(X, Y, Z + 1); scan(X, Y, Z - 1);
        // Преобразование в источник — только вода (waterSourceConversion=true):
        // 2+ соседних источника + опора снизу (твёрдый блок или источник той же воды).
        if (b == 80 && srcN >= 2) {
            const i32 belowS = sid(X, Y - 1, Z);
            if (isSolid(belowS) || isSrc(belowS, 80)) return b;
        }
        // Жидкость сверху -> падающий столб (amount 8, falling).
        if (sameFam(sid(X, Y + 1, Z), b)) return mkState(b, 8, true);
        const i32 n3 = maxAmount - dropOff;
        if (n3 <= 0) return 0;
        return mkState(b, n3, false);
    };

    // getSlopeDistance: рекурсивный поиск дистанции до ближайшей ямы (лимит slopeFind).
    std::function<i32(i32,i32,i32,i32,i32,i32)> slopeDist =
        [&](i32 X, i32 Y, i32 Z, i32 n, i32 skipDir, i32 b) -> i32 {
        const i32 slopeFind = (b == 80) ? 4 : 2;
        const i32 DX[4] = {1,-1,0,0}, DZ[4] = {0,0,1,-1}, OPP[4] = {1,0,3,2};
        i32 best = 1000;
        for (i32 d = 0; d < 4; ++d) {
            if (d == skipDir) continue;
            const i32 tx = X + DX[d], tz = Z + DZ[d];
            if (!canPassSide(sid(tx, Y, tz), b)) continue;
            if (belowIsHole(tx, Y, tz, b)) return n;
            if (n < slopeFind) {
                const i32 dd = slopeDist(tx, Y, tz, n + 1, OPP[d], b);
                if (dd < best) best = dd;
            }
        }
        return best;
    };

    // getSpread: как в vanilla, каждая целевая клетка получает СОБСТВЕННЫЙ
    // getNewLiquid(), а не один скопированный уровень исходной клетки.
    auto getSpread = [&](i32 X, i32 Y, i32 Z, i32 b, i32 spreadStates[4]) {
        const i32 DX[4] = {1,-1,0,0}, DZ[4] = {0,0,1,-1}, OPP[4] = {1,0,3,2};
        for (i32 d = 0; d < 4; ++d) spreadStates[d] = 0;
        i32 best = 1000;
        for (i32 d = 0; d < 4; ++d) {
            const i32 tx = X + DX[d], tz = Z + DZ[d];
            if (!canPassSide(sid(tx, Y, tz), b)) continue;
            const i32 candidate = getNewLiquid(tx, Y, tz, b);
            if (candidate == 0) continue;
            const i32 dist = belowIsHole(tx, Y, tz, b) ? 0 : slopeDist(tx, Y, tz, 1, OPP[d], b);
            if (dist < best) {
                best = dist;
                for (i32 j = 0; j < 4; ++j) spreadStates[j] = 0;
            }
            if (dist == best) spreadStates[d] = candidate;
        }
    };

    // place: setBlock + рассылка 0x09 + переплани��овать соседей.
    auto place = [&](i32 X, i32 Y, i32 Z, i32 st, i32 selfDelay = -1) {
        if (world_.getBlock(X, Y, Z) == st) return;
        world_.setBlock(X, Y, Z, st);
        broadcast(X, Y, Z, st);
        scheduleFallingBlockUpdate(X, Y + 1, Z, 2); // fluid may remove/support a falling block
        scheduleFallingBlockUpdate(X, Y, Z, 2);
        scheduleFallingColumnCascade(X, Y + 1, Z, 2);
        scheduleFluidUpdate(X, Y, Z, selfDelay);
        scheduleFluidUpdate(X + 1, Y, Z); scheduleFluidUpdate(X - 1, Y, Z);
        scheduleFluidUpdate(X, Y, Z + 1); scheduleFluidUpdate(X, Y, Z - 1);
        scheduleFluidUpdate(X, Y + 1, Z); scheduleFluidUpdate(X, Y - 1, Z);
        // ConcretePowderBlock.updateShape: newly arrived water hardens adjacent
        // stationary powder immediately. This also stops fluid/falling oscillation.
        static constexpr i32 DX[7] = {0, 1, -1, 0, 0, 0, 0};
        static constexpr i32 DY[7] = {0, 0, 0, 0, 0, 1, -1};
        static constexpr i32 DZ[7] = {0, 0, 0, 1, -1, 0, 0};
        for (i32 i = 0; i < 7; ++i) {
            const i32 px = X + DX[i], py = Y + DY[i], pz = Z + DZ[i];
            const i32 ps = world_.getBlock(px, py, pz);
            if (!isConcretePowderState(ps) ||
                !concretePowderShouldSolidify(world_, px, py, pz, ps)) continue;
            const i32 concrete = concreteFromPowder(ps);
            world_.setBlock(px, py, pz, concrete);
            broadcast(px, py, pz, concrete);
            scheduleFallingBlockUpdate(px, py + 1, pz, 2);
            scheduleFallingColumnCascade(px, py + 1, pz, 2);
            scheduleFluidUpdate(px, py, pz, 1);
        }
    };
    auto sourceNeighbors = [&](i32 X, i32 Y, i32 Z, i32 b) {
        i32 n = 0;
        if (isSrc(sid(X+1,Y,Z), b)) ++n; if (isSrc(sid(X-1,Y,Z), b)) ++n;
        if (isSrc(sid(X,Y,Z+1), b)) ++n; if (isSrc(sid(X,Y,Z-1), b)) ++n;
        return n;
    };

    // spread: ванильный порядок — сначала ВНИЗ, вбок только если вниз нельзя (или 3+ источника).
    auto spread = [&](i32 X, i32 Y, i32 Z, i32 b, i32 cur) {
        const i32 dropOff = (b == 80) ? 1 : 2;
        const i32 belowS = sid(X, Y - 1, Z);
        // LavaFluid.spreadTo(DOWN): падающая лава в воде создаёт stone.
        if (b == 96 && isWater(belowS)) {
            place(X, Y - 1, Z, 1);
            return;
        }
        // Ваниль не перезаписывает уже существующий FluidState того же семейства:
        // нижняя клетка пересчитает себя собственным scheduled tick.
        const bool canDown = !sameFam(belowS, b) && canHoldFluid(belowS, b);
        bool doSides = false;
        if (canDown) {
            place(X, Y - 1, Z, mkState(b, 8, true)); // падающий столб вниз
            if (sourceNeighbors(X, Y, Z, b) >= 3) doSides = true;
        } else if (isSrc(cur, b) || !belowIsHole(X, Y, Z, b)) {
            doSides = true;
        }
        if (!doSides) return;
        i32 n = isFall(cur, b) ? 7 : (amountOf(cur, b) - dropOff);
        if (n <= 0) return;
        i32 spreadStates[4]; getSpread(X, Y, Z, b, spreadStates);
        const i32 DX[4] = {1,-1,0,0}, DZ[4] = {0,0,1,-1};
        for (i32 d = 0; d < 4; ++d) {
            if (spreadStates[d] == 0) continue;
            const i32 tx = X + DX[d], tz = Z + DZ[d];
            const i32 ts = sid(tx, Y, tz);
            // FluidState.canBeReplacedWith запрещает воде/лаве напрямую затирать
            // более слабое состояние той же жидкости. Оно обновится pull-пересчётом.
            if (!sameFam(ts, b) && canHoldFluid(ts, b)) place(tx, Y, tz, spreadStates[d]);
        }
    };

    for (const u64 k : batch) {
        i32 x, y, z; fluidUnkey(k, x, y, z);
        if (!world_.getChunk(x >> 4, z >> 4)) continue;
        const i32 cur = world_.getBlock(x, y, z);
        const i32 b = isWater(cur) ? 80 : (isLava(cur) ? 96 : -1);
        if (b < 0) continue; // не жидкость — её зальёт сосед при разливе

        // Лава + вода = затвердевание (источник->обсидиан, поток->булыжник).
        if (b == 96) {
            // LiquidBlock.POSSIBLE_FLOW_DIRECTIONS: четыре стороны + UP, но не DOWN.
            const bool touchWater = isWater(sid(x+1,y,z)) || isWater(sid(x-1,y,z)) ||
                                    isWater(sid(x,y,z+1)) || isWater(sid(x,y,z-1)) ||
                                    isWater(sid(x,y+1,z));
            if (touchWater) { place(x, y, z, (cur == 96) ? 2354 : 14); continue; }
        }

        // tick(): не-источник пересчитывает себя; затем всегда spread().
        i32 state = cur;
        if (!isSrc(cur, b)) {
            const i32 nl = getNewLiquid(x, y, z, b);
            if (nl == 0) { place(x, y, z, 0); continue; } // высохла -> воздух
            if (nl != cur) {
                i32 delay = -1;
                if (b == 96 && !isFall(cur, b) && !isFall(nl, b) &&
                    amountOf(nl, b) > amountOf(cur, b)) {
                    // LavaFluid.getSpreadDelay(): в 3 случаях из 4 более высокий
                    // новый уровень ждёт 4 базовых задержки (30*4 в overworld).
                    const u64 rnd = fluidKey(x, y, z) ^ (static_cast<u64>(tickCounter_) * 0x9E3779B97F4A7C15ull);
                    if ((rnd & 3ull) != 0) delay = 120;
                }
                place(x, y, z, nl, delay);
                state = nl;
            }
        }
        spread(x, y, z, b, state);
    }
}


// FIRE_V2: bounded scheduled fire ticks. This deliberately avoids scanning chunks:
// memory and CPU scale only with currently burning cells.
void NetherCraftServer::scheduleFireUpdate(i32 x, i32 y, i32 z, i32 delay) {
    if (y < world::CHUNK_HEIGHT_MIN || y >= world::CHUNK_HEIGHT_MAX) return;
    const u64 k = fluidKey(x, y, z);
    const i32 due = tickCounter_ + std::max(1, delay);
    std::lock_guard lk(fireMutex_);
    auto it = fireDue_.find(k);
    if (it != fireDue_.end() && it->second <= due) return;
    fireDue_[k] = due;
    fireQueue_.emplace(due, k);
}

void NetherCraftServer::tickFire() { // DIMPHYS_V1
    std::vector<u64> collected;
    {
        std::lock_guard lk(fireMutex_);
        constexpr size_t LIMIT = 1024;
        while (!fireQueue_.empty() && collected.size() < LIMIT && fireQueue_.begin()->first <= tickCounter_) {
            const auto [due, k] = *fireQueue_.begin();
            fireQueue_.erase(fireQueue_.begin());
            auto it = fireDue_.find(k);
            if (it == fireDue_.end() || it->second != due) continue;
            fireDue_.erase(it);
            collected.push_back(k);
        }
    }
    if (collected.empty()) return;
    std::vector<u64> perDim[3];
    for (const u64 k : collected) perDim[dimOfKey(k) % 3].push_back(k);
    for (i32 d = 0; d < 3; ++d) {
        if (perDim[d].empty()) continue;
        if ((d == 1 && !netherReady_) || (d == 2 && !endReady_)) continue;
        tickFireIn(d, perDim[d]);
    }
}

void NetherCraftServer::tickFireIn(i32 dimIndex, const std::vector<u64>& batch) {
    world::World& world_ = worldFor(dimIndex); // C4458: осознанное перекрытие члена
    DimCtxScope dimScope(dimIndex);
    auto players = getAllPlayersCopy();
    auto broadcast = [&](i32 x, i32 y, i32 z, i32 state) {
        net::Buffer b; b.writePosition(BlockPos{x,y,z}); b.writeVarInt(state);
        const auto bytes = std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end());
        for (auto& p : players)
            if (p && p->isAlive() && p->playReady && p->dimension == dimIndex && p->getState() == entity::PlayerState::Play) // DIMSYNC_V1 / JOINSAFE_V1
                p->getConnection()->sendPacket(0x09, bytes);
    };
    auto isFire = [](i32 s) { return (s >= 2391 && s < 2872) || s == 2872; };
    auto isWater = [](i32 s) { return (s >= 80 && s <= 95) || s == 12960 || s == 12961; };
    auto flammable = [&](i32 state) {
        if (state == 2095) return true; // TNT
        const auto* bs = registries::RegistryManager::instance().blockStates().getById(state);
        if (!bs) return false;
        const std::string_view n = bs->name;
        constexpr std::string_view parts[] = {
            "_planks", "_log", "_wood", "_leaves", "_wool", "_carpet",
            "bookshelf", "hay_block", "bamboo", "scaffolding", "vine",
            "beehive", "bee_nest"
        };
        for (auto part : parts) if (n.find(part) != std::string_view::npos) return true;
        // Never match by the substring "grass": grass_block is not flammable.
        // Only the replaceable plants themselves may burn.
        return n == "minecraft:short_grass" || n == "minecraft:tall_grass" ||
               n == "minecraft:fern" || n == "minecraft:large_fern" ||
               n == "minecraft:dead_bush" || n == "minecraft:brown_mushroom" ||
               n == "minecraft:red_mushroom" || n == "minecraft:dandelion" ||
               n == "minecraft:poppy" || n == "minecraft:allium" ||
               n == "minecraft:azure_bluet" || n == "minecraft:oxeye_daisy" ||
               n == "minecraft:cornflower" || n == "minecraft:lily_of_the_valley" ||
               n == "minecraft:wither_rose" || n == "minecraft:torchflower";
    };
    constexpr i32 dirs[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (u64 k : batch) {
        i32 x,y,z; fluidUnkey(k,x,y,z);
        const i32 state = world_.getBlock(x,y,z);
        if (!isFire(state)) { std::lock_guard lk(fireMutex_); fireAge_.erase(k); continue; }
        const i32 below = world_.getBlock(x,y-1,z);
        if (state == 2872) { // soul fire is stable only on soul soil/sand
            if (below != 5850 && below != 5851) { world_.setBlock(x,y,z,0); broadcast(x,y,z,0); }
            else scheduleFireUpdate(x,y,z,40);
            continue;
        }
        bool wet = false, hasFuel = false;
        for (auto& d : dirs) {
            const i32 s = world_.getBlock(x+d[0],y+d[1],z+d[2]);
            wet |= isWater(s); hasFuel |= flammable(s);
        }
        u8 age;
        { std::lock_guard lk(fireMutex_); age = fireAge_[k] = std::min<int>(15, fireAge_[k] + 1); }
        const bool support = below != 0 && !isWater(below) && !isFire(below);
        const auto* belowInfo = registries::RegistryManager::instance().blockStates().getById(below);
        const std::string_view belowName = belowInfo ? std::string_view(belowInfo->name) : std::string_view{};
        const bool eternalSupport = belowName.find("netherrack") != std::string_view::npos ||
                                    belowName.find("magma_block") != std::string_view::npos;
        if (wet || (!support && !hasFuel) || (age >= 4 && !hasFuel && !eternalSupport) ||
            (age >= 15 && !eternalSupport)) {
            world_.setBlock(x,y,z,0); broadcast(x,y,z,0);
            std::lock_guard lk(fireMutex_); fireAge_.erase(k);
            continue;
        }
        for (i32 i=0;i<6;++i) {
            const i32 nx=x+dirs[i][0], ny=y+dirs[i][1], nz=z+dirs[i][2];
            const i32 ns=world_.getBlock(nx,ny,nz);
            const u64 h = k ^ (static_cast<u64>(tickCounter_)*0x9E3779B97F4A7C15ull) ^ static_cast<u64>(i*131+age);
            if (ns == 2095 && (h % 3u)==0u) { primeTntBlock(nx,ny,nz,0,80); continue; }
            if (flammable(ns) && (h % 4u)==0u) {
                world_.setBlock(nx,ny,nz,2391); broadcast(nx,ny,nz,2391); scheduleFireUpdate(nx,ny,nz,30);
            } else if (ns == 0 && (h % 7u)==0u) {
                bool nearFuel=false;
                for (auto& e:dirs) nearFuel |= flammable(world_.getBlock(nx+e[0],ny+e[1],nz+e[2]));
                if (nearFuel) { world_.setBlock(nx,ny,nz,2391); broadcast(nx,ny,nz,2391); scheduleFireUpdate(nx,ny,nz,30); }
            }
        }
        scheduleFireUpdate(x,y,z,30 + static_cast<i32>((k + tickCounter_) % 10));
    }
}

// RANDOM_TICK_V1: vanilla-style random block ticks -- crop growth (wheat, carrots,
// potatoes, beetroots, nether wart), sugar cane / cactus stalk growth, leaf decay,
// and farmland hydration/decay. Samples 3 random positions per loaded chunk column
// per tick (matches vanilla's default randomTickSpeed=3 per subchunk, collapsed to
// whole columns here). Grass/mycelium spread and ice/snow melting are intentionally
// NOT included: vanilla gates those on per-block light values, and this server has
// no queryable block-light data yet -- see PHYSICS_PROGRESS.txt for that gap.
void NetherCraftServer::tickRandomBlockUpdates() { // DIMPHYS_V1: рандом-тики во всех измерениях
    tickRandomBlockUpdatesIn(0);
    if (netherReady_) tickRandomBlockUpdatesIn(1);
    if (endReady_)    tickRandomBlockUpdatesIn(2);
}

void NetherCraftServer::tickRandomBlockUpdatesIn(i32 dimIndex) {
    world::World& world_ = worldFor(dimIndex); // C4458: осознанное перекрытие члена
    DimCtxScope dimScope(dimIndex);
    static std::mt19937 rng{0xC0FFEEu ^ static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count())};
    std::uniform_int_distribution<i32> coordDist(0, 15);
    std::uniform_int_distribution<i32> chanceDist(0, 999);
    std::uniform_int_distribution<i32> heightDist(world::CHUNK_HEIGHT_MIN, world::CHUNK_HEIGHT_MAX - 1);

    auto& reg = registries::RegistryManager::instance();
    auto isAir = [](i32 s) { return s == 0 || s == 12958 || s == 12959; };
    auto isWaterState = [](i32 s) { return (s >= 80 && s <= 95) || s == 12960 || s == 12961; };

    auto players = getAllPlayersCopy();
    auto broadcast = [&](i32 x, i32 y, i32 z, i32 state) {
        net::Buffer b; b.writePosition(BlockPos{x,y,z}); b.writeVarInt(state);
        const auto bytes = std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end());
        for (auto& p : players)
            if (p && p->isAlive() && p->playReady && p->dimension == dimIndex && p->getState() == entity::PlayerState::Play) // DIMSYNC_V1 / JOINSAFE_V1
                p->getConnection()->sendPacket(0x09, bytes);
    };

    // CROP_LIGHT_GATE_V1: approximate vanilla's light requirement for crop growth.
    // There is no queryable per-block light value yet, so this checks for direct
    // open-sky exposure (scanning straight up for anything opaque) or a nearby
    // (radius 2) light-emitting block as a stand-in for "light level >= 8".
    // Nether wart is intentionally exempt (it grows in the dark in vanilla too).
    auto isSkyTransparent = [&](i32 s) {
        if (isAir(s)) return true;
        const auto* b = reg.blockStates().getById(s);
        if (!b) return false;
        const std::string& bn = b->name;
        return bn.find("glass") != std::string::npos || bn.find("leaves") != std::string::npos ||
               bn.find("torch") != std::string::npos || bn.find("vine") != std::string::npos ||
               bn.find("bars") != std::string::npos;
    };
    auto hasSkyAccess = [&](i32 sx, i32 sy, i32 sz) {
        for (i32 cy = sy; cy < world::CHUNK_HEIGHT_MAX; ++cy)
            if (!isSkyTransparent(world_.getBlock(sx, cy, sz))) return false;
        return true;
    };
    auto hasNearbyLight = [&](i32 sx, i32 sy, i32 sz) {
        for (i32 dx = -2; dx <= 2; ++dx)
            for (i32 dy = -2; dy <= 2; ++dy)
                for (i32 dz = -2; dz <= 2; ++dz) {
                    const i32 s = world_.getBlock(sx + dx, sy + dy, sz + dz);
                    if (s == 0) continue;
                    const auto* b = reg.blockStates().getById(s);
                    if (!b) continue;
                    const std::string& bn = b->name;
                    if (bn == "minecraft:torch" || bn == "minecraft:wall_torch" ||
                        bn == "minecraft:soul_torch" || bn == "minecraft:soul_wall_torch" ||
                        bn == "minecraft:lantern" || bn == "minecraft:soul_lantern" ||
                        bn == "minecraft:glowstone" || bn == "minecraft:sea_lantern" ||
                        bn == "minecraft:jack_o_lantern" || bn == "minecraft:shroomlight" ||
                        bn == "minecraft:ochre_froglight" || bn == "minecraft:verdant_froglight" ||
                        bn == "minecraft:pearlescent_froglight" || bn == "minecraft:redstone_lamp" ||
                        bn == "minecraft:campfire" || bn == "minecraft:soul_campfire" ||
                        bn == "minecraft:end_rod" || bn == "minecraft:beacon" || bn == "minecraft:fire")
                        return true;
                }
        return false;
    };
    auto hasEnoughLight = [&](i32 sx, i32 sy, i32 sz) {
        return hasSkyAccess(sx, sy, sz) || hasNearbyLight(sx, sy, sz);
    };

    struct AgeCrop { i32 base; i32 maxAge; bool needsLight; };
    // base = state id at age 0 (see core/item_blocks.gen.hpp)
    static const AgeCrop kCrops[] = {
        {4278, 7, true},   // wheat
        {8595, 7, true},   // carrots
        {8603, 7, true},   // potatoes
        {12509, 3, true},  // beetroots
        {7385, 3, false},  // nether_wart (grows in the dark, no light gate)
    };

    for (const auto& [pos, chunkPtr] : world_.getAllChunks()) {
        if (!chunkPtr) continue;
        const i32 baseX = pos.x * 16, baseZ = pos.z * 16;
        for (i32 sample = 0; sample < 3; ++sample) {
            const i32 x = baseX + coordDist(rng);
            const i32 z = baseZ + coordDist(rng);
            const i32 y = heightDist(rng);
            const i32 state = world_.getBlock(x, y, z);
            if (isAir(state)) continue;
            const auto* bs = reg.blockStates().getById(state);
            if (!bs) continue;
            const std::string& name = bs->name;

            bool handledCrop = false;
            for (auto& c : kCrops) {
                if (state < c.base || state > c.base + c.maxAge) continue;
                handledCrop = true;
                const i32 age = state - c.base;
                if (age >= c.maxAge) break;
                const i32 belowState = world_.getBlock(x, y - 1, z);
                i32 growChance = 55; // baseline (nether wart on soul sand, no moisture concept)
                if (belowState >= 4286 && belowState <= 4293) { // farmland, moisture 0..7
                    const i32 moisture = belowState - 4286;
                    // FARMLAND_MOISTURE_V2: moisture 7 grants exactly double the moisture-0
                    // growth chance, scaling linearly in between.
                    growChance = 90 + moisture * 90 / 7;
                }
                if ((!c.needsLight || hasEnoughLight(x, y + 1, z)) && chanceDist(rng) < growChance) {
                    const i32 next = c.base + age + 1;
                    world_.setBlock(x, y, z, next);
                    broadcast(x, y, z, next);
                }
                break;
            }
            if (handledCrop) continue;

            // PLANT_SUPPORT_V1: random ticks also validate supports so plants placed by
            // commands, fluids, or older worlds cannot survive after their support is gone.
            // Each family follows its vanilla support direction; only the unsupported block is
            // removed, never the entire column in one tick.
            {
                const i32 below = world_.getBlock(x, y - 1, z);
                const auto* belowBs = reg.blockStates().getById(below);
                const std::string_view belowName = belowBs ? std::string_view(belowBs->name) : std::string_view{};
                auto removeUnsupported = [&] { world_.setBlock(x, y, z, 0); broadcast(x, y, z, 0); };
                auto isDirtLike = [](std::string_view n) {
                    return n == "minecraft:dirt" || n == "minecraft:grass_block" ||
                        n == "minecraft:podzol" || n == "minecraft:coarse_dirt" ||
                        n == "minecraft:rooted_dirt" || n == "minecraft:moss_block";
                };

                // CROPS_SUPPORT_V1: farmland crops and nether wart need their named soil.
                const bool ordinaryCrop = (state >= 4278 && state <= 4285) ||
                    (state >= 8595 && state <= 8602) || (state >= 8603 && state <= 8610) ||
                    (state >= 12509 && state <= 12512) || name == "minecraft:pumpkin_stem" ||
                    name == "minecraft:melon_stem" || name == "minecraft:torchflower_crop";
                if (ordinaryCrop && !(below >= 4286 && below <= 4293)) { removeUnsupported(); continue; }
                if (name == "minecraft:nether_wart" && belowName != "minecraft:soul_sand" && belowName != "minecraft:soul_soil") {
                    removeUnsupported(); continue;
                }

                // CACTUS_CANE_SUPPORT_V1: upper stalk pieces rest on their own family;
                // base cactus needs sand, while cane additionally needs adjacent water.
                if (name == "minecraft:cactus" || name == "minecraft:sugar_cane") {
                    bool valid = belowName == name;
                    if (!valid) valid = name == "minecraft:cactus"
                        ? (belowName == "minecraft:sand" || belowName == "minecraft:red_sand")
                        : isDirtLike(belowName) || belowName == "minecraft:sand" || belowName == "minecraft:red_sand";
                    if (name == "minecraft:sugar_cane" && valid && belowName != name) {
                        valid = false;
                        constexpr i32 kSides[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
                        for (auto& d : kSides)
                            if (isWaterState(world_.getBlock(x + d[0], y - 1, z + d[1]))) { valid = true; break; }
                    }
                    if (name == "minecraft:cactus" && valid) {
                        constexpr i32 kSides[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
                        for (auto& d : kSides) if (!isAir(world_.getBlock(x + d[0], y, z + d[1]))) { valid = false; break; }
                    }
                    if (!valid) { removeUnsupported(); continue; }
                }

                // KELP_SUPPORT_V1: every segment stays submerged and either rests on a
                // kelp segment or on a solid seabed. It breaks once drained.
                if (name == "minecraft:kelp" || name == "minecraft:kelp_plant") {
                    bool wet = false;
                    constexpr i32 kFaces[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
                    for (auto& f : kFaces)
                        if (isWaterState(world_.getBlock(x + f[0], y + f[1], z + f[2]))) { wet = true; break; }
                    if (!wet) { removeUnsupported(); continue; }
                    if (belowName != "minecraft:kelp" && belowName != "minecraft:kelp_plant" &&
                        (isAir(below) || isWaterState(below))) { removeUnsupported(); continue; }
                }

                // VINE_SUPPORT_V1: hanging vines need a ceiling, while twisting vines
                // need a floor. Their body variants obey the same one-sided support rule.
                if (name == "minecraft:weeping_vines" || name == "minecraft:weeping_vines_plant" ||
                    name == "minecraft:cave_vines" || name == "minecraft:cave_vines_plant") {
                    if (isAir(world_.getBlock(x, y + 1, z))) { removeUnsupported(); continue; }
                }
                if (name == "minecraft:twisting_vines" || name == "minecraft:twisting_vines_plant") {
                    if (isAir(below)) { removeUnsupported(); continue; }
                }

                // COCOA_BERRY_SUPPORT_V1: cocoa remains attached to its jungle log and berry
                // bushes remain on dirt-like ground. Cocoa's facing points away from its log.
                if (name == "minecraft:cocoa") {
                    auto faceIt = bs->properties.find("facing");
                    const std::string face = faceIt != bs->properties.end() ? faceIt->second : "north";
                    i32 sx = x, sz = z;
                    if (face == "north") ++sz; else if (face == "south") --sz;
                    else if (face == "west") ++sx; else if (face == "east") --sx;
                    const auto* supportBs = reg.blockStates().getById(world_.getBlock(sx, y, sz));
                    const std::string_view supportName = supportBs ? std::string_view(supportBs->name) : std::string_view{};
                    if (supportName != "minecraft:jungle_log" && supportName != "minecraft:stripped_jungle_log" &&
                        supportName != "minecraft:jungle_wood" && supportName != "minecraft:stripped_jungle_wood") {
                        removeUnsupported(); continue;
                    }
                }
                if (name == "minecraft:sweet_berry_bush" && !isDirtLike(belowName)) { removeUnsupported(); continue; }

                // LILYPAD_SUPPORT_V1: lily pads only float on a water surface.
                if (name == "minecraft:lily_pad" && !isWaterState(below)) { removeUnsupported(); continue; }

                // SEAGRASS_SUPPORT_V1: sea grass must remain submerged. Tall sea grass
                // additionally requires a sea-grass lower half immediately beneath it.
                if (name == "minecraft:seagrass" || name == "minecraft:tall_seagrass") {
                    bool wet = isWaterState(world_.getBlock(x + 1, y, z)) || isWaterState(world_.getBlock(x - 1, y, z)) ||
                        isWaterState(world_.getBlock(x, y, z + 1)) || isWaterState(world_.getBlock(x, y, z - 1)) ||
                        isWaterState(world_.getBlock(x, y + 1, z));
                    if (!wet || (name == "minecraft:tall_seagrass" && belowName != "minecraft:seagrass" && belowName != "minecraft:tall_seagrass")) {
                        removeUnsupported(); continue;
                    }
                }

                // TURTLE_EGG_SUPPORT_V1 / SNIFFER_EGG_SUPPORT_V1: eggs cannot remain
                // suspended after their nest block has been removed.
                if (name == "minecraft:turtle_egg") {
                    if (belowName != "minecraft:sand" && belowName != "minecraft:red_sand" &&
                        belowName != "minecraft:suspicious_sand" && belowName != "minecraft:suspicious_gravel") {
                        removeUnsupported(); continue;
                    }
                }
                if (name == "minecraft:sniffer_egg" && !isDirtLike(belowName)) { removeUnsupported(); continue; }

                // BAMBOO_SUPPORT_V1: each stalk segment rests on bamboo below, while the
                // base accepts the same soil family used by vanilla placement.
                if (name == "minecraft:bamboo" && belowName != "minecraft:bamboo" &&
                    !isDirtLike(belowName) && belowName != "minecraft:sand" && belowName != "minecraft:red_sand") {
                    removeUnsupported(); continue;
                }

                // MUSHROOM_LIGHT_DECAY_V1: mushrooms cannot survive in bright open space.
                // Mycelium/podzol keep the normal dark-growth behaviour but do not bypass
                // the light check, matching vanilla's low-light survival rule.
                if ((name == "minecraft:brown_mushroom" || name == "minecraft:red_mushroom") &&
                    hasEnoughLight(x, y + 1, z) && chanceDist(rng) < 80) { removeUnsupported(); continue; }

                // CHORUS_SUPPORT_V1: a flower detached from end stone/chorus plant drops
                // instead of waiting forever for its growth branch to notice the invalid root.
                if (name == "minecraft:chorus_flower" && belowName != "minecraft:end_stone" && belowName != "minecraft:chorus_plant") {
                    removeUnsupported(); continue;
                }

                // GROUND_PLANT_SUPPORT_V2: simple overworld foliage has no persistent
                // attachment of its own; random ticks remove it after soil disappears.
                // The upper halves of double plants are retained through their matching
                // lower-half block directly below.
                const bool grassPlant = name == "minecraft:short_grass" || name == "minecraft:tall_grass";
                if (grassPlant && belowName != name && !isDirtLike(belowName)) { removeUnsupported(); continue; }

                const bool fernPlant = name == "minecraft:fern" || name == "minecraft:large_fern";
                if (fernPlant && belowName != name && !isDirtLike(belowName)) { removeUnsupported(); continue; }

                const bool singleFlower = name == "minecraft:dandelion" || name == "minecraft:poppy" ||
                    name == "minecraft:blue_orchid" || name == "minecraft:allium" || name == "minecraft:azure_bluet" ||
                    name == "minecraft:red_tulip" || name == "minecraft:orange_tulip" || name == "minecraft:white_tulip" ||
                    name == "minecraft:pink_tulip" || name == "minecraft:oxeye_daisy" || name == "minecraft:cornflower" ||
                    name == "minecraft:lily_of_the_valley" || name == "minecraft:wither_rose";
                if (singleFlower && !isDirtLike(belowName)) { removeUnsupported(); continue; }

                const bool tallFlower = name == "minecraft:sunflower" || name == "minecraft:lilac" ||
                    name == "minecraft:rose_bush" || name == "minecraft:peony";
                if (tallFlower && belowName != name && !isDirtLike(belowName)) { removeUnsupported(); continue; }

                if (name == "minecraft:pink_petals" && !isDirtLike(belowName)) { removeUnsupported(); continue; }

                // DEAD_BUSH_SUPPORT_V1: dry shrubs survive only on dry natural terrain.
                if (name == "minecraft:dead_bush" && belowName != "minecraft:sand" && belowName != "minecraft:red_sand" &&
                    belowName != "minecraft:terracotta" && belowName.find("terracotta") == std::string_view::npos) {
                    removeUnsupported(); continue;
                }

                // NETHER_FLORA_SUPPORT_V1: roots/sprouts use their matching nylium.
                if (name == "minecraft:crimson_roots" && belowName != "minecraft:crimson_nylium" && belowName != "minecraft:soul_soil") {
                    removeUnsupported(); continue;
                }
                if (name == "minecraft:warped_roots" && belowName != "minecraft:warped_nylium" && belowName != "minecraft:soul_soil") {
                    removeUnsupported(); continue;
                }
                if (name == "minecraft:nether_sprouts" && belowName != "minecraft:warped_nylium") { removeUnsupported(); continue; }

                // CEILING_PLANT_SUPPORT_V1: hanging roots and spore blossoms cannot float
                // after their ceiling is broken. A non-air ceiling is intentional here: both
                // blocks may hang from many solid block families, not merely stone.
                if (name == "minecraft:hanging_roots" || name == "minecraft:spore_blossom") {
                    if (isAir(world_.getBlock(x, y + 1, z))) { removeUnsupported(); continue; }
                }

                // AZALEA_SUPPORT_V1: azalea bushes need a solid block underneath, matching
                // vanilla's BushBlock canSurvive (any solid full block works, not just soil).
                if ((name == "minecraft:azalea" || name == "minecraft:flowering_azalea") &&
                    !(belowBs && belowBs->isSolid())) {
                    removeUnsupported(); continue;
                }

                // MOSS_CARPET_SUPPORT_V1: moss carpet floats without a solid block below.
                if (name == "minecraft:moss_carpet" && !(belowBs && belowBs->isSolid())) {
                    removeUnsupported(); continue;
                }

                // NETHER_FUNGUS_SUPPORT_V1: mirrors the crimson/warped roots rule above —
                // small fungi need their matching nylium or soul soil.
                if (name == "minecraft:crimson_fungus" && belowName != "minecraft:crimson_nylium" && belowName != "minecraft:soul_soil") {
                    removeUnsupported(); continue;
                }
                if (name == "minecraft:warped_fungus" && belowName != "minecraft:warped_nylium" && belowName != "minecraft:soul_soil") {
                    removeUnsupported(); continue;
                }

                // FROGSPAWN_SUPPORT_V1: frogspawn dries out once its supporting block is
                // gone, the same way turtle/sniffer eggs do.
                if (name == "minecraft:frogspawn" && !(belowBs && belowBs->isSolid())) {
                    removeUnsupported(); continue;
                }

                // CANDLE_SUPPORT_V1: every candle color (matched by suffix, since black_candle
                // is intentionally excluded from property registration — see
                // CANDLE_LANTERN_STATE_V1 in registry.cpp) pops off without solid ground
                // beneath it.
                if (name.size() > 7 && name.compare(name.size() - 7, 7, "_candle") == 0 &&
                    !(belowBs && belowBs->isSolid())) {
                    removeUnsupported(); continue;
                }

                // LANTERN_SUPPORT_V1: a hanging lantern needs a solid ceiling above it; a
                // standing lantern needs a solid floor below it.
                if (name == "minecraft:lantern" || name == "minecraft:soul_lantern") {
                    auto hangIt = bs->properties.find("hanging");
                    const bool hanging = hangIt != bs->properties.end() && hangIt->second == "true";
                    const auto* aboveBs = reg.blockStates().getById(world_.getBlock(x, y + 1, z));
                    const bool ok = hanging ? (aboveBs && aboveBs->isSolid()) : (belowBs && belowBs->isSolid());
                    if (!ok) { removeUnsupported(); continue; }
                }
            }

            // SAPLING_GROWTH_V1: saplings advance through their two vanilla growth stages and,
            // once mature, attempt to grow into a simple single-trunk tree with a full leaf
            // canopy. Simplification: vanilla dark oak needs a 2x2 sapling cluster; here it
            // grows from a single sapling like the other species (documented limitation).
            {
                struct SaplingSpecies { const char* logName; const char* leavesName; i32 base; i32 trunkHeight; };
                static const SaplingSpecies kSaplings[] = {
                    {"minecraft:oak_log", "minecraft:oak_leaves", 25, 4},
                    {"minecraft:spruce_log", "minecraft:spruce_leaves", 27, 6},
                    {"minecraft:birch_log", "minecraft:birch_leaves", 29, 5},
                    {"minecraft:jungle_log", "minecraft:jungle_leaves", 31, 5},
                    {"minecraft:acacia_log", "minecraft:acacia_leaves", 33, 4},
                    {"minecraft:cherry_log", "minecraft:cherry_leaves", 35, 5},
                    {"minecraft:dark_oak_log", "minecraft:dark_oak_leaves", 37, 5},
                };
                for (const auto& sp : kSaplings) {
                    if (state != sp.base && state != sp.base + 1) continue;
                    handledCrop = true;
                    const i32 stage = state - sp.base;
                    if (!hasEnoughLight(x, y + 1, z) || chanceDist(rng) >= 45) break;
                    if (stage == 0) {
                        world_.setBlock(x, y, z, sp.base + 1);
                        broadcast(x, y, z, sp.base + 1);
                        break;
                    }
                    const i32 maxH = sp.trunkHeight;
                    bool spaceOk = true;
                    for (i32 dy = 1; dy <= maxH + 2 && spaceOk; ++dy)
                        if (!isAir(world_.getBlock(x, y + dy, z))) spaceOk = false;
                    if (!spaceOk) break;
                    auto logState = reg.getBlockStateId(sp.logName, {{"axis", "y"}});
                    auto leafState = reg.getBlockStateId(sp.leavesName, {{"persistent", "true"}, {"distance", "1"}});
                    if (!logState || !leafState) break;
                    for (i32 dy = 0; dy < maxH; ++dy) { world_.setBlock(x, y + dy, z, *logState); broadcast(x, y + dy, z, *logState); }
                    for (i32 ly = maxH - 2; ly <= maxH; ++ly) {
                        for (i32 dx = -1; dx <= 1; ++dx) {
                            for (i32 dz = -1; dz <= 1; ++dz) {
                                if (dx == 0 && dz == 0) continue;
                                if (ly == maxH && std::abs(dx) + std::abs(dz) == 2) continue; // plus-shaped cap
                                const i32 px = x + dx, pz = z + dz;
                                if (isAir(world_.getBlock(px, y + ly, pz))) { world_.setBlock(px, y + ly, pz, *leafState); broadcast(px, y + ly, pz, *leafState); }
                            }
                        }
                    }
                    world_.setBlock(x, y + maxH, z, *leafState);
                    broadcast(x, y + maxH, z, *leafState);
                    break;
                }
                if (handledCrop) continue;
            }

            // SWEET_BERRY_GROWTH_V1: sweet berry bushes age up through their registry "age"
            // property (0..maxAge, resolved at runtime, never assumed offsets). Note: planting
            // and harvest drops via the "sweet_berries" item are intentionally NOT implemented --
            // that item id could not be found anywhere in this server's generated item tables
            // (verified by exhaustive grep) and guessing it risked handing out the wrong item.
            // Growth and the bone-meal boost (see BONE_MEAL_V1) are safe because they only touch
            // the block's own age property, never an item id.
            if (name == "minecraft:sweet_berry_bush") {
                handledCrop = true;
                static const i32 kSweetBerryMaxAge = [] {
                    i32 a = 0;
                    while (registries::RegistryManager::instance().getBlockStateId(
                        "minecraft:sweet_berry_bush", {{"age", std::to_string(a + 1)}}).has_value()) ++a;
                    return a;
                }();
                auto ageIt = bs->properties.find("age");
                const i32 age = ageIt != bs->properties.end() ? std::atoi(ageIt->second.c_str()) : 0;
                if (age < kSweetBerryMaxAge && chanceDist(rng) < 45) {
                    auto next = reg.getBlockStateId("minecraft:sweet_berry_bush", {{"age", std::to_string(age + 1)}});
                    if (next) { world_.setBlock(x, y, z, *next); broadcast(x, y, z, *next); }
                }
                continue;
            }

            // GRASS_SPREAD_V1: dirt spreads into grass_block/mycelium next to a lit spreadable
            // neighbor; grass_block/mycelium die back to dirt when smothered (no light above).
            // This is an approximation of vanilla's spreadable-block light rules using the same
            // hasEnoughLight check already used for crop growth above.
            if (state == 9 || state == 10 || state == 7270) {
                handledCrop = true;
                if (state == 10) {
                    if (hasEnoughLight(x, y + 1, z) && chanceDist(rng) < 40) {
                        constexpr i32 nd[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
                        i32 spreadTo = -1;
                        for (auto& d : nd) {
                            const i32 ns = world_.getBlock(x + d[0], y + d[1], z + d[2]);
                            if (ns == 9) { spreadTo = 9; break; }
                            if (ns == 7270) spreadTo = 7270;
                        }
                        if (spreadTo > 0) { world_.setBlock(x, y, z, spreadTo); broadcast(x, y, z, spreadTo); }
                    }
                } else if (!hasEnoughLight(x, y + 1, z) && chanceDist(rng) < 40) {
                    world_.setBlock(x, y, z, 10);
                    broadcast(x, y, z, 10);
                }
                continue;
            }

            // SNOW_ICE_MELT_V1: snow layers/blocks and regular ice melt away once they receive
            // good light (same hasEnoughLight approximation used above). Packed ice and blue ice
            // are intentionally excluded, matching vanilla's permanent-ice blocks.
            if ((state >= 5772 && state <= 5779) || state == 5780 || state == 5781) {
                handledCrop = true;
                if (hasEnoughLight(x, y + 1, z) && chanceDist(rng) < 40) {
                    if (state == 5780) { world_.setBlock(x, y, z, 80); broadcast(x, y, z, 80); }
                    else if (state == 5781) { world_.setBlock(x, y, z, 0); broadcast(x, y, z, 0); }
                    else if (state == 5772) { world_.setBlock(x, y, z, 0); broadcast(x, y, z, 0); }
                    else { world_.setBlock(x, y, z, state - 1); broadcast(x, y, z, state - 1); }
                }
                continue;
            }

            // STEM_GROWTH_V1: pumpkin_stem / melon_stem grow by age (0..maxAge, read from
            // the live registry rather than assumed offsets -- see PHYSICS_PROGRESS.txt for
            // why offset arithmetic is unsafe for these blocks). Once fully grown, each
            // random tick has a 10% chance to sprout a fruit on a free adjacent side with
            // dirt-like soil beneath it, turning the stem into an attached_*_stem pointing
            // at the new pumpkin/melon -- matches vanilla StemGrownBlock.
            if (name == "minecraft:pumpkin_stem" || name == "minecraft:melon_stem") {
                handledCrop = true;
                const bool isPumpkin = name == "minecraft:pumpkin_stem";
                const char* stemName = isPumpkin ? "minecraft:pumpkin_stem" : "minecraft:melon_stem";
                const char* attachedName = isPumpkin ? "minecraft:attached_pumpkin_stem" : "minecraft:attached_melon_stem";
                const i32 fruitState = isPumpkin ? 6811 : 6812; // pumpkin / melon (no properties)
                auto ageIt = bs->properties.find("age");
                const i32 age = ageIt != bs->properties.end() ? std::atoi(ageIt->second.c_str()) : 0;
                const i32 kStemMaxAge = isPumpkin
                    ? [] { i32 a = 0; while (registries::RegistryManager::instance().getBlockStateId(
                        "minecraft:pumpkin_stem", {{"age", std::to_string(a + 1)}}).has_value()) ++a; return a; }()
                    : [] { i32 a = 0; while (registries::RegistryManager::instance().getBlockStateId(
                        "minecraft:melon_stem", {{"age", std::to_string(a + 1)}}).has_value()) ++a; return a; }();
                const i32 belowState = world_.getBlock(x, y - 1, z);
                i32 growChance = 55;
                if (belowState >= 4286 && belowState <= 4293) {
                    const i32 moisture = belowState - 4286;
                    growChance = 90 + moisture * 90 / 7;
                }
                if (!hasEnoughLight(x, y + 1, z)) { continue; }
                if (age < kStemMaxAge) {
                    if (chanceDist(rng) < growChance) {
                        auto next = reg.getBlockStateId(stemName, {{"age", std::to_string(age + 1)}});
                        if (next) { world_.setBlock(x, y, z, *next); broadcast(x, y, z, *next); }
                    }
                    continue;
                }
                if (chanceDist(rng) >= 100) continue; // 10% chance per tick once fully grown
                constexpr struct { i32 dx, dz; const char* facing; } kDirs[4] = {
                    {0, -1, "north"}, {0, 1, "south"}, {-1, 0, "west"}, {1, 0, "east"},
                };
                const auto& d = kDirs[coordDist(rng) % 4];
                const i32 fx = x + d.dx, fz = z + d.dz;
                const i32 fruitSpot = world_.getBlock(fx, y, fz);
                const i32 fruitBelow = world_.getBlock(fx, y - 1, fz);
                const auto* belowBs = reg.blockStates().getById(fruitBelow);
                const bool soilOk = belowBs && (belowBs->name == "minecraft:dirt" || belowBs->name == "minecraft:grass_block" ||
                    belowBs->name == "minecraft:coarse_dirt" || belowBs->name == "minecraft:podzol" ||
                    (fruitBelow >= 4286 && fruitBelow <= 4293));
                if (isAir(fruitSpot) && soilOk) {
                    world_.setBlock(fx, y, fz, fruitState);
                    broadcast(fx, y, fz, fruitState);
                    auto attached = reg.getBlockStateId(attachedName, {{"facing", d.facing}});
                    if (attached) { world_.setBlock(x, y, z, *attached); broadcast(x, y, z, *attached); }
                }
                continue;
            }

            // TORCHFLOWER_GROWTH_V1: torchflower_crop grows through its age states and, on
            // reaching the last one, replaces itself with a standalone torchflower flower
            // block -- matches vanilla TorchflowerCropBlock.randomTick. maxAge is resolved
            // once at runtime via the registry instead of assumed from raw table offsets.
            if (name == "minecraft:torchflower_crop") {
                handledCrop = true;
                static const i32 kTorchflowerMaxAge = [] {
                    i32 a = 0;
                    while (registries::RegistryManager::instance().getBlockStateId(
                        "minecraft:torchflower_crop", {{"age", std::to_string(a + 1)}}).has_value()) ++a;
                    return a;
                }();
                auto ageIt = bs->properties.find("age");
                const i32 age = ageIt != bs->properties.end() ? std::atoi(ageIt->second.c_str()) : 0;
                if (age >= kTorchflowerMaxAge) continue;
                const i32 belowState = world_.getBlock(x, y - 1, z);
                i32 growChance = 55;
                if (belowState >= 4286 && belowState <= 4293) {
                    const i32 moisture = belowState - 4286;
                    growChance = 90 + moisture * 90 / 7;
                }
                if (!hasEnoughLight(x, y + 1, z)) continue;
                if (chanceDist(rng) < growChance) {
                    if (age + 1 >= kTorchflowerMaxAge) {
                        world_.setBlock(x, y, z, 2076); // minecraft:torchflower (final flower block)
                        broadcast(x, y, z, 2076);
                    } else {
                        auto next = reg.getBlockStateId("minecraft:torchflower_crop", {{"age", std::to_string(age + 1)}});
                        if (next) { world_.setBlock(x, y, z, *next); broadcast(x, y, z, *next); }
                    }
                }
                continue;
            }

            // sugar cane / cactus: vanilla tracks a 0..15 growth counter per stalk and
            // attempts to grow one block taller once it fills up, regardless of outcome.
            if (name == "minecraft:sugar_cane" || name == "minecraft:cactus") {
                const u64 gk = fluidKey(x, y, z);
                u8 age;
                { std::lock_guard lk(fireMutex_); age = ++stalkGrowth_[gk]; }
                if (age < 16) continue;
                { std::lock_guard lk(fireMutex_); stalkGrowth_.erase(gk); }
                const i32 above = world_.getBlock(x, y + 1, z);
                if (!isAir(above)) continue;
                i32 height = 1;
                while (world_.getBlock(x, y - height, z) == state) ++height;
                if (height >= 3) continue;
                if (name == "minecraft:cactus") {
                    constexpr i32 sdirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
                    bool clearSides = true;
                    for (auto& d : sdirs) if (!isAir(world_.getBlock(x+d[0], y+1, z+d[1]))) clearSides = false;
                    if (!clearSides) continue;
                }
                world_.setBlock(x, y + 1, z, state);
                broadcast(x, y + 1, z, state);
                continue;
            }

            // leaf decay: non-persistent leaves whose cached distance-to-log reached the
            // vanilla cutoff (7 = no log found within 6 blocks) decay to air.
            if (name.size() > 7 && name.compare(name.size() - 7, 7, "_leaves") == 0) {
                auto itp = bs->properties.find("persistent");
                const bool persistent = itp != bs->properties.end() && itp->second == "true";
                auto itd = bs->properties.find("distance");
                const i32 distance = itd != bs->properties.end() ? std::atoi(itd->second.c_str()) : 1;
                if (!persistent && distance >= 7) {
                    world_.setBlock(x, y, z, 0);
                    broadcast(x, y, z, 0);
                }
                continue;
            }

            // LAVA_WATER_FORMATION_V1: classic stone generators. Water directly above lava turns
            // it into stone; water touching a lava source turns it into obsidian; water touching
            // flowing lava turns it into cobblestone. Lava states are the contiguous 96..111
            // range with 96 as the source, which is one of the few property sets the registry has
            // always encoded explicitly.
            if (state >= 96 && state <= 111) {
                constexpr i32 kSides[4][3] = {{1,0,0},{-1,0,0},{0,0,1},{0,0,-1}};
                const bool waterAbove = isWaterState(world_.getBlock(x, y + 1, z));
                bool waterBeside = false;
                for (auto& s : kSides)
                    if (isWaterState(world_.getBlock(x + s[0], y + s[1], z + s[2]))) waterBeside = true;
                if (!waterAbove && !waterBeside) continue;
                const char* resultName = waterAbove ? "minecraft:stone"
                    : (state == 96 ? "minecraft:obsidian" : "minecraft:cobblestone");
                auto result = reg.getBlockStateId(resultName, {});
                if (result) { world_.setBlock(x, y, z, *result); broadcast(x, y, z, *result); }
                continue;
            }

            // NETHER_VINES_V1: weeping vines grow downwards from the ceiling, twisting vines grow
            // upwards from the floor. Both use the same shape as cave vines -- an aged head block
            // (age 0..25) plus a plain body block -- so the head turns into a body block and a new
            // head with the next age is placed one block further along.
            if (name == "minecraft:weeping_vines" || name == "minecraft:twisting_vines") {
                const bool growsDown = name == "minecraft:weeping_vines";
                const i32 ny = growsDown ? y - 1 : y + 1;
                if (ny <= world::CHUNK_HEIGHT_MIN || ny >= world::CHUNK_HEIGHT_MAX - 1) continue;
                auto ageIt = bs->properties.find("age");
                const i32 age = ageIt != bs->properties.end() ? std::atoi(ageIt->second.c_str()) : 0;
                if (age >= 25 || chanceDist(rng) >= 20) continue;
                if (!isAir(world_.getBlock(x, ny, z))) continue;
                const std::string plantName = std::string(name) + "_plant";
                auto body = reg.getBlockStateId(plantName, {});
                auto head = reg.getBlockStateId(name, {{"age", std::to_string(age + 1)}});
                if (!body || !head) continue;
                world_.setBlock(x, y, z, *body);
                broadcast(x, y, z, *body);
                world_.setBlock(x, ny, z, *head);
                broadcast(x, ny, z, *head);
                continue;
            }

            // CHORUS_V1: a chorus flower standing on end stone or on its own stem grows upwards,
            // leaving a chorus plant stem behind, and withers into a dead flower once it reaches
            // age 5 or runs out of room.
            // Simplified on purpose: vanilla also grows sideways branches whose shape depends on
            // the surrounding stem geometry. That is not reproduced here -- growth is vertical
            // only -- because the chorus_plant connection properties are not something this code
            // can resolve reliably yet.
            if (name == "minecraft:chorus_flower") {
                auto ageIt = bs->properties.find("age");
                const i32 age = ageIt != bs->properties.end() ? std::atoi(ageIt->second.c_str()) : 0;
                if (chanceDist(rng) >= 30) continue;
                const i32 below = world_.getBlock(x, y - 1, z);
                auto stem = reg.getBlockStateId("minecraft:chorus_plant", {});
                auto endStone = reg.getBlockStateId("minecraft:end_stone", {});
                const bool rooted = (endStone && below == *endStone) || (stem && below == *stem);
                if (!rooted) continue;
                const bool roomAbove = y + 1 < world::CHUNK_HEIGHT_MAX - 1 && isAir(world_.getBlock(x, y + 1, z));
                if (age >= 5 || !roomAbove) {
                    auto dead = reg.getBlockStateId("minecraft:chorus_flower", {{"age", "5"}});
                    if (dead && *dead != state) { world_.setBlock(x, y, z, *dead); broadcast(x, y, z, *dead); }
                    continue;
                }
                auto next = reg.getBlockStateId("minecraft:chorus_flower",
                    {{"age", std::to_string(age + 1)}});
                if (!stem || !next) continue;
                world_.setBlock(x, y, z, *stem);
                broadcast(x, y, z, *stem);
                world_.setBlock(x, y + 1, z, *next);
                broadcast(x, y + 1, z, *next);
                continue;
            }

            // FROSTED_ICE_V1: frost walker ice ages up step by step and melts into water once it
            // runs out of age, melting faster when it has sky access (stand-in for light level).
            if (name == "minecraft:frosted_ice") {
                auto ageIt = bs->properties.find("age");
                const i32 age = ageIt != bs->properties.end() ? std::atoi(ageIt->second.c_str()) : 0;
                const i32 meltChance = hasSkyAccess(x, y + 1, z) ? 400 : 150;
                if (chanceDist(rng) >= meltChance) continue;
                if (age >= 3) {
                    world_.setBlock(x, y, z, 80);
                    broadcast(x, y, z, 80);
                } else {
                    auto next = reg.getBlockStateId("minecraft:frosted_ice",
                        {{"age", std::to_string(age + 1)}});
                    if (next) { world_.setBlock(x, y, z, *next); broadcast(x, y, z, *next); }
                }
                continue;
            }

            // NETHER_WART_V1: nether wart creeps through age 0..3 on soul sand. No light check --
            // vanilla wart ignores light entirely -- but it does require soul sand underneath.
            if (name == "minecraft:nether_wart") {
                if (world_.getBlock(x, y - 1, z) != 5850) continue;
                auto ageIt = bs->properties.find("age");
                const i32 age = ageIt != bs->properties.end() ? std::atoi(ageIt->second.c_str()) : 0;
                if (age >= 3 || chanceDist(rng) >= 100) continue;
                auto next = reg.getBlockStateId("minecraft:nether_wart",
                    {{"age", std::to_string(age + 1)}});
                if (next) { world_.setBlock(x, y, z, *next); broadcast(x, y, z, *next); }
                continue;
            }

            // AMETHYST_GROWTH_V1: an amethyst bud next to budding amethyst grows one stage,
            // small -> medium -> large -> cluster. Each of those four blocks has the same 12
            // states (facing x waterlogged) laid out in the same order, so the growth keeps the
            // bud's own offset inside its id range. That means the facing and waterlogged values
            // are carried over without this code having to know what they mean -- no guessing.
            // Not implemented: budding amethyst spawning brand new buds on its own faces, which
            // would require knowing the exact facing encoding.
            if (state >= 21042 && state <= 21089) {
                constexpr i32 kBudBases[4] = {21078, 21066, 21054, 21042}; // small, medium, large, cluster
                i32 stage = -1;
                for (i32 i = 0; i < 4; ++i)
                    if (state >= kBudBases[i] && state < kBudBases[i] + 12) stage = i;
                if (stage < 0 || stage == 3) continue; // clusters are fully grown
                constexpr i32 kFaces[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
                bool nextToBudding = false;
                for (auto& f : kFaces)
                    if (world_.getBlock(x + f[0], y + f[1], z + f[2]) == 21032) nextToBudding = true;
                if (!nextToBudding || chanceDist(rng) >= 20) continue;
                const i32 offset = state - kBudBases[stage];
                const i32 grown = kBudBases[stage + 1] + offset;
                world_.setBlock(x, y, z, grown);
                broadcast(x, y, z, grown);
                continue;
            }

            // CAVE_VINES_V1: the glow berry vine head grows downwards, converting itself into a
            // vine body block and placing a new head with the next age below it. Heads also grow
            // berries over time. Berry picking is not implemented -- the glow_berries item id is
            // not present in this server's generated item tables, same situation as sweet berries.
            if (name == "minecraft:cave_vines") {
                auto ageIt = bs->properties.find("age");
                auto berriesIt = bs->properties.find("berries");
                const i32 age = ageIt != bs->properties.end() ? std::atoi(ageIt->second.c_str()) : 0;
                const bool berries = berriesIt != bs->properties.end() && berriesIt->second == "true";
                if (!berries && chanceDist(rng) < 12) {
                    auto withBerries = reg.getBlockStateId("minecraft:cave_vines",
                        {{"age", std::to_string(age)}, {"berries", "true"}});
                    if (withBerries) {
                        world_.setBlock(x, y, z, *withBerries);
                        broadcast(x, y, z, *withBerries);
                    }
                    continue;
                }
                if (age >= 25 || chanceDist(rng) >= 25) continue;
                if (!isAir(world_.getBlock(x, y - 1, z))) continue;
                auto body = reg.getBlockStateId("minecraft:cave_vines_plant", {});
                auto head = reg.getBlockStateId("minecraft:cave_vines",
                    {{"age", std::to_string(age + 1)}, {"berries", "false"}});
                if (!body || !head) continue;
                world_.setBlock(x, y, z, *body);
                broadcast(x, y, z, *body);
                world_.setBlock(x, y - 1, z, *head);
                broadcast(x, y - 1, z, *head);
                continue;
            }

            // SPONGE_V1: a dry sponge touching water soaks up a bounded pool of water blocks
            // (breadth-first, capped like vanilla at 64 blocks / 6 block radius) and turns into
            // a wet sponge. A wet sponge sitting next to lava dries back out into a dry sponge.
            // Both sponge ids are single-state blocks (517 dry / 518 wet), so no property
            // resolution is involved and this cannot be affected by the state-variant bug.
            if (state == 517) {
                bool touchesWater = false;
                constexpr i32 kFaces[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
                for (auto& f : kFaces)
                    if (isWaterState(world_.getBlock(x + f[0], y + f[1], z + f[2]))) touchesWater = true;
                if (!touchesWater) continue;

                std::vector<std::array<i32, 3>> queue{{x, y, z}};
                std::set<std::array<i32, 3>> seen{{x, y, z}};
                i32 soaked = 0;
                for (size_t qi = 0; qi < queue.size() && soaked < 64; ++qi) {
                    const auto cur = queue[qi];
                    for (auto& f : kFaces) {
                        const i32 nx = cur[0] + f[0], ny = cur[1] + f[1], nz = cur[2] + f[2];
                        if (std::abs(nx - x) > 6 || std::abs(ny - y) > 6 || std::abs(nz - z) > 6) continue;
                        std::array<i32, 3> key{nx, ny, nz};
                        if (!seen.insert(key).second) continue;
                        if (!isWaterState(world_.getBlock(nx, ny, nz))) continue;
                        world_.setBlock(nx, ny, nz, 0);
                        broadcast(nx, ny, nz, 0);
                        if (++soaked >= 64) break;
                        queue.push_back(key);
                    }
                }
                if (soaked > 0) {
                    world_.setBlock(x, y, z, 518);
                    broadcast(x, y, z, 518);
                }
                continue;
            }
            if (state == 518) {
                bool nextToLava = false;
                constexpr i32 kFaces[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
                for (auto& f : kFaces) {
                    const i32 n = world_.getBlock(x + f[0], y + f[1], z + f[2]);
                    if (n >= 96 && n <= 111) nextToLava = true;
                }
                if (nextToLava) { world_.setBlock(x, y, z, 517); broadcast(x, y, z, 517); }
                continue;
            }

            // CAULDRON_V1: an open-air cauldron slowly fills with water one level at a time and
            // boils back down when lava sits next to it. Levels are resolved through the registry
            // (water_cauldron[level=1..3]) instead of raw id arithmetic.
            if (name == "minecraft:cauldron" || name == "minecraft:water_cauldron") {
                constexpr i32 kFaces[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
                bool nextToLava = false;
                for (auto& f : kFaces) {
                    const i32 n = world_.getBlock(x + f[0], y + f[1], z + f[2]);
                    if (n >= 96 && n <= 111) nextToLava = true;
                }
                if (name == "minecraft:water_cauldron" && nextToLava) {
                    auto levelIt = bs->properties.find("level");
                    const i32 level = levelIt != bs->properties.end() ? std::atoi(levelIt->second.c_str()) : 1;
                    if (level <= 1) {
                        auto empty = reg.getBlockStateId("minecraft:cauldron", {});
                        if (empty) { world_.setBlock(x, y, z, *empty); broadcast(x, y, z, *empty); }
                    } else {
                        auto lower = reg.getBlockStateId("minecraft:water_cauldron",
                            {{"level", std::to_string(level - 1)}});
                        if (lower) { world_.setBlock(x, y, z, *lower); broadcast(x, y, z, *lower); }
                    }
                    continue;
                }
                // Filling only happens under open sky and only slowly; there is no weather system
                // on this server yet, so this stands in for vanilla rain filling.
                if (nextToLava || !hasSkyAccess(x, y + 1, z) || chanceDist(rng) >= 15) continue;
                i32 level = 0;
                if (name == "minecraft:water_cauldron") {
                    auto levelIt = bs->properties.find("level");
                    level = levelIt != bs->properties.end() ? std::atoi(levelIt->second.c_str()) : 1;
                }
                if (level >= 3) continue;
                auto filled = reg.getBlockStateId("minecraft:water_cauldron",
                    {{"level", std::to_string(level + 1)}});
                if (filled) { world_.setBlock(x, y, z, *filled); broadcast(x, y, z, *filled); }
                continue;
            }

            // TURTLE_EGG_V1: eggs laid on sand crack step by step (hatch 0 -> 1 -> 2) and then
            // disappear as they hatch. The baby turtle entity is intentionally NOT spawned: this
            // server has no turtle entity type wired up, and inventing one would be a guess.
            if (name == "minecraft:turtle_egg") {
                const i32 below = world_.getBlock(x, y - 1, z);
                const bool onSand = below == 112 || below == 113 || below == 117 || below == 119;
                if (!onSand || chanceDist(rng) >= 25) continue;
                auto hatchIt = bs->properties.find("hatch");
                auto eggsIt = bs->properties.find("eggs");
                const i32 hatch = hatchIt != bs->properties.end() ? std::atoi(hatchIt->second.c_str()) : 0;
                const i32 eggs = eggsIt != bs->properties.end() ? std::atoi(eggsIt->second.c_str()) : 1;
                if (hatch >= 2) {
                    world_.setBlock(x, y, z, 0);
                    broadcast(x, y, z, 0);
                    continue;
                }
                auto next = reg.getBlockStateId("minecraft:turtle_egg",
                    {{"eggs", std::to_string(eggs)}, {"hatch", std::to_string(hatch + 1)}});
                if (next) { world_.setBlock(x, y, z, *next); broadcast(x, y, z, *next); }
                continue;
            }

            // SNIFFER_EGG_V1: a sniffer egg on a moss-like block progresses through
            // its three vanilla hatch states. The actual sniffer entity is intentionally
            // not spawned: no verified protocol entity type is registered by this server.
            if (name == "minecraft:sniffer_egg") {
                const i32 below = world_.getBlock(x, y - 1, z);
                const auto* belowBs = reg.blockStates().getById(below);
                const bool warmNest = belowBs && (belowBs->name == "minecraft:moss_block" ||
                    belowBs->name == "minecraft:grass_block" || belowBs->name == "minecraft:dirt" ||
                    belowBs->name == "minecraft:podzol");
                if (!warmNest || chanceDist(rng) >= 18) continue;
                auto hatchIt = bs->properties.find("hatch");
                const i32 hatch = hatchIt != bs->properties.end() ? std::atoi(hatchIt->second.c_str()) : 0;
                if (hatch >= 2) {
                    world_.setBlock(x, y, z, 0);
                    broadcast(x, y, z, 0);
                } else {
                    auto next = reg.getBlockStateId("minecraft:sniffer_egg",
                        {{"hatch", std::to_string(hatch + 1)}});
                    if (next) { world_.setBlock(x, y, z, *next); broadcast(x, y, z, *next); }
                }
                continue;
            }

            // SEA_PICKLE_GROWTH_V1: underwater sea pickles slowly multiply up to four
            // pickles per block. This preserves waterlogged=true and never grows them in air.
            if (name == "minecraft:sea_pickle") {
                auto waterIt = bs->properties.find("waterlogged");
                const bool submerged = waterIt != bs->properties.end() && waterIt->second == "true";
                if (!submerged || chanceDist(rng) >= 30) continue;
                auto picklesIt = bs->properties.find("pickles");
                const i32 pickles = picklesIt != bs->properties.end() ? std::atoi(picklesIt->second.c_str()) : 1;
                if (pickles >= 4) continue;
                auto next = reg.getBlockStateId("minecraft:sea_pickle",
                    {{"pickles", std::to_string(pickles + 1)}, {"waterlogged", "true"}});
                if (next) { world_.setBlock(x, y, z, *next); broadcast(x, y, z, *next); }
                continue;
            }

            // MUSHROOM_SPREAD_V1: red and brown mushrooms spread only through a sparse,
            // dim local population. This is the vanilla random-tick behaviour without the
            // giant-mushroom bonemeal structure generator.
            if (name == "minecraft:brown_mushroom" || name == "minecraft:red_mushroom") {
                if (hasEnoughLight(x, y + 1, z) || chanceDist(rng) >= 45) continue;
                i32 nearby = 0;
                for (i32 dx = -4; dx <= 4; ++dx)
                    for (i32 dy = -1; dy <= 1; ++dy)
                        for (i32 dz = -4; dz <= 4; ++dz) {
                            const auto* nearBs = reg.blockStates().getById(world_.getBlock(x + dx, y + dy, z + dz));
                            if (nearBs && nearBs->name == name && ++nearby >= 5) break;
                        }
                if (nearby >= 5) continue;
                static constexpr i32 kOffsets[8][3] = {
                    {1,0,0},{-1,0,0},{0,0,1},{0,0,-1},{1,0,1},{1,0,-1},{-1,0,1},{-1,0,-1}
                };
                const auto& d = kOffsets[coordDist(rng) % 8];
                const i32 nx = x + d[0], ny = y + d[1], nz = z + d[2];
                if (!isAir(world_.getBlock(nx, ny, nz))) continue;
                const auto* floorBs = reg.blockStates().getById(world_.getBlock(nx, ny - 1, nz));
                const bool suitableFloor = floorBs && (floorBs->name == "minecraft:mycelium" ||
                    floorBs->name == "minecraft:podzol" || floorBs->name == "minecraft:warped_nylium" ||
                    floorBs->name == "minecraft:crimson_nylium" || floorBs->name == "minecraft:dirt" ||
                    floorBs->name == "minecraft:grass_block");
                if (!suitableFloor) continue;
                world_.setBlock(nx, ny, nz, state);
                broadcast(nx, ny, nz, state);
                continue;
            }

            // NYLIUM_SPREAD_V1: warped/crimson nylium dies back to netherrack when
            // smothered and colonises adjacent netherrack under open space. The random
            // target choice and light approximation keep this bounded in loaded chunks.
            if (name == "minecraft:warped_nylium" || name == "minecraft:crimson_nylium") {
                const bool covered = !isSkyTransparent(world_.getBlock(x, y + 1, z));
                if (covered && chanceDist(rng) < 90) {
                    world_.setBlock(x, y, z, 5849);
                    broadcast(x, y, z, 5849);
                    continue;
                }
                if (chanceDist(rng) >= 55) continue;
                static constexpr i32 kSides[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
                const auto& d = kSides[coordDist(rng) % 4];
                const i32 nx = x + d[0], nz = z + d[1];
                if (world_.getBlock(nx, y, nz) != 5849 ||
                    !isSkyTransparent(world_.getBlock(nx, y + 1, nz))) continue;
                world_.setBlock(nx, y, nz, state);
                broadcast(nx, y, nz, state);
                continue;
            }

            // COPPER_OXIDATION_V1: exposed copper advances one weathering stage at a
            // time. Waxed blocks are deliberately absent from this table, so they remain
            // permanently unchanged. Full/weathered/oxidized shapes with orientation are
            // left untouched until their state encoding is registered.
            {
                static const std::pair<const char*, const char*> kCopperAging[] = {
                    {"minecraft:copper_block", "minecraft:exposed_copper"},
                    {"minecraft:exposed_copper", "minecraft:weathered_copper"},
                    {"minecraft:weathered_copper", "minecraft:oxidized_copper"},
                    {"minecraft:cut_copper", "minecraft:exposed_cut_copper"},
                    {"minecraft:exposed_cut_copper", "minecraft:weathered_cut_copper"},
                    {"minecraft:weathered_cut_copper", "minecraft:oxidized_cut_copper"},
                    {"minecraft:chiseled_copper", "minecraft:exposed_chiseled_copper"},
                    {"minecraft:exposed_chiseled_copper", "minecraft:weathered_chiseled_copper"},
                    {"minecraft:weathered_chiseled_copper", "minecraft:oxidized_chiseled_copper"},
                };
                const char* nextName = nullptr;
                for (const auto& age : kCopperAging) if (name == age.first) { nextName = age.second; break; }
                if (nextName && chanceDist(rng) < 8) {
                    auto next = reg.getBlockStateId(nextName, {});
                    if (next) { world_.setBlock(x, y, z, *next); broadcast(x, y, z, *next); }
                    continue;
                }
            }

            // farmland: gains moisture near water, dries out without it, and reverts to
            // dirt once fully dry with no crop planted above (vanilla behaviour).
            if (state >= 4286 && state <= 4293) {
                const i32 moisture = state - 4286;
                bool hydrated = false;
                for (i32 dx = -4; dx <= 4 && !hydrated; ++dx)
                    for (i32 dz = -4; dz <= 4 && !hydrated; ++dz)
                        if (isWaterState(world_.getBlock(x + dx, y, z + dz)) ||
                            isWaterState(world_.getBlock(x + dx, y + 1, z + dz))) hydrated = true;
                if (hydrated && moisture < 7) {
                    // FARMLAND_MOISTURE_V2: vanilla FarmBlock.randomTick sets moisture
                    // straight to the max (7), not +1 per tick.
                    world_.setBlock(x, y, z, 4286 + 7);
                    broadcast(x, y, z, 4286 + 7);
                } else if (!hydrated && moisture > 0) {
                    world_.setBlock(x, y, z, 4286 + moisture - 1);
                    broadcast(x, y, z, 4286 + moisture - 1);
                } else if (!hydrated && moisture == 0) {
                    const i32 above = world_.getBlock(x, y + 1, z);
                    bool cropAbove = false;
                    for (auto& c : kCrops) if (above >= c.base && above <= c.base + c.maxAge) cropAbove = true;
                    if (!cropAbove) { world_.setBlock(x, y, z, 10); broadcast(x, y, z, 10); } // -> dirt
                }
                continue;
            }


            // KELP_GROWTH_V1: the top kelp segment (the one carrying the "age" property)
            // grows a new segment above itself while submerged, matching vanilla KelpBlock.
            if (name == "minecraft:kelp") {
                handledCrop = true;
                auto ageIt = bs->properties.find("age");
                const i32 age = ageIt != bs->properties.end() ? std::atoi(ageIt->second.c_str()) : 0;
                const i32 aboveState = world_.getBlock(x, y + 1, z);
                if (age < 25 && isWaterState(aboveState) && chanceDist(rng) < 25) {
                    i32 height = 1;
                    while (true) {
                        const auto* belowBs = reg.blockStates().getById(world_.getBlock(x, y - height, z));
                        if (belowBs && (belowBs->name == "minecraft:kelp" || belowBs->name == "minecraft:kelp_plant")) { ++height; continue; }
                        break;
                    }
                    if (height < 25) {
                        auto plantId = reg.getBlockStateId("minecraft:kelp_plant", {});
                        auto nextHead = reg.getBlockStateId("minecraft:kelp", {{"age", std::to_string(age + 1)}});
                        if (plantId && nextHead) {
                            world_.setBlock(x, y, z, *plantId);
                            broadcast(x, y, z, *plantId);
                            world_.setBlock(x, y + 1, z, *nextHead);
                            broadcast(x, y + 1, z, *nextHead);
                        }
                    }
                }
                continue;
            }

            // BAMBOO_GROWTH_V1: the top bamboo segment (nothing but air above it) gets taller
            // over time, reusing the same per-stalk tick counter as sugar cane/cactus.
            if (name == "minecraft:bamboo") {
                handledCrop = true;
                if (!isAir(world_.getBlock(x, y + 1, z))) continue; // only the top segment grows
                i32 height = 1;
                while (true) {
                    const auto* belowBs = reg.blockStates().getById(world_.getBlock(x, y - height, z));
                    if (belowBs && belowBs->name == "minecraft:bamboo") { ++height; continue; }
                    break;
                }
                if (height >= 16) continue; // vanilla-ish max stalk height
                const u64 gk = fluidKey(x, y, z);
                u8 growTicks;
                { std::lock_guard lk(fireMutex_); growTicks = ++stalkGrowth_[gk]; }
                if (growTicks < 16) continue;
                { std::lock_guard lk(fireMutex_); stalkGrowth_.erase(gk); }
                auto ageIt = bs->properties.find("age");
                auto stageIt = bs->properties.find("stage");
                const std::string curAge = ageIt != bs->properties.end() ? ageIt->second : "0";
                const std::string curStage = stageIt != bs->properties.end() ? stageIt->second : "0";
                auto midSegment = reg.getBlockStateId("minecraft:bamboo", {{"age", curAge}, {"leaves", "small"}, {"stage", curStage}});
                auto newTop = reg.getBlockStateId("minecraft:bamboo", {{"age", "0"}, {"leaves", "large"}, {"stage", "0"}});
                if (midSegment && newTop) {
                    world_.setBlock(x, y, z, *midSegment);
                    broadcast(x, y, z, *midSegment);
                    world_.setBlock(x, y + 1, z, *newTop);
                    broadcast(x, y + 1, z, *newTop);
                }
                continue;
            }

            // COCOA_GROWTH_V1: cocoa pods advance age 0 -> 1 -> 2 on a random tick with a
            // 1-in-5 chance, matching vanilla CocoaBlock.randomTick. facing is preserved.
            if (name == "minecraft:cocoa") {
                handledCrop = true;
                auto ageIt = bs->properties.find("age");
                auto facingIt = bs->properties.find("facing");
                const i32 age = ageIt != bs->properties.end() ? std::atoi(ageIt->second.c_str()) : 0;
                const std::string facing = facingIt != bs->properties.end() ? facingIt->second : "north";
                if (age < 2 && chanceDist(rng) < 20) {
                    auto next = reg.getBlockStateId("minecraft:cocoa", {{"age", std::to_string(age + 1)}, {"facing", facing}});
                    if (next) { world_.setBlock(x, y, z, *next); broadcast(x, y, z, *next); }
                }
                continue;
            }

            // CORAL_DEATH_V1: coral blocks/plants/fans/wall fans that lose contact with water
            // turn into their "dead_" variant, matching vanilla coral random-tick behaviour.
            if (name.rfind("minecraft:", 0) == 0 && name.rfind("minecraft:dead_", 0) != 0) {
                const std::string shortName = name.substr(10);
                static const std::string_view kCoralNames[] = {
                    "tube_coral", "brain_coral", "bubble_coral", "fire_coral", "horn_coral",
                    "tube_coral_block", "brain_coral_block", "bubble_coral_block", "fire_coral_block", "horn_coral_block",
                    "tube_coral_fan", "brain_coral_fan", "bubble_coral_fan", "fire_coral_fan", "horn_coral_fan",
                    "tube_coral_wall_fan", "brain_coral_wall_fan", "bubble_coral_wall_fan", "fire_coral_wall_fan", "horn_coral_wall_fan",
                };
                bool isCoralFamily = false;
                for (auto cn : kCoralNames) if (shortName == cn) { isCoralFamily = true; break; }
                if (isCoralFamily) {
                    handledCrop = true;
                    bool inWater = false;
                    auto wlIt = bs->properties.find("waterlogged");
                    if (wlIt != bs->properties.end()) {
                        inWater = wlIt->second == "true";
                    } else {
                        constexpr i32 nd[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
                        for (auto& d : nd) if (isWaterState(world_.getBlock(x + d[0], y + d[1], z + d[2]))) { inWater = true; break; }
                    }
                    if (!inWater) {
                        auto deadId = reg.getBlockStateId("minecraft:dead_" + shortName, bs->properties);
                        if (deadId) { world_.setBlock(x, y, z, *deadId); broadcast(x, y, z, *deadId); }
                    }
                    continue;
                }
            }
        }
    }
}

// ENV_V1: урон окружением по тикам — ��аморозка в рыхлом снегу (state 22318) и урон лавой (96–111).
void NetherCraftServer::tickPlayerEnvironment() {
    auto isLavaState = [](i32 s) { return s >= 96 && s <= 111; };
    auto isWaterState = [](i32 s) { return (s >= 80 && s <= 95) || s == 12960 || s == 12961; }; // + bubble columns
    for (auto& player : getAllPlayersCopy()) {
        if (!player || !player->isAlive()) continue;
        if (player->getState() != entity::PlayerState::Play) continue;

        // PEARL_V3: ItemCooldowns advances once per server tick, not by wall clock.
        if (player->enderPearlCooldownTicks > 0 && --player->enderPearlCooldownTicks == 0) {
            net::Buffer cd; cd.writeVarInt(993); cd.writeVarInt(0);
            player->getConnection()->sendPacket(0x17,
                std::vector<u8>(cd.writtenSpan().begin(), cd.writtenSpan().end()));
        }

        // ELYTRA_STOP_V3: clear both the shared flag and pose as soon as the player
        // lands or unequips the chest item. This covers the normal landing path and
        // inventory changes that bypass the movement packet handler.
        // ELYTRA_ROCKET_V2: вода тоже гасит планирование — иначе флаг висел после падения
        // в океан и ракета продолжала работать без раскрытых элитр.
        // ELYTRA_STOP_V4: в V3 вода проверялась на уровне тела (Y+0.8), то есть СТОЯЩИЙ
        // в луже по пояс игрок уже не мог раскрыть элитры. Ваниль гасит планирование
        // только когда игрок ПОГРУЖЁН (голова в воде), поэтому смотрим на Y+1.6.
        const i32 headBlk = worldOf(player).getBlock(
            static_cast<i32>(std::floor(player->getX())),
            static_cast<i32>(std::floor(player->getY() + 1.6)),
            static_cast<i32>(std::floor(player->getZ())));
        const bool headInWater = (headBlk >= 80 && headBlk <= 95) || headBlk == 12960 || headBlk == 12961;
        if (player->elytraFlying && (player->isOnGround() || headInWater || player->invCount[6] <= 0 || player->invItemId[6] != 773)) {
            player->elytraFlying = false;
            broadcastEntityMeta(player);
        }

        // DEATH_TICK_GUARD_V1: жёсткий guard ДО координат, блоков и коллизий.
        // На death-screen не уходит ни Damage Event, ни hurt animation, ни env metadata spam.
        if (player->dead || player->health <= 0.0f) {
            player->dead = true;
            player->health = 0.0f;
            player->usingShield = false;
            player->lavaHurtCooldown = 0;
            player->contactHurtCooldown = 0;
            player->fireHurtCooldown = 0;
            player->remainingFireTicks = 0;
            continue;
        }

        const i32 px = static_cast<i32>(std::floor(player->getX()));
        const i32 pz = static_cast<i32>(std::floor(player->getZ()));
        const i32 feetY = static_cast<i32>(std::floor(player->getY() + 0.1));
        const i32 headY = static_cast<i32>(std::floor(player->getY() + 1.1));
        const i32 bFeet = world_.getBlock(px, feetY, pz);
        const i32 bHead = world_.getBlock(px, headY, pz);
        const i32 bBelow = world_.getBlock(px, static_cast<i32>(std::floor(player->getY() - 0.05)), pz);
        const bool touchingLava = isLavaState(bFeet) || isLavaState(bHead);
        const bool movedHorizontally = player->envPositionReady &&
            (std::abs(player->getX() - player->envLastX) >= 0.003 ||
             std::abs(player->getZ() - player->envLastZ) >= 0.003);
        player->envLastX = player->getX(); player->envLastZ = player->getZ();
        player->envPositionReady = true;

        // LAVAFIRE_V1: lavaHurt() в vanilla вызывает igniteForSeconds(15), а клиент
        // рисует огонь по DATA_SHARED_FLAGS bit 0. Креатив горит визуально, но не получает урон.
        if (player->gameMode == 3) {
            player->remainingFireTicks = 0;
        } else if (touchingLava) {
            player->remainingFireTicks = std::max(player->remainingFireTicks, 300);
        } else if (isWaterState(bFeet) || isWaterState(bHead)) {
            player->remainingFireTicks = 0;
        } else if (player->remainingFireTicks > 0) {
            --player->remainingFireTicks;
        }

        const bool onFireNow = player->remainingFireTicks > 0;
        if (onFireNow != player->fireFlagSynced) {
            net::Buffer fm;
            fm.writeVarInt(static_cast<i32>(player->getEntityId()));
            fm.writeByte(0); fm.writeVarInt(0); // DATA_SHARED_FLAGS, BYTE
            u8 flags = onFireNow ? 0x01 : 0x00;
            if (player->sneaking) flags |= 0x02;
            if (player->sprinting) flags |= 0x08;
            fm.writeByte(flags);
            fm.writeByte(0xFF);
            auto fv = std::vector<u8>(fm.writtenSpan().begin(), fm.writtenSpan().end());
            for (auto& target : getAllPlayersCopy())
                if (target && target->isAlive() && target->getState() == entity::PlayerState::Play)
                    target->getConnection()->sendPacket(0x58, fv);
            player->fireFlagSynced = onFireNow;
        }

        if (player->gameMode == 1 || player->gameMode == 3) { // креатив/спектатор — без урона среды
            player->ticksFrozen = 0;
            player->lavaHurtCooldown = 0;
            player->contactHurtCooldown = 0;
            player->airSupply = 300;
        } else {
            // ENVLAVA_V1: лава жжёт — 4 урона каждые 10 тиков (ванильные i-frames)
            if (touchingLava) {
                if (player->lavaHurtCooldown <= 0) {
                    applyEnvironmentalDamage(player, 4.0f, 9, std::format("{} сгорел в лаве", player->getName()));
                    player->lavaHurtCooldown = 10;
                }
            }
            if (player->dead || player->health <= 0.0f) continue;
            if (player->lavaHurtCooldown > 0) --player->lavaHurtCooldown;

            // ENV_PHYSICS_V2: contact physics ported from BaseFireBlock,
            // CactusBlock, MagmaBlock and SweetBerryBushBlock. A dedicated
            // cooldown prevents these blocks from interfering with lava i-frames.
            if (player->contactHurtCooldown > 0) --player->contactHurtCooldown;
            f32 contactDamage = 0.0f;
            std::string contactDeath;
            const bool regularFire = (bFeet >= 2391 && bFeet < 2872) ||
                                     (bHead >= 2391 && bHead < 2872);
            const bool soulFire = bFeet == 2872 || bHead == 2872;
            if (regularFire || soulFire) {
                player->remainingFireTicks = std::max(player->remainingFireTicks, 160);
                contactDamage = soulFire ? 2.0f : 1.0f;
                contactDeath = std::format("{} сгорел в огне", player->getName());
            } else if (bFeet == 5782 || bHead == 5782) {
                contactDamage = 1.0f;
                contactDeath = std::format("{} искололся о кактус", player->getName());
            } else if (bBelow == 12543 && !player->sneaking) {
                contactDamage = 1.0f;
                contactDeath = std::format("{} обнаружил, что пол — это лава", player->getName());
            } else if (bFeet >= 18576 && bFeet <= 18578 && movedHorizontally) {
                contactDamage = 1.0f;
                contactDeath = std::format("{} был исколот ягодным кустом", player->getName());
            }
            if (contactDamage > 0.0f && player->contactHurtCooldown <= 0) {
                applyEnvironmentalDamage(player, contactDamage, 9, std::move(contactDeath));
                player->contactHurtCooldown = 10;
            }
            // FIRE_V2: after leaving the fire block, the entity keeps burning and
            // receives one point every 20 ticks until water or the timer extinguishes it.
            if (!regularFire && !soulFire && !touchingLava && player->remainingFireTicks > 0 && player->gameMode != 1) {
                if (player->fireHurtCooldown > 0) --player->fireHurtCooldown;
                if (player->fireHurtCooldown <= 0) {
                    applyEnvironmentalDamage(player, 1.0f, 9, std::format("{} сгорел заживо", player->getName()));
                    player->fireHurtCooldown = 20;
                }
            } else if (player->remainingFireTicks <= 0 || isWaterState(bFeet) || isWaterState(bHead)) {
                player->fireHurtCooldown = 0;
            }
            if (player->dead || player->health <= 0.0f) continue;

            // FREEZE_V1: рыхлый снег морозит — +1/тик до 140, затем 1 урона каждые 40 тиков
            if (bFeet == 22318 || bHead == 22318) {
                if (player->ticksFrozen < 140) ++player->ticksFrozen;
                if (player->ticksFrozen >= 140 && tickCounter_ % 40 == 0)
                    applyEnvironmentalDamage(player, 1.0f, 9, std::format("{} замёрз насмерть", player->getName()));
            } else if (player->ticksFrozen > 0) {
                player->ticksFrozen = std::max(0, player->ticksFrozen - 2);
            }
            if (player->dead || player->health <= 0.0f) continue;

            // ENVWATER_V2: захлёбывание считаем по УРОВНЮ ГЛАЗ (y+1.62), как ванильный
            // клиент (isEyeInFluid). Мелкая вода «по пояс» больше НЕ топит, и урон совпадает
            // с моментом, когда у клиента опустели пузыри (раньше проверка по груди y+1.1 била на суше).
            // ENVWATER_V4: погружение ГЛАЗ считаем РОВНО как ванильный Entity.updateFluidOnEyes/
            // isEyeInFluid: берём высоту воды в блоке глаз (исток/полный столб = 1.0, поток = amount/9)
            // и то��им ТОЛЬКО если поверхность выше точной высоты глаз. Иначе клиент считает голову
            // над водой (пузыри полны) — и сервер тоже НЕ должен топить (раньше топил в потоке — «где пузыри»).
            const f64 eyeExact = player->getY() + 1.62;
            const i32 eyeY = static_cast<i32>(std::floor(eyeExact));
            const i32 bEye = world_.getBlock(px, eyeY, pz);

            // FLUIDSYNC_V1: Block Update is authoritative for client collision.
            // If a chunk was generated/loaded while streaming, force the actual
            // liquid states at the player back onto that client. This prevents
            // the observed split where the server sees state 80 but the client
            // keeps old solid terrain and applies air jump physics (0.42).
            if ((tickCounter_ % 5) == 0 &&
                (isWaterState(bFeet) || isLavaState(bFeet) ||
                 isWaterState(bEye)  || isLavaState(bEye))) {
                auto syncFluidBlock = [&](i32 y, i32 state) {
                    if (!isWaterState(state) && !isLavaState(state)) return;
                    net::Buffer bu;
                    bu.writePosition(BlockPos{px, y, pz});
                    bu.writeVarInt(state);
                    player->getConnection()->sendPacket(0x09,
                        std::vector<u8>(bu.writtenSpan().begin(), bu.writtenSpan().end()));
                };
                syncFluidBlock(feetY, bFeet);
                if (eyeY != feetY) syncFluidBlock(eyeY, bEye);
            }

            bool eyeInWater = false;
            if (isWaterState(bEye)) {
                f64 fh;
                if (bEye == 12960 || bEye == 12961) {
                    fh = 1.0; // BubbleColumnBlock has a full-height water fluid state
                } else if (isWaterState(world_.getBlock(px, eyeY + 1, pz))) {
                    fh = 1.0; // вода сверху -> блок полный
                } else {
                    const i32 lv = bEye - 80;                       // 0=исток, 1..7=поток, >=8=падающий
                    const i32 amount = (lv == 0 || lv >= 8) ? 8 : (8 - lv);
                    fh = static_cast<f64>(amount) / 9.0;           // ownHeight = amount/9
                }
                eyeInWater = (static_cast<f64>(eyeY) + fh) > eyeExact;
            }
            if (eyeInWater) {
                --player->airSupply;
                if (player->airSupply <= -20) {
                    player->airSupply = 0;
                    applyEnvironmentalDamage(player, 2.0f, 9, std::format("{} захлебнулся", player->getName()));
                }
            } else if (player->airSupply < 300) {
                // BUBBLEFIX_V1: воздух пополняем ПОСТЕПЕННО (+10/тик), а не мгновенно до 300.
                // На поверхности глаза каждый тик «дёргаются» через границу воды; при мгновенном
                // сбросе бар пузырей моргал (полный->скрыт->полный). Плавный дозаряд держит бар
                // стабильным пока реально ныряешь, и прячет его через ~1с после выхода.
                player->airSupply = std::min(300, player->airSupply + 10);
            }

        }

        // FREEZE_V1: синхронизируем «иней» на экране (метаданные DATA_TICKS_FROZEN, индекс 7, тип VarInt=1)
        if (player->ticksFrozen != player->frozenSynced) {
            net::Buffer md;
            md.writeVarInt(static_cast<i32>(player->getEntityId()));
            md.writeByte(7); md.writeVarInt(1); md.writeVarInt(player->ticksFrozen);
            md.writeByte(0xFF);
            auto mv = std::vector<u8>(md.writtenSpan().begin(), md.writtenSpan().end());
            for (auto& t : getAllPlayersCopy())
                if (t && t->isAlive() && t->getState() == entity::PlayerState::Play)
                    t->getConnection()->sendPacket(0x58, mv);
            player->frozenSynced = player->ticksFrozen;
        }

        // ENVWATER_V3: пузыри воздуха рисует клиент по DATA_AIR_SUPPLY (index 1), которое шлёт
        // сервер. Значение = airSupply, посчитанный по ГЛАЗАМ (см. выше), поэтому бар убывает ровно
        // тогда же, когда идёт урон — без прежнего десинка «бар полный, а ��ьёт».
        {
            const i32 airShow = std::max(0, std::min(300, player->airSupply));
            if (airShow != player->airSynced) {
                net::Buffer ad;
                ad.writeVarInt(static_cast<i32>(player->getEntityId()));
                ad.writeByte(1); ad.writeVarInt(1); ad.writeVarInt(airShow); // index 1, type VarInt=1, воздух
                ad.writeByte(0xFF);
                auto av = std::vector<u8>(ad.writtenSpan().begin(), ad.writtenSpan().end());
                if (player->getConnection()) player->getConnection()->sendPacket(0x58, av);
                player->airSynced = airShow;
            }
        }

        // ENVARMOR_V1: считаем броню (слоты 5-8) и шлём атрибут generic.armor (id 0) -> шкала над сердцами
        {
            f32 armor = 0.0f, tough = 0.0f;
            for (i32 as = 5; as <= 8; ++as)
                if (player->invCount[as] > 0) armorStats(player->invItemId[as], armor, tough);
            if (armor != player->armorSynced) {
                net::Buffer at;
                at.writeVarInt(static_cast<i32>(player->getEntityId()));
                at.writeVarInt(1);                     // одно свойство
                at.writeVarInt(0);                     // attribute id 0 = generic.armor
                at.writeF64(static_cast<f64>(armor));  // базовое значение
                at.writeVarInt(0);                     // 0 модификаторов
                auto atv = std::vector<u8>(at.writtenSpan().begin(), at.writtenSpan().end());
                if (player->getConnection()) player->getConnection()->sendPacket(0x75, atv);
                player->armorSynced = armor;
            }
        }
    }
}


// ============================================================
// MOBS_V1 — мирные мобы: спавн, физика, пакеты, урон, дроп, взаимодействие.
// Цифры типов/хитбоксов/HP — entity/mob.hpp (декомпил 1.21.1).
// Полноценный goal-ИИ и размножение — заход 2.
// ============================================================
static bool mobStateIsSolid(i32 s) {
    if (s <= 0) return false;
    if (s >= 80 && s <= 111) return false;   // ��ода и лава со всеми уровнями
    if (s == 12958 || s == 12959) return false; // void/cave air
    return true;
}

static void sendMobSpawnTo(const std::shared_ptr<entity::Player>& viewer, const entity::Mob& m) {
    if (!viewer || !viewer->isAlive() || viewer->getState() != entity::PlayerState::Play) return;
    if (viewer->dimension != m.dimension) return;
    const auto& d = m.def();
    net::Buffer sp;
    sp.writeVarInt(m.eid);
    sp.writeUUID(UUID{static_cast<u64>(m.eid), 0xB0000000ULL + static_cast<u64>(m.eid)});
    sp.writeVarInt(d.netId);
    sp.writeF64(m.x); sp.writeF64(m.y); sp.writeF64(m.z);
    const auto ang = [](f32 deg) { return static_cast<u8>(static_cast<i32>(deg * 256.0f / 360.0f) & 0xFF); };
    sp.writeByte(ang(m.pitch)); sp.writeByte(ang(m.yaw)); sp.writeByte(ang(m.headYaw));
    sp.writeVarInt(0); // data
    sp.writeI16(0); sp.writeI16(0); sp.writeI16(0);
    viewer->getConnection()->sendPacket(0x01, std::vector<u8>(sp.writtenSpan().begin(), sp.writtenSpan().end()));
}

void NetherCraftServer::sendMobsTo(const std::shared_ptr<entity::Player>& player) {
    std::vector<entity::Mob> copy;
    { std::lock_guard lk(mobsMutex_); copy = mobs_; }
    for (const auto& m : copy) if (!m.dead) sendMobSpawnTo(player, m);
}

// MOBS_ALL_V1: вода со всеми уровнями + водные логические блоки.
static bool mobStateIsWater(i32 s) { return (s >= 80 && s <= 95) || s == 12960 || s == 12961; }

// MOBS_AI_V1: прямая видимость. Ванильные мобы (Sensing.hasLineOfSight) не берут
// цель, не бьют и не стреляют сквозь стену.
// MOB_GOALS_V2: локальный эквивалент navigation.MoveControl + node avoidance.
// Проверяем прямой ход и 30/60/90-градусные обходы. Это работает без генерации
// чанков и не даёт mobPhysicsStep() отражать моба назад от каждой стены.
static bool mobNavigate(const entity::MobPhysicsEnv& env, entity::Mob& m,
                        f64 tx, f64 tz, f64 speed) {
    const f64 dx = tx - m.x, dz = tz - m.z;
    const f64 len = std::sqrt(dx * dx + dz * dz);
    if (len < 0.08 || speed <= 0.0) { m.vx = 0; m.vz = 0; return true; }
    const f64 base = std::atan2(dz, dx);
    static constexpr f64 turns[] = { 0.0, 0.5235987756, -0.5235987756,
                                     1.0471975512, -1.0471975512,
                                     1.5707963268, -1.5707963268 };
    for (f64 turn : turns) {
        const f64 vx = std::cos(base + turn) * speed;
        const f64 vz = std::sin(base + turn) * speed;
        if (!entity::mobBlocked(env, m, m.x + vx, m.z + vz)) {
            m.vx = vx; m.vz = vz;
            return turn == 0.0;
        }
    }
    m.vx = 0; m.vz = 0;
    return false;
}

static bool mobSeesPoint(const entity::MobPhysicsEnv& env, const entity::Mob& m,
                         f64 tx, f64 ty, f64 tz) {
    const f64 sx = m.x, sy = m.y + static_cast<f64>(m.def().eyeHeight), sz = m.z;
    const f64 dx = tx - sx, dy = ty - sy, dz = tz - sz;
    const f64 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 0.001) return true;
    const i32 steps = std::min(96, std::max(1, static_cast<i32>(dist / 0.4)));
    for (i32 i = 1; i < steps; ++i) {
        const f64 a = static_cast<f64>(i) / static_cast<f64>(steps);
        if (env.solid(static_cast<i32>(std::floor(sx + dx * a)),
                      static_cast<i32>(std::floor(sy + dy * a)),
                      static_cast<i32>(std::floor(sz + dz * a)))) return false;
    }
    return true;
}

// MOBS_ALL_V1: спавн любого моба из таблицы — яйцо спавна, /mob, естественные волны.
void NetherCraftServer::spawnMobAt(i32 typeIdx, f64 x, f64 y, f64 z, i32 dim, i32 count, bool asBaby) {
    if (typeIdx < 0 || typeIdx >= entity::mobTypeCount() || count <= 0) return;
    std::vector<entity::Mob> spawned;
    for (i32 i = 0; i < count; ++i) {
        entity::Mob m;
        m.eid = static_cast<i32>(nextEntityId_++);
        m.typeIdx = typeIdx;
        m.dimension = dim;
        m.x = x + (count > 1 ? ((std::rand() % 5) - 2) * 0.5 : 0.0);
        m.y = y;
        m.z = z + (count > 1 ? ((std::rand() % 5) - 2) * 0.5 : 0.0);
        m.yaw = static_cast<f32>(std::rand() % 360);
        m.headYaw = m.yaw;
        // SPLIT_V1: мелкие копии слизня/магма-куба слабее и живучее отскакивают.
        m.baby = asBaby;
        m.health = asBaby ? std::max(1, static_cast<i32>(m.def().maxHealth * 0.4f + 0.5f))
                           : static_cast<i32>(m.def().maxHealth + 0.5f);
        if (asBaby) { m.vx = ((std::rand() % 3) - 1) * 0.15; m.vz = ((std::rand() % 3) - 1) * 0.15; m.vy = 0.25; }
        m.eggTimer = 6000 + (std::rand() % 6000);   // Chicken.java: nextInt(6000) + 6000
        m.strollTimer = std::rand() % 100;
        // VILLAGER_TRADE_V1: VillagerData.profession — в ванили профессию даёт рабочий блок.
        // Блоков профессий у нас пока нет, поэтому она выводится из eid — один и тот же
        // житель всегда торгует одним и тем же набором, а не меняет его после перезахода.
        if (m.isKind("villager")) m.profession = 1 + (m.eid % (entity::VP_COUNT - 1));
        m.lastSentX = m.x; m.lastSentY = m.y; m.lastSentZ = m.z;
        spawned.push_back(m);
    }
    { std::lock_guard lk(mobsMutex_); for (auto& m : spawned) mobs_.push_back(m); }
    auto viewers = getAllPlayersCopy();
    for (auto& viewer : viewers) for (const auto& m : spawned) sendMobSpawnTo(viewer, m);
}

// LLAMA_CARAVAN_V1: WanderingTraderSpawner — торговец появляется не один, а с двумя
// торговыми ламами на привязи. Ламы запоминают eid ведущего и ходят за ним.
// Больше одного торговца в оверворлде одновременно не держим.
void NetherCraftServer::spawnTraderCaravan() {
    if ((std::rand() % 4) != 0) return;   // шанс попытки, чтобы караваны не шли строго по таймеру

    std::vector<std::shared_ptr<entity::Player>> pool;
    for (auto& p : getAllPlayersCopy())
        if (p && p->isAlive() && !p->dead && p->dimension == 0) pool.push_back(p);
    if (pool.empty()) return;

    {
        std::lock_guard lk(mobsMutex_);
        for (const auto& o : mobs_)
            if (!o.dead && o.dimension == 0 && o.isKind("wandering_trader")) return;
    }

    const i32 traderIdx = entity::mobIndexByName("wandering_trader");
    const i32 llamaIdx = entity::mobIndexByName("trader_llama");
    if (traderIdx < 0 || llamaIdx < 0) return;

    auto& p = pool[static_cast<size_t>(std::rand()) % pool.size()];
    auto& w = worldFor(0);
    f64 sx = 0.0, sy = -10000.0, sz = 0.0;
    for (i32 attempt = 0; attempt < 12 && sy < -1000.0; ++attempt) {
        const i32 ox = static_cast<i32>(std::floor(p->getX())) + (std::rand() % 33) - 16;
        const i32 oz = static_cast<i32>(std::floor(p->getZ())) + (std::rand() % 33) - 16;
        for (i32 y = 200; y > -60; --y) {
            const i32 st = w.getBlock(ox, y, oz);
            if (st <= 0 || st == 12958 || st == 12959) continue;   // воздух
            if (mobStateIsWater(st) || !mobStateIsSolid(st)) break; // на воде и листве караван не встаёт
            if (mobStateIsSolid(w.getBlock(ox, y + 1, oz))) break;
            if (mobStateIsSolid(w.getBlock(ox, y + 2, oz))) break;
            sx = ox + 0.5; sy = static_cast<f64>(y + 1); sz = oz + 0.5;
            break;
        }
    }
    if (sy < -1000.0) return;

    std::vector<entity::Mob> spawned;
    auto make = [&](i32 typeIdx, f64 x, f64 z, i32 leaderEid) -> i32 {
        entity::Mob m;
        m.eid = static_cast<i32>(nextEntityId_++);
        m.typeIdx = typeIdx;
        m.dimension = 0;
        m.x = x; m.y = sy; m.z = z;
        m.yaw = static_cast<f32>(std::rand() % 360);
        m.headYaw = m.yaw;
        m.health = static_cast<i32>(m.def().maxHealth + 0.5f);
        m.strollTimer = std::rand() % 100;
        m.caravanLeader = leaderEid;
        m.lastSentX = m.x; m.lastSentY = m.y; m.lastSentZ = m.z;
        const i32 eid = m.eid;
        spawned.push_back(m);
        return eid;
    };
    const i32 leader = make(traderIdx, sx, sz, 0);
    make(llamaIdx, sx + 1.5, sz + 0.5, leader);
    make(llamaIdx, sx - 1.5, sz - 0.5, leader);

    { std::lock_guard lk(mobsMutex_); for (auto& m : spawned) mobs_.push_back(m); }
    auto viewers = getAllPlayersCopy();
    for (auto& viewer : viewers) for (const auto& m : spawned) sendMobSpawnTo(viewer, m);
}

// MOBS_ALL_V1: NaturalSpawner по всей таблице — фильтры по измерению (dims-битмаска),
// категории (MONSTER только в темноте), среде (рыбы в воде, остальные на земле)
// и подходящему блоку под ногами. Группы — из SpawnPlacements (groupMin/groupMax).
void NetherCraftServer::spawnMobWave() {
    auto players = getAllPlayersCopy();
    if (players.empty()) return;
    { std::lock_guard lk(mobsMutex_); if (mobs_.size() >= 300) return; }   // глобальный кап

    // Грубое время суток: ночь 13000..23000, как в ванили.
    const i32 dayTime = ((tickCounter_ % 24000) + 24000) % 24000;
    const bool night = dayTime >= 13000 && dayTime <= 23000;

    for (auto& p : players) {
        if (!p || !p->isAlive() || p->getState() != entity::PlayerState::Play) continue;
        const i32 dim = std::clamp(p->dimension, 0, 2);
        const u8 dimBit = static_cast<u8>(1 << dim);
        const f64 px = p->getX(), pz = p->getZ();

        // MSVC: <windows.h> определяет макросы near/far, поэтому имя nearby.
        i32 nearby = 0;
        {
            std::lock_guard lk(mobsMutex_);
            for (const auto& m : mobs_) {
                if (m.dimension != p->dimension) continue;
                const f64 dx = m.x - px, dz = m.z - pz;
                if (dx * dx + dz * dz < 128.0 * 128.0) ++nearby;
            }
        }
        if (nearby >= 30) continue;                   // локальный кап плотности

        auto& w = worldFor(p->dimension);
        for (i32 attempt = 0; attempt < 8; ++attempt) {
            const i32 ox = static_cast<i32>(px) + (std::rand() % 128) - 64;
            const i32 oz = static_cast<i32>(pz) + (std::rand() % 128) - 64;
            const i32 top = (dim == 1) ? 120 : 200;
            const i32 bottom = (dim == 1) ? 30 : -60;

            i32 surface = -10000;
            bool water = false;
            for (i32 y = top; y > bottom; --y) {
                const i32 st = w.getBlock(ox, y, oz);
                if (st <= 0 || st == 12958 || st == 12959) continue;   // воздух
                if (mobStateIsWater(st)) { water = true; surface = y; break; }
                surface = y + 1; break;
            }
            if (surface <= bottom) continue;
            const i32 groundBlock = water ? 0 : w.getBlock(ox, surface - 1, oz);
            if (!water) {
                // нужны два свободных блока над землёй — иначе моб врастает в текстуры
                if (mobStateIsSolid(w.getBlock(ox, surface, oz))) continue;
                if (mobStateIsSolid(w.getBlock(ox, surface + 1, oz))) continue;
                if (!mobStateIsSolid(groundBlock)) continue;
            }
            const bool dark = night || surface < 50;   // под землёй темно всегда

            i32 pick = -1;
            for (i32 tries = 0; tries < 32 && pick < 0; ++tries) {
                const i32 idx = std::rand() % entity::mobTypeCount();
                const auto& d = entity::mobDef(idx);
                if ((d.dims & dimBit) == 0) continue;
                // MOBS_AI_V1: MISC (житель, железный/снежный голем) естественно не спавнится —
                // только яйцом/структурой/командой; на Peaceful нет монстров вообще.
                if (d.cat == entity::gen::MC_MISC) continue;
                if (config_.difficulty == 0 && d.behavior == entity::gen::MB_HOSTILE) continue;
                // боссы и особые — только яйцом спавна или /mob, естественно не лезут
                if (d.maxHealth >= 100.0f) continue;
                if (d.aquatic != water) continue;
                if (d.cat == entity::gen::MC_MONSTER && !dark) continue;
                if (!water && d.cat == entity::gen::MC_CREATURE) {
                    // скот — трава; в аду/энде ограничение не держим
                    if (dim == 0 && groundBlock != 9) continue;
                }
                pick = idx;
            }
            if (pick < 0) continue;

            const i32 group = entity::mobGroupSize(pick, std::rand());
            spawnMobAt(pick, ox + 0.5, static_cast<f64>(surface), oz + 0.5, p->dimension, group);
            break;
        }
    }
}

void NetherCraftServer::tickMobs() {
    if (tickCounter_ % 200 == 0) spawnMobWave();
    // LLAMA_CARAVAN_V1: WanderingTraderSpawner — попытка выставить караван
    // раз в 10 минут реального времени, сама функция ещё бросает кубик.
    if (tickCounter_ % 12000 == 0) spawnTraderCaravan();
    // MOBS_V1b: раньше тик работал по КОПИИ mobs_, а в конце ��озвращал её целиком
    // (mobs_ = mobs). Всё, что успевал записать mobAttack между копией и записью
    // (HP, hurtCooldown, dead, отбрасывание), стиралось — мобы были неубиваемыми
    // «призраками». Теперь список тикается на месте под mobsMutex_.
    { std::lock_guard lk(mobsMutex_); if (mobs_.empty()) return; }

    auto players = getAllPlayersCopy();

    struct Env { world::World* world; };
    Env env{ &worldFor(0) };
    i32 curDim = 0;                  // MOBS_ALL_V1: мобы живут во всех трёх измерениях
    entity::MobPhysicsEnv phys;
    phys.ctx = &env;
    phys.solidAt = [](void* ctx, i32 x, i32 y, i32 z) {
        return mobStateIsSolid(static_cast<Env*>(ctx)->world->getBlock(x, y, z));
    };
    phys.waterAt = [](void* ctx, i32 x, i32 y, i32 z) {
        return mobStateIsWater(static_cast<Env*>(ctx)->world->getBlock(x, y, z));
    };

    auto sendAll = [&](i32 packetId, const net::Buffer& b) {
        auto v = std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end());
        for (auto& pl : players)
            if (pl && pl->isAlive() && pl->getState() == entity::PlayerState::Play && pl->dimension == curDim)
                pl->getConnection()->sendPacket(packetId, v);
    };

    std::vector<i32> removed;
    std::vector<f64> eggDrops; // x,y,z тройками: spawnItemDrop() нельзя звать под mobsMutex_
    // MOBS_ALL_V1: удары по игрокам копи�� и применяем после снятия mobsMutex_.
    struct MobHit { std::shared_ptr<entity::Player> victim; f32 damage; std::string mob; };
    std::vector<MobHit> mobHits;
    // MOBS_AI_V1: выстрелы, взрывы и звуки нельзя выполнять под mobsMutex_.
    struct MobShot { i32 typeId; i32 dim; i32 owner; f64 x; f64 y; f64 z;
                     f64 vx; f64 vy; f64 vz; f32 damage; bool gravity; bool explosive;
                     i32 targetEid = 0; bool homing = false;                 // SHULKER_BULLET_V2
                     i32 effectId = -1; i32 effectAmp = 0; i32 effectDur = 0; }; // WITCH_POTION_V2
    std::vector<MobShot> mobShots;
    // EVOKER_FANGS_V1: клыки создают сущности и читают мир — тоже после снятия mobsMutex_.
    struct MobFangs { i32 dim; f64 ox; f64 oy; f64 oz; f64 tx; f64 ty; f64 tz; f32 damage; };
    std::vector<MobFangs> mobFangs;
    // EVOKER_SPELL_V1: призыв вексов нельзя делать под mobsMutex_ — копим отложенно.
    struct MobSummon { i32 typeIdx; i32 dim; f64 x; f64 y; f64 z; i32 count; };
    std::vector<MobSummon> mobSummons;
    struct MobBoom { f64 x; f64 y; f64 z; f32 radius; };
    std::vector<MobBoom> mobBooms;
    struct MobSound { const char* name; f64 x; f64 y; f64 z; };
    std::vector<MobSound> mobSounds;
    // ZOMBIE_CONVERT_V1: удары моба по мобу копим и применяем после цикла ИИ:
    // править чужой элемент mobs_ прямо внутри range-for по mobs_ — путь к гонкам.
    struct MobVsMob { i32 attacker; i32 target; f32 damage; };
    std::vector<MobVsMob> mobMelee;
    // ZOMBIE_CONVERT_V1: подмена сущности (житель -> зомби-житель и обратно).
    struct MobConvert { i32 toType; i32 dim; f64 x; f64 y; f64 z; bool baby; const char* sound; };
    std::vector<MobConvert> mobConverts;
    // Сложность масштабирует урон, как в ванили: easy x0.5, normal x1, hard x1.5.
    const f32 diffScale = config_.difficulty == 1 ? 0.5f : (config_.difficulty >= 3 ? 1.5f : 1.0f);
    const bool peaceful = config_.difficulty == 0;
    std::unique_lock<std::mutex> mobsLock(mobsMutex_);
    for (auto& m : mobs_) {
        if (m.dead) {
            if (--m.deathTimer <= 0) removed.push_back(m.eid);
            continue;
        }
        if (m.hurtCooldown > 0) --m.hurtCooldown;
        if (m.attackCooldown > 0) --m.attackCooldown;
        if (m.angryTimer > 0) --m.angryTimer;
        if (m.targetMemory > 0) --m.targetMemory;
        if (m.loveTicks > 0) --m.loveTicks;
        if (m.breedCooldown > 0) --m.breedCooldown;
        if (m.specialTimer > 0) --m.specialTimer;
        // BEE_STING_V1: после укуса через stingTimer пчела теряет жало и умирает без дропа/звука боя (как в ванили).
        if (m.stingTimer > 0 && --m.stingTimer <= 0) { m.dead = true; m.deathTimer = 20; continue; }
        // ZOMBIE_CONVERT_V1: ZombieVillager.tick() — когда таймер лечения дотикал,
        // сущность подменяется на жителя со звуком zombie_villager.converted.
        if (m.convertTimer > 0 && --m.convertTimer <= 0 && m.convertTo >= 0) {
            mobConverts.push_back(MobConvert{ m.convertTo, m.dimension, m.x, m.y, m.z, m.baby,
                                              "minecraft:entity.zombie_villager.converted" });
            removed.push_back(m.eid);
            m.dead = true; m.deathTimer = 0;
            continue;
        }

        // MOBS_ALL_V1: мир моба — его измерение, а не всегда оверворлд.
        // DROWNED_CONVERT_V1 / HUSK_DETAILS_V1: Zombie.tick() — если голова под водой
        // 30 секунд подряд, зомби становится утопленником, а хаск — обычным зомби.
        // Вынырнул раньше — таймер сбрасывается полностью, как в ванили.
        if (m.convertTimer <= 0 && !m.dead &&
            (m.isKind("zombie") || m.isKind("husk") || m.isKind("zombie_villager"))) {
            auto& cw = worldFor(m.dimension);
            const i32 wx = static_cast<i32>(std::floor(m.x));
            const i32 wy = static_cast<i32>(std::floor(m.y + static_cast<f64>(m.def().eyeHeight)));
            const i32 wz = static_cast<i32>(std::floor(m.z));
            if (mobStateIsWater(cw.getBlock(wx, wy, wz))) {
                if (++m.waterTimer >= 300) {
                    m.waterTimer = 0;
                    const i32 toIdx = entity::mobIndexByName(m.isKind("husk") ? "zombie" : "drowned");
                    if (toIdx >= 0) {
                        mobConverts.push_back(MobConvert{ toIdx, m.dimension, m.x, m.y, m.z, m.baby,
                                                          "minecraft:entity.zombie.converted_to_drowned" });
                        removed.push_back(m.eid);
                        m.dead = true; m.deathTimer = 0;
                        continue;
                    }
                }
            } else if (m.waterTimer > 0) {
                m.waterTimer = 0;
            }
        }

        // VILLAGER_RESTOCK_V1: Villager.restock() — раз в 12000 тиков житель пополняет
        // запас сделок: счётчик использований обнуляется, опыт торговли остаётся.
        if ((m.isKind("villager") || m.isKind("wandering_trader")) && m.tradeUses > 0) {
            if (--m.restockTimer <= 0) {
                m.restockTimer = 12000;
                m.tradeUses = 0;
            }
        }

        curDim = m.dimension;
        env.world = &worldFor(m.dimension);

        // MOBS_AI_V1: цель берётся только при прямой видимости, дальше работают
        // способности: подрыв крипера, стрельба, прыжок паука, ближний бой.
        std::shared_ptr<entity::Player> victim;
        // SPIDER_DAY_V1: SpiderTargetGoal не ��ыбирает цель при ярком дневном времени.
        // Если игрок ударил паука, angryTimer оставляет самооборону до его истечения.
        const i32 mobDayTime = static_cast<i32>(((g_timeOfDay % 24000) + 24000) % 24000);
        const bool spiderDayPassive = (m.isKind("spider") || m.isKind("cave_spider")) &&
                                      m.dimension == 0 && mobDayTime < 12500 && m.angryTimer <= 0;
        // PIGLIN_ARMOR_V1: PiglinAi.isWearingGold() — пиглин сам лезет в драку с игроком
        // без золотой брони, а золото заставляет его терпеть чужака.
        const bool piglinLike = m.isKind("piglin") || m.isKind("piglin_brute");
        // ENDERMAN_STARE_V1: EndermanLookForPlayerGoal — эндермен сам ищет, кто на него смотрит.
        const bool endermanLike = m.isKind("enderman");
        const bool huntsAnyway = m.hostile() || (m.neutral() && m.angryTimer > 0)
            || piglinLike || endermanLike;
        if (!peaceful && !spiderDayPassive && huntsAnyway) {
            const f64 follow = static_cast<f64>(m.def().follow);
            f64 bestDist = follow * follow;
            for (auto& pl : players) {
                if (!pl || !pl->isAlive() || pl->dead || pl->dimension != m.dimension) continue;
                if (pl->gameMode == 1 || pl->gameMode == 3) continue;   // креатив/наблюдатель не цель
                if (endermanLike && m.angryTimer <= 0) {
                    // Агрится только на взгляд в голову с близкого расстояния (<= 32 блока).
                    const f64 sdx = m.x - pl->getX();
                    const f64 sdy = (m.y + static_cast<f64>(m.def().eyeHeight)) - (pl->getY() + 1.62);
                    const f64 sdz = m.z - pl->getZ();
                    const f64 sd = std::sqrt(sdx * sdx + sdy * sdy + sdz * sdz);
                    if (sd > 32.0 || sd < 0.001) continue;
                    const f64 ry = static_cast<f64>(pl->get_yaw()) * 0.0174532925;
                    const f64 rp = static_cast<f64>(pl->get_pitch()) * 0.0174532925;
                    const f64 lx = -std::sin(ry) * std::cos(rp);
                    const f64 ly = -std::sin(rp);
                    const f64 lz = std::cos(ry) * std::cos(rp);
                    const f64 dot = (lx * sdx + ly * sdy + lz * sdz) / sd;
                    if (dot < 0.985) continue;   // взгляд мимо — эндермен спок��ен
                    if (!mobSeesPoint(phys, m, pl->getX(), pl->getY() + 1.4, pl->getZ())) continue;
                    m.angryTimer = 400;
                    mobSounds.push_back(MobSound{ "minecraft:entity.enderman.stare", m.x, m.y, m.z });
                }
                if (m.isKind("piglin") && m.angryTimer <= 0) {
                    // золотой шлем/нагрудник/поножи/ботинки — слоты 5..8, id 872..875
                    bool gold = false;
                    for (i32 as = 5; as <= 8; ++as) {
                        const i32 aid = pl->invItemId[as];
                        if (aid >= 872 && aid <= 875) { gold = true; break; }
                    }
                    if (gold) continue;   // в золоте пиглины не нападают
                }
                const f64 dx = pl->getX() - m.x, dy = pl->getY() - m.y, dz = pl->getZ() - m.z;
                const f64 d2 = dx * dx + dy * dy + dz * dz;
                if (d2 >= bestDist) continue;
                if (!mobSeesPoint(phys, m, pl->getX(), pl->getY() + 1.4, pl->getZ())) continue;
                bestDist = d2; victim = pl;
            }
        }
        if (m.shootCooldown > 0) --m.shootCooldown;
        if (m.leapCooldown > 0) --m.leapCooldown;
        if (m.tpCooldown > 0) --m.tpCooldown;
        // WARDEN_VIBRATION_V1: Warden.java + VibrationSystem — варден слепой. Цель ��ерётся
        // не по прямой видимости, а по вибрациям: бег шумит сильнее шага, присед почти
        // не шумит и ловится только нюхом вплотную. Порог агрессии — 40 единиц гнева.
        if (!m.dead && m.isKind("warden")) {
            std::shared_ptr<entity::Player> noisy;
            i32 bestGain = 0;
            for (auto& pl : players) {
                if (!pl || !pl->isAlive() || pl->dead || pl->dimension != m.dimension) continue;
                if (pl->gameMode == 1 || pl->gameMode == 3) continue;
                const f64 vdx = pl->getX() - m.x, vdy = pl->getY() - m.y, vdz = pl->getZ() - m.z;
                const f64 vd2 = vdx * vdx + vdy * vdy + vdz * vdz;
                if (vd2 > 16.0 * 16.0) continue;          // VibrationSystem: радиус слуха 16 блоков
                i32 gain = 0;
                if (pl->sprinting) gain = 10;             // бег — самая громкая вибрация
                else if (!pl->sneaking) gain = 4;         // обычный шаг
                else if (vd2 <= 16.0) gain = 2;           // Warden.SNIFF: присед слышно только в упор
                if (gain <= 0) continue;
                if (gain > bestGain) { bestGain = gain; noisy = pl; }
            }
            if (noisy) {
                m.wardenAnger = std::min(150, m.wardenAnger + bestGain);
                m.targetEid = static_cast<i32>(noisy->getEntityId());
                if (tickCounter_ % 40 == 0)
                    mobSounds.push_back(MobSound{ "minecraft:entity.warden.listen", m.x, m.y, m.z });
            } else if (tickCounter_ % 20 == 0 && m.wardenAnger > 0) {
                --m.wardenAnger;                          // AngerManagement.tick(): гнев тает
            }
            if (m.wardenAnger >= 40) {
                if (m.angryTimer <= 0)
                    mobSounds.push_back(MobSound{ "minecraft:entity.warden.roar", m.x, m.y, m.z });
                m.angryTimer = 200;
                m.digTimer = 0;
                if (!victim) {
                    for (auto& pl : players)
                        if (pl && pl->isAlive() && !pl->dead && pl->dimension == m.dimension
                            && static_cast<i32>(pl->getEntityId()) == m.targetEid) { victim = pl; break; }
                }
            } else {
                victim = nullptr;                         // без вибраций варден никого не видит
                m.angryTimer = 0;
                if (tickCounter_ % 60 == 0)
                    mobSounds.push_back(MobSound{ "minecraft:entity.warden.sniff", m.x, m.y, m.z });
                // WARDEN_DIG_V1: WardenAi — 60 секунд тишины, и варден уходит под землю.
                if (++m.digTimer >= 1200) {
                    mobSounds.push_back(MobSound{ "minecraft:entity.warden.dig", m.x, m.y, m.z });
                    removed.push_back(m.eid);
                    m.dead = true; m.deathTimer = 0;
                    continue;
                }
            }
        }
        // ENDERMAN_WATER_V1: Enderman.hurt()/aiStep() — вода жжёт эндермена,
        // и он тут же телепортируется прочь, а не купается в реке.
        if (!m.dead && m.isKind("enderman")) {
            const i32 ex = static_cast<i32>(std::floor(m.x));
            const i32 ey = static_cast<i32>(std::floor(m.y + 1.0));
            const i32 ez = static_cast<i32>(std::floor(m.z));
            if (phys.water(ex, ey, ez)) {
                if ((m.hurtCooldown <= 0)) {
                    m.hurtCooldown = 10;
                    --m.health;
                    mobSounds.push_back(MobSound{ "minecraft:entity.enderman.hurt", m.x, m.y, m.z });
                    if (m.health <= 0) { m.dead = true; m.deathTimer = 20; continue; }
                }
                if (m.tpCooldown <= 0) {
                    auto& ew = worldFor(m.dimension);
                    for (i32 t = 0; t < 8; ++t) {
                        const i32 tx = ex + (std::rand() % 33) - 16;
                        const i32 tz = ez + (std::rand() % 33) - 16;
                        for (i32 y = static_cast<i32>(m.y) + 8; y > static_cast<i32>(m.y) - 16; --y) {
                            if (!mobStateIsSolid(ew.getBlock(tx, y, tz))) continue;
                            if (mobStateIsSolid(ew.getBlock(tx, y + 1, tz))) break;
                            if (mobStateIsSolid(ew.getBlock(tx, y + 2, tz))) break;
                            if (phys.water(tx, y + 1, tz)) break;   // не прыгать из воды в воду
                            m.x = tx + 0.5; m.y = static_cast<f64>(y + 1); m.z = tz + 0.5;
                            m.vx = 0; m.vy = 0; m.vz = 0;
                            m.tpCooldown = 40;
                            mobSounds.push_back(MobSound{ "minecraft:entity.enderman.teleport", m.x, m.y, m.z });
                            break;
                        }
                        if (m.tpCooldown > 0) break;
                    }
                }
            }
        }
        // SUN_BURN_V1: Mob.aiStep() — нежить под открытым небом загорается днём.
        // Проверка неба раз в секунду, чтобы не сканировать колонку каждый тик.
        if (m.dimension == 0 && !m.dead && entity::mobBurnsInSun(m.def().name)) {
            if (--m.skyCheckTimer <= 0) {
                m.skyCheckTimer = 20;
                const i32 hx = static_cast<i32>(std::floor(m.x));
                const i32 hz = static_cast<i32>(std::floor(m.z));
                bool openSky = true;
                for (i32 sy = static_cast<i32>(std::floor(m.y)) + 2; sy < 320; ++sy) {
                    if (phys.solid(hx, sy, hz)) { openSky = false; break; }
                }
                if (openSky && mobDayTime < 12000) {
                    if (m.fireTimer <= 0) {
                        net::Buffer md;
                        md.writeVarInt(m.eid);
                        md.writeByte(0); md.writeVarInt(0); md.writeByte(0x01); // DATA_SHARED_FLAGS: горит
                        md.writeByte(0xFF);
                        sendAll(0x58, md);
                    }
                    m.fireTimer = 160; // пока стоит на солнце, горение продлевается
                }
            }
            if (m.fireTimer > 0) {
                if (--m.fireTimer % 20 == 0) {
                    --m.health;
                    mobSounds.push_back(MobSound{ "minecraft:entity.generic.burn", m.x, m.y, m.z });
                    if (m.health <= 0) { m.dead = true; m.deathTimer = 20; continue; }
                }
                if (m.fireTimer <= 0) {   // ушёл в тень — снимаем визуал огня
                    net::Buffer md;
                    md.writeVarInt(m.eid);
                    md.writeByte(0); md.writeVarInt(0); md.writeByte(0x00);
                    md.writeByte(0xFF);
                    sendAll(0x58, md);
                }
            }
        }
        if (victim) {
            const f64 dx = victim->getX() - m.x, dy = victim->getY() - m.y, dz = victim->getZ() - m.z;
            const f64 len = std::max(0.001, std::sqrt(dx * dx + dz * dz));
            // MOB_SPEED_V2: атрибут скорости уже задан в блоках за тик.
            // Потолок 0.26: моб не может обгонять спринт игрока просто за счёт кода ИИ.
            f64 chase = std::min(static_cast<f64>(m.def().speed), 0.26);
            // BABY_SPEED_V1: Zombie.setBaby() вешает модификатор скорости —
            // детёныши враждебных мобов носятся заметно быстрее взрослых.
            if (m.baby && m.hostile()) chase = std::min(chase * 1.5, 0.34);
            m.yaw = static_cast<f32>(std::atan2(-dx, dz) * 180.0 / 3.14159265358979);
            m.headYaw = m.yaw;
            m.targetEid = static_cast<i32>(victim->getEntityId());

            if (m.isKind("creeper")) {
                // Creeper.java: запал с 3 блоков, 30 тиков, взрыв радиусом 3.
                if (len <= 3.0) {
                    m.vx = 0; m.vz = 0;
                    if (m.fuseTimer < 0) {
                        m.fuseTimer = 0;
                        net::Buffer md;
                        md.writeVarInt(m.eid);
                        md.writeByte(16); md.writeVarInt(1); md.writeVarInt(1); // DATA_SWELL_DIR
                        md.writeByte(0xFF);
                        sendAll(0x58, md);
                        mobSounds.push_back(MobSound{ "minecraft:entity.creeper.primed", m.x, m.y, m.z });
                    }
                    if (++m.fuseTimer >= 30) {
                        mobBooms.push_back(MobBoom{ m.x, m.y + 0.5, m.z, 3.0f });
                        removed.push_back(m.eid);
                        m.dead = true; m.deathTimer = 0;
                        continue;
                    }
                } else {
                    if (m.fuseTimer >= 0) {
                        m.fuseTimer = -1;
                        net::Buffer md;
                        md.writeVarInt(m.eid);
                        md.writeByte(16); md.writeVarInt(1); md.writeVarInt(-1);
                        md.writeByte(0xFF);
                        sendAll(0x58, md);
                    }
                    mobNavigate(phys, m, victim->getX(), victim->getZ(), chase);
                }
            } else if (m.isKind("guardian") || m.isKind("elder_guardian")) {
                // GUARDIAN_BEAM_V1: Guardian.GuardianAttackGoal — луч заряжается 80 тиков в упорном
                // визуальном контакте и бьёт один раз, а не как обычный ближний бой.
                const f64 beamRange = m.isKind("elder_guardian") ? 15.0 : 12.0;
                if (len > beamRange) {
                    mobNavigate(phys, m, victim->getX(), victim->getZ(), chase);
                    m.shootCooldown = 80; // цель ушла — зарядка сбрасывается
                    // GUARDIAN_BEAM_V2: без этого луч на клиенте висел бы вечно.
                    if (m.beamTarget != 0) {
                        m.beamTarget = 0;
                        net::Buffer md;
                        md.writeVarInt(m.eid);
                        md.writeByte(16); md.writeVarInt(8); md.writeBool(false);  // DATA_ID_MOVING
                        md.writeByte(17); md.writeVarInt(1); md.writeVarInt(0);    // DATA_ID_ATTACK_TARGET
                        md.writeByte(0xFF);
                        sendAll(0x58, md);
                    }
                } else {
                    // GUARDIAN_BEAM_V2: луч рисуется клиентом по метаданным стража:
                    // индекс 16 — шипы прижаты/расправлены, индекс 17 — eid жертвы.
                    const i32 beamEid = victim->getEntityId();
                    if (m.beamTarget != beamEid) {
                        m.beamTarget = beamEid;
                        net::Buffer md;
                        md.writeVarInt(m.eid);
                        md.writeByte(16); md.writeVarInt(8); md.writeBool(true);
                        md.writeByte(17); md.writeVarInt(1); md.writeVarInt(beamEid);
                        md.writeByte(0xFF);
                        sendAll(0x58, md);
                    }
                    if (len < 4.0) { mobNavigate(phys, m, m.x - dx, m.z - dz, chase * 0.6); }
                    else { m.vx *= 0.6; m.vz *= 0.6; }
                    if (m.shootCooldown <= 0) {
                        m.shootCooldown = 80; // GuardianAttackGoal: attackTime до 80
                        mobHits.push_back(MobHit{ victim, m.def().damage * diffScale, std::string(m.def().name) });
                        mobSounds.push_back(MobSound{ m.isKind("elder_guardian")
                            ? "minecraft:entity.elder_guardian.curse" : "minecraft:entity.guardian.attack",
                            m.x, m.y, m.z });
                    }
                }
            } else if (m.isKind("slime") || m.isKind("magma_cube")) {
                // SLIME_JUMP_V1: SlimeMoveControl — слайм перемещается только прыжками
                // в сторону цели, а между прыжками тормозит, а не скользит по земле.
                if (m.onGround) {
                    if (m.leapCooldown <= 0) {
                        m.leapCooldown = m.baby ? 12 : 20;   // мелкие прыгают чаще
                        m.vy = 0.42;
                        const f64 push = chase * 2.2;
                        m.vx = dx / len * push;
                        m.vz = dz / len * push;
                        mobSounds.push_back(MobSound{ m.isKind("magma_cube")
                            ? "minecraft:entity.magma_cube.jump" : "minecraft:entity.slime.jump",
                            m.x, m.y, m.z });
                    } else {
                        m.vx *= 0.6; m.vz *= 0.6;   // пауза между прыжками
                    }
                }
                if (len <= 1.6 && m.attackCooldown <= 0 && !m.baby) {
                    m.attackCooldown = 20;
                    mobHits.push_back(MobHit{ victim, m.def().damage * diffScale, std::string(m.def().name) });
                    mobSounds.push_back(MobSound{ "minecraft:entity.slime.attack", m.x, m.y, m.z });
                }
            } else if (m.isKind("drowned") && (m.eid % 4) == 0) {
                // DROWNED_TRIDENT_V1: часть утопленников спавнится с трезубцем
                // и мечет его с дистанции вместо бега в упор.
                if (len > 10.0) mobNavigate(phys, m, victim->getX(), victim->getZ(), chase);
                else if (len < 4.0) mobNavigate(phys, m, m.x - dx, m.z - dz, chase * 0.8);
                else { m.vx *= 0.5; m.vz *= 0.5; }
                if (len <= 12.0 && m.shootCooldown <= 0) {
                    m.shootCooldown = (config_.difficulty >= 3) ? 50 : 70;
                    const f64 ex = dx, ez = dz;
                    const f64 ey = (victim->getY() + 1.0) - (m.y + static_cast<f64>(m.def().eyeHeight));
                    const f64 aim = ey + len * 0.12;
                    const f64 nrm = std::max(0.001, std::sqrt(ex * ex + aim * aim + ez * ez));
                    const f64 tSpeed = 1.8;
                    // Снаряд летит как стрела (id 4), но бьёт как трезубец — 8 урона.
                    mobShots.push_back(MobShot{ 4, m.dimension, m.eid,
                        m.x, m.y + static_cast<f64>(m.def().eyeHeight) - 0.1, m.z,
                        ex / nrm * tSpeed, aim / nrm * tSpeed, ez / nrm * tSpeed,
                        8.0f * diffScale, true, false });
                    mobSounds.push_back(MobSound{ "minecraft:item.trident.throw", m.x, m.y, m.z });
                }
            } else if (m.isKind("phantom")) {
                // PHANTOM_SWOOP_V1: Phantom.AttackPhase — фантом кружит высоко над целью
                // и периодически пикирует вниз, а не висит впритык.
                if (m.specialTimer > 0) {
                    mobNavigate(phys, m, victim->getX(), victim->getZ(), chase * 1.3);
                    m.vy = ((victim->getY() + 0.5) > m.y) ? 0.25 : -0.35;
                    m.specialTimer--;
                    if (len <= 1.8 && m.attackCooldown <= 0) {
                        m.attackCooldown = 20;
                        mobHits.push_back(MobHit{ victim, m.def().damage * diffScale, std::string(m.def().name) });
                        mobSounds.push_back(MobSound{ "minecraft:entity.phantom.bite", m.x, m.y, m.z });
                        m.specialTimer = 0;   // после укуса снова набирает высоту
                    }
                } else {
                    // Кружит на 7 блоков выше игрока, потом уходит в пике.
                    const f64 circleY = victim->getY() + 7.0;
                    m.vy = (m.y < circleY) ? 0.22 : -0.05;
                    const f64 ang = static_cast<f64>((m.eid * 7 + tickCounter_) % 360) * 0.0174533;
                    mobNavigate(phys, m, victim->getX() + std::cos(ang) * 6.0,
                        victim->getZ() + std::sin(ang) * 6.0, chase);
                    if (m.y >= circleY - 1.5 && m.shootCooldown <= 0) {
                        m.shootCooldown = 120;
                        m.specialTimer = 60;   // фаза пикирования
                        mobSounds.push_back(MobSound{ "minecraft:entity.phantom.swoop", m.x, m.y, m.z });
                    }
                }
            } else if (m.isKind("warden")) {
                // WARDEN_SONIC_V1: Warden.SonicBoom — на дистанции варден бьёт звуковой волной,
                // а вблизи переходит на обычный тяжёлый удар.
                mobNavigate(phys, m, victim->getX(), victim->getZ(), chase);
                if (len >= 5.0 && len <= 20.0 && m.shootCooldown <= 0) {
                    m.shootCooldown = 120;   // SonicBoom: кулдаун порядка 6 секунд
                    m.vx = 0; m.vz = 0;      // во время каста варден стоит
                    mobHits.push_back(MobHit{ victim, 10.0f * diffScale, std::string(m.def().name) });
                    mobSounds.push_back(MobSound{ "minecraft:entity.warden.sonic_charge", m.x, m.y, m.z });
                    mobSounds.push_back(MobSound{ "minecraft:entity.warden.sonic_boom", m.x, m.y, m.z });
                } else if (len <= 3.0 && m.attackCooldown <= 0) {
                    m.attackCooldown = 40;
                    mobHits.push_back(MobHit{ victim, m.def().damage * diffScale, std::string(m.def().name) });
                }
            } else if (m.isKind("evoker")) {
                // EVOKER_SPELL_V1: SpellcasterIllager — эвокер не дерётся врукопашную,
                // а чередует два заклинания: призыв вексов и ряд клыков по цели.
                if (len > 12.0) mobNavigate(phys, m, victim->getX(), victim->getZ(), chase);
                else if (len < 5.0) mobNavigate(phys, m, m.x - dx, m.z - dz, chase * 0.8);
                else { m.vx *= 0.6; m.vz *= 0.6; }
                if (m.shootCooldown <= 0) {
                    m.shootCooldown = 100;   // SpellcasterUseSpellGoal: каст раз в 5 секунд
                    // Метаданные DATA_SPELL_CASTING_ID: 1 = summon vex, 2 = fangs.
                    const bool summon = (m.specialTimer <= 0);
                    net::Buffer md;
                    md.writeVarInt(m.eid);
                    md.writeByte(17); md.writeVarInt(0); md.writeByte(summon ? 1 : 2);
                    md.writeByte(0xFF);
                    sendAll(0x58, md);
                    if (summon) {
                        // SummonSpellGoal: до 3 вексов вокруг эвокера, потом долгая пауза.
                        const i32 vexIdx = entity::mobIndexByName("vex");
                        if (vexIdx >= 0)
                            mobSummons.push_back(MobSummon{ vexIdx, m.dimension,
                                m.x + ((std::rand() % 5) - 2), m.y + 1.0, m.z + ((std::rand() % 5) - 2), 3 });
                        m.specialTimer = 340;   // вексов зовёт реже, чем кастует клыки
                        mobSounds.push_back(MobSound{ "minecraft:entity.evoker.prepare_summon", m.x, m.y, m.z });
                    } else {
                        // EVOKER_FANGS_V1: AttackSpellGoal больше не бьёт цель напрямую — вместо
                        // этого создаётся ряд настоящих evoker_fangs, которые выскакивают
                        // по очереди и кусают того, кто стоит рядом.
                        mobFangs.push_back(MobFangs{ m.dimension, m.x, m.y, m.z,
                            victim->getX(), victim->getY(), victim->getZ(), 6.0f * diffScale });
                        mobSounds.push_back(MobSound{ "minecraft:entity.evoker.prepare_attack", m.x, m.y, m.z });
                    }
                }
            } else if ((m.isKind("llama") || m.isKind("trader_llama")) && m.angryTimer > 0) {
                // LLAMA_SPIT_V1: Llama.LlamaAttackGoal — разозлённая лама не кусается,
                // а плюётся с дистанции: урон 1, залп раз в 3 секунды, ближе 3 блоков отходит.
                if (len > 8.0) mobNavigate(phys, m, victim->getX(), victim->getZ(), chase);
                else if (len < 3.0) mobNavigate(phys, m, m.x - dx, m.z - dz, chase * 0.8);
                else { m.vx *= 0.6; m.vz *= 0.6; }
                if (len <= 10.0 && m.shootCooldown <= 0) {
                    m.shootCooldown = 60;
                    const f64 sy = (victim->getY() + 1.0) - (m.y + static_cast<f64>(m.def().eyeHeight));
                    const f64 norm = std::max(0.001, std::sqrt(dx * dx + sy * sy + dz * dz));
                    const f64 spd = 1.5;   // LlamaSpit.shoot(): плевок летит настильно
                    mobShots.push_back(MobShot{ 66, m.dimension, m.eid,
                        m.x, m.y + static_cast<f64>(m.def().eyeHeight), m.z,
                        dx / norm * spd, sy / norm * spd + 0.05, dz / norm * spd,
                        1.0f * diffScale, true, false, 0, false, -1, 0, 0 });
                    mobSounds.push_back(MobSound{ "minecraft:entity.llama.spit", m.x, m.y, m.z });
                }
            } else if (m.ranged()) {
                // RangedAttackGoal: стрелки держат дистанцию, а не бегут в упор.
                // SHULKER_BULLET_V1: шалкер вообще не ходит — он прикреплён к блоку.
                const bool shulker = m.isKind("shulker");
                const f64 keep = m.isKind("ghast") ? 12.0 : 8.0;
                if (shulker) { m.vx = 0; m.vz = 0; }
                else if (len > keep + 2.0) { mobNavigate(phys, m, victim->getX(), victim->getZ(), chase); }
                else if (len < keep - 3.0) { mobNavigate(phys, m, m.x - dx, m.z - dz, chase * 0.8); }
                else if (m.isKind("skeleton") || m.isKind("stray") || m.isKind("bogged")
                         || m.isKind("pillager")) {
                    // SKELETON_STRAFE_V1: RangedBowAttackGoal.setStrafingClockwise() — стрелок
                    // не замирает, а смещается вбок, меняя направление каждые ~2 секунды.
                    const bool cw = (((tickCounter_ / 40) + m.eid) % 2) == 0;
                    const f64 sx = cw ? -dz : dz;
                    const f64 sz = cw ? dx : -dx;
                    mobNavigate(phys, m, m.x + sx, m.z + sz, chase * 0.6);
                }
                else { m.vx *= 0.5; m.vz *= 0.5; }
                if (m.shootCooldown <= 0) {
                    m.shootCooldown = (config_.difficulty >= 3) ? 30 : 40;
                    const bool fireball = m.isKind("ghast");
                    const bool smallBall = m.isKind("blaze");
                    const bool snow = m.isKind("snow_golem");
                    const bool potion = m.isKind("witch");   // WITCH_POTION_V1
                    if (potion) m.shootCooldown = 60;        // WitchAttackGoal: один бросок в 3 секунды
                    // CROSSBOW_V1: RangedCrossbowAttackGoal — арбалет стреляет реже лука,
                    // но болт летит быстрее и настильнее.
                    if (m.isKind("pillager")) m.shootCooldown = (config_.difficulty >= 3) ? 60 : 80;
                    if (shulker) m.shootCooldown = 100;      // ShulkerAttackGoal: залп раз в 5 секунд
                    // WITCH_POTION_V2: WitchAttackGoal кидает не только зелье вреда:
                    // далеко — замедление, вплотную — слабость, по здоровому — яд.
                    // id берутся из реестра mob_effect (speed = 0): slowness 1, weakness 17,
                    // poison 18, levitation 24.
                    i32 effId = -1, effAmp = 0, effDur = 0;
                    bool harming = true;
                    if (potion) {
                        if (len > 8.0) {
                            effId = 1; effDur = 600; harming = false; m.potionKind = 1;
                        } else if (victim->health >= 16.0f && (std::rand() % 4) == 0) {
                            effId = 18; effDur = 700; harming = false; m.potionKind = 2;
                        } else if (len <= 3.0 && (std::rand() % 4) == 0) {
                            effId = 17; effDur = 1800; harming = false; m.potionKind = 3;
                        } else {
                            m.potionKind = 0;   // зелье вреда II
                        }
                    }
                    // SHULKER_BULLET_V2: пуля шалкера вешает левитацию на 10 секунд.
                    if (shulker) { effId = 24; effAmp = 0; effDur = 200; }
                    const f64 ex = dx, ez = dz;
                    const f64 ey = (victim->getY() + 1.0) - (m.y + static_cast<f64>(m.def().eyeHeight));
                    const bool grav = !fireball && !smallBall && !shulker;
                    // Зелье летит по навесной дуге — ванильная ведьма целит заметно выше цели.
                    const f64 aim = potion ? ey + len * 0.35 : (grav ? ey + len * 0.15 : ey);
                    const f64 nrm = std::max(0.001, std::sqrt(ex * ex + aim * aim + ez * ez));
                    const f64 pSpeed = fireball ? 0.6 : (smallBall ? 0.7 : (potion ? 0.75
                        : (shulker ? 0.35 : (m.isKind("pillager") ? 2.0 : 1.6))));   // CROSSBOW_V1
                    // ENTITY_ID_FIX_V2: id из порядка объявления EntityType.java (сверено с 82 netId мобов):
                    // fireball 62, small_fireball 94, snowball 97, arrow 4, potion 82, shulker_bullet 89.
                    const i32 pType = fireball ? 62 : (smallBall ? 94
                        : (snow ? 97 : (potion ? 82 : (shulker ? 89 : 4))));
                    const f32 pDamage = fireball ? 0.0f
                        : (smallBall ? 5.0f * diffScale
                        : (snow ? 0.0f
                        : (potion ? (harming ? 6.0f * diffScale : 0.0f)   // WITCH_POTION_V2
                        : (shulker ? 4.0f * diffScale       // урон пули шалкера
                        : (2.0f + static_cast<f32>(std::rand() % 3)) * diffScale))));
                    mobShots.push_back(MobShot{ pType, m.dimension, m.eid,
                        m.x, m.y + static_cast<f64>(m.def().eyeHeight) - 0.1, m.z,
                        ex / nrm * pSpeed, aim / nrm * pSpeed, ez / nrm * pSpeed,
                        pDamage, grav, fireball,
                        shulker ? static_cast<i32>(victim->getEntityId()) : 0, shulker,   // SHULKER_BULLET_V2
                        effId, effAmp, effDur });                       // WITCH_POTION_V2
                    mobSounds.push_back(MobSound{ fireball ? "minecraft:entity.ghast.shoot"
                        : (smallBall ? "minecraft:entity.blaze.shoot"
                        : (potion ? "minecraft:entity.witch.throw"
                        : (shulker ? "minecraft:entity.shulker.shoot" : "minecraft:entity.skeleton.shoot"))),
                        m.x, m.y, m.z });
                }
            } else {
                mobNavigate(phys, m, victim->getX(), victim->getZ(), chase);
                // LeapAtTargetGoal: пауки прыгают на цель
                if ((m.isKind("spider") || m.isKind("cave_spider")) && m.onGround &&
                    m.leapCooldown <= 0 && len >= 2.0 && len <= 4.0 && (std::rand() % 5 == 0)) {
                    // LeapAtTargetGoal: normalize*0.4 + old velocity*0.2, yd=0.4.
                    m.leapCooldown = 20;
                    m.vy = 0.40;
                    m.vx = dx / len * 0.40 + m.vx * 0.20;
                    m.vz = dz / len * 0.40 + m.vz * 0.20;
                }
                const f64 reach = 1.2 + static_cast<f64>(m.def().width);
                if (len <= reach && std::abs(dy) <= 2.0 && m.attackCooldown <= 0 && m.def().damage > 0.0f) {
                    // Тяжёлые бьют реже: ванильный размашный удар у равагера/вардена длиннее.
                    m.attackCooldown = (m.def().damage >= 7.0f) ? 40 : 20;
                    mobHits.push_back(MobHit{ victim, m.def().damage * diffScale, std::string(m.def().name) });
                    // BEE_STING_V1: Bee.java hasStung — после укуса пчела теряет жало 300–400 тиков и умирает.
                    if (m.isKind("bee") && m.stingTimer <= 0) m.stingTimer = 300 + (std::rand() % 100);
                    // RAVAGER_ROAR_V1: Ravager.roar() — после удара равагер ревёт и задевает
                    // всех в радиусе 4 блоков, а не только основную цель.
                    if (m.isKind("ravager")) {
                        mobSounds.push_back(MobSound{ "minecraft:entity.ravager.roar", m.x, m.y, m.z });
                        for (auto& pl : players) {
                            if (!pl || !pl->isAlive() || pl->dead || pl->dimension != m.dimension) continue;
                            if (pl == victim) continue;
                            if (pl->gameMode == 1 || pl->gameMode == 3) continue;
                            const f64 rx = pl->getX() - m.x, ry = pl->getY() - m.y, rz = pl->getZ() - m.z;
                            if (rx * rx + ry * ry + rz * rz > 16.0) continue;
                            mobHits.push_back(MobHit{ pl, 6.0f * diffScale, std::string(m.def().name) });
                        }
                    }
                }
            }
        } else {
            m.targetEid = 0;
            if (m.fuseTimer >= 0) m.fuseTimer = -1;
        }

        // MOB_GOALS_V2: порядок как GoalSelector по флагу MOVE:
        // panic (1) > combat (2-5, выше) > tempt (3) > random_stroll (6).
        const f64 walkSpeed = std::min(static_cast<f64>(m.def().speed) * 0.45, 0.20);
        bool hasMoveGoal = victim != nullptr;
        if (!hasMoveGoal && m.panicTimer > 0) {
            --m.panicTimer;
            // PanicGoal: реально убегаем от того, кто ударил, а не выбираем случайный stroll.
            const f64 fx = m.x - m.panicX, fz = m.z - m.panicZ;
            mobNavigate(phys, m, m.x + fx, m.z + fz, std::min(static_cast<f64>(m.def().speed) * 1.25, 0.28));
            hasMoveGoal = true;
        }
        if (!hasMoveGoal && !m.hostile() && !m.baby) {
            // TemptGoal: животное само идёт к игроку, держащему подходящую еду.
            std::shared_ptr<entity::Player> foodPlayer;
            f64 foodD2 = 100.0; // 10 blocks
            for (auto& pl : players) {
                if (!pl || !pl->isAlive() || pl->dimension != m.dimension) continue;
                const i32 slot = 36 + pl->heldSlot;
                if (slot < 0 || slot >= entity::Player::INV_SIZE || !entity::mobLikesItem(m.typeIdx, pl->invItemId[slot])) continue;
                const f64 ddx = pl->getX() - m.x, ddz = pl->getZ() - m.z;
                const f64 d2 = ddx * ddx + ddz * ddz;
                if (d2 < foodD2) { foodD2 = d2; foodPlayer = pl; }
            }
            if (foodPlayer && foodD2 > 2.25) {
                mobNavigate(phys, m, foodPlayer->getX(), foodPlayer->getZ(), walkSpeed * 1.25);
                m.targetEid = static_cast<i32>(foodPlayer->getEntityId());
                hasMoveGoal = true;
            }
        }
        // FollowParentGoal: детёныш без более важной цели тянется к ближайшему взрослому своего вида.
        if (!hasMoveGoal && m.baby) {
            f64 bestD2 = 100.0; // 10 блоков в ванили
            entity::Mob* nearestAdult = nullptr;
            for (auto& other : mobs_) {
                if (other.dead || other.baby || other.typeIdx != m.typeIdx || other.dimension != m.dimension) continue;
                const f64 ddx = other.x - m.x, ddz = other.z - m.z;
                const f64 d2 = ddx * ddx + ddz * ddz;
                if (d2 < bestD2) { bestD2 = d2; nearestAdult = &other; }
            }
            if (nearestAdult && bestD2 > 6.25) { // держится дальше 2.5 блоков
                mobNavigate(phys, m, nearestAdult->x, nearestAdult->z, walkSpeed * 1.1);
                hasMoveGoal = true;
            }
        }
        // CREEPER_AVOID_CAT_V1: Creeper.registerGoals() — крипер шарахается от кошек
        // и оцелотов в радиусе 8 блоков, даж�� если рядом есть игрок.
        if (m.isKind("creeper")) {
            const entity::Mob* scaryCat = nullptr;
            f64 catD2 = 64.0;
            for (const auto& o : mobs_) {
                if (o.dead || o.dimension != m.dimension) continue;
                if (!o.isKind("cat") && !o.isKind("ocelot")) continue;
                const f64 cx = o.x - m.x, cz = o.z - m.z;
                const f64 cd2 = cx * cx + cz * cz;
                if (cd2 < catD2) { catD2 = cd2; scaryCat = &o; }
            }
            if (scaryCat) {
                mobNavigate(phys, m, m.x - (scaryCat->x - m.x), m.z - (scaryCat->z - m.z), walkSpeed * 1.5);
                hasMoveGoal = true;
                if (m.fuseTimer >= 0) {
                    m.fuseTimer = -1;   // испуганный крипер гасит запал
                    net::Buffer md;
                    md.writeVarInt(m.eid);
                    md.writeByte(16); md.writeVarInt(1); md.writeVarInt(-1);
                    md.writeByte(0xFF);
                    sendAll(0x58, md);
                }
            }
        }
        // VILLAGER_FLEE_V1: AvoidEntityGoal — житель убегает от зомби и иллагеров.
        if (!hasMoveGoal && (m.isKind("villager") || m.isKind("wandering_trader"))) {
            const entity::Mob* threat = nullptr;
            f64 threatD2 = 64.0;   // 8 блоков
            for (const auto& o : mobs_) {
                if (o.dead || o.dimension != m.dimension) continue;
                const char* on = o.def().name;
                const bool scary = std::strcmp(on, "zombie") == 0 || std::strcmp(on, "husk") == 0
                    || std::strcmp(on, "drowned") == 0 || std::strcmp(on, "zombie_villager") == 0
                    || std::strcmp(on, "pillager") == 0 || std::strcmp(on, "vindicator") == 0
                    || std::strcmp(on, "evoker") == 0 || std::strcmp(on, "vex") == 0
                    || std::strcmp(on, "ravager") == 0 || std::strcmp(on, "illusioner") == 0
                    || std::strcmp(on, "witch") == 0 || std::strcmp(on, "zoglin") == 0;
                if (!scary) continue;
                const f64 tx = o.x - m.x, tz = o.z - m.z;
                const f64 td2 = tx * tx + tz * tz;
                if (td2 < threatD2) { threatD2 = td2; threat = &o; }
            }
            if (threat) {
                mobNavigate(phys, m, m.x - (threat->x - m.x), m.z - (threat->z - m.z), walkSpeed * 1.4);
                hasMoveGoal = true;
                m.panicTimer = 40;
            }
        }
        // ZOMBIE_CONVERT_V1: Zombie.registerGoals() — NearestAttackableTargetGoal<Villager>.
        // Без игрока рядом нежить и иллагеры идут резать жителей, а не бродят вокруг них.
        if (!hasMoveGoal && !peaceful && !m.baby &&
            (m.isKind("zombie") || m.isKind("husk") || m.isKind("drowned") || m.isKind("zombie_villager")
             || m.isKind("vindicator") || m.isKind("pillager") || m.isKind("ravager"))) {
            const entity::Mob* prey = nullptr;
            f64 preyD2 = 32.0 * 32.0;
            for (const auto& o : mobs_) {
                if (o.dead || o.dimension != m.dimension) continue;
                if (!o.isKind("villager") && !o.isKind("wandering_trader") && !o.isKind("iron_golem")) continue;
                const f64 pdx = o.x - m.x, pdz = o.z - m.z;
                const f64 pd2 = pdx * pdx + pdz * pdz;
                if (pd2 < preyD2) { preyD2 = pd2; prey = &o; }
            }
            if (prey) {
                mobNavigate(phys, m, prey->x, prey->z, std::min(static_cast<f64>(m.def().speed), 0.26));
                hasMoveGoal = true;
                if (preyD2 <= 4.0 && m.attackCooldown <= 0 && m.def().damage > 0.0f) {
                    m.attackCooldown = (m.def().damage >= 7.0f) ? 40 : 20;
                    mobMelee.push_back(MobVsMob{ m.eid, prey->eid, m.def().damage * diffScale });
                }
            }
        }
        // WOLF_SIT_V1: SitWhenOrderedToGoal — посаженный питомец никуда не идёт.
        if (m.sitting) {
            m.vx = 0; m.vz = 0;
            hasMoveGoal = true;
        }
        // LLAMA_CARAVAN_V1: Llama.LlamaFollowCaravanGoal — торговые ламы идут
        // за ведущим цепочкой. Отстала больше 12 блоков — привязь подтягивает её
        // к каравану, убили ведущего — лама остаётся сама по себе.
        if (!hasMoveGoal && m.caravanLeader != 0) {
            const entity::Mob* lead = nullptr;
            for (const auto& o : mobs_) {
                if (o.dead || o.eid != m.caravanLeader) continue;
                lead = &o; break;
            }
            if (!lead || lead->dimension != m.dimension) {
                m.caravanLeader = 0;
            } else {
                const f64 ldx = lead->x - m.x, ldz = lead->z - m.z;
                const f64 ld2 = ldx * ldx + ldz * ldz;
                if (ld2 > 144.0) {
                    m.x = lead->x + ((std::rand() % 5) - 2) * 0.5;
                    m.y = lead->y;
                    m.z = lead->z + ((std::rand() % 5) - 2) * 0.5;
                    m.vx = 0.0; m.vz = 0.0;
                } else if (ld2 > 6.25) {
                    mobNavigate(phys, m, lead->x, lead->z, walkSpeed * 1.2);
                } else {
                    m.vx *= 0.6; m.vz *= 0.6;
                }
                hasMoveGoal = true;
            }
        }
        // FollowOwnerGoal: приручённые волк/кошка без цели боя/цели повыше тянутся к владельцу.
        if (!hasMoveGoal && m.tamed && m.owner > 0) {
            std::shared_ptr<entity::Player> ownerPl;
            for (auto& pl : players) {
                if (pl && static_cast<i32>(pl->getEntityId()) == m.owner && pl->isAlive() && pl->dimension == m.dimension) { ownerPl = pl; break; }
            }
            if (ownerPl) {
                const f64 odx = ownerPl->getX() - m.x, odz = ownerPl->getZ() - m.z;
                const f64 od2 = odx * odx + odz * odz;
                if (od2 > 100.0) { // дальше 10 блоков — телепортируется к владельцу, как в ванили
                    m.x = ownerPl->getX() - std::sin(static_cast<f64>(ownerPl->get_yaw()) * 3.14159265358979 / 180.0);
                    m.z = ownerPl->getZ() + std::cos(static_cast<f64>(ownerPl->get_yaw()) * 3.14159265358979 / 180.0);
                    m.y = ownerPl->getY();
                    m.vx = 0; m.vy = 0; m.vz = 0;
                } else if (od2 > 4.0) { // держится дальше 2 блоков
                    mobNavigate(phys, m, ownerPl->getX(), ownerPl->getZ(), std::min(static_cast<f64>(m.def().speed) * 0.7, 0.25));
                    hasMoveGoal = true;
                }
            }
        }
        if (!hasMoveGoal && --m.strollTimer <= 0) {
            m.strollTimer = 40 + (std::rand() % 120);
            if (std::rand() % 4 == 0) { m.vx = 0; m.vz = 0; }
            else {
                m.yaw = static_cast<f32>(std::rand() % 360);
                const f64 rad = m.yaw * 3.14159265358979 / 180.0;
                mobNavigate(phys, m, m.x - std::sin(rad) * 6.0, m.z + std::cos(rad) * 6.0, walkSpeed);
            }
            m.headYaw = m.yaw;
        }
        // FLY_SWIM_V1: WaterAvoidingRandomFlyingGoal и RandomSwimmingGoal — у летающих
        // и водных есть вертикальная составляющая блуждания, а рыба на суше рвётся к воде.
        if (!victim) {
            if (m.flying() && (std::rand() % 30) == 0) {
                // чаще вверх, чем вниз: моб не залипает у самой земли
                m.vy += ((std::rand() % 100) - 40) * 0.002;
            } else if (m.aquatic()) {
                const i32 wx = static_cast<i32>(std::floor(m.x));
                const i32 wy = static_cast<i32>(std::floor(m.y));
                const i32 wz = static_cast<i32>(std::floor(m.z));
                if (!phys.water(wx, wy, wz)) {
                    // выкинуло на сушу: ищем ближайшую воду в радиусе 8 блоков
                    f64 bestD2 = 1e9, bx = 0, bz = 0; bool haveWater = false;
                    for (i32 ox = -8; ox <= 8; ox += 2) {
                        for (i32 oz = -8; oz <= 8; oz += 2) {
                            for (i32 oy = -2; oy <= 1; ++oy) {
                                if (!phys.water(wx + ox, wy + oy, wz + oz)) continue;
                                const f64 d2 = static_cast<f64>(ox * ox + oz * oz);
                                if (d2 >= bestD2) continue;
                                bestD2 = d2; bx = wx + ox + 0.5; bz = wz + oz + 0.5; haveWater = true;
                            }
                        }
                    }
                    if (haveWater) mobNavigate(phys, m, bx, bz, walkSpeed);
                    if (m.onGround && (std::rand() % 6) == 0) m.vy = 0.42; // бьётся на суше
                } else if ((std::rand() % 25) == 0) {
                    m.vy += ((std::rand() % 100) - 50) * 0.0015;
                }
            }
        }
        // LookAtPlayerGoal: без движения моб всё равно поворачивает голову к ближнему игроку.
        if (!victim && (m.vx * m.vx + m.vz * m.vz) < 0.0004) {
            f64 lookD2 = 64.0; // 8 блоков
            for (auto& pl : players) {
                if (!pl || !pl->isAlive() || pl->dimension != m.dimension) continue;
                const f64 ldx = pl->getX() - m.x, ldz = pl->getZ() - m.z;
                const f64 ld2 = ldx * ldx + ldz * ldz;
                if (ld2 < lookD2) {
                    lookD2 = ld2;
                    m.headYaw = static_cast<f32>(std::atan2(-ldx, ldz) * 180.0 / 3.14159265358979);
                }
            }
        }
        entity::mobPhysicsStep(phys, m);

        // курица несёт яйцо (Chicken.java: eggTime)
        if (m.isKind("chicken") && !m.baby && --m.eggTimer <= 0) {
            m.eggTimer = 6000 + (std::rand() % 6000);
            eggDrops.push_back(m.x); eggDrops.push_back(m.y + 0.3); eggDrops.push_back(m.z);
        }

        // деспавн вдали от Игроков (ванильный порог 128 блоков)
        f64 nearest = 1e18;
        for (auto& pl : players) {
            if (!pl || pl->dimension != m.dimension) continue;
            const f64 dx = pl->getX() - m.x, dz = pl->getZ() - m.z;
            nearest = std::min(nearest, dx * dx + dz * dz);
        }
        if (nearest > 128.0 * 128.0) { removed.push_back(m.eid); continue; }

        // синхронизация позиции — Teleport Entity 0x70, раз в 2 тик�� и только при движении
        const bool moved = std::abs(m.x - m.lastSentX) > 0.01 || std::abs(m.y - m.lastSentY) > 0.01 ||
                           std::abs(m.z - m.lastSentZ) > 0.01 || std::abs(m.yaw - m.lastSentYaw) > 1.0f;
        if (moved && (tickCounter_ % 2 == 0)) {
            const auto ang = [](f32 deg) { return static_cast<u8>(static_cast<i32>(deg * 256.0f / 360.0f) & 0xFF); };
            net::Buffer tp;
            tp.writeVarInt(m.eid);
            tp.writeF64(m.x); tp.writeF64(m.y); tp.writeF64(m.z);
            tp.writeByte(ang(m.yaw)); tp.writeByte(ang(m.pitch));
            tp.writeBool(m.onGround);
            sendAll(0x70, tp);
            net::Buffer hr; hr.writeVarInt(m.eid); hr.writeByte(ang(m.headYaw));
            sendAll(0x48, hr); // Set Head Rotation �� иначе голова смотрит в одну сторону
            m.lastSentX = m.x; m.lastSentY = m.y; m.lastSentZ = m.z; m.lastSentYaw = m.yaw;
        }
    }

    // MOB_GOALS_V2: BreedGoal — пара взрослых одинакового вида в love mode создаёт детёныша.
    struct MobBirth { i32 type; i32 dim; f64 x; f64 y; f64 z; };
    std::vector<MobBirth> births;
    for (size_t a = 0; a < mobs_.size(); ++a) {
        auto& left = mobs_[a];
        if (left.dead || left.baby || left.loveTicks <= 0 || left.breedCooldown > 0) continue;
        for (size_t b = a + 1; b < mobs_.size(); ++b) {
            auto& right = mobs_[b];
            if (right.dead || right.baby || right.typeIdx != left.typeIdx || right.dimension != left.dimension ||
                right.loveTicks <= 0 || right.breedCooldown > 0) continue;
            const f64 dx = right.x - left.x, dz = right.z - left.z;
            if (dx * dx + dz * dz > 9.0) continue;
            births.push_back({ left.typeIdx, left.dimension, (left.x + right.x) * 0.5, (left.y + right.y) * 0.5, (left.z + right.z) * 0.5 });
            left.loveTicks = right.loveTicks = 0;
            left.breedCooldown = right.breedCooldown = 6000;
            break;
        }
    }

    // ZOMBIE_CONVERT_V1: удары мобов по мобам — одним проходом, уже после цикла ИИ.
    // Zombie.killedEntity(): на normal половина убитых жителей встаёт зомби-жителем,
    // на hard — все, на easy конверсии нет вообще.
    for (const auto& blow : mobMelee) {
        bool zombieKill = false;
        f64 ax = 0.0, az = 0.0;
        for (const auto& o : mobs_) {
            if (o.eid != blow.attacker) continue;
            ax = o.x; az = o.z;
            zombieKill = o.isKind("zombie") || o.isKind("husk") || o.isKind("drowned")
                      || o.isKind("zombie_villager");
            break;
        }
        entity::Mob* prey = nullptr;
        for (auto& o : mobs_) if (o.eid == blow.target && !o.dead) { prey = &o; break; }
        if (!prey || prey->hurtCooldown > 0) continue;
        prey->hurtCooldown = 10;
        prey->health -= std::max(1, static_cast<i32>(blow.damage + 0.5f));
        prey->panicTimer = 100;
        prey->panicX = ax; prey->panicZ = az;
        if (prey->isKind("iron_golem")) { prey->angryTimer = 600; prey->targetEid = blow.attacker; }
        if (prey->health > 0) continue;
        prey->dead = true; prey->deathTimer = 20;
        const i32 zvIdx = entity::mobIndexByName("zombie_villager");
        if (zombieKill && zvIdx >= 0 && prey->isKind("villager") && config_.difficulty >= 2
            && (config_.difficulty >= 3 || (std::rand() % 2) == 0)) {
            mobConverts.push_back(MobConvert{ zvIdx, prey->dimension, prey->x, prey->y, prey->z,
                                              prey->baby, "minecraft:entity.zombie.infect" });
            removed.push_back(prey->eid);
            prey->deathTimer = 0;
        }
    }

    if (!removed.empty()) {
        mobs_.erase(std::remove_if(mobs_.begin(), mobs_.end(), [&](const entity::Mob& m) {
            return std::find(removed.begin(), removed.end(), m.eid) != removed.end();
        }), mobs_.end());
    }
    mobsLock.unlock();

    if (!removed.empty()) {
        net::Buffer rm;
        rm.writeVarInt(static_cast<i32>(removed.size()));
        for (i32 eid : removed) rm.writeVarInt(eid);
        sendAll(0x42, rm);
    }
    for (size_t i = 0; i + 2 < eggDrops.size(); i += 3)
        spawnItemDrop(eggDrops[i], eggDrops[i + 1], eggDrops[i + 2], entity::ITEM_EGG, 1, 0.0, 0.1, 0.0, 10);
    for (const auto& birth : births) {
        const i32 babyEid = static_cast<i32>(nextEntityId_.load());
        spawnMobAt(birth.type, birth.x, birth.y, birth.z, birth.dim, 1);
        std::lock_guard lk(mobsMutex_);
        for (auto& child : mobs_) if (child.eid == babyEid) {
            child.baby = true;
            child.health = std::max(1, child.health / 2);
            break;
        }
    }
    // ZOMBIE_CONVERT_V1: сама подмена — старый eid уже в removed, новый спавним тут.
    for (const auto& c : mobConverts) {
        const i32 newEid = static_cast<i32>(nextEntityId_.load());
        spawnMobAt(c.toType, c.x, c.y, c.z, c.dim, 1);
        if (c.baby) {
            std::lock_guard lk(mobsMutex_);
            for (auto& nm : mobs_) if (nm.eid == newEid) { nm.baby = true; break; }
        }
        broadcastBlockSound(c.sound, static_cast<i32>(std::floor(c.x)),
                            static_cast<i32>(std::floor(c.y)), static_cast<i32>(std::floor(c.z)), 1.0f, 1.0f);
    }

    // MOBS_ALL_V1: урон от враждебных мобов — уже без mobsMutex_.
    // MOBS_AI_V1: ванильные i-frames. Раньше каждый моб бил независимо, и трое зомби
    // сносили половину здоровья за секунду; теперь считается самый сильный удар
    // за окно в 10 тиков, как в LivingEntity.hurt().
    for (auto& pl : players) if (pl && pl->mobHurtCooldown > 0) --pl->mobHurtCooldown;
    std::sort(mobHits.begin(), mobHits.end(),
              [](const MobHit& a, const MobHit& b) { return a.damage > b.damage; });
    for (const auto& s : mobSummons)
        spawnMobAt(s.typeIdx, s.x, s.y, s.z, s.dim, s.count);   // EVOKER_SPELL_V1
    for (auto& h : mobHits) {
        if (!h.victim || h.victim->mobHurtCooldown > 0) continue;
        h.victim->mobHurtCooldown = 10;
        // HUSK_DETAILS_V1: Husk.doHurtTarget() — удар хаска вешает Голод
        // на 7 секунд за уровень сложности (id эффекта hunger = 16).
        if (h.mob == "husk" && config_.difficulty > 0)
            addPlayerEffect(h.victim, 16, 0, 140 * config_.difficulty);
        applyEnvironmentalDamage(h.victim, h.damage, 9,
            std::format("{} убит: {}", h.victim->getName(), h.mob));
    }
    for (auto& s : mobShots)
        spawnMobProjectile(s.typeId, s.dim, s.owner, s.x, s.y, s.z, s.vx, s.vy, s.vz,
                           s.damage, s.gravity, s.explosive,
                           s.targetEid, s.homing, s.effectId, s.effectAmp, s.effectDur);
    for (const auto& f : mobFangs)   // EVOKER_FANGS_V1
        spawnEvokerFangLine(f.dim, f.ox, f.oy, f.oz, f.tx, f.ty, f.tz, f.damage);
    for (auto& b : mobBooms)
        explodeAt(b.x, b.y, b.z, b.radius, 0, 0);
    for (auto& s : mobSounds)
        broadcastBlockSound(s.name, static_cast<i32>(std::floor(s.x)),
                            static_cast<i32>(std::floor(s.y)),
                            static_cast<i32>(std::floor(s.z)), 1.0f, 1.0f);
}

// MOBS_AI_V1: снаряды мобов. tickProjectiles() рассчитан на владельца-игрока
// и только на оверворлд, поэтому у мобов свой список: стрела скелета,
// фаербол гаста (взрывается), огонь блейза, снежок снежного голема.
void NetherCraftServer::spawnMobProjectile(i32 typeId, i32 dim, i32 ownerEid, f64 x, f64 y, f64 z,
                                           f64 vx, f64 vy, f64 vz, f32 damage, bool gravity, bool explosive,
                                           i32 targetEid, bool homing,
                                           i32 effectId, i32 effectAmp, i32 effectDur) {
    const i32 eid = static_cast<i32>(nextEntityId_++);
    MobProjectile q{ eid, typeId, dim, ownerEid, x, y, z, vx, vy, vz, damage, 0, gravity, explosive,
                     targetEid, homing, effectId, effectAmp, effectDur };
    { std::lock_guard lk(mobProjectilesMutex_); mobProjectiles_.push_back(q); }
    net::Buffer sp;
    sp.writeVarInt(eid);
    sp.writeUUID(UUID{ static_cast<u64>(eid), 0x73000000ULL + static_cast<u64>(eid) });
    sp.writeVarInt(typeId);
    sp.writeF64(x); sp.writeF64(y); sp.writeF64(z);
    sp.writeByte(0); sp.writeByte(0); sp.writeByte(0);
    sp.writeVarInt(ownerEid);
    auto vel = [](f64 v) { return static_cast<i16>(std::clamp(v * 8000.0, -32000.0, 32000.0)); };
    sp.writeI16(vel(vx)); sp.writeI16(vel(vy)); sp.writeI16(vel(vz));
    const auto bytes = std::vector<u8>(sp.writtenSpan().begin(), sp.writtenSpan().end());
    for (auto& p : getAllPlayersCopy())
        if (p && p->isAlive() && p->dimension == dim) p->getConnection()->sendPacket(0x01, bytes);
}

void NetherCraftServer::tickMobProjectiles() {
    std::vector<MobProjectile> list;
    { std::lock_guard lk(mobProjectilesMutex_); list = mobProjectiles_; }
    if (list.empty()) return;
    auto players = getAllPlayersCopy();
    std::vector<i32> gone;
    struct Boom { f64 x; f64 y; f64 z; };
    std::vector<Boom> booms;
    struct Hit { std::shared_ptr<entity::Player> victim; f32 damage;
                 i32 effectId = -1; i32 effectAmp = 0; i32 effectDur = 0; };
    std::vector<Hit> hits;
    for (auto& q : list) {
        ++q.age;
        // SHULKER_BULLET_V2: пуля шалкера живёт дольше — она догоняет, а не летит по прямо��.
        if (q.age > (q.homing ? 400 : 200)) { gone.push_back(q.eid); continue; }
        if (q.homing) {
            // ShulkerBullet.tick(): снаряд постоянно доворачивает к цели,
            // поэтому от него нельзя просто отойти в сторону.
            for (auto& p : players) {
                if (!p || p->dead || !p->isAlive() || p->dimension != q.dim) continue;
                if (p->getEntityId() != q.targetEid) continue;
                const f64 tx = p->getX() - q.x;
                const f64 ty = (p->getY() + 1.0) - q.y;
                const f64 tz = p->getZ() - q.z;
                const f64 tl = std::max(0.001, std::sqrt(tx * tx + ty * ty + tz * tz));
                const f64 sp = 0.32;
                q.vx += (tx / tl * sp - q.vx) * 0.18;
                q.vy += (ty / tl * sp - q.vy) * 0.18;
                q.vz += (tz / tl * sp - q.vz) * 0.18;
                const f64 vl = std::max(0.001, std::sqrt(q.vx * q.vx + q.vy * q.vy + q.vz * q.vz));
                q.vx = q.vx / vl * sp; q.vy = q.vy / vl * sp; q.vz = q.vz / vl * sp;
                break;
            }
        }
        auto& w = worldFor(q.dim);
        const f64 ox = q.x, oy = q.y, oz = q.z;
        const f64 nx = ox + q.vx, ny = oy + q.vy, nz = oz + q.vz;
        const i32 steps = std::max(1, static_cast<i32>(std::ceil(
            std::sqrt(q.vx * q.vx + q.vy * q.vy + q.vz * q.vz) / 0.15)));
        bool contact = false;
        f64 hx = nx, hy = ny, hz = nz;
        std::shared_ptr<entity::Player> victim;
        for (i32 i = 1; i <= steps && !contact; ++i) {
            const f64 a = static_cast<f64>(i) / static_cast<f64>(steps);
            const f64 sx = ox + (nx - ox) * a, sy = oy + (ny - oy) * a, sz = oz + (nz - oz) * a;
            if (mobStateIsSolid(w.getBlock(static_cast<i32>(std::floor(sx)),
                                           static_cast<i32>(std::floor(sy)),
                                           static_cast<i32>(std::floor(sz))))) {
                contact = true; hx = sx; hy = sy; hz = sz; break;
            }
            for (auto& p : players) {
                if (!p || p->dead || !p->isAlive() || p->dimension != q.dim) continue;
                if (p->gameMode == 1 || p->gameMode == 3) continue;
                const f64 pdx = p->getX() - sx, pdz = p->getZ() - sz;
                if (pdx * pdx + pdz * pdz > 0.45 * 0.45) continue;
                if (sy < p->getY() - 0.1 || sy > p->getY() + 1.9) continue;
                contact = true; hx = sx; hy = sy; hz = sz; victim = p; break;
            }
        }
        if (contact) {
            if (victim && (q.damage > 0.0f || q.effectId >= 0))
                hits.push_back(Hit{ victim, q.damage, q.effectId, q.effectAmp, q.effectDur });
            if (q.explosive) booms.push_back(Boom{ hx, hy, hz });
            gone.push_back(q.eid);
            net::Buffer ev; ev.writeI32(q.eid); ev.writeByte(3);
            const auto evBytes = std::vector<u8>(ev.writtenSpan().begin(), ev.writtenSpan().end());
            for (auto& p : players)
                if (p && p->isAlive() && p->dimension == q.dim) p->getConnection()->sendPacket(0x1F, evBytes);
            continue;
        }
        q.x = nx; q.y = ny; q.z = nz;
        if (q.gravity) q.vy -= 0.05;   // Arrow/Snowball
        net::Buffer tp;
        tp.writeVarInt(q.eid);
        tp.writeF64(q.x); tp.writeF64(q.y); tp.writeF64(q.z);
        tp.writeByte(0); tp.writeByte(0); tp.writeBool(false);
        const auto tpBytes = std::vector<u8>(tp.writtenSpan().begin(), tp.writtenSpan().end());
        for (auto& p : players)
            if (p && p->isAlive() && p->dimension == q.dim) p->getConnection()->sendPacket(0x70, tpBytes);
    }
    {
        std::lock_guard lk(mobProjectilesMutex_);
        for (const auto& q : list)
            for (auto& cur : mobProjectiles_)
                if (cur.eid == q.eid) { cur = q; break; }
        if (!gone.empty())
            mobProjectiles_.erase(std::remove_if(mobProjectiles_.begin(), mobProjectiles_.end(),
                [&](const MobProjectile& p) {
                    return std::find(gone.begin(), gone.end(), p.eid) != gone.end();
                }), mobProjectiles_.end());
    }
    if (!gone.empty()) {
        net::Buffer rm;
        rm.writeVarInt(static_cast<i32>(gone.size()));
        for (i32 eid : gone) rm.writeVarInt(eid);
        const auto rmBytes = std::vector<u8>(rm.writtenSpan().begin(), rm.writtenSpan().end());
        for (auto& p : players) if (p && p->isAlive()) p->getConnection()->sendPacket(0x42, rmBytes);
    }
    for (auto& h : hits) {
        if (!h.victim) continue;
        // WITCH_POTION_V2 / SHULKER_BULLET_V2: эффект вешается даже тогда,
        // когда сам снаряд урона не наносит (замедление, слабость, яд).
        if (h.effectId >= 0) addPlayerEffect(h.victim, h.effectId, h.effectAmp, h.effectDur);
        if (h.damage <= 0.0f) continue;
        if (h.victim->mobHurtCooldown > 0) continue;
        h.victim->mobHurtCooldown = 10;
        applyEnvironmentalDamage(h.victim, h.damage, 9,
            std::format("{} застрелен", h.victim->getName()));
    }
    for (auto& b : booms) explodeAt(b.x, b.y, b.z, 1.0f, 0, 0);
}

// EFFECTS_V1: Update Mob Effect (0x76) — VarInt eid, VarInt id эффекта из реестра
// mob_effect, byte уровень, VarInt длительность в тиках, byte флаги
// (0x01 ambient, 0x02 частицы, 0x04 иконка, 0x08 blend).
void NetherCraftServer::sendMobEffect(const std::shared_ptr<entity::Player>& player, i32 effectId,
                                      i32 amplifier, i32 durationTicks) {
    if (!player || effectId < 0 || durationTicks <= 0) return;
    if (!player->isAlive() || !player->getConnection()) return;
    net::Buffer eff;
    eff.writeVarInt(player->getEntityId());
    eff.writeVarInt(effectId);
    eff.writeByte(static_cast<u8>(amplifier));
    eff.writeVarInt(durationTicks);
    eff.writeByte(0x02 | 0x04);
    const auto bytes = std::vector<u8>(eff.writtenSpan().begin(), eff.writtenSpan().end());
    player->getConnection()->sendPacket(0x76, bytes);
}

// EFFECTS_V2: LivingEntity.addEffect() — эффект теперь живёт на сервере, а не только
// в виде пакета клиенту. Новый эффект перебивает старый по ванильному правилу:
// больший уровень либо тот же уровень с большей оставшейся длительностью.
void NetherCraftServer::addPlayerEffect(const std::shared_ptr<entity::Player>& player, i32 effectId,
                                        i32 amplifier, i32 durationTicks) {
    if (!player || effectId < 0 || durationTicks <= 0) return;
    if (!player->isAlive() || player->dead) return;
    for (auto& e : player->effects) {
        if (e.id != effectId) continue;
        if (amplifier < e.amplifier) return;
        if (amplifier == e.amplifier && durationTicks <= e.ticks) return;
        e.amplifier = amplifier;
        e.ticks = durationTicks;
        sendMobEffect(player, effectId, amplifier, durationTicks);
        return;
    }
    entity::Player::ActiveEffect fresh;
    fresh.id = effectId; fresh.amplifier = amplifier; fresh.ticks = durationTicks;
    player->effects.push_back(fresh);
    sendMobEffect(player, effectId, amplifier, durationTicks);
}

// EFFECTS_V2: уровень эффекта (0 = I) или -1, если эффекта нет.
i32 NetherCraftServer::playerEffectAmplifier(const std::shared_ptr<entity::Player>& player, i32 effectId) {
    if (!player) return -1;
    for (const auto& e : player->effects)
        if (e.id == effectId && e.ticks > 0) return e.amplifier;
    return -1;
}

// EFFECTS_V2: Remove Mob Effect (0x43) — VarInt eid + VarInt id эффекта.
void NetherCraftServer::removePlayerEffect(const std::shared_ptr<entity::Player>& player, i32 effectId) {
    if (!player) return;
    bool had = false;
    for (size_t i = 0; i < player->effects.size(); ++i) {
        if (player->effects[i].id != effectId) continue;
        player->effects.erase(player->effects.begin() + static_cast<std::ptrdiff_t>(i));
        had = true;
        break;
    }
    if (!had || !player->getConnection()) return;
    net::Buffer rm;
    rm.writeVarInt(player->getEntityId());
    rm.writeVarInt(effectId);
    player->getConnection()->sendPacket(0x43, std::vector<u8>(rm.writtenSpan().begin(), rm.writtenSpan().end()));
}

// EFFECTS_APPLY_V1: MobEffectInstance.tick() — периодика яда, иссушения и регенерации
// плюс истечение длительности. Сила, Слабость и Сопротивление читаются в бою.
// id из реестра mob_effect (0-based): регенерация 9, яд 18, иссушение 19.
void NetherCraftServer::tickPlayerEffects() {
    for (auto& player : getAllPlayersCopy()) {
        if (!player || !player->getConnection()) continue;
        if (player->effects.empty()) continue;
        if (!player->isAlive() || player->dead) { player->effects.clear(); continue; }
        ++player->effectPulse;
        std::vector<i32> expired;
        for (auto& e : player->effects) {
            if (--e.ticks <= 0) { expired.push_back(e.id); continue; }
            const i32 step = e.amplifier + 1;
            if (e.id == 18) {                       // яд: по полсердца, но не добивает
                const i32 period = std::max(5, 25 / step);
                if (player->effectPulse % period == 0 && player->health > 1.0f)
                    applyEnvironmentalDamage(player, 1.0f, 9, std::format("{} отравлен", player->getName()));
            } else if (e.id == 19) {                // иссушение бьёт насмерть
                const i32 period = std::max(5, 40 / step);
                if (player->effectPulse % period == 0)
                    applyEnvironmentalDamage(player, 1.0f, 9, std::format("{} иссох", player->getName()));
            } else if (e.id == 9) {                 // регенерация
                const i32 period = std::max(10, 50 / step);
                if (player->effectPulse % period == 0 && player->health < 20.0f) {
                    player->health = std::min(20.0f, player->health + 1.0f);
                    net::Buffer hp;
                    hp.writeF32(player->health); hp.writeVarInt(20); hp.writeF32(5.0f);
                    player->getConnection()->sendPacket(0x5D,
                        std::vector<u8>(hp.writtenSpan().begin(), hp.writtenSpan().end()));
                }
            }
        }
        for (i32 id : expired) removePlayerEffect(player, id);
    }
}

// EVOKER_FANGS_V1: EvokerFangs — отдельная сущность (тип 36), а не мгновенный урон.
// AttackSpellGoal строит ряд клыков от эвокера к жертве и два кольца вокруг неё.
void NetherCraftServer::spawnEvokerFangLine(i32 dim, f64 ox, f64 oy, f64 oz,
                                            f64 tx, f64 ty, f64 tz, f32 damage) {
    const f64 dx = tx - ox, dz = tz - oz;
    const f64 len = std::max(0.001, std::sqrt(dx * dx + dz * dz));
    std::vector<EvokerFang> fresh;
    const i32 line = std::min(16, static_cast<i32>(len / 1.25) + 1);
    for (i32 i = 1; i <= line; ++i) {
        EvokerFang f;
        f.dim = dim;
        f.x = ox + dx / len * 1.25 * static_cast<f64>(i);
        f.z = oz + dz / len * 1.25 * static_cast<f64>(i);
        f.y = oy;
        f.delay = i;              // клыки выскакивают волной, по одному за тик
        f.damage = damage;
        fresh.push_back(f);
    }
    for (i32 i = 0; i < 5; ++i) {
        const f64 a = static_cast<f64>(i) * 1.2566370614;   // 2*pi/5
        for (i32 ring = 0; ring < 2; ++ring) {
            const f64 r = (ring == 0) ? 1.5 : 2.5;
            EvokerFang f;
            f.dim = dim;
            f.x = tx + std::cos(a) * r;
            f.z = tz + std::sin(a) * r;
            f.y = ty;
            f.delay = line + 2 + ring * 3;
            f.damage = damage;
            fresh.push_back(f);
        }
    }
    auto& w = worldFor(dim);
    auto players = getAllPlayersCopy();
    std::vector<EvokerFang> spawned;
    for (auto& f : fresh) {
        const i32 bx = static_cast<i32>(std::floor(f.x));
        const i32 bz = static_cast<i32>(std::floor(f.z));
        const i32 by0 = static_cast<i32>(std::floor(f.y));
        i32 ground = -1000;
        for (i32 by = by0 + 1; by >= by0 - 4; --by) {
            if (mobStateIsSolid(w.getBlock(bx, by, bz))
                && !mobStateIsSolid(w.getBlock(bx, by + 1, bz))) { ground = by + 1; break; }
        }
        if (ground == -1000) continue;   // нет опоры — клык не вырастает, как в ванили
        f.y = static_cast<f64>(ground);
        f.eid = static_cast<i32>(nextEntityId_++);
        net::Buffer sp;
        sp.writeVarInt(f.eid);
        sp.writeUUID(UUID{ static_cast<u64>(f.eid), 0x36000000ULL + static_cast<u64>(f.eid) });
        sp.writeVarInt(36);   // evoker_fangs
        sp.writeF64(f.x); sp.writeF64(f.y); sp.writeF64(f.z);
        sp.writeByte(0);
        sp.writeByte(static_cast<u8>(std::rand() % 256));   // поворот клыка
        sp.writeByte(0);
        sp.writeVarInt(0);
        sp.writeI16(0); sp.writeI16(0); sp.writeI16(0);
        const auto bytes = std::vector<u8>(sp.writtenSpan().begin(), sp.writtenSpan().end());
        for (auto& p : players)
            if (p && p->isAlive() && p->dimension == dim) p->getConnection()->sendPacket(0x01, bytes);
        spawned.push_back(f);
    }
    if (spawned.empty()) return;
    std::lock_guard lk(evokerFangsMutex_);
    for (const auto& f : spawned) evokerFangs_.push_back(f);
}

void NetherCraftServer::tickEvokerFangs() {
    std::vector<EvokerFang> list;
    { std::lock_guard lk(evokerFangsMutex_); list = evokerFangs_; }
    if (list.empty()) return;
    auto players = getAllPlayersCopy();
    std::vector<i32> gone;
    struct FangHit { std::shared_ptr<entity::Player> victim; f32 damage; };
    std::vector<FangHit> hits;
    for (auto& f : list) {
        ++f.age;
        if (f.age < f.delay) continue;
        const i32 local = f.age - f.delay;
        if (local == 1)
            broadcastBlockSound("minecraft:entity.evoker_fangs.attack",
                                static_cast<i32>(std::floor(f.x)),
                                static_cast<i32>(std::floor(f.y)),
                                static_cast<i32>(std::floor(f.z)), 1.0f, 1.0f);
        // warmupDelayTicks = 20: урон только в момент щелчка челюстей.
        if (local >= 20 && !f.struck) {
            f.struck = true;
            for (auto& p : players) {
                if (!p || p->dead || !p->isAlive() || p->dimension != f.dim) continue;
                if (p->gameMode == 1 || p->gameMode == 3) continue;
                const f64 pdx = p->getX() - f.x, pdz = p->getZ() - f.z;
                if (pdx * pdx + pdz * pdz > 1.0) continue;
                if (p->getY() < f.y - 1.5 || p->getY() > f.y + 2.0) continue;
                hits.push_back(FangHit{ p, f.damage });
            }
        }
        if (local > 24) gone.push_back(f.eid);
    }
    {
        std::lock_guard lk(evokerFangsMutex_);
        for (const auto& f : list)
            for (auto& cur : evokerFangs_)
                if (cur.eid == f.eid) { cur = f; break; }
        if (!gone.empty())
            evokerFangs_.erase(std::remove_if(evokerFangs_.begin(), evokerFangs_.end(),
                [&](const EvokerFang& f) {
                    return std::find(gone.begin(), gone.end(), f.eid) != gone.end();
                }), evokerFangs_.end());
    }
    if (!gone.empty()) {
        net::Buffer rm;
        rm.writeVarInt(static_cast<i32>(gone.size()));
        for (i32 eid : gone) rm.writeVarInt(eid);
        const auto rmBytes = std::vector<u8>(rm.writtenSpan().begin(), rm.writtenSpan().end());
        for (auto& p : players) if (p && p->isAlive()) p->getConnection()->sendPacket(0x42, rmBytes);
    }
    for (auto& h : hits) {
        if (!h.victim || h.victim->mobHurtCooldown > 0) continue;
        h.victim->mobHurtCooldown = 10;
        applyEnvironmentalDamage(h.victim, h.damage, 9,
            std::format("{} разорван клыками эвокера", h.victim->getName()));
    }
}

// Атака по мобу: урон, отбрасывание, смерть и дроп по loot_table.
bool NetherCraftServer::mobAttack(const std::shared_ptr<entity::Player>& player, i32 targetEid) {
    if (!player) return false;
    entity::Mob hit;
    bool found = false, died = false;
    {
        std::lock_guard lk(mobsMutex_);
        for (auto& m : mobs_) {
            if (m.eid != targetEid || m.dead) continue;
            found = true;
            if (m.hurtCooldown > 0) { hit = m; break; }
            // EFFECTS_APPLY_V1: Сила (id 4) даёт +3 урона за уровень, Слабость (id 17) — -4.
            f32 handHit = 1.0f;
            const i32 strAmp = playerEffectAmplifier(player, 4);
            if (strAmp >= 0) handHit += 3.0f * static_cast<f32>(strAmp + 1);
            const i32 weakAmp = playerEffectAmplifier(player, 17);
            if (weakAmp >= 0) handHit -= 4.0f * static_cast<f32>(weakAmp + 1);
            if (handHit < 0.0f) handHit = 0.0f;
            m.health -= static_cast<i32>(handHit + 0.5f);                       // базовый удар рукой; урон оружия — заход 2
            m.hurtCooldown = 10;                 // invulnerableTime
            m.panicTimer = 100; // PanicGoal около 5 секунд
            m.panicX = player->getX(); m.panicZ = player->getZ();
            // MOBS_ALL_V1: нейтральные (эндермен, свинозомби, волк…) злятся на 400 тиков
            if (m.neutral()) { m.angryTimer = 400; m.targetEid = static_cast<i32>(player->getEntityId()); }
            // MOBS_AI_V1: Enderman.teleport() — уходит из-под удара в радиусе 16 блоков
            if (m.isKind("enderman") && m.health > 0 && m.tpCooldown <= 0) {
                auto& tw = worldFor(m.dimension);
                for (i32 t = 0; t < 8; ++t) {
                    const i32 tx = static_cast<i32>(m.x) + (std::rand() % 33) - 16;
                    const i32 tz = static_cast<i32>(m.z) + (std::rand() % 33) - 16;
                    i32 ty = 0; bool ok = false;
                    for (i32 y = static_cast<i32>(m.y) + 8; y > static_cast<i32>(m.y) - 16; --y) {
                        if (!mobStateIsSolid(tw.getBlock(tx, y, tz))) continue;
                        if (mobStateIsSolid(tw.getBlock(tx, y + 1, tz))) break;
                        if (mobStateIsSolid(tw.getBlock(tx, y + 2, tz))) break;
                        ty = y + 1; ok = true; break;
                    }
                    if (!ok) continue;
                    m.x = tx + 0.5; m.y = static_cast<f64>(ty); m.z = tz + 0.5;
                    m.vx = 0; m.vy = 0; m.vz = 0;
                    m.tpCooldown = 40;
                    break;
                }
            }
            const f64 dx = m.x - player->getX(), dz = m.z - player->getZ();
            const f64 len = std::max(0.001, std::sqrt(dx * dx + dz * dz));
            m.vx += dx / len * 0.4; m.vz += dz / len * 0.4; m.vy = 0.36; // knockback
            if (m.health <= 0) { m.dead = true; m.deathTimer = 20; died = true; }
            hit = m;
            break;
        }
    }
    if (!found) return false;

    // GOLEM_DEFEND_V1: IronGolem.DefendVillageTargetGoal — удар по жителю поднимает
    // всех железных големов в радиусе 24 блоков.
    if (hit.isKind("villager") || hit.isKind("wandering_trader")) {
        const i32 golemTarget = static_cast<i32>(player->getEntityId());
        std::lock_guard lk(mobsMutex_);
        for (auto& o : mobs_) {
            if (o.dead || o.dimension != hit.dimension) continue;
            if (!o.isKind("iron_golem")) continue;
            const f64 gx = o.x - hit.x, gy = o.y - hit.y, gz = o.z - hit.z;
            if (gx * gx + gy * gy + gz * gz > 576.0) continue;
            o.angryTimer = 600;
            o.targetEid = golemTarget;
        }
    }

    // GROUP_ANGER_V1: NearestAttackableTargetGoal + HurtByTargetGoal.setAlertOthers().
    // Волчья стая, свинозомби, пчёлы и прочие нейтралы злятся всей группой,
    // а взрослые вступаются за своих детёнышей (белый медв��дь, хоглин).
    if (hit.neutral()) {
        const f64 alertRange = hit.isKind("zombified_piglin") ? 32.0
            : (hit.isKind("bee") ? 20.0 : 16.0);
        const i32 attacker = static_cast<i32>(player->getEntityId());
        std::lock_guard lk(mobsMutex_);
        for (auto& o : mobs_) {
            if (o.dead || o.eid == hit.eid) continue;
            if (o.dimension != hit.dimension) continue;
            if (o.typeIdx != hit.typeIdx) continue;   // только свой вид
            if (o.tamed && o.owner == static_cast<i32>(player->getEntityId())) continue; // свой питомец
            if (o.baby) continue;                     // детёныши не бросаются в бой
            const f64 ax = o.x - hit.x, ay = o.y - hit.y, az = o.z - hit.z;
            if (ax * ax + ay * ay + az * az > alertRange * alertRange) continue;
            o.angryTimer = 400;
            o.targetEid = attacker;
        }
    }

    auto players = getAllPlayersCopy();
    auto sendAll = [&](i32 packetId, const net::Buffer& b) {
        auto v = std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end());
        for (auto& pl : players)
            if (pl && pl->isAlive() && pl->getState() == entity::PlayerState::Play && pl->dimension == hit.dimension)
                pl->getConnection()->sendPacket(packetId, v);
    };
    // MOB_FEEDBACK_V2: Entity Event 0x1F = fixed I32 entity id + byte event.
    // 0x24 Hurt Animation отдельно использует VarInt entity id + float yaw.
    // Не смешивать форматы: VarInt в 0x1F даёт клиенту пакет длиной 3 байта и DecoderException.
    net::Buffer ev; ev.writeI32(hit.eid); ev.writeByte(died ? 3 : 2);
    sendAll(0x1F, ev);
    if (!died) {
        net::Buffer hurt; hurt.writeVarInt(hit.eid); hurt.writeF32(0.0f);
        sendAll(0x24, hurt); // Hurt Animation
    }
    const char* soundBase = hit.def().name;
    if (hit.isKind("mooshroom")) soundBase = "cow";
    else if (hit.isKind("cave_spider")) soundBase = "spider";
    else if (hit.isKind("trader_llama")) soundBase = "llama";
    if (!hit.isKind("giant")) {
        const std::string sound = std::format("minecraft:entity.{}.{}", soundBase, died ? "death" : "hurt");
        broadcastBlockSound(sound.c_str(), static_cast<i32>(std::floor(hit.x)),
                            static_cast<i32>(std::floor(hit.y)), static_cast<i32>(std::floor(hit.z)),
                            1.0f, 1.0f);
    }

    if (died) {
        for (const auto& d : entity::mobDrops(hit.typeIdx)) {
            const i32 count = d.min + (d.max > d.min ? (std::rand() % (d.max - d.min + 1)) : 0);
            if (count <= 0) continue;
            if (hit.isKind("sheep") && d.itemId == entity::ITEM_WHITE_WOOL && hit.sheared) continue;
            spawnItemDrop(hit.x, hit.y + 0.3, hit.z, d.itemId, count, 0.0, 0.1, 0.0, 10);
        }
        spawnExperienceOrb(hit.x, hit.y + 0.3, hit.z, 1 + (std::rand() % 3)); // ванильные 1-3 за животное
        // RAID_WAVES_V1: Raids.java — убийство капитана патруля вешает Дурное предзнаменование.
        // Своего баннера на голове у нас нет, поэтому капитаном считается каждый
        // четвёртый иллагер по eid и любой эвокер.
        if ((hit.isKind("pillager") || hit.isKind("vindicator") || hit.isKind("evoker"))
            && ((hit.eid % 4) == 0 || hit.isKind("evoker"))) {
            player->badOmen = std::min(5, player->badOmen + 1);
            player->sendSystemMessage(std::format("§5Дурное предзнаменование {}", player->badOmen));
        }
        // SPLIT_V1: Slime.java remove() — взрослый (не baby) слайм/магма-куб при смерти делится на 2-4 мелких копии.
        if (!hit.baby && (hit.isKind("slime") || hit.isKind("magma_cube"))) {
            spawnMobAt(hit.typeIdx, hit.x, hit.y, hit.z, hit.dimension, 2 + (std::rand() % 3), true);
        }
    }
    return true;
}

// ПКМ по мобу: ведро у коровы → молоко, ножницы у овцы → шерсть.
// Это не дроп, а mobInteract() из Cow.java / Sheep.java — в loot_table его нет.
bool NetherCraftServer::mobInteract(const std::shared_ptr<entity::Player>& player, i32 targetEid) {
    if (!player) return false;
    entity::Mob target;
    bool found = false;
    i32 openTrade = 0;        // VILLAGER_TRADE_V1: eid жителя, чьё окно надо открыть после блокировки
    bool curedSound = false;  // ZOMBIE_CONVERT_V1: звук начавшегося лечения
    {
        std::lock_guard lk(mobsMutex_);
        for (auto& m : mobs_) {
            if (m.eid != targetEid || m.dead) continue;
            found = true; target = m;
            const i32 slot = 36 + player->heldSlot;
            const i32 held = player->invItemId[slot];
            if (m.def().milkable && held == entity::ITEM_BUCKET && !m.baby) {   // корова, муушрум, коза
                player->invItemId[slot] = entity::ITEM_MILK_BUCKET; // ItemUtils.createFilledResult
                player->invCount[slot] = 1;
            } else if (m.def().shearable && held == entity::ITEM_SHEARS && !m.sheared) {
                m.sheared = true;
                spawnItemDrop(m.x, m.y + 0.3, m.z, entity::ITEM_WHITE_WOOL, 1 + (std::rand() % 3), 0.0, 0.1, 0.0, 10);
            } else if (!m.baby && m.breedCooldown <= 0 && entity::mobLikesItem(m.typeIdx, held)) {
                // Animal#useItem: еда переводит животное в love mode на 600 тиков.
                m.loveTicks = 600;
                if (player->gameMode != 1 && player->invCount[slot] > 0) --player->invCount[slot];
            } else if (!m.tamed && held == entity::mobTameItem(m.typeIdx) && held >= 0 && entity::mobTameItem(m.typeIdx) >= 0) {
                // TAME_V1: Wolf#mobInteract / Cat#mobInteract — 1/3 шанс приручить за один предмет, иначе просто тратится.
                if (player->gameMode != 1 && player->invCount[slot] > 0) --player->invCount[slot];
                if (std::rand() % 3 == 0) {
                    m.tamed = true;
                    m.owner = static_cast<i32>(player->getEntityId());
                    m.health = m.def().maxHealth; // полное исцеление при успешном приручении
                }
            } else if (m.tamed && m.owner == static_cast<i32>(player->getEntityId()) && held <= 0) {
                // WOLF_SIT_V1: ПКМ пустой рукой по своему питомцу — сесть/встать.
                m.sitting = !m.sitting;
                if (m.sitting) { m.vx = 0; m.vz = 0; m.panicTimer = 0; }
            } else if (m.isKind("zombie_villager") && held == entity::ITEM_GOLDEN_APPLE && m.convertTimer <= 0) {
                // ZOMBIE_CONVERT_V1: ZombieVillager.startConverting() — золотое яблоко запускает
                // лечение на 3600-6000 тиков. Предварительной Слабости пока не требуем:
                // зельеварение ещё не сделано, иначе механику было бы не запустить.
                if (player->gameMode != 1 && player->invCount[slot] > 0) --player->invCount[slot];
                m.convertTimer = 3600 + (std::rand() % 2400);
                m.convertTo = entity::mobIndexByName("villager");
                curedSound = true;
            } else if ((m.isKind("villager") || m.isKind("wandering_trader")) && !m.baby) {
                // VILLAGER_TRADE_V1: Villager.mobInteract() — ПКМ по жителю открывает окно торговли.
                openTrade = m.eid;
            } else if (m.isKind("piglin") && !m.baby && held == entity::ITEM_GOLD_INGOT && m.specialTimer <= 0) {
                // BARTER_V1: PiglinBarterGoal — отдаёт золото, через короткий кулдаун роняет предмет обратно.
                if (player->gameMode != 1 && player->invCount[slot] > 0) --player->invCount[slot];
                m.specialTimer = 40; // ~2 секунды на "осм��тр слитка"
                const i32 lootId = entity::mobBarterLoot(std::rand());
                spawnItemDrop(m.x, m.y + 1.0, m.z, lootId, 1 + (std::rand() % 3), 0.0, 0.15, 0.0, 10);
            }
            break;
        }
    }
    if (!found) return false;

    // синхронизировать слот с клиентом (Set Container Slot 0x15)
    const i32 slot = 36 + player->heldSlot;
    net::Buffer sb;
    sb.writeByte(0); sb.writeVarInt(0); sb.writeI16(static_cast<i16>(slot));
    sb.writeVarInt(player->invCount[slot]);
    if (player->invCount[slot] > 0) { sb.writeVarInt(player->invItemId[slot]); sb.writeVarInt(0); sb.writeVarInt(0); }
    player->getConnection()->sendPacket(0x15, std::vector<u8>(sb.writtenSpan().begin(), sb.writtenSpan().end()));
    // ZOMBIE_CONVERT_V1: звук начавшегося лечения — уже без mobsMutex_.
    if (curedSound)
        broadcastBlockSound("minecraft:entity.zombie_villager.cure", static_cast<i32>(std::floor(target.x)),
                            static_cast<i32>(std::floor(target.y)), static_cast<i32>(std::floor(target.z)), 1.0f, 1.0f);
    // VILLAGER_TRADE_V1: окно торговли открываем после синхронизации слота.
    if (openTrade != 0) openVillagerTradeFor(player, target);
    return true;
}

// VILLAGER_TRADE_V1: Open Screen (0x33, menu type 19 = minecraft:merchant) + Merchant Offers (0x2D).
// Формат 1.21.1: вход — ItemCost (VarInt id, VarInt count, VarInt число предикатов),
// выход — обычный Slot (VarInt count, VarInt id, VarInt add, VarInt remove).
void NetherCraftServer::openVillagerTradeFor(const std::shared_ptr<entity::Player>& player,
                                             const entity::Mob& villager) {
    if (!player || !player->isAlive()) return;
    const i32 profession = villager.isKind("wandering_trader") ? entity::VP_FLETCHER : villager.profession;
    const auto offers = entity::villagerOffers(profession);
    if (offers.empty()) {
        // Villager.java: безработный житель просто не открывает меню.
        broadcastBlockSound("minecraft:entity.villager.no", static_cast<i32>(std::floor(villager.x)),
                            static_cast<i32>(std::floor(villager.y)), static_cast<i32>(std::floor(villager.z)), 1.0f, 1.0f);
        return;
    }
    const i32 wid = player->nextWindowId++;
    if (player->nextWindowId > 99) player->nextWindowId = 1;
    player->openWindowId = wid;
    player->openContainerKey = 0;
    player->openContainerKey2 = 0;
    player->openIsDouble = false;
    player->openIsEnder = false;
    player->openMerchantEid = villager.eid;

    // VILLAGER_LEVEL_V1: VillagerData.level — ванильные пороги опыта торговли
    // 0/10/70/150/250: новичок, подмастерье, специалист, эксперт, мастер.
    // Каждый уровень открывает следующую строку ассортимента.
    const i32 tradeXp = villager.tradeXp;
    const i32 level = tradeXp >= 250 ? 5 : tradeXp >= 150 ? 4 : tradeXp >= 70 ? 3 : tradeXp >= 10 ? 2 : 1;
    const i32 unlocked = std::min(static_cast<i32>(offers.size()), level + 1);

    net::Buffer open;
    open.writeVarInt(wid);
    open.writeVarInt(19); // minecraft:merchant
    writeTextComponent(open, entity::villagerProfessionName(profession));
    player->getConnection()->sendPacket(0x33, std::vector<u8>(open.writtenSpan().begin(), open.writtenSpan().end()));

    net::Buffer mo;
    mo.writeVarInt(wid);
    mo.writeVarInt(static_cast<i32>(offers.size()));
    for (size_t oi = 0; oi < offers.size(); ++oi) {
        const auto& o = offers[oi];
        // VILLAGER_LEVEL_V1: сделки выше уровня и распроданные уходят закрытыми.
        const bool locked = static_cast<i32>(oi) >= unlocked || villager.tradeUses >= o.maxUses;
        mo.writeVarInt(o.buyId); mo.writeVarInt(o.buyCount); mo.writeVarInt(0);   // ItemCost #1
        mo.writeVarInt(o.sellCount);                                             // Slot: count
        mo.writeVarInt(o.sellId); mo.writeVarInt(0); mo.writeVarInt(0);          // Slot: id + компоненты
        if (o.buy2Id > 0) {
            mo.writeByte(1);                                                     // Optional ItemCost #2
            mo.writeVarInt(o.buy2Id); mo.writeVarInt(o.buy2Count); mo.writeVarInt(0);
        } else {
            mo.writeByte(0);
        }
        mo.writeByte(locked ? 1 : 0);   // trade disabled
        mo.writeI32(locked ? o.maxUses : std::min(villager.tradeUses, o.maxUses)); // uses
        mo.writeI32(o.maxUses);     // max uses
        mo.writeI32(o.xp);          // xp за сделку
        mo.writeI32(0);             // special price
        mo.writeF32(0.05f);         // price multiplier
        mo.writeI32(0);             // demand
    }
    mo.writeVarInt(level);          // VILLAGER_LEVEL_V1: уровень из накопленного опыта
    mo.writeVarInt(villager.tradeXp);
    mo.writeByte(villager.isKind("wandering_trader") ? 0 : 1); // is regular villager
    mo.writeByte(1);                                          // can restock
    player->getConnection()->sendPacket(0x2D, std::vector<u8>(mo.writtenSpan().begin(), mo.writtenSpan().end()));
}

// VILLAGER_TRADE_V1: MerchantMenu.tryMoveItems() — списать цену из рюкзака и выдать товар.
// Слоты 9..44 — рюкзак + хотбар; броню и оффхенд торговля не трогает.
bool NetherCraftServer::villagerSelectTrade(const std::shared_ptr<entity::Player>& player, i32 index) {
    if (!player || player->openMerchantEid == 0) return false;
    entity::Mob merchant;
    bool found = false;
    {
        std::lock_guard lk(mobsMutex_);
        for (auto& m : mobs_)
            if (m.eid == player->openMerchantEid && !m.dead) { merchant = m; found = true; break; }
    }
    if (!found) return false;
    const i32 profession = merchant.isKind("wandering_trader") ? entity::VP_FLETCHER : merchant.profession;
    const auto offers = entity::villagerOffers(profession);
    if (index < 0 || index >= static_cast<i32>(offers.size())) return false;
    const auto& o = offers[static_cast<size_t>(index)];

    auto countItem = [&](i32 id) {
        i32 n = 0;
        if (id <= 0) return n;
        for (i32 s = 9; s <= 44; ++s) if (player->invItemId[s] == id) n += player->invCount[s];
        return n;
    };
    auto takeItem = [&](i32 id, i32 need) {
        for (i32 s = 9; s <= 44 && need > 0; ++s) {
            if (player->invItemId[s] != id || player->invCount[s] <= 0) continue;
            const i32 take = std::min(need, player->invCount[s]);
            player->invCount[s] -= take;
            need -= take;
            if (player->invCount[s] <= 0) { player->invItemId[s] = 0; player->invCount[s] = 0; }
        }
    };
    auto refuse = [&](const char* why) {
        player->sendSystemMessage(std::string("§c") + why);
        broadcastBlockSound("minecraft:entity.villager.no", static_cast<i32>(std::floor(merchant.x)),
                            static_cast<i32>(std::floor(merchant.y)), static_cast<i32>(std::floor(merchant.z)), 1.0f, 1.0f);
        return false;
    };

    // VILLAGER_LEVEL_V1: сервер не верит клиенту и сам проверяет уровень и остаток сделки.
    const i32 tradeXp = merchant.tradeXp;
    const i32 level = tradeXp >= 250 ? 5 : tradeXp >= 150 ? 4 : tradeXp >= 70 ? 3 : tradeXp >= 10 ? 2 : 1;
    if (index >= std::min(static_cast<i32>(offers.size()), level + 1))
        return refuse("Житель ещё не открыл эту сделку");
    if (merchant.tradeUses >= o.maxUses)
        return refuse("Житель распродал товар, зайдите позже");

    if (countItem(o.buyId) < o.buyCount) return refuse("Житель качает головой: не хватает предметов");
    if (o.buy2Id > 0 && countItem(o.buy2Id) < o.buy2Count)
        return refuse("Житель качает головой: не хватает второго предмета");

    i32 dest = -1;
    for (i32 s = 9; s <= 44; ++s)
        if (player->invItemId[s] == o.sellId && player->invCount[s] > 0
            && player->invCount[s] + o.sellCount <= 64) { dest = s; break; }
    if (dest < 0)
        for (i32 s = 9; s <= 44; ++s)
            if (player->invItemId[s] <= 0 || player->invCount[s] <= 0) { dest = s; break; }
    if (dest < 0) return refuse("Инвентарь полон");

    takeItem(o.buyId, o.buyCount);
    if (o.buy2Id > 0) takeItem(o.buy2Id, o.buy2Count);
    if (player->invItemId[dest] == o.sellId && player->invCount[dest] > 0) player->invCount[dest] += o.sellCount;
    else { player->invItemId[dest] = o.sellId; player->invCount[dest] = o.sellCount; }
    sendFullPlayerInventory(player);

    {
        std::lock_guard lk(mobsMutex_);
        for (auto& m : mobs_)
            if (m.eid == merchant.eid) { ++m.tradeUses; m.tradeXp += o.xp; break; }
    }
    // Villager.rewardTradeXp(): игроку падает 3-6 опыта за сделку.
    spawnExperienceOrb(merchant.x, merchant.y + 0.5, merchant.z, 3 + (std::rand() % 4));
    broadcastBlockSound("minecraft:entity.villager.yes", static_cast<i32>(std::floor(merchant.x)),
                        static_cast<i32>(std::floor(merchant.y)), static_cast<i32>(std::floor(merchant.z)), 1.0f, 1.0f);
    return true;
}

// RAID_WAVES_V1: Raid.java — старт рейда вокруг центра деревни.
void NetherCraftServer::startRaid(i32 dim, f64 x, f64 y, f64 z) {
    Raid r;
    r.id = nextRaidId_++;
    r.dim = dim; r.x = x; r.y = y; r.z = z;
    r.wave = 0;
    // Число волн как в ванили зависит от сложности: 3 / 5 / 7.
    r.totalWaves = (config_.difficulty >= 3) ? 7 : (config_.difficulty <= 1 ? 3 : 5);
    r.spawnDelay = 40;
    r.finished = false;
    raids_.push_back(r);
    for (auto& p : getAllPlayersCopy()) {
        if (!p || !p->isAlive() || p->dimension != dim) continue;
        const f64 dx = p->getX() - x, dz = p->getZ() - z;
        if (dx * dx + dz * dz > 96.0 * 96.0) continue;
        p->sendSystemMessage(std::format("§cНачался рейд! Волн: {}", r.totalWaves));
    }
    broadcastBlockSound("minecraft:event.raid.horn", static_cast<i32>(std::floor(x)),
                        static_cast<i32>(std::floor(y)), static_cast<i32>(std::floor(z)), 8.0f, 1.0f);
}

// RAID_WAVES_V1: состав волны — упрощённая таблица Raid.RaiderType.
// С каждой волной прибавляются новые типы: виндикаторы, ведьма, равагер, эвокер.
void NetherCraftServer::spawnRaidWave(Raid& r) {
    struct WaveEntry { const char* name; i32 count; };
    const i32 w = r.wave;
    std::vector<WaveEntry> comp;
    comp.push_back(WaveEntry{ "pillager", 2 + w / 2 });
    if (w >= 2) comp.push_back(WaveEntry{ "vindicator", 1 + w / 3 });
    if (w >= 3) comp.push_back(WaveEntry{ "witch", 1 });
    if (w >= 4) comp.push_back(WaveEntry{ "ravager", 1 });
    if (w >= 5) comp.push_back(WaveEntry{ "evoker", 1 });
    auto& rw = worldFor(r.dim);
    for (const auto& e : comp) {
        const i32 idx = entity::mobIndexByName(e.name);
        if (idx < 0) continue;
        for (i32 i = 0; i < e.count; ++i) {
            const f64 ang = (std::rand() % 360) * 3.14159265358979 / 180.0;
            const f64 rad = 18.0 + (std::rand() % 9);
            const f64 sx = r.x + std::cos(ang) * rad;
            const f64 sz = r.z + std::sin(ang) * rad;
            // Ищем поверхность, чтобы рейдеры не сыпались в гору и не висли в воздухе.
            const i32 bx = static_cast<i32>(std::floor(sx));
            const i32 bz = static_cast<i32>(std::floor(sz));
            f64 sy = r.y;
            for (i32 yy = static_cast<i32>(r.y) + 12; yy > static_cast<i32>(r.y) - 12; --yy) {
                if (!mobStateIsSolid(rw.getBlock(bx, yy, bz))) continue;
                if (mobStateIsSolid(rw.getBlock(bx, yy + 1, bz))) continue;
                if (mobStateIsSolid(rw.getBlock(bx, yy + 2, bz))) continue;
                sy = static_cast<f64>(yy + 1);
                break;
            }
            const i32 newEid = static_cast<i32>(nextEntityId_.load());
            spawnMobAt(idx, sx, sy, sz, r.dim, 1);
            std::lock_guard lk(mobsMutex_);
            for (auto& m : mobs_) {
                if (m.eid != newEid) continue;
                m.raidId = r.id;
                m.raidWave = w;
                m.angryTimer = 6000;   // рейдер агрессивен всю волну, а не только в агро-радиусе
                break;
            }
        }
    }
    for (auto& p : getAllPlayersCopy()) {
        if (!p || !p->isAlive() || p->dimension != r.dim) continue;
        const f64 dx = p->getX() - r.x, dz = p->getZ() - r.z;
        if (dx * dx + dz * dz > 96.0 * 96.0) continue;
        p->sendSystemMessage(std::format("§cВолна {}/{}", w, r.totalWaves));
    }
    broadcastBlockSound("minecraft:event.raid.horn", static_cast<i32>(std::floor(r.x)),
                        static_cast<i32>(std::floor(r.y)), static_cast<i32>(std::floor(r.z)), 8.0f, 1.0f);
}

// RAID_WAVES_V1: тик рейдов — вызыва��тся раз в 20 тиков.
void NetherCraftServer::tickRaids() {
    if (config_.difficulty == 0) return; // Raids.java: на peaceful рейдов нет
    // 1. Старт: игрок с Дурным предзнаменованием зашёл в деревню.
    for (auto& p : getAllPlayersCopy()) {
        if (!p || !p->isAlive() || p->dead || p->badOmen <= 0) continue;
        bool nearVillage = false;
        f64 vx = 0, vy = 0, vz = 0;
        {
            std::lock_guard lk(mobsMutex_);
            for (auto& m : mobs_) {
                if (m.dead || m.dimension != p->dimension) continue;
                if (!m.isKind("villager") && !m.isKind("iron_golem")) continue;
                const f64 dx = m.x - p->getX(), dz = m.z - p->getZ();
                if (dx * dx + dz * dz > 32.0 * 32.0) continue;
                nearVillage = true; vx = m.x; vy = m.y; vz = m.z;
                break;
            }
        }
        if (!nearVillage) continue;
        bool already = false;
        for (const auto& r : raids_) {
            if (r.finished || r.dim != p->dimension) continue;
            const f64 dx = r.x - vx, dz = r.z - vz;
            if (dx * dx + dz * dz <= 64.0 * 64.0) { already = true; break; }
        }
        if (already) continue;
        p->badOmen = 0; // эффект тратится на вызов рейда
        startRaid(p->dimension, vx, vy, vz);
    }
    // 2. Волны: следующая только после зачистки предыдущей.
    for (auto& r : raids_) {
        if (r.finished) continue;
        i32 alive = 0;
        {
            std::lock_guard lk(mobsMutex_);
            for (const auto& m : mobs_) if (!m.dead && m.raidId == r.id) ++alive;
        }
        if (alive > 0) continue;
        if (r.spawnDelay > 0) { r.spawnDelay -= 20; continue; }
        if (r.wave >= r.totalWaves) {
            r.finished = true;
            for (auto& p : getAllPlayersCopy()) {
                if (!p || !p->isAlive() || p->dimension != r.dim) continue;
                const f64 dx = p->getX() - r.x, dz = p->getZ() - r.z;
                if (dx * dx + dz * dz > 96.0 * 96.0) continue;
                p->sendSystemMessage("§aРейд отбит!");
            }
            broadcastBlockSound("minecraft:ui.toast.challenge_complete", static_cast<i32>(std::floor(r.x)),
                                static_cast<i32>(std::floor(r.y)), static_cast<i32>(std::floor(r.z)), 4.0f, 1.0f);
            continue;
        }
        ++r.wave;
        spawnRaidWave(r);
        r.spawnDelay = 60;
    }
    raids_.erase(std::remove_if(raids_.begin(), raids_.end(),
                                [](const Raid& r) { return r.finished; }), raids_.end());
}

void NetherCraftServer::tick() {
    // TICKPROF_V1: микро-профайлер тика — ищет, что ест TPS. Замер общего времени
    // тика и ��яжёлых ��аз; при тике >52мс (просадка ниже 20 TPS) пишет виновника в лог.
    using namespace std::chrono;
    const auto _tp_start = steady_clock::now();
    auto _tp_mark = _tp_start;
    f64 _tp_console = 0.0, _tp_drain = 0.0, _tp_boss = 0.0, _tp_tab = 0.0, _tp_time = 0.0, _tp_keep = 0.0, _tp_save = 0.0; // TICKPROF_V2
    auto _tp_lap = [&]() { const auto _n = steady_clock::now(); const f64 _d = duration<f64, std::milli>(_n - _tp_mark).count(); _tp_mark = _n; return _d; };

    g_dimCtx = 0; // DIMPHYS_V1: тик-поток стартует в оверворлде
    processConsoleCommands(); // CONSOLE_V2: all world/network operations remain on the tick thread
    _tp_console = _tp_lap(); // TICKPROF_V2
    // FASTBOOT_V1: install chunks streamed from disk by the background loader (main thread only).
    world_.drainLoadedChunks();
    {
        static bool s_worldLoadLogged = false;
        if (!s_worldLoadLogged && world_.isBackgroundLoadDone()) {
            s_worldLoadLogged = true;
            if (config_.language == "rus") NC_INFO("Server", "Фоновая загрузка снимка мира завершена");
            else NC_INFO("Server", "World snapshot background load complete");
        }
    }
    _tp_drain = _tp_lap(); // TICKPROF_V2 (drainChunks + world-load-лог)
    tickCounter_++;
    flushMiniEditPackets(); // FASTASYNC_V1: at most 32 section updates per tick
    updateTpsBossbar();
    _tp_boss = _tp_lap();

    // STRESS_FIX_V1: пачка join/leave за тик шлёт один tab_list пакет вместо одного на каждого бота —
    // раньше 100 join/с = 100 широковещательных рассылок/с, каждая по всем онлайн игрокам (O(n^2) шторм).
    if (tabListDirty_.exchange(false, std::memory_order_relaxed)) {
        broadcastTabListHeaderFooter();
    }
    _tp_tab = _tp_lap(); // TICKPROF_V2

    tickItemDrops(); // ITEMDROP_V1: фи��ика/подбор выпавших предметов
    tickFluids();    // FLUID_V1: динамика воды/лавы (поток, высыхание, камень/обсидиан)
    tickFire();      // FIRE_V2: scheduled decay, spread, TNT ignition
    tickFallingBlocks(); // FALLING_V1: scheduled blocks + FallingBlockEntity motion/landing
    tickPrimedTnt(); // TNT_V1: fuse, movement, explosion and chain reactions
    tickBulkTntCollapse(); // TNT_ANTILAG_V1: bounded connected-component collapse
    tickTntLightResync(); // TNT_ANTILAG_V3: bounded Chunk Data + Light repair
    tickProjectiles(); // PROJECTILE_V1: raycast, drag/gravity, ender pearl teleport
    tickExperienceOrbs(); // XP_ORB_V1: gravity, magnet pickup, grantExperience on collect
    tickVehicles(); // VEHICLE_PHYSICS_V1: rail following, boat float, passenger sync
    tickMobs(); // MOBS_V1: спавн/физика/деспавн мобов
    tickMobProjectiles(); // MOBS_AI_V1: стрелы/фаерболы/снежки мобов
    tickPortals();
    tickSpawnWarmups(); // SPAWNCFG_V1        // PORTAL_V2: переход через порталы Ада/Энда
    tickPlayerEffects();  // EFFECTS_APPLY_V1: периодика и истечение эффектов игрока
    tickEvokerFangs();    // EVOKER_FANGS_V1: волна клыков эвокера
    if (tickCounter_ % 20 == 0) tickRaids(); // RAID_WAVES_V1: волны рейда раз в секунду
    // MEM_V3: раньше выгрузка чанков запускалась ТОЛЬКО когда игрок менял
    // центральный чанк. Стоишь на месте (или вышел) — нагенерированное висело
    // в ОЗУ до конца жизни процесса. Теперь чистим раз в 5 секунд безусловно.
    if (tickCounter_ % 100 == 0) pruneAllWorlds();
    tickPlayerEnvironment(); // ENV_V1: заморозка в рыхлом снегу и урон лавой
    tickRandomBlockUpdates(); // RANDOM_TICK_V1: crop growth, cane/cactus, leaf decay, farmland
    tickMiniEditVisuals(); // MINIEDIT_V1: sparse red cuboid edges, refreshed once per second

    // RESPAWN_INVULN_V1: уменьшаем ПОСЛЕ обработки урона, чтобы игрок получил
    // ровно 60 полных защищённых server ticks (3 секунды при 20 TPS), без off-by-one.
    for (auto& p : getAllPlayersCopy()) {
        if (p && p->respawnInvulnerabilityTicks > 0)
            --p->respawnInvulnerabilityTicks;
    }

    // TIMESYNC_V1: время суток тикает на сервере и рассылается раз в секунду —
    // иначе после смерти/респавна день и ночь у игроков расходятся
    if (tickCounter_ % 20 == 0) {
        // DAYNIGHT_LENGTH_V1: раньше день (0-12000) и ночь (12000-24000) шли с одной
        // скоростью (20 тиков/сек) — по 10 минут каждая. Теперь день длится 15 минут,
        // а ночь 5 минут: тиковый диапазон тот же (0..24000, день/ночь на тех же
        // границах, /time noon|night|midnight не трогали), просто скорость хода
        // разная по фазам. Копим дробный остаток в g_timeOfDayAccum, чтобы не терять
        // тики из-за округления.
        const bool isDayPhase = g_timeOfDay < 12000;
        const f64 ticksPerSecond = isDayPhase ? (12000.0 / 600.0) : (12000.0 / 600.0); // 15 мин / 5 мин
        g_timeOfDayAccum += ticksPerSecond;
        const i64 wholeTicks = static_cast<i64>(g_timeOfDayAccum);
        g_timeOfDayAccum -= static_cast<f64>(wholeTicks);
        g_timeOfDay = (g_timeOfDay + wholeTicks) % 24000;
        for (auto& p : getAllPlayersCopy())
            if (p && p->isAlive() && p->getState() == entity::PlayerState::Play) sendTimeUpdate(p);
    }
    _tp_time = _tp_lap(); // TICKPROF_V2

    if (tickCounter_ % 300 == 0) {
        tickKeepAlive();
    }
    _tp_keep = _tp_lap(); // TICKPROF_V2

    // WORLDSAVE_V1: автосохранение каждые 5 минут (6000 тиков)
    if (tickCounter_ % 6000 == 0) {
        // ASYNCSAVE_V1: раньше сериализация всех блоков + запись на диск шли в tick-потоке
        // (тик #6000 = 282мс). Теперь tick только снапшотит данные игроков в строку,
        // а сериализацию мира и запись делает фоновый поток.
        if (saveBusy_.load(std::memory_order_acquire)) {
            if (config_.language == "rus") NC_WARN("Server", "Автосохранение пропущено: пр��дыдущее ещё пишется");
            else NC_WARN("Server", "Auto-save skipped: previous one still running");
        } else {
            std::vector<std::pair<std::string, std::string>> pdata; // ник -> содержимое файла
            for (auto& p : getAllPlayersCopy())
                if (p && !p->getName().empty()) pdata.emplace_back(p->getName(), buildPlayerDataContent(p));
            if (saveThread_.joinable()) saveThread_.join(); // прошлый уже отработал — просто прибрать
            saveBusy_.store(true, std::memory_order_release);
            const bool ruLang = (config_.language == "rus");
            saveThread_ = std::thread([this, ruLang, pdata = std::move(pdata)]() mutable {
                const auto t0 = std::chrono::steady_clock::now();
                saveWorlds(); // DIMSAVE_V1
                std::error_code ec;
                std::filesystem::create_directories("world/playerdata", ec);
                for (auto& [name, content] : pdata) {
                    const std::string path = "world/playerdata/" + name + ".txt";
                    const std::string tmp = path + ".tmp";
                    std::ofstream pf(tmp, std::ios::trunc);
                    if (!pf) continue;
                    pf << content;
                    pf.close();
                    if (!pf.good()) { std::filesystem::remove(tmp, ec); continue; }
                    std::filesystem::rename(tmp, path, ec);
                    if (ec) std::filesystem::remove(tmp, ec);
                }
                const f64 saveMs = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (ruLang) NC_INFO("Server", "Автосохранение (фон): мир + игроки за {:.0f}мс, тики не тронуты", saveMs);
                else NC_INFO("Server", "Auto-save (background): world + players in {:.0f}ms, ticks untouched", saveMs);
                saveBusy_.store(false, std::memory_order_release);
            });
        }
    }
    _tp_save = _tp_lap(); // TICKPROF_V2

    // TICKPROF_V1/V2: итоговый разбор тика (фаза "прочее" теперь разбита на console/drain/tab/time).
    const f64 _tp_total = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - _tp_start).count();
    const f64 _tp_other = _tp_total - _tp_console - _tp_drain - _tp_boss - _tp_tab - _tp_time - _tp_keep - _tp_save;
    auto _tp_who = [&]() -> const char* {
        f64 m = _tp_other; const char* n = "прочее";
        if (_tp_console > m) { m = _tp_console; n = "console"; }
        if (_tp_drain   > m) { m = _tp_drain;   n = "drainChunks"; }
        if (_tp_boss    > m) { m = _tp_boss;    n = "tpsBossbar+sampleStats"; }
        if (_tp_tab     > m) { m = _tp_tab;     n = "tabList-broadcast"; }
        if (_tp_time    > m) { m = _tp_time;    n = "timeSync-broadcast"; }
        if (_tp_keep    > m) { m = _tp_keep;    n = "keepAlive"; }
        if (_tp_save    > m) { m = _tp_save;    n = "autoSave(диск)"; }
        return n;
    };
    if (_tp_total > 52.0) {
        NC_DEBUG("TickProf", "МЕДЛЕННЫЙ тик #{}: {:.1f}мс (>50 = просадка TPS) | виновник={} | boss={:.1f} keep={:.1f} save={:.1f} прочее={:.1f}",
                tickCounter_, _tp_total, _tp_who(), _tp_console, _tp_drain, _tp_boss, _tp_tab, _tp_time, _tp_keep, _tp_save, _tp_other);
    }
    // рекорд худшего тика за 5с ��� ловит даже редкие 0.4-TPS икоты в соло
    static f64 s_tp_wmax = 0.0; static const char* s_tp_wwho = "-"; static i32 s_tp_wtick = 0;
    if (_tp_total > s_tp_wmax) { s_tp_wmax = _tp_total; s_tp_wwho = _tp_who(); s_tp_wtick = tickCounter_; }
    if (tickCounter_ % 100 == 0) {
        NC_DEBUG("TickProf", "худший тик за 5с: {:.1f}мс (#{}, фаза={})", s_tp_wmax, s_tp_wtick, s_tp_wwho);
        s_tp_wmax = 0.0; s_tp_wwho = "-";
    }
}

void NetherCraftServer::sendTpsBossbar(const std::shared_ptr<entity::Player>& player, bool add) {
    if (!player || !player->isAlive() || player->getState() != entity::PlayerState::Play) return;
    const f32 progress = std::clamp(tps_ / 20.0f, 0.0f, 1.0f);
    const std::string title = tpsBossTitle(tps_, ramMb_, cpuPercent_);

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
        player->tpsBossbarColor = tpsBossColor(tps_); // BOSSCOLOR_V1
        return;
    }

    // BOSSCOLOR_V1: цвет раньше задавался только в ADD — при смене TPS бар перекрашивался
    // только после перезахода. Теперь при смене цвета шлём UPDATE_STYLE=4 автоматом.
    const i32 color = tpsBossColor(tps_);
    if (color != player->tpsBossbarColor) {
        net::Buffer style;
        style.writeUUID(TPS_BOSSBAR_ID);
        style.writeVarInt(4);     // ClientboundBossEventPacket.OperationType.UPDATE_STYLE
        style.writeVarInt(color); // BossBarColor
        style.writeVarInt(0);     // BossBarOverlay.PROGRESS
        player->getConnection()->sendPacket(0x0A, std::vector<u8>(style.writtenSpan().begin(), style.writtenSpan().end()));
        player->tpsBossbarColor = color;
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
    if (seconds < 0.5) return; // BOSSFAST_V1: было 1.0с — обновляем бар вдвое чаще, чтобы лаг был виден быстрее

    const f32 instant = std::clamp(static_cast<f32>(tpsSampleTicks_ / seconds), 0.0f, 20.0f);
    // TPS20_V2 / TPSCHAT_V1: раньше снапали к 20.00 уже при >=19.95 — из-за этого
    // мелкие просадки прятались и метр выглядел "фейковым". Теперь снапим только
    // при почти идеальных >=19.99 (чистим суб-мс джиттер спин-планировщика),
    // а любой реальный лаг показываем как есть.
    // BOSSFAST_V1: при лаге бар тормозил — EMA тянул вниз несколько секунд (показывал 19.85,
    // когда реально было 1.8). Теперь пр��садку показываем МГНОВЕННО, а подъём сглаживаем.
    if (instant >= 19.99f) tps_ = 20.0f;
    else if (instant < tps_) tps_ = instant;          // падение — сразу, без EMA-лага
    else tps_ = tps_ * 0.5f + instant * 0.5f;         // восстановление — плавно, чтобы бар не мигал
    tpsSampleTicks_ = 0;
    tpsSampleStart_ = now;
    sampleProcessStats(); // HUD_V1: refresh real RAM/CPU once per second, same cadence as TPS
    for (auto& player : getAllPlayersCopy()) {
        if (player && player->tpsBossbarEnabled) sendTpsBossbar(player, false);
    }

    // TPSCHAT_V1: когда TPS прос��дает до 15 или ниже — пишем ВСЕМ в чат текущий TPS,
    // чтобы лаги были видны без включённого босс-бара и было понятно, что метр не врёт.
    // Троттлинг 3 с, чтобы не заспамить чат (и не добавить лагов рассылкой) при долгой просадке.
    if (tps_ <= 15.0f) {
        if (lastLowTpsWarn_.time_since_epoch().count() == 0 ||
            std::chrono::duration<f64>(now - lastLowTpsWarn_).count() >= 3.0) {
            lastLowTpsWarn_ = now;
            const bool ru = (config_.language == "rus");
            // TPSCHAT_V1: без спецсимволов/эмодзи — к��иент 1.21.1 роняет system_chat с непечатным
            // символом (DecoderException / "Соединение потеряно"). Только текст + §-цвета.
            // TPSCHAT_V1 fix: u8"..." -> real UTF-8 bytes on the wire. Plain \u escapes were encoded
            // by MSVC in the exec charset (CP-1251) as a single byte -> client crashed decoding
            // system_chat (UTFDataFormatException: malformed input around byte 0). Same trick as u8s above.
            auto u8s = [](const char8_t* s) { return std::string(reinterpret_cast<const char*>(s)); };
            const std::string tpsStr = std::format("{:.2f}", tps_);
            const std::string msg = ru
                ? u8s(u8"§cСервер лагает! Текущий TPS: §e") + tpsStr + u8s(u8"§c из 20")
                : u8s(u8"§cServer is lagging! Current TPS: §e") + tpsStr + u8s(u8"§c of 20");
            // TPSCHAT_V1: через асинхронный chat-воркер (как обычный чат), чтобы
            // рассылка на сотни игроков не блокировала тик-поток синхронными send'ами.
            enqueueChatBroadcast(msg);
        }
    } else {
        lastLowTpsWarn_ = {}; // сброс: следующий уход <15 сразу даст первое сообщение
    }
}

// HUD_V1: self-measured RAM (working set) and CPU% (process time delta over wall
// time, normalized by core count the same way Task Manager's default view does),
// so the boss bar shows the server's own real resource usage, not a guess.
void NetherCraftServer::sampleProcessStats() {
#ifdef _WIN32
    const auto now = std::chrono::steady_clock::now();
    PROCESS_MEMORY_COUNTERS pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        ramMb_ = static_cast<f32>(pmc.WorkingSetSize) / (1024.0f * 1024.0f);
    }

    FILETIME creationTime{}, exitTime{}, kernelTime{}, userTime{};
    if (GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime)) {
        auto toU64 = [](const FILETIME& ft) -> u64 {
            return (static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        };
        const u64 cpuNow = toU64(kernelTime) + toU64(userTime);
        if (lastCpuSampleTime_.time_since_epoch().count() != 0) {
            const f64 wallSeconds = std::chrono::duration<f64>(now - lastCpuSampleTime_).count();
            if (wallSeconds > 0.05) {
                const f64 cpuSeconds = static_cast<f64>(cpuNow - lastCpuTotal100ns_) / 10'000'000.0;
                SYSTEM_INFO si{};
                GetSystemInfo(&si);
                const u32 cores = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
                cpuPercent_ = static_cast<f32>(std::clamp((cpuSeconds / wallSeconds / cores) * 100.0, 0.0, 100.0));
            }
        }
        lastCpuTotal100ns_ = cpuNow;
        lastCpuSampleTime_ = now;
    }
#endif
}

void NetherCraftServer::tickKeepAlive() {
    auto allPlayers = getAllPlayersCopy();
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    for (auto& p : allPlayers) {
        if (!p->isAlive()) continue;
        // KEEPALIVE_TIMEOUT_V1: клиент-призрак (не отвечает 30 с на keep-alive, но TCP сокет ещё открыт) —
        // рвём соединение принудительно, чтобы его инвентарь/буферы не висели в памяти
        if (p->awaitingKeepAlive && (nowMs - p->keepAliveSentAtMs) > 30000) {
            p->getConnection()->close();
            continue;
        }
        sendKeepAlive(p);
    }
}

} // namespace nc
