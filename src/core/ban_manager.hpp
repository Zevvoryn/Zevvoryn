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

// IPBAN_V1 - /ban-ip, /pardon-ip backed by banned-ips.json. Same storage and
// locking design as BanManager above; the on-disk shape is the vanilla one so
// the file round-trips with vanilla / Spigot / Paper / Purpur / Folia:
//
//   [
//     {
//       "ip": "127.0.0.1",
//       "created": "2026-08-03 14:30:00",
//       "source": "Console",
//       "expires": "forever",
//       "reason": "Banned by an operator."
//     }
//   ]
struct IpBanEntry {
    std::string ip;
    std::string created;
    std::string source  = "Console";
    std::string expires = "forever";
    std::string reason  = "Banned by an operator.";
};

class IpBanManager {
public:
    static IpBanManager& instance();

    void init(const std::filesystem::path& serverRoot);

    // `ip` may carry a port or an IPv4-mapped IPv6 prefix; it is normalised.
    bool isBanned(const std::string& ip) const;
    std::optional<IpBanEntry> find(const std::string& ip) const;
    std::vector<IpBanEntry> list() const;

    bool ban(const std::string& ip, const std::string& source, const std::string& reason);
    bool pardon(const std::string& ip);

    bool load();
    bool save() const;

    const std::filesystem::path& filePath() const { return path_; }

    // "[::ffff:1.2.3.4]:52134" -> "1.2.3.4"
    static std::string normalize(const std::string& ip);

private:
    IpBanManager() = default;
    std::string serialize() const; // caller holds the lock

    mutable std::shared_mutex mutex_;
    std::vector<IpBanEntry> entries_;
    std::filesystem::path path_{"banned-ips.json"};
};

} // namespace nc
