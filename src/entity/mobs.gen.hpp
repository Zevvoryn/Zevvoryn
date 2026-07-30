#pragma once
// MOBS_ALL_V1 — автогенерация, не редактировать вручную.
// Источники: EntityType.java (габариты/категории), DefaultAttributes/Attributes.java (HP, скорость, атака),
// reports/registries.json (числовые id), loot_table/entities/*.json (дроп). Версия 1.21.1.
#include <cstdint>
#include <cstddef>

namespace nc::entity::gen {

enum MobCat : uint8_t { MC_MONSTER = 0, MC_CREATURE = 1, MC_AMBIENT = 2, MC_WATER = 3, MC_MISC = 4 };
enum MobBehavior : uint8_t { MB_PASSIVE = 0, MB_NEUTRAL = 1, MB_HOSTILE = 2 };

struct MobDrop { int16_t item; uint8_t lo; uint8_t hi; int16_t cooked; };

struct MobInfo {
	const char*  name;      // имя без minecraft:
	int          netId;     // числовой id типа сущности в протоколе 767
	float        width;
	float        height;
	float        eyeHeight;
	float        maxHealth;
	float        speed;      // MOVEMENT_SPEED
	float        damage;     // ATTACK_DAMAGE
	float        follow;     // FOLLOW_RANGE
	float        armor;
	uint8_t      cat;        // MobCat
	uint8_t      behavior;   // MobBehavior
	uint8_t      dims;       // битмаска: 1 overworld, 2 nether, 4 end
	bool         fireImmune;
	bool         flying;
	bool         aquatic;
	bool         milkable;
	bool         shearable;
	bool         ranged;      // стреляет: скелеты, ведьма, гаст, блейз, снежный голем
	uint8_t      groupMin;
	uint8_t      groupMax;
	int16_t      spawnEgg;   // id предмета-яйца (или -1)
	uint8_t      dropCount;
	MobDrop      drops[4];
};

inline constexpr MobInfo MOBS[] = {
	{ "allay", 0, 0.3500f, 0.6000f, 0.3600f, 20.0f, 0.1000f, 2.0f, 48.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, true, false, false, false, false, 1, 2, 1009, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "armadillo", 2, 0.7000f, 0.6500f, 0.2600f, 12.0f, 0.1400f, 2.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 1, 2, 1008, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "axolotl", 5, 0.7500f, 0.4200f, 0.2751f, 14.0f, 1.0000f, 2.0f, 16.0f, 0.0f, MC_WATER, MB_PASSIVE, 1, false, false, true, false, false, false, 1, 2, 1010, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "bat", 6, 0.5000f, 0.9000f, 0.4500f, 6.0f, 0.7000f, 2.0f, 16.0f, 0.0f, MC_AMBIENT, MB_PASSIVE, 1, false, true, false, false, false, false, 1, 2, 1011, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "bee", 7, 0.7000f, 0.6000f, 0.3000f, 10.0f, 0.3000f, 2.0f, 48.0f, 0.0f, MC_CREATURE, MB_NEUTRAL, 1, false, true, false, false, false, false, 2, 3, 1012, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "blaze", 8, 0.6000f, 1.8000f, 1.5300f, 20.0f, 0.2300f, 6.0f, 48.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 2, true, false, false, false, false, true, 1, 2, 1013, 1, { { 994, 0, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "bogged", 11, 0.6000f, 1.9900f, 1.7400f, 16.0f, 0.2500f, 2.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, true, 1, 4, 1014, 3, { { 802, 0, 2, -1 }, { 961, 0, 2, -1 }, { 1160, 0, 1, -1 }, { -1, 0, 0, -1 } } },
	{ "breeze", 12, 0.6000f, 1.7700f, 1.3452f, 30.0f, 0.6300f, 3.0f, 24.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, false, 1, 2, 1015, 1, { { 1332, 1, 2, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "camel", 14, 1.7000f, 2.3750f, 2.2750f, 32.0f, 0.0900f, 2.0f, 32.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 1, 2, 1017, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "cat", 15, 0.6000f, 0.7000f, 0.3500f, 10.0f, 0.3000f, 3.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 2, 3, 1016, 1, { { 850, 0, 2, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "cave_spider", 16, 0.7000f, 0.5000f, 0.4500f, 12.0f, 0.3000f, 2.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, false, 1, 2, 1018, 2, { { 850, 0, 2, -1 }, { 1000, 0, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "chicken", 19, 0.4000f, 0.7000f, 0.6440f, 4.0f, 0.2500f, 2.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 2, 4, 1019, 2, { { 851, 0, 2, -1 }, { 990, 1, 1, 991 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "cod", 20, 0.5000f, 0.3000f, 0.1950f, 3.0f, 0.7000f, 2.0f, 16.0f, 0.0f, MC_WATER, MB_PASSIVE, 1, false, false, true, false, false, false, 3, 6, 1020, 2, { { 935, 1, 1, -1 }, { 960, 1, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "cow", 22, 0.9000f, 1.4000f, 1.3000f, 10.0f, 0.2000f, 2.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, true, false, false, 2, 4, 1021, 2, { { 913, 0, 2, -1 }, { 988, 1, 3, 989 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "creeper", 23, 0.6000f, 1.7000f, 1.4450f, 20.0f, 0.2500f, 2.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, false, 1, 2, 1022, 1, { { 852, 0, 2, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "dolphin", 24, 0.9000f, 0.6000f, 0.3000f, 10.0f, 1.2000f, 3.0f, 16.0f, 0.0f, MC_WATER, MB_NEUTRAL, 1, false, false, true, false, false, false, 1, 2, 1023, 1, { { 935, 0, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "donkey", 25, 1.3965f, 1.5000f, 1.4250f, 20.0f, 0.1750f, 2.0f, 32.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 2, 4, 1024, 1, { { 913, 0, 2, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "drowned", 27, 0.6000f, 1.9500f, 1.7400f, 20.0f, 0.2300f, 3.0f, 35.0f, 2.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, false, 1, 2, 1025, 2, { { 992, 0, 2, -1 }, { 813, 1, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "elder_guardian", 29, 1.9975f, 1.9975f, 0.9988f, 80.0f, 0.3000f, 8.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, true, false, false, false, 1, 2, 1026, 4, { { 1116, 0, 2, -1 }, { 935, 1, 1, -1 }, { 1117, 1, 1, -1 }, { 187, 1, 1, -1 } } },
	{ "ender_dragon", 31, 16.0000f, 8.0000f, 6.8000f, 200.0f, 0.7000f, 2.0f, 16.0f, 0.0f, MC_MONSTER, MB_PASSIVE, 1, true, true, false, false, false, false, 1, 2, 1027, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "enderman", 33, 0.6000f, 2.9000f, 2.5500f, 40.0f, 0.3000f, 7.0f, 64.0f, 0.0f, MC_MONSTER, MB_NEUTRAL, 7, false, false, false, false, false, false, 1, 2, 1028, 1, { { 993, 0, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "endermite", 34, 0.4000f, 0.3000f, 0.1300f, 8.0f, 0.2500f, 2.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 5, false, false, false, false, false, false, 1, 2, 1029, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "evoker", 35, 0.6000f, 1.9500f, 1.6575f, 24.0f, 0.5000f, 2.0f, 12.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, false, 1, 2, 1030, 2, { { 1163, 1, 1, -1 }, { 806, 0, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "fox", 42, 0.6000f, 0.7000f, 0.4000f, 10.0f, 0.3000f, 2.0f, 32.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 2, 3, 1031, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "frog", 43, 0.5000f, 0.5000f, 0.4250f, 10.0f, 1.0000f, 10.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 1, 2, 1032, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "ghast", 45, 4.0000f, 4.0000f, 2.6000f, 10.0f, 0.7000f, 2.0f, 100.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 2, true, true, false, false, false, true, 1, 2, 1033, 2, { { 995, 0, 1, -1 }, { 852, 0, 2, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "giant", 46, 3.6000f, 12.0000f, 10.4400f, 100.0f, 0.5000f, 50.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, false, 1, 2, -1, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "glow_squid", 48, 0.8000f, 0.8000f, 0.4000f, 20.0f, 0.7000f, 2.0f, 32.0f, 0.0f, MC_WATER, MB_PASSIVE, 1, false, false, true, false, false, false, 1, 2, 1034, 1, { { 942, 1, 3, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "goat", 49, 0.9000f, 1.3000f, 1.1050f, 10.0f, 0.2000f, 2.0f, 16.0f, 0.0f, MC_CREATURE, MB_NEUTRAL, 1, false, false, false, true, false, false, 2, 3, 1035, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "guardian", 50, 0.8500f, 0.8500f, 0.4250f, 30.0f, 0.5000f, 6.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, true, false, false, false, 1, 2, 1036, 3, { { 1116, 0, 2, -1 }, { 935, 1, 1, -1 }, { 1117, 1, 1, -1 }, { -1, 0, 0, -1 } } },
	{ "hoglin", 51, 1.3965f, 1.4000f, 1.1900f, 40.0f, 0.3000f, 6.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 2, false, false, false, false, false, false, 1, 2, 1037, 2, { { 881, 2, 4, 882 }, { 913, 0, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "horse", 53, 1.3965f, 1.6000f, 1.5200f, 53.0f, 0.2250f, 2.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 2, 4, 1038, 1, { { 913, 0, 2, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "husk", 54, 0.6000f, 1.9500f, 1.7400f, 20.0f, 0.2300f, 3.0f, 35.0f, 2.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, false, 1, 4, 1039, 4, { { 992, 0, 2, -1 }, { 811, 1, 1, -1 }, { 1097, 1, 1, -1 }, { 1098, 1, 1, -1 } } },
	{ "illusioner", 55, 0.6000f, 1.9500f, 1.6575f, 32.0f, 0.5000f, 2.0f, 18.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, true, 1, 2, -1, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "iron_golem", 57, 1.4000f, 2.7000f, 2.2950f, 100.0f, 0.2500f, 15.0f, 16.0f, 0.0f, MC_MISC, MB_NEUTRAL, 1, false, false, false, false, false, false, 1, 2, 1040, 2, { { 219, 0, 2, -1 }, { 811, 3, 5, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "llama", 65, 0.9000f, 1.8700f, 1.7765f, 20.0f, 0.7000f, 2.0f, 40.0f, 0.0f, MC_CREATURE, MB_NEUTRAL, 1, false, false, false, false, false, false, 2, 3, 1041, 1, { { 913, 0, 2, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "magma_cube", 67, 0.5200f, 0.5200f, 0.3250f, 20.0f, 0.2000f, 2.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 2, true, false, false, false, false, false, 1, 2, 1042, 4, { { 1003, 0, 1, -1 }, { 1265, 1, 1, -1 }, { 1264, 1, 1, -1 }, { 1263, 1, 1, -1 } } },
	{ "mooshroom", 70, 0.9000f, 1.4000f, 1.3000f, 10.0f, 0.2000f, 2.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, true, false, false, 2, 4, 1043, 2, { { 913, 0, 2, -1 }, { 988, 1, 3, 989 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "mule", 71, 1.3965f, 1.6000f, 1.5200f, 20.0f, 0.1750f, 2.0f, 32.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 1, 2, 1044, 1, { { 913, 0, 2, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "ocelot", 72, 0.6000f, 0.7000f, 0.5950f, 10.0f, 0.3000f, 3.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 2, 3, 1045, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "panda", 74, 1.3000f, 1.2500f, 1.0625f, 20.0f, 0.1500f, 6.0f, 16.0f, 0.0f, MC_CREATURE, MB_NEUTRAL, 1, false, false, false, false, false, false, 1, 2, 1046, 1, { { 251, 1, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "parrot", 75, 0.5000f, 0.9000f, 0.5400f, 6.0f, 0.2000f, 3.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, true, false, false, false, false, 2, 3, 1047, 1, { { 851, 1, 2, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "phantom", 76, 0.9000f, 0.5000f, 0.1750f, 20.0f, 0.7000f, 2.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, true, false, false, false, false, 1, 2, 1048, 1, { { 1189, 0, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "pig", 77, 0.9000f, 0.9000f, 0.7650f, 10.0f, 0.2500f, 2.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 2, 4, 1049, 1, { { 881, 1, 3, 882 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "piglin", 78, 0.6000f, 1.9500f, 1.7900f, 16.0f, 0.3500f, 5.0f, 16.0f, 0.0f, MC_MONSTER, MB_NEUTRAL, 2, false, false, false, false, false, false, 1, 2, 1050, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "piglin_brute", 79, 0.6000f, 1.9500f, 1.7900f, 50.0f, 0.3500f, 7.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 2, false, false, false, false, false, false, 1, 2, 1051, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "pillager", 80, 0.6000f, 1.9500f, 1.6575f, 24.0f, 0.3500f, 5.0f, 32.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, true, 1, 2, 1052, 1, { { 1331, 1, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "polar_bear", 81, 1.4000f, 1.4000f, 1.1900f, 30.0f, 0.2500f, 6.0f, 20.0f, 0.0f, MC_CREATURE, MB_NEUTRAL, 1, false, false, false, false, false, false, 2, 3, 1053, 2, { { 935, 0, 2, -1 }, { 936, 0, 2, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "pufferfish", 83, 0.7000f, 0.7000f, 0.4550f, 3.0f, 0.7000f, 2.0f, 16.0f, 0.0f, MC_WATER, MB_PASSIVE, 1, false, false, true, false, false, false, 1, 2, 1054, 2, { { 938, 1, 1, -1 }, { 960, 1, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "rabbit", 84, 0.4000f, 0.5000f, 0.4250f, 3.0f, 0.3000f, 3.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 2, 3, 1055, 3, { { 1122, 0, 1, -1 }, { 1118, 1, 1, -1 }, { 1121, 1, 1, -1 }, { -1, 0, 0, -1 } } },
	{ "ravager", 85, 1.9500f, 2.2000f, 1.8700f, 100.0f, 0.3000f, 12.0f, 32.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, false, 1, 2, 1056, 1, { { 765, 1, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "salmon", 86, 0.7000f, 0.4000f, 0.2600f, 3.0f, 0.7000f, 2.0f, 16.0f, 0.0f, MC_WATER, MB_PASSIVE, 1, false, false, true, false, false, false, 3, 6, 1057, 2, { { 936, 1, 1, -1 }, { 960, 1, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "sheep", 87, 0.9000f, 1.3000f, 1.2350f, 8.0f, 0.2300f, 2.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, true, false, 2, 4, 1058, 1, { { 1131, 1, 2, 1132 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "shulker", 88, 1.0000f, 1.0000f, 0.5000f, 30.0f, 0.7000f, 2.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 4, true, false, false, false, false, true, 1, 2, 1059, 1, { { 1164, 1, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "silverfish", 90, 0.4000f, 0.3000f, 0.1300f, 8.0f, 0.2500f, 1.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, false, 1, 2, 1060, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "skeleton", 91, 0.6000f, 1.9900f, 1.7400f, 20.0f, 0.2500f, 2.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 3, false, false, false, false, false, true, 1, 4, 1061, 2, { { 802, 0, 2, -1 }, { 961, 0, 2, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "skeleton_horse", 92, 1.3965f, 1.6000f, 1.5200f, 15.0f, 0.2000f, 2.0f, 32.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 1, 2, 1062, 1, { { 961, 0, 2, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "slime", 93, 0.5200f, 0.5200f, 0.3250f, 20.0f, 0.7000f, 2.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, false, 1, 2, 1063, 1, { { 926, 0, 2, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "sniffer", 95, 1.9000f, 1.7500f, 1.0500f, 14.0f, 0.1000f, 2.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 1, 2, 1064, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "snow_golem", 96, 0.7000f, 1.9000f, 1.7000f, 4.0f, 0.2000f, 2.0f, 16.0f, 0.0f, MC_MISC, MB_PASSIVE, 1, false, false, false, false, false, true, 1, 2, 1065, 1, { { 912, 0, 15, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "spider", 100, 1.4000f, 0.9000f, 0.6500f, 16.0f, 0.3000f, 2.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, false, 1, 4, 1066, 2, { { 850, 0, 2, -1 }, { 1000, 0, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "squid", 101, 0.8000f, 0.8000f, 0.4000f, 10.0f, 0.7000f, 2.0f, 16.0f, 0.0f, MC_WATER, MB_PASSIVE, 1, false, false, true, false, false, false, 1, 2, 1067, 1, { { 941, 1, 3, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "stray", 102, 0.6000f, 1.9900f, 1.7400f, 20.0f, 0.2500f, 2.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, true, 1, 4, 1068, 3, { { 802, 0, 2, -1 }, { 961, 0, 2, -1 }, { 1160, 0, 1, -1 }, { -1, 0, 0, -1 } } },
	{ "strider", 103, 0.9000f, 1.7000f, 1.4450f, 20.0f, 0.1750f, 2.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 2, true, false, false, false, false, false, 1, 2, 1069, 1, { { 850, 2, 5, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "tadpole", 104, 0.4000f, 0.3000f, 0.1950f, 6.0f, 1.0000f, 2.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, true, false, false, false, 1, 2, 1070, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "trader_llama", 108, 0.9000f, 1.8700f, 1.7765f, 20.0f, 0.7000f, 2.0f, 40.0f, 0.0f, MC_CREATURE, MB_NEUTRAL, 1, false, false, false, false, false, false, 1, 2, 1071, 1, { { 913, 0, 2, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "tropical_fish", 110, 0.5000f, 0.4000f, 0.2600f, 3.0f, 0.7000f, 2.0f, 16.0f, 0.0f, MC_WATER, MB_PASSIVE, 1, false, false, true, false, false, false, 3, 6, 1072, 2, { { 937, 1, 1, -1 }, { 960, 1, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "turtle", 111, 1.2000f, 0.4000f, 0.3400f, 30.0f, 0.2500f, 2.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, true, false, false, false, 1, 2, 1073, 2, { { 200, 0, 2, -1 }, { 799, 1, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "vex", 112, 0.4000f, 0.8000f, 0.5188f, 14.0f, 0.7000f, 4.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, true, true, false, false, false, false, 1, 2, 1074, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "villager", 113, 0.6000f, 1.9500f, 1.6200f, 20.0f, 0.5000f, 2.0f, 48.0f, 0.0f, MC_MISC, MB_PASSIVE, 1, false, false, false, false, false, false, 1, 2, 1075, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "vindicator", 114, 0.6000f, 1.9500f, 1.6575f, 24.0f, 0.3500f, 5.0f, 12.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, false, 1, 2, 1076, 1, { { 806, 0, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "wandering_trader", 115, 0.6000f, 1.9500f, 1.6200f, 20.0f, 0.7000f, 2.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 1, 2, 1077, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "warden", 116, 0.9000f, 2.9000f, 2.4650f, 500.0f, 0.3000f, 30.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, true, false, false, false, false, false, 1, 2, 1078, 1, { { 373, 1, 1, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "witch", 118, 0.6000f, 1.9500f, 1.6200f, 26.0f, 0.2500f, 2.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, true, 1, 2, 1079, 4, { { 934, 0, 2, -1 }, { 962, 0, 2, -1 }, { 1000, 0, 2, -1 }, { 999, 0, 2, -1 } } },
	{ "wither", 119, 0.9000f, 3.5000f, 2.9750f, 300.0f, 0.6000f, 2.0f, 40.0f, 4.0f, MC_MONSTER, MB_PASSIVE, 1, true, true, false, false, false, false, 1, 2, 1080, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "wither_skeleton", 120, 0.7000f, 2.4000f, 2.1000f, 20.0f, 0.2500f, 2.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 2, true, false, false, false, false, false, 1, 2, 1081, 3, { { 803, 0, 1, -1 }, { 961, 0, 2, -1 }, { 1104, 1, 1, -1 }, { -1, 0, 0, -1 } } },
	{ "wolf", 122, 0.6000f, 0.8500f, 0.6800f, 8.0f, 0.3000f, 4.0f, 16.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 2, 3, 1082, 0, { { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "zoglin", 123, 1.3965f, 1.4000f, 1.1900f, 40.0f, 0.3000f, 6.0f, 16.0f, 0.0f, MC_MONSTER, MB_HOSTILE, 3, true, false, false, false, false, false, 1, 2, 1083, 1, { { 992, 1, 3, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "zombie", 124, 0.6000f, 1.9500f, 1.7400f, 20.0f, 0.2300f, 3.0f, 35.0f, 2.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, false, 1, 4, 1084, 4, { { 992, 0, 2, -1 }, { 811, 1, 1, -1 }, { 1097, 1, 1, -1 }, { 1098, 1, 1, -1 } } },
	{ "zombie_horse", 125, 1.3965f, 1.6000f, 1.5200f, 15.0f, 0.2000f, 2.0f, 32.0f, 0.0f, MC_CREATURE, MB_PASSIVE, 1, false, false, false, false, false, false, 1, 2, 1085, 1, { { 992, 0, 2, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 }, { -1, 0, 0, -1 } } },
	{ "zombie_villager", 126, 0.6000f, 1.9500f, 1.7400f, 20.0f, 0.2300f, 3.0f, 35.0f, 2.0f, MC_MONSTER, MB_HOSTILE, 1, false, false, false, false, false, false, 1, 2, 1086, 4, { { 992, 0, 2, -1 }, { 811, 1, 1, -1 }, { 1097, 1, 1, -1 }, { 1098, 1, 1, -1 } } },
	{ "zombified_piglin", 127, 0.6000f, 1.9500f, 1.7900f, 20.0f, 0.2300f, 5.0f, 35.0f, 2.0f, MC_MONSTER, MB_NEUTRAL, 2, true, false, false, false, false, false, 1, 2, 1087, 3, { { 992, 0, 1, -1 }, { 996, 0, 1, -1 }, { 815, 1, 1, -1 }, { -1, 0, 0, -1 } } },
};

inline constexpr int MOB_COUNT = (int)(sizeof(MOBS) / sizeof(MOBS[0]));

inline const MobInfo* findMob(const char* name) {
	if (!name) return nullptr;
	for (int i = 0; i < MOB_COUNT; ++i) {
		const char* a = MOBS[i].name; const char* b = name;
		while (*a && *a == *b) { ++a; ++b; }
		if (*a == 0 && *b == 0) return &MOBS[i];
	}
	return nullptr;
}

inline const MobInfo* mobBySpawnEgg(int itemId) {
	if (itemId <= 0) return nullptr;
	for (int i = 0; i < MOB_COUNT; ++i) if (MOBS[i].spawnEgg == itemId) return &MOBS[i];
	return nullptr;
}

inline const MobInfo* mobByNetId(int netId) {
	for (int i = 0; i < MOB_COUNT; ++i) if (MOBS[i].netId == netId) return &MOBS[i];
	return nullptr;
}

} // namespace nc::entity::gen
