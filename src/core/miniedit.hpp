#pragma once

#include "types.hpp"
#include "../world/chunk.hpp"
#include <deque>
#include <limits>

namespace nc::miniedit {

// MiniEdit deliberately owns no networking, command parsing, entities, lighting,
// or block-entity code. The server injects those concerns through EditHooks.
// This keeps the synchronous core reusable by a future async executor.

struct Selection {
    std::optional<BlockPos> pos1;
    std::optional<BlockPos> pos2;

    bool complete() const { return pos1.has_value() && pos2.has_value(); }

    BlockPos min() const {
        return {std::min(pos1->x, pos2->x), std::min(pos1->y, pos2->y), std::min(pos1->z, pos2->z)};
    }
    BlockPos max() const {
        return {std::max(pos1->x, pos2->x), std::max(pos1->y, pos2->y), std::max(pos1->z, pos2->z)};
    }
    u64 volume() const {
        if (!complete()) return 0;
        const BlockPos lo = min(), hi = max();
        return static_cast<u64>(hi.x - lo.x + 1) * static_cast<u64>(hi.y - lo.y + 1) *
               static_cast<u64>(hi.z - lo.z + 1);
    }
};

struct Clipboard {
    i32 sizeX = 0, sizeY = 0, sizeZ = 0;
    i32 originX = 0, originY = 0, originZ = 0; // selection min relative to copy-time player position
    i32 rotation = 0;
    std::vector<i32> states;

    bool empty() const { return states.empty(); }
    size_t index(i32 x, i32 y, i32 z) const {
        return (static_cast<size_t>(y) * static_cast<size_t>(sizeZ) + static_cast<size_t>(z)) *
               static_cast<size_t>(sizeX) + static_cast<size_t>(x);
    }
    i32 at(i32 x, i32 y, i32 z) const { return states[index(x, y, z)]; }
};

struct BlockChange {
    BlockPos pos;
    i32 before = 0;
    i32 after = 0;
};

class EditOperation {
public:
    EditOperation() = default;
    EditOperation(std::string label, std::vector<BlockChange> changes)
        : label_(std::move(label)), count_(changes.size()) {
        if (changes.empty()) return;
        commonBefore_ = changes.front().before;
        commonAfter_ = changes.front().after;
        sameBefore_ = std::all_of(changes.begin(), changes.end(), [&](const BlockChange& c) {
            return c.before == commonBefore_;
        });
        sameAfter_ = std::all_of(changes.begin(), changes.end(), [&](const BlockChange& c) {
            return c.after == commonAfter_;
        });

        // RAM_HISTORY_V2: edits are traversed in spatial order, so almost every
        // entry is simply x+1. Store that as one byte and varint-encode the rare
        // coordinate jumps. Uniform //set before/after states are stored once.
        data_.reserve(std::min<size_t>(changes.size() * 2, 8 * 1024 * 1024));
        i64 px = 0, py = 0, pz = 0;
        bool first = true;
        for (const auto& c : changes) {
            if (!first && c.pos.x == px + 1 && c.pos.y == py && c.pos.z == pz) {
                data_.push_back(0);
            } else {
                data_.push_back(1);
                writeSigned(data_, static_cast<i64>(c.pos.x) - px);
                writeSigned(data_, static_cast<i64>(c.pos.y) - py);
                writeSigned(data_, static_cast<i64>(c.pos.z) - pz);
            }
            if (!sameBefore_) writeUnsigned(data_, static_cast<u32>(c.before));
            if (!sameAfter_) writeUnsigned(data_, static_cast<u32>(c.after));
            px = c.pos.x; py = c.pos.y; pz = c.pos.z; first = false;
        }
        // Release the former 20-byte-per-block buffer immediately. Keeping the
        // moved parameter alive until the constructor returns can otherwise make
        // the process appear to retain hundreds of MB after a huge //set.
        std::vector<BlockChange>().swap(changes);
    }

    const std::string& label() const { return label_; }
    size_t size() const { return count_; }
    size_t compressedBytes() const { return data_.size(); }
    bool empty() const { return count_ == 0; }

    template<class Fn>
    void forEachChange(Fn&& fn) const {
        size_t cursor = 0;
        i64 x = 0, y = 0, z = 0;
        for (size_t index = 0; index < count_; ++index) {
            if (cursor >= data_.size()) return; // corrupt history: fail closed
            const u8 opcode = data_[cursor++];
            if (opcode == 0) {
                ++x;
            } else {
                i64 dx = 0, dy = 0, dz = 0;
                if (!readSigned(data_, cursor, dx) || !readSigned(data_, cursor, dy) ||
                    !readSigned(data_, cursor, dz)) return;
                x += dx; y += dy; z += dz;
            }
            u64 before = static_cast<u32>(commonBefore_);
            u64 after = static_cast<u32>(commonAfter_);
            if (!sameBefore_ && !readUnsigned(data_, cursor, before)) return;
            if (!sameAfter_ && !readUnsigned(data_, cursor, after)) return;
            fn(BlockChange{{static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)},
                           static_cast<i32>(before), static_cast<i32>(after)});
        }
    }

private:
    static void writeUnsigned(std::vector<u8>& out, u64 value) {
        do {
            u8 byte = static_cast<u8>(value & 0x7Fu);
            value >>= 7;
            if (value) byte |= 0x80u;
            out.push_back(byte);
        } while (value);
    }
    static void writeSigned(std::vector<u8>& out, i64 value) {
        const u64 zigzag = (static_cast<u64>(value) << 1) ^
                           static_cast<u64>(value >> 63);
        writeUnsigned(out, zigzag);
    }
    static bool readUnsigned(const std::vector<u8>& in, size_t& cursor, u64& value) {
        value = 0;
        for (i32 shift = 0; shift < 64; shift += 7) {
            if (cursor >= in.size()) return false;
            const u8 byte = in[cursor++];
            value |= static_cast<u64>(byte & 0x7Fu) << shift;
            if ((byte & 0x80u) == 0) return true;
        }
        return false;
    }
    static bool readSigned(const std::vector<u8>& in, size_t& cursor, i64& value) {
        u64 zigzag = 0;
        if (!readUnsigned(in, cursor, zigzag)) return false;
        value = static_cast<i64>((zigzag >> 1) ^ (~(zigzag & 1) + 1));
        return true;
    }

    std::string label_;
    std::vector<u8> data_;
    size_t count_ = 0;
    i32 commonBefore_ = 0;
    i32 commonAfter_ = 0;
    bool sameBefore_ = true;
    bool sameAfter_ = true;
};

class EditHistory {
public:
    explicit EditHistory(size_t maxOperations = 20, size_t maxChangedBlocks = 2'000'000)
        : maxOperations_(maxOperations), maxChangedBlocks_(maxChangedBlocks) {}

    void push(EditOperation op) {
        if (op.empty()) return;
        redo_.clear();
        undo_.push_back(std::move(op));
        trim();
    }

    std::optional<EditOperation> takeUndo() {
        if (undo_.empty()) return std::nullopt;
        EditOperation op = std::move(undo_.back());
        undo_.pop_back();
        return op;
    }
    std::optional<EditOperation> takeRedo() {
        if (redo_.empty()) return std::nullopt;
        EditOperation op = std::move(redo_.back());
        redo_.pop_back();
        return op;
    }
    void moveToRedo(EditOperation op) { redo_.push_back(std::move(op)); }
    void moveToUndo(EditOperation op) { undo_.push_back(std::move(op)); trim(); }

    size_t undoCount() const { return undo_.size(); }
    size_t redoCount() const { return redo_.size(); }

private:
    void trim() {
        while (undo_.size() > maxOperations_) undo_.pop_front();
        size_t blocks = 0;
        for (const auto& op : undo_) blocks += op.size();
        // Keep at least the newest operation even when that single operation is
        // larger than the soft history budget. A huge edit must still be undoable.
        while (undo_.size() > 1 && blocks > maxChangedBlocks_) {
            blocks -= undo_.front().size();
            undo_.pop_front();
        }
    }

    size_t maxOperations_;
    size_t maxChangedBlocks_;
    std::deque<EditOperation> undo_;
    std::deque<EditOperation> redo_;
};

class EditSession {
public:
    Selection selection;
    Clipboard clipboard;
    EditHistory history;
    bool wandEnabled = false;
    bool visualActive = false;
};

// Explicit extension seams. They are intentionally interfaces only: masks,
// patterns, brushes, schematics and block entities are not implemented in V1.
class IMask;
class IPattern;
class IBrush;
class ISchematicCodec;
class IBlockEntityAdapter;
class IEditScheduler;

struct EditHooks {
    // Called once per operation, not once per block. The server should aggregate
    // changes into section-update packets and schedule neighbour physics here.
    std::function<void(std::span<const BlockChange>)> publishChanges;

    // Optional block-state transform for directional states. If absent, exact
    // state IDs are preserved while only coordinates rotate.
    std::function<i32(i32 state, i32 degrees)> rotateState;
};

struct EditResult {
    bool ok = false;
    size_t affected = 0;
    std::string message;

    static EditResult success(size_t n, std::string msg = {}) {
        return {true, n, std::move(msg)};
    }
    static EditResult failure(std::string msg) {
        return {false, 0, std::move(msg)};
    }
};

struct SessionVisual {
    u64 playerKey = 0;
    Selection selection;
};

class MiniEditManager {
public:
    explicit MiniEditManager(world::World& world) : world_(world) {}

    void removeSession(u64 playerKey) {
        std::lock_guard lock(mutex_);
        sessions_.erase(playerKey);
    }

    void clear() {
        std::lock_guard lock(mutex_);
        sessions_.clear();
    }

    void setWandEnabled(u64 playerKey, bool enabled = true) {
        std::lock_guard lock(mutex_);
        sessions_[playerKey].wandEnabled = enabled;
    }

    bool wandEnabled(u64 playerKey) const {
        std::lock_guard lock(mutex_);
        auto it = sessions_.find(playerKey);
        return it != sessions_.end() && it->second.wandEnabled;
    }

    Selection setPosition(u64 playerKey, int which, BlockPos pos) {
        std::lock_guard lock(mutex_);
        auto& s = sessions_[playerKey].selection;
        if (which == 1) s.pos1 = pos; else s.pos2 = pos;
        sessions_[playerKey].visualActive = true;
        return s;
    }

    void hideVisual(u64 playerKey) {
        std::lock_guard lock(mutex_);
        auto it = sessions_.find(playerKey);
        if (it != sessions_.end()) it->second.visualActive = false;
    }

    std::optional<Selection> selection(u64 playerKey) const {
        std::lock_guard lock(mutex_);
        auto it = sessions_.find(playerKey);
        if (it == sessions_.end()) return std::nullopt;
        return it->second.selection;
    }

    std::vector<SessionVisual> visuals() const {
        std::lock_guard lock(mutex_);
        std::vector<SessionVisual> out;
        out.reserve(sessions_.size());
        for (const auto& [key, session] : sessions_)
            if (session.visualActive && session.selection.complete()) out.push_back({key, session.selection});
        return out;
    }

    EditResult set(u64 playerKey, i32 state, const EditHooks& hooks) {
        std::lock_guard lock(mutex_);
        auto& s = sessions_[playerKey];
        auto valid = validateSelection(s.selection);
        if (!valid.ok) return valid;
        std::vector<BlockChange> changes;
        changes.reserve(static_cast<size_t>(s.selection.volume()));
        editCuboid(s.selection, [&](i32) { return true; }, [&](i32) { return state; }, changes);
        return finishOperation(s, "set", std::move(changes), hooks);
    }

    EditResult replace(u64 playerKey, i32 from, i32 to, const EditHooks& hooks) {
        std::lock_guard lock(mutex_);
        auto& s = sessions_[playerKey];
        auto valid = validateSelection(s.selection);
        if (!valid.ok) return valid;
        std::vector<BlockChange> changes;
        changes.reserve(std::min<u64>(s.selection.volume(), 65'536));
        editCuboid(s.selection, [&](i32 old) { return old == from; }, [&](i32) { return to; }, changes);
        return finishOperation(s, "replace", std::move(changes), hooks);
    }

    EditResult copy(u64 playerKey, BlockPos playerPos) {
        std::lock_guard lock(mutex_);
        auto& s = sessions_[playerKey];
        auto valid = validateSelection(s.selection);
        if (!valid.ok) return valid;
        const BlockPos lo = s.selection.min(), hi = s.selection.max();
        Clipboard cb;
        cb.sizeX = hi.x - lo.x + 1; cb.sizeY = hi.y - lo.y + 1; cb.sizeZ = hi.z - lo.z + 1;
        cb.originX = lo.x - playerPos.x; cb.originY = lo.y - playerPos.y; cb.originZ = lo.z - playerPos.z;
        cb.states.resize(static_cast<size_t>(s.selection.volume()));

        for (i32 cx = lo.x >> 4; cx <= (hi.x >> 4); ++cx) {
            const i32 x0 = std::max(lo.x, cx << 4), x1 = std::min(hi.x, (cx << 4) + 15);
            for (i32 cz = lo.z >> 4; cz <= (hi.z >> 4); ++cz) {
                const i32 z0 = std::max(lo.z, cz << 4), z1 = std::min(hi.z, (cz << 4) + 15);
                auto chunk = world_.getOrGenerateChunk(cx, cz);
                for (i32 y = lo.y; y <= hi.y; ++y)
                    for (i32 z = z0; z <= z1; ++z)
                        for (i32 x = x0; x <= x1; ++x)
                            cb.states[cb.index(x - lo.x, y - lo.y, z - lo.z)] = chunk->getBlock(x, y, z);
            }
        }
        s.clipboard = std::move(cb);
        return EditResult::success(s.clipboard.states.size(), "copied");
    }

    EditResult rotate(u64 playerKey, i32 degrees) {
        // MINIEDIT_ROTATE_V2: accept -90 / 360 / 0 as well, they are the same turns.
        const i32 norm = ((degrees % 360) + 360) % 360;
        if (norm != 0 && norm != 90 && norm != 180 && norm != 270)
            return EditResult::failure("rotation must be 90, 180, 270 (or -90 / 0)");
        std::lock_guard lock(mutex_);
        auto& cb = sessions_[playerKey].clipboard;
        if (cb.empty())
            return EditResult::failure("clipboard is empty - do //pos1, //pos2 and //copy first");
        cb.rotation = (cb.rotation + norm) % 360;
        return EditResult::success(static_cast<i64>(cb.states.size()),
                                   "rotation=" + std::to_string(cb.rotation));
    }

    EditResult paste(u64 playerKey, BlockPos playerPos, const EditHooks& hooks) {
        std::lock_guard lock(mutex_);
        auto& s = sessions_[playerKey];
        const Clipboard& cb = s.clipboard;
        if (cb.empty()) return EditResult::failure("clipboard is empty");
        std::vector<BlockChange> changes;
        changes.reserve(cb.states.size());
        std::unordered_map<u64, std::shared_ptr<world::ChunkColumn>> chunkCache;
        chunkCache.reserve((cb.states.size() / 4096) + 4);
        for (i32 y = 0; y < cb.sizeY; ++y) for (i32 z = 0; z < cb.sizeZ; ++z) for (i32 x = 0; x < cb.sizeX; ++x) {
            i32 rx = cb.originX + x, rz = cb.originZ + z;
            const auto [tx, tz] = rotateXZ(rx, rz, cb.rotation);
            BlockPos dst{playerPos.x + tx, playerPos.y + cb.originY + y, playerPos.z + tz};
            if (dst.y < world::CHUNK_HEIGHT_MIN || dst.y >= world::CHUNK_HEIGHT_MAX) continue;
            const i32 cx = dst.x >> 4, cz = dst.z >> 4;
            const u64 key = (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cz);
            auto& chunk = chunkCache[key];
            if (!chunk) chunk = world_.getOrGenerateChunk(cx, cz);
            const i32 before = chunk->getBlock(dst.x, dst.y, dst.z);
            i32 after = cb.at(x, y, z);
            if (hooks.rotateState) after = hooks.rotateState(after, cb.rotation);
            if (before == after) continue;
            chunk->setBlock(dst.x, dst.y, dst.z, after);
            changes.push_back({dst, before, after});
        }
        return finishOperation(s, "paste", std::move(changes), hooks);
    }

    EditResult undo(u64 playerKey, const EditHooks& hooks) {
        std::lock_guard lock(mutex_);
        auto& history = sessions_[playerKey].history;
        auto op = history.takeUndo();
        if (!op) return EditResult::failure("nothing to undo");
        applyOperation(*op, false, hooks);
        sessions_[playerKey].visualActive = false;
        const size_t n = op->size();
        history.moveToRedo(std::move(*op));
        return EditResult::success(n, "undo");
    }

    EditResult redo(u64 playerKey, const EditHooks& hooks) {
        std::lock_guard lock(mutex_);
        auto& history = sessions_[playerKey].history;
        auto op = history.takeRedo();
        if (!op) return EditResult::failure("nothing to redo");
        applyOperation(*op, true, hooks);
        sessions_[playerKey].visualActive = false;
        const size_t n = op->size();
        history.moveToUndo(std::move(*op));
        return EditResult::success(n, "redo");
    }

private:
    // ANTILAG_SET_V1: shared cap for /set, /replace, and /copy (which also bounds
    // /paste, since paste is limited by whatever was captured via copy). A single
    // oversized edit was observed to stall the tick loop badly enough that a player
    // fell through the floor client-side (chunk/collision desync) until rejoining.
    static constexpr u64 kMaxEditVolume = 8'000'000;

    EditResult validateSelection(const Selection& selection) const {
        if (!selection.complete()) return EditResult::failure("set pos1 and pos2 first");
        if (selection.min().y < world::CHUNK_HEIGHT_MIN || selection.max().y >= world::CHUNK_HEIGHT_MAX)
            return EditResult::failure("selection exceeds world height");
        if (selection.volume() > kMaxEditVolume)
            return EditResult::failure("selection too large (max 8,000,000 blocks)");
        return EditResult::success(0);
    }

    template<class Predicate, class Transform>
    void editCuboid(const Selection& selection, Predicate&& predicate, Transform&& transform,
                    std::vector<BlockChange>& out) {
        const BlockPos lo = selection.min(), hi = selection.max();
        for (i32 cx = lo.x >> 4; cx <= (hi.x >> 4); ++cx) {
            const i32 x0 = std::max(lo.x, cx << 4), x1 = std::min(hi.x, (cx << 4) + 15);
            for (i32 cz = lo.z >> 4; cz <= (hi.z >> 4); ++cz) {
                const i32 z0 = std::max(lo.z, cz << 4), z1 = std::min(hi.z, (cz << 4) + 15);
                auto chunk = world_.getOrGenerateChunk(cx, cz);
                for (i32 y = lo.y; y <= hi.y; ++y) for (i32 z = z0; z <= z1; ++z) for (i32 x = x0; x <= x1; ++x) {
                    const i32 before = chunk->getBlock(x, y, z);
                    if (!predicate(before)) continue;
                    const i32 after = transform(before);
                    if (before == after) continue;
                    chunk->setBlock(x, y, z, after);
                    out.push_back({{x, y, z}, before, after});
                }
            }
        }
    }

    EditResult finishOperation(EditSession& session, std::string label, std::vector<BlockChange> changes,
                               const EditHooks& hooks) {
        const size_t n = changes.size();
        if (n == 0) {
            session.visualActive = false;
            return EditResult::success(0, "no blocks changed");
        }
        if (hooks.publishChanges) hooks.publishChanges(changes);
        session.history.push(EditOperation(std::move(label), std::move(changes)));
        session.visualActive = false;
        return EditResult::success(n);
    }

    void applyOperation(const EditOperation& op, bool forward, const EditHooks& hooks) {
        std::unordered_map<u64, std::shared_ptr<world::ChunkColumn>> chunkCache;
        chunkCache.reserve((op.size() / 4096) + 4);
        std::vector<BlockChange> published;
        constexpr size_t kPublishWindow = 65'536;
        published.reserve(std::min(op.size(), kPublishWindow));
        auto flushPublished = [&] {
            if (hooks.publishChanges && !published.empty()) hooks.publishChanges(published);
            published.clear();
        };
        op.forEachChange([&](const BlockChange& change) {
            const i32 cx = change.pos.x >> 4, cz = change.pos.z >> 4;
            const u64 key = (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cz);
            auto& chunk = chunkCache[key];
            if (!chunk) chunk = world_.getOrGenerateChunk(cx, cz);
            const i32 current = chunk->getBlock(change.pos.x, change.pos.y, change.pos.z);
            const i32 target = forward ? change.after : change.before;
            if (current == target) return;
            chunk->setBlock(change.pos.x, change.pos.y, change.pos.z, target);
            published.push_back({change.pos, current, target});
            if (published.size() >= kPublishWindow) flushPublished();
        });
        flushPublished();
    }

    static std::pair<i32, i32> rotateXZ(i32 x, i32 z, i32 degrees) {
        switch ((degrees % 360 + 360) % 360) {
            case 90:  return {-z, x};
            case 180: return {-x, -z};
            case 270: return {z, -x};
            default:  return {x, z};
        }
    }

    world::World& world_;
    mutable std::mutex mutex_;
    std::unordered_map<u64, EditSession> sessions_;
};

} // namespace nc::miniedit
