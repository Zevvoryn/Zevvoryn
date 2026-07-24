#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
SMOKE_V3: смоук-бот для сервера Zevvoryn (Minecraft Java 1.21.1, offline mode).

Подключается двумя ботами и проверяет:
  LOGIN      — вход обоих ботов (handshake -> login -> configuration -> play)
  SPAWN_Y    — высота спавна (FLATNATIVE_V1: ноги на Y=4)
  CHUNKS     — чанки лежат на Y=0..3, нет старых чанков на Y=-64..-61
  BIOME      — биом равнин (plains=39, BIOMEGREEN_V1)
  VISIBILITY — бот B находит бота A ПО UUID (посторонние игроки не мешают)
  PVP_HIT    — урон проходит (Interact 0x16 -> Update Health 0x5D)
  IFRAME     — быстрый даблклик не даёт двойной урон (IFRAME_V2)
  DEATH      — экран смерти (Combat Death 0x3C)
  DEATHVIS   — труп исчезает у второго бота (Remove Entities 0x42, DEATHVIS_V1)
  RESPAWN    — телепорт и восстановление хп после респавна
  INVRESPAWN — инвентарь пересылается после респавна (0x13, INVRESPAWN_V1)
  PERMLEVEL  — уровень прав 24..28 после респавна (Entity Event 0x1F, PERMLEVEL_V2)
  RESPAWNVIS — возрождённый снова виден второму боту (DEATHVIS_V1)
  SWORD      — алмазный меч, полный замах: урон ~7 (COMBAT_V2)
  ARMOR      — полное железо против меча: урон ~3.78 (ARMOR_V1, ванильная формула)
  SHIELD     — поднятый щит блокирует удар спереди: урон 0 (SHIELD_V1)
  SHIELD_DOWN— опущенный щит: урон снова проходит
  KB         — обычный удар отбрасывает жертву (Entity Velocity 0x5A)
  SHIELD_NOKB— заблокированный удар НЕ отбрасывает жертву (SHIELD_V2)
  SHIELD_NEWVIS — новый игрок (бот C) видит поднятый щит A (SHIELD_V2)
  AXE_BREAK  — топор об щит: урон 0, но щит уходит в кд на 5 с (SHIELD_V2)
  SHIELD_CD  — во время кд щит не поднимается, урон проходит (SHIELD_V2)
  SHIELD_RECOVER — после 5 с кд щит снова блокирует (SHIELD_V2)
  TIME       — /time set 6000 доходит до всех (0x64, TIMESYNC_V1)

Запуск:  python tools/smoke_bot.py [--host 127.0.0.1] [--port 25565]
Требования: Python 3.10+, сервер в offline-режиме, pvp=true, ops пустой,
генератор FLAT, лучше на свежем мире. Зрители на сервере не ломают
прицеливание (боты ищут друг друга по UUID), но не бейте ботов во время прогона :)
"""
import argparse
import socket
import struct
import sys
import threading
import time
import uuid as uuidlib
import zlib

PROTOCOL = 767  # 1.21.1
DIAMOND_SWORD = 838   # 818 + tier4*5 + sword
DIAMOND_AXE = 841     # 818 + tier4*5 + axe (SHIELD_V2: топор отключает щит)
IRON_ARMOR = (864, 865, 866, 867)  # шлем, нагрудник, поножи, ботинки
SHIELD = 1162


def enc_varint(v: int) -> bytes:
    v &= 0xFFFFFFFF
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def enc_string(s: str) -> bytes:
    d = s.encode("utf-8")
    return enc_varint(len(d)) + d


class Reader:
    def __init__(self, data: bytes):
        self.d = data
        self.o = 0

    def u8(self):
        v = self.d[self.o]
        self.o += 1
        return v

    def i16(self):
        v = struct.unpack_from(">h", self.d, self.o)[0]
        self.o += 2
        return v

    def u16(self):
        v = struct.unpack_from(">H", self.d, self.o)[0]
        self.o += 2
        return v

    def i32(self):
        v = struct.unpack_from(">i", self.d, self.o)[0]
        self.o += 4
        return v

    def i64(self):
        v = struct.unpack_from(">q", self.d, self.o)[0]
        self.o += 8
        return v

    def f32(self):
        v = struct.unpack_from(">f", self.d, self.o)[0]
        self.o += 4
        return v

    def f64(self):
        v = struct.unpack_from(">d", self.d, self.o)[0]
        self.o += 8
        return v

    def varint(self):
        v = 0
        sh = 0
        while True:
            b = self.u8()
            v |= (b & 0x7F) << sh
            if not (b & 0x80):
                break
            sh += 7
        if v >= 2 ** 31:
            v -= 2 ** 32
        return v

    def string(self):
        n = self.varint()
        s = self.d[self.o:self.o + n].decode("utf-8", "replace")
        self.o += n
        return s

    def bytes_(self, n):
        b = self.d[self.o:self.o + n]
        self.o += n
        return b

    def skip(self, n):
        self.o += n

    def remaining(self):
        return len(self.d) - self.o

    # --- NBT (network: root без имени) ---
    def nbt_skip_payload(self, t):
        if t == 0:
            return
        if t == 1:
            self.skip(1)
        elif t == 2:
            self.skip(2)
        elif t in (3, 5):
            self.skip(4)
        elif t in (4, 6):
            self.skip(8)
        elif t == 7:
            self.skip(self.i32())
        elif t == 8:
            self.skip(self.u16())
        elif t == 9:
            it = self.u8()
            n = self.i32()
            for _ in range(max(0, n)):
                self.nbt_skip_payload(it)
        elif t == 10:
            while True:
                it = self.u8()
                if it == 0:
                    break
                self.skip(self.u16())  # имя тега
                self.nbt_skip_payload(it)
        elif t == 11:
            self.skip(self.i32() * 4)
        elif t == 12:
            self.skip(self.i32() * 8)
        else:
            raise ValueError(f"NBT tag {t}")

    def nbt_skip_network(self):
        t = self.u8()
        self.nbt_skip_payload(t)

    # --- Paletted Container (блоки/биомы чанк-секции) ---
    def paletted(self):
        bits = self.u8()
        single = None
        if bits == 0:
            single = self.varint()
        elif bits <= 8:
            for _ in range(self.varint()):
                self.varint()
        n = self.varint()
        self.skip(n * 8)
        return single


class Bot(threading.Thread):
    def __init__(self, name, host, port, ground_section):
        super().__init__(daemon=True)
        self.bot_name = name
        self.host = host
        self.port = port
        self.ground_section = ground_section
        self.sock = None
        self.slock = threading.Lock()
        self.lock = threading.Lock()
        self.compress = -1
        self.error = None
        self.eid = None
        self.my_uuid = None             # UUID из Login Success — по нему нас ищет второй бот
        self.pos = None
        self.health = 20.0
        self.died = False
        self.death_packet = False
        self.got_inventory = 0          # сколько раз пришёл 0x13 (окно 0)
        self.perm_events = []           # статусы 24..28 для своего eid
        self.spawned = {}               # eid -> uuid (16 байт) из Spawn Entity
        self.spawn_counts = {}          # eid -> сколько раз спавнился
        self.removed = set()            # eid из Remove Entities
        self.velocity_events = 0        # SMOKE_V3: Entity Velocity 0x5A на свой eid (отброс)
        self.metadata = {}              # SMOKE_V3: eid -> {index: byte} из Set Entity Metadata 0x58
        self.time_of_day = None
        self.chunks_total = 0
        self.chunks_bad = 0             # чанки с блоками в секции 0 (Y=-64..-49)
        self.chunks_good = 0            # чанки с блоками в ожидаемой секции
        self.chunk_parse_errors = 0
        self.biome_single = None
        self.in_play = threading.Event()
        self.positioned = threading.Event()

    # ---------- сеть ----------
    def _read_exact(self, n):
        buf = b""
        while len(buf) < n:
            part = self.sock.recv(n - len(buf))
            if not part:
                raise ConnectionError("соединение закрыто сервером")
            buf += part
        return buf

    def _read_varint_sock(self):
        v = 0
        sh = 0
        while True:
            b = self._read_exact(1)[0]
            v |= (b & 0x7F) << sh
            if not (b & 0x80):
                return v
            sh += 7

    def read_frame(self):
        ln = self._read_varint_sock()
        data = self._read_exact(ln)
        if self.compress >= 0:
            r = Reader(data)
            ul = r.varint()
            data = data[r.o:]
            if ul:
                data = zlib.decompress(data)
        return data

    def send_packet(self, pid, payload=b""):
        data = enc_varint(pid) + payload
        if self.compress >= 0:
            if len(data) >= self.compress:
                body = enc_varint(len(data)) + zlib.compress(data)
            else:
                body = enc_varint(0) + data
        else:
            body = data
        frame = enc_varint(len(body)) + body
        with self.slock:
            self.sock.sendall(frame)

    # ---------- действия ----------
    def send_command(self, cmd):
        self.send_packet(0x04, enc_string(cmd))  # Chat Command

    def attack(self, eid):
        self.send_packet(0x16, enc_varint(eid) + enc_varint(1) + b"\x00")  # Interact: attack

    def respawn(self):
        self.send_packet(0x09, enc_varint(0))  # Client Command: perform respawn

    def send_pos(self):
        if self.pos:
            x, y, z = self.pos
            self.send_packet(0x1A, struct.pack(">ddd", x, y, z) + b"\x01")  # onGround=true

    def set_creative_slot(self, slot, item_id, count=1):
        # Set Creative Mode Slot 0x32: сервер читает slot + count + itemId, хвост игнорирует
        self.send_packet(0x32, struct.pack(">h", slot) + enc_varint(count) + enc_varint(item_id))

    def clear_slot(self, slot):
        # SMOKE_V4: count=0 — сервер трактует слот как пустой (invItemId=0, state=-1)
        self.send_packet(0x32, struct.pack(">h", slot) + enc_varint(0))

    def use_item(self, hand):
        self.send_packet(0x39, enc_varint(hand) + enc_varint(0))  # Use Item (поднять щит)

    def release_use(self):
        # Player Action 0x24, status 5 = Release Use Item (опустить щит)
        self.send_packet(0x24, enc_varint(5) + struct.pack(">Q", 0) + b"\x00" + enc_varint(0))

    def find_by_uuid(self, target_uuid, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            with self.lock:
                for eid, u in self.spawned.items():
                    if u == target_uuid:
                        return eid
            time.sleep(0.2)
        return None

    # ---------- жизненный цикл ----------
    def run(self):
        try:
            self._run()
        except Exception as e:
            self.error = f"{type(e).__name__}: {e}"
            self.in_play.set()
            self.positioned.set()

    def _run(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=20)
        self.sock.settimeout(40)
        # Handshake -> Login
        self.send_packet(0x00, enc_varint(PROTOCOL) + enc_string(self.host)
                         + struct.pack(">H", self.port) + enc_varint(2))
        u = uuidlib.uuid3(uuidlib.NAMESPACE_OID, "OfflinePlayer:" + self.bot_name)
        self.send_packet(0x00, enc_string(self.bot_name) + u.bytes)
        state = "login"
        while True:
            data = self.read_frame()
            r = Reader(data)
            pid = r.varint()
            if state == "login":
                if pid == 0x00:
                    raise ConnectionError("кик на логине: " + r.string())
                elif pid == 0x01:
                    raise ConnectionError("сервер требует шифрование — включи offline-режим")
                elif pid == 0x03:
                    self.compress = r.varint()
                elif pid == 0x02:
                    self.my_uuid = r.bytes_(16)  # UUID, который выдал сервер
                    self.send_packet(0x03)  # Login Acknowledged
                    state = "config"
                    # Client Information + Known Packs (пустой список)
                    self.send_packet(0x00, enc_string("ru_RU") + b"\x08" + enc_varint(0)
                                     + b"\x01\x7f" + enc_varint(1) + b"\x00\x01")
                    self.send_packet(0x07, enc_varint(0))
            elif state == "config":
                if pid == 0x02:
                    raise ConnectionError("кик в конфигурации")
                elif pid == 0x04:
                    self.send_packet(0x04, r.d[r.o:r.o + 8])
                elif pid == 0x03:
                    self.send_packet(0x03)  # Ack Finish Configuration
                    state = "play"
                    self.in_play.set()
            else:
                self.dispatch_play(pid, r)

    def dispatch_play(self, pid, r):
        try:
            if pid == 0x26:  # Keep Alive
                self.send_packet(0x18, r.d[r.o:r.o + 8])
            elif pid == 0x2B:  # Join Game
                self.eid = r.i32()
            elif pid == 0x40:  # Synchronize Player Position
                x = r.f64(); y = r.f64(); z = r.f64()
                r.f32(); r.f32(); r.u8()
                tid = r.varint()
                self.send_packet(0x00, enc_varint(tid))  # Teleport Confirm
                self.pos = (x, y, z)
                self.positioned.set()
            elif pid in (0x0C, 0x0D):  # Chunk Batch Start/Finished
                if r.remaining():  # Finished несёт varint -> отвечаем Chunk Batch Received
                    self.send_packet(0x08, struct.pack(">f", 25.0))
            elif pid == 0x5D:  # Update Health
                self.health = r.f32()
                if self.health <= 0:
                    self.died = True
            elif pid == 0x3C:  # Combat Death
                self.death_packet = True
                self.died = True
            elif pid == 0x13:  # Container Set Content
                if r.u8() == 0:
                    with self.lock:
                        self.got_inventory += 1
            elif pid == 0x1F:  # Entity Event
                eid = r.i32(); st = r.u8()
                if eid == self.eid and 24 <= st <= 28:
                    with self.lock:
                        self.perm_events.append(st)
            elif pid == 0x01:  # Spawn Entity
                eid = r.varint()
                euuid = r.bytes_(16)
                r.varint()  # тип
                if eid != self.eid:
                    with self.lock:
                        self.spawned[eid] = euuid
                        self.spawn_counts[eid] = self.spawn_counts.get(eid, 0) + 1
            elif pid == 0x42:  # Remove Entities
                n = r.varint()
                with self.lock:
                    for _ in range(n):
                        self.removed.add(r.varint())
            elif pid == 0x5A:  # Entity Velocity (отброс) — SMOKE_V3
                if r.varint() == self.eid:
                    with self.lock:
                        self.velocity_events += 1
            elif pid == 0x58:  # Set Entity Metadata (byte-поля, напр. hand states index 8) — SMOKE_V3
                meid = r.varint()
                while r.remaining() > 0:
                    idx = r.u8()
                    if idx == 0xFF:
                        break
                    if r.varint() != 0:
                        break  # не byte-сериализатор — дальше не разбираем
                    val = r.u8()
                    with self.lock:
                        self.metadata.setdefault(meid, {})[idx] = val
            elif pid == 0x64:  # Time Update
                r.i64()
                self.time_of_day = r.i64()
            elif pid == 0x27:  # Chunk Data
                self.parse_chunk(r)
        except Exception:
            with self.lock:
                self.chunk_parse_errors += 1

    def parse_chunk(self, r):
        r.i32(); r.i32()
        r.nbt_skip_network()
        size = r.varint()
        sub = Reader(r.d[r.o:r.o + size])
        occ = []
        biome0 = None
        for s in range(24):
            bc = sub.i16()
            sv = sub.paletted()      # блоки
            bio = sub.paletted()     # биомы
            if s == self.ground_section and bio is not None:
                biome0 = bio
            if bc > 0 or (sv is not None and sv != 0):
                occ.append(s)
        with self.lock:
            self.chunks_total += 1
            if 0 in occ:
                self.chunks_bad += 1
            if self.ground_section in occ:
                self.chunks_good += 1
            if biome0 is not None:
                self.biome_single = biome0


class Suite:
    def __init__(self):
        self.results = []

    def check(self, name, ok, info=""):
        mark = "PASS" if ok else "FAIL"
        line = f"[{mark}] {name}" + (f" — {info}" if info else "")
        print(line, flush=True)
        self.results.append((name, ok))

    def summary(self):
        passed = sum(1 for _, ok in self.results if ok)
        failed = len(self.results) - passed
        print("-" * 60, flush=True)
        print(f"Итог: {passed} PASS, {failed} FAIL из {len(self.results)}", flush=True)
        return failed == 0


def hit_and_measure(attacker, victim, target_eid, wait_before=1.2, wait_after=0.9):
    """Полный замах (ждём кулдаун + окно i-frames), один удар, замер урона."""
    time.sleep(wait_before)
    h0 = victim.health
    attacker.attack(target_eid)
    time.sleep(wait_after)
    return h0 - victim.health


def main():
    ap = argparse.ArgumentParser(description="Zevvoryn smoke bot (1.21.1)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=25565)
    ap.add_argument("--grass-y", type=int, default=3,
                    help="ожидаемый Y травы флэта (FLATNATIVE_V1: 3)")
    args = ap.parse_args()

    ground_section = (args.grass_y + 64) // 16
    spawn_y = args.grass_y + 1
    s = Suite()
    print(f"SMOKE_V3: сервер {args.host}:{args.port}, ожидаем траву на Y={args.grass_y}, спавн Y={spawn_y}", flush=True)

    a = Bot("SmokeBotA", args.host, args.port, ground_section)
    b = Bot("SmokeBotB", args.host, args.port, ground_section)
    a.start()
    if not a.in_play.wait(30) or a.error:
        s.check("LOGIN", False, a.error or "таймаут входа бота A")
        s.summary()
        sys.exit(1)
    b.start()
    if not b.in_play.wait(30) or b.error:
        s.check("LOGIN", False, b.error or "таймаут входа бота B")
        s.summary()
        sys.exit(1)
    s.check("LOGIN", True, "оба бота в игре")

    a.positioned.wait(20)
    b.positioned.wait(20)
    time.sleep(6)  # даём чанкам доехать

    s.check("SPAWN_Y", a.pos is not None and abs(a.pos[1] - spawn_y) < 0.01,
            f"спавн Y={a.pos[1] if a.pos else '?'} (ожидался {spawn_y}); если тут -60 — старый world/spawn.dat")
    s.check("CHUNKS", a.chunks_total > 0 and a.chunks_bad == 0,
            f"чанков: {a.chunks_total}, старых (блоки на -64..-49): {a.chunks_bad}, "
            f"на родной высоте: {a.chunks_good}, ошибок парсинга: {a.chunk_parse_errors}")
    s.check("BIOME", a.biome_single == 39,
            f"биом секции земли: {a.biome_single} (ожидался plains=39)")

    # оба в выживание и фиксируем позиции
    b.send_command("gm0 SmokeBotA")
    b.send_command("gm0 SmokeBotB")
    time.sleep(1.0)

    # SMOKE_V4: сервер хранит инвентарь по оффлайн-UUID между запусками —
    # без сброса прошлый алмазный меч у B и железо у A портят IFRAME/SWORD (урон 3.78 вместо ~1/~7)
    for bot in (a, b):
        for slot in (5, 6, 7, 8, 36, 45):  # броня, активный хотбар-слот, оффхенд
            bot.clear_slot(slot)
    time.sleep(0.5)
    for _ in range(3):
        a.send_pos()
        b.send_pos()
        time.sleep(0.1)

    # SMOKE_V2: ищем бота A строго по UUID из его Login Success —
    # посторонние игроки на сервере больше не перехватывают прицел
    target = b.find_by_uuid(a.my_uuid, 10)
    s.check("VISIBILITY", target is not None,
            "B не нашёл бота A по UUID" if target is None else f"A найден по UUID, eid={target}")
    if target is None:
        s.summary()
        sys.exit(1)

    # PVP: быстрый даблклик кулаком (проверка i-frames с досчётом разницы)
    h0 = a.health
    b.attack(target)
    time.sleep(0.1)
    b.attack(target)
    time.sleep(0.9)
    drop = h0 - a.health
    s.check("PVP_HIT", drop > 0, f"хп A: {h0:.1f} -> {a.health:.1f} (урон {drop:.2f})")
    s.check("IFRAME", 0 < drop <= 1.55,
            f"урон за даблклик {drop:.2f} (ожидалось ~1; без i-frames было бы ~2)")

    # добиваем до смерти
    t0 = time.time()
    while not a.died and time.time() - t0 < 90:
        b.attack(target)
        a.send_pos()
        time.sleep(0.55)
    s.check("DEATH", a.died and a.death_packet,
            f"хп={a.health:.1f}, пакет смерти 0x3C: {a.death_packet}")
    time.sleep(1.5)
    s.check("DEATHVIS", target in b.removed,
            "труп убран у B (0x42)" if target in b.removed else "B не получил Remove Entities — труп стоит")

    # респавн
    inv_before = a.got_inventory
    perm_before = len(a.perm_events)
    spawn_before = b.spawn_counts.get(target, 0)
    a.positioned.clear()
    a.respawn()
    got_tp = a.positioned.wait(10)
    time.sleep(2.0)
    s.check("RESPAWN", got_tp and a.health > 0,
            f"телепорт: {got_tp}, хп={a.health:.1f}, Y={a.pos[1] if a.pos else '?'}")
    s.check("INVRESPAWN", a.got_inventory > inv_before,
            f"пакетов инвентаря после респавна: {a.got_inventory - inv_before}")
    perm_new = a.perm_events[perm_before:]
    s.check("PERMLEVEL", bool(perm_new) and max(perm_new) >= 26,
            f"события прав после респавна: {perm_new} (нужно >=26 для F3+F4)")
    s.check("RESPAWNVIS", b.spawn_counts.get(target, 0) > spawn_before,
            f"спавнов A у B: {spawn_before} -> {b.spawn_counts.get(target, 0)}")

    # фиксируем позиции после респавна
    for _ in range(3):
        a.send_pos()
        b.send_pos()
        time.sleep(0.1)

    # SWORD: алмазный меч в хотбар B (слот 36 = активный), полный замах -> ~7
    b.set_creative_slot(36, DIAMOND_SWORD)
    v0 = a.velocity_events
    drop = hit_and_measure(b, a, target)
    s.check("SWORD", 6.5 <= drop <= 7.5,
            f"алмазный меч, полный замах: урон {drop:.2f} (ожидалось ~7)")
    s.check("KB", a.velocity_events > v0,
            f"пакетов отброса при обычном ударе: +{a.velocity_events - v0} (должен быть отброс)")

    # ARMOR: полное железо на A (слоты 5-8), меч 7 -> red=15-3.5=11.5 -> 7*(1-11.5/25)=3.78
    for slot, item in zip((5, 6, 7, 8), IRON_ARMOR):
        a.set_creative_slot(slot, item)
    drop = hit_and_measure(b, a, target)
    s.check("ARMOR", 3.2 <= drop <= 4.4,
            f"полное железо против меча: урон {drop:.2f} (ожидалось ~3.78, ванильная формула)")

    # SHIELD: щит в оффхенд A (слот 45), B встаёт ПЕРЕД лицом A (yaw 0 = взгляд на +Z)
    a.set_creative_slot(45, SHIELD)
    ax, ay, az = a.pos
    b.pos = (ax, ay, az + 2.0)
    for _ in range(3):
        b.send_pos()
        a.send_pos()
        time.sleep(0.1)
    a.use_item(1)  # поднять щит (оффхенд)
    time.sleep(0.5)
    v0 = a.velocity_events
    drop = hit_and_measure(b, a, target, wait_before=0.8)
    s.check("SHIELD", abs(drop) < 0.01,
            f"удар в поднятый щит спереди: урон {drop:.2f} (ожидался 0)")
    s.check("SHIELD_NOKB", a.velocity_events == v0,
            f"пакетов отброса при блоке: +{a.velocity_events - v0} (отброса быть не должно, SHIELD_V2)")

    # SHIELD_DOWN: опускаем щит — урон снова проходит (сквозь броню ~3.78)
    a.release_use()
    drop = hit_and_measure(b, a, target)
    s.check("SHIELD_DOWN", drop > 0.5,
            f"щит опущен: урон {drop:.2f} (должен проходить)")

    # SHIELD_NEWVIS: A снова поднимает щит, заходит бот C — новичок должен видеть щит (SHIELD_V2)
    a.use_item(1)
    time.sleep(0.4)
    c = Bot("SmokeBotC", args.host, args.port, ground_section)
    c.start()
    c_ok = c.in_play.wait(30) and not c.error
    time.sleep(2.5)
    hs = c.metadata.get(target, {}).get(8, 0) if c_ok else -1
    s.check("SHIELD_NEWVIS", bool(c_ok and (hs & 0x01)),
            f"hand state A у нового игрока C: {hs} (bit0 = щит поднят)" if c_ok
            else f"бот C не зашёл: {c.error}")

    # AXE_BREAK: топор об поднятый щит — урон 0, но щит отключается на 5 с (как в ванилле)
    b.set_creative_slot(36, DIAMOND_AXE)
    drop = hit_and_measure(b, a, target, wait_before=1.4)
    s.check("AXE_BREAK", abs(drop) < 0.01,
            f"топор в щит: урон {drop:.2f} (ожидался 0, щит ушёл в кд)")

    # SHIELD_CD: во время кд поднять щит нельзя — меч проходит
    a.use_item(1)  # попытка поднять во время кд — сервер должен игнорить
    time.sleep(0.3)
    b.set_creative_slot(36, DIAMOND_SWORD)
    drop = hit_and_measure(b, a, target)
    s.check("SHIELD_CD", drop > 0.5,
            f"удар во время кд щита: урон {drop:.2f} (должен проходить)")

    # SHIELD_RECOVER: через 5 с кд истёк — щит снова блокирует
    time.sleep(5.5)
    a.use_item(1)
    time.sleep(0.4)
    drop = hit_and_measure(b, a, target, wait_before=0.8)
    s.check("SHIELD_RECOVER", abs(drop) < 0.01,
            f"после кд: урон {drop:.2f} (ожидался 0 — щит снова блокирует)")

    # время
    b.send_command("time set 6000")
    time.sleep(2.0)
    t = a.time_of_day
    s.check("TIME", t is not None and 6000 <= t <= 6300, f"time у A: {t}")

    ok = s.summary()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nПрервано", flush=True)
        sys.exit(130)
