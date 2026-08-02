#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
SMOKE_V13: смоук-бот для сервера Zevvoryn (Minecraft Java 1.21.1, offline mode).

Подключается двумя ботами и проверяет:
  LOGIN      — вход обоих ботов (handshake -> login -> configuration -> play)
  CROWD_BOTS — дополнительно держит ещё 15 ботов онлайн для нагрузки
  CROWD_SWARM — первые crowd-боты реально гоняют параллельные physics-сценарии в отдельных зонах
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
  --- физика (PHYS_V2), отдельный бот SmokeBotP ---
  ITEM_SPAWN     — ломание блока роняет ровно один предмет-сущность (тип 58)
  ITEM_ARC       — предмет летит по дуге (гравитация), а не падает мгновенно на пол (0x70)
  ITEM_PICKUP    — предмет подбирается (Take Item 0x6F)
  PICKUP_COUNT_1 — за раз подбирается ровно 1 (проверка бага «2 блока»)
  ITEM_MERGE     — два одинаковых предмета в одной точке сливаются в один стак (0x42)
  FALL_SMALL     — падение на 2 блока не наносит урона
  FALL_BIG       — падение с ~12 блоков на траву наносит урон (0x5D)
  FALL_WATER     — приземление в воду: урон 0 (FALLSOFT_V1)
  FALL_COBWEB    — приземление в паутину: урон 0 (FALLSOFT_V1)
  FALL_HONEY     — приземление на мёд: урон 0 (FALLSOFT_V1)
  FALL_SLIME     — приземление на слизь: урон 0 + отскок вверх (BOUNCE_V1, 0x5A)
  FALL_HAY       — приземление на стог сена: урон снижен на ~80% (FALLSOFT_V1)
  ITEM_REST      — предмет ложится на землю и останавливается на целочисленном Y (пол+трение)
  ITEM_FLOAT     — предмет всплывает в воде (плавучесть, vy += 0.0005)
  ITEM_ICE_SLIDE — брошенный предмет скользит по льду далеко (низкое трение 0.98 vs 0.6)
  FALLING_SPAWN  — песок через 2 тика превращается в FallingBlockEntity (тип 40)
  FALLING_ARC    — FallingBlockEntity падает по серверной траектории, а не телепортируется
  FALLING_LAND   — после приземления entity исчезает и песок снова становится блоком
  FALLING_CHAIN  — вертикальная колонна гравия обрушивается сверху вниз
  CONCRETE_WATER — concrete powder при падении в воду твердеет в matching concrete
  CONCRETE_PLACE_WATER — поставленный прямо в воду powder сразу твердеет
  CONCRETE_ADJACENT — вода сбоку твердеет поставленный powder
  CONCRETE_FLOW_IN — вода, пришедшая позже, твердеет стоящий powder
  ANVIL_DAMAGE   — падающая наковальня наносит урон и удаляет entity
  TNT_FUSE/ARC/EXPLODE/CHAIN — primed TNT летит, взрывает блоки и поджигает соседний TNT
  PEARL_SPAWN/ARC/HIT — ender pearl летит по raycast, телепортирует и наносит 5 урона
  PEARL_COOLDOWN/SWING — двойной use создаёт один pearl, cooldown 20->0, один swing
  SNOWBALL_SPAWN/ARC/HIT — снежок спавнится, летит по серверной дуге и исчезает при ударе
  EGG_SPAWN/ARC/HIT — яйцо спавнится, летит по серверной дуге и исчезает при ударе
  XP_BOTTLE_SPAWN/ARC/HIT/GAIN — пузырёк опыта летит по общей throwable-физике, бьётся при ударе и реально даёт опыт
  FIRE_PLACE/SPREAD/AFTERBURN/DECAY — огонь ставится, распространяется, жжёт после выхода и тухнет
  FIRE_NO_GRASS_WAVE — grass_block не считается топливом и не запускает волну
  BUCKET_REUSE — survival water bucket работает place→pickup→place повторно
  MINIEDIT_SET/REPLACE/UNDO/REDO/COPY_ROTATE_PASTE — встроенный редактор и section batch packets
  MINIEDIT_OUTLINE — красная dust-рамка выделения, только рёбра
  --- измерения и порталы (SMOKE_DIM_V1), отдельный бот SmokeBotD ---
  PORTAL_LIT / PORTAL_NO_CORNERS — рамка без углов зажигается огнивом (PORTAL_V2)
  PORTAL_TRAVEL / PORTAL_SCALE — переход в Ад и масштаб 1:8
  NETHER_TERRAIN / NETHER_CEILING / NETHER_BIOMES — реальная генерация вместо флэта (DIMGEN_V1)
  NETHER_FALLING / NETHER_FLUID — гравитация и жидкости тикают в Аду (DIMPHYS_V1)
  END_TRAVEL / END_ISLAND / END_VOID / END_BIOMES / END_FALLING — острова Энда и физика в нём
  ENDPORTAL_EYES / ENDPORTAL_FILL / ENDPORTAL_TRAVEL / END_PLATFORM — кольцо рамок, глаза, высадка на платформу
  MINIEDIT_WAND_NAME — деревянный топор приходит с custom_name Builder's Wand

Запуск:  python tools/smoke_bot.py [--host 127.0.0.1] [--port 25565]
         [--crowd-bots 29] [--time-scale auto|1.5] [--any-world]
SMOKE_V15: физ-бот и dim-бот заходят первыми, а размер толпы считается по max-players
из статус-пинга — больше никто не получает «Сервер заполнен».
SMOKE_V14: перед прогоном проверяется, что мир действительно ФЛЭТ (иначе стоп за 20 секунд,
а не 80 ложных FAIL), измеряется реальный TPS сервера и все ожидания растягиваются
под слабый CPU, а все фазы (толпа + физика + измерения) идут параллельно.
Требования: Python 3.10+, сервер в offline-режиме, pvp=true, ops пустой,
генератор FLAT, лучше на свежем мире. Зрители на сервере не ломают
прицеливание (боты ищут друг друга по UUID), но не бейте ботов во время прогона :)
"""
import argparse
import json
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
# PHYS_V2: item-id блоков для тестов мягкого приземления (см. core/item_blocks.gen.hpp)
COBWEB_ITEM = 194     # -> состояние 2004 (паутина гасит урон)
SLIME_ITEM = 664      # -> состояние 10364 (слизь: отскок без урона)
HAY_ITEM = 445        # -> состояние 10726 (стог: -80% урона)
HONEY_ITEM = 665      # -> состояние 19445 (мёд гасит урон)
WATER_BUCKET = 909    # -> состояние 80 (вода гасит урон)
ICE_ITEM = 306        # -> состояние 5780 (лёд: трение 0.98, предмет скользит)
ANVIL_ITEM = 419      # -> состояние 9107
WHITE_POWDER_ITEM = 571  # -> состояние 12744; matching concrete = 12728
FLINT_AND_STEEL = 798
SNOWBALL = 912
EGG = 927
ENDER_PEARL = 993
EXPERIENCE_BOTTLE = 1088
DIAMOND_HOE = 842      # 818 + tier4*5 + hoe (HOE_TILL_V1)
WHEAT_SEEDS_ITEM = 853 # SEED_PLANT_V1 / BONE_MEAL_V1
BONE_MEAL_ITEM = 960   # BONE_MEAL_V1: minecraft:bone_meal
# SMOKE_DIM_V1: real Nether/End generation + portals
OBSIDIAN_ITEM = 290
SAND_ITEM = 57
LAVA_BUCKET = 910
END_PORTAL_FRAME_ITEM = 376
ENDER_EYE_ITEM = 1006
OVERWORLD_ID = "minecraft:overworld"
NETHER_ID = "minecraft:the_nether"
END_ID = "minecraft:the_end"
NETHER_BIOMES = {2, 7, 34, 48, 58}   # basalt_deltas, crimson, wastes, soul_sand, warped
END_BIOMES = {16, 17, 18, 43, 55}    # barrens, highlands, midlands, small_islands, the_end
FALLING_BLOCK_TYPE = 40


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

    def varlong(self):
        v = 0
        sh = 0
        while True:
            b = self.u8()
            v |= (b & 0x7F) << sh
            if not (b & 0x80):
                return v
            sh += 7

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
        # nearest_spawn вызывается и внутри уже защищённых снимков тестов;
        # RLock не допускает дедлок при таком вложенном чтении.
        self.lock = threading.RLock()
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
        self.entity_types = {}          # PHYS_V2: eid -> тип сущности (58 = выпавший предмет)
        self.entity_spawn = {}          # PHYS_V4: eid -> (x,y,z,data), точный выбор entity тестом
        self.entity_pos = {}            # PHYS_V7: текущая позиция для relative move 0x2E/0x2F
        self.item_tp = {}               # PHYS_V2: eid -> [(x,y,z),...] из Teleport Entity 0x70 (траектория)
        self.take_events = []           # PHYS_V2: (collected_eid, collector_eid, count) из 0x6F
        self.block_updates = []          # PHYS_V3: (x,y,z,state) из Block Update 0x09
        self.cooldown_events = []        # FIRE/PEARL_V3: (item_id, ticks), packet 0x17
        self.arm_animations = []         # PEARL_V3: (eid, action), packet 0x03
        self.section_update_packets = 0  # MINIEDIT_V1: batched section updates 0x49
        self.particle_packets = 0        # MINIEDIT_V1: red selection outline 0x29
        self.wand_name = None            # MINIEDIT_V2: custom_name component on item 821
        self.wand_color = None
        self.time_of_day = None
        self.time_stamps = []      # SMOKE_V14: метки Update Time — по ним считается реальный TPS
        self.experience_bar = None
        self.experience_level = 0
        self.experience_total = 0
        self.chunks_total = 0
        self.chunks_bad = 0             # чанки с блоками в секции 0 (Y=-64..-49)
        self.chunks_good = 0            # чанки с блоками в ожидаемой секции
        self.chunk_parse_errors = 0
        self.biome_single = None
        self.chunk_profiles = {}   # SMOKE_DIM_V1: (cx,cz) -> block counts of every section
        self.chunk_biomes = {}     # SMOKE_DIM_V1: (cx,cz) -> biome ids of every section
        self.dim_name = OVERWORLD_ID  # SMOKE_DIM_V1: current dimension from Respawn 0x47
        self.dim_changes = []
        self.in_play = threading.Event()
        self.positioned = threading.Event()

    def reset_chunk_stats(self):
        """SMOKE_DIM_V1: drop the terrain profile before entering a new dimension."""
        with self.lock:
            self.chunk_profiles.clear()
            self.chunk_biomes.clear()

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

    def send_pos_raw(self, x, y, z, on_ground):
        # PHYS_V2: Player Position 0x1A с произвольным onGround — для симуляции падения
        self.send_packet(0x1A, struct.pack(">ddd", x, y, z) + (b"\x01" if on_ground else b"\x00"))
        self.pos = (x, y, z)

    def send_pos_rot(self, x, y, z, yaw, pitch, on_ground):
        # PHYS_V2: Player Position And Rotation 0x1B — задаём взгляд (для детерминированного броска)
        self.send_packet(0x1B, struct.pack(">ddd", x, y, z) + struct.pack(">ff", yaw, pitch)
                         + (b"\x01" if on_ground else b"\x00"))
        self.pos = (x, y, z)

    @staticmethod
    def enc_block_pos(x, y, z):
        val = ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)
        return struct.pack(">Q", val & 0xFFFFFFFFFFFFFFFF)

    def break_block(self, x, y, z):
        # PHYS_V2: Player Action 0x24, status 2 (finish digging) — в выживании сервер ломает блок
        self.send_packet(0x24, enc_varint(2) + self.enc_block_pos(x, y, z) + b"\x01" + enc_varint(0))

    def drop_one(self):
        # PHYS_V2: Player Action 0x24, status 4 (drop single) — выброс одного предмета
        self.send_packet(0x24, enc_varint(4) + struct.pack(">Q", 0) + b"\x00" + enc_varint(0))

    def place_block(self, x, y, z, face=1):
        # PHYS_V2: Use Item On 0x38 — ставит текущий блок хотбара (hotbarBlockState[held])
        # на грань блока (x,y,z). face=1 -> блок появляется сверху, на (x,y+1,z).
        payload = (enc_varint(0) + self.enc_block_pos(x, y, z) + enc_varint(face)
                   + struct.pack(">fff", 0.5, 1.0, 0.5) + b"\x00" + enc_varint(0))
        self.send_packet(0x38, payload)

    def set_creative_slot(self, slot, item_id, count=1):
        # Set Creative Mode Slot 0x32: сервер читает slot + count + itemId, хвост игнорирует
        self.send_packet(0x32, struct.pack(">h", slot) + enc_varint(count) + enc_varint(item_id))

    def clear_slot(self, slot):
        # SMOKE_V4: count=0 — сервер трактует слот как пустой (invItemId=0, state=-1)
        self.send_packet(0x32, struct.pack(">h", slot) + enc_varint(0))

    def use_item(self, hand):
        # 1.21.1 ServerboundUseItemPacket: hand, sequence, yaw, pitch.
        # После BUCKET_V2 сервер читает полный пакет; старый короткий вариант
        # обрывал соединение smoke-бота после первого поднятия щита.
        self.send_packet(0x39, enc_varint(hand) + enc_varint(0) + struct.pack(">ff", 0.0, 0.0))

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
            if pid == 0x03:  # Animate Entity
                with self.lock: self.arm_animations.append((r.varint(), r.u8()))
            elif pid == 0x17:  # Set Cooldown (protocol 767; 0x16 is Cookie Request)
                with self.lock: self.cooldown_events.append((r.varint(), r.varint()))
            elif pid == 0x26:  # Keep Alive
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
                etype = r.varint()  # тип сущности (PHYS_V2: 58 = предмет)
                ex, ey, ez = r.f64(), r.f64(), r.f64()
                r.u8(); r.u8(); r.u8()  # pitch / yaw / head yaw
                edata = r.varint()
                if eid != self.eid:
                    with self.lock:
                        self.spawned[eid] = euuid
                        self.spawn_counts[eid] = self.spawn_counts.get(eid, 0) + 1
                        self.entity_types[eid] = etype
                        self.entity_spawn[eid] = (ex, ey, ez, edata)
                        self.entity_pos[eid] = (ex, ey, ez)
                        self.item_tp.setdefault(eid, []).append((ex, ey, ez))
            elif pid == 0x09:  # Block Update — PHYS_V3
                raw = r.i64() & 0xFFFFFFFFFFFFFFFF
                x = (raw >> 38) & 0x3FFFFFF
                z = (raw >> 12) & 0x3FFFFFF
                y = raw & 0xFFF
                if x & 0x2000000: x -= 0x4000000
                if z & 0x2000000: z -= 0x4000000
                if y & 0x800: y -= 0x1000
                state = r.varint()
                with self.lock:
                    self.block_updates.append((x, y, z, state))
            elif pid == 0x49:  # Section Blocks Update — MINIEDIT_V1
                raw = r.i64() & 0xFFFFFFFFFFFFFFFF
                sx = (raw >> 42) & 0x3FFFFF
                sz = (raw >> 20) & 0x3FFFFF
                sy = raw & 0xFFFFF
                if sx & 0x200000: sx -= 0x400000
                if sz & 0x200000: sz -= 0x400000
                if sy & 0x80000: sy -= 0x100000
                count = r.varint()
                decoded = []
                for _ in range(count):
                    entry = r.varlong()
                    rel, state = entry & 0xFFF, entry >> 12
                    x = (sx << 4) + ((rel >> 8) & 15)
                    z = (sz << 4) + ((rel >> 4) & 15)
                    y = (sy << 4) + (rel & 15)
                    decoded.append((x, y, z, state))
                with self.lock:
                    self.section_update_packets += 1
                    self.block_updates.extend(decoded)
            elif pid == 0x29:  # Level Particles — MINIEDIT_V1 wireframe
                with self.lock:
                    self.particle_packets += 1
            elif pid == 0x15:  # Container Set Slot — MINIEDIT_V2 named wand
                r.u8(); r.varint(); r.i16()
                count = r.varint()
                if count > 0:
                    item = r.varint(); added = r.varint(); removed = r.varint()
                    for _ in range(added):
                        component_type = r.varint()
                        if item == 821 and component_type == 5:
                            tag = r.u8()
                            if tag == 0x08:
                                name_len = r.u16()
                                with self.lock: self.wand_name = r.bytes_(name_len).decode("utf-8", "replace")
                            elif tag == 0x0A:
                                values = {}
                                while True:
                                    child = r.u8()
                                    if child == 0: break
                                    key_len = r.u16(); key = r.bytes_(key_len).decode("utf-8", "replace")
                                    if child == 0x08:
                                        value_len = r.u16(); values[key] = r.bytes_(value_len).decode("utf-8", "replace")
                                    elif child == 0x01:
                                        values[key] = r.u8()
                                    else: break
                                with self.lock:
                                    self.wand_name = values.get("text")
                                    self.wand_color = values.get("color")
                        else:
                            break  # current server sends no other added SetSlot components
            elif pid == 0x42:  # Remove Entities
                n = r.varint()
                with self.lock:
                    for _ in range(n):
                        self.removed.add(r.varint())
            elif pid == 0x6F:  # Take Item Entity (подбор) — PHYS_V2
                collected = r.varint(); collector = r.varint(); cnt = r.varint()
                with self.lock:
                    self.take_events.append((collected, collector, cnt))
            elif pid == 0x70:  # Teleport Entity (абс. позиция) — PHYS_V2: траектория предмета (x,y,z)
                teid = r.varint(); tx = r.f64(); ty = r.f64(); tz = r.f64()
                with self.lock:
                    self.entity_pos[teid] = (tx, ty, tz)
                    self.item_tp.setdefault(teid, []).append((tx, ty, tz))
            elif pid in (0x2E, 0x2F):  # Relative Entity Position[/Rotation], scale 4096
                teid = r.varint(); dx = r.i16() / 4096.0; dy = r.i16() / 4096.0; dz = r.i16() / 4096.0
                if pid == 0x2F: r.u8(); r.u8()
                r.u8()  # onGround bool
                with self.lock:
                    ox, oy, oz = self.entity_pos.get(teid, (0.0, 0.0, 0.0))
                    pos = (ox + dx, oy + dy, oz + dz)
                    self.entity_pos[teid] = pos
                    self.item_tp.setdefault(teid, []).append(pos)
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
            elif pid == 0x5C:  # Set Experience
                self.experience_bar = r.f32()
                self.experience_level = r.varint()
                self.experience_total = r.varint()
            elif pid == 0x64:  # Time Update
                r.i64()
                self.time_of_day = r.i64()
                with self.lock:
                    self.time_stamps.append(time.time())   # SMOKE_V14
            elif pid == 0x47:  # Respawn - SMOKE_DIM_V1: dimension switch
                r.varint()
                dim_name = r.string()
                with self.lock:
                    self.dim_name = dim_name
                    self.dim_changes.append(dim_name)
            elif pid == 0x27:  # Chunk Data
                self.parse_chunk(r)
        except Exception:
            with self.lock:
                self.chunk_parse_errors += 1

    def parse_chunk(self, r):
        cx = r.i32(); cz = r.i32()   # SMOKE_DIM_V1: chunk coords feed the terrain profile
        r.nbt_skip_network()
        size = r.varint()
        sub = Reader(r.d[r.o:r.o + size])
        occ = []
        counts = []          # SMOKE_DIM_V1
        biomes = []          # SMOKE_DIM_V1
        biome0 = None
        for s in range(24):
            if sub.remaining() < 3:
                break        # SMOKE_DIM_V1: the Nether/End columns are shorter than 24 sections
            bc = sub.i16()
            counts.append(bc)
            sv = sub.paletted()      # блоки
            bio = sub.paletted()     # биомы
            if bio is not None:
                biomes.append(bio)   # SMOKE_DIM_V1
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
            self.chunk_profiles[(cx, cz)] = counts   # SMOKE_DIM_V1
            self.chunk_biomes[(cx, cz)] = biomes


class Suite:
    def __init__(self):
        self.results = []
        self.skipped = []        # SMOKE_V14
        self.started = time.time()
        self.lock = threading.Lock()

    def skip(self, name, info=""):
        """SMOKE_V14: тест неприменим к этому миру — это не провал."""
        line = "[SKIP] %s" % name + ((" — %s" % info) if info else "")
        with self.lock:
            print(line, flush=True)
            self.skipped.append(name)

    def check(self, name, ok, info=""):
        mark = "PASS" if ok else "FAIL"
        line = f"[{mark}] {name}" + (f" — {info}" if info else "")
        with self.lock:
            print(line, flush=True)
            self.results.append((name, ok))

    def summary(self):
        with self.lock:
            passed = sum(1 for _, ok in self.results if ok)
            failed = len(self.results) - passed
            print("-" * 60, flush=True)
            mins, secs = divmod(int(time.time() - self.started), 60)
            skipped = len(self.skipped)
            print(f"Итог: {passed} PASS, {failed} FAIL, {skipped} SKIP из {len(self.results) + skipped}"
                  f" — прогон занял {mins} мин {secs} с", flush=True)
        return failed == 0


def hit_and_measure(attacker, victim, target_eid, wait_before=1.2, wait_after=0.9):
    """Полный замах (ждём кулдаун + окно i-frames), один удар, замер урона."""
    time.sleep(wait_before)
    h0 = victim.health
    attacker.attack(target_eid)
    time.sleep(wait_after)
    return h0 - victim.health


def nearest_spawn(bot, ids, x, z, max_distance=2.0):
    """Выбирает entity конкретного теста по spawn X/Z, а не случайный eid из set."""
    with bot.lock:
        spawns = dict(bot.entity_spawn)
    ranked = []
    for eid in ids:
        pos = spawns.get(eid)
        if pos is None:
            continue
        d2 = (pos[0] - x) ** 2 + (pos[2] - z) ** 2
        if d2 <= max_distance * max_distance:
            ranked.append((d2, eid))
    return min(ranked)[1] if ranked else None


def block_fall_test(p, s, name, item_id, bx, bz, grass_y, mode):
    """PHYS_V2: ставит блок на (bx,grass_y+1,bz) и меряет урон при падении на него сверху.

    mode: 'none' — урон должен быть 0; 'reduced' — небольшой (стог, -80%);
          'bounce' — 0 урона + отскок (Entity Velocity 0x5A на свой eid).
    """
    if p.health <= 0.5:  # RESPAWN_V1: если умерли в прошлом тесте — возрождаемся (HP=20)
        p.respawn(); time.sleep(1.2)
    p.set_creative_slot(36, item_id, 1)   # блок в активный слот -> hotbarBlockState[0]
    time.sleep(0.35)
    p.place_block(bx, grass_y, bz, 1)     # клик сверху по грунту -> блок на grass_y+1
    time.sleep(0.7)
    cx, cz = bx + 0.5, bz + 0.5
    for _ in range(2):
        p.send_pos_raw(cx, grass_y + 2, cz, True); time.sleep(0.15)
    h0 = p.health
    v0 = p.velocity_events
    p.send_pos_raw(cx, grass_y + 22, cz, False); time.sleep(0.3)   # пик над блоком
    p.send_pos_raw(cx, grass_y + 2, cz, True);   time.sleep(0.8)   # приземление на блок
    dmg = h0 - p.health
    bounced = (p.velocity_events - v0) >= 1
    if mode == "bounce":
        s.check(name, dmg < 0.01 and bounced,
                f"урон {dmg:.1f} (ожидался 0), отскок 0x5A: {'да' if bounced else 'нет'}")
    elif mode == "reduced":
        s.check(name, 0.01 < dmg < 8.0,
                f"урон {dmg:.1f} (ожидался небольшой ~3, стог -80%)")
    else:  # none
        s.check(name, dmg < 0.01, f"урон {dmg:.1f} (ожидался 0)")


def move_bot_to_work_area(bot, grass_y, wx, wz):
    # CROWD_SWARM_V2: рабочие боты теперь в gm0, а не в gm1, так что реально
    # горят/тонут/получают урон, а не стоят бессмертными манекенами.
    bot.send_command(f"gm0 {bot.bot_name}")
    time.sleep(0.5)
    for _ in range(6):
        bot.send_pos_raw(wx + 0.5, grass_y + 1, wz + 0.5, True)
        time.sleep(0.25)
    time.sleep(1.5)


def crowd_swarm_falling(bot, s, grass_y, wx, wz, label):
    try:
        move_bot_to_work_area(bot, grass_y, wx, wz)
        for yy in range(grass_y + 1, grass_y + 8):
            bot.send_command(f"setblock {wx} {yy} {wz} air")
        bot.send_command(f"setblock {wx} {grass_y} {wz} grass_block")
        bot.send_command(f"setblock {wx} {grass_y + 1} {wz} stone")
        for yy in range(grass_y + 2, grass_y + 5):
            bot.send_command(f"setblock {wx} {yy} {wz} gravel")
        time.sleep(0.6)
        with bot.lock:
            mark = len(bot.block_updates)
        bot.send_command(f"setblock {wx} {grass_y + 1} {wz} air")
        time.sleep(8.0)
        with bot.lock:
            landed = {(x, y, z, st) for x, y, z, st in bot.block_updates[mark:]
                      if x == wx and z == wz and st == 118 and grass_y + 1 <= y <= grass_y + 4}
        s.check(f"CROWD_SWARM_FALLING_{label}", len(landed) >= 3,
                f"гравий в колонне: {len(landed)}/3")
    except Exception as e:
        s.check(f"CROWD_SWARM_FALLING_{label}", False, f"{type(e).__name__}: {e}")


def crowd_swarm_tnt(bot, s, grass_y, wx, wz, label):
    try:
        move_bot_to_work_area(bot, grass_y, wx, wz)
        bot.send_command(f"setblock {wx} {grass_y} {wz} stone")
        bot.send_command(f"setblock {wx} {grass_y + 1} {wz} tnt")
        bot.send_command(f"setblock {wx + 1} {grass_y + 1} {wz} stone")
        time.sleep(0.5)
        bot.set_creative_slot(36, FLINT_AND_STEEL, 1)
        time.sleep(0.25)
        with bot.lock:
            before = {e for e, t in bot.entity_types.items() if t == 106}
            mark = len(bot.block_updates)
        bot.place_block(wx, grass_y + 1, wz, face=1)
        time.sleep(4.8)
        with bot.lock:
            spawned = {e for e, t in bot.entity_types.items() if t == 106} - before
            teid = nearest_spawn(bot, spawned, wx + 0.5, wz + 0.5, 1.0)
            removed = teid in bot.removed if teid is not None else False
            updates = list(bot.block_updates[mark:])
        destroyed = (wx + 1, grass_y + 1, wz, 0) in updates
        s.check(f"CROWD_SWARM_TNT_{label}", removed and destroyed,
                f"entity удалена: {'да' if removed else 'нет'}, stone destroyed: {'да' if destroyed else 'нет'}")
    except Exception as e:
        s.check(f"CROWD_SWARM_TNT_{label}", False, f"{type(e).__name__}: {e}")


def crowd_swarm_fire(bot, s, grass_y, wx, wz, label):
    try:
        move_bot_to_work_area(bot, grass_y, wx, wz)
        bot.send_command(f"setblock {wx} {grass_y} {wz} stone")
        bot.send_command(f"setblock {wx} {grass_y + 1} {wz} air")
        bot.send_command(f"setblock {wx + 1} {grass_y + 1} {wz} oak_planks")
        time.sleep(0.5)
        bot.set_creative_slot(36, FLINT_AND_STEEL, 1)
        time.sleep(0.25)
        with bot.lock:
            mark = len(bot.block_updates)
        bot.place_block(wx, grass_y, wz, face=1)
        time.sleep(5.2)
        with bot.lock:
            updates = list(bot.block_updates[mark:])
        placed = (wx, grass_y + 1, wz, 2391) in updates
        spread = any(x == wx + 1 and y == grass_y + 1 and z == wz and st in (0, 2391)
                     for x, y, z, st in updates)
        s.check(f"CROWD_SWARM_FIRE_{label}", placed and spread,
                f"place={'да' if placed else 'нет'}, spread={'да' if spread else 'нет'}")
    except Exception as e:
        s.check(f"CROWD_SWARM_FIRE_{label}", False, f"{type(e).__name__}: {e}")


def crowd_swarm_projectile(bot, s, grass_y, wx, wz, label):
    try:
        move_bot_to_work_area(bot, grass_y, wx, wz)
        bot.send_command(f"setblock {wx} {grass_y} {wz} grass_block")
        for dz in range(1, 12):
            bot.send_command(f"setblock {wx} {grass_y} {wz + dz} grass_block")
            for yy in range(grass_y + 1, grass_y + 5):
                bot.send_command(f"setblock {wx} {yy} {wz + dz} air")
        for yy in range(grass_y + 1, grass_y + 5):
            bot.send_command(f"setblock {wx} {yy} {wz + 10} stone")
        time.sleep(0.8)
        bot.send_pos_rot(wx + 0.5, grass_y + 1, wz + 0.5, 0.0, 0.0, True)
        time.sleep(0.3)
        bot.set_creative_slot(36, SNOWBALL, 8)
        time.sleep(0.25)
        with bot.lock:
            before = {e for e, t in bot.entity_types.items() if t == 97}
        bot.use_item(0)
        time.sleep(2.1)
        with bot.lock:
            created = {e for e, t in bot.entity_types.items() if t == 97} - before
        eid = nearest_spawn(bot, created, wx + 0.5, wz + 0.5, 1.0)
        with bot.lock:
            traj = list(bot.item_tp.get(eid, [])) if eid is not None else []
            removed = eid in bot.removed if eid is not None else False
        ok = eid is not None and removed and len(traj) >= 2
        s.check(f"CROWD_SWARM_PROJECTILE_{label}", ok,
                f"spawn={'да' if eid is not None else 'нет'}, removed={'да' if removed else 'нет'}, points={len(traj)}")
    except Exception as e:
        s.check(f"CROWD_SWARM_PROJECTILE_{label}", False, f"{type(e).__name__}: {e}")


def crowd_swarm_concrete(bot, s, grass_y, wx, wz, label):
    """CROWD_SWARM_V4: пятая группа (Д) ссылалась на эту функцию, но она не была
    реализована — вызов main() падал бы с NameError. Проверяем CONCRETE_WATER:
    concrete powder (571) сразу твердеет в voду (state 12728)."""
    try:
        move_bot_to_work_area(bot, grass_y, wx, wz)
        for yy in range(grass_y + 1, grass_y + 6):
            bot.send_command(f"setblock {wx} {yy} {wz} air")
        bot.send_command(f"setblock {wx} {grass_y} {wz} stone")
        bot.send_command(f"setblock {wx} {grass_y + 1} {wz} water")
        time.sleep(0.4)
        bot.set_creative_slot(36, WHITE_POWDER_ITEM, 1)
        time.sleep(0.25)
        with bot.lock:
            mark = len(bot.block_updates)
        bot.place_block(wx, grass_y, wz, face=1)
        time.sleep(1.5)
        with bot.lock:
            updates = list(bot.block_updates[mark:])
        hardened = (wx, grass_y + 1, wz, 12728) in updates
        s.check(f"CROWD_SWARM_CONCRETE_{label}", hardened,
                "powder в воде сразу стал white_concrete" if hardened else "нет state=12728")
    except Exception as e:
        s.check(f"CROWD_SWARM_CONCRETE_{label}", False, f"{type(e).__name__}: {e}")


def crowd_swarm_farming(bot, s, grass_y, wx, wz, label):
    """Проверяет три детерминированных фермерских механики за одно задание:
    HOE_TILL_V1 (мотыга: dirt -> farmland), SEED_PLANT_V1 (посадка wheat_seeds
    на farmland) и BONE_MEAL_V1 (мгновенный рост костной мукой). Естественный
    рост без костной муки идёт через tickRandomBlockUpdates (3 случайные клетки
    на чанк-колонну за тик) и здесь намеренно не проверяется — ждать его в
    коротком тесте статистически бессмысленно."""
    try:
        move_bot_to_work_area(bot, grass_y, wx, wz)
        for yy in range(grass_y + 1, grass_y + 4):
            bot.send_command(f"setblock {wx} {yy} {wz} air")
        bot.send_command(f"setblock {wx} {grass_y} {wz} dirt")
        time.sleep(0.4)
        bot.set_creative_slot(36, DIAMOND_HOE, 1)
        time.sleep(0.25)
        with bot.lock:
            mark = len(bot.block_updates)
        bot.place_block(wx, grass_y, wz, face=1)
        time.sleep(0.8)
        with bot.lock:
            tilled = (wx, grass_y, wz, 4286) in bot.block_updates[mark:]
        bot.set_creative_slot(36, WHEAT_SEEDS_ITEM, 1)
        time.sleep(0.25)
        with bot.lock:
            mark2 = len(bot.block_updates)
        bot.place_block(wx, grass_y, wz, face=1)
        time.sleep(0.8)
        with bot.lock:
            planted = (wx, grass_y + 1, wz, 4278) in bot.block_updates[mark2:]
        bot.set_creative_slot(36, BONE_MEAL_ITEM, 1)
        time.sleep(0.25)
        with bot.lock:
            mark3 = len(bot.block_updates)
        bot.place_block(wx, grass_y + 1, wz, face=1)
        time.sleep(0.8)
        with bot.lock:
            grown = any(x == wx and y == grass_y + 1 and z == wz and 4279 <= st <= 4285
                        for x, y, z, st in bot.block_updates[mark3:])
        ok = tilled and planted and grown
        s.check(f"CROWD_SWARM_FARMING_{label}", ok,
                f"till={'да' if tilled else 'нет'}, plant={'да' if planted else 'нет'}, bonemeal={'да' if grown else 'нет'}")
    except Exception as e:
        s.check(f"CROWD_SWARM_FARMING_{label}", False, f"{type(e).__name__}: {e}")


def crowd_swarm_trample(bot, s, grass_y, wx, wz, label):
    """FARMLAND_TRAMPLE_V1: приземление на farmland с высоты >0.5 блока без
    crouch вытаптывает его обратно в dirt (state 10)."""
    try:
        move_bot_to_work_area(bot, grass_y, wx, wz)
        for yy in range(grass_y + 1, grass_y + 5):
            bot.send_command(f"setblock {wx} {yy} {wz} air")
        bot.send_command(f"setblock {wx} {grass_y} {wz} farmland")
        time.sleep(0.4)
        with bot.lock:
            mark = len(bot.block_updates)
        bot.send_pos_raw(wx + 0.5, grass_y + 3.0, wz + 0.5, True)
        time.sleep(0.15)
        bot.send_pos_raw(wx + 0.5, grass_y + 1.001, wz + 0.5, True)
        time.sleep(0.9)
        with bot.lock:
            updates = list(bot.block_updates[mark:])
        trampled = (wx, grass_y, wz, 10) in updates
        s.check(f"CROWD_SWARM_TRAMPLE_{label}", trampled,
                "farmland вытоптан обратно в dirt" if trampled else "нет state=10 (dirt)")
    except Exception as e:
        s.check(f"CROWD_SWARM_TRAMPLE_{label}", False, f"{type(e).__name__}: {e}")


def crowd_swarm_bamboo(bot, s, grass_y, wx, wz, label):
    """RANDOMTICK_WARP_V1 делает этот тест возможным: команда /warprandomtick <N>
    мгновенно прогоняет N проходов того же случайного тика, что и идёт в обычной игре
    (3 случайные клетки на чанк-колонну за проход), только без ожидания реального
    времени между проходами. Проверяет BAMBOO_SUPPORT_V1 (бамбук без опоры сносится
    за 1 попадание) и BAMBOO_GROWTH_V1 (верхний сегмент растёт после 16 накопленных
    попаданий именно на эту клетку). Вероятностный тест: среднее ожидаемое число
    попаданий на одну конкретную клетку за N проходов ≈ N/32768 (при 3 случайных клетках
    из ~98304 возможных в колонне); при N=1_000_000 это ~30 ожидаемых попаданий — шанс
    не набрать 16 попаданий пренебрежимо мал, но не ровно нулю — это честно указано в info."""
    try:
        move_bot_to_work_area(bot, grass_y, wx, wz)
        for yy in range(grass_y + 1, grass_y + 4):
            bot.send_command(f"setblock {wx} {yy} {wz} air")
            bot.send_command(f"setblock {wx} {yy} {wz + 4} air")
        # Риг 1: бамбук на камне (невалидная опора) — должен снестись.
        bot.send_command(f"setblock {wx} {grass_y} {wz} stone")
        bot.send_command(f"setblock {wx} {grass_y + 1} {wz} bamboo")
        # Риг 2: бамбук на dirt (валидная опора) — должен вырасти ввысь.
        bot.send_command(f"setblock {wx} {grass_y} {wz + 4} dirt")
        bot.send_command(f"setblock {wx} {grass_y + 1} {wz + 4} bamboo")
        time.sleep(0.5)
        with bot.lock:
            mark = len(bot.block_updates)
        bot.send_command("warprandomtick 1000000")
        time.sleep(4.0)
        with bot.lock:
            updates = list(bot.block_updates[mark:])
        removed = any(x == wx and y == grass_y + 1 and z == wz and st == 0 for x, y, z, st in updates)
        grew = any(x == wx and y == grass_y + 2 and z == wz + 4 and st != 0 for x, y, z, st in updates)
        ok = removed and grew
        s.check(f"CROWD_SWARM_BAMBOO_{label}", ok,
                f"support={'да' if removed else 'нет'}, growth={'да' if grew else 'нет'} (вероятностный тест через /warprandomtick)")
    except Exception as e:
        s.check(f"CROWD_SWARM_BAMBOO_{label}", False, f"{type(e).__name__}: {e}")


def run_physics_phase(s, args, ground_section, grass_y):
    """PHYS_V2: отдельный бот проверяет физику предметов и урон от падения."""
    p = Bot("SmokeBotP", args.host, args.port, ground_section)
    p.start()
    if not p.in_play.wait(30) or p.error:
        s.check("PHYS_LOGIN", False, p.error or "таймаут входа физ-бота")
        return
    p.positioned.wait(20)
    time.sleep(4.8)  # ждём чанки и лишних crowd-игроков
    if p.pos is None:
        s.check("PHYS_LOGIN", False, "нет позиции физ-бота")
        return
    s.check("PHYS_LOGIN", True, f"физ-бот в игре, Y={p.pos[1]:.1f}")

    # выживание + чистый инвентарь (иначе ломание/урон не работают в креативе)
    p.send_command("gm0 SmokeBotP")
    time.sleep(0.8)
    for slot in (5, 6, 7, 8, 36, 45):
        p.clear_slot(slot)
    time.sleep(0.4)

    # WORKAREA_V1: spawn-protection=16 блокирует ломание/установку у спавна (0x24/0x38
    # откатываются). Уводим физ-бота за радиус защиты, чтобы break/place/bucket работали.
    wx, wz = 300.5, 300.5
    for _ in range(6):
        p.send_pos_raw(wx, grass_y + 1, wz, True); time.sleep(0.3)
    time.sleep(1.5)  # ждём стрим чанков рабочей зоны
    px, py, pz = wx, grass_y + 1, wz
    bxi, bzi = int(px // 1), int(pz // 1)

    # ---- MINIEDIT_V1: cuboid API, names/ids, history, clipboard rotation, batch packets ----
    mx, my, mz = bxi + 70, grass_y + 1, bzi
    p.send_command("wand"); time.sleep(0.5)  # ordinary one-slash /wand
    s.check("MINIEDIT_WAND_NAME", p.wand_name == "Builder's Wand" and p.wand_color == "yellow",
            f"custom_name={p.wand_name!r}, color={p.wand_color!r}")
    p.clear_slot(36); time.sleep(0.2)
    p.send_pos_raw(mx + 0.2, my, mz + 0.2, True); time.sleep(0.2)
    p.send_command("/pos1")  # wire form of //pos1 (client strips the first slash)
    time.sleep(0.2)
    p.send_pos_raw(mx + 1.2, my + 1, mz + 1.2, True); time.sleep(0.2)
    with p.lock:
        outline_mark = p.particle_packets
        edit_mark = len(p.block_updates)
        section_mark = p.section_update_packets
    p.send_command("/pos2")
    time.sleep(0.35)
    p.send_command("set stone")   # MINIEDIT_V2: ordinary single-slash /set (wire has no slash)
    time.sleep(0.6)
    region = {(x, y, z) for x in range(mx, mx + 2) for y in range(my, my + 2) for z in range(mz, mz + 2)}
    with p.lock:
        set_updates = list(p.block_updates[edit_mark:])
        set_batch = p.section_update_packets > section_mark
        outline_ok = p.particle_packets > outline_mark
    set_seen = {(x, y, z) for x, y, z, st in set_updates if st == 1 and (x, y, z) in region}
    s.check("MINIEDIT_SET", set_batch and set_seen == region,
            f"section batch: {'да' if set_batch else 'нет'}, stone: {len(set_seen)}/8")
    s.check("MINIEDIT_OUTLINE", outline_ok,
            f"dust-пакетов после pos2: {p.particle_packets - outline_mark}")
    with p.lock: outline_after_operation = p.particle_packets
    time.sleep(1.2)
    s.check("MINIEDIT_OUTLINE_STOPS", p.particle_packets == outline_after_operation,
            f"новых outline-пакетов после set: {p.particle_packets - outline_after_operation}")

    with p.lock: replace_mark = len(p.block_updates)
    p.send_command("we replace 1 diorite")  # numeric from + named to; /we alias
    time.sleep(0.6)
    with p.lock: replace_updates = list(p.block_updates[replace_mark:])
    replace_seen = {(x, y, z) for x, y, z, st in replace_updates if st == 4 and (x, y, z) in region}
    s.check("MINIEDIT_REPLACE", replace_seen == region, f"diorite: {len(replace_seen)}/8")

    with p.lock: undo_mark = len(p.block_updates)
    p.send_command("undo"); time.sleep(0.5)
    with p.lock: undo_updates = list(p.block_updates[undo_mark:])
    undo_seen = {(x, y, z) for x, y, z, st in undo_updates if st == 1 and (x, y, z) in region}
    s.check("MINIEDIT_UNDO", undo_seen == region, f"stone restored: {len(undo_seen)}/8")

    with p.lock: redo_mark = len(p.block_updates)
    p.send_command("redo"); time.sleep(0.5)
    with p.lock: redo_updates = list(p.block_updates[redo_mark:])
    redo_seen = {(x, y, z) for x, y, z, st in redo_updates if st == 4 and (x, y, z) in region}
    s.check("MINIEDIT_REDO", redo_seen == region, f"diorite restored: {len(redo_seen)}/8")

    # Copy origin is the player's current block (pos2 corner), then rotate around Y and paste elsewhere.
    p.send_command("/copy"); time.sleep(0.25)
    p.send_command("/rotate 90"); time.sleep(0.25)
    paste_x, paste_z = mx + 12, mz
    p.send_pos_raw(paste_x + 0.2, my + 1, paste_z + 0.2, True); time.sleep(0.2)
    with p.lock: paste_mark = len(p.block_updates)
    p.send_command("/paste"); time.sleep(0.7)
    with p.lock: paste_updates = list(p.block_updates[paste_mark:])
    pasted = {(x, y, z) for x, y, z, st in paste_updates if st == 4}
    s.check("MINIEDIT_COPY_ROTATE_PASTE", len(pasted) == 8,
            f"повёрнутых block-state обновлений: {len(pasted)}/8")

    # Restore the physics bot's original work position before the existing suite.
    for _ in range(3):
        p.send_pos_raw(px, py, pz, True); time.sleep(0.15)

    # ---- ITEM_SPAWN/ARC: ломаем блок в 2 клетках (чтобы не подобрать сразу) ----
    bx, by, bz = bxi + 2, grass_y, bzi
    # SMOKE_IDEMPOTENT_V1: прошлый запуск уже сломал эту траву. Каждый тест
    # восстанавливает обязательное начальное состояние, поэтому smoke можно
    # запускать сколько угодно раз на том же world без ручной очистки.
    p.send_command(f"setblock {bx} {by} {bz} grass_block")
    time.sleep(0.35)
    with p.lock:
        items_before = {e for e, t in p.entity_types.items() if t == 58}
    p.break_block(bx, by, bz)
    time.sleep(2.0)
    with p.lock:
        items_after = {e for e, t in p.entity_types.items() if t == 58}
    new_items = items_after - items_before
    s.check("ITEM_SPAWN", len(new_items) >= 1,
            f"новых предметов-сущностей после ломания: {len(new_items)} (ожидалась 1)")

    item_eid = nearest_spawn(p, new_items, bx + 0.5, bz + 0.5)
    if item_eid is not None:
        with p.lock:
            ys = [t[1] for t in p.item_tp.get(item_eid, [])]
        arc = len(ys) >= 2 and (max(ys) - min(ys) > 0.1)
        s.check("ITEM_ARC", arc,
                f"обновлений позици������: {len(ys)}, разброс Y: {(max(ys) - min(ys)) if ys else 0:.2f} (не мгн��венно на пол)")
    else:
        s.check("ITEM_ARC", False, "нет предмета для проверки дуги")

    # ---- ITEM_PICKUP / PICKUP_COUNT_1: встаём на предмет, подбираем ровно 1 ----
    took_before = len(p.take_events)
    for _ in range(8):
        p.send_pos_raw(bx + 0.5, grass_y + 1, bz + 0.5, True)
        time.sleep(0.25)
    with p.lock:
        picks = list(p.take_events[took_before:])
    picked_one = any(cnt == 1 for (_, _, cnt) in picks)
    s.check("ITEM_PICKUP", len(picks) >= 1, f"событий подбора (0x6F): {len(picks)}")
    s.check("PICKUP_COUNT_1", picked_one,
            f"подобрано за раз: {[c for (_, _, c) in picks] or '—'} (ожидался ровно 1 — баг '2 блока')")

    # возвращаемся на исходную клетку
    for _ in range(3):
        p.send_pos_raw(px, py, pz, True)
        time.sleep(0.15)

    # ---- ITEM_MERGE: бросаем два одинаковых предмета в одну точку ----
    p.set_creative_slot(36, 1, 2)  # 2 одинаковых предмета в активный слот
    time.sleep(0.4)
    with p.lock:
        merge_before = {e for e, t in p.entity_types.items() if t == 58}
    p.drop_one(); time.sleep(0.15); p.drop_one()
    time.sleep(1.0)  # задержка подбора 40 тиков (2с) — успеваем поймать слияние
    with p.lock:
        merge_after = {e for e, t in p.entity_types.items() if t == 58}
        removed_now = set(p.removed)
    dropped = merge_after - merge_before
    merged = len(dropped) >= 2 and len(dropped & removed_now) >= 1
    s.check("ITEM_MERGE", merged,
            f"выброшено: {len(dropped)}, слилось (удалено): {len(dropped & removed_now)} (ожидалось 2 -> 1)")

    # ---- ITEM_REST: предмет ложится на землю и останавливается (пол + трение, покой на целом Y) ----
    rbx, rby, rbz = bxi + 9, grass_y, bzi
    p.send_command(f"setblock {rbx} {rby} {rbz} grass_block")
    time.sleep(0.35)
    with p.lock:
        rest_before = {e for e, t in p.entity_types.items() if t == 58}
    p.break_block(rbx, rby, rbz)
    time.sleep(4.6)  # даём предмету упасть и реально успокоиться под нагрузкой
    with p.lock:
        rest_new = {e for e, t in p.entity_types.items() if t == 58} - rest_before
        reid = nearest_spawn(p, rest_new, rbx + 0.5, rbz + 0.5)
        rtraj = list(p.item_tp.get(reid, [])) if reid is not None else []
    if rtraj:
        last_y = rtraj[-1][1]
        tail = [t[1] for t in rtraj[-5:]]
        settled = (max(tail) - min(tail) < 0.03) and (last_y >= grass_y + 0.2)
        s.check("ITEM_REST", settled,
                f"предмет успокоился на Y={last_y:.2f} (стабильный покой над блоком)")
    else:
        s.check("ITEM_REST", False, "нет траектории предмета для проверки покоя")

    # ---- ITEM_FLOAT: предмет всплывает в воде (vanilla ItemEntity, vy += 0.0005) ----
    # Ломаем траву -> предмет ложится в ямку; заливаем воду -> предмет всплывает.
    fbx, fby, fbz = bxi + 10, grass_y, bzi
    p.send_command(f"setblock {fbx} {fby} {fbz} grass_block")  # убирает воду прошлого запуска
    time.sleep(0.35)
    with p.lock:
        float_before = {e for e, t in p.entity_types.items() if t == 58}
    p.break_block(fbx, fby, fbz)
    time.sleep(2.5)
    with p.lock:
        float_new = {e for e, t in p.entity_types.items() if t == 58} - float_before
        feid = nearest_spawn(p, float_new, fbx + 0.5, fbz + 0.5)
        pre = list(p.item_tp.get(feid, [])) if feid is not None else []
    y_before = pre[-1][1] if pre else None
    p.set_creative_slot(36, WATER_BUCKET, 1)
    time.sleep(0.4)
    p.place_block(fbx, fby - 1, fbz, face=1)  # вода появляется на (fbx, grass_y, fbz)
    time.sleep(2.5)  # даём предмету всплыть
    with p.lock:
        post = list(p.item_tp.get(feid, [])) if feid is not None else []
    if y_before is not None and post:
        y_after = max(t[1] for t in post)
        s.check("ITEM_FLOAT", (y_after - y_before) > 0.3,
                f"предмет всплыл: Y {y_before:.2f} -> {y_after:.2f} (плавучесть в воде)")
    else:
        s.check("ITEM_FLOAT", False, "нет траектории предмета для проверки плавучести")

    # ---- ITEM_ICE_SLIDE: предмет скользит по льду дальше, чем по траве (трение 0.98 vs 0.6) ----
    # Сравниваем два броска с одинаковой высоты (оба падают на 1.3 блока — воздушная
    # часть одинакова); разница пути по Z = выигрыш от скольжения по льду.
    def throw_travel(sx, sy, sz):
        p.send_pos_rot(sx, sy, sz, 0.0, 0.0, True)  # yaw=0 -> бросок по +Z (vz=+0.3)
        time.sleep(0.4)
        p.set_creative_slot(36, 1, 1); time.sleep(0.3)
        with p.lock:
            before = {e for e, t in p.entity_types.items() if t == 58}
        p.drop_one()
        time.sleep(3.0)
        with p.lock:
            new = {e for e, t in p.entity_types.items() if t == 58} - before
            eid = nearest_spawn(p, new, sx, sz)
            trj = list(p.item_tp.get(eid, [])) if eid is not None else []
        return (max(t[2] for t in trj) - min(t[2] for t in trj)) if len(trj) >= 2 else None

    grass_travel = throw_travel(bxi + 0.5, grass_y + 1, bzi + 20.5)  # база: голая трава
    icz0 = bzi + 2
    # Бот уже в survival: раньше сюда выдавался ОДИН блок льда, поэтому из
    # нарисованной в цикле «дорожки» сервер реально ставил лишь первую клетку.
    # 16x3 = 48 блоков помещаются в один стак и дают полноценную полосу.
    p.set_creative_slot(36, ICE_ITEM, 64); time.sleep(0.3)
    for dz in range(0, 16):
        for dx in (-1, 0, 1):
            p.place_block(bxi + dx, grass_y, icz0 + dz, face=1)  # лёд на (x, grass_y+1, z)
            time.sleep(0.05)
    time.sleep(0.6)
    ice_travel = throw_travel(bxi + 0.5, grass_y + 2, icz0 + 0.5)  # бросок со льда
    if grass_travel is not None and ice_travel is not None:
        s.check("ITEM_ICE_SLIDE", ice_travel > grass_travel + 1.0,
                f"путь по Z: лёд {ice_travel:.2f} vs трава {grass_travel:.2f} (лёд скользкее на >1.0)")
    else:
        s.check("ITEM_ICE_SLIDE", False,
                f"��ет траектории (лёд={ice_travel}, трава={grass_travel})")
    for _ in range(3):  # возвращаем бота на старт для дальнейших тестов падения
        p.send_pos_raw(px, py, pz, True); time.sleep(0.15)

    # ---- FALLING BLOCKS: scheduled tick -> entity type 40 -> landing block ----
    fbx, fbz = bxi + 14, bzi + 8
    for yy in range(grass_y + 1, grass_y + 10):
        p.send_command(f"setblock {fbx} {yy} {fbz} air")
    p.send_command(f"setblock {fbx} {grass_y} {fbz} grass_block")
    time.sleep(3.0)  # дать старым scheduled FallingBlockEntity полностью завершиться
    for yy in range(grass_y + 1, grass_y + 10):
        p.send_command(f"setblock {fbx} {yy} {fbz} air")
    time.sleep(0.35)
    with p.lock:
        falling_before = {e for e, t in p.entity_types.items() if t == 40}
        update_mark = len(p.block_updates)
    p.send_command(f"setblock {fbx} {grass_y + 7} {fbz} sand")
    time.sleep(3.0)
    with p.lock:
        falling_new = {e for e, t in p.entity_types.items() if t == 40} - falling_before
        fe = nearest_spawn(p, falling_new, fbx + 0.5, fbz + 0.5, 0.75)
        ftraj = list(p.item_tp.get(fe, [])) if fe is not None else []
        fupdates = list(p.block_updates[update_mark:])
        fremoved = fe in p.removed if fe is not None else False
    s.check("FALLING_SPAWN", len(falling_new) >= 1,
            f"новых FallingBlockEntity type=40: {len(falling_new)}")
    s.check("FALLING_ARC", len(ftraj) >= 3 and ftraj[0][1] > ftraj[-1][1],
            f"точек траектории: {len(ftraj)}, Y: " +
            (f"{ftraj[0][1]:.2f}->{ftraj[-1][1]:.2f}" if ftraj else "—"))
    landed = (fbx, grass_y + 1, fbz, 112) in fupdates
    s.check("FALLING_LAND", landed and fremoved,
            f"sand state=112 на Y={grass_y + 1}: {'да' if landed else 'нет'}, entity удалена: {'да' if fremoved else 'нет'}")

    # Колонна гравия: сначала строим её на временной опоре, затем убираем опору.
    # Старый тест сразу ставил блоки в воздух, из-за чего scheduled ticks могли
    # сработать ещё во время построения и результат зависел от тайминга команд.
    cbx, cbz = bxi + 16, bzi + 8
    for yy in range(grass_y + 1, grass_y + 8):
        p.send_command(f"setblock {cbx} {yy} {cbz} air")
    p.send_command(f"setblock {cbx} {grass_y} {cbz} grass_block")
    time.sleep(3.0)
    for yy in range(grass_y + 1, grass_y + 8):
        p.send_command(f"setblock {cbx} {yy} {cbz} air")
    p.send_command(f"setblock {cbx} {grass_y + 1} {cbz} stone")
    for yy in range(grass_y + 2, grass_y + 5):
        p.send_command(f"setblock {cbx} {yy} {cbz} gravel")
    time.sleep(0.6)
    with p.lock: chain_mark = len(p.block_updates)
    p.send_command(f"setblock {cbx} {grass_y + 1} {cbz} air")
    time.sleep(5.5)
    with p.lock:
        chain_land = {(x, y, z, st) for x, y, z, st in p.block_updates[chain_mark:]
                      if x == cbx and z == cbz and st == 118 and grass_y + 1 <= y <= grass_y + 3}
    s.check("FALLING_CHAIN", len(chain_land) == 3,
            f"приземлившихся gravel-блоков внизу: {len(chain_land)}/3")

    # Белый concrete powder (12744) при входе в воду должен стать white concrete (12728).
    wbx, wbz = bxi + 18, bzi + 8
    for yy in range(grass_y + 1, grass_y + 8):
        p.send_command(f"setblock {wbx} {yy} {wbz} air")
    p.send_command(f"setblock {wbx} {grass_y + 1} {wbz} water")
    time.sleep(0.3)
    with p.lock: concrete_mark = len(p.block_updates)
    p.send_command(f"setblock {wbx} {grass_y + 6} {wbz} white_concrete_powder")
    time.sleep(3.0)
    with p.lock:
        concrete_updates = list(p.block_updates[concrete_mark:])
    hardened = any(x == wbx and z == wbz and st == 12728 for x, y, z, st in concrete_updates)
    s.check("CONCRETE_WATER", hardened,
            "white_concrete state=12728 после контакта с водой: " + ("да" if hardened else "нет"))

    # Постановка powder непосредственно в воду: getStateForPlacement должен
    # отправить concrete сразу, без промежуточного сухого блока/FallingEntity.
    dpx, dpz = bxi + 20, bzi + 8
    p.send_command(f"setblock {dpx} {grass_y} {dpz} stone")
    p.send_command(f"setblock {dpx} {grass_y + 1} {dpz} water")
    time.sleep(0.4)
    p.set_creative_slot(36, WHITE_POWDER_ITEM, 1); time.sleep(0.25)
    with p.lock: direct_mark = len(p.block_updates)
    p.place_block(dpx, grass_y, dpz, face=1)
    time.sleep(1.2)
    with p.lock: direct_updates = list(p.block_updates[direct_mark:])
    direct_ok = (dpx, grass_y + 1, dpz, 12728) in direct_updates
    s.check("CONCRETE_PLACE_WATER", direct_ok,
            "powder в целевой воде сразу стал white_concrete" if direct_ok else "нет state=12728")

    # Вода сбоку: ConcretePowderBlock.touchesLiquid/updateShape.
    apx, apz = bxi + 22, bzi + 8
    p.send_command(f"setblock {apx} {grass_y} {apz} stone")
    p.send_command(f"setblock {apx} {grass_y + 1} {apz} air")
    p.send_command(f"setblock {apx + 1} {grass_y + 1} {apz} water")
    time.sleep(0.4)
    p.set_creative_slot(36, WHITE_POWDER_ITEM, 1); time.sleep(0.25)
    with p.lock: adjacent_mark = len(p.block_updates)
    p.place_block(apx, grass_y, apz, face=1)
    time.sleep(0.7)
    with p.lock: adjacent_updates = list(p.block_updates[adjacent_mark:])
    adjacent_ok = (apx, grass_y + 1, apz, 12728) in adjacent_updates
    s.check("CONCRETE_ADJACENT", adjacent_ok,
            "боковая вода сразу дала white_concrete" if adjacent_ok else "нет state=12728")

    # Powder уже стоит, вода появляется позже: vanilla updateShape.
    # Отдельная площадка: прежний тест стоял вплотную к воде CONCRETE_ADJACENT
    # и powder затвердевал ДО flow_mark, давая ложный FAIL.
    fpx, fpz = bxi + 30, bzi + 8
    p.send_command(f"setblock {fpx} {grass_y} {fpz} stone")
    for ox, oz in ((1,0),(-1,0),(0,1),(0,-1)):
        p.send_command(f"setblock {fpx + ox} {grass_y + 1} {fpz + oz} air")
    p.send_command(f"setblock {fpx} {grass_y + 1} {fpz} white_concrete_powder")
    p.send_command(f"setblock {fpx + 1} {grass_y + 1} {fpz} air")
    time.sleep(0.4)
    with p.lock: flow_mark = len(p.block_updates)
    p.send_command(f"setblock {fpx + 1} {grass_y + 1} {fpz} water")
    time.sleep(0.7)
    with p.lock: flow_updates = list(p.block_updates[flow_mark:])
    flow_ok = (fpx, grass_y + 1, fpz, 12728) in flow_updates
    s.check("CONCRETE_FLOW_IN", flow_ok,
            "пришедшая вода превратила powder в concrete" if flow_ok else "нет updateShape state=12728")

    # ---- SOFT-BLOCKS: приземление на блоки, влияющие на урон (FALLSOFT_V1 / BOUNCE_V1) ----
    # Все, кроме стога, гасят урон в 0 -> здоровье не тратится; стог (~3 урона) идёт последним.
    block_fall_test(p, s, "FALL_WATER",  WATER_BUCKET, bxi + 3, bzi, grass_y, "none")
    block_fall_test(p, s, "FALL_COBWEB", COBWEB_ITEM,  bxi + 4, bzi, grass_y, "none")
    block_fall_test(p, s, "FALL_HONEY",  HONEY_ITEM,   bxi + 5, bzi, grass_y, "none")
    block_fall_test(p, s, "FALL_SLIME",  SLIME_ITEM,   bxi + 6, bzi, grass_y, "bounce")
    block_fall_test(p, s, "FALL_HAY",    HAY_ITEM,     bxi + 7, bzi, grass_y, "reduced")

    # ---- FALL: симуляция падения пакетами позиции (на обычной траве) ----
    if p.health <= 0.5:  # RESPAWN_V1
        p.respawn(); time.sleep(1.2)
    for _ in range(3):
        p.send_pos_raw(px, grass_y + 1, pz, True)
        time.sleep(0.15)
    h0 = p.health
    p.send_pos_raw(px, grass_y + 3, pz, False); time.sleep(0.3)   # пик +2
    p.send_pos_raw(px, grass_y + 1, pz, True);  time.sleep(0.6)   # приземление
    s.check("FALL_SMALL", abs(p.health - h0) < 0.01,
            f"падение на 2 блока: урон {h0 - p.health:.1f} (ожидался 0)")

    if p.health <= 0.5:  # RESPAWN_V1
        p.respawn(); time.sleep(1.2)
    # Отдельная заведомо сухая площадка: результат не зависит от воды/мёда/
    # слизи, оставшихся в рабочей зоне после прошлого запуска smoke.
    fallx, fallz = bxi + 24, bzi
    p.send_command(f"setblock {fallx} {grass_y} {fallz} grass_block")
    for yy in range(grass_y + 1, grass_y + 15):
        p.send_command(f"setblock {fallx} {yy} {fallz} air")
    time.sleep(0.7)
    fallpx, fallpz = fallx + 0.5, fallz + 0.5
    for _ in range(2):
        p.send_pos_raw(fallpx, grass_y + 1, fallpz, True); time.sleep(0.15)
    h1 = p.health
    # Нелетальное падение с промежуточными нисходящими пакетами. Ст����рый тест
    # телепорт��ровал уже раненого бота на +21 и сразу в землю, смешивая проверку
    # fall tracker с обработкой смерти. Здесь всё ещё ожидается серьёзный урон,
    # но отдельно и детерминированно проверяется накопление fallDistance.
    p.send_pos_raw(fallpx, grass_y + 13, fallpz, False); time.sleep(0.15)  # пик +12
    p.send_pos_raw(fallpx, grass_y + 9,  fallpz, False); time.sleep(0.10)
    p.send_pos_raw(fallpx, grass_y + 5,  fallpz, False); time.sleep(0.10)
    p.send_pos_raw(fallpx, grass_y + 1,  fallpz, True);  time.sleep(0.8)   # приземление
    s.check("FALL_BIG", (h1 - p.health) > 3.0,
            f"падение с ~12 блоков на траву: урон {h1 - p.health:.1f} (ожидался значительный)")

    # Падающая наковальня: отдельный бот уже может быть ранен предыдущими
    # тестами, поэтому проверяем сам факт значительного урона или смерти.
    if p.health <= 0.5:
        p.respawn(); time.sleep(3.3)  # дождаться 60 тиков respawn immunity
    avx, avz = bxi + 28, bzi
    p.send_command(f"setblock {avx} {grass_y} {avz} grass_block")
    for yy in range(grass_y + 1, grass_y + 12):
        p.send_command(f"setblock {avx} {yy} {avz} air")
    time.sleep(0.5)
    for _ in range(2):
        p.send_pos_raw(avx + 0.5, grass_y + 1, avz + 0.5, True); time.sleep(0.15)
    with p.lock:
        anvil_before = {e for e, t in p.entity_types.items() if t == 40}
    ah0 = p.health
    p.send_command(f"setblock {avx} {grass_y + 8} {avz} anvil")
    time.sleep(3.0)
    with p.lock:
        anvil_new = {e for e, t in p.entity_types.items() if t == 40} - anvil_before
    aeid = nearest_spawn(p, anvil_new, avx + 0.5, avz + 0.5, 0.75)
    anvil_hit = (ah0 - p.health) > 3.0 or p.health <= 0.0
    s.check("ANVIL_DAMAGE", aeid is not None and aeid in p.removed and anvil_hit,
            f"урон {ah0 - p.health:.1f}, entity удалена: {'да' if aeid in p.removed else 'нет'}")

    # ---- TNT: flint -> PrimedTnt type 106 -> fuse 80 -> explosion + chain ----
    if p.health <= 0.5:
        p.respawn(); time.sleep(3.3)
    p.send_command("gm1 SmokeBotP")  # взрыв не убьёт бота и не оборвёт измерение
    time.sleep(0.5)
    tx, tz = bxi + 40, bzi
    p.send_command(f"setblock {tx} {grass_y} {tz} stone")
    p.send_command(f"setblock {tx} {grass_y + 1} {tz} tnt")
    p.send_command(f"setblock {tx + 1} {grass_y + 1} {tz} stone")
    p.send_command(f"setblock {tx + 2} {grass_y + 1} {tz} tnt")
    time.sleep(0.5)
    p.set_creative_slot(36, FLINT_AND_STEEL, 1); time.sleep(0.25)
    with p.lock:
        tnt_before = {e for e, t in p.entity_types.items() if t == 106}
        tnt_mark = len(p.block_updates)
    p.place_block(tx, grass_y + 1, tz, face=1)  # клик именно по TNT
    time.sleep(0.8)
    with p.lock:
        first_tnt = {e for e, t in p.entity_types.items() if t == 106} - tnt_before
    teid = nearest_spawn(p, first_tnt, tx + 0.5, tz + 0.5, 1.0)
    with p.lock: ttraj_early = list(p.item_tp.get(teid, [])) if teid is not None else []
    s.check("TNT_FUSE", teid is not None, f"PrimedTnt type=106: {'да' if teid is not None else 'нет'}")
    s.check("TNT_ARC", len(ttraj_early) >= 3 and max(v[1] for v in ttraj_early) > min(v[1] for v in ttraj_early),
            f"точек траектории до взрыва: {len(ttraj_early)}")
    time.sleep(4.8)
    with p.lock:
        tnt_updates = list(p.block_updates[tnt_mark:])
        all_tnt_new = {e for e, t in p.entity_types.items() if t == 106} - tnt_before
        te_removed = teid in p.removed if teid is not None else False
    destroyed = (tx + 1, grass_y + 1, tz, 0) in tnt_updates
    s.check("TNT_EXPLODE", te_removed and destroyed,
            f"entity удалена: {'да' if te_removed else 'нет'}, соседний stone разрушен: {'да' if destroyed else 'нет'}")
    s.check("TNT_CHAIN", len(all_tnt_new) >= 2,
            f"PrimedTnt после цепной реакции: {len(all_tnt_new)} (ожидалось >=2)")

    # ---- FIRE_V2: placement, spread, afterburn and finite lifetime ----
    fx, fz = bxi + 47, bzi
    p.send_command("gm1 SmokeBotP")
    p.send_command(f"setblock {fx} {grass_y} {fz} stone")
    p.send_command(f"setblock {fx} {grass_y + 1} {fz} air")
    p.send_command(f"setblock {fx + 1} {grass_y + 1} {fz} oak_planks")
    time.sleep(0.5)
    p.set_creative_slot(36, FLINT_AND_STEEL, 1); time.sleep(0.2)
    with p.lock: fire_mark = len(p.block_updates)
    p.place_block(fx, grass_y, fz, face=1)
    time.sleep(0.7)
    with p.lock: fire_early = list(p.block_updates[fire_mark:])
    s.check("FIRE_PLACE", (fx, grass_y + 1, fz, 2391) in fire_early,
            f"block updates: {fire_early[-8:]}")

    p.send_command("gm0 SmokeBotP"); time.sleep(0.3)
    p.send_pos_raw(fx + 0.5, grass_y + 1, fz + 0.5, True)
    time.sleep(0.7)
    p.send_pos_raw(fx + 3.5, grass_y + 1, fz + 0.5, True)
    health_on_exit = p.health
    time.sleep(1.2)
    s.check("FIRE_AFTERBURN", p.health < health_on_exit,
            f"health после выхода: {health_on_exit:.1f}->{p.health:.1f}")
    time.sleep(3.0)
    with p.lock: fire_all = list(p.block_updates[fire_mark:])
    spread = any(x == fx + 1 and y == grass_y + 1 and z == fz and st == 2391
                 for x, y, z, st in fire_all)
    s.check("FIRE_SPREAD", spread, f"обновлений огня: {sum(1 for *_, st in fire_all if st == 2391)}")
    p.send_command("gm1 SmokeBotP")
    p.send_command(f"setblock {fx + 1} {grass_y + 1} {fz} air")
    time.sleep(6.5)
    with p.lock: fire_final = list(p.block_updates[fire_mark:])
    s.check("FIRE_DECAY", (fx, grass_y + 1, fz, 0) in fire_final,
            "обычный fire без топлива должен потухнуть; netherrack/soul-fire остаются стабильными")

    # Grass block сам по себе НЕ горючий: на superflat не должно появляться
    # ни одного соседнего fire и тем более бесконечной волны.
    gx, gz = bxi + 55, bzi
    p.send_command(f"setblock {gx} {grass_y} {gz} grass_block")
    for ox, oz in ((0,0),(1,0),(-1,0),(0,1),(0,-1)):
        p.send_command(f"setblock {gx + ox} {grass_y + 1} {gz + oz} air")
    p.set_creative_slot(36, FLINT_AND_STEEL, 1); time.sleep(0.2)
    with p.lock: grass_fire_mark = len(p.block_updates)
    p.place_block(gx, grass_y, gz, face=1)
    time.sleep(3.0)
    with p.lock: grass_fire_updates = list(p.block_updates[grass_fire_mark:])
    grass_wave = [(x,y,z) for x,y,z,st in grass_fire_updates
                  if st == 2391 and (x,y,z) != (gx,grass_y+1,gz)]
    s.check("FIRE_NO_GRASS_WAVE", not grass_wave,
            f"соседних fire на grass_block: {len(grass_wave)}")

    # Survival bucket transaction must be reusable: filled -> empty -> filled -> empty.
    wx, wz = bxi + 58, bzi
    for dz in (0, 3):
        p.send_command(f"setblock {wx} {grass_y} {wz + dz} stone")
        p.send_command(f"setblock {wx} {grass_y + 1} {wz + dz} air")
    p.send_command("gm0 SmokeBotP"); time.sleep(0.3)
    p.set_creative_slot(36, WATER_BUCKET, 1); time.sleep(0.2)
    with p.lock: bucket_mark = len(p.block_updates)
    p.place_block(wx, grass_y, wz, face=1)                 # 909 -> 908
    time.sleep(0.3)
    p.place_block(wx, grass_y + 1, wz, face=1)             # 908 -> 909, забрать source
    time.sleep(0.3)
    p.place_block(wx, grass_y, wz + 3, face=1)             # 909 -> 908 снова
    time.sleep(0.7)
    with p.lock: bucket_updates = list(p.block_updates[bucket_mark:])
    bucket_reuse = ((wx, grass_y + 1, wz, 80) in bucket_updates and
                    (wx, grass_y + 1, wz, 0) in bucket_updates and
                    (wx, grass_y + 1, wz + 3, 80) in bucket_updates)
    s.check("BUCKET_REUSE", bucket_reuse, f"updates: {bucket_updates[-12:]}")

    # ---- ENDER PEARL: common projectile raycast + teleport + 5 fall damage ----
    p.send_command("gm0 SmokeBotP"); time.sleep(0.5)
    if p.health <= 0.5:
        p.respawn(); time.sleep(3.3)
    ex, ez = bxi + 52, bzi
    p.send_command(f"setblock {ex} {grass_y} {ez} grass_block")
    for dz in range(1, 10):
        p.send_command(f"setblock {ex} {grass_y} {ez + dz} grass_block")
        for yy in range(grass_y + 1, grass_y + 5):
            p.send_command(f"setblock {ex} {yy} {ez + dz} air")
    for yy in range(grass_y + 1, grass_y + 5):
        p.send_command(f"setblock {ex} {yy} {ez + 8} stone")
    time.sleep(0.8)
    p.send_pos_rot(ex + 0.5, grass_y + 1, ez + 0.5, 0.0, 0.0, True)
    time.sleep(0.3)
    p.set_creative_slot(36, ENDER_PEARL, 3); time.sleep(0.25)
    with p.lock:
        pearl_before = {e for e, t in p.entity_types.items() if t == 32}
        cooldown_mark = len(p.cooldown_events)
        swing_mark = len(p.arm_animations)
    ph0 = p.health
    p.use_item(0)
    time.sleep(0.05)
    p.use_item(0)  # должен быть отклонён тем же 20-tick ItemCooldowns
    time.sleep(2.0)
    with p.lock:
        pearl_new = {e for e, t in p.entity_types.items() if t == 32} - pearl_before
    peid = nearest_spawn(p, pearl_new, ex + 0.5, ez + 0.5, 1.0)
    with p.lock:
        ptraj = list(p.item_tp.get(peid, [])) if peid is not None else []
        pe_removed = peid in p.removed if peid is not None else False
        pearl_cooldowns = list(p.cooldown_events[cooldown_mark:])
        pearl_swings = [event for event in p.arm_animations[swing_mark:] if event[0] == p.eid]
    moved_forward = p.pos is not None and p.pos[2] > ez + 4.0
    s.check("PEARL_SPAWN", peid is not None and len(pearl_new) == 1,
            f"EnderPearl type=32 после двойного use: {len(pearl_new)} (ожидалась ровно 1)")
    s.check("PEARL_COOLDOWN", (ENDER_PEARL, 20) in pearl_cooldowns and (ENDER_PEARL, 0) in pearl_cooldowns,
            f"cooldown events: {pearl_cooldowns}")
    s.check("PEARL_SWING", len(pearl_swings) == 1 and pearl_swings[0][1] == 0,
            f"arm animations прин����того броска: {pearl_swings}")
    s.check("PEARL_ARC", len(ptraj) >= 3 and (max(v[2] for v in ptraj) - min(v[2] for v in ptraj)) > 3.0,
            f"точек: {len(ptraj)}, путь Z: {(max(v[2] for v in ptraj)-min(v[2] for v in ptraj)) if ptraj else 0:.2f}")
    s.check("PEARL_HIT", pe_removed and moved_forward and (ph0 - p.health) > 0.0,
            f"entity удалена: {'да' if pe_removed else 'нет'}, teleport Z={p.pos[2] if p.pos else '?'}; урон {ph0-p.health:.1f}")

    # ---- SNOWBALL / EGG: same throwable pipeline without pearl teleport/cooldown ----
    p.send_command("gm1 SmokeBotP"); time.sleep(0.5)
    tx, tz = bxi + 64, bzi
    p.send_command(f"setblock {tx} {grass_y} {tz} grass_block")
    for dz in range(1, 11):
        p.send_command(f"setblock {tx} {grass_y} {tz + dz} grass_block")
        for yy in range(grass_y + 1, grass_y + 5):
            p.send_command(f"setblock {tx} {yy} {tz + dz} air")
    for yy in range(grass_y + 1, grass_y + 5):
        p.send_command(f"setblock {tx} {yy} {tz + 8} stone")
    time.sleep(0.8)
    p.send_pos_rot(tx + 0.5, grass_y + 1, tz + 0.5, 0.0, 0.0, True)
    time.sleep(0.3)

    def run_throwable_test(item_id, entity_type, prefix, expect_xp=False):
        p.set_creative_slot(36, item_id, 8)
        time.sleep(0.25)
        with p.lock:
            before = {e for e, t in p.entity_types.items() if t == entity_type}
            xp_before = p.experience_total
        p.use_item(0)
        time.sleep(1.6)
        with p.lock:
            created = {e for e, t in p.entity_types.items() if t == entity_type} - before
        eid = nearest_spawn(p, created, tx + 0.5, tz + 0.5, 1.0)
        with p.lock:
            traj = list(p.item_tp.get(eid, [])) if eid is not None else []
            removed = eid in p.removed if eid is not None else False
            xp_after = p.experience_total
        s.check(f"{prefix}_SPAWN", eid is not None and len(created) == 1,
                f"type={entity_type} новых сущностей: {len(created)}")
        s.check(f"{prefix}_ARC", len(traj) >= 2 and (max(v[2] for v in traj) - min(v[2] for v in traj)) > 2.5,
                f"точек: {len(traj)}, путь Z: {(max(v[2] for v in traj)-min(v[2] for v in traj)) if traj else 0:.2f}")
        s.check(f"{prefix}_HIT", removed,
                f"entity удалена после удара: {'да' if removed else 'нет'}")
        if expect_xp:
            s.check(f"{prefix}_GAIN", xp_after > xp_before,
                    f"xp total: {xp_before} -> {xp_after}")

    run_throwable_test(SNOWBALL, 97, "SNOWBALL")
    run_throwable_test(EGG, 28, "EGG")
    run_throwable_test(EXPERIENCE_BOTTLE, 37, "XP_BOTTLE", expect_xp=True)


# ---------------------------------------------------------------- SMOKE_DIM_V1
def dim_wait(bot, name, timeout=25.0):
    """Waits for a Respawn packet that puts the bot into the given dimension."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if bot.dim_name == name:
            return True
        time.sleep(0.2)
    return False


def region_set(bot, x1, y1, z1, x2, y2, z2, block, settle=0.55):
    """MiniEdit cuboid fill: //pos1 -> //pos2 -> //set <block>."""
    bot.send_pos_raw(x1 + 0.5, y1, z1 + 0.5, True); time.sleep(0.22)
    bot.send_command("/pos1"); time.sleep(0.22)
    bot.send_pos_raw(x2 + 0.5, y2, z2 + 0.5, True); time.sleep(0.22)
    bot.send_command("/pos2"); time.sleep(0.22)
    bot.send_command("set " + block); time.sleep(settle)


def terrain_report(bot):
    """Flat preset => every column looks identical; real generation varies."""
    with bot.lock:
        profiles = {k: list(v) for k, v in bot.chunk_profiles.items()}
        biome_map = {k: list(v) for k, v in bot.chunk_biomes.items()}
    biomes = set()
    for v in biome_map.values():
        biomes.update(v)
    columns = varied = empty = 0
    sections = set()
    tops = []
    signatures = set()
    for counts in profiles.values():
        nonzero = [i for i, c in enumerate(counts) if c > 0]
        if not nonzero:
            empty += 1
            continue
        columns += 1
        sections.update(nonzero)
        tops.append(nonzero[-1])
        signatures.add(tuple(counts))
        partial = [c for c in counts if 0 < c < 4096]
        if len(set(partial)) >= 2:
            varied += 1
    return {
        "chunks": len(profiles), "columns": columns, "empty": empty,
        "varied": varied, "sections": sections, "tops": tops,
        "signatures": len(signatures), "biomes": biomes,
    }


def falling_probe(bot, s, px, py, pz, label):
    """Places sand next to a floating block: gravity must run in this dimension."""
    region_set(bot, px + 2, py + 4, pz, px + 2, py + 4, pz, "obsidian")
    bot.set_creative_slot(36, SAND_ITEM); time.sleep(0.4)
    with bot.lock:
        known = set(bot.entity_types)
    bot.place_block(px + 2, py + 4, pz, face=4)   # west face -> sand hangs in the air
    time.sleep(2.0)
    with bot.lock:
        fresh = [eid for eid, t in bot.entity_types.items()
                 if t == FALLING_BLOCK_TYPE and eid not in known]
    s.check(label, len(fresh) >= 1,
            "FallingBlockEntity: %d (гравитация вне оверворлда)" % len(fresh))


def run_dimension_phase(s, args, ground_section, grass_y):
    """SMOKE_DIM_V1: real Nether/End terrain, corner-less Nether portal, End portal."""
    d = Bot("SmokeBotD", args.host, args.port, ground_section)
    d.start()
    if not d.in_play.wait(30) or d.error:
        s.check("DIM_LOGIN", False, d.error or "таймаут входа dim-бота")
        return
    d.positioned.wait(20)
    time.sleep(4.0)
    if d.pos is None:
        s.check("DIM_LOGIN", False, "нет позиции dim-бота")
        return
    s.check("DIM_LOGIN", True, "dim-бот в игре, Y=%.1f" % d.pos[1])
    d.send_command("gm1 SmokeBotD")   # creative: one-tick portal delay
    time.sleep(0.9)

    # ---------------- corner-less Nether portal (PORTAL_V2) ----------------
    wx, wz = 700, 700
    y0 = int(d.pos[1]) + 1   # SMOKE_V14: от реальной высоты бота, а не от высоты флэт-травы
    base_y = y0
    for _ in range(6):
        d.send_pos_raw(wx + 0.5, y0, wz + 0.5, True); time.sleep(0.3)
    time.sleep(2.0)   # wait for the work-area chunks

    region_set(d, wx - 2, y0 - 1, wz - 1, wx + 3, y0 + 5, wz + 1, "air")
    region_set(d, wx - 2, y0 - 2, wz - 1, wx + 3, y0 - 2, wz + 1, "obsidian")   # SMOKE_V14: опора в любом мире
    region_set(d, wx, y0 - 1, wz, wx + 1, y0 - 1, wz, "obsidian")      # floor
    region_set(d, wx, y0 + 3, wz, wx + 1, y0 + 3, wz, "obsidian")      # ceiling
    region_set(d, wx - 1, y0, wz, wx - 1, y0 + 2, wz, "obsidian")      # left column
    region_set(d, wx + 2, y0, wz, wx + 2, y0 + 2, wz, "obsidian")      # right column
    # the four corners stay air on purpose - vanilla does not need them

    inner = {(wx + dx, y0 + dy, wz) for dx in (0, 1) for dy in (0, 1, 2)}
    with d.lock:
        mark = len(d.block_updates)
    d.set_creative_slot(36, FLINT_AND_STEEL); time.sleep(0.5)
    d.place_block(wx, y0 - 1, wz, face=1)   # click the floor -> ignite the cell above
    time.sleep(1.6)
    with d.lock:
        fresh = list(d.block_updates[mark:])
    lit = {(x, y, z): st for x, y, z, st in fresh if (x, y, z) in inner and st != 0}
    states = set(lit.values())
    ok_lit = len(lit) == 6 and len(states) == 1
    s.check("PORTAL_LIT", ok_lit,
            "портальных блоков %d/6, states=%s" % (len(lit), sorted(states)))
    s.check("PORTAL_NO_CORNERS", ok_lit,
            "рамка без угловых блоков зажигается" if ok_lit else "без углов портал не зажёгся")

    # ---------------- travel to the Nether ----------------
    d.reset_chunk_stats()
    for _ in range(12):
        d.send_pos_raw(wx + 0.5, y0, wz + 0.5, True)
        time.sleep(0.3)
        if d.dim_name == NETHER_ID:
            break
    in_nether = dim_wait(d, NETHER_ID, 20)
    s.check("PORTAL_TRAVEL", in_nether, "dimension=%s" % d.dim_name)
    if not in_nether:
        return
    time.sleep(3.0)
    scaled = abs(d.pos[0] - (wx + 0.5) / 8.0) <= 40 and abs(d.pos[2] - (wz + 0.5) / 8.0) <= 40
    s.check("PORTAL_SCALE", scaled,
            "1:8 — прибытие (%.1f, %.1f, %.1f)" % d.pos)

    time.sleep(5.0)   # let the Nether chunks stream in
    rep = terrain_report(d)
    s.check("NETHER_TERRAIN", rep["varied"] >= 3 and rep["signatures"] >= 4,
            "чанков %d, разных профилей %d, неровных %d (флэт дал бы 1 профиль)"
            % (rep["chunks"], rep["signatures"], rep["varied"]))
    tops = rep["tops"]
    roof = bool(tops) and tops.count(max(set(tops), key=tops.count)) >= max(1, int(len(tops) * 0.8))
    s.check("NETHER_CEILING", roof and len(rep["sections"]) >= 4,
            "верхние секции %s, всего секций %d" % (sorted(set(tops)), len(rep["sections"])))
    nether_biomes = rep["biomes"] & NETHER_BIOMES
    s.check("NETHER_BIOMES", len(nether_biomes) >= 1 and 39 not in rep["biomes"],
            "биомы %s (ожидались %s)" % (sorted(rep["biomes"]), sorted(NETHER_BIOMES)))

    # ---------------- physics inside the Nether ----------------
    px, py, pz = int(d.pos[0]), int(d.pos[1]), int(d.pos[2])
    region_set(d, px - 3, py, pz - 3, px + 3, py + 6, pz + 3, "air")
    region_set(d, px - 3, py - 1, pz - 3, px + 3, py - 1, pz + 3, "obsidian")
    falling_probe(d, s, px, py, pz, "NETHER_FALLING")

    d.set_creative_slot(36, LAVA_BUCKET); time.sleep(0.4)
    with d.lock:
        mark = len(d.block_updates)
    d.place_block(px, py - 1, pz, face=1)
    time.sleep(3.5)
    with d.lock:
        fresh = list(d.block_updates[mark:])
    spread = {(x, z) for x, y, z, st in fresh if y == py and st != 0 and (x, z) != (px, pz)}
    s.check("NETHER_FLUID", len(spread) >= 2,
            "лава растеклась на %d клеток (tickFluidsIn в Аду)" % len(spread))

    # ---------------- the End: island + void ----------------
    d.reset_chunk_stats()
    d.send_command("end")
    in_end = dim_wait(d, END_ID, 25)
    s.check("END_TRAVEL", in_end, "dimension=%s" % d.dim_name)
    if in_end:
        time.sleep(6.0)
        rep = terrain_report(d)
        s.check("END_ISLAND", rep["varied"] >= 1 and rep["columns"] >= 1,
                "чанков с рельефом %d, разных профилей %d" % (rep["columns"], rep["signatures"]))
        s.check("END_VOID", rep["empty"] >= 1,
                "пустых чанков вокруг острова: %d" % rep["empty"])
        s.check("END_BIOMES", len(rep["biomes"] & END_BIOMES) >= 1,
                "биомы %s" % sorted(rep["biomes"]))
        if d.pos is not None:
            ex, ey, ez = int(d.pos[0]), int(d.pos[1]), int(d.pos[2])
            region_set(d, ex - 3, ey, ez - 3, ex + 3, ey + 6, ez + 3, "air")
            region_set(d, ex - 3, ey - 1, ez - 3, ex + 3, ey - 1, ez + 3, "obsidian")
            falling_probe(d, s, ex, ey, ez, "END_FALLING")

    # ---------------- End portal frames + eyes (ENDPORTAL_V1) ----------------
    d.send_command("overworld")
    if not dim_wait(d, OVERWORLD_ID, 25):
        s.check("ENDPORTAL_EYES", False, "не вернулись в оверворлд")
        return
    time.sleep(2.5)
    cx, cz = 800, 800
    fy = base_y   # SMOKE_V14
    for _ in range(6):
        d.send_pos_raw(cx + 0.5, fy, cz + 0.5, True); time.sleep(0.3)
    time.sleep(2.0)
    region_set(d, cx - 3, fy, cz - 3, cx + 3, fy + 4, cz + 3, "air")
    region_set(d, cx - 3, fy - 1, cz - 3, cx + 3, fy - 1, cz + 3, "obsidian")   # SMOKE_V14
    region_set(d, cx - 2, fy, cz - 1, cx - 2, fy, cz + 1, "end_portal_frame")
    region_set(d, cx + 2, fy, cz - 1, cx + 2, fy, cz + 1, "end_portal_frame")
    region_set(d, cx - 1, fy, cz - 2, cx + 1, fy, cz - 2, "end_portal_frame")
    region_set(d, cx - 1, fy, cz + 2, cx + 1, fy, cz + 2, "end_portal_frame")

    ring = ([(cx - 2, cz + i) for i in (-1, 0, 1)] + [(cx + 2, cz + i) for i in (-1, 0, 1)]
            + [(cx + i, cz - 2) for i in (-1, 0, 1)] + [(cx + i, cz + 2) for i in (-1, 0, 1)])
    centre = {(cx + i, fy, cz + j) for i in (-1, 0, 1) for j in (-1, 0, 1)}
    with d.lock:
        mark = len(d.block_updates)
    d.set_creative_slot(36, ENDER_EYE_ITEM); time.sleep(0.5)
    for fx, fz in ring:
        d.place_block(fx, fy, fz, face=1)
        time.sleep(0.28)
    time.sleep(2.0)
    with d.lock:
        fresh = list(d.block_updates[mark:])
    eyed = {(x, z) for x, y, z, st in fresh if y == fy and (x, z) in ring and st != 0}
    filled = {(x, y, z): st for x, y, z, st in fresh if (x, y, z) in centre and st != 0}
    s.check("ENDPORTAL_EYES", len(eyed) >= 12,
            "рамок с глазом %d/12" % len(eyed))
    s.check("ENDPORTAL_FILL", len(filled) == 9 and len(set(filled.values())) == 1,
            "блоков портала %d/9, states=%s" % (len(filled), sorted(set(filled.values()))))

    time.sleep(16.0)   # portal cooldown after the Nether trip (300 ticks)
    for _ in range(12):
        d.send_pos_raw(cx + 0.5, fy, cz + 0.5, True)
        time.sleep(0.3)
        if d.dim_name == END_ID:
            break
    arrived = dim_wait(d, END_ID, 20)
    s.check("ENDPORTAL_TRAVEL", arrived, "dimension=%s" % d.dim_name)
    if arrived:
        time.sleep(3.0)
        x, y, z = d.pos
        s.check("END_PLATFORM", abs(x - 100.5) < 3 and abs(z - 0.5) < 3 and 46 <= y <= 54,
                "высадка (%.1f, %.1f, %.1f), ждали (100.5, 49, 0.5)" % (x, y, z))
        s.check("END_ALIVE", d.health > 0.0, "хп после высадки: %.1f" % d.health)
    d.send_command("overworld")


# ------------------------------------------------------------- SMOKE_V14
_REAL_SLEEP = time.sleep


def install_time_scale(scale):
    """Растягивает ВСЕ ожидания сразу: на слабом CPU сервер не успевает за 20 tps."""
    if abs(scale - 1.0) < 0.05:
        return

    def scaled(seconds):
        _REAL_SLEEP(seconds * scale)

    time.sleep = scaled


def measure_tps(bot, window=8.0):
    """Сервер шлёт Update Time каждые 20 тиков, так что интервал = 20 / tps."""
    with bot.lock:
        bot.time_stamps.clear()
    _REAL_SLEEP(window)
    with bot.lock:
        stamps = list(bot.time_stamps)
    if len(stamps) < 3:
        return None, None
    gaps = sorted(b - a for a, b in zip(stamps, stamps[1:]))
    median = gaps[len(gaps) // 2]
    if median <= 0.05:
        return None, None
    return 20.0 / median, median


def calibrate(args, bot):
    """SMOKE_V14: автоподстройка под реальный TPS сервера."""
    if args.time_scale != "auto":
        scale = max(0.5, min(6.0, float(args.time_scale)))
        print("[LAG] ручной множитель ожиданий x%.2f" % scale, flush=True)
        install_time_scale(scale)
        return scale
    tps, median = measure_tps(bot)
    if tps is None:
        print("[LAG] TPS не измерился (нет Update Time), берём x1.5", flush=True)
        install_time_scale(1.5)
        return 1.5
    scale = max(1.0, min(4.0, median))
    print("[LAG] сервер даёт ~%.1f tps -> все ожидания x%.2f" % (tps, scale), flush=True)
    install_time_scale(scale)
    return scale


def world_precheck(s, bot, args, spawn_y):
    """SMOKE_V14: большинство тестов ждёт свежий ФЛЭТ-мир.

    На обычном мире они дают десятки ложных FAIL: боты строят в толще камня
    на Y=4, а земля там на Y=112. Лучше честно остановиться через 20 секунд,
    чем гнать 10 минут и получить фейковый отчёт.
    """
    y_ok = bot.pos is not None and abs(bot.pos[1] - spawn_y) < 0.01
    chunks_ok = bot.chunks_total > 0 and bot.chunks_bad == 0
    biome_ok = bot.biome_single == 39
    y_info = ("спавн Y=%s (ожидался %s); если тут -60 — старый world/spawn.dat"
              % (bot.pos[1] if bot.pos else '?', spawn_y))
    chunk_info = ("чанков: %d, старых (блоки на -64..-49): %d, на родной высоте: %d, ошибок парсинга: %d"
                  % (bot.chunks_total, bot.chunks_bad, bot.chunks_good, bot.chunk_parse_errors))
    biome_info = "биом секции земли: %s (ожидался plains=39)" % bot.biome_single
    flat = y_ok and chunks_ok and biome_ok
    if flat or not args.any_world:
        s.check("SPAWN_Y", y_ok, y_info)
        s.check("CHUNKS", chunks_ok, chunk_info)
        s.check("BIOME", biome_ok, biome_info)
    else:
        s.skip("SPAWN_Y", y_info)
        s.skip("CHUNKS", chunk_info)
        s.skip("BIOME", biome_info)
    if flat:
        return True
    print("", flush=True)
    print("=" * 70, flush=True)
    print("ЭТО НЕ ФЛЭТ-МИР (или мир не свежий).", flush=True)
    print("Боты строят и ломают на Y=%d; если там камень или воздух, почти всё упадёт зря." % args.grass_y, flush=True)
    print("Нужно: level-type=FLAT, чистая папка world/, или запуск с --grass-y под ваш мир.", flush=True)
    print("Флаг --any-world запустит прогон всё равно (результаты будут ненадёжными).", flush=True)
    print("=" * 70, flush=True)
    return bool(args.any_world)


def server_status(host, port, timeout=5.0):
    """SMOKE_V15: обычный server list ping — узнаём max-players ДО того, как забьём слоты."""
    sock = None
    try:
        sock = socket.create_connection((host, port), timeout=timeout)
        sock.settimeout(timeout)
        host_b = host.encode("utf-8")
        payload = (enc_varint(0x00) + enc_varint(PROTOCOL) + enc_varint(len(host_b)) + host_b
                   + struct.pack(">H", port) + enc_varint(1))
        sock.sendall(enc_varint(len(payload)) + payload)
        req = enc_varint(0x00)
        sock.sendall(enc_varint(len(req)) + req)

        def read_varint():
            num = 0
            shift = 0
            while True:
                chunk = sock.recv(1)
                if not chunk:
                    raise ConnectionError("status: соединение закрыто")
                byte = chunk[0]
                num |= (byte & 0x7F) << shift
                if not (byte & 0x80):
                    return num
                shift += 7

        length = read_varint()
        data = b""
        while len(data) < length:
            chunk = sock.recv(length - len(data))
            if not chunk:
                break
            data += chunk
        r = Reader(data)
        r.varint()          # packet id 0x00
        return json.loads(r.string())
    except Exception:
        return None
    finally:
        if sock is not None:
            try:
                sock.close()
            except Exception:
                pass


RESERVED_SLOTS = 5   # SmokeBotA, SmokeBotB, SmokeBotC, физ-бот, dim-бот


def plan_crowd_size(args, s):
    """SMOKE_V15: считаем бюджет слотов.

    Раньше толпа съедала все слоты, и физ-бот с dim-ботом получали «Сервер
    заполнен» — половина прогона просто не выполнялась.
    """
    status = server_status(args.host, args.port)
    if not status:
        print("[SLOTS] статус-пинг не ответил, берём crowd как есть", flush=True)
        return args.crowd_bots, None
    try:
        max_players = int(status["players"]["max"])
        online = int(status["players"]["online"])
    except Exception:
        return args.crowd_bots, None
    allowed = max_players - online - RESERVED_SLOTS
    if allowed < 0:
        allowed = 0
    if args.crowd_bots <= allowed:
        print("[SLOTS] max-players=%d, занято %d, crowd=%d, резерв %d слотов под фазовые боты"
              % (max_players, online, args.crowd_bots, RESERVED_SLOTS), flush=True)
        return args.crowd_bots, max_players
    print("", flush=True)
    print("[SLOTS] max-players=%d — на %d crowd-ботов места не хватит." % (max_players, args.crowd_bots), flush=True)
    print("[SLOTS] снижаю толпу до %d, чтобы остались слоты под физ-бота, dim-бота и бота C." % allowed, flush=True)
    print("[SLOTS] чтобы гнать все %d: поставьте max-players=%d в server.properties."
          % (args.crowd_bots, args.crowd_bots + RESERVED_SLOTS), flush=True)
    print("", flush=True)
    return allowed, max_players


def main():
    ap = argparse.ArgumentParser(description="Zevvoryn smoke bot (1.21.1)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=25565)
    ap.add_argument("--grass-y", type=int, default=3,
                    help="ожидаемый Y травы флэта (FLATNATIVE_V1: 3)")
    ap.add_argument("--crowd-bots", type=int, default=29,
                    help="дополнительные активные боты (SMOKE_V14: по умолчанию 29, все работают параллельно по 8 заданиям А-З)")
    ap.add_argument("--time-scale", default="auto",
                    help="SMOKE_V14: множитель всех ожиданий. auto = считается по реальному TPS сервера")
    ap.add_argument("--any-world", action="store_true",
                    help="SMOKE_V14: не останавливаться, если мир не флэт (результаты будут ненадёжными)")
    args = ap.parse_args()

    ground_section = (args.grass_y + 64) // 16
    spawn_y = args.grass_y + 1
    s = Suite()
    print(f"SMOKE_V15: сервер {args.host}:{args.port}, ожидаем траву на Y={args.grass_y}, спавн Y={spawn_y}, crowd={args.crowd_bots}", flush=True)

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

    # SMOKE_V14: сначала убеждаемся, что мир вообще тот, под который написаны тесты,
    # и только потом зовём толпу и мучаем слабый CPU десять минут.
    a.positioned.wait(20)
    b.positioned.wait(20)
    _REAL_SLEEP(6)   # даём чанкам доехать
    if not world_precheck(s, a, args, spawn_y):
        s.summary()
        sys.exit(2)

    # SMOKE_V14: замеряем TPS до нагрузки и растягиваем все ожидания под машину.
    calibrate(args, a)

    # SMOKE_V15: фазовые боты заходят ПЕРВЫМИ — иначе толпа съедает все слоты
    # и целые фазы (физика, порталы) падают с «Сервер заполнен».
    physics_thread = threading.Thread(
        target=run_physics_phase,
        args=(s, args, ground_section, args.grass_y),
        daemon=True,
    )
    physics_thread.start()
    dimension_thread = threading.Thread(
        target=run_dimension_phase,
        args=(s, args, ground_section, args.grass_y),
        daemon=True,
    )
    dimension_thread.start()
    _REAL_SLEEP(3.0)   # даём им взять свои слоты

    planned, max_players = plan_crowd_size(args, s)

    crowd = []
    server_full = False
    for i in range(planned):
        bot = Bot(f"SmokeCrowd{i + 1:02d}", args.host, args.port, ground_section)
        crowd.append(bot)
        bot.start()
        time.sleep(0.05)
        if any(b.error and "заполнен" in b.error for b in crowd):
            server_full = True
            print("[SLOTS] сервер сказал «заполнен» на боте %d — больше не зовём" % (i + 1), flush=True)
            break
    for bot in crowd:
        bot.in_play.wait(30)
    crowd_ok = [bot for bot in crowd if bot.error is None and bot.in_play.is_set()]
    if server_full or planned < args.crowd_bots:
        s.skip("CROWD_BOTS",
               f"в игре {len(crowd_ok)} из запрошенных {args.crowd_bots}: мало слотов "
               f"(max-players={max_players if max_players else '?'}), это настройка сервера, а не баг")
    else:
        s.check("CROWD_BOTS", len(crowd_ok) == planned,
                f"crowd online: {len(crowd_ok)}/{planned}")

    # CROWD_SWARM_V3: раньше только первые 4 crowd-бота реально работали, а
    # остальные сразу уходили в gm3 (креатив) и просто стояли без дела до конца
    # прогона. Теперь КАЖДЫЙ crowd-бот получает задание — они по кругу делятся
    # на 5 групп (А/Б/В/Г/Д), каждая проверяет свою область физики, и никто не
    # остаётся простаивать в креативе. move_bot_to_work_area сам п��реводит
    # бота в gm0 (выживание) прямо перед началом его задания.
    swarm_groups = [
        ("А", "падающие блоки (гравий)", crowd_swarm_falling, 900),
        ("Б", "взрывчатка (TNT)", crowd_swarm_tnt, 980),
        ("В", "огонь и распространение", crowd_swarm_fire, 1060),
        ("Г", "метательные снаряды", crowd_swarm_projectile, 1140),
        ("Д", "жидкости (бетон в воде)", crowd_swarm_concrete, 1220),
        ("Е", "фермерство (мотыга/посадка/удобрение)", crowd_swarm_farming, 1300),
        ("Ж", "вытаптывание farmland", crowd_swarm_trample, 1380),
        ("З", "бамбук: опора+рост через /warprandomtick", crowd_swarm_bamboo, 1460),
    ]
    swarm_threads = []
    group_rows = [0] * len(swarm_groups)
    for i, worker in enumerate(crowd_ok):
        gi = i % len(swarm_groups)
        letter, desc, fn, gx = swarm_groups[gi]
        row = group_rows[gi]
        group_rows[gi] += 1
        gz = 900 + row * 60  # 60 блоков между работниками одной группы — без пересечения зон
        label = f"{letter}{row + 1}"
        print(f"[ЗАДАНИЕ {label}] {worker.bot_name} -> {desc} @ ({gx},{gz})", flush=True)
        th = threading.Thread(target=fn, args=(worker, s, args.grass_y, gx, gz, label), daemon=True)
        th.start()
        swarm_threads.append(th)
    s.check("CROWD_SWARM", len(swarm_threads) == len(crowd_ok),
            f"активных рабочих ботов: {len(swarm_threads)}/{len(crowd_ok)} (задания А-З, никто не простаивает в gm3)")

    # SMOKE_V14: SPAWN_Y / CHUNKS / BIOME уже проверены в world_precheck до входа толпы

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

    # PVP: быстрый даблклик кулаком (проверка i-frames с досчёто�� разницы)
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
            f"алмазный м��ч, полный замах: урон {drop:.2f} (ожидалось ~7)")
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
            f"пакетов отброса при блоке: +{a.velocity_events - v0} (��тброса быть не должно, SHIELD_V2)")

    # SHIELD_DOWN: опускаем щит — урон снова проходит (сквозь броню ~3.78)
    a.release_use()
    drop = hit_and_measure(b, a, target)
    s.check("SHIELD_DOWN", drop > 0.5,
            f"щит опущен: урон {drop:.2f} (должен проходить)")

    # SHIELD_NEWVIS: A снова поднимает щит, заходит бот C — новичок ��олжен видеть щит (SHIELD_V2)
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
    a.release_use()  # сбрасываем локальное удержание use после AXE_BREAK
    time.sleep(0.15)
    a.use_item(1)  # попытка поднять во время кд — сервер должен игнорить
    time.sleep(0.45)
    b.set_creative_slot(36, DIAMOND_SWORD)
    drop = hit_and_measure(b, a, target)
    s.check("SHIELD_CD", drop > 0.5,
            f"удар во время кд щита: урон {drop:.2f} (должен проходить)")

    # PHYS_PARALLEL_V1: физический пакет гоняем параллельно с combat/time.

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

    physics_thread.join()
    dimension_thread.join()   # SMOKE_DIM_V1
    for th in swarm_threads:
        th.join()

    ok = s.summary()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nПрервано", flush=True)
        sys.exit(130)
