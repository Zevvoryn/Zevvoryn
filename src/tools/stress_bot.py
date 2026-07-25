#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
STRESS_V1: нагрузочный тест сервера Zevvoryn — N ботов (по умолчанию 500)
заходят, бегают по миру, прыгают, болтают в чат и постоянно перезаходят.

Зачем:
  • сетевой поток: тысячи пакетов движения в секунду — теряются ли пакеты, растут ли лаги;
  • утечки памяти: массовый вход/выход игроков (churn) — следи за RAM zevvoryn.exe
    в диспетчере задач: после теста память должна вернуться примерно к уровню до теста;
  • TPS: смотри TPS-боссбар/лог сервера во время прогона — не проседает ли ниже 20.

Запуск: python tools/stress_bot.py [--bots 100] [--duration 120] [--churn 0.2]
        [--host 127.0.0.1] [--port 25565]
  --bots      сколько ботов (default 500)
  --duration  длительность теста в секундах (default 120)
  --churn     доля ботов в режиме вечного перезахода (default 0 — 500 стабильных; ставь 0.2 для теста утечек)
Требования: Python 3.10+, offline-режим. Не запускай на проде :)
Лежит рядом со smoke_bot.py и переиспользует его протокольный движок.
"""
import argparse
import math
import os
import random
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_bot import Bot, enc_varint, enc_string  # noqa: E402

try:
    import psutil  # STRESS_RAM_V1: точное измерение RSS сервера, если модуль стоит
except ImportError:
    psutil = None


def find_server_process(exe_name="zevvoryn.exe"):
    """STRESS_RAM_V1: находим PID сервера по имени процесса (psutil, иначе tasklist на Windows)."""
    if psutil is not None:
        for proc in psutil.process_iter(["pid", "name"]):
            try:
                if proc.info["name"] and exe_name.lower() in proc.info["name"].lower():
                    return proc
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue
        return None
    return None


def read_server_ram_mb(proc, exe_name="zevvoryn.exe"):
    """STRESS_RAM_V1 -> STRESS_RAM_V2: RSS сервера в МБ. Возвращает None, если процесс не найден/недоступен."""
    if psutil is not None and proc is not None:
        try:
            return proc.memory_info().rss / (1024 * 1024)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass  # STRESS_RAM_V2: не сдаёмся сразу - пробуем tasklist ниже
    # STRESS_RAM_V2: подстраховка через tasklist (Windows) — срабатывает, даже если psutil установлен,
    # но find_server_process() не нашёл процесс (например, сервер запущен от имени администратора,
    # а сама консоль со stress.bat — нет: process_iter() тогда молча пропускает чужой процесс через
    # AccessDenied). tasklist по имени образа работает независимо от уровня привилегий скрипта.
    if os.name == "nt":
        try:
            out = subprocess.check_output(
                ["tasklist", "/FI", f"IMAGENAME eq {exe_name}", "/FO", "CSV", "/NH"],
                text=True, stderr=subprocess.DEVNULL,
            )
            line = out.strip().splitlines()[0] if out.strip() else ""
            parts = [p.strip('"') for p in line.split('","')]
            if len(parts) >= 5 and parts[0].lower() == exe_name.lower():
                mem_str = parts[4].replace(",", "").replace(" \u041a", "").replace(" K", "").strip()
                return int(mem_str) / 1024.0
        except (subprocess.CalledProcessError, IndexError, ValueError, FileNotFoundError):
            return None
    return None


class StressBot(Bot):
    def __init__(self, name, host, port):
        super().__init__(name, host, port, ground_section=4)
        self.stopped = threading.Event()
        self.sent = 0
        self.recv = 0

    def parse_chunk(self, r):  # чанки не разбираем — экономим CPU на стороне ботов
        with self.lock:
            self.chunks_total += 1

    def dispatch_play(self, pid, r):
        self.recv += 1
        super().dispatch_play(pid, r)

    def send_packet(self, pid, payload=b""):
        self.sent += 1
        super().send_packet(pid, payload)

    def close(self):
        self.stopped.set()
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass

    # STRESSREAL_V1: валидные игровые пакеты (не мусор) — swing/look/sneak/sprint/слот.
    def swing(self, hand=0):
        self.send_packet(0x36, enc_varint(hand))  # Swing Arm

    def look(self, yaw, pitch, on_ground=True):
        self.send_packet(0x1C, struct.pack(">ff", yaw, pitch) + (b"\x01" if on_ground else b"\x00"))  # Look

    def pos_look(self, x, y, z, yaw, pitch, on_ground=True):
        self.send_packet(0x1B, struct.pack(">dddff", x, y, z, yaw, pitch) + (b"\x01" if on_ground else b"\x00"))  # Position+Look

    def entity_action(self, action, jump_boost=0):
        # Player Command 0x25: 0/1 sneak start/stop, 3/4 sprint start/stop
        if self.eid is None:
            return
        self.send_packet(0x25, enc_varint(self.eid) + enc_varint(action) + enc_varint(jump_boost))

    def set_held_slot(self, slot):
        self.send_packet(0x2F, struct.pack(">h", slot))  # Set Carried Item (смена слота хотбара)


def chat_packet(msg):
    # Chat Message 0x06: message + timestamp + salt + без подписи + 0 ack + пустой bitset
    return (enc_string(msg) + struct.pack(">q", int(time.time() * 1000))
            + struct.pack(">q", 0) + b"\x00" + enc_varint(0) + b"\x00\x00\x00")


def actor_loop(bot, deadline, do_chat, move_hz=5.0, noise=0.25):
    """STRESSREAL_V1 + STRESSCALM_V1: случайное блуждание + прыжки + чат + РЕАЛЬНЫЕ пакеты,
    но без спама: движение с частотой move_hz (было жёстко 10/с), без дублирующего
    look 0x1C (поворот уже едёт в pos_look 0x1B), а swing/sprint/sneak/slot масштабируются через noise.
    Сохранены все прежние (0x1A pos, 0x06 chat, 0x09 respawn); pos+look 0x1B, swing 0x36,
    sneak/sprint 0x25, смена слота 0x2F — всё с валидными полями, а не мусором."""
    move_dt = 1.0 / move_hz if move_hz > 0 else 0.1  # STRESSCALM_V1: период между пакетами движения
    noise = max(0.0, min(1.0, noise))
    rng = random.Random(bot.bot_name)
    ang = rng.uniform(0, 2 * math.pi)
    if not bot.positioned.wait(90) or not bot.pos:  # STRESS500_V1
        return
    x, y, z = bot.pos
    next_chat = time.time() + rng.uniform(15, 40)
    jump_until = 0.0
    pitch = 0.0
    sprinting = False
    sneaking = False
    slot = 0
    while time.time() < deadline and not bot.stopped.is_set():
        if getattr(bot, "died", False):  # STRESS_V3: бота убили — возрождаемся
            bot.died = False
            bot.positioned.clear()
            try:
                bot.respawn()
            except OSError:
                return
            if bot.positioned.wait(10) and bot.pos:
                x, _, z = bot.pos
            sprinting = sneaking = False
            time.sleep(0.5)
            continue
        if rng.random() < 0.05:
            ang += rng.uniform(-1.2, 1.2)  # сменить направление
        # STRESSCALM_V1: шаг масштабируем под период, чтобы скорость ходьбы не зависела от move_hz
        step = 0.28 * (move_dt / 0.1)
        x += math.cos(ang) * step
        z += math.sin(ang) * step
        if x * x + z * z > 80 * 80:  # радиус 80 блоков от спавна
            ang += math.pi
        now = time.time()
        yy = y
        if rng.random() < 0.02:
            jump_until = now + 0.25
        if now < jump_until:
            yy = y + 1.0
        # STRESSREAL_V1: yaw из направления движения, pitch — плавное качание; оба в легальных диапазонах
        yaw = (math.degrees(-ang) + 180.0) % 360.0 - 180.0
        pitch += rng.uniform(-5, 5)
        pitch = max(-90.0, min(90.0, pitch))
        try:
            bot.pos = (x, yy, z)
            # движение: чередуем pos (0x1A, как раньше) и pos+look (0x1B)
            if rng.random() < 0.5:
                bot.send_pos()                     # 0x1A — прежний пакет движения
            else:
                bot.pos_look(x, yy, z, yaw, pitch)  # 0x1B
            # STRESSCALM_V1: убран дублирующий look 0x1C — поворот уже едёт в pos_look 0x1B выше.
            # Редкие экшены масштабируем через noise (0 = только движение).
            if rng.random() < 0.15 * noise:
                bot.swing(0)                       # 0x36 — взмах рукой
            if rng.random() < 0.04 * noise:
                sprinting = not sprinting
                bot.entity_action(3 if sprinting else 4)  # 0x25 — спринт вкл/выкл
            if rng.random() < 0.03 * noise:
                sneaking = not sneaking
                bot.entity_action(0 if sneaking else 1)   # 0x25 — присед вкл/выкл
            if rng.random() < 0.05 * noise:
                slot = rng.randint(0, 8)
                bot.set_held_slot(slot)            # 0x2F — смена слота хотбара
            if do_chat and now >= next_chat:
                bot.send_packet(0x06, chat_packet(f"стресс-тест: {bot.bot_name} на связи"))
                next_chat = now + rng.uniform(20, 50)
        except OSError:
            return
        time.sleep(move_dt)  # STRESSCALM_V1: период движения через --move-hz (было жёстко 0.1 = 10/с)


def main():
    ap = argparse.ArgumentParser(description="Zevvoryn stress test (1.21.1)")
    ap.add_argument("--bots", type=int, default=500)  # STRESS500_V1: 500 ботов по умолчанию
    ap.add_argument("--duration", type=int, default=120)
    ap.add_argument("--churn", type=float, default=0.0)  # STRESS500_V1: без churn — 500 стабильных ботов не отваливаются
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=25565)
    # STRESS_V4: скорость входа теперь настраиваемая (раньше была зашита жёстко 20/с —
    # для 1000 ботов это 50 секунд только на вход). По умолчанию теперь 50/с.
    ap.add_argument("--rate", type=float, default=25.0)  # STRESS500_V1: мягче вход (500/25=20с), меньше burst-сбросов
    # STRESSCALM_V1: частота движения и доля рандомных экшенов — чтобы боты не спамили пакетами
    ap.add_argument("--move-hz", type=float, default=5.0)  # сколько раз/с бот шлёт движение (было жёстко 10)
    ap.add_argument("--noise", type=float, default=0.25)  # множитель частоты swing/sprint/sneak/slot (0=только движение, 1=как раньше)
    args = ap.parse_args()
    join_rate = args.rate if args.rate > 0 else 50.0

    # STRESS_V1 -> STRESS_V2 -> STRESS_V4: параллельный вход, настраиваемые старты/с, не блокируемся на handshake
    print(f"STRESS_V4: {args.bots} ботов на {args.host}:{args.port}, {args.duration} с, "
          f"churn {args.churn:.0%}, вход {join_rate:.0f}/с (≈{args.bots / join_rate:.1f} с на вход всех)", flush=True)
    print("Следи за RAM zevvoryn.exe в диспетчере задач и за TPS сервера!", flush=True)

    # STRESS_RAM_V1: RAM сервера до/во время/после теста — чтобы не смотреть в диспетчер вручную
    server_proc = find_server_process()
    if psutil is None and os.name != "nt":
        print("RAM: psutil не установлен и мы не на Windows — замер RAM пропущен (поставь: pip install psutil)", flush=True)
    ram_before = read_server_ram_mb(server_proc)
    ram_peak = [ram_before or 0.0]
    ram_samples = [] if ram_before is None else [ram_before]
    if ram_before is not None:
        print(f"RAM сервера до теста: {ram_before:.0f} МБ", flush=True)
    else:
        print("RAM: процесс zevvoryn.exe не найден — замер RAM пропущен (запускай сервер до стресс-теста)", flush=True)

    deadline = time.time() + args.duration
    bots = []
    all_bots = []  # STRESS_V3: все боты за сессию — счётчики не пропадают при удалении из активных
    errors = []
    lock = threading.Lock()
    reconnects = [0]

    def launch(i):
        bot = StressBot(f"Stress{i:03d}", args.host, args.port)
        bot.start()
        if not bot.in_play.wait(90) or bot.error:  # STRESS500_V1: под 500 ботов вход занимает дольше
            with lock:
                errors.append(f"{bot.bot_name}: {bot.error or 'таймаут входа'}")
            return None
        try:
            if bot.sock:
                bot.sock.settimeout(120)  # STRESS500_V1: больше воздуха на recv, чтобы не рвать по таймауту
        except OSError:
            pass
        th = threading.Thread(target=actor_loop, args=(bot, deadline, i % 5 == 0, args.move_hz, args.noise), daemon=True)  # STRESSCALM_V1
        th.start()
        with lock:
            bots.append(bot)
            all_bots.append(bot)  # STRESS_V3
        return bot

    t_start = time.time()
    # STRESS_V2: каждый вход в сво��м потоке — launch() блокируется на in_play.wait(30),
    # раньше очередь стартов вставала (100 ботов заходили 44 с вместо ~5 с)
    launch_threads = []
    for i in range(args.bots):
        th = threading.Thread(target=launch, args=(i,), daemon=True)
        th.start()
        launch_threads.append(th)
        time.sleep(1.0 / join_rate)  # STRESS_V4: настраиваемый темп через --rate (было жёстко 20/с)
    for th in launch_threads:
        th.join(180)  # STRESS500_V1: ждём медленные входы под нагрузкой
    join_time = time.time() - t_start
    with lock:
        online = len(bots)
    print(f"Зашло {online}/{args.bots} ботов за {join_time:.1f} с, ошибок входа: {len(errors)}", flush=True)

    # churn: часть ботов постоянно выходит и заходит — ловим утечки на входе/выходе
    # STRESS_V3: ОБЩИЙ счётчик имён! Раньше каждый чурнер стартовал со своим idx=1300,
    # плодил тёзок Stress1300 с одним оффлайн-UUID, и сервер честно убивал старую
    # сессию (защита от дубль-логина MP_DUP_V1) — отсюда «соединение закрыто сервером»
    next_idx = [args.bots + 1000]

    def churner():
        while time.time() < deadline:
            time.sleep(random.uniform(3, 8))
            with lock:
                if not bots:
                    continue
                victim = random.choice(bots)
                bots.remove(victim)
                idx = next_idx[0]
                next_idx[0] += 1
            victim.close()
            time.sleep(random.uniform(0.5, 2.0))
            if time.time() < deadline and launch(idx):
                reconnects[0] += 1

    churn_workers = max(1, int(args.bots * args.churn) // 5) if args.churn > 0 else 0
    for _ in range(churn_workers):
        threading.Thread(target=churner, daemon=True).start()

    # статистика раз в 5 секунд
    last_sent = last_recv = 0
    while time.time() < deadline:
        time.sleep(min(5, max(1, deadline - time.time())))
        with lock:
            alive = sum(1 for x in bots if x.is_alive() and not x.error)
            dead = sum(1 for x in bots if x.error)
            sent = sum(x.sent for x in all_bots)  # STRESS_V3: по всем ботам — иначе скорость уходила в минус при удалении
            recv = sum(x.recv for x in all_bots)
        # STRESS_RAM_V1: добираем текущий RSS сервера к каждой строке статистики
        if server_proc is None:
            server_proc = find_server_process()
        ram_now = read_server_ram_mb(server_proc)
        ram_str = "?"
        if ram_now is not None:
            ram_samples.append(ram_now)
            ram_peak[0] = max(ram_peak[0], ram_now)
            ram_str = f"{ram_now:.0f}МБ (пик {ram_peak[0]:.0f}МБ)"
        print(f"[{int(time.time() - t_start):4d}с] онлайн: {alive}, отвалилось: {dead}, "
              f"отправка: {(sent - last_sent) // 5}/с, приём: {(recv - last_recv) // 5}/с, "
              f"перезаходов: {reconnects[0]}, RAM: {ram_str}", flush=True)
        last_sent, last_recv = sent, recv

    with lock:
        survivors = sum(1 for x in bots if x.is_alive() and not x.error)
        dead_errors = [x.error for x in bots if x.error]
        sent = sum(x.sent for x in all_bots)  # STRESS_V3
        recv = sum(x.recv for x in all_bots)
        snapshot = list(bots)
    for bot in snapshot:
        bot.close()
    print("-" * 60, flush=True)
    print(f"Итог: доехало до конца {survivors} ботов, перезаходов: {reconnects[0]}, "
          f"пакетов отправлено {sent}, принято {recv}", flush=True)
    problems = errors + dead_errors
    if problems:
        print(f"Ошибки ({len(problems)}), первые 10:", flush=True)
        for e in problems[:10]:
            print("  " + str(e), flush=True)

    # STRESS_RAM_V1: итоговый замер RAM и проверка на утечку вместо ручной сверки с диспетчером задач
    if server_proc is None:
        server_proc = find_server_process()
    time.sleep(2)  # даём серверу время отработать отключения и почистить состояние игроков
    ram_after = read_server_ram_mb(server_proc)
    is_alive = server_proc is not None and (psutil is None or (psutil.pid_exists(server_proc.pid) if hasattr(server_proc, "pid") else True))
    if ram_before is not None and ram_after is not None:
        growth = ram_after - ram_before
        print(f"RAM сервера: до {ram_before:.0f}МБ -> пик {ram_peak[0]:.0f}МБ -> после {ram_after:.0f}МБ "
              f"(рост {growth:+.0f}МБ)", flush=True)
        if growth > max(50.0, ram_before * 0.15):
            print("RAM: похоже на утечку — память не вернулась к исходному уровню после выхода всех ботов", flush=True)
    elif ram_before is not None:
        print(f"RAM сервера: до {ram_before:.0f}МБ -> пик {ram_peak[0]:.0f}МБ -> после неизвестно (процесс исчез — сервер, похоже, упал)", flush=True)
    else:
        print("После теста: проверь, что RAM сервера вернулась к норме (утечки) и что сервер жив.", flush=True)
    print(f"Сервер после теста: {'жив' if is_alive else 'процесс не найден (проверь вручную, возможен краш)'}", flush=True)
    ok = survivors > 0 and not dead_errors
    print("*** STRESS: БЕЗ ПАДЕНИЙ БОТОВ ***" if ok else "*** STRESS: ЕСТЬ ПРОБЛЕМЫ ***", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nПрервано", flush=True)
        sys.exit(130)
