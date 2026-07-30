#pragma once
// BANMGR_V1 — /ban, /pardon, /banlist backed by banned-players.json.
//
// Same design as OpManager (see op_manager.hpp): in-memory list guarded by a
// shared_mutex, atomic temp-file + rename on write, vanilla-shaped JSON so the
// file can be edited by hand or reused by external tooling:
//
//   [
//     {
//       "uuid": "00000000-0000-0000-0000-000000000000",
//       "name": "Griefer",
//       "created": "2026-07-31 03:00:00",
//       "source": "Console",
//       "expires": "forever",
//       "reason": "Banned by an operator."
//     }
//   ]

#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace nc {

struct BanEntry {
    std::string uuid;
    std::string name;
    std::string created;
    std::string source  = "Console";
    std::string expires = "forever";
    std::string reason  = "Banned by an operator.";
};

class BanManager {
public:
    static BanManager& instance();

    void init(const std::filesystem::path& serverRoot);

    bool isBanned(const std::string& name) const;
    std::optional<BanEntry> find(const std::string& name) const;
    std::vector<BanEntry> list() const;

    // Returns false when the player was already banned.
    bool ban(const std::string& name, const std::string& uuid, const std::string& source,
             const std::string& reason);
    // Returns false when the player was not banned in the first place.
    bool pardon(const std::string& name);

    bool load();
    bool save() const;

    const std::filesystem::path& filePath() const { return path_; }

private:
    BanManager() = default;
    static std::string lower(const std::string& s);
    std::string serialize() const; // caller holds the lock

    mutable std::shared_mutex mutex_;
    std::vector<BanEntry> entries_;
    std::filesystem::path path_{"banned-players.json"};
};

} // namespace nc
