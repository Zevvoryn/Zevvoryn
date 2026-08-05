#pragma once

// ============================================================
// PACKETS_V9: сборка и отправка clientbound-пакетов состояния Play.
//
// Цель папки packets/ — чтобы байты протокола лежали в одном месте,
// а в core/server.cpp оставалась только игровая логика: «когда отправить»,
// а не «какие байты записать».
//
// Правила для этого файла:
//   1. Каждая функция = ровно один пакет, имя совпадает с ванильным классом.
//   2. Над каждой функцией — раскладка полей по порядку и типам.
//   3. Числовые id берутся только из packet_ids.hpp.
//   4. Порядковые номера enum'ов (SoundSource и т.п.) — тоже из packet_ids.hpp.
//
// Форматы сверены с официальными маппингами server.jar 1.21.1 (protocol 767).
// ============================================================

#include "packet_ids.hpp"
#include "../core/types.hpp"
#include "../network/buffer.hpp"
#include "../entity/player.hpp"

#include <memory>
#include <string>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <utility>
#include <vector>

namespace nc::packets {

using PlayerPtr = std::shared_ptr<entity::Player>;

// ------------------------------------------------------------
// Общие примитивы записи
// ------------------------------------------------------------

// Java 1.21.1 шлёт chat-компоненты как безымянный network-NBT, а не JSON-строку.
inline void writeTextComponent(net::Buffer& out, std::string_view text) {
    out.writeByte(0x08); // TAG_String root
    out.writeU16(static_cast<u16>(text.size()));
    out.writeBytes(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

// Holder<SoundEvent> в inline-форме: varint 0 = «дальше идёт описание звука».
inline void writeInlineSoundHolder(net::Buffer& out, std::string_view soundName,
                                   bool hasFixedRange = false, f32 fixedRange = 0.0f) {
    out.writeVarInt(0);
    out.writeString(soundName);
    out.writeBool(hasFixedRange);
    if (hasFixedRange) out.writeF32(fixedRange);
}

namespace detail {
inline std::vector<u8> toBytes(const net::Buffer& buf) {
    auto span = buf.writtenSpan();
    return std::vector<u8>(span.begin(), span.end());
}
inline void dispatch(const PlayerPtr& player, i32 packetId, const net::Buffer& buf) {
    if (!player) return;
    auto conn = player->getConnection();
    if (!conn) return;
    conn->sendPacket(packetId, detail::toBytes(buf));
}
} // namespace detail

// ------------------------------------------------------------
// 0x04 Award Statistics — ClientboundAwardStatsPacket
//   VarInt count, далее count раз: VarInt category, VarInt statId, VarInt value
// Счётчиков сервер пока не ведёт, пустой список — валидный ответ.
// ------------------------------------------------------------
inline void sendAwardStatistics(const PlayerPtr& player) {
    net::Buffer buf;
    buf.writeVarInt(0);
    detail::dispatch(player, cb::AwardStatistics, buf);
}

// ------------------------------------------------------------
// 0x0F Clear Titles — ClientboundClearTitlesPacket
//   Bool resetTimes
// ------------------------------------------------------------
inline void sendClearTitles(const PlayerPtr& player, bool resetTimes) {
    net::Buffer buf;
    buf.writeBool(resetTimes);
    detail::dispatch(player, cb::ClearTitles, buf);
}

// ------------------------------------------------------------
// 0x21 Unload Chunk — ClientboundForgetLevelChunkPacket
//   Int chunkZ, Int chunkX   (именно в таком порядке, это не опечатка)
// ------------------------------------------------------------
inline void sendUnloadChunk(const PlayerPtr& player, i32 chunkX, i32 chunkZ) {
    net::Buffer buf;
    buf.writeI32(chunkZ);
    buf.writeI32(chunkX);
    detail::dispatch(player, cb::UnloadChunk, buf);
}

// ------------------------------------------------------------
// 0x25 Initialize World Border — ClientboundInitializeBorderPacket
//   Double newCenterX, Double newCenterZ, Double oldSize, Double newSize,
//   VarLong lerpTime, VarInt newAbsoluteMaxSize, VarInt warningBlocks, VarInt warningTime
// ------------------------------------------------------------
inline void sendInitializeWorldBorder(const PlayerPtr& player,
                                      f64 centerX = 0.0, f64 centerZ = 0.0,
                                      f64 oldSize = 59999968.0, f64 newSize = 59999968.0,
                                      i64 lerpTimeMs = 0, i32 absoluteMaxSize = 29999984,
                                      i32 warningBlocks = 5, i32 warningTimeSec = 15) {
    net::Buffer buf;
    buf.writeF64(centerX);
    buf.writeF64(centerZ);
    buf.writeF64(oldSize);
    buf.writeF64(newSize);
    buf.writeVarLong(lerpTimeMs);
    buf.writeVarInt(absoluteMaxSize);
    buf.writeVarInt(warningBlocks);
    buf.writeVarInt(warningTimeSec);
    detail::dispatch(player, cb::InitializeWorldBorder, buf);
}

// ------------------------------------------------------------
// 0x31 Move Vehicle — ClientboundMoveVehiclePacket
//   Double x, Double y, Double z, Float yRot, Float xRot
// ------------------------------------------------------------
inline void sendMoveVehicle(const PlayerPtr& player, f64 x, f64 y, f64 z, f32 yaw, f32 pitch) {
    net::Buffer buf;
    buf.writeF64(x);
    buf.writeF64(y);
    buf.writeF64(z);
    buf.writeF32(yaw);
    buf.writeF32(pitch);
    detail::dispatch(player, cb::MoveVehicle, buf);
}

// ------------------------------------------------------------
// 0x32 Open Book — ClientboundOpenBookPacket
//   VarInt hand (0 = main hand, 1 = off hand)
// ------------------------------------------------------------
inline void sendOpenBook(const PlayerPtr& player, i32 hand) {
    net::Buffer buf;
    buf.writeVarInt(hand == 1 ? 1 : 0);
    detail::dispatch(player, cb::OpenBook, buf);
}

// ------------------------------------------------------------
// 0x34 Open Sign Editor — ClientboundOpenSignEditorPacket
//   Position pos, Bool isFrontText
// ------------------------------------------------------------
inline void sendOpenSignEditor(const PlayerPtr& player, const BlockPos& pos, bool frontText) {
    net::Buffer buf;
    buf.writePosition(pos);
    buf.writeBool(frontText);
    detail::dispatch(player, cb::OpenSignEditor, buf);
}

// ------------------------------------------------------------
// 0x37 Place Ghost Recipe — ClientboundPlaceGhostRecipePacket
//   Byte containerId, Identifier recipe
// ------------------------------------------------------------
inline void sendPlaceGhostRecipe(const PlayerPtr& player, i32 containerId, std::string_view recipeId) {
    net::Buffer buf;
    buf.writeByte(static_cast<u8>(containerId & 0xFF));
    buf.writeString(recipeId);
    detail::dispatch(player, cb::PlaceGhostRecipe, buf);
}

// ------------------------------------------------------------
// 0x3A End Combat — ClientboundPlayerCombatEndPacket
//   VarInt duration (в тиках)
// ------------------------------------------------------------
inline void sendEndCombat(const PlayerPtr& player, i32 durationTicks) {
    net::Buffer buf;
    buf.writeVarInt(durationTicks);
    detail::dispatch(player, cb::EndCombat, buf);
}

// ------------------------------------------------------------
// 0x3B Enter Combat — ClientboundPlayerCombatEnterPacket
//   пустое тело
// ------------------------------------------------------------
inline void sendEnterCombat(const PlayerPtr& player) {
    net::Buffer buf;
    detail::dispatch(player, cb::EnterCombat, buf);
}

// ------------------------------------------------------------
// 0x4B Server Data — ClientboundServerDataPacket
//   TextComponent motd, Bool hasIcon, [Prefixed byte array iconBytes]
// ------------------------------------------------------------
inline void sendServerData(const PlayerPtr& player, std::string_view motd) {
    net::Buffer buf;
    writeTextComponent(buf, motd);
    buf.writeBool(false); // иконка в этом пакете не передаётся
    detail::dispatch(player, cb::ServerData, buf);
}

// ------------------------------------------------------------
// 0x52 Set Camera — ClientboundSetCameraPacket
//   VarInt cameraId
// ------------------------------------------------------------
inline void sendSetCamera(const PlayerPtr& player, i32 cameraEntityId) {
    net::Buffer buf;
    buf.writeVarInt(cameraEntityId);
    detail::dispatch(player, cb::SetCamera, buf);
}

// ------------------------------------------------------------
// 0x62 Set Simulation Distance — ClientboundSetSimulationDistancePacket
//   VarInt simulationDistance
// ------------------------------------------------------------
inline void sendSetSimulationDistance(const PlayerPtr& player, i32 distance) {
    net::Buffer buf;
    buf.writeVarInt(distance);
    detail::dispatch(player, cb::SetSimulationDistance, buf);
}

// ------------------------------------------------------------
// 0x67 Entity Sound Effect — ClientboundSoundEntityPacket
//   Holder<SoundEvent> sound, VarInt soundSource, VarInt entityId,
//   Float volume, Float pitch, Long seed
// ------------------------------------------------------------
inline void sendEntitySoundEffect(const PlayerPtr& player, std::string_view soundName,
                                  SoundCategory category, i32 entityId,
                                  f32 volume, f32 pitch, i64 seed) {
    net::Buffer buf;
    writeInlineSoundHolder(buf, soundName);
    buf.writeVarInt(static_cast<i32>(category));
    buf.writeVarInt(entityId);
    buf.writeF32(volume);
    buf.writeF32(pitch);
    buf.writeI64(seed);
    detail::dispatch(player, cb::EntitySoundEffect, buf);
}

// ------------------------------------------------------------
// 0x74 Update Advancements — ClientboundUpdateAdvancementsPacket
//   Bool reset, VarInt addedCount, VarInt removedCount, VarInt progressCount
// Сервер не ведёт достижений, поэтому при входе просто чистим дерево.
// ------------------------------------------------------------
inline void sendUpdateAdvancements(const PlayerPtr& player, bool reset = true) {
    net::Buffer buf;
    buf.writeBool(reset);
    buf.writeVarInt(0); // added
    buf.writeVarInt(0); // removed
    buf.writeVarInt(0); // progress
    detail::dispatch(player, cb::UpdateAdvancements, buf);
}

// ------------------------------------------------------------
// 0x78 Update Tags — ClientboundUpdateTagsPacket
//   VarInt registryCount, далее по реестрам
// Теги уже отправлены в состоянии Configuration, тут переопределений нет.
// ------------------------------------------------------------
inline void sendUpdateTags(const PlayerPtr& player) {
    net::Buffer buf;
    buf.writeVarInt(0);
    detail::dispatch(player, cb::UpdateTags, buf);
}

// ============================================================
// PACKETS_V10: миграция уже готовых пакетов из core/server.cpp.
// Байтовые лайауты сверены с протоколом 1.21.1 (protocol 767).
// ============================================================

// 0x05 Acknowledge Block Change — ClientboundBlockChangedAckPacket
//   VarInt sequence
inline void sendAckBlockChange(const PlayerPtr& player, i32 sequence) {
    net::Buffer buf;
    buf.writeVarInt(sequence);
    detail::dispatch(player, cb::AckBlockChange, buf);
}

// 0x09 Block Update — ClientboundBlockUpdatePacket
//   Position pos, VarInt blockStateId
inline std::vector<u8> buildBlockUpdate(const BlockPos& pos, i32 blockStateId) {
    net::Buffer buf;
    buf.writePosition(pos);
    buf.writeVarInt(blockStateId);
    return detail::toBytes(buf);
}
inline void sendBlockUpdate(const PlayerPtr& player, const BlockPos& pos, i32 blockStateId) {
    net::Buffer buf;
    buf.writePosition(pos);
    buf.writeVarInt(blockStateId);
    detail::dispatch(player, cb::BlockUpdate, buf);
}

// 0x03 Entity Animation — ClientboundAnimatePacket
//   VarInt entityId, UByte animation (0 swing main hand, 1 hurt, 2 wake up, 3 swing offhand, 5 crit)
inline void sendEntityAnimation(const PlayerPtr& player, i32 entityId, u8 animation) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeByte(animation);
    detail::dispatch(player, cb::EntityAnimation, buf);
}

// 0x17 Set Cooldown — ClientboundCooldownPacket
//   VarInt itemId, VarInt cooldownTicks
inline void sendSetCooldown(const PlayerPtr& player, i32 itemId, i32 ticks) {
    net::Buffer buf;
    buf.writeVarInt(itemId);
    buf.writeVarInt(ticks);
    detail::dispatch(player, cb::SetCooldown, buf);
}

// 0x1F Entity Event — ClientboundEntityEventPacket
//   Int entityId (не VarInt!), Byte eventStatus
inline void sendEntityEvent(const PlayerPtr& player, i32 entityId, u8 status) {
    net::Buffer buf;
    buf.writeI32(entityId);
    buf.writeByte(status);
    detail::dispatch(player, cb::EntityEvent, buf);
}

// 0x22 Game Event — ClientboundGameEventPacket
//   UByte event, Float value
//   3 = change gamemode, 11 = immediate respawn flag, 13 = start waiting for chunks
inline void sendGameEvent(const PlayerPtr& player, u8 event, f32 value = 0.0f) {
    net::Buffer buf;
    buf.writeByte(event);
    buf.writeF32(value);
    detail::dispatch(player, cb::GameEvent, buf);
}

// 0x24 Hurt Animation — ClientboundHurtAnimationPacket
//   VarInt entityId, Float yaw (направление удара в градусах)
inline void sendHurtAnimation(const PlayerPtr& player, i32 entityId, f32 yaw) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeF32(yaw);
    detail::dispatch(player, cb::HurtAnimation, buf);
}

// 0x42 Remove Entities — ClientboundRemoveEntitiesPacket
//   VarInt count, VarInt[] entityIds
inline void sendRemoveEntities(const PlayerPtr& player, const std::vector<i32>& ids) {
    if (ids.empty()) return;
    net::Buffer buf;
    buf.writeVarInt(static_cast<i32>(ids.size()));
    for (i32 id : ids) buf.writeVarInt(id);
    detail::dispatch(player, cb::RemoveEntities, buf);
}
inline void sendRemoveEntity(const PlayerPtr& player, i32 entityId) {
    sendRemoveEntities(player, std::vector<i32>{ entityId });
}

// 0x54 Set Center Chunk — ClientboundSetChunkCacheCenterPacket
//   VarInt chunkX, VarInt chunkZ
inline void sendSetCenterChunk(const PlayerPtr& player, i32 chunkX, i32 chunkZ) {
    net::Buffer buf;
    buf.writeVarInt(chunkX);
    buf.writeVarInt(chunkZ);
    detail::dispatch(player, cb::SetCenterChunk, buf);
}

// 0x5A Set Entity Velocity — ClientboundSetEntityMotionPacket
//   VarInt entityId, Short vx, Short vy, Short vz (блоки/тик * 8000)
inline void sendSetEntityVelocity(const PlayerPtr& player, i32 entityId, f64 vx, f64 vy, f64 vz) {
    auto clampVel = [](f64 v) -> u16 {
        f64 s = v * 8000.0;
        if (s > 32767.0) s = 32767.0;
        if (s < -32768.0) s = -32768.0;
        return static_cast<u16>(static_cast<i16>(s));
    };
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeU16(clampVel(vx));
    buf.writeU16(clampVel(vy));
    buf.writeU16(clampVel(vz));
    detail::dispatch(player, cb::SetEntityMotion, buf);
}

// 0x5D Set Health — ClientboundSetHealthPacket
//   Float health, VarInt food, Float saturation
inline void sendSetHealth(const PlayerPtr& player, f32 health, i32 food, f32 saturation) {
    net::Buffer buf;
    buf.writeF32(health);
    buf.writeVarInt(food);
    buf.writeF32(saturation);
    detail::dispatch(player, cb::SetHealth, buf);
}

// 0x64 Update Time — ClientboundSetTimePacket
//   Long worldAge, Long timeOfDay (отрицательное = цикл суток остановлен)
inline void sendUpdateTime(const PlayerPtr& player, i64 worldAge, i64 timeOfDay) {
    net::Buffer buf;
    buf.writeI64(worldAge);
    buf.writeI64(timeOfDay);
    detail::dispatch(player, cb::SetTime, buf);
}


// ------------------------------------------------------------
// PACKETS_V15: контейнеры, удаление сущностей, экран смерти
// ------------------------------------------------------------

// 0x15 Set Container Slot - ClientboundContainerSetSlotPacket
//   Byte containerId, VarInt stateId, Short slot, Slot item
//   Slot: VarInt count; если count > 0 - VarInt itemId, VarInt addedComponents, VarInt removedComponents
inline void sendContainerSlot(const PlayerPtr& player, i32 containerId, i32 stateId,
                              i16 slot, i32 itemId, i32 count) {
    net::Buffer buf;
    buf.writeByte(static_cast<u8>(containerId));
    buf.writeVarInt(stateId);
    buf.writeI16(slot);
    buf.writeVarInt(count);
    if (count > 0) { buf.writeVarInt(itemId); buf.writeVarInt(0); buf.writeVarInt(0); }
    detail::dispatch(player, cb::ContainerSetSlot, buf);
}

// 0x42 Remove Entities - ClientboundRemoveEntitiesPacket
//   VarInt count, далее count раз VarInt entityId
// Байты собираются один раз и рассылаются всем зрителям.
inline std::vector<u8> buildRemoveEntities(const std::vector<i32>& ids) {
    net::Buffer buf;
    buf.writeVarInt(static_cast<i32>(ids.size()));
    for (i32 id : ids) buf.writeVarInt(id);
    return detail::toBytes(buf);
}

// 0x3C Death Combat Event - ClientboundPlayerCombatKillPacket
//   VarInt playerId, TextComponent deathMessage
inline void sendDeathCombatEvent(const PlayerPtr& player, i32 entityId, std::string_view message) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    writeTextComponent(buf, message);
    detail::dispatch(player, cb::DeathCombat, buf);
}


// ------------------------------------------------------------
// PACKETS_V16: boss bar, окна контейнеров, время, level event, телепорт сущностей
// ------------------------------------------------------------

// 0x0A Boss Event - ClientboundBossEventPacket
//   UUID id, VarInt operation, далее поля зависят от операции:
//   0 ADD: Component name, Float progress, VarInt color, VarInt overlay, UByte flags
//   1 REMOVE: больше ничего
//   2 UPDATE_PROGRESS: Float progress
//   3 UPDATE_NAME: Component name
//   4 UPDATE_STYLE: VarInt color, VarInt overlay
inline void sendBossBarAdd(const PlayerPtr& player, const UUID& id, std::string_view title,
                           f32 progress, i32 color, i32 overlay, u8 flags) {
    net::Buffer buf;
    buf.writeUUID(id);
    buf.writeVarInt(0);
    writeTextComponent(buf, title);
    buf.writeF32(progress);
    buf.writeVarInt(color);
    buf.writeVarInt(overlay);
    buf.writeByte(flags);
    detail::dispatch(player, cb::BossEvent, buf);
}
inline void sendBossBarRemove(const PlayerPtr& player, const UUID& id) {
    net::Buffer buf;
    buf.writeUUID(id);
    buf.writeVarInt(1);
    detail::dispatch(player, cb::BossEvent, buf);
}
inline void sendBossBarProgress(const PlayerPtr& player, const UUID& id, f32 progress) {
    net::Buffer buf;
    buf.writeUUID(id);
    buf.writeVarInt(2);
    buf.writeF32(progress);
    detail::dispatch(player, cb::BossEvent, buf);
}
inline void sendBossBarName(const PlayerPtr& player, const UUID& id, std::string_view title) {
    net::Buffer buf;
    buf.writeUUID(id);
    buf.writeVarInt(3);
    writeTextComponent(buf, title);
    detail::dispatch(player, cb::BossEvent, buf);
}
inline void sendBossBarStyle(const PlayerPtr& player, const UUID& id, i32 color, i32 overlay) {
    net::Buffer buf;
    buf.writeUUID(id);
    buf.writeVarInt(4);
    buf.writeVarInt(color);
    buf.writeVarInt(overlay);
    detail::dispatch(player, cb::BossEvent, buf);
}

// 0x33 Open Screen - ClientboundOpenScreenPacket
//   VarInt containerId, VarInt menuType, Component title
inline void sendOpenScreen(const PlayerPtr& player, i32 containerId, i32 menuType,
                           std::string_view title) {
    net::Buffer buf;
    buf.writeVarInt(containerId);
    buf.writeVarInt(menuType);
    writeTextComponent(buf, title);
    detail::dispatch(player, cb::OpenScreen, buf);
}

// 0x53 Set Held Slot - ClientboundSetCarriedItemPacket
//   Byte slot (0..8)
inline void sendSetHeldSlot(const PlayerPtr& player, i32 slot) {
    net::Buffer buf;
    buf.writeByte(static_cast<u8>(slot));
    detail::dispatch(player, cb::SetHeldSlot, buf);
}

// 0x64 Update Time - вариант для рассылки (байты собираются один раз)
inline std::vector<u8> buildUpdateTime(i64 worldAge, i64 timeOfDay) {
    net::Buffer buf;
    buf.writeI64(worldAge);
    buf.writeI64(timeOfDay);
    return detail::toBytes(buf);
}

// 0x28 Level Event - ClientboundLevelEventPacket
//   Int event, Position pos, Int data, Bool globalOverride
//   2001 = ломание блока, 1009 = тушение огня, 2005 = костная мука
inline std::vector<u8> buildLevelEvent(i32 event, const BlockPos& pos, i32 data,
                                       bool globalOverride) {
    net::Buffer buf;
    buf.writeI32(event);
    buf.writePosition(pos);
    buf.writeI32(data);
    buf.writeBool(globalOverride);
    return detail::toBytes(buf);
}

// 0x70 Teleport Entity - ClientboundTeleportEntityPacket
//   VarInt entityId, Double x/y/z, Angle yaw, Angle pitch, Bool onGround
inline std::vector<u8> buildTeleportEntity(i32 entityId, f64 x, f64 y, f64 z,
                                           u8 yaw, u8 pitch, bool onGround) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeF64(x);
    buf.writeF64(y);
    buf.writeF64(z);
    buf.writeByte(yaw);
    buf.writeByte(pitch);
    buf.writeBool(onGround);
    return detail::toBytes(buf);
}

// ============================================================
// PACKETS_V18 — одиночные пакеты (batch 3)
// ============================================================

// 0x55 Set Chunk Cache Radius — VarInt viewDistance
inline void sendSetRenderDistance(const PlayerPtr& player, i32 viewDistance) {
    net::Buffer buf;
    buf.writeVarInt(viewDistance);
    detail::dispatch(player, cb::SetRenderDistance, buf);
}

// 0x5C Set Experience — Float progress, VarInt level, VarInt total
inline void sendSetExperience(const PlayerPtr& player, f32 progress, i32 level, i32 total) {
    net::Buffer buf;
    buf.writeF32(progress);
    buf.writeVarInt(level);
    buf.writeVarInt(total);
    detail::dispatch(player, cb::SetExperience, buf);
}

// 0x56 Set Default Spawn Position — Position pos, Float angle (именно f32, не u8)
inline void sendSetDefaultSpawn(const PlayerPtr& player, const BlockPos& pos, f32 angle) {
    net::Buffer buf;
    buf.writePosition(pos);
    buf.writeF32(angle);
    detail::dispatch(player, cb::SetDefaultSpawn, buf);
}

// 0x40 Player Position — Double x/y/z, Float yaw/pitch, Byte flags, VarInt teleportId
inline void sendPlayerPosition(const PlayerPtr& player, f64 x, f64 y, f64 z,
                               f32 yaw, f32 pitch, u8 flags, i32 teleportId) {
    net::Buffer buf;
    buf.writeF64(x);
    buf.writeF64(y);
    buf.writeF64(z);
    buf.writeF32(yaw);
    buf.writeF32(pitch);
    buf.writeByte(flags);
    buf.writeVarInt(teleportId);
    detail::dispatch(player, cb::PlayerPosition, buf);
}

// 0x26 Keep Alive — Long id
inline void sendKeepAlive(const PlayerPtr& player, i64 id) {
    net::Buffer buf;
    buf.writeI64(id);
    detail::dispatch(player, cb::KeepAlive, buf);
}

// 0x48 Rotate Head — VarInt entityId, Angle yaw
inline void sendRotateHead(const PlayerPtr& player, i32 entityId, u8 yaw) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeByte(yaw);
    detail::dispatch(player, cb::RotateHead, buf);
}

// 0x36 Pong Response (play) — Long id
inline void sendPongResponse(const PlayerPtr& player, i64 id) {
    net::Buffer buf;
    buf.writeI64(id);
    detail::dispatch(player, cb::PongResponse, buf);
}

// 0x12 Container Close — UByte containerId
inline void sendContainerClose(const PlayerPtr& player, u8 containerId) {
    net::Buffer buf;
    buf.writeByte(containerId);
    detail::dispatch(player, cb::ContainerClose, buf);
}

// 0x76 Update Mob Effect — VarInt entityId, VarInt effectId, Byte amplifier,
//   VarInt duration, Byte flags (0x01 ambient, 0x02 частицы, 0x04 иконка, 0x08 blend)
inline void sendUpdateMobEffect(const PlayerPtr& player, i32 entityId, i32 effectId,
                                u8 amplifier, i32 durationTicks, u8 flags) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeVarInt(effectId);
    buf.writeByte(amplifier);
    buf.writeVarInt(durationTicks);
    buf.writeByte(flags);
    detail::dispatch(player, cb::UpdateMobEffect, buf);
}

// 0x43 Remove Mob Effect — VarInt entityId, VarInt effectId
inline void sendRemoveMobEffect(const PlayerPtr& player, i32 entityId, i32 effectId) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeVarInt(effectId);
    detail::dispatch(player, cb::RemoveMobEffect, buf);
}

// 0x5F Set Passengers — VarInt vehicleId, VarInt count, VarInt[] passengers
inline std::vector<u8> buildSetPassengers(i32 vehicleId, i32 passengerId) {
    net::Buffer buf;
    buf.writeVarInt(vehicleId);
    if (passengerId != 0) { buf.writeVarInt(1); buf.writeVarInt(passengerId); }
    else buf.writeVarInt(0);
    return detail::toBytes(buf);
}

// 0x6F Take Item Entity — VarInt collectedId, VarInt collectorId, VarInt count
inline std::vector<u8> buildTakeItemEntity(i32 collectedId, i32 collectorId, i32 count) {
    net::Buffer buf;
    buf.writeVarInt(collectedId);
    buf.writeVarInt(collectorId);
    buf.writeVarInt(count);
    return detail::toBytes(buf);
}

// 0x1A Damage Event — VarInt entityId, VarInt damageType, VarInt sourceCause+1,
//   VarInt directSource+1, Bool hasSourcePosition
inline std::vector<u8> buildDamageEvent(i32 entityId, i32 damageTypeId) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeVarInt(damageTypeId);
    buf.writeVarInt(0); // source cause absent
    buf.writeVarInt(0); // direct source absent
    buf.writeBool(false);
    return detail::toBytes(buf);
}

// 0x75 Update Attributes — одно свойство generic.armor (id 0) без модификаторов
// PACKETS_V21: частный случай buildUpdateAttributes (определён ниже в этом же файле).
inline std::vector<u8> buildUpdateArmorAttribute(i32 entityId, f64 armor) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeVarInt(1);
    buf.writeVarInt(0); // attr::Armor
    buf.writeF64(armor);
    buf.writeVarInt(0); // модификаторов нет
    return detail::toBytes(buf);
}

// 0x6D Set Tab List Header And Footer — два NBT-тега TAG_String подряд (без имён)
inline std::vector<u8> buildTabListHeaderFooter(const std::string& header, const std::string& footer) {
    net::Buffer buf;
    const auto nbtText = [&buf](const std::string& text) {
        buf.writeByte(0x08); // TAG_String
        buf.writeU16(static_cast<u16>(text.size()));
        buf.writeBytes(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
    };
    nbtText(header);
    nbtText(footer);
    return detail::toBytes(buf);
}

// ============================================================
// PACKETS_V11: пачка clientbound-пакетов Play (протокол 767).
// Скорборд, команды, торговля, взрывы, тикинг и служебные пакеты.
// ============================================================

// 0x11 Commands — ClientboundCommandsPacket
//   VarInt nodeCount, далее узлы графа, VarInt rootIndex
//   Узел: Byte flags, VarInt childCount, VarInt[] children, [String name]
//   flags: 0x00 root, 0x01 literal, 0x04 executable
inline void sendCommands(const PlayerPtr& player, const std::vector<std::string>& commands) {
    net::Buffer buf;
    const i32 count = static_cast<i32>(commands.size());
    buf.writeVarInt(count + 1); // root + по узлу на команду
    buf.writeByte(0x00);        // root
    buf.writeVarInt(count);
    for (i32 i = 0; i < count; ++i) buf.writeVarInt(i + 1);
    for (const auto& name : commands) {
        buf.writeByte(0x01 | 0x04); // literal + executable
        buf.writeVarInt(0);         // без дочерних аргументов
        buf.writeString(name);
    }
    buf.writeVarInt(0); // rootIndex
    detail::dispatch(player, cb::Commands, buf);
}

// 0x13 Set Container Content — ClientboundContainerSetContentPacket
//   Byte containerId, VarInt stateId, VarInt count, Slot[] items, Slot carried
//   Пустой слот кодируется одним VarInt 0 (itemCount = 0).
inline void sendContainerSetContent(const PlayerPtr& player, u8 containerId, i32 stateId, i32 slotCount) {
    net::Buffer buf;
    buf.writeByte(containerId);
    buf.writeVarInt(stateId);
    buf.writeVarInt(slotCount);
    for (i32 i = 0; i < slotCount; ++i) buf.writeVarInt(0);
    buf.writeVarInt(0); // предмет на курсоре
    detail::dispatch(player, cb::ContainerSetContent, buf);
}

// 0x2D Merchant Offers — ClientboundMerchantOffersPacket
//   VarInt containerId, VarInt size, Trade[], VarInt level, VarInt experience,
//   Bool isRegularVillager, Bool canRestock
inline void sendMerchantOffers(const PlayerPtr& player, i32 containerId, i32 level,
                               i32 experience, bool regularVillager, bool canRestock) {
    net::Buffer buf;
    buf.writeVarInt(containerId);
    buf.writeVarInt(0); // офферов пока нет
    buf.writeVarInt(level);
    buf.writeVarInt(experience);
    buf.writeBool(regularVillager);
    buf.writeBool(canRestock);
    detail::dispatch(player, cb::MerchantOffers, buf);
}

// 0x47 Respawn — ClientboundRespawnPacket
//   VarInt dimensionType, Identifier dimensionName, Long hashedSeed, UByte gamemode,
//   Byte previousGamemode, Bool isDebug, Bool isFlat, Bool hasDeathLocation,
//   VarInt portalCooldown, Byte dataKept (0x01 attributes, 0x02 metadata)
inline void sendRespawn(const PlayerPtr& player, i32 dimensionTypeId, std::string_view dimensionName,
                        i64 hashedSeed, u8 gamemode, i8 previousGamemode,
                        i32 portalCooldown = 0, u8 dataKept = 0x00) {
    net::Buffer buf;
    buf.writeVarInt(dimensionTypeId);
    buf.writeString(dimensionName);
    buf.writeI64(hashedSeed);
    buf.writeByte(gamemode);
    buf.writeByte(static_cast<u8>(previousGamemode));
    buf.writeBool(false); // isDebug
    buf.writeBool(false); // isFlat
    buf.writeBool(false); // hasDeathLocation
    buf.writeVarInt(portalCooldown);
    buf.writeByte(dataKept);
    detail::dispatch(player, cb::Respawn, buf);
}

// 0x6E Tag Query Response — ClientboundTagQueryPacket
//   VarInt transactionId, NBT (0x00 = TAG_End, то есть пусто)
inline void sendTagQueryResponse(const PlayerPtr& player, i32 transactionId) {
    net::Buffer buf;
    buf.writeVarInt(transactionId);
    buf.writeByte(0x00);
    detail::dispatch(player, cb::TagQuery, buf);
}

// 0x20 Explosion — ClientboundExplodePacket
//   Double x/y/z, Float strength, VarInt recordCount, Byte[3]*count,
//   Float motionX/Y/Z, VarInt blockInteraction, VarInt smallParticleId,
//   VarInt largeParticleId, Holder<SoundEvent>
//   blockInteraction: 0 keep, 1 destroy, 2 destroy_with_decay, 3 trigger_block
inline void sendExplosion(const PlayerPtr& player, f64 x, f64 y, f64 z, f32 strength,
                          f32 motionX = 0.0f, f32 motionY = 0.0f, f32 motionZ = 0.0f,
                          i32 blockInteraction = 1,
                          i32 smallParticleId = 0, i32 largeParticleId = 0,
                          std::string_view sound = "minecraft:entity.generic.explode") {
    net::Buffer buf;
    buf.writeF64(x);
    buf.writeF64(y);
    buf.writeF64(z);
    buf.writeF32(strength);
    buf.writeVarInt(0); // блоки клиент ломает сам по BlockUpdate
    buf.writeF32(motionX);
    buf.writeF32(motionY);
    buf.writeF32(motionZ);
    buf.writeVarInt(blockInteraction);
    buf.writeVarInt(smallParticleId); // частицы без доп. данных
    buf.writeVarInt(largeParticleId);
    writeInlineSoundHolder(buf, sound);
    detail::dispatch(player, cb::Explosion, buf);
}

// 0x2C Map Data — ClientboundMapItemDataPacket
//   VarInt mapId, Byte scale, Bool locked, Bool hasIcons, [Icon[]], Byte columns
//   columns = 0 означает «пикселей в этом пакете нет».
inline void sendMapData(const PlayerPtr& player, i32 mapId, i8 scale, bool locked) {
    net::Buffer buf;
    buf.writeVarInt(mapId);
    buf.writeByte(static_cast<u8>(scale));
    buf.writeBool(locked);
    buf.writeBool(false); // hasIcons
    buf.writeByte(0);     // columns
    detail::dispatch(player, cb::MapData, buf);
}

// 0x35 Ping — ClientboundPingPacket
//   Int id (обычный Int, не VarInt — это не PingRequest из Status)
inline void sendPing(const PlayerPtr& player, i32 id) {
    net::Buffer buf;
    buf.writeI32(id);
    detail::dispatch(player, cb::Ping, buf);
}

// 0x41 Unlock Recipes — ClientboundRecipePacket
//   VarInt action (0 init, 1 add, 2 remove), 8 Bool-флагов книги рецептов,
//   VarInt count + Identifier[], а при action = 0 ещё один такой же список
inline void sendUnlockRecipes(const PlayerPtr& player, i32 action = 0) {
    net::Buffer buf;
    buf.writeVarInt(action);
    for (int i = 0; i < 8; ++i) buf.writeBool(false); // crafting/smelting/blast/smoker: open + filter
    buf.writeVarInt(0);
    if (action == 0) buf.writeVarInt(0);
    detail::dispatch(player, cb::UnlockRecipes, buf);
}

// 0x77 Update Recipes — ClientboundUpdateRecipesPacket
//   VarInt count, далее рецепты. Пустой список валиден: крафт идёт по ванильным правилам клиента.
inline void sendUpdateRecipes(const PlayerPtr& player) {
    net::Buffer buf;
    buf.writeVarInt(0);
    detail::dispatch(player, cb::UpdateRecipes, buf);
}

// ------------------------------------------------------------
// Скорборд
// ------------------------------------------------------------

// 0x57 Display Objective — ClientboundSetDisplayObjectivePacket
//   VarInt position (0 list, 1 sidebar, 2 below name, 3..18 sidebar по цвету команды),
//   String objectiveName
inline void sendDisplayObjective(const PlayerPtr& player, i32 position, std::string_view objectiveName) {
    net::Buffer buf;
    buf.writeVarInt(position);
    buf.writeString(objectiveName);
    detail::dispatch(player, cb::DisplayObjective, buf);
}

// 0x5E Set Objective — ClientboundSetObjectivePacket
//   String name, Byte mode (0 create, 1 remove, 2 update)
//   при mode 0/2: TextComponent displayName, VarInt type (0 integer, 1 hearts), Bool hasNumberFormat
inline void sendCreateObjective(const PlayerPtr& player, std::string_view name,
                                std::string_view displayName, i32 renderType = 0, bool update = false) {
    net::Buffer buf;
    buf.writeString(name);
    buf.writeByte(update ? 2 : 0);
    writeTextComponent(buf, displayName);
    buf.writeVarInt(renderType);
    buf.writeBool(false); // hasNumberFormat
    detail::dispatch(player, cb::SetObjective, buf);
}
inline void sendRemoveObjective(const PlayerPtr& player, std::string_view name) {
    net::Buffer buf;
    buf.writeString(name);
    buf.writeByte(1);
    detail::dispatch(player, cb::SetObjective, buf);
}

// 0x61 Set Score — ClientboundSetScorePacket
//   String entityName, String objectiveName, VarInt value,
//   Bool hasDisplayName, [TextComponent], Bool hasNumberFormat
inline void sendSetScore(const PlayerPtr& player, std::string_view entityName,
                         std::string_view objectiveName, i32 value) {
    net::Buffer buf;
    buf.writeString(entityName);
    buf.writeString(objectiveName);
    buf.writeVarInt(value);
    buf.writeBool(false); // hasDisplayName
    buf.writeBool(false); // hasNumberFormat
    detail::dispatch(player, cb::SetScore, buf);
}

// 0x44 Reset Score — ClientboundResetScorePacket
//   String entityName, Bool hasObjective, [String objectiveName]
inline void sendResetScore(const PlayerPtr& player, std::string_view entityName,
                           std::string_view objectiveName = {}) {
    net::Buffer buf;
    buf.writeString(entityName);
    const bool hasObjective = !objectiveName.empty();
    buf.writeBool(hasObjective);
    if (hasObjective) buf.writeString(objectiveName);
    detail::dispatch(player, cb::ResetScore, buf);
}

// 0x60 Set Player Team — ClientboundSetPlayerTeamPacket
//   String teamName, Byte mode (0 create, 1 remove, 2 update info, 3 add players, 4 remove players)
//   mode 0/2: TextComponent displayName, Byte friendlyFlags, String nameTagVisibility,
//             String collisionRule, VarInt color, TextComponent prefix, TextComponent suffix
//   mode 0/3/4: VarInt entityCount, String[] entities
inline void sendCreateTeam(const PlayerPtr& player, std::string_view teamName,
                           std::string_view displayName, i32 color,
                           const std::vector<std::string>& members,
                           std::string_view prefix = {}, std::string_view suffix = {}) {
    net::Buffer buf;
    buf.writeString(teamName);
    buf.writeByte(0);
    writeTextComponent(buf, displayName);
    buf.writeByte(0x03); // allow friendly fire + see invisible teammates
    buf.writeString("always");
    buf.writeString("always");
    buf.writeVarInt(color);
    writeTextComponent(buf, prefix);
    writeTextComponent(buf, suffix);
    buf.writeVarInt(static_cast<i32>(members.size()));
    for (const auto& member : members) buf.writeString(member);
    detail::dispatch(player, cb::SetPlayerTeam, buf);
}
inline void sendRemoveTeam(const PlayerPtr& player, std::string_view teamName) {
    net::Buffer buf;
    buf.writeString(teamName);
    buf.writeByte(1);
    detail::dispatch(player, cb::SetPlayerTeam, buf);
}
inline void sendTeamMembers(const PlayerPtr& player, std::string_view teamName,
                            const std::vector<std::string>& members, bool add) {
    net::Buffer buf;
    buf.writeString(teamName);
    buf.writeByte(add ? 3 : 4);
    buf.writeVarInt(static_cast<i32>(members.size()));
    for (const auto& member : members) buf.writeString(member);
    detail::dispatch(player, cb::SetPlayerTeam, buf);
}

// ------------------------------------------------------------
// Тикинг и служебные пакеты
// ------------------------------------------------------------

// 0x71 Ticking State — ClientboundTickingStatePacket
//   Float tickRate, Bool isFrozen
inline void sendTickingState(const PlayerPtr& player, f32 tickRate, bool frozen) {
    net::Buffer buf;
    buf.writeF32(tickRate);
    buf.writeBool(frozen);
    detail::dispatch(player, cb::TickingState, buf);
}

// 0x72 Ticking Step — ClientboundTickingStepPacket
//   VarInt tickSteps
inline void sendTickingStep(const PlayerPtr& player, i32 tickSteps) {
    net::Buffer buf;
    buf.writeVarInt(tickSteps);
    detail::dispatch(player, cb::TickingStep, buf);
}

// 0x73 Transfer — ClientboundTransferPacket
//   String host, VarInt port
inline void sendTransfer(const PlayerPtr& player, std::string_view host, i32 port) {
    net::Buffer buf;
    buf.writeString(host);
    buf.writeVarInt(port);
    detail::dispatch(player, cb::Transfer, buf);
}

// 0x79 Projectile Power — ClientboundProjectilePowerPacket
//   VarInt entityId, Double accelerationPower
inline void sendProjectilePower(const PlayerPtr& player, i32 entityId, f64 accelerationPower) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeF64(accelerationPower);
    detail::dispatch(player, cb::ProjectilePower, buf);
}

// 0x7A Custom Report Details — ClientboundCustomReportDetailsPacket
//   VarInt count, далее пары String title / String description (видно в краш-репорте клиента)
inline void sendCustomReportDetails(const PlayerPtr& player,
                                    const std::vector<std::pair<std::string, std::string>>& details) {
    net::Buffer buf;
    buf.writeVarInt(static_cast<i32>(details.size()));
    for (const auto& entry : details) {
        buf.writeString(entry.first);
        buf.writeString(entry.second);
    }
    detail::dispatch(player, cb::CustomReportDetails, buf);
}

// 0x7B Server Links — ClientboundServerLinksPacket
//   VarInt count, далее: Bool isBuiltIn, VarInt knownType | TextComponent label, String url
//   knownType: 0 bug report, 1 community guidelines, 2 support, 3 status, 4 feedback,
//              5 community, 6 website, 7 forums, 8 news, 9 announcements
inline void sendServerLinks(const PlayerPtr& player,
                            const std::vector<std::pair<std::string, std::string>>& links) {
    net::Buffer buf;
    buf.writeVarInt(static_cast<i32>(links.size()));
    for (const auto& link : links) {
        buf.writeBool(false); // своя подпись, а не встроенный тип
        writeTextComponent(buf, link.first);
        buf.writeString(link.second);
    }
    detail::dispatch(player, cb::ServerLinks, buf);
}

// ============================================================
// PACKETS_V12: остаток «простых» clientbound-пакетов Play (protocol 767).
// Здесь то, что кодируется без NBT-структур и палитр чанков.
// ============================================================

// 0x00 Bundle Delimiter — тело пустое, два пакета оборачивают группу на один тик
inline void sendBundleDelimiter(const PlayerPtr& player) {
    net::Buffer buf;
    detail::dispatch(player, cb::BundleDelimiter, buf);
}

// 0x01 Spawn Entity — VarInt entityId, UUID, VarInt type, Double x/y/z,
//   Angle pitch, Angle yaw, Angle headYaw, VarInt data, Short velX/velY/velZ
//   Игроки в 1.21.1 спавнятся этим же пакетом: SpawnPlayer убран.
inline std::vector<u8> buildSpawnEntity(i32 entityId, const UUID& uuid, i32 typeId,
                                        f64 x, f64 y, f64 z,
                                        Angle pitch, Angle yaw, Angle headYaw,
                                        i32 data = 0, i16 velX = 0, i16 velY = 0, i16 velZ = 0) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeUUID(uuid);
    buf.writeVarInt(typeId);
    buf.writeF64(x);
    buf.writeF64(y);
    buf.writeF64(z);
    buf.writeAngle(pitch);
    buf.writeAngle(yaw);
    buf.writeAngle(headYaw);
    buf.writeVarInt(data);
    buf.writeI16(velX);
    buf.writeI16(velY);
    buf.writeI16(velZ);
    return detail::toBytes(buf);
}

// 0x02 Spawn Experience Orb — VarInt entityId, Double x/y/z, Short count
inline std::vector<u8> buildSpawnExperienceOrb(i32 entityId, f64 x, f64 y, f64 z, i16 count) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeF64(x);
    buf.writeF64(y);
    buf.writeF64(z);
    buf.writeI16(count);
    return detail::toBytes(buf);
}

// 0x06 Set Block Destroy Stage — VarInt entityId, Position, Byte stage (0..9)
inline std::vector<u8> buildBlockDestroyStage(i32 entityId, const BlockPos& pos, i8 stage) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writePosition(pos);
    buf.writeByte(static_cast<u8>(stage));
    return detail::toBytes(buf);
}

// 0x08 Block Action — Position, UByte actionId, UByte actionParam, VarInt blockType
inline std::vector<u8> buildBlockAction(const BlockPos& pos, u8 actionId, u8 actionParam, i32 blockType) {
    net::Buffer buf;
    buf.writePosition(pos);
    buf.writeByte(actionId);
    buf.writeByte(actionParam);
    buf.writeVarInt(blockType);
    return detail::toBytes(buf);
}

// 0x0B Change Difficulty — UByte difficulty (0..3), Bool locked
inline void sendChangeDifficulty(const PlayerPtr& player, u8 difficulty, bool locked) {
    net::Buffer buf;
    buf.writeByte(difficulty);
    buf.writeBool(locked);
    detail::dispatch(player, cb::ChangeDifficulty, buf);
}

// 0x0D Chunk Batch Start — тело пустое
inline void sendChunkBatchStart(const PlayerPtr& player) {
    net::Buffer buf;
    detail::dispatch(player, cb::ChunkBatchStart, buf);
}

// 0x0C Chunk Batch Finished — VarInt batchSize; клиент ответит ChunkBatchReceived
inline void sendChunkBatchFinished(const PlayerPtr& player, i32 batchSize) {
    net::Buffer buf;
    buf.writeVarInt(batchSize);
    detail::dispatch(player, cb::ChunkBatchFinished, buf);
}

// 0x10 Command Suggestions Response — VarInt id, VarInt start, VarInt length,
//   VarInt count, далее: String match, Bool hasTooltip, [TextComponent]
inline void sendCommandSuggestions(const PlayerPtr& player, i32 transactionId, i32 start, i32 length,
                                   const std::vector<std::string>& matches) {
    net::Buffer buf;
    buf.writeVarInt(transactionId);
    buf.writeVarInt(start);
    buf.writeVarInt(length);
    buf.writeVarInt(static_cast<i32>(matches.size()));
    for (const auto& match : matches) {
        buf.writeString(match);
        buf.writeBool(false);
    }
    detail::dispatch(player, cb::CommandSuggestions, buf);
}

// 0x14 Set Container Property — Byte containerId, Short property, Short value
inline void sendContainerSetData(const PlayerPtr& player, u8 containerId, i16 property, i16 value) {
    net::Buffer buf;
    buf.writeByte(containerId);
    buf.writeI16(property);
    buf.writeI16(value);
    detail::dispatch(player, cb::ContainerSetData, buf);
}

// 0x16 Cookie Request — Identifier key
inline void sendCookieRequest(const PlayerPtr& player, std::string_view key) {
    net::Buffer buf;
    buf.writeString(key);
    detail::dispatch(player, cb::CookieRequest, buf);
}

// Store Cookie — Identifier key, VarInt length, Byte[] payload
inline void sendStoreCookie(const PlayerPtr& player, std::string_view key, std::span<const u8> payload) {
    net::Buffer buf;
    buf.writeString(key);
    buf.writeVarInt(static_cast<i32>(payload.size()));
    buf.writeBytes(payload);
    detail::dispatch(player, cb::StoreCookie, buf);
}

// Plugin Message — Identifier channel, Byte[] data (до конца пакета)
inline void sendPluginMessage(const PlayerPtr& player, std::string_view channel, std::span<const u8> data) {
    net::Buffer buf;
    buf.writeString(channel);
    buf.writeBytes(data);
    detail::dispatch(player, cb::PluginMessage, buf);
}

// Disconnect (Play) — TextComponent reason в виде NBT
inline void sendPlayDisconnect(const PlayerPtr& player, std::string_view reason) {
    net::Buffer buf;
    writeTextComponent(buf, reason);
    detail::dispatch(player, cb::Disconnect, buf);
}

// System Chat — TextComponent content, Bool overlay (true = над хотбаром)
inline void sendSystemChatMessage(const PlayerPtr& player, std::string_view text, bool overlay = false) {
    net::Buffer buf;
    writeTextComponent(buf, text);
    buf.writeBool(overlay);
    detail::dispatch(player, cb::SystemChat, buf);
}

// Tab List Header/Footer — два TextComponent подряд
inline void sendTabListHeaderFooter(const PlayerPtr& player, std::string_view header, std::string_view footer) {
    net::Buffer buf;
    writeTextComponent(buf, header);
    writeTextComponent(buf, footer);
    detail::dispatch(player, cb::TabList, buf);
}

// Level Particles — Bool longDistance, Double x/y/z, Float offsetX/Y/Z,
//   Float maxSpeed, Int count, VarInt particleId
//   С 1.20.5 particleId стоит в конце пакета, а не в начале.
inline std::vector<u8> buildLevelParticlesPacket(i32 particleId, f64 x, f64 y, f64 z,
                                                 f32 offsetX, f32 offsetY, f32 offsetZ,
                                                 f32 maxSpeed, i32 count, bool longDistance = false) {
    net::Buffer buf;
    buf.writeBool(longDistance);
    buf.writeF64(x);
    buf.writeF64(y);
    buf.writeF64(z);
    buf.writeF32(offsetX);
    buf.writeF32(offsetY);
    buf.writeF32(offsetZ);
    buf.writeF32(maxSpeed);
    buf.writeI32(count);
    buf.writeVarInt(particleId);
    return detail::toBytes(buf);
}

// Move Entity Pos — VarInt entityId, Short dX/dY/dZ, Bool onGround
//   Дельта = (new - old) * 4096, шаг ограничен 8 блоками.
inline std::vector<u8> buildMoveEntityPos(i32 entityId, i16 dx, i16 dy, i16 dz, bool onGround) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeI16(dx);
    buf.writeI16(dy);
    buf.writeI16(dz);
    buf.writeBool(onGround);
    return detail::toBytes(buf);
}

// Move Entity Pos Rot — то же + Angle yaw, Angle pitch
inline std::vector<u8> buildMoveEntityPosRot(i32 entityId, i16 dx, i16 dy, i16 dz,
                                             Angle yaw, Angle pitch, bool onGround) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeI16(dx);
    buf.writeI16(dy);
    buf.writeI16(dz);
    buf.writeAngle(yaw);
    buf.writeAngle(pitch);
    buf.writeBool(onGround);
    return detail::toBytes(buf);
}

// Move Entity Rot — VarInt entityId, Angle yaw, Angle pitch, Bool onGround
inline std::vector<u8> buildMoveEntityRot(i32 entityId, Angle yaw, Angle pitch, bool onGround) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeAngle(yaw);
    buf.writeAngle(pitch);
    buf.writeBool(onGround);
    return detail::toBytes(buf);
}

// Set Entity Link — Int attachedEntityId, Int holdingEntityId (-1 = отвязать)
inline std::vector<u8> buildSetEntityLink(i32 attachedEntityId, i32 holdingEntityId) {
    net::Buffer buf;
    buf.writeI32(attachedEntityId);
    buf.writeI32(holdingEntityId);
    return detail::toBytes(buf);
}

// Set Equipment — VarInt entityId, далее пары (Byte slot, Slot item);
//   у всех записей кроме последней выставлен бит 0x80.
//   Слоты: 0 рука, 1 вторая рука, 2 ботинки, 3 поножи, 4 нагрудник, 5 шлем.
inline std::vector<u8> buildClearEquipment(i32 entityId, const std::vector<u8>& slots) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    for (size_t i = 0; i < slots.size(); ++i) {
        const bool last = (i + 1 == slots.size());
        buf.writeByte(static_cast<u8>(last ? slots[i] : (slots[i] | 0x80)));
        buf.writeVarInt(0);
    }
    return detail::toBytes(buf);
}

// Set Action Bar Text — TextComponent
inline void sendActionBarText(const PlayerPtr& player, std::string_view text) {
    net::Buffer buf;
    writeTextComponent(buf, text);
    detail::dispatch(player, cb::SetActionBarText, buf);
}

// Set Title Text — TextComponent
inline void sendTitleText(const PlayerPtr& player, std::string_view text) {
    net::Buffer buf;
    writeTextComponent(buf, text);
    detail::dispatch(player, cb::SetTitleText, buf);
}

// Set Subtitle Text — TextComponent
inline void sendSubtitleText(const PlayerPtr& player, std::string_view text) {
    net::Buffer buf;
    writeTextComponent(buf, text);
    detail::dispatch(player, cb::SetSubtitleText, buf);
}

// Set Title Animation Times — Int fadeIn, Int stay, Int fadeOut (в тиках)
inline void sendTitleAnimation(const PlayerPtr& player, i32 fadeIn, i32 stay, i32 fadeOut) {
    net::Buffer buf;
    buf.writeI32(fadeIn);
    buf.writeI32(stay);
    buf.writeI32(fadeOut);
    detail::dispatch(player, cb::SetTitleAnimation, buf);
}

// Set Border Center — Double x, Double z
inline void sendBorderCenter(const PlayerPtr& player, f64 x, f64 z) {
    net::Buffer buf;
    buf.writeF64(x);
    buf.writeF64(z);
    detail::dispatch(player, cb::SetBorderCenter, buf);
}

// Set Border Lerp Size — Double oldDiameter, Double newDiameter, VarLong speedMs
inline void sendBorderLerpSize(const PlayerPtr& player, f64 oldDiameter, f64 newDiameter, i64 speedMs) {
    net::Buffer buf;
    buf.writeF64(oldDiameter);
    buf.writeF64(newDiameter);
    buf.writeVarLong(speedMs);
    detail::dispatch(player, cb::SetBorderLerpSize, buf);
}

// Set Border Size — Double diameter
inline void sendBorderSize(const PlayerPtr& player, f64 diameter) {
    net::Buffer buf;
    buf.writeF64(diameter);
    detail::dispatch(player, cb::SetBorderSize, buf);
}

// Set Border Warning Delay — VarInt seconds
inline void sendBorderWarningDelay(const PlayerPtr& player, i32 warningTimeSeconds) {
    net::Buffer buf;
    buf.writeVarInt(warningTimeSeconds);
    detail::dispatch(player, cb::SetBorderWarningDelay, buf);
}

// Set Border Warning Distance — VarInt blocks
inline void sendBorderWarningDistance(const PlayerPtr& player, i32 warningBlocks) {
    net::Buffer buf;
    buf.writeVarInt(warningBlocks);
    detail::dispatch(player, cb::SetBorderWarningDist, buf);
}

// Sound Effect — Holder<SoundEvent>, VarInt category, Int x*8, Int y*8, Int z*8,
//   Float volume, Float pitch, Long seed
inline std::vector<u8> buildSoundEffect(std::string_view soundName, i32 category,
                                        f64 x, f64 y, f64 z,
                                        f32 volume, f32 pitch, i64 seed) {
    net::Buffer buf;
    writeInlineSoundHolder(buf, soundName);
    buf.writeVarInt(category);
    buf.writeI32(static_cast<i32>(x * 8.0));
    buf.writeI32(static_cast<i32>(y * 8.0));
    buf.writeI32(static_cast<i32>(z * 8.0));
    buf.writeF32(volume);
    buf.writeF32(pitch);
    buf.writeI64(seed);
    return detail::toBytes(buf);
}

// Stop Sound — Byte flags (0x01 есть источник, 0x02 есть звук), [VarInt source], [Identifier sound]
inline void sendStopSound(const PlayerPtr& player, std::string_view sound = {}, i32 source = -1) {
    net::Buffer buf;
    u8 flags = 0;
    if (source >= 0) flags |= 0x01;
    if (!sound.empty()) flags |= 0x02;
    buf.writeByte(flags);
    if (source >= 0) buf.writeVarInt(source);
    if (!sound.empty()) buf.writeString(sound);
    detail::dispatch(player, cb::StopSound, buf);
}

// Player Abilities — Byte flags (0x01 неуязвим, 0x02 летит, 0x04 может летать,
//   0x08 творческий), Float flyingSpeed, Float fovModifier
inline void sendPlayerAbilities(const PlayerPtr& player, u8 flags,
                                f32 flyingSpeed = 0.05f, f32 fovModifier = 0.1f) {
    net::Buffer buf;
    buf.writeByte(flags);
    buf.writeF32(flyingSpeed);
    buf.writeF32(fovModifier);
    detail::dispatch(player, cb::PlayerAbilities, buf);
}

// Player Info Remove — VarInt count, UUID[]
inline std::vector<u8> buildPlayerInfoRemove(const std::vector<UUID>& uuids) {
    net::Buffer buf;
    buf.writeVarInt(static_cast<i32>(uuids.size()));
    for (const auto& uuid : uuids) buf.writeUUID(uuid);
    return detail::toBytes(buf);
}

// Update Section Blocks — Long sectionPos (x 22 бита | z 22 бита | y 20 бит),
//   VarInt count, VarLong[]: (stateId << 12) | (x<<8 | z<<4 | y) внутри секции
inline std::vector<u8> buildSectionBlocksUpdate(i32 sectionX, i32 sectionY, i32 sectionZ,
                                                const std::vector<std::pair<u16, i32>>& blocks) {
    net::Buffer buf;
    const i64 packed = ((static_cast<i64>(sectionX) & 0x3FFFFF) << 42)
                     | ((static_cast<i64>(sectionZ) & 0x3FFFFF) << 20)
                     | (static_cast<i64>(sectionY) & 0xFFFFF);
    buf.writeI64(packed);
    buf.writeVarInt(static_cast<i32>(blocks.size()));
    for (const auto& block : blocks) {
        buf.writeVarLong((static_cast<i64>(block.second) << 12) | static_cast<i64>(block.first));
    }
    return detail::toBytes(buf);
}

// Look At — VarInt feetOrEyes (0 ноги, 1 глаза), Double x/y/z, Bool isEntity
inline void sendLookAt(const PlayerPtr& player, f64 x, f64 y, f64 z, bool atEyes = true) {
    net::Buffer buf;
    buf.writeVarInt(atEyes ? 1 : 0);
    buf.writeF64(x);
    buf.writeF64(y);
    buf.writeF64(z);
    buf.writeBool(false);
    detail::dispatch(player, cb::LookAt, buf);
}

// Remove Resource Pack — Bool hasUuid, [UUID]; без UUID снимает все паки
inline void sendResourcePackPop(const PlayerPtr& player) {
    net::Buffer buf;
    buf.writeBool(false);
    detail::dispatch(player, cb::ResourcePackPop, buf);
}

// Add Resource Pack — UUID, String url, String hash, Bool forced,
//   Bool hasPromptMessage, [TextComponent]
inline void sendResourcePackPush(const PlayerPtr& player, const UUID& uuid, std::string_view url,
                                 std::string_view hash, bool forced, std::string_view prompt = {}) {
    net::Buffer buf;
    buf.writeUUID(uuid);
    buf.writeString(url);
    buf.writeString(hash);
    buf.writeBool(forced);
    const bool hasPrompt = !prompt.empty();
    buf.writeBool(hasPrompt);
    if (hasPrompt) writeTextComponent(buf, prompt);
    detail::dispatch(player, cb::ResourcePackPush, buf);
}

// Select Advancements Tab — Bool hasId, [Identifier tabId]
inline void sendSelectAdvancementsTab(const PlayerPtr& player, std::string_view tabId = {}) {
    net::Buffer buf;
    const bool hasId = !tabId.empty();
    buf.writeBool(hasId);
    if (hasId) buf.writeString(tabId);
    detail::dispatch(player, cb::SelectAdvancementsTab, buf);
}

// Open Horse Screen — VarInt containerId, VarInt slotCount, Int entityId
inline void sendOpenHorseScreen(const PlayerPtr& player, i32 containerId, i32 slotCount, i32 entityId) {
    net::Buffer buf;
    buf.writeVarInt(containerId);
    buf.writeVarInt(slotCount);
    buf.writeI32(entityId);
    detail::dispatch(player, cb::OpenHorseScreen, buf);
}

// Start Configuration — тело пустое; клиент уходит в Configuration и ждёт FinishConfiguration
inline void sendStartConfiguration(const PlayerPtr& player) {
    net::Buffer buf;
    detail::dispatch(player, cb::StartConfiguration, buf);
}

// ============================================================
// PACKETS_V21: метаданные сущностей, таб-лист, атрибуты, биомы чанка.
// Форматы сверены с protocol.json из minecraft-data 1.21.1 (version 767).
// ============================================================

// Индексы и флаги synched-метаданных. Индекс — это порядковый номер поля
// в иерархии класса, поэтому один и тот же номер у разных сущностей
// значит разное: 8 у LivingEntity — состояние рук, 8 у ItemEntity — сам предмет.
namespace meta {
inline constexpr u8 SharedFlags        = 0;  // Entity
inline constexpr u8 AirSupply          = 1;  // Entity
inline constexpr u8 Pose               = 6;  // Entity
inline constexpr u8 TicksFrozen        = 7;  // Entity
inline constexpr u8 LivingHandStates   = 8;  // LivingEntity
inline constexpr u8 ItemStack          = 8;  // ItemEntity / ThrowableItemProjectile
inline constexpr u8 BoatVariant        = 11; // Boat
inline constexpr u8 DisplayedSkinParts = 17; // Player

// DATA_SHARED_FLAGS (индекс 0)
inline constexpr u8 FlagOnFire     = 0x01;
inline constexpr u8 FlagCrouching  = 0x02;
inline constexpr u8 FlagSprinting  = 0x08;
inline constexpr u8 FlagFallFlying = 0x80;

// Pose (сериализатор 21)
inline constexpr i32 PoseStanding    = 0;
inline constexpr i32 PoseFallFlying  = 1;
inline constexpr i32 PoseCrouching   = 5;

// LivingEntity hand states (индекс 8, byte)
inline constexpr u8 HandActive  = 0x01;
inline constexpr u8 HandOffhand = 0x02;
} // namespace meta

// 0x58 Set Entity Metadata — VarInt entityId, далее пачка полей
//   (Byte index, VarInt serializerId, value…), терминатор 0xFF.
// Сериализаторы, которые реально нужны серверу: 0 = Byte, 1 = VarInt,
// 7 = Slot, 21 = Pose. build() дописывает терминатор, звать его нужно
// ровно один раз на объект.
class EntityMetadata {
public:
    explicit EntityMetadata(i32 entityId) { buf_.writeVarInt(entityId); }

    EntityMetadata& byteField(u8 index, u8 value) {
        buf_.writeByte(index);
        buf_.writeVarInt(0);
        buf_.writeByte(value);
        return *this;
    }

    EntityMetadata& varIntField(u8 index, i32 value) {
        buf_.writeByte(index);
        buf_.writeVarInt(1);
        buf_.writeVarInt(value);
        return *this;
    }

    EntityMetadata& poseField(i32 pose) {
        buf_.writeByte(meta::Pose);
        buf_.writeVarInt(21);
        buf_.writeVarInt(pose);
        return *this;
    }

    // Slot: VarInt count, VarInt itemId, VarInt добавленных компонентов, VarInt снятых
    EntityMetadata& slotField(u8 index, i32 itemId, i32 count) {
        buf_.writeByte(index);
        buf_.writeVarInt(7);
        buf_.writeVarInt(count);
        buf_.writeVarInt(itemId);
        buf_.writeVarInt(0);
        buf_.writeVarInt(0);
        return *this;
    }

    std::vector<u8> build() {
        buf_.writeByte(0xFF);
        return detail::toBytes(buf_);
    }

private:
    net::Buffer buf_;
};

// Собрать байт DATA_SHARED_FLAGS из состояний сущности.
inline u8 sharedFlagsByte(bool onFire, bool sneaking, bool sprinting, bool fallFlying) {
    u8 flags = 0;
    if (onFire)     flags |= meta::FlagOnFire;
    if (sneaking)   flags |= meta::FlagCrouching;
    if (sprinting)  flags |= meta::FlagSprinting;
    if (fallFlying) flags |= meta::FlagFallFlying;
    return flags;
}

inline std::vector<u8> buildEntitySharedFlags(i32 entityId, u8 flags) {
    return EntityMetadata(entityId).byteField(meta::SharedFlags, flags).build();
}

// Полный внешний вид игрока: флаги, поза и слои скина одним пакетом.
inline std::vector<u8> buildPlayerAppearance(i32 entityId, bool sneaking, bool sprinting,
                                             u8 skinParts, bool onFire, bool elytraFlying) {
    return EntityMetadata(entityId)
        .byteField(meta::SharedFlags, sharedFlagsByte(onFire, sneaking, sprinting, elytraFlying))
        .poseField(elytraFlying ? meta::PoseFallFlying : (sneaking ? meta::PoseCrouching : meta::PoseStanding))
        .byteField(meta::DisplayedSkinParts, skinParts)
        .build();
}

inline std::vector<u8> buildEntityHandStates(i32 entityId, bool handActive, bool offhand) {
    const u8 state = handActive
        ? static_cast<u8>(meta::HandActive | (offhand ? meta::HandOffhand : 0))
        : static_cast<u8>(0);
    return EntityMetadata(entityId).byteField(meta::LivingHandStates, state).build();
}

inline std::vector<u8> buildEntityItemStack(i32 entityId, i32 itemId, i32 count) {
    return EntityMetadata(entityId).slotField(meta::ItemStack, itemId, count).build();
}

inline std::vector<u8> buildEntityTicksFrozen(i32 entityId, i32 ticks) {
    return EntityMetadata(entityId).varIntField(meta::TicksFrozen, ticks).build();
}

inline std::vector<u8> buildEntityAirSupply(i32 entityId, i32 air) {
    return EntityMetadata(entityId).varIntField(meta::AirSupply, air).build();
}

inline std::vector<u8> buildBoatVariant(i32 entityId, i32 variant) {
    return EntityMetadata(entityId).varIntField(meta::BoatVariant, variant).build();
}

// 0x3E Player Info Update — Byte action (EnumSet из 6 действий, ровно один байт,
// НЕ VarInt), VarInt count, далее на каждую запись: UUID и поля тех действий,
// чьи биты взведены, в порядке битов.
namespace playerInfo {
inline constexpr u8 AddPlayer         = 0x01;
inline constexpr u8 InitializeChat    = 0x02;
inline constexpr u8 UpdateGameMode    = 0x04;
inline constexpr u8 UpdateListed      = 0x08;
inline constexpr u8 UpdateLatency     = 0x10;
inline constexpr u8 UpdateDisplayName = 0x20;
} // namespace playerInfo

// Добавление игрока в таб-лист: ADD_PLAYER | UPDATE_GAME_MODE | UPDATE_LISTED |
// UPDATE_LATENCY. profileProperties — уже закодированный хвост game_profile
// (VarInt количество свойств и сами свойства): скин и плащ собираются в ядре,
// потому что зависят от режима авторизации, а не от протокола.
inline std::vector<u8> buildPlayerInfoAdd(const UUID& uuid, std::string_view name,
                                          std::span<const u8> profileProperties,
                                          i32 gameMode, bool listed, i32 latencyMs) {
    net::Buffer buf;
    buf.writeByte(static_cast<u8>(playerInfo::AddPlayer | playerInfo::UpdateGameMode |
                                 playerInfo::UpdateListed | playerInfo::UpdateLatency));
    buf.writeVarInt(1);
    buf.writeUUID(uuid);
    buf.writeString(name);
    buf.writeBytes(profileProperties);
    buf.writeVarInt(gameMode);
    buf.writeVarInt(listed ? 1 : 0);
    buf.writeVarInt(latencyMs);
    return detail::toBytes(buf);
}

// Только смена режима игры уже добавленного игрока.
inline std::vector<u8> buildPlayerInfoGameMode(const UUID& uuid, i32 gameMode) {
    net::Buffer buf;
    buf.writeByte(playerInfo::UpdateGameMode);
    buf.writeVarInt(1);
    buf.writeUUID(uuid);
    buf.writeVarInt(gameMode);
    return detail::toBytes(buf);
}

// Только пинг в таб-листе (миллисекунды; -1 рисует красный крест).
inline std::vector<u8> buildPlayerInfoLatency(const UUID& uuid, i32 latencyMs) {
    net::Buffer buf;
    buf.writeByte(playerInfo::UpdateLatency);
    buf.writeVarInt(1);
    buf.writeUUID(uuid);
    buf.writeVarInt(latencyMs);
    return detail::toBytes(buf);
}

// 0x75 Update Attributes — VarInt entityId, VarInt count, далее на свойство:
//   VarInt attributeId, Double baseValue, VarInt количество модификаторов.
// В 1.21.1 attributeId — числовой индекс реестра, а не строка.
namespace attr {
inline constexpr i32 Armor           = 0;
inline constexpr i32 ArmorToughness  = 1;
inline constexpr i32 AttackDamage    = 2;
inline constexpr i32 AttackSpeed     = 4;
inline constexpr i32 FollowRange     = 10;
inline constexpr i32 MaxHealth       = 16;
inline constexpr i32 MovementSpeed   = 17;
inline constexpr i32 Scale           = 19;
} // namespace attr

struct AttributeValue {
    i32 attributeId = 0;
    f64 baseValue = 0.0;
};

inline std::vector<u8> buildUpdateAttributes(i32 entityId, const std::vector<AttributeValue>& values) {
    net::Buffer buf;
    buf.writeVarInt(entityId);
    buf.writeVarInt(static_cast<i32>(values.size()));
    for (const AttributeValue& v : values) {
        buf.writeVarInt(v.attributeId);
        buf.writeF64(v.baseValue);
        buf.writeVarInt(0); // модификаторов нет: шлём готовое базовое значение
    }
    return detail::toBytes(buf);
}

// 0x0E Chunk Biomes — VarInt count, далее на чанк: Long packedChunkPos
//   ((z << 32) | (x & 0xFFFFFFFF)) и ByteArray с секциями биомов в том же
//   paletted-формате, что и в Chunk Data.
// Пакет нужен, чтобы поменять биом без пересылки всего чанка.
struct ChunkBiomeData {
    i32 chunkX = 0;
    i32 chunkZ = 0;
    std::vector<u8> sections;
};

inline i64 packChunkPos(i32 chunkX, i32 chunkZ) {
    return (static_cast<i64>(chunkZ) << 32) | (static_cast<i64>(static_cast<u32>(chunkX)));
}

inline std::vector<u8> buildChunkBiomes(const std::vector<ChunkBiomeData>& chunks) {
    net::Buffer buf;
    buf.writeVarInt(static_cast<i32>(chunks.size()));
    for (const ChunkBiomeData& c : chunks) {
        buf.writeI64(packChunkPos(c.chunkX, c.chunkZ));
        buf.writeVarInt(static_cast<i32>(c.sections.size()));
        buf.writeBytes(std::span<const u8>(c.sections.data(), c.sections.size()));
    }
    return detail::toBytes(buf);
}

inline void sendChunkBiomes(const PlayerPtr& player, const std::vector<ChunkBiomeData>& chunks) {
    if (!player) return;
    auto conn = player->getConnection();
    if (!conn) return;
    conn->sendPacket(cb::ChunkBiomes, buildChunkBiomes(chunks));
}

} // namespace nc::packets

