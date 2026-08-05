#include "core/server.hpp"
#include "core/config.hpp"
#include "core/log.hpp"
#include "core/crash_context.hpp" // CRASHCTX_V1
#include "core/crash_dump.hpp" // CRASHDEEP_V1
#include "core/embedded_bot_files.hpp" // AUTOEXTRACT_V1
#include "console/gui_console.hpp" // GUICON_V1
#include <iostream>
#include <csignal>
#include <thread>
#include <string>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#ifdef _WIN32
    #include <windows.h>
    #include <conio.h> // CRASHRESTART_V1: _kbhit/_getch для «ENTER = рестарт сразу»
#endif

static nc::NetherCraftServer* g_server = nullptr;

// GUIWIZARD_V1: true only while the first-run wizard is using the GUI window —
// the window's close button should exit immediately then (nothing to save yet).
static std::atomic<bool> g_wizardActive{false};
#ifdef _WIN32
// ONEINSTANCE_V1 / CLASSICNOW_V1: хендл нужен снаружи блока проверки: перед перезапуском
// в терминале его надо отпустить, иначе дочерний процесс решит, что сервер уже запущен.
static HANDLE g_instanceMutex = nullptr;
#endif

// LANGSYNC_V1: mirrors the effective language as soon as it's known (peeked config, or
// wizard result), independent of nc::NetherCraftServer's own config_. This is what the
// close-handlers below read, so an early close (before/around the wizard) always shows
// the right language instead of silently defaulting.
static std::atomic<bool> g_langRu{true};
// CLASSICTERM_V1: true, если classic-режим пишет в терминал родителя (cmd/PowerShell/.bat),
// а не в собственное окно консоли: такое окно закрывать и паузить нельзя.
static bool g_attachedToParentConsole = false;

// GUIWIZARD_V1: bridge for nc::log::setGuiReadLine — must be a plain function
// pointer (see core/log.hpp GuiReadLine).
static std::string wizardReadLineBridge() { return nc::console::readWizardLine(); }

// PATHU8_V1: fs::path::string() на Windows отдаёт путь в ANSI (CP1251), а консоль у нас в UTF-8 —
// из-за этого C:\Users\<русское имя> превращался в кракозябры. u8string() всегда UTF-8.
static std::string pathU8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// AUTOSTARTPANEL_V1: запуск DiscrordBotRcon/index.js (Discord-бот + веб-панель) вместе с
// zevvoryn.exe, если это включено в мастере установки (auto-start-panel=true).
#ifdef _WIN32
static PROCESS_INFORMATION g_panelProc{};
static bool g_panelProcStarted = false;

// AUTOEXTRACT_V1: если папки/index.js нет рядом с exe и в текущей рабочей папке,
// распаковываем встроенные в бинарник исходники бота (index.js, webpanel.js, rcon.js,
// commands.js, deploy-commands.js, package.json) прямо в целевую папку. Не перезаписываем
// уже существующие файлы (например, .env мастера или файлы, изменённые пользователем).
static void extractEmbeddedBotFiles(const std::filesystem::path& panelDir, bool ru) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(panelDir, ec);
    int written = 0;
    int updated = 0;
    for (const auto& [name, content] : nc::embeddedBotFiles()) {
        fs::path target = panelDir / name;
        const bool existed = fs::exists(target);
        if (existed) {
            // AUTOEXTRACT_V2: an old panel version on disk used to survive forever because
            // existing files were skipped. Now outdated bundled files are refreshed and the
            // previous content is kept next to them as <name>.bak.
            std::ifstream in(target, std::ios::binary);
            const std::string onDisk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            if (onDisk == std::string(content)) continue;
            fs::copy_file(target, fs::path(target.string() + ".bak"), fs::copy_options::overwrite_existing, ec);
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) continue;
        out << content;
        out.close();
        if (out) { if (existed) ++updated; else ++written; }
    }
    if (updated > 0) {
        if (ru) {
            std::cout << "\033[36m[AutoStart] " << updated
                       << " \xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2 \xd0\xb1\xd0\xbe\xd1\x82\xd0\xb0/\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd0\xb8 \xd0\xbe\xd0\xb1\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xbe \xd0\xb4\xd0\xbe \xd0\xb2\xd0\xb5\xd1\x80\xd1\x81\xd0\xb8\xd0\xb8 \xd0\xb8\xd0\xb7 exe (\xd1\x81\xd1\x82\xd0\xb0\xd1\x80\xd1\x8b\xd0\xb5 \xd0\xba\xd0\xbe\xd0\xbf\xd0\xb8\xd0\xb8 \xd1\x80\xd1\x8f\xd0\xb4\xd0\xbe\xd0\xbc \xd1\x81 .bak)\033[0m\n" << std::flush;
        } else {
            std::cout << "\033[36m[AutoStart] " << updated
                       << " bundled bot/panel files were refreshed from the exe (old copies kept as .bak)\033[0m\n" << std::flush;
        }
    }
    if (written > 0) {
        if (ru) {
            std::cout << "\033[36m[AutoStart] \xd0\xa0\xd0\xb0\xd1\x81\xd0\xbf\xd0\xb0\xd0\xba\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xbe " << written
                       << " \xd0\xb2\xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xb5\xd0\xbd\xd0\xbd\xd1\x8b\xd1\x85 \xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2 \xd0\xb1\xd0\xbe\xd1\x82\xd0\xb0 \xd0\xb2 " << pathU8(panelDir)
                       << " (\xd0\xb8\xd0\xb7 \xd1\x81\xd0\xb0\xd0\xbc\xd0\xbe\xd0\xb3\xd0\xbe zevvoryn.exe). \xd0\x9e\xd1\x81\xd1\x82\xd0\xb0\xd1\x91\xd1\x82\xd1\x81\xd1\x8f \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe npm install \xd0\xb4\xd0\xbb\xd1\x8f node_modules.\033[0m\n" << std::flush;
        } else {
            std::cout << "\033[36m[AutoStart] Extracted " << written << " bundled bot files into " << pathU8(panelDir)
                       << " (from zevvoryn.exe itself). Only npm install for node_modules is still needed.\033[0m\n" << std::flush;
        }
    }
}

// RCONENV_V1: DiscrordBotRcon/.env must have RCON_HOST/RCON_PORT/RCON_PASSWORD matching
// settings.properties, or the web panel/Discord bot's RCON connection silently stays
// "disconnected". Users should never have to hand-sync two separate config files for
// this to work, so we do it automatically every time the panel (re)starts.
// WNARROW_V1: корректный wide->UTF-8 (без C4244 и без кракозябр)
static std::string narrowW(const std::wstring& w) {
#ifdef _WIN32
    if (w.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(need > 0 ? need : 0), '\0');
    if (need > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], need, nullptr, nullptr);
    return out;
#else
    return std::string(w.begin(), w.end());
#endif
}

static void syncPanelEnvFile(const std::filesystem::path& panelDir, const nc::ServerConfig& cfg, bool ru) {
    namespace fs = std::filesystem;
    fs::path envPath = panelDir / ".env";
    std::vector<std::string> lines;
    bool existed = fs::exists(envPath);
    if (existed) {
        std::ifstream in(envPath, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    auto setKv = [&](const std::string& key, const std::string& value) {
        const std::string prefix = key + "=";
        for (auto& l : lines) {
            if (l.compare(0, prefix.size(), prefix) == 0) {
                l = prefix + value;
                return;
            }
        }
        lines.push_back(prefix + value);
    };
    // Эти три поля обязаны совпадать с settings.properties, иначе RCON молча не подключится.
    setKv("RCON_HOST", "127.0.0.1");
    setKv("RCON_PORT", std::to_string(cfg.rconPort));
    setKv("RCON_PASSWORD", cfg.rconPassword);
    if (!existed) {
        // Свежесозданный файл: заполняем остальные поля разумными значениями по умолчанию,
        // чтобы .env сразу был полным и понятным (Discord-бот остаётся выключенным, пока
        // пользователь сам не впишет токен).
        setKv("DISCORD_TOKEN", "");
        setKv("CLIENT_ID", "");
        setKv("GUILD_ID", "");
        setKv("RCON_TIMEOUT_MS", "10000");
        setKv("ADMIN_ROLE_ID", "");
        setKv("COMMAND_CHANNEL_ID", "");
        setKv("WEB_ENABLED", "true");
        setKv("WEB_HOST", "127.0.0.1");
        setKv("WEB_PORT", "3000");
        setKv("WEB_PASSWORD", "");
        setKv("WEB_ROOT_PASSWORD", "");
        setKv("WEB_SESSION_TTL_MIN", "720");
        setKv("WEB_LANG", cfg.language == "rus" ? "ru" : "en");
    }
    std::ofstream out(envPath, std::ios::binary | std::ios::trunc);
    for (auto& l : lines) out << l << "\r\n";
    out.close();
    if (ru) {
        std::cout << "\033[36m[AutoStart] " << (existed ? "\xd0\x9e\xd0\xb1\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbb\xd1\x91\xd0\xbd " : "\xd0\xa1\xd0\xbe\xd0\xb7\xd0\xb4\xd0\xb0\xd0\xbd ") << pathU8(envPath)
                   << " \xd1\x81 RCON-\xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd0\xb5\xd0\xbc \xd0\xb8\xd0\xb7 settings.properties (\xd1\x87\xd1\x82\xd0\xbe\xd0\xb1\xd1\x8b \xd0\xb2\xd0\xb5\xd0\xb1-\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c/\xd0\xb1\xd0\xbe\xd1\x82 \xd0\xbc\xd0\xbe\xd0\xb3\xd0\xbb\xd0\xb8 \xd0\xbf\xd0\xbe\xd0\xb4\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb8\xd1\x82\xd1\x8c\xd1\x81\xd1\x8f \xd0\xba RCON \xd0\xb1\xd0\xb5\xd0\xb7 \xd1\x80\xd1\x83\xd1\x87\xd0\xbd\xd0\xbe\xd0\xb9 \xd0\xbf\xd1\x80\xd0\xb0\xd0\xb2\xd0\xba\xd0\xb8).\033[0m\n" << std::flush;
    } else {
        std::cout << "\033[36m[AutoStart] " << (existed ? "Updated " : "Created ") << pathU8(envPath)
                   << " with the RCON password from settings.properties (so the web panel/bot can connect to RCON without manual editing).\033[0m\n" << std::flush;
    }
}

// NODEFIND_V1: Node.js may be installed but missing from this process' PATH, or not installed
// at all. Look in PATH first, then in the standard install locations.
static std::wstring findNodeExe() {
    namespace fs = std::filesystem;
    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"node.exe", nullptr, MAX_PATH, found, nullptr) > 0) return std::wstring(found);
    auto envVar = [](const wchar_t* name) -> std::wstring {
        wchar_t buf[MAX_PATH]{};
        DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
        return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring();
    };
    std::vector<std::wstring> candidates;
    const std::wstring pf = envVar(L"ProgramFiles");
    const std::wstring pf86 = envVar(L"ProgramFiles(x86)");
    const std::wstring lad = envVar(L"LOCALAPPDATA");
    const std::wstring apd = envVar(L"APPDATA");
    if (!pf.empty())   candidates.push_back(pf + L"\\nodejs\\node.exe");
    if (!pf86.empty()) candidates.push_back(pf86 + L"\\nodejs\\node.exe");
    if (!lad.empty())  candidates.push_back(lad + L"\\Programs\\nodejs\\node.exe");
    if (!apd.empty())  candidates.push_back(apd + L"\\nvm\\node.exe");
    candidates.push_back(L"C:\\Program Files\\nodejs\\node.exe");
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c;
    }
    return std::wstring();
}

// RUNWAIT_V1: run a command inside workDir and wait for it. CreateProcessW takes the working
// directory directly, so no "cd /d ... && ..." chain that cmd.exe silently splits apart.
static int runAndWait(const std::wstring& cmdLine, const std::wstring& workDir, bool newConsole) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                        newConsole ? CREATE_NEW_CONSOLE : 0,
                        nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

// ASKYN_V1: Enter or y/yes/da means yes; only an explicit n / net means no. Console input is
// not UTF-8 on Windows, so Cyrillic "n" is matched by its cp1251/cp866/UTF-8 first byte.
static bool askYesNo() {
    std::string a;
    if (!std::getline(std::cin, a)) return false;
    while (!a.empty() && (a.back() == '\r' || a.back() == ' ' || a.back() == '\t')) a.pop_back();
    if (a.empty()) return true;
    const unsigned char c0 = static_cast<unsigned char>(a[0]);
    if (c0 == 'n' || c0 == 'N' || c0 == '0') return false;
    if (c0 == 0xED || c0 == 0xAD) return false;                                  // cp1251 / cp866 "n"
    if (c0 == 0xD0 && a.size() > 1 && static_cast<unsigned char>(a[1]) == 0xBD) return false; // UTF-8 "n"
    return true;
}

// NODEINSTALL_V1: no Node.js -> say it in red, offer to install it (winget, then the official
// MSI), and only fall back to "go download it yourself" if both ways fail.
static std::wstring ensureNodeAvailable(bool ru) {
    std::wstring node = findNodeExe();
    if (!node.empty()) return node;
    if (ru) {
        std::cout << "\033[31m[AutoStart] Node.js " << "\xd0\xbd\xd0\xb5 \xd0\xbd\xd0\xb0\xd0\xb9\xd0\xb4\xd0\xb5\xd0\xbd \xd0\xbd\xd0\xb0 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xbc \xd0\xba\xd0\xbe\xd0\xbc\xd0\xbf\xd1\x8c\xd1\x8e\xd1\x82\xd0\xb5\xd1\x80\xd0\xb5 (\xd0\xb1\xd0\xb5\xd0\xb7 \xd0\xbd\xd0\xb5\xd0\xb3\xd0\xbe \xd0\xb2\xd0\xb5\xd0\xb1-\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c \xd0\xb8 \xd0\xb1\xd0\xbe\xd1\x82 \xd0\xbd\xd0\xb5 \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd1\x82\xd1\x8f\xd1\x82\xd1\x81\xd1\x8f)." << "\033[0m\n" << std::flush;
        std::cout << "\033[33m[AutoStart] " << "\xd0\xa1\xd0\xba\xd0\xb0\xd1\x87\xd0\xb0\xd1\x82\xd1\x8c \xd0\xb8 \xd1\x83\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xb8\xd1\x82\xd1\x8c Node.js LTS \xd1\x81\xd0\xb5\xd0\xb9\xd1\x87\xd0\xb0\xd1\x81? [Y/n]: " << "\033[0m" << std::flush;
    } else {
        std::cout << "\033[31m[AutoStart] Node.js was not found on this machine (the web panel and the Discord bot cannot run without it).\033[0m\n" << std::flush;
        std::cout << "\033[33m[AutoStart] Download and install Node.js LTS now? [Y/n]: \033[0m" << std::flush;
    }
    if (!askYesNo()) {
        if (ru) std::cout << "\033[33m[AutoStart] " << "\xd0\x9e\xd0\xba, \xd1\x83\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xb8\xd1\x82\xd0\xb5 \xd1\x81\xd0\xb0\xd0\xbc\xd0\xb8: https://nodejs.org/en/download (\xd0\xbf\xd0\xbe\xd1\x82\xd0\xbe\xd0\xbc \xd0\xbf\xd0\xb5\xd1\x80\xd0\xb5\xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd1\x82\xd0\xb8\xd1\x82\xd0\xb5 \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80)." << "\033[0m\n" << std::flush;
        else    std::cout << "\033[33m[AutoStart] Fine - install it yourself: https://nodejs.org/en/download (then restart the server).\033[0m\n" << std::flush;
        return std::wstring();
    }
    if (ru) std::cout << "\033[36m[AutoStart] " << "\xd0\xa3\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xb0\xd0\xb2\xd0\xbb\xd0\xb8\xd0\xb2\xd0\xb0\xd1\x8e Node.js LTS \xd1\x87\xd0\xb5\xd1\x80\xd0\xb5\xd0\xb7 winget, \xd0\xbf\xd0\xbe\xd0\xb4\xd0\xbe\xd0\xb6\xd0\xb4\xd0\xb8\xd1\x82\xd0\xb5..." << "\033[0m\n" << std::flush;
    else    std::cout << "\033[36m[AutoStart] Installing Node.js LTS via winget, please wait...\033[0m\n" << std::flush;
    runAndWait(L"cmd.exe /C winget install -e --id OpenJS.NodeJS.LTS --accept-source-agreements --accept-package-agreements",
               std::wstring(), true);
    node = findNodeExe();
    if (!node.empty()) return node;
    if (ru) std::cout << "\033[36m[AutoStart] " << "winget \xd0\xbd\xd0\xb5 \xd1\x81\xd1\x80\xd0\xb0\xd0\xb1\xd0\xbe\xd1\x82\xd0\xb0\xd0\xbb, \xd1\x81\xd0\xba\xd0\xb0\xd1\x87\xd0\xb8\xd0\xb2\xd0\xb0\xd1\x8e \xd0\xbe\xd1\x84\xd0\xb8\xd1\x86\xd0\xb8\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd1\x8b\xd0\xb9 MSI-\xd1\x83\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd1\x89\xd0\xb8\xd0\xba Node.js..." << "\033[0m\n" << std::flush;
    else    std::cout << "\033[36m[AutoStart] winget did not work, downloading the official Node.js MSI...\033[0m\n" << std::flush;
    const std::wstring psCmd =
        L"powershell -NoProfile -ExecutionPolicy Bypass -Command "
        L"\"$ErrorActionPreference='Stop';"
        L"$u='https://nodejs.org/dist/v22.14.0/node-v22.14.0-x64.msi';"
        L"$o=Join-Path $env:TEMP 'nodejs-lts-x64.msi';"
        L"Invoke-WebRequest -Uri $u -OutFile $o -UseBasicParsing;"
        L"Start-Process msiexec.exe -ArgumentList '/i',$o,'/passive','/norestart' -Wait\"";
    runAndWait(psCmd, std::wstring(), true);
    node = findNodeExe();
    if (!node.empty()) {
        if (ru) std::cout << "\033[32m[AutoStart] Node.js " << "\xd1\x83\xd1\x81\xd0\xbf\xd0\xb5\xd1\x88\xd0\xbd\xd0\xbe \xd1\x83\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd." << "\033[0m\n" << std::flush;
        else    std::cout << "\033[32m[AutoStart] Node.js installed successfully.\033[0m\n" << std::flush;
        return node;
    }
    if (ru) std::cout << "\033[31m[AutoStart] " << "\xd0\x90\xd0\xb2\xd1\x82\xd0\xbe\xd0\xbc\xd0\xb0\xd1\x82\xd0\xb8\xd1\x87\xd0\xb5\xd1\x81\xd0\xba\xd0\xb0\xd1\x8f \xd1\x83\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xba\xd0\xb0 \xd0\xbd\xd0\xb5 \xd1\x83\xd0\xb4\xd0\xb0\xd0\xbb\xd0\xb0\xd1\x81\xd1\x8c. \xd0\x9e\xd1\x82\xd0\xba\xd1\x80\xd1\x8b\xd0\xb2\xd0\xb0\xd1\x8e https://nodejs.org/en/download \xe2\x80\x94 \xd1\x83\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xb8\xd1\x82\xd0\xb5 \xd0\xb8 \xd0\xbf\xd0\xb5\xd1\x80\xd0\xb5\xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd1\x82\xd0\xb8\xd1\x82\xd0\xb5 \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80." << "\033[0m\n" << std::flush;
    else    std::cout << "\033[31m[AutoStart] Automatic install failed. Opening https://nodejs.org/en/download - install it and restart the server.\033[0m\n" << std::flush;
    runAndWait(L"cmd.exe /C start \"\" https://nodejs.org/en/download", std::wstring(), false);
    return std::wstring();
}

static void spawnPanelProcess(const nc::ServerConfig& cfg) {
    namespace fs = std::filesystem;
    const bool ru = (cfg.language == "rus");
    // AUTOSTARTPANEL_V2: рабочая папка процесса может отличаться от папки проекта
    // (например, exe запущен из build-ninja) — ищем DiscrordBotRcon сначала рядом с
    // текущей рабочей папкой, потом рядом с самим zevvoryn.exe, и всегда логируем результат.
    // PANELDIR_V2: the folder shipped with the server is DiscrordBotRcon (historic typo);
    // accept the correctly spelled DiscordBotRcon too, so renaming it does not break autostart.
    fs::path panelDir = fs::exists(fs::path("DiscrordBotRcon") / "index.js") ? fs::path("DiscrordBotRcon")
                        : (fs::exists(fs::path("DiscordBotRcon") / "index.js") ? fs::path("DiscordBotRcon")
                                                                               : fs::path("DiscrordBotRcon"));
    if (!fs::exists(panelDir / "index.js")) {
        wchar_t exeBuf[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH) > 0) {
            fs::path exeDir = fs::path(exeBuf).parent_path();
            if (fs::exists(exeDir / "DiscrordBotRcon" / "index.js")) panelDir = exeDir / "DiscrordBotRcon";
            else if (fs::exists(exeDir / "DiscordBotRcon" / "index.js")) panelDir = exeDir / "DiscordBotRcon";
            else if (fs::exists(exeDir / "DiscrordBotRcon")) panelDir = exeDir / "DiscrordBotRcon";
        }
    }
    // AUTOEXTRACT_V1: не нашли index.js ни в одной из папок-кандидатов — досыпаем недостающие
    // файлы бота из самого exe в выбранную (или дефолтную) панель-папку и перепроверяем.
    if (!fs::exists(panelDir / "index.js")) {
        extractEmbeddedBotFiles(panelDir, ru);
    }
    if (!fs::exists(panelDir / "index.js")) {
        std::error_code cwdEc, dirEc;
        bool folderExists = fs::exists(panelDir, dirEc) && fs::is_directory(panelDir, dirEc);
        if (ru) {
            if (folderExists) {
                std::cout << "\033[33m[AutoStart] \xd0\x9f\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0 " << pathU8(panelDir)
                           << " \xd0\xbd\xd0\xb0\xd0\xb9\xd0\xb4\xd0\xb5\xd0\xbd\xd0\xb0 (" << pathU8(fs::current_path(cwdEc))
                           << "), \xd0\xbd\xd0\xbe \xd0\xb2 \xd0\xbd\xd0\xb5\xd0\xb9 \xd0\xbd\xd0\xb5\xd1\x82 index.js \xe2\x80\x94 \xd0\xbf\xd0\xbe\xd1\x85\xd0\xbe\xd0\xb6\xd0\xb5, \xd1\x8d\xd1\x82\xd0\xb0 \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0 \xd0\xb1\xd1\x8b\xd0\xbb\xd0\xb0 \xd1\x81\xd0\xbe\xd0\xb7\xd0\xb4\xd0\xb0\xd0\xbd\xd0\xb0 \xd0\xbc\xd0\xb0\xd1\x81\xd1\x82\xd0\xb5\xd1\x80\xd0\xbe\xd0\xbc \xd1\x83\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xba\xd0\xb8 \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd0\xb4\xd0\xbb\xd1\x8f .env. \xd0\xa1\xd0\xba\xd0\xbe\xd0\xbf\xd0\xb8\xd1\x80\xd1\x83\xd0\xb9 \xd1\x82\xd1\x83\xd0\xb4\xd0\xb0 index.js, webpanel.js, package.json, rcon.js, commands.js \xd0\xb8 node_modules \xd0\xb8\xd0\xb7 \xd0\xb8\xd1\x81\xd1\x85\xd0\xbe\xd0\xb4\xd0\xbd\xd0\xb8\xd0\xba\xd0\xbe\xd0\xb2 \xd0\xbf\xd1\x80\xd0\xbe\xd0\xb5\xd0\xba\xd1\x82\xd0\xb0. \xd0\x90\xd0\xb2\xd1\x82\xd0\xbe\xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba \xd0\xbf\xd1\x80\xd0\xbe\xd0\xbf\xd1\x83\xd1\x89\xd0\xb5\xd0\xbd.\033[0m\n" << std::flush;
            } else {
                std::cout << "\033[33m[AutoStart] " << pathU8(panelDir / "index.js")
                           << " \xd0\xbd\xd0\xb5 \xd0\xbd\xd0\xb0\xd0\xb9\xd0\xb4\xd0\xb5\xd0\xbd (\xd1\x82\xd0\xb5\xd0\xba\xd1\x83\xd1\x89\xd0\xb0\xd1\x8f \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0: " << pathU8(fs::current_path(cwdEc))
                           << "). \xd0\x9f\xd0\xbe\xd0\xbb\xd0\xbe\xd0\xb6\xd0\xb8 \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd1\x83 DiscrordBotRcon \xd1\x80\xd1\x8f\xd0\xb4\xd0\xbe\xd0\xbc \xd1\x81 zevvoryn.exe \xd0\xb8\xd0\xbb\xd0\xb8 \xd0\xb2 \xd1\x82\xd0\xb5\xd0\xba\xd1\x83\xd1\x89\xd0\xb5\xd0\xb9 \xd1\x80\xd0\xb0\xd0\xb1\xd0\xbe\xd1\x87\xd0\xb5\xd0\xb9 \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb5. \xd0\x90\xd0\xb2\xd1\x82\xd0\xbe\xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba \xd0\xbf\xd1\x80\xd0\xbe\xd0\xbf\xd1\x83\xd1\x89\xd0\xb5\xd0\xbd.\033[0m\n" << std::flush;
            }
        } else {
            if (folderExists) {
                std::cout << "\033[33m[AutoStart] Folder " << pathU8(panelDir)
                           << " was found (" << pathU8(fs::current_path(cwdEc))
                           << "), but it has no index.js \xe2\x80\x94 this folder was likely auto-created by the setup wizard, which only writes .env there. Copy index.js, webpanel.js, package.json, rcon.js, commands.js and node_modules from the project source into it. Auto-start skipped.\033[0m\n" << std::flush;
            } else {
                std::cout << "\033[33m[AutoStart] " << pathU8(panelDir / "index.js")
                           << " not found (current dir: " << pathU8(fs::current_path(cwdEc))
                           << "). Place the DiscrordBotRcon folder next to zevvoryn.exe or in the current working directory. Auto-start skipped.\033[0m\n" << std::flush;
            }
        }
        return;
    }
    // RCONENV_V1: keep DiscrordBotRcon/.env's RCON_* values in sync with settings.properties
    // on every launch attempt, before we even check for node_modules, so the panel/bot never
    // silently fail to authenticate with a stale or missing RCON password.
    syncPanelEnvFile(panelDir, cfg, ru);
    // NODEINSTALL_V1: make sure Node.js exists before anything else.
    const std::wstring nodeExe = ensureNodeAvailable(ru);
    if (nodeExe.empty()) return;
    if (!fs::exists(panelDir / "node_modules")) {
        // NPMPROMPT_V2: ask, then run npm install *inside* the panel folder.
        if (ru) {
            std::cout << "\033[31m[AutoStart] " << pathU8(panelDir) << "/node_modules \xd0\xbd\xd0\xb5 \xd0\xbd\xd0\xb0\xd0\xb9\xd0\xb4\xd0\xb5\xd0\xbd (\xd0\xb1\xd0\xb8\xd0\xb1\xd0\xbb\xd0\xb8\xd0\xbe\xd1\x82\xd0\xb5\xd0\xba\xd0\xb8 Node \xd0\xbd\xd0\xb5 \xd1\x83\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd1\x8b).\033[0m\n" << std::flush;
            std::cout << "\033[33m[AutoStart] \xd0\xa1\xd0\xba\xd0\xb0\xd1\x87\xd0\xb0\xd1\x82\xd1\x8c \xd0\xb8\xd1\x85 \xd1\x81\xd0\xb5\xd0\xb9\xd1\x87\xd0\xb0\xd1\x81 (npm install)? [Y/n]: \033[0m" << std::flush;
        } else {
            std::cout << "\033[31m[AutoStart] " << pathU8(panelDir) << "/node_modules not found (Node dependencies are missing).\033[0m\n" << std::flush;
            std::cout << "\033[33m[AutoStart] Download them now (npm install)? [Y/n]: \033[0m" << std::flush;
        }
        if (!askYesNo()) {
            if (ru) std::cout << "\033[33m[AutoStart] \xd0\x9e\xd0\xba, \xd1\x83\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xb8\xd1\x82\xd0\xb5 \xd1\x81\xd0\xb0\xd0\xbc\xd0\xb8: cd " << pathU8(panelDir) << " && npm install\033[0m\n" << std::flush;
            else    std::cout << "\033[33m[AutoStart] Fine, install them yourself: cd " << pathU8(panelDir) << " && npm install\033[0m\n" << std::flush;
            return;
        }
        if (ru) std::cout << "\033[36m[AutoStart] \xd0\x97\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba\xd0\xb0\xd1\x8e npm install \xd0\xb2 \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb5 \xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd0\xb8, \xd1\x8d\xd1\x82\xd0\xbe \xd0\xbc\xd0\xbe\xd0\xb6\xd0\xb5\xd1\x82 \xd0\xb7\xd0\xb0\xd0\xbd\xd1\x8f\xd1\x82\xd1\x8c \xd0\xbc\xd0\xb8\xd0\xbd\xd1\x83\xd1\x82\xd1\x83...\033[0m\n" << std::flush;
        else    std::cout << "\033[36m[AutoStart] Running npm install, this can take a minute...\033[0m\n" << std::flush;
        const fs::path nodeDir = fs::path(nodeExe).parent_path();
        const fs::path npmCmdPath = nodeDir / "npm.cmd";
        std::error_code npmEc;
        std::wstring npmCmdLine = fs::exists(npmCmdPath, npmEc)
            ? (L"cmd.exe /C call \"" + npmCmdPath.wstring() + L"\" install")
            : std::wstring(L"cmd.exe /C call npm install");
        const std::wstring panelDirW = fs::absolute(panelDir, npmEc).wstring();
        const int npmRc = runAndWait(npmCmdLine, panelDirW, true);
        if (npmRc != 0 || !fs::exists(panelDir / "node_modules")) {
            if (ru) std::cout << "\033[31m[AutoStart] npm install \xd0\xbd\xd0\xb5 \xd1\x83\xd0\xb4\xd0\xb0\xd0\xbb\xd1\x81\xd1\x8f (\xd0\xba\xd0\xbe\xd0\xb4 " << npmRc << "). \xd0\x9c\xd0\xbe\xd0\xb6\xd0\xbd\xd0\xbe \xd0\xb2\xd1\x80\xd1\x83\xd1\x87\xd0\xbd\xd1\x83\xd1\x8e: cd " << pathU8(panelDir) << " && npm install\033[0m\n" << std::flush;
            else    std::cout << "\033[31m[AutoStart] npm install failed (code " << npmRc << "). Manual way: cd " << pathU8(panelDir) << " && npm install\033[0m\n" << std::flush;
            return;
        }
        if (ru) std::cout << "\033[32m[AutoStart] \xd0\x97\xd0\xb0\xd0\xb2\xd0\xb8\xd1\x81\xd0\xb8\xd0\xbc\xd0\xbe\xd1\x81\xd1\x82\xd0\xb8 \xd1\x83\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd1\x8b, \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba\xd0\xb0\xd1\x8e \xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c...\033[0m\n" << std::flush;
        else    std::cout << "\033[32m[AutoStart] Dependencies installed, starting the panel...\033[0m\n" << std::flush;
    }
    // CRASHLOG_V1: панель запускается в свёрнутом отдельном окне (SW_SHOWMINNOACTIVE), и это окно
    // закрывается мгновенно вместе с процессом node.exe -- то есть при падении окно
    // исчезает без единого видимого символа, даже если Node успел напечатать стектрейс.
    // Перенаправляем stdout/stderr дочернего процесса в файл на диске, чтобы причина
    // краша сохранялась и не терялась вместе с закрывшимся окном.
    std::wstring logPathW = (panelDir / L"panel-crash.log").wstring();
    SECURITY_ATTRIBUTES logSa{}; logSa.nLength = sizeof(logSa); logSa.bInheritHandle = TRUE; logSa.lpSecurityDescriptor = nullptr;
    HANDLE hPanelLog = CreateFileW(logPathW.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &logSa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    if (hPanelLog != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = hPanelLog;
        si.hStdError = hPanelLog;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    // NODEFIND_V1: launch the exact node.exe we found (PATH may not contain it).
    std::wstring cmdStr = L"\"" + nodeExe + L"\" index.js";
    std::vector<wchar_t> cmd(cmdStr.begin(), cmdStr.end());
    cmd.push_back(L'\0');
    std::wstring wPanelDir = panelDir.wstring();
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NEW_CONSOLE, nullptr, wPanelDir.c_str(), &si, &pi)) {
        g_panelProc = pi;
        g_panelProcStarted = true;
        if (ru) {
            std::cout << "\033[32m[AutoStart] Discord-\xd0\xb1\xd0\xbe\xd1\x82/\xd0\xb2\xd0\xb5\xd0\xb1-\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x89\xd0\xb5\xd0\xbd\xd1\x8b \xd0\xb0\xd0\xb2\xd1\x82\xd0\xbe\xd0\xbc\xd0\xb0\xd1\x82\xd0\xb8\xd1\x87\xd0\xb5\xd1\x81\xd0\xba\xd0\xb8 (PID " << pi.dwProcessId << ")\033[0m\n" << std::flush;
        } else {
            std::cout << "\033[32m[AutoStart] Discord bot/web panel started automatically (PID " << pi.dwProcessId << ")\033[0m\n" << std::flush;
        }
        // CRASHLOG_WATCH_V1: если процесс завершается практически сразу же после запуска (не
        // смог стартовать из-за отсутствующего .env / токена / порта и т.п.), пользователю
        // было бы невидно и причину пришлось бы искать в panel-crash.log вручную. Вместо этого сразу
        // подтягиваем содержимое лога прямо в основной консоль сервера.
        HANDLE watchProc = pi.hProcess;
        std::wstring watchLogPath = logPathW;
        bool watchRu = ru;
        std::thread([watchProc, watchLogPath, watchRu]() {
            if (WaitForSingleObject(watchProc, 3000) == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeProcess(watchProc, &code);
                std::ifstream logIn(watchLogPath, std::ios::binary);
                std::string logText((std::istreambuf_iterator<char>(logIn)), std::istreambuf_iterator<char>());
                if (watchRu) {
                    std::cout << "\033[31m[AutoStart] Discord-\xd0\xb1\xd0\xbe\xd1\x82/\xd0\xb2\xd0\xb5\xd0\xb1-\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c \xd0\xb7\xd0\xb0\xd0\xb2\xd0\xb5\xd1\x80\xd1\x88\xd0\xb8\xd0\xbb\xd0\xb0\xd1\x81\xd1\x8c \xd1\x81\xd1\x80\xd0\xb0\xd0\xb7\xd1\x83 \xd0\xbf\xd0\xbe\xd1\x81\xd0\xbb\xd0\xb5 \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba\xd0\xb0 (\xd0\xba\xd0\xbe\xd0\xb4 " << code << "). \xd0\x9f\xd1\x80\xd0\xb8\xd1\x87\xd0\xb8\xd0\xbd\xd0\xb0 (\xd0\xb8\xd0\xb7 " << narrowW(watchLogPath) << "):\033[0m\n" << std::flush;
                } else {
                    std::cout << "\033[31m[AutoStart] Discord bot/web panel process exited right after launch (code " << code << "). Reason (from " << narrowW(watchLogPath) << "):\033[0m\n" << std::flush;
                }
                if (!logText.empty()) {
                    std::cout << logText << std::flush;
                }
            }
        }).detach();
    } else {
        if (ru) {
            std::cout << "\033[31m[AutoStart] \xd0\x9d\xd0\xb5 \xd1\x83\xd0\xb4\xd0\xb0\xd0\xbb\xd0\xbe\xd1\x81\xd1\x8c \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd1\x82\xd0\xb8\xd1\x82\xd1\x8c Discord-\xd0\xb1\xd0\xbe\xd1\x82/\xd0\xb2\xd0\xb5\xd0\xb1-\xd0\xbf\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c (\xd1\x83\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd \xd0\xbb\xd0\xb8 Node.js \xd0\xb8 \xd0\xb5\xd1\x81\xd1\x82\xd1\x8c \xd0\xbb\xd0\xb8 \xd0\xbe\xd0\xbd \xd0\xb2 PATH?). \xd0\x9a\xd0\xbe\xd0\xb4 \xd0\xbe\xd1\x88\xd0\xb8\xd0\xb1\xd0\xba\xd0\xb8: " << GetLastError() << "\033[0m\n" << std::flush;
        } else {
            std::cout << "\033[31m[AutoStart] Failed to start Discord bot/web panel (is Node.js installed and on PATH?). Error code: " << GetLastError() << "\033[0m\n" << std::flush;
        }
    }
    if (hPanelLog != INVALID_HANDLE_VALUE) CloseHandle(hPanelLog);
}

static void stopPanelProcess() {
    if (!g_panelProcStarted) return;
    g_panelProcStarted = false;
    TerminateProcess(g_panelProc.hProcess, 0);
    CloseHandle(g_panelProc.hThread);
    CloseHandle(g_panelProc.hProcess);
}
#else
static void spawnPanelProcess(const nc::ServerConfig&) {}
static void stopPanelProcess() {}
#endif

// CRASHGUARD_V1: при фатальной ошибке сервер не закрывает окно молча.
// Пишем ошибку красным в консоль, дублируем в основной лог и в logs/crash-*.txt,
// даём 180 секунд на чтение и только потом закрываемся.
// CRASHDEEP_V1: SEH-фильтр кладёт сюда указатель на контекст исключения,
// чтобы crashNote() разобрал стек и регистры ИМЕННО упавшего потока,
// а не того места внутри обработчика, где мы сейчас находимся.
#ifdef _WIN32
static std::atomic<EXCEPTION_POINTERS*> g_sehInfo{nullptr}; // CRASHDEEP_V1
#endif

// CRASHSAFE_V1: обработчик краша сам может зависнуть навсегда: поток мог упасть,
// держа блокировку кучи или loader lock, а сбор символов и запись минидампа
// ту же блокировку требуют. Именно так окно застывало на «Сохранение мира...» без
// всякого отчёта. Сторож ниже всегда доводит дело до конца.
static std::atomic<bool> g_crashReportDone{false}; // CRASHSAFE_V1

static void crashNote(const std::string& what) {
    static std::atomic<bool> g_crashOnce{false};
    if (g_crashOnce.exchange(true)) { // повторный сбой внутри обработчика — просто ждём и выходим
        std::this_thread::sleep_for(std::chrono::seconds(200));
        std::_Exit(3);
    }

    // CRASHSAFE_V1: сторож стартует РАНЬШЕ всего остального. Если через 25 секунд
    // отчёт так и не дописан — значит обработчик застрял в символах или минидампе.
    // Тогда пишем аварийный короткий отчёт (журнал последних действий всегда доступен,
    // он не требует ни кучи, ни мьютексов) и убиваем процесс, чтобы окно не висело.
    {
        std::string whatCopy = what;
        std::thread([whatCopy]() {
            for (int i = 0; i < 250; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (g_crashReportDone.load()) return;
            }
            std::string emerg;
            emerg += "---- Zevvoryn Crash Report (аварийный: обработчик завис) ----\n\n";
            emerg += "Reason:\n" + whatCopy + "\n\n";
            emerg += "Обработчик не успел за 25 секунд — скорее всего упавший поток держал\n";
            emerg += "блокировку кучи. Ниже — журнал последних действий сервера.\n\n";
            emerg += nc::crash::dumpTrace(40);
            try {
                std::filesystem::create_directories("logs");
                std::ofstream hf("logs/crash-hang.txt", std::ios::trunc);
                hf << emerg;
            } catch (...) {}
            std::cout << "\n\033[31m\033[1m" << emerg << "\033[0m" << std::endl;
            std::_Exit(4);
        }).detach();
    }

    // CRASHNET_V1: ПЕРВЫМ делом уводим сервер оффлайн. Раньше crashNote() просто
    // печатал отчёт и висел 180 секунд, а сеть всё это время оставалась живой —
    // accept-луп принимал новых игроков, и на «упавший» сервер можно было спокойно
    // зайти и играть. Теперь сразу закрываем listen-сокет и рвём все соединения —
    // ещё ДО печати отчёта и окна ожидания.
    if (g_server) {
        g_server->getNetwork().crashShutdown();
    }
    stopPanelProcess(); // AUTOSTARTPANEL_V1: не держать старый бот/панель живым при авто-рестарте

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
    // CRASHDEEP_V1: самое важное — где именно упало. Стек с именами функций,
    // вид доступа, модуль+смещение, регистры и окружение процесса.
    report += "\nDetails:\n";
#ifdef _WIN32
    report += nc::crash::details(g_sehInfo.load(), nc::NC_VERSION);
#else
    report += nc::crash::details(nullptr, nc::NC_VERSION);
#endif

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
#ifdef _WIN32
    // CRASHDEEP_V1: рядом с текстовым отчётом кладём минидамп: в нём есть ВСЕ потоки,
    // а не только упавший. Открывается двойным кликом в Visual Studio.
    try {
        std::string dumpPath = crashPath;
        if (dumpPath.size() > 4) dumpPath.resize(dumpPath.size() - 4);
        dumpPath += ".dmp";
        if (nc::crash::writeMiniDump(g_sehInfo.load(), dumpPath.c_str())) {
            std::cout << "\033[33m  Минидамп для отладки: " << dumpPath << "\033[0m\n";
        }
    } catch (...) {}
#endif
    g_crashReportDone.store(true); // CRASHSAFE_V1: отчёт целиком на диске — сторож больше не нужен
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
    // CRASHDEEP_V1: сначала запоминаем контекст — без него не будет ни стека,
    // ни регистров, ни минидампа.
    g_sehInfo.store(info);
    char buf[320];
    std::string where = nc::crash::moduleOf(reinterpret_cast<DWORD64>(info->ExceptionRecord->ExceptionAddress));
    std::snprintf(buf, sizeof(buf), "критический сбой SEH 0x%08lX (%s) по адресу %p [%s]",
                  (unsigned long)info->ExceptionRecord->ExceptionCode,
                  nc::crash::codeName(info->ExceptionRecord->ExceptionCode),
                  info->ExceptionRecord->ExceptionAddress,
                  where.c_str());
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
static bool serverLangRu() { // WINSAVE_V1
    // LANGSYNC_V1: g_langRu всегда отражает последний известный язык (до и после мастера),
    // в отличие от g_server->getConfig(), который до startWithConfig()/start() мог бы ещё держать дефолтное значение.
    return g_langRu.load();
}

static BOOL WINAPI consoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_LOGOFF_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        const bool ru = serverLangRu();
        // WINSAVE_V1: Windows 11 / Windows Terminal убивает процесс быстрее, чем старая
        // conhost, а окно исчезает сразу — поэтому всё дублируется в лог-файл, а у системы
        // просим дополнительное время на завершение.
        if (HWND consoleWnd = GetConsoleWindow())
            ShutdownBlockReasonCreate(consoleWnd, ru ? L"Сохранение мира..." : L"Saving the world...");
        std::cout << (ru
            ? "\n\033[31m\033[1mВсе твои данные… будут потеряны если ты будешь меня закрывать принудительно\033[0m\n"
            : "\n\033[31m\033[1mAll your data... will be lost if you keep killing me like that\033[0m\n") << std::flush;
        std::cout << (ru
            ? "\033[33m...ладно, шучу. Экстренно сохраняю мир и игроков...\033[0m\n"
            : "\033[33m...okay, just kidding. Emergency-saving the world and players...\033[0m\n") << std::flush;
        // те же строки в лог-файл: окно может уже закрыться, лог останется
        if (ru) NC_INFO("Main", "Крестик консоли: экстренное сохранение мира и игроков");
        else NC_INFO("Main", "Console close button: emergency world + player save");
        if (g_server) g_server->stop("closed"); // CLEANEXIT_V3: в last-exit.txt попадёт именно «закрыли крестиком»
        stopPanelProcess(); // AUTOSTARTPANEL_V1
        if (ru) NC_INFO("Main", "Готово, всё сохранено");
        else NC_INFO("Main", "Done, everything is saved");
        std::cout << (ru
            ? "\033[32mГотово, всё сохранено. Но лучше останавливай командой stop ;)\033[0m\n"
            : "\033[32mDone, everything is saved. Still, prefer the stop command ;)\033[0m\n") << std::flush;
        if (HWND consoleWnd = GetConsoleWindow()) ShutdownBlockReasonDestroy(consoleWnd);
        return TRUE; // мы всё сделали — процесс может закрываться
    }
    return FALSE; // Ctrl+C / Ctrl+Break обрабатываются signalHandler'ом как раньше
}
#endif

void signalHandler(int signal) {
    if (g_server) {
        NC_INFO("Main", "Received signal {}, stopping...", signal);
        g_server->stop("signal"); // CLEANEXIT_V3: Ctrl+C — это не команда stop
    }
}

// EXITPAUSE_V1: при запуске двойным кликом Windows закрывает окно вместе с процессом,
// и причину ошибки прочитать невозможно. Ждём любую клавишу — но только если
// консоль наша собственная: в чужом терминале, .bat-файле или CI пауза только мешает.
static void pauseBeforeExit(bool guiActive = false) {
#ifdef _WIN32
    // NOCONHOST_V1: если уже открыто своё GUI-окно — оно не закроется само и ошибка уже
    // видна в нём; ждать нажатие клавиши в отвязанной (FreeConsole) консоли здесь нельзя —
    // это повесило бы процесс навсегда.
    if (guiActive) return;
    // CLASSICTERM_V1: если мы прицепились к чужому терминалу, он никуда не денется после выхода,
    // а пауза там только мешает скриптам.
    if (g_attachedToParentConsole) return;
    DWORD pids[4] = {};
    const DWORD owners = GetConsoleProcessList(pids, 4);
    if (owners > 1) return; // окно переживёт наш выход — сообщение и так видно
    std::cout << "\n\033[33mНажми любую клавишу, чтобы закрыть это окно...\033[0m\n"
              << "\033[90mPress any key to close this window...\033[0m\n" << std::flush;
    while (_kbhit()) (void)_getch(); // выгребаем случайные нажатия, иначе окно закроется сразу
    (void)_getch();
#endif
}

#ifdef _WIN32
// CLASSICNOW_V1: мастер всегда идёт в нашем окне (конфига ещё нет, читать неоткуда),
// но если в нём выбрали classic — ждать следующего запуска глупо. Открываем
// системный терминал и продолжаем работу там, как в CLASSICTERM_V2 при обычном старте.
static bool relaunchInSystemTerminal(int argc, char* argv[]) {
    wchar_t exePathW[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exePathW, MAX_PATH) == 0) return false;

    const std::wstring exePath(exePathW);
    const std::wstring workDir = std::filesystem::path(exePath).parent_path().wstring();
    const std::wstring q(1, static_cast<wchar_t>(34)); // кавычка без экранирования

    std::wstring extraArgs;
    for (int ai = 1; ai < argc; ++ai) {
        const int need = MultiByteToWideChar(CP_ACP, 0, argv[ai], -1, nullptr, 0);
        if (need <= 1) continue;
        std::wstring warg(static_cast<size_t>(need - 1), static_cast<wchar_t>(0));
        MultiByteToWideChar(CP_ACP, 0, argv[ai], -1, warg.data(), need);
        extraArgs += L" " + q + warg + q;
    }
    const std::wstring quotedExe = q + exePath + q;

    // Метка защищает от бесконечного перезапуска: у дочернего родитель — cmd,
    // так что AttachConsole в начале main() подхватит его окно.
    SetEnvironmentVariableW(L"ZEVVORYN_TERM_CHILD", L"1");

    auto spawnInTerminal = [&](const std::wstring& cmdLine) -> bool {
        std::wstring mutableCmd = cmdLine;
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                            CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT, nullptr,
                            workDir.empty() ? nullptr : workDir.c_str(), &si, &pi))
            return false;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    };

    bool ok = spawnInTerminal(L"wt.exe -d " + q + workDir + q + L" cmd.exe /k " + quotedExe + extraArgs);
    if (!ok) ok = spawnInTerminal(L"cmd.exe /k " + quotedExe + extraArgs);
    if (!ok) SetEnvironmentVariableW(L"ZEVVORYN_TERM_CHILD", nullptr);
    return ok;
}
#endif

int main(int argc, char* argv[]) {
    // GUICON_V2: путь к конфигу нужен до первого вывода: по нему решаем, какая консоль главная.
    // NOCONHOST_V1: этот блок переехал выше бывшего VTFIRST_V1 — теперь мы должны знать guiWanted
    // ДО любого обращения к консоли: с /SUBSYSTEM:WINDOWS её больше не создаёт сама Windows,
    // и для classic-режима мы должны вызвать AllocConsole() сами, раньше любого вывода.
    std::string configPath = "settings.properties";
    if (argc > 1) {
        configPath = argv[1];
    }

    const bool firstRun = nc::setup::needsSetup(configPath);
    bool guiWanted = false;
    bool guiRu     = true;
    std::string guiTitle = "Zevvoryn";
    // NOCONHOST_V1: текст диагностики копим, но печатаем через NC_INFO чуть позже — после того как
    // консоль (для classic) точно уже есть.
    std::string diagLine;
    std::string cfgWarn; // CFGVALIDATE_V1
    if (!firstRun) {
        const nc::ServerConfig peek = nc::ServerConfig::loadFrom(configPath);
        // CFGVALIDATE_V1: неизвестное значение больше не уходит в classic молча.
        const bool modeKnown = (peek.consoleMode == "gui" || peek.consoleMode == "classic");
        const bool langKnown = (peek.language == "rus" || peek.language == "eng");
        guiWanted = modeKnown ? (peek.consoleMode == "gui") : true;
        guiRu     = langKnown ? (peek.language == "rus") : true;
        if (!modeKnown)
            cfgWarn += "console-mode=" + peek.consoleMode + " — " + std::string(guiRu ? "\xd0\xbd\xd0\xb5\xd0\xb8\xd0\xb7\xd0\xb2\xd0\xb5\xd1\x81\xd1\x82\xd0\xbd\xd0\xbe\xd0\xb5 \xd0\xb7\xd0\xbd\xd0\xb0\xd1\x87\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5, \xd0\xbe\xd0\xb6\xd0\xb8\xd0\xb4\xd0\xb0\xd0\xbb\xd0\xbe\xd1\x81\xd1\x8c gui \xd0\xb8\xd0\xbb\xd0\xb8 classic; \xd0\xbe\xd1\x82\xd0\xba\xd0\xb0\xd1\x82 \xd0\xbd\xd0\xb0 gui." : "unknown value, expected gui or classic; falling back to gui.") + " ";
        if (!langKnown)
            cfgWarn += "language=" + peek.language + " — " + std::string("\xd0\xbd\xd0\xb5\xd0\xb8\xd0\xb7\xd0\xb2\xd0\xb5\xd1\x81\xd1\x82\xd0\xbd\xd1\x8b\xd0\xb9 \xd1\x8f\xd0\xb7\xd1\x8b\xd0\xba, \xd0\xbe\xd0\xb6\xd0\xb8\xd0\xb4\xd0\xb0\xd0\xbb\xd0\xbe\xd1\x81\xd1\x8c rus \xd0\xb8\xd0\xbb\xd0\xb8 eng; \xd0\xbe\xd1\x82\xd0\xba\xd0\xb0\xd1\x82 \xd0\xbd\xd0\xb0 rus. / unknown language, expected rus or eng; falling back to rus.");
        guiTitle  = std::string("Zevvoryn \xe2\x80\x94 ") + peek.motd;
        // DIAG_V1: помогает понять, какой именно файл и какие значения реально загрузились —
        // если вы правите console-mode в файле, а приложение всё равно откроет старый
        // режим, эта строка в логах покажет, читается ли вообще тот файл, который вы правили.
        diagLine = "config: " + pathU8(std::filesystem::absolute(configPath))
            + " | console-mode=" + peek.consoleMode + " | language=" + peek.language;
    } else {
        // GUIWIZARD_V1: конфига ещё нет — язык и MOTD станут известны только после мастера,
        // но окно нужно поднять уже сейчас, иначе шапка и весь диалог уйдёт в classic-консоль.
        guiWanted = nc::console::isAvailable();
        guiTitle  = std::string("Zevvoryn \xe2\x80\x94 ") + "\xd0\xa3\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xba\xd0\xb0";
    }

#ifdef _WIN32
    // NOCONHOST_V1: в classic-режиме (или если GUI недоступен) консоли ещё нет — поднимаем свою
    // свою свежую (точно такую же, какую раньше автоматически создавала подсистема CONSOLE),
    // ДО первого лога — иначе хендл в writeConsole() прочитается невалидным в пустоту.
    if (!guiWanted) {
        // CLASSICTERM_V1: classic — это именно терминал Windows, а не наше окно.
        // Сначала пробуем прицепиться к консоли родителя: если сервер запущен из cmd,
        // PowerShell, Windows Terminal или .bat — весь вывод останется в том же окне,
        // а не улетит в отдельное всплывшее окно. Двойной клик по exe родительской
        // консоли не даёт — тогда создаём свою через AllocConsole().
        g_attachedToParentConsole = (AttachConsole(ATTACH_PARENT_PROCESS) != 0);

        // CLASSICTERM_V2: при запуске двойным кликом родительской консоли нет, а AllocConsole()
        // рисует голый conhost — именно он и выглядит как «наше» окно.
        // Настоящий classic — это системный терминал: Windows Terminal (Windows 11),
        // а если его нет — обычный cmd.exe. Поэтому перезапускаем себя внутри терминала
        // и сразу выходим; у дочернего процесса родитель — cmd, так что AttachConsole выше
        // подхватит его окно. Метка ZEVVORYN_TERM_CHILD защищает от бесконечного перезапуска.
        if (!g_attachedToParentConsole) {
            wchar_t* childMark = nullptr; size_t childMarkLen = 0;
            _wdupenv_s(&childMark, &childMarkLen, L"ZEVVORYN_TERM_CHILD");
            const bool alreadyChild = (childMark != nullptr);
            if (childMark) free(childMark);

            bool relaunched = false;
            if (!alreadyChild) {
                wchar_t exePathW[MAX_PATH] = {0};
                if (GetModuleFileNameW(nullptr, exePathW, MAX_PATH) > 0) {
                    const std::wstring exePath(exePathW);
                    std::error_code wdec;
                    std::wstring workDir = std::filesystem::path(exePath).parent_path().wstring();

                    // Аргументы (путь к конфигу) передаём дальше как есть.
                    std::wstring extraArgs;
                    for (int ai = 1; ai < argc; ++ai) {
                        const int need = MultiByteToWideChar(CP_ACP, 0, argv[ai], -1, nullptr, 0);
                        if (need <= 1) continue;
                        std::wstring warg(static_cast<size_t>(need - 1), L'\0');
                        MultiByteToWideChar(CP_ACP, 0, argv[ai], -1, warg.data(), need);
                        extraArgs += L" \"" + warg + L"\"";
                    }
                    const std::wstring quotedExe = L"\"" + exePath + L"\"";

                    SetEnvironmentVariableW(L"ZEVVORYN_TERM_CHILD", L"1");

                    auto spawnInTerminal = [&](const std::wstring& cmdLine) -> bool {
                        std::wstring mutableCmd = cmdLine;
                        STARTUPINFOW si{}; si.cb = sizeof(si);
                        PROCESS_INFORMATION pi{};
                        if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                                            CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                            workDir.empty() ? nullptr : workDir.c_str(), &si, &pi))
                            return false;
                        CloseHandle(pi.hThread);
                        CloseHandle(pi.hProcess);
                        return true;
                    };

                    // 1) Windows Terminal (Windows 11 и установленный вручную на Windows 10)
                    relaunched = spawnInTerminal(L"wt.exe -d \"" + workDir + L"\" cmd.exe /k " + quotedExe + extraArgs);
                    // 2) Запасной путь: обычный cmd в своём окне (Windows 10 без wt)
                    if (!relaunched) relaunched = spawnInTerminal(L"cmd.exe /k " + quotedExe + extraArgs);
                    if (!relaunched) SetEnvironmentVariableW(L"ZEVVORYN_TERM_CHILD", nullptr);
                }
            }

            if (relaunched) return 0; // работа продолжится в окне терминала
            AllocConsole();          // терминал не нашёлся — как раньше, своя консоль
        }

        FILE* nc_stdout = nullptr;
        FILE* nc_stderr = nullptr;
        FILE* nc_stdin  = nullptr;
        freopen_s(&nc_stdout, "CONOUT$", "w", stdout);
        freopen_s(&nc_stderr, "CONOUT$", "w", stderr);
        freopen_s(&nc_stdin,  "CONIN$",  "r", stdin);

        if (g_attachedToParentConsole) {
            // В чужом терминале курсор стоит после строки с командой запуска.
            std::cout << "\n" << std::flush;
        } else {
            // Своя консоль — доводим её до ума: заголовок и длинная прокрутка,
            // иначе стартовый лог уезжает за пределы буфера уже на загрузке мира.
            SetConsoleTitleW(guiRu ? L"Zevvoryn \u2014 \u0441\u0435\u0440\u0432\u0435\u0440 (classic)"
                                   : L"Zevvoryn \u2014 server (classic)");
            HANDLE hConOut = GetStdHandle(STD_OUTPUT_HANDLE);
            CONSOLE_SCREEN_BUFFER_INFO csbi{};
            if (hConOut != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(hConOut, &csbi)) {
                COORD bufSize = csbi.dwSize;
                if (bufSize.X < 100) bufSize.X = 100;
                if (bufSize.Y < 3000) bufSize.Y = 3000;
                SetConsoleScreenBufferSize(hConOut, bufSize);
            }
        }

        // Выделение мышью и Enter для копирования — штатное поведение консоли Windows,
        // плюс построчный ввод с эхом для команд и мастера настройки.
        HANDLE hConIn = GetStdHandle(STD_INPUT_HANDLE);
        DWORD inMode = 0;
        if (hConIn != INVALID_HANDLE_VALUE && GetConsoleMode(hConIn, &inMode)) {
            SetConsoleMode(hConIn, inMode | ENABLE_EXTENDED_FLAGS | ENABLE_QUICK_EDIT_MODE
                                          | ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
        }
    }
#endif

#ifdef _WIN32
    // VTFIRST_V1: console setup MUST happen before the first line we print,
    // otherwise the version banner is emitted while the console still has
    // virtual-terminal processing disabled and the user sees the raw escapes
    // (←[36m←[1m...). This is what happened on Windows 10 / older
    // conhost, where VT is off by default; Windows 11 Terminal enables it for us.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    bool g_ansiOk = false;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)) {
        if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) {
            g_ansiOk = true;
        } else {
            DWORD want = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            g_ansiOk = SetConsoleMode(hOut, want) != 0;
            if (!g_ansiOk) SetConsoleMode(hOut, mode); // restore, terminal is too old
        }
    } else if (hOut != INVALID_HANDLE_VALUE) {
        // Output is redirected to a file or a pipe: colours would be garbage there.
        g_ansiOk = false;
    }
#else
    const bool g_ansiOk = true;
#endif

    g_langRu.store(guiRu); // LANGSYNC_V1
    // GUIBACKLOG_V2: буфер заводится только в GUI-режиме и только на время старта окна.
    if (guiWanted) nc::log::enableBacklog(true);
    // NOCONHOST_V1: теперь печатаем отложенную выше диагностику — консоль (classic) уже поднята,
    // а в gui-режиме это уйдёт в бэклог и попадёт в окно как только оно откроется.
    if (!diagLine.empty()) NC_INFO("Main", diagLine.c_str());
    if (!cfgWarn.empty()) NC_WARN("Main", cfgWarn.c_str()); // CFGVALIDATE_V1

    // VERSION_V1: версия ядра первой строкой, до любых других логов.
    if (g_ansiOk) {
        std::cout << "\033[36m\033[1m" << nc::NC_CODENAME << " V: " << nc::NC_VERSION
                  << "\033[0m  \033[90m(Minecraft 1.21.1, protocol 767)\033[0m\n" << std::flush;
    } else {
        // Plain text fallback — better than printing raw escape sequences.
        std::cout << nc::NC_CODENAME << " V: " << nc::NC_VERSION
                  << "  (Minecraft 1.21.1, protocol 767)\n" << std::flush;
    }
    // GUIBACKLOG_V1: та же шапка без ANSI — чтобы окно-терминал увидел её при реплее.
    nc::log::guiLine(std::string(nc::NC_CODENAME) + " V: " + nc::NC_VERSION +
                     "  (Minecraft 1.21.1, protocol 767)", 6); // GUICOLOR_V2: голубая, как в classic

#ifdef _WIN32
    // ONEINSTANCE_V1: два экземпляра из одной папки пишут в один world.dat и latest.log,
    // и это ловилось только на bind(), когда мир уже тронут. Отсекаем сразу.
    {
        std::error_code mtxEc;
        std::string key = pathU8(std::filesystem::absolute(configPath, mtxEc));
        for (auto& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        unsigned long long hash = 1469598103934665603ULL;
        for (unsigned char ch : key) { hash ^= ch; hash *= 1099511628211ULL; }
        wchar_t mutexName[64];
        swprintf_s(mutexName, L"Local\\ZevvorynServer_%016llx", hash);
        g_instanceMutex = CreateMutexW(nullptr, TRUE, mutexName);
        if (g_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            const char* warn = guiRu ? "\xd0\xa1\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80 \xd0\xb8\xd0\xb7 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb9 \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb8 \xd1\x83\xd0\xb6\xd0\xb5 \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x89\xd0\xb5\xd0\xbd. \xd0\x94\xd0\xb2\xd0\xb0 \xd1\x8d\xd0\xba\xd0\xb7\xd0\xb5\xd0\xbc\xd0\xbf\xd0\xbb\xd1\x8f\xd1\x80\xd0\xb0 \xd0\xbf\xd0\xb8\xd1\x88\xd1\x83\xd1\x82 \xd0\xb2 \xd0\xbe\xd0\xb4\xd0\xb8\xd0\xbd \xd0\xbc\xd0\xb8\xd1\x80 \xd0\xb8 \xd0\xbb\xd0\xbe\xd0\xb3 \xd0\xb8 \xd0\xbb\xd0\xbe\xd0\xbc\xd0\xb0\xd1\x8e\xd1\x82 \xd0\xb5\xd0\xb3\xd0\xbe \xe2\x80\x94 \xd0\xb7\xd0\xb0\xd0\xba\xd1\x80\xd0\xbe\xd0\xb9 \xd0\xbf\xd0\xb5\xd1\x80\xd0\xb2\xd0\xbe\xd0\xb5 \xd0\xbe\xd0\xba\xd0\xbd\xd0\xbe \xd0\xb8\xd0\xbb\xd0\xb8 \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba\xd0\xb0\xd0\xb9 \xd0\xba\xd0\xbe\xd0\xbf\xd0\xb8\xd1\x8e \xd0\xb8\xd0\xb7 \xd0\xb4\xd1\x80\xd1\x83\xd0\xb3\xd0\xbe\xd0\xb9 \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb8." : "This server folder is already running. Two instances write the same world and log and corrupt it \xe2\x80\x94 close the first window or start the copy from another folder.";
            NC_FATAL("Main", warn);
            MessageBoxW(nullptr, guiRu ? L"\u0421\u0435\u0440\u0432\u0435\u0440 \u0438\u0437 \u044d\u0442\u043e\u0439 \u043f\u0430\u043f\u043a\u0438 \u0443\u0436\u0435 \u0437\u0430\u043f\u0443\u0449\u0435\u043d.\n\u0417\u0430\u043a\u0440\u043e\u0439 \u043f\u0435\u0440\u0432\u043e\u0435 \u043e\u043a\u043d\u043e \u0438\u043b\u0438 \u0437\u0430\u043f\u0443\u0441\u043a\u0430\u0439 \u043a\u043e\u043f\u0438\u044e \u0438\u0437 \u0434\u0440\u0443\u0433\u043e\u0439 \u043f\u0430\u043f\u043a\u0438."
                                   : L"This server folder is already running.\nClose the first window or start the copy from another folder.",
                        guiRu ? L"Zevvoryn \u2014 \u0443\u0436\u0435 \u0437\u0430\u043f\u0443\u0449\u0435\u043d" : L"Zevvoryn \u2014 already running", MB_OK | MB_ICONERROR | MB_TOPMOST);
            return 1;
        }
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

    // GUICON_V2: окно поднимаем ДО старта сервера — тогда в него попадает весь вывод:
    // шапка версии, баннер, загрузка мира. Если окно не создалось — остаёмся в classic.
    bool guiConsole = false;
    if (guiWanted) {
        if (!nc::console::isAvailable()) {
            NC_WARN("Main", guiRu ? "console-mode=gui \xd0\xbd\xd0\xb5 \xd0\xbf\xd0\xbe\xd0\xb4\xd0\xb4\xd0\xb5\xd1\x80\xd0\xb6\xd0\xb8\xd0\xb2\xd0\xb0\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f \xd0\xbd\xd0\xb0 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb9 \xd0\xbf\xd0\xbb\xd0\xb0\xd1\x82\xd1\x84\xd0\xbe\xd1\x80\xd0\xbc\xd0\xb5, \xd1\x80\xd0\xb0\xd0\xb1\xd0\xbe\xd1\x82\xd0\xb0\xd0\xb5\xd0\xbc \xd0\xb2 \xd0\xbe\xd0\xb1\xd1\x8b\xd1\x87\xd0\xbd\xd0\xbe\xd0\xb9 \xd0\xba\xd0\xbe\xd0\xbd\xd1\x81\xd0\xbe\xd0\xbb\xd0\xb8"
                                  : "console-mode=gui is not supported on this platform, staying in the classic console");
        } else {
            nc::console::GuiOptions guiOptions;
            guiOptions.title   = guiTitle;
            guiOptions.russian = guiRu;
            guiConsole = nc::console::start(
                guiOptions,
                [](const std::string& command) { if (g_server) g_server->queueConsoleCommand(command); },
                [] { // GUIWIZARD_V1: во время мастера сервера ещё нет — сохранять нечего, выходим сразу
                    if (g_wizardActive.load()) { std::_Exit(0); }
                    if (g_server) g_server->stop("closed");
                }); // крестик окна = честный сейв
            if (guiConsole) {
                // Приёмник сразу проигрывает всё, что успело напечататься до его регистрации.
                nc::log::setGuiSink([](int level, const char* tag, const char* text) {
                    nc::console::pushLine(static_cast<nc::console::LineLevel>(level),
                                          tag ? tag : "", text ? text : "");
                });
#ifdef _WIN32
                // Окно наше — чёрный квадрат conhost больше не нужен.
                if (HWND conhost = GetConsoleWindow()) ShowWindow(conhost, SW_HIDE);
#endif
                // GUIQUIET_V1: Windows Terminal спрятать нельзя, но можно перестать в него писать.
                nc::log::setConsoleQuiet(true);
                std::fflush(stdout);
#ifdef _WIN32
                std::freopen("NUL", "w", stdout);
#else
                std::freopen("/dev/null", "w", stdout);
#endif
#ifdef _WIN32
                // NOCONHOST_V1: как и во втором блоке ниже — полностью отвязываем процесс от
                // conhost (мы туда всё равно больше не пишем), иначе мелькает второе окно.
                FreeConsole();
#endif
                if (firstRun) {
                    // GUIWIZARD_V1: окно уже наше — переключаем ввод мастера в него, чтобы шапка
                    // версии, баннер и все вопросы шли туда, а не в classic-консоль.
                    g_wizardActive.store(true);
                    nc::console::enableWizardInput(true);
                    nc::log::setGuiReadLine(&wizardReadLineBridge);
                }
                NC_INFO("Main", guiRu ? "\xd0\x9e\xd0\xba\xd0\xbd\xd0\xbe-\xd1\x82\xd0\xb5\xd1\x80\xd0\xbc\xd0\xb8\xd0\xbd\xd0\xb0\xd0\xbb \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x89\xd0\xb5\xd0\xbd (console-mode=gui)"
                                      : "GUI console started (console-mode=gui)");
            } else {
#ifdef _WIN32
                // NOCONHOST_V1: gui не поднялся, а консоли ещё нет (guiWanted был true, поэтому выше мы её
                // не создавали) — поднимаем сейчас, иначе и мастер настройки, и classic-режим
                // останутся без ввода/вывода вообще.
                AllocConsole();
                FILE* nc_stdout2 = nullptr;
                FILE* nc_stderr2 = nullptr;
                FILE* nc_stdin2  = nullptr;
                freopen_s(&nc_stdout2, "CONOUT$", "w", stdout);
                freopen_s(&nc_stderr2, "CONOUT$", "w", stderr);
                freopen_s(&nc_stdin2,  "CONIN$",  "r", stdin);
#endif
                NC_WARN("Main", guiRu ? "\xd0\x9d\xd0\xb5 \xd1\x83\xd0\xb4\xd0\xb0\xd0\xbb\xd0\xbe\xd1\x81\xd1\x8c \xd1\x81\xd0\xbe\xd0\xb7\xd0\xb4\xd0\xb0\xd1\x82\xd1\x8c \xd0\xbe\xd0\xba\xd0\xbd\xd0\xbe-\xd1\x82\xd0\xb5\xd1\x80\xd0\xbc\xd0\xb8\xd0\xbd\xd0\xb0\xd0\xbb, \xd0\xbe\xd1\x81\xd1\x82\xd0\xb0\xd1\x91\xd0\xbc\xd1\x81\xd1\x8f \xd0\xb2 \xd0\xbe\xd0\xb1\xd1\x8b\xd1\x87\xd0\xbd\xd0\xbe\xd0\xb9 \xd0\xba\xd0\xbe\xd0\xbd\xd1\x81\xd0\xbe\xd0\xbb\xd0\xb8"
                                      : "Could not create the GUI console window, staying in the classic console");
                nc::log::enableBacklog(false);
            }
        }
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    nc::NetherCraftServer server;
    g_server = &server;
#ifdef _WIN32
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE); // CONCLOSE_V1: крестик = сохранить, потом закрыться
    // WINSAVE_V1: просим Windows гасить нас ПОСЛЕДНИМИ при выходе/перезагрузке,
    // чтобы успеть дописать мир (на Windows 11 окно закрывается агрессивнее).
    SetProcessShutdownParameters(0x100, SHUTDOWN_NORETRY);
#endif
    // GUIWIZARD_V1: мастер вызываем ровно один раз, чтобы потом обновить заголовок уже открытого окна.
    nc::ServerConfig wizardCfg{};
    if (firstRun) {
        wizardCfg = nc::setup::runWizard();
        if (guiConsole && g_wizardActive.load()) {
            // GUIWIZARD_V1: мастер прошёл внутри уже открытого окна — не пересоздаём его, просто
            // переключаемся обратно на обычный режим и обновляем заголовок под MOTD.
            g_wizardActive.store(false);
            nc::console::enableWizardInput(false);
            nc::log::setGuiReadLine(nullptr);
            guiRu = (wizardCfg.language == "rus");
            g_langRu.store(guiRu); // LANGSYNC_V1
            nc::console::setTitle(std::string("Zevvoryn \xe2\x80\x94 ") + wizardCfg.motd);
            // CLASSICNOW_V1: раньше выбор classic применялся только со следующего запуска,
            // потому что окно мастера уже открыто. Теперь сразу уходим в терминал Windows.
            bool needModeNote = (wizardCfg.consoleMode == "classic");
#ifdef _WIN32
            if (wizardCfg.consoleMode == "classic") {
                NC_INFO("Main", guiRu ? "Выбран classic — открываю системный терминал и продолжаю там"
                                      : "classic selected - opening the system terminal and continuing there");
                if (g_instanceMutex) {
                    ReleaseMutex(g_instanceMutex);
                    CloseHandle(g_instanceMutex);
                    g_instanceMutex = nullptr;
                }
                if (relaunchInSystemTerminal(argc, argv)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    std::_Exit(0); // сервер ещё не стартовал, сохранять нечего
                }
                // терминал не открылся — остаёмся в этом окне и честно об этом говорим
            }
#endif
            if (needModeNote) NC_INFO("Main", guiRu
                ? "\xd0\x92\xd1\x8b\xd0\xb1\xd0\xbe\xd1\x80 classic/gui \xd0\xb8\xd0\xb7 \xd0\xbc\xd0\xb0\xd1\x81\xd1\x82\xd0\xb5\xd1\x80\xd0\xb0 \xd0\xbf\xd1\x80\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x82\xd1\x81\xd1\x8f \xd1\x81\xd0\xbe \xd1\x81\xd0\xbb\xd0\xb5\xd0\xb4\xd1\x83\xd1\x8e\xd1\x89\xd0\xb5\xd0\xb3\xd0\xbe \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba\xd0\xb0, \xd1\x8d\xd1\x82\xd0\xb0 \xd1\x81\xd0\xb5\xd1\x81\xd1\x81\xd0\xb8\xd1\x8f \xd0\xbe\xd1\x81\xd1\x82\xd0\xb0\xd1\x91\xd1\x82\xd1\x81\xd1\x8f \xd0\xb2 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xbc \xd0\xbe\xd0\xba\xd0\xbd\xd0\xb5"
                : "The classic/gui choice from the wizard takes effect from the next launch; this session stays in this window");
        }
    }
    const bool started = firstRun
        ? server.startWithConfig(wizardCfg)
        : server.start(configPath);
    if (!started) { pauseBeforeExit(guiConsole); return 1; } // EXITPAUSE_V1, NOCONHOST_V1

    if (server.getConfig().autoStartPanel) spawnPanelProcess(server.getConfig()); // AUTOSTARTPANEL_V1 / RCONENV_V1

    // GUICON_V2: конфиг мог быть только что создан мастером — тогда окно поднимаем сейчас.
    if (!guiConsole && server.getConfig().consoleMode == "gui" && nc::console::isAvailable()) {
        const bool ruGui = server.getConfig().language == "rus";
        nc::console::GuiOptions guiOptions;
        guiOptions.title   = std::string("Zevvoryn \xe2\x80\x94 ") + server.getConfig().motd;
        guiOptions.russian = ruGui;
        guiConsole = nc::console::start(
            guiOptions,
            [](const std::string& command) { if (g_server) g_server->queueConsoleCommand(command); },
            [] { if (g_server) g_server->stop("closed"); });
        if (guiConsole) {
            nc::log::setGuiSink([](int level, const char* tag, const char* text) {
                nc::console::pushLine(static_cast<nc::console::LineLevel>(level),
                                      tag ? tag : "", text ? text : "");
            });
#ifdef _WIN32
            if (HWND conhost = GetConsoleWindow()) ShowWindow(conhost, SW_HIDE);
#endif
            nc::log::setConsoleQuiet(true); // GUIQUIET_V1
            std::fflush(stdout);
#ifdef _WIN32
            std::freopen("NUL", "w", stdout);
#else
            std::freopen("/dev/null", "w", stdout);
#endif
#ifdef _WIN32
            // NOCONHOST_V1: мы уже ничего не пишем в conhost (stdout ушёл в NUL), поэтому отвязываем
            // процесс от консоли совсем — окно не просто скрыто, а фактически уничтожено.
            // Полностью исключить краткое появление окна при старте может только /SUBSYSTEM:WINDOWS в сборке (нужен CMakeLists.txt).
            FreeConsole();
#endif
            NC_INFO("Main", ruGui ? "\xd0\x9e\xd0\xba\xd0\xbd\xd0\xbe-\xd1\x82\xd0\xb5\xd1\x80\xd0\xbc\xd0\xb8\xd0\xbd\xd0\xb0\xd0\xbb \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x89\xd0\xb5\xd0\xbd (console-mode=gui)"
                                  : "GUI console started (console-mode=gui)");
        }
    }

    // CONSOLE_V2: server simulation owns world state; console lines are queued for its tick.
    std::thread serverThread([&server] { server.run(); });
    // CONSTOP_V1: раньше main блокировался в getline и после /stop из игры окно
    // «висело». Теперь консоль читает detached-поток, а main ждёт серверный
    // поток и закрывает процесс сразу при любом способе остановки.
    std::thread consoleThread([&server] {
        // STDINGUARD_V1: с /SUBSYSTEM:WINDOWS в gui-режиме мы больше не вызываем AllocConsole вовсе —
        // у процесса нет валидного stdin. Раньше (под CONSOLE-подсистемой) Windows всё
        // равно создавала реальную консоль, и std::getline просто вечно блокировался.
        // Теперь же он немедленно возвращает EOF, и цикл ниже тут же считал это выходом и
        // сам себе ставил "stop" сразу после старта — поэтому сервер тут же останавливался.
        // В gui-режиме команды и так идут через свой коллбэк nc::console::start(...), поэтому этому
        // потоку тут вообще нечего читать — просто ждём остановки сервера.
        if (nc::console::isRunning()) {
            while (server.isRunning()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            return;
        }
        // GUIBACKLOG_V1: в GUI-режиме у окна своя строка ввода — приглашение туда не дублируем.
        // LANGFIX_V1: раньше эта строка была зашита только по-английски, хотя язык выбирался русский.
        std::cout << (g_langRu.load()
                ? "\xd0\x9a\xd0\xbe\xd0\xbd\xd1\x81\xd0\xbe\xd0\xbb\xd1\x8c \xd0\xb3\xd0\xbe\xd1\x82\xd0\xbe\xd0\xb2\xd0\xb0. \xd0\x92\xd0\xb2\xd0\xb5\xd0\xb4\xd0\xb8\xd1\x82\xd0\xb5 help \xd0\xb4\xd0\xbb\xd1\x8f \xd1\x81\xd0\xbf\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0 \xd0\xba\xd0\xbe\xd0\xbc\xd0\xb0\xd0\xbd\xd0\xb4.\n> "
                : "Console ready. Type help for commands.\n> ") << std::flush;
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
    if (guiConsole) { // GUICON_V1: сначала отцепляем приёмник, потом гасим окно
        nc::log::setGuiSink(nullptr);
        nc::console::stop();
    }
    stopPanelProcess(); // AUTOSTARTPANEL_V1
    g_server = nullptr;
    return 0;
}
