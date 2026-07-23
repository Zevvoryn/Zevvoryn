#include "core/server.hpp"
#include "core/config.hpp"
#include "core/log.hpp"
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
    std::cout << "\n\033[31m\033[1m[FATAL] Сервер аварийно остановлен!\033[0m\n";
    std::cout << "\033[31m  Ошибка: " << what << "\033[0m\n";
    NC_FATAL("Crash", "Сервер аварийно остановлен: {}", what);
    std::string crashPath = "logs/crash-last.txt";
    try {
        std::filesystem::create_directories("logs");
        std::time_t t = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char namebuf[48];
        std::snprintf(namebuf, sizeof(namebuf), "logs/crash-%02d.%02d.%02d-%02d%02d%02d.txt",
                      tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year % 100,
                      tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        crashPath = namebuf;
        std::ofstream cf(crashPath, std::ios::trunc);
        cf << "Zevvoryn crash report\n" << what << "\n";
    } catch (...) {}
    std::cout << "\033[33m  Ошибка сохранена в " << crashPath << " и в текущий лог.\033[0m\n";
    std::cout << "\033[33m  Окно закроется автоматически через 180 секунд...\033[0m\n";
    for (int left = 180; left > 0; left -= 30) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (left - 30 > 0)
            std::cout << "\033[90m  ...закрытие через " << (left - 30) << " с\033[0m\n";
    }
    std::cout << "\033[31m[FATAL] Время вышло, закрываю окно.\033[0m\n";
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
    const bool started = nc::setup::needsSetup(configPath)
        ? server.startWithConfig(nc::setup::runWizard())
        : server.start(configPath);
    if (!started) return 1;

    // CONSOLE_V2: server simulation owns world state; console lines are queued for its tick.
    std::thread serverThread([&server] { server.run(); });
    std::cout << "Console ready. Type help for commands.\n> " << std::flush;
    std::string line;
    while (server.isRunning() && std::getline(std::cin, line)) {
        server.queueConsoleCommand(line);
        if (line == "stop" || line == "/stop") break;
        std::cout << "> " << std::flush;
    }
    if (server.isRunning()) server.queueConsoleCommand("stop");
    if (serverThread.joinable()) serverThread.join();
    server.stop();
    g_server = nullptr;
    return 0;
}
