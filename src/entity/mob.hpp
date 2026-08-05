#pragma once
// MOBS_ALL_V1 — все мобы 1.21.1 (79 типов: 34 мирных, 11 нейтральных, 34 враждебных).
// Раньше здесь был enum на 4 животных с ручными цифрами. Теперь всё берётся из таблицы
// mobs.gen.hpp, сгенерированной из исходников игры:
//   • EntityType.java        — id, хитбокс, высота глаз, MobCategory, fireImmune
//   • DefaultAttributes.java + Attributes.java — HP, скорость, атака, броня, радиус агро
//   • loot_table/entities/*.json — дроп с диапазонами и вариантом "жареное"
//   • reports/registries.json — id сущностей и id яиц спавна
// Полный goal-ИИ (пасфайндинг, размножение, стайность) — следующий заход.
#include "../core/types.hpp"
#include "mobs.gen.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include <cstring>

namespace nc::entity {

using MobInfo = gen::MobInfo;

inline i32 mobTypeCount() { return gen::MOB_COUNT; }

inline const MobInfo& mobDef(i32 idx) {
    if (idx < 0 || idx >= gen::MOB_COUNT) idx = 0;
    return gen::MOBS[idx];
}
inline i32 mobIndexByName(const char* name) {
    if (!name) return -1;
    for (i32 i = 0; i < gen::MOB_COUNT; ++i)
        if (std::strcmp(gen::MOBS[i].name, name) == 0) return i;
    return -1;
}
inline i32 mobIndexBySpawnEgg(i32 itemId) {
    if (itemId <= 0) return -1;
    for (i32 i = 0; i < gen::MOB_COUNT; ++i)
        if (gen::MOBS[i].spawnEgg == itemId) return i;
    return -1;
}

// Размер группы при спавне (SpawnPlacements: скот стадами, монстры по 1-4).
inline i32 mobGroupSize(i32 idx, i32 rnd) {
    const MobInfo& d = mobDef(idx);
    const i32 span = (i32)d.groupMax - (i32)d.groupMin + 1;
    return (i32)d.groupMin + (span > 1 ? (rnd % span) : 0);
}

// ---------- Дроп ----------
struct MobDropEntry { i32 itemId; i32 min, max; i32 cookedId; };

// id предметов для взаимодействий (дроп берётся из таблицы).
enum : i32 {
    ITEM_LEATHER = 913, ITEM_BEEF = 988, ITEM_COOKED_BEEF = 989,
    ITEM_PORKCHOP = 881, ITEM_COOKED_PORKCHOP = 882,
    ITEM_MUTTON = 1131, ITEM_COOKED_MUTTON = 1132,
    ITEM_CHICKEN = 990, ITEM_COOKED_CHICKEN = 991, ITEM_FEATHER = 851,
    ITEM_EGG = 927, ITEM_WHITE_WOOL = 202,
    ITEM_BUCKET = 908, ITEM_MILK_BUCKET = 914, ITEM_SHEARS = 983,
    ITEM_WHEAT = 854, ITEM_WHEAT_SEEDS = 853, ITEM_CARROT = 1097,
    ITEM_POTATO = 1098, ITEM_BEETROOT = 1154, ITEM_BEETROOT_SEEDS = 1155,
    ITEM_MELON_SEEDS = 987, ITEM_PUMPKIN_SEEDS = 986,
    ITEM_BONE = 961, ITEM_COD_RAW = 935, ITEM_GOLD_INGOT = 815,
    ITEM_STRING = 850, ITEM_GRAVEL = 61, ITEM_ENDER_PEARL = 993,
    ITEM_OBSIDIAN = 290, ITEM_NETHER_WART = 997, ITEM_SPIDER_EYE = 1000,
    ITEM_FIRE_CHARGE = 1089, ITEM_SOUL_SAND = 326, ITEM_CRYING_OBSIDIAN = 1227,
    ITEM_IRON_NUGGET = 1165, ITEM_LEATHER_ITEM = 913,
    // BREED_FOOD_V1: корм для остальных видов
    ITEM_GOLDEN_CARROT = 1102, ITEM_GOLDEN_APPLE = 884, ITEM_ENCHANTED_GOLDEN_APPLE = 885,
    ITEM_HAY_BLOCK = 445, ITEM_SUGAR = 962, ITEM_APPLE = 800,
    ITEM_SWEET_BERRIES = 1216, ITEM_GLOW_BERRIES = 1217, ITEM_SEAGRASS = 200,
    ITEM_BAMBOO = 251, ITEM_CRIMSON_FUNGUS = 236, ITEM_WARPED_FUNGUS = 237,
    ITEM_KELP = 244, ITEM_DANDELION = 218, ITEM_CACTUS = 308,
    ITEM_SALMON_RAW = 936, ITEM_TROPICAL_FISH = 937, ITEM_SLIME_BALL = 926,
    ITEM_TORCHFLOWER_SEEDS = 1152, ITEM_HONEYCOMB = 1221, ITEM_SPIDER_EYE_ITEM = 1000
};

// TAME_V1: предмет приручения для конкретного вида (Wolf.java / Cat.java isFood для приручения).
inline i32 mobTameItem(i32 idx) {
    const char* n = mobDef(idx).name;
    if (std::strcmp(n, "wolf") == 0) return ITEM_BONE;
    if (std::strcmp(n, "cat") == 0 || std::strcmp(n, "ocelot") == 0) return ITEM_COD_RAW;
    return -1;
}

// BARTER_V1: PiglinBarterLoot — упрощённый пул предметов за золотой слиток.
inline i32 mobBarterLoot(i32 rnd) {
    static const i32 pool[] = { ITEM_STRING, ITEM_GRAVEL, ITEM_ENDER_PEARL, ITEM_OBSIDIAN,
        ITEM_NETHER_WART, ITEM_SPIDER_EYE, ITEM_FIRE_CHARGE, ITEM_SOUL_SAND,
        ITEM_CRYING_OBSIDIAN, ITEM_IRON_NUGGET, ITEM_LEATHER_ITEM };
    return pool[static_cast<u32>(rnd) % (sizeof(pool) / sizeof(pool[0]))];
}

inline std::vector<MobDropEntry> mobDrops(i32 idx) {
    std::vector<MobDropEntry> out;
    const MobInfo& d = mobDef(idx);
    for (i32 i = 0; i < (i32)d.dropCount && i < 4; ++i) {
        const auto& e = d.drops[i];
        if (e.item < 0) continue;
        out.push_back({ (i32)e.item, (i32)e.lo, (i32)e.hi, (i32)e.cooked });
    }
    return out;
}
inline i32 cookedVariant(i32 rawId) {
    switch (rawId) {
        case ITEM_BEEF:     return ITEM_COOKED_BEEF;
        case ITEM_PORKCHOP: return ITEM_COOKED_PORKCHOP;
        case ITEM_MUTTON:   return ITEM_COOKED_MUTTON;
        case ITEM_CHICKEN:  return ITEM_COOKED_CHICKEN;
        default:            return rawId;
    }
}

// Приманка/корм — isFood() из декомпила.
inline bool mobLikesItem(i32 idx, i32 itemId) {
    const char* n = mobDef(idx).name;
    if (std::strcmp(n, "cow") == 0 || std::strcmp(n, "mooshroom") == 0 ||
        std::strcmp(n, "sheep") == 0 || std::strcmp(n, "goat") == 0)
        return itemId == ITEM_WHEAT;
    if (std::strcmp(n, "pig") == 0)
        return itemId == ITEM_CARROT || itemId == ITEM_POTATO || itemId == ITEM_BEETROOT;
    if (std::strcmp(n, "chicken") == 0)
        return itemId == ITEM_WHEAT_SEEDS || itemId == ITEM_BEETROOT_SEEDS
            || itemId == ITEM_MELON_SEEDS || itemId == ITEM_PUMPKIN_SEEDS;
    // BREED_FOOD_V1: остальные виды — по Animal.isFood() из ванильных исходников 1.21.1.
    if (std::strcmp(n, "wolf") == 0)
        return itemId == ITEM_BEEF || itemId == ITEM_COOKED_BEEF
            || itemId == ITEM_PORKCHOP || itemId == ITEM_COOKED_PORKCHOP
            || itemId == ITEM_CHICKEN || itemId == ITEM_COOKED_CHICKEN
            || itemId == ITEM_MUTTON || itemId == ITEM_COOKED_MUTTON;
    if (std::strcmp(n, "cat") == 0 || std::strcmp(n, "ocelot") == 0)
        return itemId == ITEM_COD_RAW || itemId == ITEM_SALMON_RAW;
    if (std::strcmp(n, "horse") == 0 || std::strcmp(n, "donkey") == 0 ||
        std::strcmp(n, "mule") == 0)
        return itemId == ITEM_GOLDEN_CARROT || itemId == ITEM_GOLDEN_APPLE
            || itemId == ITEM_ENCHANTED_GOLDEN_APPLE || itemId == ITEM_HAY_BLOCK;
    if (std::strcmp(n, "llama") == 0 || std::strcmp(n, "trader_llama") == 0)
        return itemId == ITEM_HAY_BLOCK;
    if (std::strcmp(n, "camel") == 0) return itemId == ITEM_CACTUS;
    if (std::strcmp(n, "rabbit") == 0)
        return itemId == ITEM_CARROT || itemId == ITEM_GOLDEN_CARROT || itemId == ITEM_DANDELION;
    if (std::strcmp(n, "turtle") == 0) return itemId == ITEM_SEAGRASS;
    if (std::strcmp(n, "panda") == 0) return itemId == ITEM_BAMBOO;
    if (std::strcmp(n, "fox") == 0) return itemId == ITEM_SWEET_BERRIES || itemId == ITEM_GLOW_BERRIES;
    if (std::strcmp(n, "bee") == 0) return itemId == ITEM_DANDELION;
    if (std::strcmp(n, "hoglin") == 0) return itemId == ITEM_CRIMSON_FUNGUS;
    if (std::strcmp(n, "strider") == 0) return itemId == ITEM_WARPED_FUNGUS;
    if (std::strcmp(n, "axolotl") == 0) return itemId == ITEM_TROPICAL_FISH;
    if (std::strcmp(n, "frog") == 0) return itemId == ITEM_SLIME_BALL;
    if (std::strcmp(n, "sniffer") == 0) return itemId == ITEM_TORCHFLOWER_SEEDS;
    if (std::strcmp(n, "armadillo") == 0) return itemId == ITEM_SPIDER_EYE_ITEM;
    return false;
}

// SUN_BURN_V1: Zombie.isSunBurnTick() — горят на дневном солнце только часть нежити.
// Husk, drowned, свинозомби, иссушенные скелеты и зоглины не горят.
inline bool mobBurnsInSun(const char* n) {
    return std::strcmp(n, "zombie") == 0 || std::strcmp(n, "zombie_villager") == 0
        || std::strcmp(n, "skeleton") == 0 || std::strcmp(n, "stray") == 0
        || std::strcmp(n, "bogged") == 0 || std::strcmp(n, "phantom") == 0;
}

// ---------- VILLAGER_TRADE_V1 ----------
// Предметы, которые нужны только для сделок жителей (id из items.gen.hpp).
enum : i32 {
    ITEM_EMERALD = 806, ITEM_BREAD = 855, ITEM_PAPER = 924, ITEM_BOOK = 925,
    ITEM_COAL = 803, ITEM_IRON_INGOT = 811, ITEM_ARROW = 802, ITEM_STICK = 848,
    ITEM_ROTTEN_FLESH = 992, ITEM_LAPIS_LAZULI = 807, ITEM_REDSTONE_DUST = 657,
    ITEM_GLOWSTONE_DUST = 934, ITEM_DIAMOND = 805, ITEM_COMPASS_ITEM = 928
};

// VillagerProfession.java: у нас шесть рабочих профессий, безработный не торгует.
enum : i32 {
    VP_NONE = 0, VP_FARMER, VP_LIBRARIAN, VP_TOOLSMITH,
    VP_BUTCHER, VP_CLERIC, VP_FLETCHER, VP_COUNT
};

inline const char* villagerProfessionName(i32 profession) {
    switch (profession) {
        case VP_FARMER:     return "Фермер";
        case VP_LIBRARIAN:  return "Библиотекарь";
        case VP_TOOLSMITH:  return "Кузнец-инструментальщик";
        case VP_BUTCHER:    return "Мясник";
        case VP_CLERIC:     return "Священник";
        case VP_FLETCHER:   return "Лучник";
        default:            return "Безработный";
    }
}

// MerchantOffer.java: до двух входных предметов и один выходной.
// buy2Id == 0 — второго входа нет (у ванильных сделок он опционален).
struct VillagerOffer {
    i32 buyId; i32 buyCount;
    i32 buy2Id; i32 buy2Count;
    i32 sellId; i32 sellCount;
    i32 maxUses; i32 xp;
};

// VillagerTrades.java — упрощённый пул новичка (уровень 1-2) по каждой профессии.
// Полная лесенка уровней с разблокировкой — следующий заход.
inline std::vector<VillagerOffer> villagerOffers(i32 profession) {
    std::vector<VillagerOffer> out;
    switch (profession) {
        case VP_FARMER:
            out.push_back({ ITEM_WHEAT, 20, 0, 0, ITEM_EMERALD, 1, 16, 2 });
            out.push_back({ ITEM_POTATO, 26, 0, 0, ITEM_EMERALD, 1, 16, 2 });
            out.push_back({ ITEM_EMERALD, 1, 0, 0, ITEM_BREAD, 6, 16, 1 });
            break;
        case VP_LIBRARIAN:
            out.push_back({ ITEM_PAPER, 24, 0, 0, ITEM_EMERALD, 1, 16, 2 });
            out.push_back({ ITEM_BOOK, 4, 0, 0, ITEM_EMERALD, 1, 12, 5 });
            out.push_back({ ITEM_EMERALD, 4, 0, 0, ITEM_COMPASS_ITEM, 1, 12, 10 });
            break;
        case VP_TOOLSMITH:
            out.push_back({ ITEM_COAL, 15, 0, 0, ITEM_EMERALD, 1, 16, 2 });
            out.push_back({ ITEM_IRON_INGOT, 4, 0, 0, ITEM_EMERALD, 1, 12, 10 });
            out.push_back({ ITEM_DIAMOND, 1, 0, 0, ITEM_EMERALD, 3, 12, 20 });
            break;
        case VP_BUTCHER:
            out.push_back({ ITEM_CHICKEN, 14, 0, 0, ITEM_EMERALD, 1, 16, 2 });
            out.push_back({ ITEM_PORKCHOP, 7, 0, 0, ITEM_EMERALD, 1, 16, 2 });
            out.push_back({ ITEM_EMERALD, 1, 0, 0, ITEM_COOKED_PORKCHOP, 5, 16, 1 });
            break;
        case VP_CLERIC:
            out.push_back({ ITEM_ROTTEN_FLESH, 32, 0, 0, ITEM_EMERALD, 1, 16, 2 });
            out.push_back({ ITEM_EMERALD, 1, 0, 0, ITEM_REDSTONE_DUST, 4, 16, 1 });
            out.push_back({ ITEM_EMERALD, 1, 0, 0, ITEM_LAPIS_LAZULI, 1, 16, 1 });
            out.push_back({ ITEM_EMERALD, 4, 0, 0, ITEM_GLOWSTONE_DUST, 1, 12, 5 });
            break;
        case VP_FLETCHER:
            out.push_back({ ITEM_STICK, 32, 0, 0, ITEM_EMERALD, 1, 16, 2 });
            // Двухвходовая сделка: гравий + изумруд за стрелы (ванильный обмен лучника).
            out.push_back({ ITEM_GRAVEL, 10, ITEM_EMERALD, 1, ITEM_ARROW, 10, 12, 5 });
            out.push_back({ ITEM_EMERALD, 1, 0, 0, ITEM_ARROW, 16, 16, 1 });
            break;
        default:
            break;
    }
    return out;
}

// ---------- Экземпляр моба ----------
struct Mob {
    i32 eid = 0;
    i32 typeIdx = 0;            // индекс в gen::MOBS
    i32 dimension = 0;
    f64 x = 0, y = 0, z = 0;
    f64 vx = 0, vy = 0, vz = 0;
    f32 yaw = 0, pitch = 0, headYaw = 0;
    i32 health = 10;
    bool onGround = true;
    bool baby = false;
    bool dead = false;
    i32 deathTimer = 0;         // тики анимации смерти (ванильные 20)
    i32 hurtCooldown = 0;       // invulnerableTime, 10 тиков
    i32 wool = 0;
    bool sheared = false;
    i32 eggTimer = 0;
    i32 strollTimer = 0;
    i32 panicTimer = 0;
    i32 targetEid = 0;          // MOBS_ALL_V1: цель враждебного/разозлённого моба
    i32 attackCooldown = 0;     // 20 тиков между ударами
    i32 angryTimer = 0;         // нейтральные злятся 400 тиков после удара
    // MOBS_AI_V1: состояние способностей
    i32 fuseTimer = -1;         // крипер: тики запала (-1 — не подожжён)
    i32 shootCooldown = 0;      // стрелки: перезарядка
    i32 leapCooldown = 0;       // паук: прыжок к цели
    i32 tpCooldown = 0;         // эндермен: телепорт из-под удара
    // MOB_GOALS_V2: устойчивое состояние goal-ИИ, не одноразовый "беги к игроку".
    // panicX/Z — источник последнего удара; loveTicks/breedCooldown — Animal#inLove.
    i32 targetMemory = 0;        // сохраняем последнюю видимую цель за препятствием
    i32 loveTicks = 0;
    i32 breedCooldown = 0;
    f64 panicX = 0, panicZ = 0;
    // TAME_V1 / BARTER_V1 / SPLIT_V1: приручение волка/кошки, бартер пиглина, стингер пчелы.
    i32 owner = -1;               // eid игрока-владельца, -1 = не приручён
    bool tamed = false;
    i32 specialTimer = 0;         // у пиглина — кулдаун бартера после сделки
    i32 stingTimer = 0;
    bool sitting = false;       // WOLF_SIT_V1: приручённый питомец посажен командой хозяина         // BEE_STING_V1: пчела ужалила, теряет жало и скоро умрёт (Bee.java: hasStung)
    i32 fireTimer = 0;          // SUN_BURN_V1: тики горения на солнце
    i32 skyCheckTimer = 0;      // SUN_BURN_V1: редкая проверка открытого неба            // BEE_STING_V1: пчела ужалила, теряет жало и скоро умрёт (Bee.java: hasStung)
    // VILLAGER_TRADE_V1: профессия и накопленный опыт торговли (VillagerData).
    i32 profession = 0;
    i32 tradeUses = 0;
    i32 tradeXp = 0;
    // RAID_WAVES_V1: рейдер помнит номер рейда и волны, чтобы Raid.java знал,
    // когда волна зачищена и можно поднимать следующую.
    i32 raidId = 0;
    i32 raidWave = 0;
    // WARDEN_VIBRATION_V1: варден слеп, цель берётся по уровню гнева от вибраций,
    // digTimer считает тики тишины до закапывания (WardenAi.DIG).
    i32 wardenAnger = 0;
    i32 digTimer = 0;
    // ZOMBIE_CONVERT_V1: таймер превращения (лечение зомби-жителя золотым яблоком)
    // и тип, в который моб превратится по его истечении.
    i32 convertTimer = 0;
    i32 convertTo = -1;
    i32 beamTarget = 0;   // GUARDIAN_BEAM_V2: eid цели луча стража (0 = луча нет)
    i32 potionKind = 0;   // WITCH_POTION_V2: тип последнего зелья ведьмы
    // DROWNED_CONVERT_V1: тики подряд с головой под водой (Zombie.conversionTime).
    i32 waterTimer = 0;
    // LLAMA_CARAVAN_V1: eid ведущего каравана, за которым идёт лама (0 — сама по себе).
    i32 caravanLeader = 0;
    // VILLAGER_RESTOCK_V1: тики до очередного пополнения запаса сделок.
    i32 restockTimer = 0;
    f64 lastSentX = 0, lastSentY = 0, lastSentZ = 0;
    f32 lastSentYaw = 0;

    const MobInfo& def() const { return mobDef(typeIdx); }
    bool isKind(const char* name) const { return std::strcmp(def().name, name) == 0; }
    bool hostile() const { return def().behavior == gen::MB_HOSTILE; }
    bool neutral() const { return def().behavior == gen::MB_NEUTRAL; }
    bool aquatic() const { return def().aquatic; }
    bool flying() const { return def().flying; }
    bool ranged() const { return def().ranged; }   // MOBS_AI_V1
};

// ---------- Физика ----------
struct MobPhysicsEnv {
    bool (*solidAt)(void* ctx, i32 x, i32 y, i32 z) = nullptr;
    bool (*waterAt)(void* ctx, i32 x, i32 y, i32 z) = nullptr;
    void* ctx = nullptr;
    bool solid(i32 x, i32 y, i32 z) const { return solidAt ? solidAt(ctx, x, y, z) : false; }
    bool water(i32 x, i32 y, i32 z) const { return waterAt ? waterAt(ctx, x, y, z) : false; }
};

inline bool mobBlocked(const MobPhysicsEnv& env, const Mob& m, f64 nx, f64 nz) {
    const f32 half = m.def().width * 0.5f;
    const i32 y0 = (i32)std::floor(m.y);
    const i32 y1 = (i32)std::floor(m.y + m.def().height - 0.1);
    for (i32 y = y0; y <= y1; ++y)
        for (f64 ox : { -(f64)half, (f64)half })
            for (f64 oz : { -(f64)half, (f64)half })
                if (env.solid((i32)std::floor(nx + ox), y, (i32)std::floor(nz + oz))) return true;
    return false;
}

// Один тик: гравитация (или полёт/плавание), шаг с подъёмом на блок, трение.
inline void mobPhysicsStep(const MobPhysicsEnv& env, Mob& m) {
    const i32 bx = (i32)std::floor(m.x), by = (i32)std::floor(m.y), bz = (i32)std::floor(m.z);
    const bool inWater = env.water(bx, by, bz);

    if (m.flying()) {
        // Летающие: без гравитации, держатся над землёй
        m.vy *= 0.9;
        if (env.solid(bx, by - 1, bz) || env.solid(bx, by - 2, bz)) m.vy += 0.04;
        else m.vy -= 0.01;
        if (m.vy > 0.3) m.vy = 0.3;
        if (m.vy < -0.3) m.vy = -0.3;
        m.y += m.vy;
        m.onGround = false;
    } else if (inWater && m.aquatic()) {
        // водные: почти нейтральная плавучесть, из воздуха тянет вниз
        m.vy = m.vy * 0.8 + (env.water(bx, by + 1, bz) ? 0.0 : -0.02);
        m.y += m.vy;
        m.onGround = false;
    } else {
        m.vy = (m.vy - 0.08) * 0.98;
        if (inWater) m.vy = m.vy * 0.5 + 0.02;   // наземные выныривают
        if (m.vy < -3.0) m.vy = -3.0;
        f64 ny = m.y + m.vy;
        if (m.vy < 0) {
            const i32 groundY = (i32)std::floor(ny);
            if (env.solid(bx, groundY, bz)) { ny = groundY + 1.0; m.vy = 0; m.onGround = true; }
            else m.onGround = false;
        } else m.onGround = false;
        m.y = ny;
    }

    const f64 nx = m.x + m.vx, nz = m.z + m.vz;
    if (!mobBlocked(env, m, nx, nz)) {
        m.x = nx; m.z = nz;
    } else if (m.onGround && !env.solid((i32)std::floor(nx), (i32)std::floor(m.y) + 1, (i32)std::floor(nz))
                          && !env.solid((i32)std::floor(nx), (i32)std::floor(m.y) + 2, (i32)std::floor(nz))) {
        m.y += 1.0; m.x = nx; m.z = nz;   // ступенька в блок
    } else {
        m.vx = -m.vx; m.vz = -m.vz;       // упёрся — разворот
    }
    // MOBFRIC_V1: раньше трение 0.6 применялось и в полёте: отброшенный моб
    // терял скорость за 2 тика, а клиент, получив Set Entity Motion, летел дальше
    // со своим 0.91 — потом его тянуло назад пакетами позиции, что и выглядело
    // катанием по льду. В ванилле воздушное сопротивление 0.91, а на земле
    // скользкость блока 0.6 умножается на то же 0.91 = 0.546.
    f64 fric;
    if (m.flying() || (inWater && m.aquatic())) fric = 0.91;
    else if (m.onGround) fric = 0.6 * 0.91;
    else fric = 0.91;
    m.vx *= fric; m.vz *= fric;
    // MOBSPEEDCAP_V1: с ванильным воздушным трением 0.91 любая ошибка ИИ разгоняет
    // моба до абсурда. В ванилле быстрейшие мобы идут ~0.25 блока за тик,
    // а отброс от удара даёт 0.4 — выше этого горизонтально быть нечему.
    const f64 hsp = std::sqrt(m.vx * m.vx + m.vz * m.vz);
    if (hsp > 0.42) { m.vx = m.vx / hsp * 0.42; m.vz = m.vz / hsp * 0.42; }
}

} // namespace nc::entity
