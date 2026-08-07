# Zevvoryn — What's New (English summary)

> **Status: 0.2.0 — Alpha (proof of concept).** This is an early, actively-changing
> and **unstable** build. Some of the systems below are implemented and playable,
> but this is **not** a finished, polished server. Expect bugs, rough edges, and
> features that only half-work or don't work at all yet. Almost none of this has
> been tested on a live running server under load — most of it was only checked
> with static syntax/bracket-balance checks, not by actually playing on a running
> world, so real-world behavior may differ. Back up your world.

## 0.2.0 — Mining, durability, armor & inventory

**Block hardness & mining time** — vanilla 1.21.1 hardness and material tables
(generated into `mining_data.gen.hpp`); dig time is computed from the block and
the tool in hand, matching vanilla break-speed math. A `/digdebug` command prints
the per-block calculation.

**Tool speed** — correct "best tool" detection, tool speed multipliers, and the
resulting tick count for every block, driven by the generated tables.

**Item durability** — tools, armor, shield, elytra and bow now take damage and
wear out: when mining, when attacking, when blocking with a shield, and when
armor absorbs incoming damage. Damage values follow vanilla 1.21.1.

**Inventory fixes** — real vanilla stack limits, armor-slot gating (you can no
longer drop boots onto the helmet slot — each armor piece only fits its own
slot), and proper reset of the held cursor / drag state on inventory changes.

**Armor & equipping** — vanilla damage absorption using armor + toughness,
equipping armor in both survival and creative, and equipment changes are now
broadcast to other players so they see what you're wearing/holding.

**Mob combat** — attacking mobs applies damage with the correct cooldown and
wears down the weapon, the victim's shield, and the victim's armor.

**Skins & capes** — `/skin <name>` lets an offline player wear any licensed
nickname's skin, `/skin reset` restores their own; `/cape <id>` puts on an
official minecraft.net cape, `/cape off` removes it, and `/capes` lists what's
available. Skins and capes **only work with `online-mode=false`** — under
`online-mode=true` capes are disabled on purpose, because handing minecraft.net
capes to licensed accounts would violate the Minecraft EULA. If a cape is worn,
the first `/skin reset` only warns; running it again within 15 seconds drops
both the skin and the cape.

---

## 0.1.0 — Physics & server tooling (proof of concept)

## What has been added (gameplay physics)

**Fluids** — water/lava flow ported to match vanilla `FlowingFluid` behavior
(spread priority, slope-finding toward the nearest drop, source creation from
2+ neighboring sources, lava+water -> obsidian/cobblestone).

**Environmental damage** — drowning (checked at eye level), lava damage,
 freezing in powder snow, fire/soul fire contact damage, cactus and magma
 block damage, sweet berry bush damage on horizontal movement only.

**Fall damage** — softened/cancelled by water, cobwebs, powder snow, sweet
 berries, honey blocks; hay bales reduce damage 80%; slime blocks bounce;
 damage rounds up like vanilla.

**Falling blocks** — sand, gravel, concrete powder, dragon egg, anvils all
 fall correctly with real swept collision, turn into a proper falling-block
 entity while airborne, and go back to being a placed block (or drop as an
 item) on landing. Concrete powder hardens into concrete on contact with
 water. Vertical columns cascade down one after another.

**TNT / explosions** — TNT block -> real primed TNT entity, vanilla-shaped
 explosion raycasting, block destruction with partial drops, damage falloff,
 knockback, and chain reactions between nearby TNT. Includes anti-lag
 safeguards (explosion/collapse budgets per tick, TNT count caps) so a big
 chain of TNT doesn't freeze the server, plus an overall world limit on
 `/set`, `/replace`, `/copy`/`/paste` volume to stop accidental server
 freezes from huge selections.

**Projectiles** — ender pearls, snowballs, eggs, and experience bottles all
 use a shared swept-raycast flight path so fast throws can't clip through
 walls; ender pearl teleport + fall damage on landing; experience bottles
 spawn real, visible experience orbs that are attracted to and picked up by
 nearby players instead of granting XP instantly and invisibly.

**Buckets** — fill/empty water, lava, and powder snow; repeated fill/empty
 cycles work without needing to relog.

**Swimming & bubble columns** — buoyancy in 2+ block deep water/lava;
 soul sand / magma block bubble columns push entities up/down with correct
 vanilla speed limits, and affect dropped items, falling blocks, TNT, and
 projectiles too.

**Crop growth & world ticking** — vanilla-style random block ticks drive
 crop growth (wheat, carrots, potatoes, beetroots, nether wart, sugar cane,
 cactus, kelp, vines, saplings growing into trees, pumpkin/melon stems),
 farmland moisture/drying based on nearby water, leaf decay, coral dying
 without water, cauldrons filling with rain/buckets, turtle/sniffer eggs
 hatching over time. Bone meal instantly advances supported crops.

**Vehicles** — boats and minecarts (client-authoritative movement, server
 validates and syncs position, boat paddle animations mirrored to other
 players, floating/gravity physics when no passenger is aboard).

**Potion effects** — poison, wither, regeneration, resistance, strength,
 and weakness now actually apply damage/healing/damage-modifiers over time,
 not just show the icon.

**Villager trading** — trade levels unlock gradually with trading XP
 (vanilla thresholds), trades restock automatically over time.

**Server tooling** — RCON server, whitelist system, a first-run setup
 wizard (language, RCON, Discord bot, web panel), a Discord bot with
 slash commands (list/say/cmd/tp/give/kick/gamemode/weather/time/
 difficulty/stop/whitelist/save/status) with admin-role permission
 checks, and a browser-based web control panel (dashboard/console/
 settings tabs, quick actions, live command console over WebSocket,
 status polling) restricted to localhost by default with optional
 password protection.

## What is still missing or NOT working yet

- **Mobs & AI** — no mobs, no mob AI, no item drops from blocks/mob deaths.
- **Crafting & furnaces** — not implemented.
- **Redstone, pistons, minecart transport on rails/redstone** — not
 implemented (intentionally postponed).
- **Nether / End / portals** — only placeholder block names and a portal
 cooldown field exist; actual teleportation between dimensions is **not**
 implemented. This is the single biggest remaining feature gap.
- **Grass/mycelium spread and ice/snow melting from light** — blocked on a
 missing internal API to read block/sky light at a given position; not
 implemented yet.
- **Enchanting, brewing/potions crafting, plugins/API** — not implemented.
- **Some decorative/interactive blocks intentionally skipped for now**:
 pointed dripstone, scaffolding collapse, glow lichen spread, big/small
 dripleaf, the sculk family, mangrove propagules, and facing-aware
 doors/fences.
- **5 known smoke-test failures not yet fixed**: `CROWD_SWARM_FALLING`,
 `ITEM_REST`, `FALLING_CHAIN`, `CONCRETE_FLOW_IN`, `TNT_EXPLODE`.
- **Web panel** is HTTP only (no real TLS/HTTPS) and only supports a single
 shared Basic Auth password — fine for local/personal use, not meant for
 exposing directly to the internet.
- Various effect/AI interactions are deferred: weakness-before-healing for
 zombie villagers (needs brewing first), a proper wandering trader trade
 pool, effects persisting on mobs (not just players), and
 slowness/speed affecting server-side player movement speed.

**Bottom line:** a large amount of vanilla-like physics and server tooling
is in place and playable, but this is still a proof-of-concept build — some
systems are unfinished, a few are known to be buggy, and several large
features (dimensions/portals, redstone/rail transport, mobs) simply aren't
there yet.
