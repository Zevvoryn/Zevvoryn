#pragma once

// ============================================================
// PACKETS_V9: единый реестр идентификаторов пакетов протокола 1.21.1 (protocol 767).
//
// Здесь и только здесь живут числовые id пакетов. В остальном коде сервера
// нельзя писать «магические» 0x__ — используйте константы отсюда, иначе
// ориентироваться в протоколе становится невозможно.
//
// Значения сверены с официальными маппингами server.jar 1.21.1.
// ============================================================

#include "../core/types.hpp"

namespace nc::packets {

// ------------------------------------------------------------
// Состояния до Play. Идентификаторы разделены по направлению и состоянию,
// потому что одинаковые wire-id имеют разный смысл в разных фазах.
// ------------------------------------------------------------
namespace handshake::sb {
inline constexpr i32 Intention = 0x00;
}
namespace status::sb {
inline constexpr i32 Request = 0x00;
inline constexpr i32 Ping    = 0x01;
}
namespace status::cb {
inline constexpr i32 Response = 0x00;
inline constexpr i32 Pong     = 0x01;
}
namespace login::sb {
inline constexpr i32 Hello             = 0x00;
inline constexpr i32 Key               = 0x01;
inline constexpr i32 CustomQueryAnswer = 0x02;
inline constexpr i32 Acknowledged      = 0x03;
inline constexpr i32 CookieResponse    = 0x04;
}
namespace login::cb {
inline constexpr i32 Disconnect  = 0x00;
inline constexpr i32 Hello       = 0x01;
inline constexpr i32 Success     = 0x02;
inline constexpr i32 Compression = 0x03;
inline constexpr i32 CustomQuery = 0x04;
inline constexpr i32 CookieRequest = 0x05;
}
namespace config::sb {
inline constexpr i32 ClientInformation    = 0x00;
inline constexpr i32 CookieResponse       = 0x01;
inline constexpr i32 CustomPayload        = 0x02;
inline constexpr i32 FinishAcknowledged   = 0x03;
inline constexpr i32 KeepAlive            = 0x04;
inline constexpr i32 Pong                 = 0x05;
inline constexpr i32 ResourcePackResponse = 0x06;
inline constexpr i32 SelectKnownPacks     = 0x07;
}
namespace config::cb {
inline constexpr i32 CookieRequest       = 0x00;
inline constexpr i32 CustomPayload       = 0x01;
inline constexpr i32 Disconnect          = 0x02;
inline constexpr i32 Finish              = 0x03;
inline constexpr i32 KeepAlive           = 0x04;
inline constexpr i32 Ping                = 0x05;
inline constexpr i32 ResetChat           = 0x06;
inline constexpr i32 RegistryData        = 0x07;
inline constexpr i32 ResourcePackPop     = 0x08;
inline constexpr i32 ResourcePackPush    = 0x09;
inline constexpr i32 StoreCookie         = 0x0A;
inline constexpr i32 Transfer            = 0x0B;
inline constexpr i32 FeatureFlags        = 0x0C;
inline constexpr i32 UpdateTags          = 0x0D;
inline constexpr i32 SelectKnownPacks    = 0x0E;
inline constexpr i32 CustomReportDetails = 0x0F;
inline constexpr i32 ServerLinks         = 0x10;
}

// ------------------------------------------------------------
// Clientbound, состояние Play (сервер -> клиент)
// ------------------------------------------------------------
namespace cb {

inline constexpr i32 BundleDelimiter        = 0x00;
inline constexpr i32 SpawnEntity            = 0x01;
inline constexpr i32 SpawnExperienceOrb     = 0x02;
inline constexpr i32 EntityAnimation        = 0x03;
inline constexpr i32 AwardStatistics        = 0x04; // ClientboundAwardStatsPacket
inline constexpr i32 AckBlockChange         = 0x05;
inline constexpr i32 BlockDestroyStage      = 0x06;
inline constexpr i32 BlockEntityData        = 0x07;
inline constexpr i32 BlockAction            = 0x08;
inline constexpr i32 BlockUpdate            = 0x09;
inline constexpr i32 BossEvent              = 0x0A;
inline constexpr i32 ChangeDifficulty       = 0x0B;
inline constexpr i32 ChunkBatchFinished     = 0x0C;
inline constexpr i32 ChunkBatchStart        = 0x0D;
inline constexpr i32 ChunkBiomes            = 0x0E;
inline constexpr i32 ClearTitles            = 0x0F; // ClientboundClearTitlesPacket
inline constexpr i32 CommandSuggestions     = 0x10;
inline constexpr i32 Commands               = 0x11;
inline constexpr i32 ContainerClose         = 0x12;
inline constexpr i32 ContainerSetContent    = 0x13;
inline constexpr i32 ContainerSetData       = 0x14;
inline constexpr i32 ContainerSetSlot       = 0x15;
inline constexpr i32 CookieRequest          = 0x16;
inline constexpr i32 SetCooldown            = 0x17;
inline constexpr i32 ChatSuggestions        = 0x18;
inline constexpr i32 PluginMessage          = 0x19;
inline constexpr i32 DamageEvent            = 0x1A;
inline constexpr i32 DebugSample            = 0x1B;
inline constexpr i32 DeleteChatMessage      = 0x1C;
inline constexpr i32 Disconnect             = 0x1D;
inline constexpr i32 DisguisedChatMessage   = 0x1E;
inline constexpr i32 EntityEvent            = 0x1F;
inline constexpr i32 Explosion              = 0x20; // ClientboundExplodePacket
inline constexpr i32 UnloadChunk            = 0x21; // ClientboundForgetLevelChunkPacket
inline constexpr i32 GameEvent              = 0x22;
inline constexpr i32 OpenHorseScreen        = 0x23;
inline constexpr i32 HurtAnimation          = 0x24;
inline constexpr i32 InitializeWorldBorder  = 0x25; // ClientboundInitializeBorderPacket
inline constexpr i32 KeepAlive              = 0x26;
inline constexpr i32 ChunkDataAndLight      = 0x27;
inline constexpr i32 LevelEvent             = 0x28;
inline constexpr i32 LevelParticles         = 0x29;
inline constexpr i32 LightUpdate            = 0x2A;
inline constexpr i32 Login                  = 0x2B;
inline constexpr i32 MapData                = 0x2C; // ClientboundMapItemDataPacket
inline constexpr i32 MerchantOffers         = 0x2D;
inline constexpr i32 MoveEntityPos          = 0x2E;
inline constexpr i32 MoveEntityPosRot       = 0x2F;
inline constexpr i32 MoveEntityRot          = 0x30;
inline constexpr i32 MoveVehicle            = 0x31; // ClientboundMoveVehiclePacket
inline constexpr i32 OpenBook               = 0x32; // ClientboundOpenBookPacket
inline constexpr i32 OpenScreen             = 0x33;
inline constexpr i32 OpenSignEditor         = 0x34; // ClientboundOpenSignEditorPacket
inline constexpr i32 Ping                   = 0x35;
inline constexpr i32 PongResponse           = 0x36;
inline constexpr i32 PlaceGhostRecipe       = 0x37; // ClientboundPlaceGhostRecipePacket
inline constexpr i32 PlayerAbilities        = 0x38;
inline constexpr i32 PlayerChat             = 0x39;
inline constexpr i32 EndCombat              = 0x3A; // ClientboundPlayerCombatEndPacket
inline constexpr i32 EnterCombat            = 0x3B; // ClientboundPlayerCombatEnterPacket
inline constexpr i32 DeathCombat            = 0x3C;
inline constexpr i32 PlayerInfoRemove       = 0x3D;
inline constexpr i32 PlayerInfoUpdate       = 0x3E;
inline constexpr i32 LookAt                 = 0x3F;
inline constexpr i32 PlayerPosition         = 0x40;
inline constexpr i32 UnlockRecipes          = 0x41;
inline constexpr i32 RemoveEntities         = 0x42;
inline constexpr i32 RemoveMobEffect        = 0x43;
inline constexpr i32 ResetScore             = 0x44;
inline constexpr i32 ResourcePackPop        = 0x45;
inline constexpr i32 ResourcePackPush       = 0x46;
inline constexpr i32 Respawn                = 0x47;
inline constexpr i32 RotateHead             = 0x48;
inline constexpr i32 SectionBlocksUpdate    = 0x49;
inline constexpr i32 SelectAdvancementsTab  = 0x4A;
inline constexpr i32 ServerData             = 0x4B; // ClientboundServerDataPacket
inline constexpr i32 SetActionBarText       = 0x4C;
inline constexpr i32 SetBorderCenter        = 0x4D;
inline constexpr i32 SetBorderLerpSize      = 0x4E;
inline constexpr i32 SetBorderSize          = 0x4F;
inline constexpr i32 SetBorderWarningDelay  = 0x50;
inline constexpr i32 SetBorderWarningDist   = 0x51;
inline constexpr i32 SetCamera              = 0x52; // ClientboundSetCameraPacket
inline constexpr i32 SetHeldSlot            = 0x53;
inline constexpr i32 SetCenterChunk         = 0x54;
inline constexpr i32 SetRenderDistance      = 0x55;
inline constexpr i32 SetDefaultSpawn        = 0x56;
inline constexpr i32 DisplayObjective       = 0x57;
inline constexpr i32 SetEntityMetadata      = 0x58;
inline constexpr i32 SetEntityLink          = 0x59;
inline constexpr i32 SetEntityMotion        = 0x5A;
inline constexpr i32 SetEquipment           = 0x5B;
inline constexpr i32 SetExperience          = 0x5C;
inline constexpr i32 SetHealth              = 0x5D;
inline constexpr i32 SetObjective           = 0x5E;
inline constexpr i32 SetPassengers          = 0x5F;
inline constexpr i32 SetPlayerTeam          = 0x60;
inline constexpr i32 SetScore               = 0x61;
inline constexpr i32 SetSimulationDistance  = 0x62; // ClientboundSetSimulationDistancePacket
inline constexpr i32 SetSubtitleText        = 0x63;
inline constexpr i32 SetTime                = 0x64;
inline constexpr i32 SetTitleText           = 0x65;
inline constexpr i32 SetTitleAnimation      = 0x66;
inline constexpr i32 EntitySoundEffect      = 0x67; // ClientboundSoundEntityPacket
inline constexpr i32 SoundEffect            = 0x68;
inline constexpr i32 StartConfiguration     = 0x69;
inline constexpr i32 StopSound              = 0x6A;
inline constexpr i32 StoreCookie            = 0x6B;
inline constexpr i32 SystemChat             = 0x6C;
inline constexpr i32 TabList                = 0x6D;
inline constexpr i32 TagQuery               = 0x6E;
inline constexpr i32 TakeItemEntity         = 0x6F;
inline constexpr i32 TeleportEntity         = 0x70;
inline constexpr i32 TickingState           = 0x71;
inline constexpr i32 TickingStep            = 0x72;
inline constexpr i32 Transfer               = 0x73;
inline constexpr i32 UpdateAdvancements     = 0x74; // ClientboundUpdateAdvancementsPacket
inline constexpr i32 UpdateAttributes       = 0x75;
inline constexpr i32 UpdateMobEffect        = 0x76;
inline constexpr i32 UpdateRecipes          = 0x77;
inline constexpr i32 UpdateTags             = 0x78; // ClientboundUpdateTagsPacket
inline constexpr i32 ProjectilePower        = 0x79; // ClientboundProjectilePowerPacket
inline constexpr i32 CustomReportDetails    = 0x7A;
inline constexpr i32 ServerLinks            = 0x7B;

} // namespace cb

// ------------------------------------------------------------
// Serverbound, состояние Play (клиент -> сервер)
// ------------------------------------------------------------
namespace sb {

inline constexpr i32 AcceptTeleportation    = 0x00;
inline constexpr i32 BlockEntityTagQuery    = 0x01;
inline constexpr i32 ChangeDifficulty       = 0x02;
inline constexpr i32 ChatAck                = 0x03;
inline constexpr i32 ChatCommand            = 0x04;
inline constexpr i32 ChatCommandSigned      = 0x05;
inline constexpr i32 Chat                   = 0x06;
inline constexpr i32 ChatSessionUpdate      = 0x07;
inline constexpr i32 ChunkBatchReceived     = 0x08;
inline constexpr i32 ClientCommand          = 0x09;
inline constexpr i32 ClientInformation      = 0x0A;
inline constexpr i32 CommandSuggestion      = 0x0B;
inline constexpr i32 ConfigurationAck       = 0x0C;
inline constexpr i32 ContainerButtonClick   = 0x0D;
inline constexpr i32 ContainerClick         = 0x0E;
inline constexpr i32 ContainerClose         = 0x0F;
inline constexpr i32 ContainerSlotStateSet  = 0x10;
inline constexpr i32 CookieResponse         = 0x11;
inline constexpr i32 CustomPayload          = 0x12;
inline constexpr i32 DebugSampleSubscribe   = 0x13;
inline constexpr i32 EditBook               = 0x14;
inline constexpr i32 EntityTagQuery         = 0x15;
inline constexpr i32 Interact               = 0x16;
inline constexpr i32 JigsawGenerate         = 0x17;
inline constexpr i32 KeepAlive              = 0x18;
inline constexpr i32 LockDifficulty         = 0x19;
inline constexpr i32 MovePlayerPos          = 0x1A;
inline constexpr i32 MovePlayerPosRot       = 0x1B;
inline constexpr i32 MovePlayerRot          = 0x1C;
inline constexpr i32 MovePlayerStatusOnly   = 0x1D;
inline constexpr i32 MoveVehicle            = 0x1E;
inline constexpr i32 PaddleBoat             = 0x1F;
inline constexpr i32 PickItem               = 0x20;
inline constexpr i32 PingRequest            = 0x21;
inline constexpr i32 PlaceRecipe            = 0x22;
inline constexpr i32 PlayerAbilities        = 0x23;
inline constexpr i32 PlayerAction           = 0x24;
inline constexpr i32 PlayerCommand          = 0x25;
inline constexpr i32 PlayerInput            = 0x26;
inline constexpr i32 Pong                   = 0x27;
inline constexpr i32 RecipeBookChangeSet    = 0x28;
inline constexpr i32 RecipeBookSeenRecipe   = 0x29;
inline constexpr i32 RenameItem             = 0x2A;
inline constexpr i32 ResourcePackResponse   = 0x2B;
inline constexpr i32 SeenAdvancements       = 0x2C;
inline constexpr i32 SelectTrade            = 0x2D;
inline constexpr i32 SetBeaconEffect        = 0x2E;
inline constexpr i32 SetCarriedItem         = 0x2F;
inline constexpr i32 SetCommandBlock        = 0x30;
inline constexpr i32 SetCommandMinecart     = 0x31;
inline constexpr i32 SetCreativeModeSlot    = 0x32;
inline constexpr i32 SetJigsawBlock         = 0x33;
inline constexpr i32 SetStructureBlock      = 0x34;
inline constexpr i32 SignUpdate             = 0x35;
inline constexpr i32 Swing                  = 0x36;
inline constexpr i32 TeleportToEntity       = 0x37;
inline constexpr i32 UseItemOn              = 0x38;
inline constexpr i32 UseItem                = 0x39;

} // namespace sb

// ------------------------------------------------------------
// SoundSource (net.minecraft.sounds.SoundSource) — порядок важен,
// по проводу уходит порядковый номер enum.
// ------------------------------------------------------------
enum class SoundCategory : i32 {
    Master  = 0,
    Music   = 1,
    Records = 2,
    Weather = 3,
    Blocks  = 4,
    Hostile = 5,
    Neutral = 6,
    Players = 7,
    Ambient = 8,
    Voice   = 9,
};

} // namespace nc::packets
