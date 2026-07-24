#pragma once
// CRASHCTX_V1: lightweight "what was running when we crashed" tracker.
//
// Every connection is handled on its own thread (see network/connection.cpp),
// so this is thread_local: each thread keeps track of only what IT was doing.
// When a fatal signal / abort() / unhandled exception fires, the handler runs
// on the SAME thread that caused it, so reading this thread_local at crash
// time reliably reports the right command/player/source — not some unrelated
// thread's last action.
//
// `source` is forward-looking for plugin support: today everything is
// "core" (there are no plugins yet). Once a plugin API exists, plugin-invoked
// code should call setCrashContext("plugin:<name>", ...) so crash reports can
// tell core bugs apart from plugin bugs.

#include <string>

namespace nc {

struct CrashContext {
    std::string source;   // "core", or later "plugin:<name>"
    std::string action;   // e.g. "/crash", "chat message", "packet 0x0B"
    std::string player;   // player name, if applicable
};

inline thread_local CrashContext g_crashContext;

inline void setCrashContext(const std::string& source, const std::string& action, const std::string& player = "") {
    g_crashContext.source = source;
    g_crashContext.action = action;
    g_crashContext.player = player;
}

inline void clearCrashContext() {
    g_crashContext.action.clear();
    g_crashContext.player.clear();
}

// Human-readable lines for the crash report. Returns false via `hasAction`
// if this thread had no tracked action (e.g. a background/worldgen thread).
inline bool describeCrashContext(std::string& outAction, std::string& outPlayer, std::string& outSource) {
    if (g_crashContext.action.empty()) return false;
    outAction = g_crashContext.action;
    outPlayer = g_crashContext.player;
    outSource = g_crashContext.source.empty() ? "core" : g_crashContext.source;
    return true;
}

} // namespace nc
