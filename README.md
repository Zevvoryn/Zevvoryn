<div align="center">

# Zevvoryn

**A Minecraft: Java Edition 1.21.1 server in pure C++20.**
No JVM. No JNI. No wrappers.

[Русский](README.ru.md) · **English**

![version](https://img.shields.io/badge/version-0.2.0--alpha-orange)
![status](https://img.shields.io/badge/status-alpha%20%C2%B7%20unstable-red)
![minecraft](https://img.shields.io/badge/Minecraft-1.21.1-green)
![protocol](https://img.shields.io/badge/protocol-767-lightgrey)
![c++](https://img.shields.io/badge/C%2B%2B-20-blue)
![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)

</div>

---

> ## ⚠️ This is an early alpha
>
> Zevvoryn is at an **alpha** stage and is **very unstable**. It's a
> proof of concept: some systems work and are playable, but a lot is missing,
> some things half-work, and some are buggy. Treat it as an experiment, not a
> finished server.
>
> **Not there yet / not working properly:** crafting and furnaces, redstone and
> mechanisms, portals and travel between the Nether/End (stubs only),
> enchanting and brewing, full mob AI and mob variety, plus a number of other
> blocks/mechanics. Full list is in [`license/CHANGELOG_EN.md`](license/CHANGELOG_EN.md).
>
> Almost none of this has been tested on a live server under load — most of it
> was only checked statically (syntax / bracket balance), not by actually
> playing on a running world, so real behavior may differ. Back up your world.

---

## What it is

Zevvoryn is a from-scratch implementation of the Minecraft: Java Edition 1.21.1
server (protocol 767), written in C++20. A vanilla client connects to it
directly — no mods, no proxy.

The goal is fast startup and predictable resource usage, where the official
Java server spends seconds spinning up the JVM and hundreds of megabytes on the
heap. This is a learning/research project, not a drop-in replacement for the
official server.

```
[17:01:10.482] [Server thread/INFO] Done! (0.05s)
```

---

## What's new in 0.2.0

- **Block hardness & mining time** — vanilla Minecraft 1.21.1 hardness and
  material tables; dig time is computed from the block and the tool in hand
  (debug it with the `/digdebug` command).
- **Tool speed** — correct "best tool" detection, speed multipliers, and the
  tick count per block.
- **Item durability** — tools, armor, shields, elytra and bows now wear out:
  when mining, attacking, blocking with a shield, and when armor absorbs damage.
- **Inventory fixes** — real vanilla stack limits, armor-slot gating (you can't
  put boots on your head anymore — each piece only fits its own slot), and
  proper reset of the held cursor and drag state.
- **Armor & equipping** — vanilla damage absorption (armor + toughness),
  equipping armor in both survival and creative, and equipment changes are
  broadcast to other players.
- **Mob combat** — damage, attack cooldown, and wear on the weapon, the
  victim's shield and the victim's armor.

Full list: [`license/CHANGELOG_EN.md`](license/CHANGELOG_EN.md).

---

## Features

Legend: ✅ works · 🚧 partial / buggy · ❌ not yet

| Area | State |
|------|-------|
| Protocol 1.21.1 (767): handshake, status, play | ✅ |
| Online/offline mode, encryption, packet compression | ✅ |
| Vanilla world generation (cubiomes), vanilla-matching seeds | ✅ |
| Anvil format: read and write regions/chunks | ✅ |
| Block physics, fluids, tick-based updates | ✅ |
| Inventory, items, game modes | ✅ |
| Mining: block hardness, tool speed, durability | ✅ |
| Armor, equipment, combat vs players/mobs | 🚧 |
| Chat, system messages, tab list, skins | ✅ |
| Whitelist (`whitelist.txt`), operators, bans | ✅ |
| RCON (Source RCON, works with any client) | ✅ |
| Discord bot + web control panel | ✅ |
| First-run setup wizard (RU/EN) | ✅ |
| Auto-save, safe save on window close | ✅ |
| Rotating logs (last 15), crash protection | ✅ |
| MiniEdit — fast bulk region block ops | ✅ |
| Mobs: spawning and a basic set | 🚧 |
| Mobs: full AI and variety | ❌ |
| Crafting and furnaces | ❌ |
| Nether, End, inter-dimension portals | ❌ |
| Redstone, mechanisms, rail transport | ❌ |
| Enchanting, brewing | ❌ |

---

## Requirements

- **Windows 10/11 x64** or **Linux x64**
- A compiler with full C++20 support: **MSVC 2022** (17.10+) or GCC 13+/Clang 17+
- **CMake 3.20+** and **Ninja**
- **Node.js 18+** — only if you want the Discord bot and web panel

---

## Building

### Windows (MSVC + Ninja)

From the "x64 Native Tools Command Prompt for VS 2022":

```bat
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Put the resulting `zevvoryn.exe` in its own folder — next to it the server will
create `settings.properties`, `world/`, `logs/` and `DiscrordBotRcon/`.

### Linux

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/zevvoryn
```

---

## First run

On the first start (when there's no `settings.properties` yet) a setup wizard
kicks in: language, port, game mode, difficulty, view distance, whitelist, RCON,
Discord bot and web panel. Everything you pick is saved to
`settings.properties`, and a `DiscrordBotRcon/.env` is created for the panel.

After that the server starts normally:

```
zevvoryn.exe                          # default config
zevvoryn.exe my-settings.properties   # custom config file
```

---

## Configuration (`settings.properties`)

Plain `key=value`, like PocketMine/PMMP.

```properties
# ── Server ──
language=eng             # rus | eng — language of logs and messages
motd=Zevvoryn Server
server-port=25565
max-players=80
view-distance=16
simulation-distance=8

# ── World ──
level-name=world
level-type=DEFAULT       # DEFAULT | FLAT | VOID
level-seed=0             # 0 = random

# ── Gameplay ──
gamemode=creative
difficulty=2             # 0 peaceful … 3 hard
pvp=true
spawn-protection=16

# ── Access ──
online-mode=false
white-list=false         # player names go in whitelist.txt

# ── RCON ──
enable-rcon=true
rcon.port=25575
rcon.password=change_me
rcon.max-clients=4

# ── Panel ──
auto-start-panel=true    # launch the Discord bot / web panel with the server
```

> **Important:** the RCON password is set **only here**. The server syncs
> `DiscrordBotRcon/.env` on every start — don't edit it by hand.

---

## Console commands

```
help                      help
list                      who is online
say <text>                message from the console
gamemode <0-3> [name]     change mode (or gm0..gm3)
give <name> <item>        give an item
tp <name> <x> <y> <z>     teleport
spawn / setworldspawn     spawn point
time set <value>          time of day
weather <clear|rain|thunder>
summon / mob / killall    mobs
setblock <x> <y> <z> <block>
locate <structure>
nether / end / overworld  dimension switch
whitelist <add|remove|on|off|list>
kick <name>
save                      save the world now
tps                       performance
reload                    soft config reload
stop                      graceful shutdown
```

The same commands are available over RCON, the Discord bot and the web panel.

---

## Whitelist

`whitelist.txt` next to the server, one name per line:

```
# Player whitelist
Notch
Steve
```

Enable it with `white-list=true` or the `whitelist on` command.

---

## RCON, Discord bot and web panel

The bundled `DiscrordBotRcon` folder is a Node.js app that connects to the
server over RCON and offers two interfaces: Discord commands and a browser web
panel.

```bash
cd DiscrordBotRcon
npm install
```

With `auto-start-panel=true` the server launches it together with
`zevvoryn.exe` (and writes the current RCON port and password into `.env`).

| Variable | Purpose |
|----------|---------|
| `RCON_HOST`, `RCON_PORT`, `RCON_PASSWORD` | filled in automatically by the server |
| `WEB_ENABLED` | `false` — disable the web panel (enabled by default) |
| `WEB_HOST`, `WEB_PORT` | panel address, default `127.0.0.1:3000` |
| `WEB_PASSWORD` | panel login password |
| `DISCORD_TOKEN`, `CLIENT_ID`, `GUILD_ID` | needed only for the Discord bot |
| `ADMIN_ROLE_ID`, `COMMAND_CHANNEL_ID` | permission gating in Discord |

The panel shows server status, who's online, the whitelist, and gives you a
console in the browser: `http://127.0.0.1:3000`

Register the Discord slash commands (once):

```bash
npm run deploy
```

> The web panel is HTTP only (no real TLS/HTTPS) and uses a single shared Basic
> Auth password — fine for local/personal use, not meant to be exposed directly
> to the internet.

---

## Logs and shutdown

- Logs go to `logs/log-DD.MM.YY.log` (in the English locale — `MM.DD.YY`), the
  last 15 are kept.
- The proper way to stop is the `stop` command.
- Closing the window with the X is also safe: the server kicks players, saves
  the world and player data, and only then exits. Windows gives a closing
  console app about five seconds, so for very large worlds prefer `stop`.
- On sign-out or shutdown the server sets a Windows "shutdown block reason" and
  saves the world with no time limit.
- After a crash the server reports it on the next start.

---

## Project layout

```
core/        config, logs, RCON, whitelist, commands, MiniEdit, server core
network/     TCP server, connections, compression
protocol/    1.21.1 packet codecs and shared structures
entity/      players and mobs
world/       chunks, generation, biomes, Anvil format
registries/  block, item and biome registries
crypto/      protocol encryption
utils/       NBT and helpers
data/        Minecraft game data
thirdparty/  cubiomes (vanilla generation)
tools/       smoke and stress test bots in Python
DiscrordBotRcon/  Discord bot and web panel (Node.js)
```

Extra docs: [`PHYSICS.md`](PHYSICS.md) — the physics model,
[`MINIEDIT.md`](MINIEDIT.md) — bulk region block operations.

---

## Troubleshooting

**The panel says "RCON: Disconnected".**
Check that `settings.properties` has `enable-rcon=true` and a non-empty
`rcon.password` (the remote console won't start with an empty password). The
startup log should contain `Remote console listening on 0.0.0.0:25575`.

**The panel won't open.**
Default port is `3000`, address `http://127.0.0.1:3000`. Look for the crash
cause in `DiscrordBotRcon/panel-crash.log`.

**Panel auto-start was skipped.**
You need Node.js on `PATH` and `npm install` run in the `DiscrordBotRcon` folder.

**The client doesn't see the server on the LAN.**
Open TCP port `25565` in the Windows firewall.

---

## Roadmap

- Crafting and furnaces
- Portals and travel between the Nether/End
- Full mob AI and mob variety
- Structure and village generation
- Redstone and mechanisms
- Minecarts and transport
- Web panel extensions (world map, TPS graphs)

---

## License

Distributed under the **Zevvoryn Custom License v1.0** — see
[`license/LICENSE.txt`](license/LICENSE.txt). In short: you may use, study,
modify and fork it, but only with attribution preserved and source disclosed,
and **no commercial distribution** without written permission. Third-party
components and their licenses are listed in
[`license/THIRD-PARTY-NOTICES.txt`](license/THIRD-PARTY-NOTICES.txt)
(notably [cubiomes](https://github.com/Cubitect/cubiomes)).

Minecraft is a trademark of Mojang Studios. This project is not affiliated with
Mojang Studios or Microsoft and contains none of their code.

---

<div align="center">

**GitHub:** https://github.com/Zevvoryn/Zevvoryn

</div>
