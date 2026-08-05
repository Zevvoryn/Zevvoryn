#pragma once
// CRASHDEEP_V1: глубокие подробности краша.
//
// Раньше отчёт говорил только "SEH 0xC0000005 по адресу 0x...", а это
// бесполезно: адрес меняется от сборки к сборке. Здесь собирается
// всё, что позволяет найти виновника без отладчика:
//   * расшифровка кода исключения и вида доступа (чтение/запись/исполнение);
//   * модуль + смещение вместо голого адреса;
//   * стек вызовов с именами функций и строками исходников (если рядом есть .pdb);
//   * регистры и идентификатор потока;
//   * окружение процесса (версия, PID, время работы, память);
//   * минидамп рядом с отчётом — его можно открыть в Visual Studio и увидеть все потоки.
//
// Всё с перестраховкой: код работает в уже умирающем процессе, поэтому любой
// шаг может упасть сам — каждый блок обёрнут в try/catch и проверки.

#include "crash_trace.hpp" // CRASHTRACE_V1

#include <string>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

namespace nc {
namespace crash {

#ifdef _WIN32

inline bool& symbolsReady() { static bool v = false; return v; }

inline void initSymbols() {
    if (symbolsReady()) return;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (SymInitialize(GetCurrentProcess(), nullptr, TRUE)) symbolsReady() = true;
}

// Имена взяты из winnt.h. Коды, которые реально встречаются в живом сервере,
// сопровождаются пояснением на русском — чтобы не гуглить код каждый раз.
inline const char* codeName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION (обращение по неверному указателю)";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW (бесконечная рекурсия)";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO (целое деление на ноль)";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION (повреждён код или указатель на функцию)";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR (страница недоступна, часто диск/сеть)";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_INT_OVERFLOW:          return "INT_OVERFLOW";
        case EXCEPTION_BREAKPOINT:            return "BREAKPOINT";
        case 0xE06D7363:                      return "C++ EH (вылетело исключение C++)";
        default:                              return "неизвестный код";
    }
}

// Модуль + смещение. Именно смещение переживает ASLR и годится для сравнения
// двух отчётов одной и той же сборки.
inline std::string moduleOf(DWORD64 addr) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &mod) || !mod) {
        return "<неизвестный модуль>";
    }
    char path[MAX_PATH] = {0};
    if (!GetModuleFileNameA(mod, path, MAX_PATH)) return "<модуль без имени>";
    const char* base = std::strrchr(path, 92);
    const char* slash = std::strrchr(path, 47);
    if (slash && (!base || slash > base)) base = slash;
    const char* name = base ? base + 1 : path;
    const DWORD64 off = addr - reinterpret_cast<DWORD64>(mod);
    char buf[MAX_PATH + 64];
    std::snprintf(buf, sizeof(buf), "%s+0x%llX", name, static_cast<unsigned long long>(off));
    return buf;
}

// Одна строка стека: имя функции + файл:строка из .pdb, или модуль+смещение.
inline std::string frameLine(int idx, DWORD64 pc) {
    char head[64];
    std::snprintf(head, sizeof(head), "  #%02d 0x%016llX ", idx, static_cast<unsigned long long>(pc));
    std::string out = head;

    bool named = false;
    if (symbolsReady()) {
        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {0};
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = MAX_SYM_NAME;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), pc, &disp, sym)) {
            out += sym->Name;
            if (disp) {
                char d[32];
                std::snprintf(d, sizeof(d), "+0x%llX", static_cast<unsigned long long>(disp));
                out += d;
            }
            named = true;

            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD lineDisp = 0;
            if (SymGetLineFromAddr64(GetCurrentProcess(), pc, &lineDisp, &line) && line.FileName) {
                char l[MAX_PATH + 32];
                std::snprintf(l, sizeof(l), "  (%s:%lu)", line.FileName, static_cast<unsigned long>(line.LineNumber));
                out += l;
            }
        }
    }
    if (!named) out += moduleOf(pc);
    return out + "\n";
}

// StackWalk64 портит переданный CONTEXT, поэтому берём его копией.
// HANGDIAG_V1: тот же разбор стека, но для ЛЮБОГО потока, а не только текущего.
// Нужно, чтобы сторож мог снять стек ЗАВИСШЕГО потока со стороны.
inline std::string stackOfThread(HANDLE thread, CONTEXT ctx) {
    std::string out;
#if defined(_M_X64)
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = ctx.Rip;    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
#elif defined(_M_IX86)
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = ctx.Eip;    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Ebp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Esp; sf.AddrStack.Mode = AddrModeFlat;
    const DWORD machine = IMAGE_FILE_MACHINE_I386;
#else
    (void)ctx;
    return "  (архитектура не поддерживает разбор стека)\n";
#endif
#if defined(_M_X64) || defined(_M_IX86)
    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(machine, GetCurrentProcess(), thread, &sf, &ctx,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        if (sf.AddrPC.Offset == 0) break;
        out += frameLine(i, sf.AddrPC.Offset);
    }
#endif
    if (out.empty()) out = "  (стек разобрать не удалось)\n";
    return out;
}

inline std::string stackOf(CONTEXT ctx) { return stackOfThread(GetCurrentThread(), ctx); } // HANGDIAG_V1

inline std::string registersOf(const CONTEXT& c) {
    char buf[1024];
#if defined(_M_X64)
    std::snprintf(buf, sizeof(buf),
        "  RIP=%016llX RSP=%016llX RBP=%016llX EFL=%08lX\n"
        "  RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n"
        "  RSI=%016llX RDI=%016llX R8 =%016llX R9 =%016llX\n"
        "  R10=%016llX R11=%016llX R12=%016llX R13=%016llX\n"
        "  R14=%016llX R15=%016llX\n",
        (unsigned long long)c.Rip, (unsigned long long)c.Rsp, (unsigned long long)c.Rbp, (unsigned long)c.EFlags,
        (unsigned long long)c.Rax, (unsigned long long)c.Rbx, (unsigned long long)c.Rcx, (unsigned long long)c.Rdx,
        (unsigned long long)c.Rsi, (unsigned long long)c.Rdi, (unsigned long long)c.R8,  (unsigned long long)c.R9,
        (unsigned long long)c.R10, (unsigned long long)c.R11, (unsigned long long)c.R12, (unsigned long long)c.R13,
        (unsigned long long)c.R14, (unsigned long long)c.R15);
#elif defined(_M_IX86)
    std::snprintf(buf, sizeof(buf),
        "  EIP=%08lX ESP=%08lX EBP=%08lX EFL=%08lX\n"
        "  EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX\n",
        (unsigned long)c.Eip, (unsigned long)c.Esp, (unsigned long)c.Ebp, (unsigned long)c.EFlags,
        (unsigned long)c.Eax, (unsigned long)c.Ebx, (unsigned long)c.Ecx, (unsigned long)c.Edx,
        (unsigned long)c.Esi, (unsigned long)c.Edi);
#else
    (void)c;
    std::snprintf(buf, sizeof(buf), "  (регистры недоступны на этой архитектуре)\n");
#endif
    return buf;
}

// Окружение процесса: часто сразу видно, что кончилась память или сервер упал на старте.
inline std::string environmentInfo(const char* version) {
    std::string out;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "  Версия ядра: %s\n  PID: %lu   Поток: %lu\n",
                  version ? version : "?", GetCurrentProcessId(), GetCurrentThreadId());
    out += buf;

    const ULONGLONG up = GetTickCount64();
    std::snprintf(buf, sizeof(buf), "  Процесс живёт (с загрузки системы): %llu с\n",
                  static_cast<unsigned long long>(up / 1000ULL));
    out += buf;

    FILETIME ct{}, et{}, kt{}, ut{};
    if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
        ULARGE_INTEGER k{}, u{};
        k.LowPart = kt.dwLowDateTime; k.HighPart = kt.dwHighDateTime;
        u.LowPart = ut.dwLowDateTime; u.HighPart = ut.dwHighDateTime;
        std::snprintf(buf, sizeof(buf), "  ЦП: %.1f с в юзере, %.1f с в ядре\n",
                      static_cast<double>(u.QuadPart) / 1e7, static_cast<double>(k.QuadPart) / 1e7);
        out += buf;
    }

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        std::snprintf(buf, sizeof(buf), "  Память системы: занято %lu%%, свободно %llu МБ из %llu МБ\n",
                      static_cast<unsigned long>(ms.dwMemoryLoad),
                      static_cast<unsigned long long>(ms.ullAvailPhys >> 20),
                      static_cast<unsigned long long>(ms.ullTotalPhys >> 20));
        out += buf;
    }
    return out;
}

// Минидамп — самое ценное для разбора постфактум: в нём есть ВСЕ потоки,
// а не только тот, что упал.
inline bool writeMiniDump(EXCEPTION_POINTERS* info, const char* path) {
    if (!path) return false;
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers = FALSE;
    // CRASHSAFE_V1: было MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory — эти два
    // флага заставляют dbghelp обойти ВСЮ кучу процесса. Если упавший поток держал
    // блокировку кучи (а при сохранении мира он её держит почти всегда) — запись
    // дампа встаёт насмерть, и окно висит без всякого отчёта. Лёгкий дамп берёт
    // стеки всех потоков — этого для разбора хватает.
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                      info ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(file);
    return ok == TRUE;
}

// CRASHTRACE_V1: отпечаток сборки из PE-заголовка. Два числа однозначно задают
// конкретный exe. По ним плюс RVA из стека разработчик восстанавливает имена
// функций СВОИМ pdb у себя — игроку ничего докачивать не надо.
inline std::string buildStamp() {
    HMODULE h = GetModuleHandleA(nullptr);
    if (!h) return "?";
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return "?";
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const BYTE*>(h) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return "?";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%08lX-%08lX",
                  static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
                  static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage));
    return buf;
}

// Главная точка входа: блок "Details" для отчёта. info может быть nullptr —
// тогда берём контекст текущего потока (случай abort / std::terminate / SIGSEGV).
inline std::string details(EXCEPTION_POINTERS* info, const char* version) {
    std::string out;
    try {
        initSymbols();

        if (info && info->ExceptionRecord) {
            const EXCEPTION_RECORD* er = info->ExceptionRecord;
            char buf[512];
            std::snprintf(buf, sizeof(buf), "  Код: 0x%08lX  %s\n",
                          static_cast<unsigned long>(er->ExceptionCode), codeName(er->ExceptionCode));
            out += buf;

            const DWORD64 pc = reinterpret_cast<DWORD64>(er->ExceptionAddress);
            std::snprintf(buf, sizeof(buf), "  Адрес сбоя: 0x%016llX  (%s)\n",
                          static_cast<unsigned long long>(pc), moduleOf(pc).c_str());
            out += buf;

            if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                 er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && er->NumberParameters >= 2) {
                const ULONG_PTR kind = er->ExceptionInformation[0];
                const char* what = kind == 0 ? "чтение" : kind == 1 ? "запись" : kind == 8 ? "исполнение (DEP)" : "неизвестный вид";
                const ULONG_PTR bad = er->ExceptionInformation[1];
                std::snprintf(buf, sizeof(buf), "  Доступ: %s по адресу 0x%016llX%s\n",
                              what, static_cast<unsigned long long>(bad),
                              bad < 0x10000 ? "  <- похоже на разыменование nullptr" : "");
                out += buf;
            }
        } else {
            out += "  Код: не SEH (abort / std::terminate / сигнал)\n";
        }

        out += "\nСтек вызовов (поток " + std::to_string(GetCurrentThreadId()) + "):\n";
        if (info && info->ContextRecord) {
            out += stackOf(*info->ContextRecord);
            out += "\nРегистры:\n";
            out += registersOf(*info->ContextRecord);
        } else {
            CONTEXT here{};
            here.ContextFlags = CONTEXT_FULL;
            RtlCaptureContext(&here);
            out += stackOf(here);
            out += "\nРегистры:\n";
            out += registersOf(here);
        }

        if (!symbolsReady()) {
            out += "\n  Имён функций нет (это норма для релиза). Строки вида zevvoryn.exe+0xRVA\n";
            out += "  расшифровываются разработчиком по его собственному pdb той же сборки.\n";
        }

        out += "\nСборка: " + buildStamp() + "  (TimeDateStamp-SizeOfImage из PE)\n";

        // CRASHTRACE_V1: главное для релиза — что сервер делал перед смертью.
        // Это читается без всяких символов и отладочных файлов.
        out += "\nПоследние действия сервера (свежие — внизу):\n";
        out += dumpTrace(40);

        out += "\nОкружение:\n";
        out += environmentInfo(version);
    } catch (...) {
        out += "  (сбор подробностей сам упал — берём что успели)\n";
    }
    return out;
}

#else // !_WIN32

inline std::string details(void*, const char*) { return "  (подробный стек реализован только для Windows)\n"; }
inline bool writeMiniDump(void*, const char*) { return false; }

#endif

} // namespace crash
} // namespace nc
