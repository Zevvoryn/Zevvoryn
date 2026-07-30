// i18n.hpp — I18N_V1
//
// Tiny translation table for command replies. The language is chosen per
// recipient: for players from the locale they announce in ClientInformation
// ("ru_ru", "en_us", ...), for console/RCON from `language=` in
// settings.properties. English is the default for everything unknown.
//
// Adding a language later = adding one column to Entry + one case in pick().
#pragma once

#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace nc::i18n {

enum class Lang { En, Ru };

// "ru_ru", "ru-RU", "rus" -> Ru; everything else -> En.
inline Lang langFromLocale(std::string_view locale) {
    return (locale.size() >= 2 && (locale[0] == 'r' || locale[0] == 'R') && (locale[1] == 'u' || locale[1] == 'U'))
               ? Lang::Ru
               : Lang::En;
}

struct Entry {
    const char* en;
    const char* ru;
};

inline const std::unordered_map<std::string_view, Entry>& table() {
    static const std::unordered_map<std::string_view, Entry> kTable = {
        // generic
        {"player.notfound", {"\u00a7cPlayer {} not found", "\u00a7c\u0418\u0433\u0440\u043e\u043a {} \u043d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d"}},
        {"arg.number", {"\u00a7cThat argument must be a number", "\u00a7c\u042d\u0442\u043e\u0442 \u0430\u0440\u0433\u0443\u043c\u0435\u043d\u0442 \u2014 \u0447\u0438\u0441\u043b\u043e"}},

        // /kill
        {"kill.usage", {"\u00a7cUsage: /kill <player>", "\u00a7c\u0418\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u043d\u0438\u0435: /kill <\u0438\u0433\u0440\u043e\u043a>"}},
        {"kill.death", {"{} was killed by a command", "{} \u0431\u044b\u043b \u0443\u0431\u0438\u0442 \u043a\u043e\u043c\u0430\u043d\u0434\u043e\u0439"}},
        {"kill.ok", {"\u00a7aKilled: {}", "\u00a7a\u0423\u0431\u0438\u0442: {}"}},

        // /clear
        {"clear.usage", {"\u00a7cUsage: /clear <player>", "\u00a7c\u0418\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u043d\u0438\u0435: /clear <\u0438\u0433\u0440\u043e\u043a>"}},
        {"clear.ok", {"\u00a7aCleared inventory of {} ({} items)", "\u00a7a\u0418\u043d\u0432\u0435\u043d\u0442\u0430\u0440\u044c {} \u043e\u0447\u0438\u0449\u0435\u043d (\u043f\u0440\u0435\u0434\u043c\u0435\u0442\u043e\u0432: {})"}},

        // /effect
        {"effect.usage", {"\u00a7cUsage: /effect give <player> <id> [sec] [amp] | /effect clear <player> [id]",
                          "\u00a7c\u0418\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u043d\u0438\u0435: /effect give <\u0438\u0433\u0440\u043e\u043a> <id> [\u0441\u0435\u043a] [\u0443\u0440\u043e\u0432\u0435\u043d\u044c] | /effect clear <\u0438\u0433\u0440\u043e\u043a> [id]"}},
        {"effect.usage.give", {"\u00a7cUsage: /effect give <player> <id> [sec] [amp]",
                               "\u00a7c\u0418\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u043d\u0438\u0435: /effect give <\u0438\u0433\u0440\u043e\u043a> <id> [\u0441\u0435\u043a] [\u0443\u0440\u043e\u0432\u0435\u043d\u044c]"}},
        {"effect.badid", {"\u00a7cInvalid effect id", "\u00a7c\u041d\u0435\u0432\u0435\u0440\u043d\u044b\u0439 id \u044d\u0444\u0444\u0435\u043a\u0442\u0430"}},
        {"effect.given", {"\u00a7aEffect {} (level {}) for {}s \u2192 {}",
                          "\u00a7a\u042d\u0444\u0444\u0435\u043a\u0442 {} (\u0443\u0440\u043e\u0432\u0435\u043d\u044c {}) \u043d\u0430 {} \u0441\u0435\u043a \u2192 {}"}},
        {"effect.removed", {"\u00a7aRemoved effect {} from {}", "\u00a7a\u042d\u0444\u0444\u0435\u043a\u0442 {} \u0441\u043d\u044f\u0442 \u0441 {}"}},
        {"effect.removed.all", {"\u00a7aRemoved {} effect(s) from {}", "\u00a7a\u0421\u043d\u044f\u0442\u043e \u044d\u0444\u0444\u0435\u043a\u0442\u043e\u0432: {} \u0443 {}"}},

        // /xp
        {"xp.usage", {"\u00a7cUsage: /xp add|set <player> <amount> [levels]",
                      "\u00a7c\u0418\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u043d\u0438\u0435: /xp add|set <\u0438\u0433\u0440\u043e\u043a> <\u043a\u043e\u043b\u0438\u0447\u0435\u0441\u0442\u0432\u043e> [levels]"}},
        {"xp.ok", {"\u00a7a{}: level {}, total XP {}", "\u00a7a{}: \u0443\u0440\u043e\u0432\u0435\u043d\u044c {}, \u0432\u0441\u0435\u0433\u043e \u043e\u043f\u044b\u0442\u0430 {}"}},

        // /msg /tell /w /me
        {"msg.usage", {"\u00a7cUsage: /msg <player> <text>", "\u00a7c\u0418\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u043d\u0438\u0435: /msg <\u0438\u0433\u0440\u043e\u043a> <\u0442\u0435\u043a\u0441\u0442>"}},
        {"msg.in", {"\u00a7d[{} \u2192 you] \u00a7f{}", "\u00a7d[{} \u2192 \u0432\u0430\u043c] \u00a7f{}"}},
        {"msg.out", {"\u00a7d[you \u2192 {}] \u00a7f{}", "\u00a7d[\u0432\u044b \u2192 {}] \u00a7f{}"}},
        {"me.usage", {"\u00a7cUsage: /me <action>", "\u00a7c\u0418\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u043d\u0438\u0435: /me <\u0434\u0435\u0439\u0441\u0442\u0432\u0438\u0435>"}},

        // /seed /difficulty
        {"seed.ok", {"\u00a7aWorld seed: {}", "\u00a7a\u0421\u0438\u0434 \u043c\u0438\u0440\u0430: {}"}},
        {"diff.current", {"\u00a7eDifficulty: {} ({})", "\u00a7e\u0421\u043b\u043e\u0436\u043d\u043e\u0441\u0442\u044c: {} ({})"}},
        {"diff.usage", {"\u00a7cUsage: /difficulty <peaceful|easy|normal|hard>",
                        "\u00a7c\u0418\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u043d\u0438\u0435: /difficulty <peaceful|easy|normal|hard>"}},
        {"diff.set", {"\u00a7aDifficulty: {}", "\u00a7a\u0421\u043b\u043e\u0436\u043d\u043e\u0441\u0442\u044c: {}"}},

        // /ban /pardon /banlist
        {"ban.usage", {"\u00a7cUsage: /ban <player> [reason]", "\u00a7c\u0418\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u043d\u0438\u0435: /ban <\u0438\u0433\u0440\u043e\u043a> [\u043f\u0440\u0438\u0447\u0438\u043d\u0430]"}},
        {"ban.already", {"\u00a7e{} is already banned", "\u00a7e{} \u0443\u0436\u0435 \u0437\u0430\u0431\u0430\u043d\u0435\u043d"}},
        {"ban.ok", {"\u00a7a{} has been banned{}", "\u00a7a{} \u0437\u0430\u0431\u0430\u043d\u0435\u043d{}"}},
        {"ban.kick", {"You are banned from this server", "\u0412\u044b \u0437\u0430\u0431\u0430\u043d\u0435\u043d\u044b \u043d\u0430 \u044d\u0442\u043e\u043c \u0441\u0435\u0440\u0432\u0435\u0440\u0435"}},
        {"ban.kick.reason", {"You are banned: {}", "\u0412\u044b \u0437\u0430\u0431\u0430\u043d\u0435\u043d\u044b: {}"}},
        {"pardon.usage", {"\u00a7cUsage: /pardon <player>", "\u00a7c\u0418\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u043d\u0438\u0435: /pardon <\u0438\u0433\u0440\u043e\u043a>"}},
        {"pardon.ok", {"\u00a7a{} has been unbanned", "\u00a7a{} \u0440\u0430\u0437\u0431\u0430\u043d\u0435\u043d"}},
        {"pardon.not", {"\u00a7e{} is not banned", "\u00a7e{} \u0438 \u0442\u0430\u043a \u043d\u0435 \u0432 \u0431\u0430\u043d\u0435"}},
        {"banlist.empty", {"\u00a7eBan list is empty", "\u00a7e\u0411\u0430\u043d-\u043b\u0438\u0441\u0442 \u043f\u0443\u0441\u0442"}},
        {"banlist.head", {"\u00a76Banned players: {} \u2014 ", "\u00a76\u0417\u0430\u0431\u0430\u043d\u0435\u043d\u043e: {} \u2014 "}},

        // /tp
        {"tp.usage", {"\u00a7cUsage: /tp <player> <x y z> or /tp <player> <target>",
                      "\u00a7c\u0418\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u043d\u0438\u0435: /tp <\u0438\u0433\u0440\u043e\u043a> <x y z> \u0438\u043b\u0438 /tp <\u0438\u0433\u0440\u043e\u043a> <\u0446\u0435\u043b\u044c>"}},
        {"tp.oob", {"\u00a7cOut of bounds: X/Z \u00b129999984, Y -63..319",
                    "\u00a7c\u0412\u043d\u0435 \u0433\u0440\u0430\u043d\u0438\u0446: X/Z \u00b129999984, Y -63..319"}},
        {"tp.nan", {"\u00a7cCoordinates must be numbers", "\u00a7c\u041a\u043e\u043e\u0440\u0434\u0438\u043d\u0430\u0442\u044b \u2014 \u0447\u0438\u0441\u043b\u0430"}},
        {"tp.coords", {"\u00a7a{} \u2192 {:.1f} {:.1f} {:.1f}", "\u00a7a{} \u2192 {:.1f} {:.1f} {:.1f}"}},
        {"tp.player", {"\u00a7a{} \u2192 {}", "\u00a7a{} \u2192 {}"}},

        // /commands
        {"cmds.head", {"\u00a76Commands in tree: {} \u2014 ", "\u00a76\u041a\u043e\u043c\u0430\u043d\u0434 \u0432 \u0434\u0435\u0440\u0435\u0432\u0435: {} \u2014 "}},
    };
    return kTable;
}

// Returns the pattern for `key` in `lang`; falls back to English, then to the
// key itself so a missing translation is visible instead of crashing.
inline std::string_view tr(Lang lang, std::string_view key) {
    const auto& t = table();
    const auto it = t.find(key);
    if (it == t.end()) return key;
    const char* s = (lang == Lang::Ru) ? it->second.ru : it->second.en;
    if (!s || !*s) s = it->second.en;
    return s;
}

// Formats a translated pattern: f(lang, "kill.ok", name)
template <class... Args>
inline std::string f(Lang lang, std::string_view key, Args&&... args) {
    try {
        return std::vformat(tr(lang, key), std::make_format_args(args...));
    } catch (...) {
        return std::string(tr(lang, key));
    }
}

} // namespace nc::i18n
