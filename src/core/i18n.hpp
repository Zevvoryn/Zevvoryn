// i18n.hpp — I18N_V1
//
// Tiny translation table for command replies. The language is chosen per
// recipient: for players from the locale they announce in ClientInformation
// ("ru_ru", "en_us", ...), for console/RCON from `language=` in
// settings.properties. English is the default for everything unknown.
//
// Adding a language later = adding one column to Entry + one case in pick().
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nc::i18n {

// LANGFILE_V1: Lang used to be a two-value enum. It is now a handle to a
// language pack: 0 = eng, 1 = rus (both built in), 2+ = extra packs loaded
// from lang/*.json. Lang::En / Lang::Ru keep every old call site working.
struct Lang {
    int id = 0;
    constexpr Lang() = default;
    constexpr explicit Lang(int v) : id(v) {}
    static const Lang En;
    static const Lang Ru;
    friend constexpr bool operator==(Lang a, Lang b) { return a.id == b.id; }
    friend constexpr bool operator!=(Lang a, Lang b) { return a.id != b.id; }
};
inline const Lang Lang::En{0};
inline const Lang Lang::Ru{1};

inline Lang langFromLocale(std::string_view locale);
inline Lang langFromCode(std::string_view code);

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

        // /spawn, /setspawn — SPAWNCMD_V1
        {"spawn.tp", {"\u00a7aTeleported to world spawn", "\u00a7aТелепорт на спавн"}},
        {"spawn.set", {"\u00a7aWorld spawn set: {} {} {}", "\u00a7aМировой спавн установлен: {} {} {}"}},
        {"spawn.set.usage", {"\u00a7eUsage: /setspawn [x y z] — without arguments uses your position",
                              "\u00a7eИспользование: /setspawn [x y z] — без аргументов берётся твоя позиция"}},
        {"spawn.int", {"\u00a7cCoordinates must be whole numbers", "\u00a7cКоординаты — целые числа"}},

        // /spawn warmup — SPAWNCFG_V1
        {"spawn.disabled", {"\u00a7cThe /spawn command is disabled on this server",
                            "\u00a7cКоманда /spawn отключена на этом сервере"}},
        {"spawn.countdown", {"Teleporting to spawn in {}s", "Телепорт на спавн через {} с"}},
        {"spawn.cancelled", {"\u00a7cYou moved — teleport cancelled", "\u00a7cТы сдвинулся — телепорт отменён"}},
        {"spawn.already", {"\u00a7eTeleport is already counting down", "\u00a7eОтсчёт телепорта уже идёт"}},

        // /commands
        {"cmds.head", {"\u00a76Commands in tree: {} \u2014 ", "\u00a76\u041a\u043e\u043c\u0430\u043d\u0434 \u0432 \u0434\u0435\u0440\u0435\u0432\u0435: {} \u2014 "}},
    };
    return kTable;
}

// ===================================================================
// LANGFILE_V1: language packs in lang/*.json
//
// lang/eng.json and lang/rus.json are written on the first start from the
// built-in table above and re-read on every start, so anybody can edit the
// wording without recompiling. Dropping another file next to them, e.g.
// lang/deu.json, adds a third language: players whose client locale starts
// with "de" will get it. Anything with no matching pack falls back to eng.
// ===================================================================

struct LangPack {
    std::string code;    // file stem: "eng", "rus", "deu"
    std::string prefix;  // locale prefix: "en", "ru", "de"
    std::map<std::string, std::string> strings;
};

inline std::vector<LangPack>& packs() {
    static std::vector<LangPack> v = [] {
        std::vector<LangPack> init;
        LangPack en; en.code = "eng"; en.prefix = "en";
        LangPack ru; ru.code = "rus"; ru.prefix = "ru";
        for (const auto& [k, e] : table()) {
            en.strings[std::string(k)] = e.en ? e.en : "";
            ru.strings[std::string(k)] = (e.ru && *e.ru) ? e.ru : (e.en ? e.en : "");
        }
        init.push_back(std::move(en));
        init.push_back(std::move(ru));
        return init;
    }();
    return v;
}

// "ru_ru", "ru-RU", "rus" -> the rus pack; "de_de" -> lang/deu.json if it
// exists; everything else -> eng. English is the universal fallback.
inline Lang langFromLocale(std::string_view locale) {
    if (locale.size() < 2) return Lang::En;
    const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(locale[0])));
    const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(locale[1])));
    const auto& v = packs();
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].prefix.size() == 2 && v[i].prefix[0] == a && v[i].prefix[1] == b)
            return Lang(static_cast<int>(i));
    return Lang::En;
}

// "rus" / "eng" / "deu" from settings.properties -> pack; unknown -> eng.
inline Lang langFromCode(std::string_view code) {
    std::string lc(code);
    std::transform(lc.begin(), lc.end(), lc.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto& v = packs();
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].code == lc) return Lang(static_cast<int>(i));
    return langFromLocale(lc);
}

inline std::string_view langCode(Lang lang) {
    const auto& v = packs();
    const size_t i = static_cast<size_t>(lang.id);
    return i < v.size() ? std::string_view(v[i].code) : std::string_view("eng");
}

// ---- minimal flat JSON (object of string -> string) -----------------

namespace detail {

inline std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c; // UTF-8 passes through untouched
                }
        }
    }
    return out;
}

inline void appendUtf8(std::string& out, unsigned int cp) {
    if (cp < 0x80) { out += static_cast<char>(cp); }
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

inline bool jsonString(const std::string& t, size_t& i, std::string& out) {
    while (i < t.size() && t[i] != '\"') ++i;
    if (i >= t.size()) return false;
    ++i;
    out.clear();
    while (i < t.size()) {
        const char c = t[i++];
        if (c == '\"') return true;
        if (c != '\\') { out += c; continue; }
        if (i >= t.size()) return false;
        const char e = t[i++];
        switch (e) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (i + 4 > t.size()) return false;
                unsigned int cp = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = t[i + static_cast<size_t>(k)];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<unsigned int>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned int>(h - 'A' + 10);
                    else return false;
                }
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                    unsigned int lo = 0;
                    bool okLow = true;
                    for (int k = 0; k < 4; ++k) {
                        const char h = t[i + 2 + static_cast<size_t>(k)];
                        lo <<= 4;
                        if (h >= '0' && h <= '9') lo |= static_cast<unsigned int>(h - '0');
                        else if (h >= 'a' && h <= 'f') lo |= static_cast<unsigned int>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') lo |= static_cast<unsigned int>(h - 'A' + 10);
                        else { okLow = false; break; }
                    }
                    if (okLow && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                appendUtf8(out, cp);
                break;
            }
            default: out += e; break;
        }
    }
    return false;
}

inline std::map<std::string, std::string> parseFlatJson(const std::string& text) {
    std::map<std::string, std::string> out;
    size_t i = text.find('{');
    if (i == std::string::npos) return out;
    ++i;
    while (i < text.size()) {
        while (i < text.size() && text[i] != '\"' && text[i] != '}') ++i;
        if (i >= text.size() || text[i] == '}') break;
        std::string key, val;
        if (!jsonString(text, i, key)) break;
        while (i < text.size() && text[i] != ':') ++i;
        if (i >= text.size()) break;
        ++i;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
        if (i >= text.size() || text[i] != '\"') { // skip non-string values
            while (i < text.size() && text[i] != ',' && text[i] != '}') ++i;
            if (i < text.size() && text[i] == ',') ++i;
            continue;
        }
        if (!jsonString(text, i, val)) break;
        out[key] = val;
    }
    return out;
}

inline void writePack(const std::filesystem::path& file, const LangPack& pack, const char* header) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << "{\n";
    out << "  \"__comment__\": \"" << header << "\",\n";
    out << "  \"__locale__\": \"" << pack.prefix << "\"";
    for (const auto& [k, v] : pack.strings)
        out << ",\n  \"" << jsonEscape(k) << "\": \"" << jsonEscape(v) << "\"";
    out << "\n}\n";
}

inline void writeHelpFile(const std::filesystem::path& file) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return;
    struct HelpLine { const char* key; const char* text; };
    static const HelpLine kHelp[] = {
        {"en_01_what_is_this",
         "This folder holds every text the server sends. eng.json is English, rus.json is Russian. "
         "Help.json is this manual and is never loaded as a language."},
        {"en_02_how_to_edit",
         "Open a pack in any editor, change the text on the right side of the colon, save it as UTF-8 "
         "without BOM and restart the server. Rebuilding the exe is not needed."},
        {"en_03_placeholders",
         "Curly braces are values the server fills in. The first pair takes the first value, the second "
         "pair the second one. Keep them and keep their order, otherwise the line is shown as is."},
        {"en_04_colors",
         "The section sign starts a colour: c red, a green, e yellow, 6 gold, d pink, f white."},
        {"en_05_empty_value",
         "An empty value means use the English text. Clear the value, not the key, for lines you do not "
         "want to translate."},
        {"en_06_new_keys",
         "After a server update new keys are appended to your files automatically and your own wording "
         "is kept."},
        {"en_07_new_language",
         "To add a language copy eng.json to a new file, for example deu.json, set __locale__ to the two "
         "letter client locale (de), translate what you need and restart. A partial file is fine."},
        {"en_08_locale_match",
         "A player gets the pack whose __locale__ matches the first two letters of the client language, "
         "for example ru_ru or de_de. If nothing matches the player gets English."},
        {"en_09_console",
         "The console and RCON use language= from settings.properties (rus or eng) or the name of any "
         "pack you add."},
        {"en_10_broken_file",
         "A broken file is skipped with a warning at startup and the built-in text is used, so a typo "
         "can never stop the server. Delete the file to get a fresh copy."},

        {"ru_01_chto_eto",
         "В этой папке лежит весь текст сервера. eng.json — английский, rus.json — русский. "
         "Help.json — эта инструкция, как язык он не загружается."},
        {"ru_02_kak_menyat",
         "Открой файл любым редактором, меняй текст справа от двоеточия, сохрани в UTF-8 "
         "без BOM и перезапусти сервер. Пересборка не нужна."},
        {"ru_03_skobki",
         "Фигурные скобки — это значения, которые подставляет сервер. Первая пара — "
         "первое значение, вторая — второе. Сохраняй их и порядок, иначе строка выведется как есть."},
        {"ru_04_cveta",
         "Знак параграфа задаёт цвет: c красный, a зелёный, e жёлтый, 6 золотой, d розовый, f белый."},
        {"ru_05_pustoe_znachenie",
         "Пустое значение означает взять английский текст. Удаляй значение, а не ключ."},
        {"ru_06_novye_klyuchi",
         "После обновления сервера новые ключи дописываются сами, твои правки не теряются."},
        {"ru_07_novyj_yazyk",
         "Чтобы добавить язык, скопируй eng.json в новый файл, например deu.json, "
         "поставь в __locale__ две буквы локали (de), переведи нужное и перезапусти. "
         "Неполный файл тоже работает."},
        {"ru_08_kak_vybiraetsya",
         "Игрок получает пак, у которого __locale__ совпадает с первыми двумя буквами "
         "языка клиента (ru_ru, de_de). Если совпадений нет — будет английский."},
        {"ru_09_konsol",
         "Консоль и RCON берут язык из language= в settings.properties (rus или eng) "
         "или имя любого добавленного пака."},
        {"ru_10_bityj_fajl",
         "Битый файл пропускается с предупреждением при старте, берётся встроенный текст "
         "— сервер опечаткой не убьёшь. Удали файл, чтобы получить свежую копию."},
    };
    out << "{\n";
    for (size_t i = 0; i < std::size(kHelp); ++i)
        out << "  \"" << kHelp[i].key << "\": \"" << jsonEscape(kHelp[i].text) << "\""
            << (i + 1 < std::size(kHelp) ? ",\n" : "\n");
    out << "}\n";
}

} // namespace detail

// LANGCHECK_V1: what happened while loading, so the server can report it.
struct LangLoadReport {
    std::vector<std::string> codes;    // packs that are usable now
    std::vector<std::string> invalid;  // file names that were skipped
};

// Creates lang/, writes Help.json and the two built-in packs when they are
// missing, merges new keys into existing files, and picks up any extra
// lang/<code>.json. Broken files are skipped instead of killing the start.
inline LangLoadReport loadLangPacks(const std::filesystem::path& dir = "lang") {
    LangLoadReport report;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);

    auto readFile = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };

    // built-in packs: create or merge
    for (size_t idx = 0; idx < 2; ++idx) {
        LangPack& pack = packs()[idx];
        const fs::path file = dir / (pack.code + ".json");
        const bool exists = fs::exists(file, ec);
        bool needWrite = !exists;
        if (exists) {
            const auto disk = detail::parseFlatJson(readFile(file));
            if (disk.empty()) { // LANGCHECK_V1: unreadable or empty -> rewrite from built-ins
                report.invalid.push_back(file.filename().string());
                needWrite = true;
            }
            for (const auto& [k, v] : disk) {
                if (k.rfind("__", 0) == 0) continue;
                if (v.empty()) continue;
                pack.strings[k] = v;
            }
            for (const auto& kv : pack.strings)
                if (disk.find(kv.first) == disk.end()) needWrite = true; // new keys after an update
        }
        if (needWrite)
            detail::writePack(file, pack,
                              idx == 0 ? "Zevvoryn language pack. Edit the values, keep {} placeholders in place."
                                       : "Zevvoryn language pack. Edit the values, keep {} placeholders in place.");
    }

    { // LANGFILE_V1: short manual right next to the packs
        const fs::path help = dir / "Help.json";
        if (!fs::exists(help, ec)) detail::writeHelpFile(help);
    }

    // extra packs: any other *.json in lang/
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".json") continue;
        const std::string stem = e.path().stem().string();
        std::string stemLc = stem;
        std::transform(stemLc.begin(), stemLc.end(), stemLc.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (stemLc == "eng" || stemLc == "rus" || stemLc == "help") continue;
        const auto disk = detail::parseFlatJson(readFile(e.path()));
        if (disk.empty()) { // LANGCHECK_V1: broken json / no string values
            report.invalid.push_back(e.path().filename().string());
            continue;
        }
        LangPack pack;
        pack.code = stem;
        const auto loc = disk.find("__locale__");
        pack.prefix = (loc != disk.end() && loc->second.size() >= 2) ? loc->second.substr(0, 2)
                                                                    : stem.substr(0, 2);
        std::transform(pack.prefix.begin(), pack.prefix.end(), pack.prefix.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        pack.strings = packs()[0].strings; // English base, so a partial file still works
        for (const auto& [k, v] : disk) {
            if (k.rfind("__", 0) == 0) continue;
            if (!v.empty()) pack.strings[k] = v;
        }
        bool dup = false;
        for (const auto& p : packs()) if (p.code == pack.code) dup = true;
        if (!dup) packs().push_back(std::move(pack));
    }

    for (const auto& p : packs()) report.codes.push_back(p.code);
    return report;
}

// Returns the pattern for `key` in `lang`; falls back to English, then to the
// key itself so a missing translation is visible instead of crashing.
inline std::string_view tr(Lang lang, std::string_view key) {
    const auto& v = packs();
    const std::string k(key);
    const size_t i = static_cast<size_t>(lang.id);
    if (i < v.size()) {
        const auto it = v[i].strings.find(k);
        if (it != v[i].strings.end() && !it->second.empty()) return it->second;
    }
    if (!v.empty()) {
        const auto it = v[0].strings.find(k);
        if (it != v[0].strings.end() && !it->second.empty()) return it->second;
    }
    const auto& t = table();
    const auto bt = t.find(key);
    if (bt == t.end()) return key;
    const char* s = bt->second.en;
    return (s && *s) ? std::string_view(s) : key;
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
