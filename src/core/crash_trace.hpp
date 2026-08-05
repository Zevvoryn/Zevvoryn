#pragma once
// CRASHTRACE_V1: «хлебные крошки» — что сервер делал перед падением.
//
// Зачем это вместо стека с символами: стек читается только при наличии .pdb,
// а таскать .pdb в релиз нельзя (весит больше exe и раздаёт всю внутреннюю
// кухню). Здесь лог рассказывает о происходящем САМ: вместо «zevvoryn.exe+0x2868DB»
// будет «stop: saveWorldExtras — секция banners (14 записей)».
//
// Требования к реализации, раз это читается в уже умирающем процессе:
//   * НИКАКИХ мьютексов — поток мог упасть с захваченным мьютексом, будет дедлок;
//   * НИКАКИХ аллокаций при записи — куча могла быть повреждена;
//   * фиксированный кольцевой буфер статического размера и атомарный счётчик.
//
// Стоимость одной крошки — vsnprintf в стековый буфер плюс копирование до 120 байт.
// Ставить их на границах операций (сохранение, загрузка, остановка), НЕ в горячем
// цикле тика по каждому блоку.

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace nc {
namespace crash {

inline constexpr int kTraceSlots = 128;   // глубина истории
inline constexpr int kTraceTextMax = 120; // длина одной записи

struct TraceSlot {
    std::atomic<unsigned long long> seq{0}; // 0 = пусто/пишется прямо сейчас
    unsigned long thread = 0;
    long long ms = 0;
    char text[kTraceTextMax] = {0};
};

inline TraceSlot* traceSlots() {
    static TraceSlot slots[kTraceSlots];
    return slots;
}

inline std::atomic<unsigned long long>& traceCounter() {
    static std::atomic<unsigned long long> counter{0};
    return counter;
}

inline long long traceNowMs() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

inline unsigned long traceThreadId() {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentThreadId());
#else
    return 0UL;
#endif
}

// Каждый поток может подписаться один раз («Server thread», «Autosave», «Conn 7») —
// тогда в отчёте вместо голого числа будет понятное имя.
inline thread_local char g_traceThreadName[32] = {0};

inline void setTraceThreadName(const char* name) {
    if (!name) return;
    std::snprintf(g_traceThreadName, sizeof(g_traceThreadName), "%s", name);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline void trace(const char* fmt, ...) {
    char buf[kTraceTextMax];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    const unsigned long long n = traceCounter().fetch_add(1, std::memory_order_relaxed) + 1;
    TraceSlot& slot = traceSlots()[(n - 1) % kTraceSlots];

    // seq = 0 на время записи: если краш придётся ровно сюда, читатель
    // просто пропустит полурваную строку вместо того чтобы показать мусор.
    slot.seq.store(0, std::memory_order_release);
    slot.thread = traceThreadId();
    slot.ms = traceNowMs();
    if (g_traceThreadName[0]) {
        std::snprintf(slot.text, sizeof(slot.text), "[%s] %s", g_traceThreadName, buf);
    } else {
        std::memcpy(slot.text, buf, sizeof(slot.text));
        slot.text[sizeof(slot.text) - 1] = 0;
    }
    slot.seq.store(n, std::memory_order_release);
}

// Самые свежие — внизу, как в обычном логе. Последняя строка и есть
// то место, где сервер умер.
inline std::string dumpTrace(int maxLines = 40) {
    const unsigned long long total = traceCounter().load(std::memory_order_acquire);
    if (total == 0) return "  (пусто — до краша ни одной отметки не было)\n";

    if (maxLines > kTraceSlots) maxLines = kTraceSlots;
    unsigned long long first = total > static_cast<unsigned long long>(maxLines)
                                   ? total - static_cast<unsigned long long>(maxLines) + 1
                                   : 1;

    std::string out;
    for (unsigned long long n = first; n <= total; ++n) {
        const TraceSlot& slot = traceSlots()[(n - 1) % kTraceSlots];
        if (slot.seq.load(std::memory_order_acquire) != n) continue; // затёрто или рваное
        char head[48];
        std::snprintf(head, sizeof(head), "  %6llu  +%8.3f\u0441  \u043f\u043e\u0442\u043e\u043a %-6lu  ",
                      static_cast<unsigned long long>(n),
                      static_cast<double>(slot.ms) / 1000.0,
                      slot.thread);
        out += head;
        out += slot.text;
        out += "\n";
    }
    if (out.empty()) out = "  (все отметки затёрты — слишком много событий после краша)\n";
    return out;
}

} // namespace crash
} // namespace nc

// Короткие имена для мест вызова. Формат — printf, НЕ fmt::format:
//   NC_CTRACE("stop: saveWorlds start, chunks=%d", n);
#define NC_CTRACE(...) ::nc::crash::trace(__VA_ARGS__)
#define NC_CTRACE_THREAD(name) ::nc::crash::setTraceThreadName(name)
