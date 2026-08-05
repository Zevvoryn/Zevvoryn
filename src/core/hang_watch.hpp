#pragma once
// HANGDIAG_V1: сторож зависаний.
//
// Предыдущая попытка (CRASHSAFE_V1) писала диагностику через обычный логер
// (nc::log::rawLine) — и молчала. Логер берёт свой мьютекс, собирает std::string
// и кладёт строку в очередь GUI-консоли. Если зависший поток держит лог-мьютекс
// или блокировку кучи — сторож встаёт рядом и тоже молчит. Именно это и случилось.
//
// Здесь никакого логера и никакого GUI:
//   * отчёт пишется НАПРЯМУЮ в logs/hang-*.txt через CreateFile/WriteFile;
//   * шапка и журнал действий ложатся на диск ПЕРВЫМИ, до любых рискованных шагов;
//   * стек снимается С ЗАВИСШЕГО потока со стороны (SuspendThread + StackWalk64);
//   * по жёсткому таймауту процесс закрывается, чтобы окно не висело вечно.

#include "crash_dump.hpp" // CRASHDEEP_V1 + CRASHTRACE_V1

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace nc {
namespace crash {

#ifdef _WIN32

// Запись файла голым WinAPI: ни ofstream, ни filesystem, ни локалей, ни логера.
inline void hangWriteFile(const char* path, const char* text, size_t len) {
    CreateDirectoryA("logs", nullptr);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, text, static_cast<DWORD>(len), &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
}

inline void hangStampName(char* out, size_t cap) {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    std::snprintf(out, cap, "logs/hang-%02d.%02d.%02d-%02d%02d%02d.txt",
                  tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year % 100,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

// Стек чужого (зависшего) потока. На время снятия поток приостанавливается:
// иначе StackWalk64 читает стек прямо под изменением и врёт.
inline std::string hangStackOf(HANDLE thread) {
    if (!thread) return "  (нет дескриптора потока)\n";
    if (SuspendThread(thread) == static_cast<DWORD>(-1))
        return "  (не удалось приостановить поток)\n";
    std::string out;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &ctx)) {
        out += stackOfThread(thread, ctx);
        out += "\nРегистры зависшего потока:\n";
        out += registersOf(ctx);
    } else {
        out += "  (не удалось получить контекст потока)\n";
    }
    ResumeThread(thread);
    return out;
}

// Сторож с двумя порогами. Создаётся на стеке перед опасной операцией,
// снимается автоматически в деструкторе — в том числе при исключении.
class HangWatch {
public:
    HangWatch(const char* what, int softSec = 10, int hardSec = 40)
        : what_(what ? what : "?"), soft_(softSec), hard_(hardSec) {
        targetId_ = GetCurrentThreadId();
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                        GetCurrentProcess(), &target_, 0, FALSE, DUPLICATE_SAME_ACCESS);
        initSymbols(); // ЗАРАНЕЕ, пока всё живо: dbghelp внутри берёт loader lock
        thread_ = std::thread([this] { run(); });
    }

    ~HangWatch() {
        done_.store(true);
        if (thread_.joinable()) thread_.join();
        if (target_) CloseHandle(target_);
    }

    HangWatch(const HangWatch&) = delete;
    HangWatch& operator=(const HangWatch&) = delete;

private:
    // true — операция завершилась, сторож больше не нужен
    bool waitFor(int seconds) {
        for (int i = 0; i < seconds * 10; ++i) {
            if (done_.load()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return done_.load();
    }

    std::string header(int waited, bool fatal) const {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "---- Zevvoryn Hang Report ----\n\n"
            "Что делали: %s\n"
            "Поток %lu не отвечает уже %d секунд.\n%s\n",
            what_.c_str(), static_cast<unsigned long>(targetId_), waited,
            fatal ? "Ждать больше нечего — процесс будет закрыт принудительно."
                  : "Сервер продолжает ждать.");
        return buf;
    }

    void snapshot(int waited, bool fatal) {
        char path[128];
        hangStampName(path, sizeof(path));

        // Шаг 1 — гарантированная часть: шапка и журнал действий. Он никогда не виснет.
        std::string text = header(waited, fatal);
        text += "Сборка: " + buildStamp() + "  (TimeDateStamp-SizeOfImage из PE)\n";
        text += "\nПоследние действия сервера (свежие — внизу):\n";
        text += dumpTrace(40);
        hangWriteFile(path, text.c_str(), text.size());
        hangWriteFile("logs/hang-last.txt", text.c_str(), text.size());

        // Шаг 2 — стек зависшего потока. Этот шаг теоретически может зависнуть сам
        // (dbghelp аллоцирует память), поэтому шаг 1 уже лежит на диске.
        text += "\nСтек зависшего потока (самая верхняя строка — где стоит):\n";
        text += hangStackOf(target_);
        text += "\nИмён функций может не быть — это норма для релиза. Строки вида\n";
        text += "zevvoryn.exe+0xRVA разработчик расшифровывает своим pdb той же сборки.\n";
        hangWriteFile(path, text.c_str(), text.size());
        hangWriteFile("logs/hang-last.txt", text.c_str(), text.size());
    }

    void run() {
        if (waitFor(soft_)) return;
        snapshot(soft_, false);
        if (waitFor(hard_ > soft_ ? hard_ - soft_ : 1)) return;
        snapshot(hard_, true);
        std::_Exit(5); // окно больше не висит никогда
    }

    std::string what_;
    int soft_ = 10;
    int hard_ = 40;
    DWORD targetId_ = 0;
    HANDLE target_ = nullptr;
    std::atomic<bool> done_{false};
    std::thread thread_;
};

#else // !_WIN32

class HangWatch {
public:
    HangWatch(const char*, int = 10, int = 40) {}
};

#endif

} // namespace crash
} // namespace nc
