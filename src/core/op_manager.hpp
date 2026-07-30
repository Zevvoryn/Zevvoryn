#pragma once
// OPMGR_V1 — operator (OP) management for Zevvoryn.
//
// Owns ops.json in the server root and answers "is this player an operator,
// and at which level (1..4)".
//
//   [
//     {
//       "uuid": "00000000-0000-0000-0000-000000000000",
//       "name": "PlayerName",
//       "level": 4,
//       "bypassesPlayerLimit": false
//     }
//   ]
//
// Design notes
// ------------
// * Reads are lock-cheap: std::shared_mutex, many readers in parallel. isOp()
//   is called from hot paths (block break, spawn protection), so it never
//   touches the disk.
// * Writes are asynchronous: addOp()/removeOp() update memory, mark the state
//   dirty and wake a background writer thread. The writer serialises the whole
//   list into ops.json.tmp and renames it over ops.json, so a crash mid-write
//   can never leave a truncated ops.json behind.
// * Names are matched case-insensitively (vanilla behaviour), UUIDs are stored
//   as-is; for offline-mode players the vanilla "OfflinePlayer:<name>" MD5 v3
//   UUID is generated so the file stays compatible with vanilla tooling.
// * A refresh hook lets core/server.cpp push the new permission level to the
//   client (Entity Event 24 + level) the moment /op or /deop runs.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace nc {

struct OpEntry {
    std::string uuid;                 // canonical 8-4-4-4-12 form
    std::string name;                 // as typed by the admin (display case)
    int level = 4;                    // 1..4, vanilla default for /op is 4
    bool bypassesPlayerLimit = false; // vanilla field, honoured on join
};

class OpManager {
public:
    static OpManager& instance();

    // Points the manager at <serverRoot>/ops.json and loads it. Safe to call
    // again on a soft reload — it simply re-reads the file.
    void init(const std::filesystem::path& serverRoot);

    // Called by core/server.cpp so the manager can refresh a player's client
    // (Entity Event / command tree) right after their level changes.
    // Signature: (playerName, newLevel) — newLevel is 0 when de-opped.
    void setRefreshHook(std::function<void(const std::string&, int)> hook);

    // ---- queries (thread-safe, no I/O) ----
    bool isOp(const std::string& name) const;
    int level(const std::string& name) const;              // 0 when not an op
    bool bypassesPlayerLimit(const std::string& name) const;
    bool empty() const;
    std::vector<OpEntry> list() const;
    std::optional<OpEntry> find(const std::string& name) const;

    // ---- mutations (memory first, disk in the background) ----
    // level is clamped to 1..4; 4 (full operator) is the default.
    // uuid may be empty — then the offline-mode UUID is derived from the name.
    bool addOp(const std::string& name, const std::string& uuid = {}, int level = 4,
               bool bypassesPlayerLimit = false);
    bool removeOp(const std::string& name);

    // ---- disk ----
    bool load();          // re-read ops.json (called by init and /reload)
    bool save();          // synchronous flush, used on shutdown
    void requestSave();   // asynchronous flush (used by addOp/removeOp)
    void shutdown();      // flush + stop the writer thread

    // Seeds ops.json once from the legacy `ops=` CSV in settings.properties so
    // nobody loses their operator status when upgrading. No-op if ops.json
    // already exists or the CSV is empty.
    void importLegacyCsv(const std::string& opsCsv);

    const std::filesystem::path& filePath() const { return path_; }

    // Vanilla offline-mode UUID: UUID v3 of "OfflinePlayer:<name>".
    static std::string offlineUuid(const std::string& name);

private:
    OpManager() = default;
    ~OpManager();
    OpManager(const OpManager&) = delete;
    OpManager& operator=(const OpManager&) = delete;

    static std::string lower(const std::string& s);
    std::string serialize() const;                    // caller holds the lock
    bool writeFile(const std::string& json) const;
    void startWriter();

    mutable std::shared_mutex mutex_;
    std::vector<OpEntry> entries_;
    std::filesystem::path path_{"ops.json"};
    std::function<void(const std::string&, int)> refreshHook_;
    bool loaded_ = false;

    struct Writer;
    Writer* writer_ = nullptr;
};

// Convenience used across core/server.cpp: true when `name` may run operator
// commands. ops.json wins; the legacy `ops=` CSV still works; and when both
// are empty the server stays in its original "everyone is op" bootstrap mode
// so a fresh server is not locked out.
bool opAllowed(const std::string& opsCsv, const std::string& name);

// Effective permission level for the vanilla Entity Event packet (0..4).
int opLevelOf(const std::string& opsCsv, const std::string& name);

} // namespace nc
