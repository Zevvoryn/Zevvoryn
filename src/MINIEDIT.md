# MiniEdit V1

MiniEdit is a built-in cuboid editor for Zevvoryn. It is intentionally **not WorldEdit** and has no external dependencies.

## Commands

- `//wand` or `/wand` — gives a wooden axe named **Builder's Wand** in yellow, non-italic text; left click sets pos1, right click sets pos2.
- `//pos1`, `//pos2` — use the player’s current block position.
- `/pso`, `/pos1`, `/pos2` — direct convenience aliases (`pso` maps to pos1).
- `//set <block|state-id>` — fills the selection.
- `//replace <from> <to>` — replaces one exact block state.
- `//copy` — copies the selection relative to the player’s current block position.
- `//rotate <90|180|270>` — changes clipboard rotation around Y.
- `//paste` — pastes at the player’s current block position.
- `/undo`, `/redo` — per-player edit history.
- `/edit ...`, `/we ...` — aliases for all operations, for example `/edit set stone` and `/we replace 1 diorite`.
- Ordinary one-slash forms also work: `/wand`, `/set`, `/replace`, `/copy`, `/paste`, `/rotate`.

Block arguments accept names (`stone`, `minecraft:stone`) and numeric Java 1.21.1 state IDs.

## Architecture

`core/miniedit.hpp` is a reusable editing layer containing:

- `Selection`
- `Clipboard`
- `BlockChange`
- `EditOperation`
- `EditHistory`
- `EditSession`
- `MiniEditManager`

Command parsing, packets and player inventory remain in the server adapter. Editing classes do not depend on network or player classes.

## Performance decisions

- Cuboids are traversed chunk-first.
- A chunk is acquired once for each intersecting chunk rather than once per block.
- Clipboard reads and edits use `ChunkColumn` directly.
- Network updates use one `ClientboundSectionBlocksUpdatePacket` (`0x49`) per changed section, not one packet per block.
- FastAsync packet dispatch flushes at most 64 changed sections per server tick, preventing a huge edit from monopolizing network time.
- Packets are sent only to players within view distance of the changed section.
- Solid-to-solid edits do not enqueue unnecessary fluid/falling updates.
- Lighting is not recalculated per block; the current server uses its existing chunk lighting path.
- Operations are currently synchronous and serialized by `MiniEditManager`; the edit API and one-shot publish hook are suitable for a future async executor.

## History and large operations

- History is per player and stores exact before/after state IDs.
- `RAM_HISTORY_V2` compresses history instead of retaining a 20-byte `BlockChange`
  structure for every block. Consecutive X coordinates use a one-byte opcode,
  other coordinate jumps use zigzag varints, and uniform before/after states are
  stored once per operation. A representative uniform cuboid test compressed
  253,260 bytes to 12,915 bytes (19.6x) without losing exact undo/redo data.
- Undo/redo decode and publish in windows of 65,536 changes, so restoring a
  ten-million-block edit does not allocate another full-size change vector.
- Default depth: 20 operations.
- Default history budget: 2,000,000 changed blocks per player, but the newest operation is always retained even when it alone exceeds that soft budget.
- There is no artificial selection or operation block limit. Available memory and world-coordinate bounds are the natural limits.
- Builder's Wand ownership is stored in playerdata (`MINIEDIT1`) and its yellow custom-name component is restored in the login inventory packet.
- A new operation clears redo history.
- Sessions are removed on disconnect and soft reload.

## Clipboard semantics

The clipboard origin is the player’s block position at copy time. Paste uses the player’s current block position, including the copied relative offset. Rotation is spatial around the Y axis. Exact block state IDs are preserved. The optional `rotateState` hook is reserved for a future complete directional-state transformer.

Entities and block entities are intentionally ignored in V1.

## Selection visualization

A completed selection is shown only to its owner as a bright red dust wireframe. Particle packets are sent directly to that player's connection and are never broadcast. Only the twelve cuboid edges are rendered. Each edge is split into at most eight dense particle segments. After set/replace/paste/undo/redo the server stops refreshing the outline, so existing client particles expire naturally.

## Reserved extension seams

The API reserves interfaces/hooks for a future scheduler, masks, patterns, brushes, schematic codecs and block-entity adapters. None of those features is implemented in V1.
