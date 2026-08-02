#pragma once
// SPAWNCFG_V1: настройка команды /spawn через папку spawn (по образцу tab_Server).
// Позволяет выключить команду целиком, включить обратный отсчёт перед телепортом,
// требовать стоять на месте, задать свой цвет и текст, либо жёстко выбрать язык.
#include "tab.gen.hpp" // SPAWNCFG_V1: переиспользуем trim/bool/CP1251->UTF-8 из tab_Server

#include <filesystem>
#include <fstream>
#include <string>

namespace nc::spawncfg {

struct SpawnConfig {
    bool disable = false;        // true = команда /spawn выключена
    int warmup = -1;             // секунд ожидания перед телепортом; -1 или 0 = мгновенно
    bool standStill = true;      // сдвинулся во время отсчёта = телепорт отменён
    std::string color = "\u00a7e";    // цвет строк отсчёта (§e = жёлтый)
    std::string lang = "auto";   // auto = по языку клиента, ru / en = принудительно
    std::string textCountdown;   // своя строка отсчёта, {} = секунды
    std::string textDone;        // своя строка после телепорта
    std::string textCancelled;   // своя строка при отмене
};

// SPAWNCFG_V1: создаёт папку spawn с spawn.properties при первом запуске и читает её.
// Файл перечитывается на каждый /spawn — правки применяются без перезапуска сервера.
inline SpawnConfig loadSpawnConfig(bool ru) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories("spawn", ec);

    // CFGDOC_V1: инструкция рядом с конфигом — на языке сервера
    nc::tab::writeConfigDoc("spawn", ru, ru
        ? std::string(
            "ПАПКА spawn — настройка команды /spawn\r\n"
            "====================================================\r\n"
            "\r\n"
            "Всё настраивается в файле spawn.properties рядом с этой инструкцией.\r\n"
            "Файл читается при каждом вызове /spawn — перезапуск сервера не нужен.\r\n"
            "Сохраняй его в кодировке UTF-8 (в Блокноте: Сохранить как -> UTF-8),\r\n"
            "иначе русские буквы в твоих строках превратятся в крякозябры.\r\n"
            "Удалишь этот файл или spawn.properties — сервер создаст их заново при старте.\r\n"
            "\r\n"
            "КОМАНДЫ\r\n"
            "----------------------------------------------------\r\n"
            "/spawn      — телепорт на мировой спавн. Работает из любого измерения:\r\n"
            "              из Ада или Энда сначала переносит в обычный мир.\r\n"
            "/setspawn   — поставить спавн (только оператор). Без аргументов — туда, где стоишь,\r\n"
            "              или явно: /setspawn 100 70 -30\r\n"
            "              Полное имя команды — /setworldspawn, работают оба.\r\n"
            "\r\n"
            "НАСТРОЙКИ spawn.properties\r\n"
            "----------------------------------------------------\r\n"
            "disable         true  — команда /spawn выключена для всех.\r\n"
            "                       /setspawn у оператора при этом продолжает работать.\r\n"
            "                false — команда доступна (так по умолчанию).\r\n"
            "\r\n"
            "warmup          Сколько секунд ждать перед телепортом.\r\n"
            "                -1 или 0 — отсчёта нет, перенос мгновенный (по умолчанию).\r\n"
            "                Например 5 — в чат каждую секунду идёт отсчёт 5, 4, 3, 2, 1.\r\n"
            "                Повторный /spawn во время отсчёта второй таймер не запускает.\r\n"
            "                Максимум 3600 (час) — защита от опечатки вроде warmup=999999.\r\n"
            "\r\n"
            "stand-still     true  — если игрок сдвинется во время отсчёта, телепорт отменяется.\r\n"
            "                        Допуск — около трети блока, чтобы дёрганье мыши не считалось.\r\n"
            "                false — можно бегать и драться, телепорт всё равно сработает.\r\n"
            "                Работает только когда warmup больше нуля.\r\n"
            "\r\n"
            "color           Цвет строк отсчёта и своих текстов. Коды майнкрафта:\r\n"
            "                §0 чёрный    §1 тёмно-синий  §2 тёмно-зелёный  §3 бирюзовый\r\n"
            "                §4 тёмно-красный  §5 фиолетовый  §6 золотой  §7 серый\r\n"
            "                §8 тёмно-серый  §9 синий  §a зелёный  §b голубой\r\n"
            "                §c красный  §d розовый  §e жёлтый  §f белый\r\n"
            "                Можно добавить стиль: §l жирный, §o курсив, §n подчёркнутый.\r\n"
            "                Например: color=§b§l\r\n"
            "                Символ § набирается копированием отсюда или Alt+0167.\r\n"
            "\r\n"
            "lang            auto — каждый игрок видит сообщения на языке своего клиента.\r\n"
            "                ru   — всем по-русски, независимо от клиента.\r\n"
            "                en   — всем по-английски.\r\n"
            "\r\n"
            "text-countdown  Своя строка отсчёта. Пусто — встроенный перевод.\r\n"
            "                {} заменяется на оставшиеся секунды ({sec} и {seconds} тоже работают).\r\n"
            "text-done       Своя строка после успешного телепорта.\r\n"
            "text-cancelled  Своя строка при отмене из-за движения.\r\n"
            "                Свои тексты одинаковы для всех языков — перевода у них нет.\r\n"
            "\r\n"
            "ПРИМЕР 1 — строгий выживаловский спавн\r\n"
            "----------------------------------------------------\r\n"
            "disable=false\r\n"
            "warmup=10\r\n"
            "stand-still=true\r\n"
            "color=§c\r\n"
            "lang=auto\r\n"
            "\r\n"
            "ПРИМЕР 2 — быстрый спавн со своим текстом\r\n"
            "----------------------------------------------------\r\n"
            "warmup=3\r\n"
            "stand-still=false\r\n"
            "color=§d\r\n"
            "text-countdown=Собираю тебя по кусочкам... {}\r\n"
            "text-done=Готово, ты дома!\r\n"
            "\r\n"
            "ПРИМЕР 3 — выключить команду совсем\r\n"
            "----------------------------------------------------\r\n"
            "disable=true\r\n")
        : std::string(
            "FOLDER spawn — /spawn command settings\r\n"
            "====================================================\r\n"
            "\r\n"
            "Everything is configured in spawn.properties next to this file.\r\n"
            "It is re-read on every /spawn call — no server restart needed.\r\n"
            "Save it as UTF-8 (Notepad: Save As -> UTF-8), otherwise non-ASCII\r\n"
            "characters in your own lines will show up garbled.\r\n"
            "Delete this file or spawn.properties and the server recreates them on start.\r\n"
            "\r\n"
            "COMMANDS\r\n"
            "----------------------------------------------------\r\n"
            "/spawn      — teleport to the world spawn. Works from any dimension:\r\n"
            "              from the Nether or the End you are moved to the overworld first.\r\n"
            "/setspawn   — set the spawn (operator only). No arguments — where you stand,\r\n"
            "              or explicitly: /setspawn 100 70 -30\r\n"
            "              The full name is /setworldspawn, both work.\r\n"
            "\r\n"
            "spawn.properties KEYS\r\n"
            "----------------------------------------------------\r\n"
            "disable         true  — /spawn is disabled for everyone.\r\n"
            "                       /setspawn still works for operators.\r\n"
            "                false — command is available (default).\r\n"
            "\r\n"
            "warmup          Seconds to wait before the teleport.\r\n"
            "                -1 or 0 — no countdown, instant teleport (default).\r\n"
            "                For example 5 — chat counts down 5, 4, 3, 2, 1.\r\n"
            "                Calling /spawn again during the countdown starts no second timer.\r\n"
            "                Capped at 3600 (one hour) to survive typos like warmup=999999.\r\n"
            "\r\n"
            "stand-still     true  — moving during the countdown cancels the teleport.\r\n"
            "                        Tolerance is about a third of a block, so mouse jitter is fine.\r\n"
            "                false — run and fight freely, the teleport still happens.\r\n"
            "                Only matters when warmup is above zero.\r\n"
            "\r\n"
            "color           Colour of the countdown and of your own texts. Minecraft codes:\r\n"
            "                §0 black     §1 dark blue   §2 dark green   §3 dark aqua\r\n"
            "                §4 dark red  §5 purple      §6 gold        §7 gray\r\n"
            "                §8 dark gray §9 blue        §a green       §b aqua\r\n"
            "                §c red       §d pink        §e yellow      §f white\r\n"
            "                You may append a style: §l bold, §o italic, §n underline.\r\n"
            "                For example: color=§b§l\r\n"
            "                Type the § sign by copying it from here or with Alt+0167.\r\n"
            "\r\n"
            "lang            auto — every player sees the messages in their client language.\r\n"
            "                ru   — always Russian, whatever the client uses.\r\n"
            "                en   — always English.\r\n"
            "\r\n"
            "text-countdown  Your own countdown line. Empty — use the built-in translation.\r\n"
            "                {} is replaced with the remaining seconds ({sec} and {seconds} work too).\r\n"
            "text-done       Your own line after a successful teleport.\r\n"
            "text-cancelled  Your own line when the teleport is cancelled by movement.\r\n"
            "                Your own texts are the same for every language — they are not translated.\r\n"
            "\r\n"
            "EXAMPLE 1 — strict survival spawn\r\n"
            "----------------------------------------------------\r\n"
            "disable=false\r\n"
            "warmup=10\r\n"
            "stand-still=true\r\n"
            "color=§c\r\n"
            "lang=auto\r\n"
            "\r\n"
            "EXAMPLE 2 — quick spawn with custom text\r\n"
            "----------------------------------------------------\r\n"
            "warmup=3\r\n"
            "stand-still=false\r\n"
            "color=§d\r\n"
            "text-countdown=Reassembling you... {}\r\n"
            "text-done=Done, welcome home!\r\n"
            "\r\n"
            "EXAMPLE 3 — turn the command off completely\r\n"
            "----------------------------------------------------\r\n"
            "disable=true\r\n"));

    const char* cfgPath = "spawn/spawn.properties";
    if (!fs::exists(cfgPath, ec)) {
        std::ofstream out(cfgPath, std::ios::binary);
        if (out.is_open()) {
            if (ru) {
                out <<
                    "# Настройка команды /spawn (телепорт на мировой спавн)\n"
                    "# ВАЖНО: сохраняйте этот файл в кодировке UTF-8 (в Блокноте — 'Сохранить как' -> UTF-8),\n"
                    "# иначе кириллица в своих строках может отобразиться крякозябрами.\n"
                    "# ==========================================\n"
                    "\n"
                    "# disable=true - полностью отключить команду /spawn\n"
                    "# (/setspawn для оператора продолжает работать)\n"
                    "disable=false\n"
                    "\n"
                    "# Сколько секунд ждать перед телепортом (обратный отсчёт в чате).\n"
                    "# -1 или 0 - отсчёта нет, телепорт мгновенный.\n"
                    "warmup=-1\n"
                    "\n"
                    "# stand-still=true - если игрок сдвинется во время отсчёта, телепорт отменяется.\n"
                    "# stand-still=false - можно спокойно бегать, телепорт всё равно сработает.\n"
                    "stand-still=true\n"
                    "\n"
                    "# Цвет строк отсчёта: §a зелёный, §e жёлтый, §c красный, §b голубой, §d розовый,\n"
                    "# §6 золотой, §7 серый, §f белый\n"
                    "color=§e\n"
                    "\n"
                    "# Язык сообщений: auto - по языку клиента, ru или en - принудительно\n"
                    "lang=auto\n"
                    "\n"
                    "# Свои тексты. Пустая строка - взять встроенный перевод.\n"
                    "# В строке отсчёта {} заменяется на оставшиеся секунды.\n"
                    "text-countdown=\n"
                    "text-done=\n"
                    "text-cancelled=\n";
            } else {
                out <<
                    "# /spawn command settings (teleport to world spawn)\n"
                    "# IMPORTANT: save this file as UTF-8 (in Notepad: 'Save As' -> UTF-8),\n"
                    "# otherwise non-ASCII characters in your lines may show up garbled.\n"
                    "# ==========================================\n"
                    "\n"
                    "# disable=true - fully disable the /spawn command\n"
                    "# (/setspawn still works for operators)\n"
                    "disable=false\n"
                    "\n"
                    "# Seconds to wait before teleporting (countdown in chat).\n"
                    "# -1 or 0 - no countdown, teleport is instant.\n"
                    "warmup=-1\n"
                    "\n"
                    "# stand-still=true - moving during the countdown cancels the teleport.\n"
                    "# stand-still=false - you may walk around, the teleport still happens.\n"
                    "stand-still=true\n"
                    "\n"
                    "# Countdown colour: §a green, §e yellow, §c red, §b aqua, §d pink,\n"
                    "# §6 gold, §7 gray, §f white\n"
                    "color=§e\n"
                    "\n"
                    "# Message language: auto - follow the client, ru or en - force it\n"
                    "lang=auto\n"
                    "\n"
                    "# Your own texts. Empty line - use the built-in translation.\n"
                    "# In the countdown line {} is replaced with the remaining seconds.\n"
                    "text-countdown=\n"
                    "text-done=\n"
                    "text-cancelled=\n";
            }
        }
    }

    SpawnConfig cfg;
    std::ifstream in(cfgPath, std::ios::binary);
    std::string line;
    if (in.is_open()) {
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string t = nc::tab::tabTrim(line);
            if (t.empty() || t[0] == '#') continue;
            const auto eq = t.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = nc::tab::tabTrim(t.substr(0, eq));
            const std::string val = nc::tab::tabTrim(t.substr(eq + 1));
            if (key == "disable") cfg.disable = nc::tab::tabParseBool(val, false);
            else if (key == "warmup") { try { cfg.warmup = std::stoi(val); } catch (...) { cfg.warmup = -1; } }
            else if (key == "stand-still") cfg.standStill = nc::tab::tabParseBool(val, true);
            else if (key == "color") { if (!val.empty()) cfg.color = nc::tab::tabToUtf8(val); }
            else if (key == "lang") { if (!val.empty()) cfg.lang = val; }
            else if (key == "text-countdown") cfg.textCountdown = nc::tab::tabToUtf8(val);
            else if (key == "text-done") cfg.textDone = nc::tab::tabToUtf8(val);
            else if (key == "text-cancelled") cfg.textCancelled = nc::tab::tabToUtf8(val);
        }
    }
    if (cfg.warmup > 3600) cfg.warmup = 3600; // SPAWNCFG_V1: защита от опечатки вроде warmup=999999
    return cfg;
}

// SPAWNCFG_V1: подстановка секунд в пользовательский текст ({} или {sec}).
inline std::string spawnFillSeconds(std::string text, int seconds) {
    const std::string num = std::to_string(seconds);
    for (const char* token : {"{}", "{sec}", "{seconds}"}) {
        const std::string tok(token);
        std::string::size_type pos = 0;
        while ((pos = text.find(tok, pos)) != std::string::npos) {
            text.replace(pos, tok.size(), num);
            pos += num.size();
        }
    }
    return text;
}

} // namespace nc::spawncfg
