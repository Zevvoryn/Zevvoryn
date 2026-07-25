#include "core/server.hpp"
#include "core/config.hpp"
#include "core/log.hpp"
#include "core/crash_context.hpp" // CRASHCTX_V1
#include <iostream>
#include <csignal>
#include <thread>
#include <string>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
    #include <windows.h>
    #include <conio.h> // CRASHRESTART_V1: _kbhit/_getch для «ENTER = рестарт сразу»
#endif

static nc::NetherCraftServer* g_server = nullptr;

// CRASHGUARD_V1: при фатальной ошибке сервер не закрывает окно молча.
// Пишем ошибку красным в консоль, дублируем в основной лог и в logs/crash-*.txt,
// даём 180 секунд на чтение и только потом закрываемся.
static void crashNote(const std::string& what) {
    static std::atomic<bool> g_crashOnce{false};
    if (g_crashOnce.exchange(true)) { // повторный сбой внутри обработчика — просто ждём и выходим
        std::this_thread::sleep_for(std::chrono::seconds(200));
        std::_Exit(3);
    }

    // CRASHNET_V1: ПЕРВЫМ делом уводим сервер оффлайн. Раньше crashNote() просто
    // печатал отчёт и висел 180 секунд, а сеть всё это время оставалась живой —
    // accept-луп принимал новых игроков, и на «упавший» сервер можно было спокойно
    // зайти и играть. Теперь сразу закрываем listen-сокет и рвём все соединения —
    // ещё ДО печати отчёта и окна ожидания.
    if (g_server) {
        g_server->getNetwork().crashShutdown();
    }

    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char timebuf[32];
    std::snprintf(timebuf, sizeof(timebuf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    // CRASHCTX_V1: pull whatever this crashing thread was last doing (command,
    // player, core/plugin source) so the report says *why*, not just *that*.
    std::string ctxAction, ctxPlayer, ctxSource;
    bool hasCtx = nc::describeCrashContext(ctxAction, ctxPlayer, ctxSource);

    // CRASHREPORT_V2: richer, structured report format (banner + Time/Description
    // + Reason + Caused by), per owner's requested layout.
    std::string report;
    report += "---- Zevvoryn Crash Report ----\n\n";
    report += std::string("Time: ") + timebuf + "\n";
    report += "Description: Critical internal server error\n\n";
    report += "Reason:\n" + what + "\n\n";
    report += "Caused by:\n";
    if (hasCtx) {
        report += "  Command: " + ctxAction + "\n";
        report += "  Player: " + (ctxPlayer.empty() ? std::string("-") : ctxPlayer) + "\n";
        report += "  Source: " + ctxSource + "\n";
    } else {
        report += "  (no active command on this thread — likely core engine / background thread)\n";
        report += "  Source: core\n";
    }
    report += "\nThe server terminated immediately because of this error.\n";
    report += "No world data was saved.\n";

    // CRASHDUP_V1: print the full report to the console exactly once (raw, so it
    // keeps its own color/spacing) and mirror it into the MAIN log file exactly
    // once via rawLine (no timestamp/level prefix, no second console echo).
    // A short NC_ERROR line (not the whole report) still marks the moment in the
    // main log for quick scanning, without duplicating the whole block.
    std::cout << "\n\033[31m\033[1m" << report << "\033[0m" << std::endl;
    nc::log::rawLine(report);
    NC_ERROR("Crash", "See full crash report above / in logs/crash-last.txt");
    std::string crashPath = "logs/crash-last.txt";
    try {
        std::filesystem::create_directories("logs");
        char namebuf[48];
        std::snprintf(namebuf, sizeof(namebuf), "logs/crash-%02d.%02d.%02d-%02d%02d%02d.txt",
                      tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year % 100,
                      tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        crashPath = namebuf;
        std::ofstream cf(crashPath, std::ios::trunc);
        cf << report;
    } catch (...) {}
    std::cout << "\033[33m  Ошибка сохранена в " << crashPath << " и в текущий лог.\033[0m\n";
    // CRASHRESTART_V1: ENTER = перезапуск сразу, иначе авто-перезапуск через 180с.
    // Упавший процесс лечить нельзя — запускаем СВЕЖИЙ процесс в новом окне.
    std::cout << "\033[33m  Нажми ENTER — сервер перезапустится СРАЗУ.\033[0m\n";
    std::cout << "\033[33m  Иначе авто-перезапуск через 180 секунд...\033[0m\n";
#ifdef _WIN32
    bool restartNow = false;
    for (int waited = 0; waited < 180000; waited += 100) {
        while (_kbhit()) {
            int ch = _getch();
            if (ch == '\r' || ch == '\n') { restartNow = true; break; }
        }
        if (restartNow) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (waited > 0 && waited % 30000 == 0)
            std::cout << "\033[90m  ...перезапуск через " << (180 - waited / 1000) << " с (или ENTER — сразу)\033[0m\n";
    }
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(exePath, nullptr, nullptr, nullptr, FALSE,
                           CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            std::cout << "\033[32m  Новый процесс сервера запущен в новом окне. Это окно закроется.\033[0m\n";
        } else {
            std::cout << "\033[31m  Не удалось перезапустить автоматически — запусти zevvoryn.exe вручную.\033[0m\n";
        }
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
#else
    std::this_thread::sleep_for(std::chrono::seconds(180));
#endif
}

[[noreturn]] static void terminateHandler() {
    std::string msg = "необработанное исключение (std::terminate)";
    if (auto ex = std::current_exception()) {
        try { std::rethrow_exception(ex); }
        catch (const std::exception& e) { msg = std::string("необработанное исключение: ") + e.what(); }
        catch (...) { msg = "необработанное исключение неизвестного типа"; }
    }
    crashNote(msg);
    std::_Exit(3);
}

#ifdef _WIN32
static LONG WINAPI sehCrashHandler(EXCEPTION_POINTERS* info) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "критический сбой SEH 0x%08lX по адресу %p",
                  (unsigned long)info->ExceptionRecord->ExceptionCode,
                  info->ExceptionRecord->ExceptionAddress);
    crashNote(buf);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static void abortHandler(int) {
    crashNote("вызван abort() — критическая внутренняя ошибка");
    std::_Exit(3);
}

// CRASHGUARD_V2: страховка через сигналы — ловим фатальные сбои,
// даже если SEH-фильтр по какой-то причине не сработал.
static void fatalSignalHandler(int sig) {
    const char* what = sig == SIGSEGV ? "обращение к недоступной памяти (SIGSEGV)"
                     : sig == SIGILL  ? "недопустимая инструкция (SIGILL)"
                     : sig == SIGFPE  ? "ошибка арифметики/деление на ноль (SIGFPE)"
                     : "фатальный сигнал";
    crashNote(what);
    std::_Exit(3);
}


#ifdef _WIN32
// CONCLOSE_V1: перехват крестика консоли (CTRL_CLOSE_EVENT), выхода из системы и shutdown.
// Windows даёт ~5 секунд до принудительного убийства процесса — успеваем сохранить
// мир и игроков вместо мгновенной потери всего несохранённого.
static BOOL WINAPI consoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_LOGOFF_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        std::cout << "\n\033[31m\033[1mВсе твои данные… будут потеряны если ты будешь меня закрывать принудительно\033[0m\n";
        std::cout << "\033[33m...ладно, шучу. Экстренно сохраняю мир и игроков...\033[0m\n" << std::flush;
        if (g_server) g_server->stop(); // сохранит мир + игроков и погасит сеть
        std::cout << "\033[32mГотово, всё сохранено. Но лучше останавливай командой stop ;)\033[0m\n" << std::flush;
        return TRUE; // мы всё сделали — процесс может закрываться
    }
    return FALSE; // Ctrl+C / Ctrl+Break обрабатываются signalHandler'ом как раньше
}
#endif

void signalHandler(int signal) {
    if (g_server) {
        NC_INFO("Main", "Received signal {}, stopping...", signal);
        g_server->stop();
    }
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Принудительно ставим UTF-8 для консоли
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // Включаем ANSI escape codes (цвета)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)) {
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hOut, mode);
    }
#endif

    // CRASHGUARD_V1: перехват фатальных ошибок (исключения, SEH, abort)
    std::set_terminate(terminateHandler);
    std::signal(SIGABRT, abortHandler);
    std::signal(SIGSEGV, fatalSignalHandler); // CRASHGUARD_V2
    std::signal(SIGILL, fatalSignalHandler);  // CRASHGUARD_V2
    std::signal(SIGFPE, fatalSignalHandler);  // CRASHGUARD_V2
#ifdef _WIN32
    SetUnhandledExceptionFilter(sehCrashHandler);
#ifdef _MSC_VER
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // CRASHGUARD_V2: без окна WER — сразу наш обработчик
#endif
#endif

    std::string configPath = "settings.properties";
    if (argc > 1) {
        configPath = argv[1];
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    nc::NetherCraftServer server;
    g_server = &server;
#ifdef _WIN32
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE); // CONCLOSE_V1: крестик = сохранить, потом закрыться
#endif
    const bool started = nc::setup::needsSetup(configPath)
        ? server.startWithConfig(nc::setup::runWizard())
        : server.start(configPath);
    if (!started) return 1;

    // CONSOLE_V2: server simulation owns world state; console lines are queued for its tick.
    std::thread serverThread([&server] { server.run(); });
    // CONSTOP_V1: раньше main блокировался в getline и после /stop из игры окно
    // «висело». Теперь консоль читает detached-поток, а main ждёт серверный
    // поток и закрывает процесс сразу при любом способе остановки.
    std::thread consoleThread([&server] {
        std::cout << "Console ready. Type help for commands.\n> " << std::flush;
        std::string line;
        bool sentStop = false; // STOPLOG_V1: не дублируем stop в очереди — было два «Stopping server...»
        while (server.isRunning() && std::getline(std::cin, line)) {
            server.queueConsoleCommand(line);
            if (line == "stop" || line == "/stop") { sentStop = true; break; }
            if (!server.isRunning()) break;
            std::cout << "> " << std::flush;
        }
        if (!sentStop && server.isRunning()) server.queueConsoleCommand("stop");
    });
    consoleThread.detach();

    if (serverThread.joinable()) serverThread.join();
    server.stop();
    g_server = nullptr;
    return 0;
}
