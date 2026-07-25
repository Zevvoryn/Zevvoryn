#pragma once
// PLUGINCMD_V1: single source of truth for commands that are not part of the
// original hand-built command set.
//
// Problem this solves: previously, adding one new command required editing
// THREE separate places by hand (server console dispatcher, in-game chat
// dispatcher, and the raw byte-level Commands packet / 0x11 sent to clients).
// That does not scale to a plugin system — a donation plugin (or any other
// plugin) cannot ship a new command if every single one has to be wired into
// core/server.cpp by hand first.
//
// With this registry, a plugin (or any future core code) calls
// registerCommand() ONCE, and the command is automatically:
//   1) recognized by the server console (no "Unknown command" for it anymore)
//   2) recognized by the in-game chat command dispatcher as a fallback, when
//      it doesn't match one of the built-in `cmd == "..."` branches
//   3) advertised to every connected client inside the Commands packet
//      (wire id 0x11), so the client stops drawing a red "unknown command"
//      squiggle under it
// No other file needs to change again when a new simple command is added.
//
// Example — how a future plugin registers a command:
//
//   nc::cmd::CommandRegistry::instance().registerCommand({
//       .name = "donate",
//       .opOnly = false,
//       .source = "plugin:donate",
//       .usage = "/donate <package>",
//       .handler = [](nc::cmd::CommandContext& ctx) {
//           ctx.reply("§aСпасибо за донат! Пакет: " + (ctx.args.size() > 1 ? ctx.args[1] : "?"));
//       }
//   });
//
// That single call is enough — the plugin does not touch server.cpp at all.

#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <mutex>

namespace nc::cmd {

// Context passed to a registered command's handler. Works the same whether
// the command was typed in-game or into the server console.
struct CommandContext {
    std::string playerName;   // empty when run from the console
    bool isConsole = false;
    bool isOp = false;        // true for console (owner) and op players
    std::vector<std::string> args; // args[0] == the command name itself
    // Sends a reply back to wherever the command came from (in-game chat
    // message for players, NC_INFO console line for the console).
    std::function<void(const std::string&)> reply;
};

using CommandHandler = std::function<void(CommandContext&)>;

struct CommandDef {
    std::string name;
    bool opOnly = true;
    std::string source = "core"; // "core" or "plugin:<name>"
    std::string usage;
    // May be null for names registered only so they appear in the console /
    // Commands-packet tree, because their real logic already lives in an
    // existing hand-written `cmd == "..."` branch (e.g. legacy core commands).
    CommandHandler handler;
};

class CommandRegistry {
public:
    static CommandRegistry& instance() {
        static CommandRegistry inst;
        return inst;
    }

    void registerCommand(CommandDef def) {
        std::lock_guard<std::mutex> lk(mutex_);
        order_.push_back(def.name);
        commands_[def.name] = std::move(def);
    }

    // Returns nullptr if not registered.
    const CommandDef* find(const std::string& name) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = commands_.find(name);
        return it == commands_.end() ? nullptr : &it->second;
    }

    // All registered commands, in registration order (stable so the Commands
    // packet node indices stay stable across a run).
    std::vector<CommandDef> all() {
        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<CommandDef> out;
        out.reserve(order_.size());
        for (auto& name : order_) {
            auto it = commands_.find(name);
            if (it != commands_.end()) out.push_back(it->second);
        }
        return out;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> order_;
    std::unordered_map<std::string, CommandDef> commands_;
};

} // namespace nc::cmd
