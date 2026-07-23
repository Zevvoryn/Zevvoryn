#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================
# Zevvoryn Hotfix 18a+18b  ->  MP_V1 (мультиплеер: видимость + движение)
#
# Запускать ИЗ КОРНЯ проекта TestC++ (там, где лежит папка src\):
#     python hotfix18_multiplayer.py
#
# Патчит:
#   src/entity/player.hpp   (+3 поля mpLast* для delta-move)
#   src/core/server.hpp     (+5 объявлений методов MP_V1)
#   src/core/server.cpp     (+определения MP_V1, вызовы при входе/выходе/движении)
#
# Каждая правка проверяет, что якорь встречается РОВНО один раз (count==1),
# поддерживает CRLF/LF, и после применения проверяет баланс { } и odd-quote.
# Идемпотентность: если маркер MP_V1 уже есть в файле — правки этого файла
# пропускаются, чтобы не патчить дважды.
# ============================================================
import sys, os

ERRORS = []

def load(path):
    with open(path, 'r', encoding='utf-8', newline='') as f:
        return f.read()

def save(path, text):
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)

def nl_of(text):
    return '\r\n' if '\r\n' in text else '\n'

def apply_edits(path, edits, marker='MP_V1'):
    if not os.path.exists(path):
        ERRORS.append(f'НЕ НАЙДЕН ФАЙЛ: {path}')
        return
    text = load(path)
    nl = nl_of(text)
    if marker in text:
        print(f'[skip] {path}: маркер {marker} уже присутствует — файл не трогаю')
        return
    for i, (old, new) in enumerate(edits, 1):
        o = old.replace('\n', nl)
        n = new.replace('\n', nl)
        cnt = text.count(o)
        if cnt != 1:
            ERRORS.append(f'{path}: правка #{i}: якорь найден {cnt} раз (нужно ровно 1)')
            return
        text = text.replace(o, n, 1)
    save(path, text)
    print(f'[ok]   {path}: применено правок: {len(edits)}')

def lint(path):
    if not os.path.exists(path):
        return
    text = load(path)
    op = text.count('{'); cl = text.count('}')
    status = 'OK' if op == cl else 'ДИСБАЛАНС!'
    print(f'[lint] {path}: {{={op} }}={cl} -> {status}')
    if op != cl:
        ERRORS.append(f'{path}: дисбаланс скобок {{={op} }}={cl}')
    for ln, line in enumerate(text.split('\n'), 1):
        if line.count('"') % 2 != 0:
            print(f'[lint][odd-quote] {path}:{ln}: {line.strip()[:80]}')
            ERRORS.append(f'{path}:{ln}: нечётное число кавычек')

# ============================================================
# player.hpp
# ============================================================
PLAYER_HPP = 'src/entity/player.hpp'
player_edits = [
(
"""    // GM_V1: режим игры конкретного игрока (0=survival 1=creative 2=adventure 3=spectator)
    i32 gameMode = 1;
""",
"""    // GM_V1: режим игры конкретного игрока (0=survival 1=creative 2=adventure 3=spectator)
    i32 gameMode = 1;

    // MP_V1: последняя позиция, разосланная другим игрокам (точка отсчёта для delta-move пакетов)
    f64 mpLastX = 0.0;
    f64 mpLastY = 4.0;
    f64 mpLastZ = 0.0;
"""
),
]

# ============================================================
# server.hpp
# ============================================================
SERVER_HPP = 'src/core/server.hpp'
server_hpp_edits = [
(
"""    void sendKeepAlive(std::shared_ptr<entity::Player> player);
""",
"""    void sendKeepAlive(std::shared_ptr<entity::Player> player);

    // MP_V1: мультиплеер — видимость и синхронизация игроков между собой
    void broadcastToOthers(const std::shared_ptr<entity::Player>& except, i32 packetId, const std::vector<u8>& payload);
    void spawnPlayerFor(const std::shared_ptr<entity::Player>& viewer, const std::shared_ptr<entity::Player>& target);
    void onPlayerEnterPlay(const std::shared_ptr<entity::Player>& player);
    void broadcastPlayerMovement(const std::shared_ptr<entity::Player>& player, bool posChanged, bool rotChanged);
    void broadcastPlayerRemove(const std::shared_ptr<entity::Player>& player);
"""
),
]

# ============================================================
# server.cpp
# ============================================================
SERVER_CPP = 'src/core/server.cpp'

MP_DEFS = """// ============================================================
// MP_V1: Мультиплеер — видимость и синхронизация игроков между собой
// ============================================================

// Ванильный entity-type id игрока в протоколе 1.21.1 (поле type в Spawn Entity 0x01).
// ВНИМАНИЕ: если чужой игрок отрисуется как ДРУГОЙ моб — поправь это число
// (значение protocol_id для minecraft:player из реестра entity_type 1.21.1).
static constexpr i32 MP_PLAYER_ENTITY_TYPE = 128;

void NetherCraftServer::broadcastToOthers(const std::shared_ptr<entity::Player>& except, i32 packetId, const std::vector<u8>& payload) {
    auto all = getAllPlayersCopy();
    for (auto& p : all) {
        if (p.get() == except.get()) continue;
        if (p->getState() != entity::PlayerState::Play) continue; // не слать Play-пакеты тем, кто ещё в Configuration
        if (!p->isAlive()) continue;
        p->getConnection()->sendPacket(packetId, payload);
    }
}

void NetherCraftServer::spawnPlayerFor(const std::shared_ptr<entity::Player>& viewer, const std::shared_ptr<entity::Player>& target) {
    if (!viewer || !target || viewer.get() == target.get()) return;
    if (!viewer->isAlive() || viewer->getState() != entity::PlayerState::Play) return;

    const i32 eid = static_cast<i32>(target->getEntityId());
    const Angle yaw = Angle::fromDegrees(target->get_yaw());
    const Angle pitch = Angle::fromDegrees(target->get_pitch());

    // 1) Player Info Update (0x3E) — добавить в tab list (иначе сущность-игрок не отрисуется и будет без ника)
    {
        net::Buffer info;
        info.writeVarInt(0x0D); // ADD_PLAYER | UPDATE_GAME_MODE | UPDATE_LISTED | UPDATE_LATENCY
        info.writeVarInt(1);    // count
        info.writeUUID(target->getUuid());
        info.writeString(target->getName());
        info.writeVarInt(0);    // 0 свойств (offline — без скина)
        info.writeVarInt(target->gameMode);
        info.writeVarInt(1);    // listed = true
        info.writeVarInt(0);    // latency
        viewer->getConnection()->sendPacket(0x3E, std::vector<u8>(info.writtenSpan().begin(), info.writtenSpan().end()));
    }

    // 2) Spawn Entity (0x01) — type = игрок
    {
        net::Buffer sp;
        sp.writeVarInt(eid);
        sp.writeUUID(target->getUuid());
        sp.writeVarInt(MP_PLAYER_ENTITY_TYPE);
        sp.writeF64(target->getX());
        sp.writeF64(target->getY());
        sp.writeF64(target->getZ());
        sp.writeByte(pitch.value); // Pitch (Angle)
        sp.writeByte(yaw.value);   // Yaw (Angle)
        sp.writeByte(yaw.value);   // Head Yaw (Angle)
        sp.writeVarInt(0);         // Data
        sp.writeI16(0);            // Velocity X
        sp.writeI16(0);            // Velocity Y
        sp.writeI16(0);            // Velocity Z
        viewer->getConnection()->sendPacket(0x01, std::vector<u8>(sp.writtenSpan().begin(), sp.writtenSpan().end()));
    }

    // 3) Entity Head Rotation (0x48)
    {
        net::Buffer hr;
        hr.writeVarInt(eid);
        hr.writeByte(yaw.value);
        viewer->getConnection()->sendPacket(0x48, std::vector<u8>(hr.writtenSpan().begin(), hr.writtenSpan().end()));
    }
}

void NetherCraftServer::onPlayerEnterPlay(const std::shared_ptr<entity::Player>& player) {
    if (!player) return;
    // Синхронизировать точку отсчёта delta-move с текущей позицией
    player->mpLastX = player->getX();
    player->mpLastY = player->getY();
    player->mpLastZ = player->getZ();

    auto all = getAllPlayersCopy();
    for (auto& other : all) {
        if (other.get() == player.get()) continue;
        if (other->getState() != entity::PlayerState::Play) continue;
        if (!other->isAlive()) continue;
        spawnPlayerFor(player, other); // показать уже находящегося в игре — новичку
        spawnPlayerFor(other, player); // показать новичка — тому, кто уже в игре
    }
    NC_INFO("Server", "MP_V1: {} viden ostalnym igrokam", player->getName());
}

void NetherCraftServer::broadcastPlayerMovement(const std::shared_ptr<entity::Player>& player, bool posChanged, bool rotChanged) {
    if (!player || player->getState() != entity::PlayerState::Play) return;
    const i32 eid = static_cast<i32>(player->getEntityId());
    const Angle yaw = Angle::fromDegrees(player->get_yaw());
    const Angle pitch = Angle::fromDegrees(player->get_pitch());
    const bool onGround = player->isOnGround();

    const f64 dx = player->getX() - player->mpLastX;
    const f64 dy = player->getY() - player->mpLastY;
    const f64 dz = player->getZ() - player->mpLastZ;
    const bool bigJump = std::fabs(dx) >= 7.9 || std::fabs(dy) >= 7.9 || std::fabs(dz) >= 7.9;

    if (posChanged && bigJump) {
        // Teleport Entity (0x70) — абсолютная позиция, когда delta не влезает в short
        net::Buffer b;
        b.writeVarInt(eid);
        b.writeF64(player->getX());
        b.writeF64(player->getY());
        b.writeF64(player->getZ());
        b.writeByte(yaw.value);
        b.writeByte(pitch.value);
        b.writeBool(onGround);
        broadcastToOthers(player, 0x70, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()));
        player->mpLastX = player->getX();
        player->mpLastY = player->getY();
        player->mpLastZ = player->getZ();
    } else if (posChanged && rotChanged) {
        // Update Entity Position and Rotation (0x2F)
        const i16 ddx = static_cast<i16>(dx * 4096.0);
        const i16 ddy = static_cast<i16>(dy * 4096.0);
        const i16 ddz = static_cast<i16>(dz * 4096.0);
        net::Buffer b;
        b.writeVarInt(eid);
        b.writeI16(ddx);
        b.writeI16(ddy);
        b.writeI16(ddz);
        b.writeByte(yaw.value);
        b.writeByte(pitch.value);
        b.writeBool(onGround);
        broadcastToOthers(player, 0x2F, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()));
        player->mpLastX += static_cast<f64>(ddx) / 4096.0;
        player->mpLastY += static_cast<f64>(ddy) / 4096.0;
        player->mpLastZ += static_cast<f64>(ddz) / 4096.0;
    } else if (posChanged) {
        // Update Entity Position (0x2E)
        const i16 ddx = static_cast<i16>(dx * 4096.0);
        const i16 ddy = static_cast<i16>(dy * 4096.0);
        const i16 ddz = static_cast<i16>(dz * 4096.0);
        net::Buffer b;
        b.writeVarInt(eid);
        b.writeI16(ddx);
        b.writeI16(ddy);
        b.writeI16(ddz);
        b.writeBool(onGround);
        broadcastToOthers(player, 0x2E, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()));
        player->mpLastX += static_cast<f64>(ddx) / 4096.0;
        player->mpLastY += static_cast<f64>(ddy) / 4096.0;
        player->mpLastZ += static_cast<f64>(ddz) / 4096.0;
    } else if (rotChanged) {
        // Update Entity Rotation (0x30)
        net::Buffer b;
        b.writeVarInt(eid);
        b.writeByte(yaw.value);
        b.writeByte(pitch.value);
        b.writeBool(onGround);
        broadcastToOthers(player, 0x30, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()));
    }

    if (rotChanged) {
        // Entity Head Rotation (0x48)
        net::Buffer h;
        h.writeVarInt(eid);
        h.writeByte(yaw.value);
        broadcastToOthers(player, 0x48, std::vector<u8>(h.writtenSpan().begin(), h.writtenSpan().end()));
    }
}

void NetherCraftServer::broadcastPlayerRemove(const std::shared_ptr<entity::Player>& player) {
    if (!player) return;
    const i32 eid = static_cast<i32>(player->getEntityId());
    // Remove Entities (0x42)
    {
        net::Buffer b;
        b.writeVarInt(1);
        b.writeVarInt(eid);
        broadcastToOthers(player, 0x42, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()));
    }
    // Player Info Remove (0x3D)
    {
        net::Buffer b;
        b.writeVarInt(1);
        b.writeUUID(player->getUuid());
        broadcastToOthers(player, 0x3D, std::vector<u8>(b.writtenSpan().begin(), b.writtenSpan().end()));
    }
}

"""

server_cpp_edits = [
# --- 1) определения MP_V1 перед handlePlay ---
(
"void NetherCraftServer::handlePlay(std::shared_ptr<entity::Player> player, net::Buffer& data, i32 wireId) {",
MP_DEFS + "void NetherCraftServer::handlePlay(std::shared_ptr<entity::Player> player, net::Buffer& data, i32 wireId) {"
),
# --- 2) вызов onPlayerEnterPlay в конце входа в Play ---
(
"""            sendChunksAround(player, pcx, pcz, r, 9); // CLIENT_BATCH_V1: первая пачка 3x3, дальше клиент запросит сам
        }
    } else if (wireId == 0x07) {""",
"""            sendChunksAround(player, pcx, pcz, r, 9); // CLIENT_BATCH_V1: первая пачка 3x3, дальше клиент запросит сам
        }

        onPlayerEnterPlay(player); // MP_V1: показать игрока другим и других — ему
    } else if (wireId == 0x07) {"""
),
# --- 3) движение 0x1A (позиция) ---
(
"""            player->setPosition(x, y, z);
            player->setOnGround(onGround);
            if (y < -40.0) { // FLATWORLD_V1: спасение из бездны -> телепорт на спавн
                player->setPosition(0.5, 4.0, 0.5);
                sendPlayerPositionAndLook(player);
            }
            streamChunks(player); // FLATWORLD_V1: подгрузка чанков при движении
            break;
        }
        case 0x1B: { // Player Position And Rotation""",
"""            player->setPosition(x, y, z);
            player->setOnGround(onGround);
            if (y < -40.0) { // FLATWORLD_V1: спасение из бездны -> телепорт на спавн
                player->setPosition(0.5, 4.0, 0.5);
                sendPlayerPositionAndLook(player);
            }
            broadcastPlayerMovement(player, true, false); // MP_V1: разослать движение остальным
            streamChunks(player); // FLATWORLD_V1: подгрузка чанков при движении
            break;
        }
        case 0x1B: { // Player Position And Rotation"""
),
# --- 4) движение 0x1B (позиция+поворот) ---
(
"""            player->setPosition(x, y, z);
            player->setRotation(yaw, pitch);
            player->setOnGround(onGround);
            if (y < -40.0) { // FLATWORLD_V1: спасение из бездны -> телепорт на спавн
                player->setPosition(0.5, 4.0, 0.5);
                sendPlayerPositionAndLook(player);
            }
            streamChunks(player); // FLATWORLD_V1: подгрузка чанков при движении
            break;
        }
        case 0x1C: { // Player Rotation""",
"""            player->setPosition(x, y, z);
            player->setRotation(yaw, pitch);
            player->setOnGround(onGround);
            if (y < -40.0) { // FLATWORLD_V1: спасение из бездны -> телепорт на спавн
                player->setPosition(0.5, 4.0, 0.5);
                sendPlayerPositionAndLook(player);
            }
            broadcastPlayerMovement(player, true, true); // MP_V1: разослать движение+поворот остальным
            streamChunks(player); // FLATWORLD_V1: подгрузка чанков при движении
            break;
        }
        case 0x1C: { // Player Rotation"""
),
# --- 5) движение 0x1C (только поворот) ---
(
"""        case 0x1C: { // Player Rotation
            f32 yaw = data.readF32();
            f32 pitch = data.readF32();
            bool onGround = data.readBool();
            player->setRotation(yaw, pitch);
            player->setOnGround(onGround);
            break;
        }""",
"""        case 0x1C: { // Player Rotation
            f32 yaw = data.readF32();
            f32 pitch = data.readF32();
            bool onGround = data.readBool();
            player->setRotation(yaw, pitch);
            player->setOnGround(onGround);
            broadcastPlayerMovement(player, false, true); // MP_V1: разослать поворот остальным
            break;
        }"""
),
# --- 6) удаление сущности при выходе игрока ---
(
"""        savePlayerData(player); // WORLDSAVE_V1
    }

    std::lock_guard lock(playersMutex_);
    players_.erase(conn->getId());""",
"""        savePlayerData(player); // WORLDSAVE_V1
    }

    if (player) {
        broadcastPlayerRemove(player); // MP_V1: убрать сущность игрока у остальных
    }

    std::lock_guard lock(playersMutex_);
    players_.erase(conn->getId());"""
),
]

def main():
    print('=== Zevvoryn Hotfix 18a+18b (MP_V1) ===')
    if not os.path.isdir('src'):
        print('ОШИБКА: запусти скрипт из корня проекта TestC++ (не вижу папку src/)')
        sys.exit(2)
    apply_edits(PLAYER_HPP, player_edits)
    apply_edits(SERVER_HPP, server_hpp_edits)
    apply_edits(SERVER_CPP, server_cpp_edits)
    print('--- линты ---')
    for p in (PLAYER_HPP, SERVER_HPP, SERVER_CPP):
        lint(p)
    print('--- маркеры MP_V1 ---')
    for p in (PLAYER_HPP, SERVER_HPP, SERVER_CPP):
        if os.path.exists(p):
            c = load(p).count('MP_V1')
            print(f'[mark] {p}: MP_V1 x{c}')
    if ERRORS:
        print('\n!!! ЕСТЬ ОШИБКИ:')
        for e in ERRORS:
            print('  -', e)
        sys.exit(1)
    print('\nГОТОВО: Хотфикс 18a+18b применён успешно.')

if __name__ == '__main__':
    main()
