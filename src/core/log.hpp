#pragma once

#include <format>
#include <iostream>
#include <mutex>
#include <source_location>
#include <chrono>
#include <string>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <utility>
#include <cctype>

// CONUTF8_V1: консольный вывод через WriteConsoleW — одна строка = один вызов,
// иначе UTF-8 последовательность рвётся между вызовами WriteFile и кириллица превращается в "??".
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace nc::log {

enum class Level : u8 {
    Trace, Debug, Info, Warn, Error, Fatal
};

// LOGLEVEL_V1: default INFO so startup/join logs are visible; overridden by config log-level at startup.
inline std::atomic<Level> g_level{Level::Info};

inline void setLevel(Level lv) { g_level.store(lv, std::memory_order_relaxed); }

// LOGLEVEL_V1: parse a textual level (TRACE/DEBUG/INFO/WARN/ERROR/FATAL, any case).
inline void setLevelFromString(std::string_view s) {
    std::string u; u.reserve(s.size());
    for (char c : s) u += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    Level lv = Level::Info;
    if (u == "TRACE") lv = Level::Trace;
    else if (u == "DEBUG") lv = Level::Debug;
    else if (u == "INFO") lv = Level::Info;
    else if (u == "WARN" || u == "WARNING") lv = Level::Warn;
    else if (u == "ERROR") lv = Level::Error;
    else if (u == "FATAL") lv = Level::Fatal;
    setLevel(lv);
}

namespace detail {

inline std::mutex g_mutex;
inline std::ofstream g_file; // WORLDSAVE_V1: файл текущего лога
// RCONQUIET_V1: когда silent=true, перехваченный текст уходит ТОЛЬКО RCON-клиенту,
// не засоряя консоль и лог-файл опросами веб-панели каждую минуту.
inline thread_local bool g_captureSilent = false;
inline thread_local std::string* g_captureSink = nullptr; // RCON_BRIDGE_V1: перехват текста ответа для RCON

// ── ANSI цвета ──
constexpr const char* RESET   = "\033[0m";
constexpr const char* CYAN    = "\033[36m";    // время
constexpr const char* GREEN   = "\033[32m";    // INFO
constexpr const char* YELLOW  = "\033[33m";    // WARN
constexpr const char* RED     = "\033[31m";    // ERROR
constexpr const char* MAGENTA = "\033[35m";    // FATAL
constexpr const char* GRAY    = "\033[90m";    // DEBUG / TRACE
constexpr const char* WHITE   = "\033[37m";    // обычный текст
constexpr const char* BRIGHT  = "\033[1m";     // жирный

constexpr const char* levelTag(Level lv) {
    switch (lv) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
        case Level::Fatal: return "FATAL";
    }
    return "?????";
}

constexpr const char* levelColor(Level lv) {
    switch (lv) {
        case Level::Trace: return GRAY;
        case Level::Debug: return GRAY;
        case Level::Info:  return GREEN;
        case Level::Warn:  return YELLOW;
        case Level::Error: return RED;
        case Level::Fatal: return MAGENTA;
    }
    return WHITE;
}

// Реальное время (_wall clock_)
inline std::string wallTime() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &time);
#else
    localtime_r(&time, &tm_buf);
#endif

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

    return std::format("{}.{}",
        std::string(buf),
        std::format("{:03d}", ms.count()));
}

// CONUTF8_V1: печатаем целую строку за один вызов. На Windows — через WriteConsoleW
// (переводим UTF-8 -> UTF-16), чтобы консоль не ломала многобайтные символы.
// Если вывод перенаправлен в файл/pipe — честно пишем UTF-8 байты.
// GUIQUIET_V1: в gui-режиме единственный приёмник — окно-терминал, в conhost не пишем.
inline std::atomic<bool> g_consoleQuiet{false};

inline void writeConsole(const std::string& s) {
    if (g_consoleQuiet.load(std::memory_order_relaxed)) return; // GUIQUIET_V1
#ifdef _WIN32
    // NOCONHOST_V1: было `static HANDLE hOut = GetStdHandle(...)`, кэшировавшееся один раз навсегда.
    // С /SUBSYSTEM:WINDOWS консоль может появиться позже первого вызова (AllocConsole в main.cpp),
    // поэтому хендл теперь читаем заново каждый раз — иначе он навсегда оставался бы невалидным.
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr && GetConsoleMode(hOut, &mode)) {
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (wlen > 0) {
            std::wstring w(static_cast<size_t>(wlen), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), wlen);
            DWORD written = 0;
            WriteConsoleW(hOut, w.data(), static_cast<DWORD>(w.size()), &written, nullptr);
            return;
        }
    }
#endif
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

// GUICON_V1: приёмник строк для собственного окна-терминала. Обычный указатель на
// функцию, а не включение console/gui_console.hpp: логгер не должен знать о Win32.
// Никто не зарегистрировался — нулевая стоимость, классический режим не затронут.
using GuiSink = void (*)(int level, const char* tag, const char* text);
inline std::atomic<GuiSink> g_guiSink{nullptr};

// GUIBACKLOG_V1: окно-терминал поднимается уже после старта сервера, поэтому весь
// вывод копится здесь и проигрывается в окно при регистрации приёмника. Иначе
// баннер и лог запуска остаются только в classic-консоли.
struct BacklogLine { int level; std::string tag; std::string text; };
inline std::vector<BacklogLine> g_backlog;
// GUIBACKLOG_V2: буфер живёт только между стартом процесса и появлением окна
// (это доли секунды), поэтому потолок щедрый — обрезка на практике не наступает.
inline constexpr std::size_t kBacklogMax = 50000;
inline std::atomic<bool> g_backlogClosed{false}; // после реплея копить больше не нужно
inline std::atomic<bool> g_backlogEnabled{false}; // в classic-режиме не копим вообще

// Вызывать только под g_mutex.
inline void emitGui(int level, std::string_view tag, std::string text) {
    if (auto sink = g_guiSink.load(std::memory_order_relaxed))
        sink(level, std::string(tag).c_str(), text.c_str());
    if (!g_backlogEnabled.load(std::memory_order_relaxed)) return;
    if (g_backlogClosed.load(std::memory_order_relaxed)) return;
    if (g_backlog.size() >= kBacklogMax)
        g_backlog.erase(g_backlog.begin(), g_backlog.begin() + static_cast<std::ptrdiff_t>(kBacklogMax / 8));
    g_backlog.push_back(BacklogLine{level, std::string(tag), std::move(text)});
}

inline void doLog(Level lv, std::string_view tag, const std::string& msg)
{
    const char* color = levelColor(lv);
    (void)tag; // CONUTF8_V1: тег пока не выводится — глушим C4100 на /W4

    // RCON_BRIDGE_V1: если для текущего потока включён перехват (RCON-команда,
    // исполняемая на тик-потоке), копим текст сообщения — это станет ответом RCON-клиенту.
    if (g_captureSink) {
        if (!g_captureSink->empty()) g_captureSink->push_back('\n');
        g_captureSink->append(msg);
        // RCONQUIET_V1: в консоли должно оставаться только то, что ввёл человек.
        if (g_captureSilent) return;
    }

    std::lock_guard lock(g_mutex);

    // Формат как PMMP:
    // [HH:mm:ss.ms] [Server thread/LEVEL]: сообщение
    std::string line;
    line.reserve(96 + msg.size());
    line += CYAN;   line += "["; line += wallTime(); line += "]";
    line += RESET;  line += " ";
    line += color;  line += BRIGHT;
    line += "[Server thread/"; line += levelTag(lv); line += "]";
    line += RESET;  line += " ";
    line += WHITE;  line += msg; line += RESET;
    line += "\n";
    writeConsole(line); // CONUTF8_V1

    // GUICON_V1: в окно отдаём чистый текст без ANSI — цвет там свой, по уровню.
    emitGui(static_cast<int>(lv), tag,
            "[" + wallTime() + "] [Server thread/" + levelTag(lv) + "] " + msg); // GUIBACKLOG_V1

    // WORLDSAVE_V1: Дублируем в файл (без ANSI-цветов)
    if (g_file.is_open()) {
        g_file << "[" << wallTime() << "] [Server thread/" << levelTag(lv) << "] " << msg << "\n";
        g_file.flush();
    }
}

} // namespace detail

// RCON_BRIDGE_V1: включить/выключить перехват вывода NC_INFO/NC_WARN/NC_ERROR для
// текущего (тик-)потока — используется, чтобы вернуть текст ответа RCON-клиенту.
inline void beginCapture(std::string* sink, bool silent = false) {
    detail::g_captureSink = sink;
    detail::g_captureSilent = silent; // RCONQUIET_V1
}
inline void endCapture() {
    detail::g_captureSink = nullptr;
    detail::g_captureSilent = false;
}

// CONUTF8_V1: сырая печать в консоль одним вызовом (без таймштампа и без записи в файл).
inline void console(const std::string& s) { detail::writeConsole(s); }

// LOGNAME_V1 (бывш. WORLDSAVE_V1): логи в файл с датой в имени, храним 15 последних.
// rus: logs/log-20.07.26.log (ДД.ММ.ГГ), eng: logs/log-07.20.26.log (ММ.ДД.ГГ, как в США).
// "/" в имени файла Windows запрещён, поэтому разделитель — точки.
// Повторный запуск в тот же день: log-20.07.26-2.log, -3.log и т.д.
// LOGBANNER_V1: сырые строки (баннер, мастер установки) дублируются в файл лога
// без префиксов времени/уровня. До открытия файла строки копятся в буфере
// и сбрасываются при initFileLog.
inline std::vector<std::string>& pendingRawLines() {
    static std::vector<std::string> v;
    return v;
}
// GUICON_V1: регистрация окна-терминала как второго приёмника лога. nullptr = отключить.
// GUIBACKLOG_V1: при подключении сразу проигрываем всё, что было напечатано до старта окна.
// GUIBACKLOG_V2: включается в самом начале main, если console-mode=gui. В classic
// режиме буфер не заводится вовсе — ноль лишней памяти.
inline void enableBacklog(bool on) {
    std::lock_guard<std::mutex> lock(detail::g_mutex);
    detail::g_backlogEnabled.store(on, std::memory_order_relaxed);
    if (!on) { detail::g_backlog.clear(); detail::g_backlog.shrink_to_fit(); }
}

inline void setGuiSink(detail::GuiSink sink) {
    std::lock_guard<std::mutex> lock(detail::g_mutex);
    detail::g_guiSink.store(sink, std::memory_order_relaxed);
    if (!sink) return;
    for (const auto& l : detail::g_backlog) sink(l.level, l.tag.c_str(), l.text.c_str());
    detail::g_backlogClosed.store(true, std::memory_order_relaxed);
    detail::g_backlog.clear();
    detail::g_backlog.shrink_to_fit();
}

// GUIBACKLOG_V1: сырая строка только для окна/бэклога (в consol'ь её печатает вызывающий,
// например баннер через std::cout с ANSI-цветами).
inline void guiLine(const std::string& s, int level = 2) { // GUIQUIET_V1: level задаёт цвет в окне
    std::lock_guard<std::mutex> lock(detail::g_mutex);
    detail::emitGui(level, "", s);
}

// GUIQUIET_V1: заглушить дублирующий вывод в системную консоль (gui-режим).
inline void setConsoleQuiet(bool on) {
    detail::g_consoleQuiet.store(on, std::memory_order_relaxed);
}

// GUIWIZARD_V1: мост между мастером первого запуска (core/config.hpp) и окном-терминалом
// (console/gui_console.cpp), без прямого include между ними: обычный указатель на функцию,
// как и GuiSink выше. nullptr = читать из std::cin, как раньше.
namespace detail {
using GuiReadLine = std::string (*)();
inline std::atomic<GuiReadLine> g_guiReadLine{nullptr};
} // namespace detail

inline void setGuiReadLine(detail::GuiReadLine fn) {
    detail::g_guiReadLine.store(fn, std::memory_order_relaxed);
}
inline bool hasGuiReadLine() {
    return detail::g_guiReadLine.load(std::memory_order_relaxed) != nullptr;
}
inline std::string guiReadLine() {
    auto fn = detail::g_guiReadLine.load(std::memory_order_relaxed);
    return fn ? fn() : std::string();
}

inline void rawLine(const std::string& s) {
    std::lock_guard<std::mutex> lock(detail::g_mutex);
    detail::emitGui(2, "", s); // GUICON_V1 + GUIBACKLOG_V1
    if (detail::g_file.is_open()) {
        detail::g_file << s << "\n";
        detail::g_file.flush();
    } else {
        pendingRawLines().push_back(s);
    }
}

// CHATLOG_V1: игровой чат пишем только в консоль, НЕ в файл лога
// (чтобы переписка игроков не засоряла logs/*.log).
inline void chatLine(const std::string& s) {
    std::lock_guard<std::mutex> lock(detail::g_mutex);
    detail::emitGui(7, "CHAT", "[" + detail::wallTime() + "] [Server thread/CHAT] " + s); // GUICON_V1: уровень 7 = Chat
    // CONUTF8_V1
    detail::writeConsole("[" + detail::wallTime() + "] [Server thread/CHAT] " + s + "\n");
}

// CMDLOG_V2: команды (/op и прочие) нигде не попадали в файл: игровые глушились
// нарочно (CMDLOG_QUIET_V1), а консольные просто никто не писал. Только файл:
// в окне/терминале эхо и так есть, дублировать его незачем.
inline void commandLine(const std::string& who, const std::string& cmd) {
    std::lock_guard<std::mutex> lock(detail::g_mutex);
    const std::string line = "[" + detail::wallTime() + "] [Server thread/CMD] <" + who + "> " + cmd;
    if (detail::g_file.is_open()) {
        detail::g_file << line << "\n";
        detail::g_file.flush();
    } else {
        pendingRawLines().push_back(line);
    }
}

// CMDLOG_V1: console/window commands were echoed straight into the window buffer,
// bypassing the logger entirely - so nothing a human typed ever reached logs/*.log.
// File only: the window already prints the echo itself.
inline void fileLine(const std::string& s) {
    std::lock_guard<std::mutex> lock(detail::g_mutex);
    if (detail::g_file.is_open()) {
        detail::g_file << s << "\n";
        detail::g_file.flush();
    } else {
        pendingRawLines().push_back(s);
    }
}

inline void initFileLog(const std::string& language = "rus") {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories("logs", ec);
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char datebuf[16];
    if (language == "eng") {
        std::snprintf(datebuf, sizeof(datebuf), "%02d.%02d.%02d",
                      tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_year % 100);
    } else {
        std::snprintf(datebuf, sizeof(datebuf), "%02d.%02d.%02d",
                      tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year % 100);
    }
    const std::string base = std::string("logs/log-") + datebuf;
    std::string name = base + ".log";
    for (int i = 2; fs::exists(name) && i < 1000; ++i) {
        name = base + "-" + std::to_string(i) + ".log";
    }
    // Храним максимум 15 логов: удаляем самые старые по времени изменения
    std::vector<std::pair<fs::file_time_type, fs::path>> oldLogs;
    for (const auto& e : fs::directory_iterator("logs", ec)) {
        if (!e.is_regular_file()) continue;
        const auto fname = e.path().filename().string();
        if (fname.rfind("log", 0) == 0 && e.path().extension() == ".log") {
            oldLogs.emplace_back(fs::last_write_time(e.path(), ec), e.path());
        }
    }
    std::sort(oldLogs.begin(), oldLogs.end());
    while (oldLogs.size() >= 15) { // +1 новый = ровно 15
        fs::remove(oldLogs.front().second, ec);
        oldLogs.erase(oldLogs.begin());
    }
    detail::g_file.open(name, std::ios::out | std::ios::trunc);
    { // LOGBANNER_V1: сброс накопленных сырых строк (баннер/мастер установки)
        std::lock_guard<std::mutex> lock(detail::g_mutex);
        if (detail::g_file.is_open()) {
            for (const auto& s : pendingRawLines()) detail::g_file << s << "\n";
            detail::g_file.flush();
        }
        pendingRawLines().clear();
    }
}

template<typename... Args>
void log(Level lv, std::string_view tag,
         std::format_string<Args...> fmt, Args&&... args)
{
    if (lv < g_level.load(std::memory_order_relaxed)) return;
    auto msg = std::format(fmt, std::forward<Args>(args)...);
    detail::doLog(lv, tag, msg);
}

inline void log(Level lv, std::string_view tag, std::string msg) {
    if (lv < g_level.load(std::memory_order_relaxed)) return;
    detail::doLog(lv, tag, msg);
}

} // namespace nc::log

// ── Макросы ──
#define NC_TRACE(tag, ...) ::nc::log::log(::nc::log::Level::Trace, tag, __VA_ARGS__)
#define NC_DEBUG(tag, ...) ::nc::log::log(::nc::log::Level::Debug, tag, __VA_ARGS__)
#define NC_INFO(tag, ...)  ::nc::log::log(::nc::log::Level::Info,  tag, __VA_ARGS__)
#define NC_WARN(tag, ...)  ::nc::log::log(::nc::log::Level::Warn,  tag, __VA_ARGS__)
#define NC_ERROR(tag, ...) ::nc::log::log(::nc::log::Level::Error, tag, __VA_ARGS__)
#define NC_FATAL(tag, ...) ::nc::log::log(::nc::log::Level::Fatal, tag, __VA_ARGS__)
