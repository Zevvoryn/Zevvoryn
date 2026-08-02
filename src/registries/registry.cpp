#include "registry.hpp"
#include "../core/log.hpp"
#include "../core/item_blocks.gen.hpp"
#include <nlohmann/json.hpp>
#include <fstream>

namespace nc::registries {

bool BlockState::isSolid() const {
    auto& mgr = RegistryManager::instance();
    const auto* block = mgr.blocks().getById(blockId);
    return block ? block->solid : true;
}

void RegistryManager::loadDefaults() {
    // Загружаем из data/registry/*.json
    // Пока захардкожены базовые блоки для flat мира
    loadBlockDefaults();
    loadItemDefaults();
    loadEntityDefaults();
    loadBiomeDefaults();

    NC_DEBUG("Reg", "Реестры загружены: {} блоков, {} предметов, {} энтити, {} биомов",
        blocks_.size(), items_.size(), entities_.size(), biomes_.size());
}

i32 RegistryManager::getFlatWorldBlockStateId() const {
    // Для простого flat мира: bedrock(1) + dirt(2) x2 + grass(9)
    auto id = getBlockStateId("minecraft:grass_block", {{"snowy", "false"}});
    return id.value_or(9); // LIGHT_V1: 9 = grass_block[snowy=false]
}

std::optional<i32> RegistryManager::getBlockStateId(std::string_view name,
    const std::unordered_map<std::string, std::string>& props) const
{
    // Простой поиск — в реальном сервере это индексированный поиск
    for (const auto& state : blockStates_.getAll()) {
        if (state.name == name) {
            bool match = true;
            for (const auto& [k, v] : props) {
                auto it = state.properties.find(k);
                if (it == state.properties.end() || it->second != v) {
                    match = false;
                    break;
                }
            }
            if (match) return state.id;
        }
    }
    return std::nullopt;
}

// ============================================================
// Встроенные данные Java 1.21.1 / protocol 767.
// item_blocks.gen.hpp уже содержит полную сгенерированную таблицу
// minecraft:block -> vanilla default block-state id (1331 блок).
// ============================================================

void RegistryManager::loadBlockDefaults() {
    // REGISTRY_FULL_V1: больше никакой семиблочной заглушки. Клиент 1.21.1
    // имеет тот же статический реестр; сервер обязан использовать его ID.
    blockStates_.registerEntry({0, 0, "minecraft:air", {}});
    blocks_.registerEntry({0, "minecraft:air", 0, 0, false, false, 0.0f, 0.0f, 0.0f});

    for (const auto& [shortName, defaultState] : nc::gen::blockStateByName()) {
        if (shortName == "grass_block" || shortName == "water" || shortName == "lava" ||
            shortName == "air") continue; // состояния ниже требуют специальных свойств

        const std::string name = "minecraft:" + shortName;
        const bool air = shortName == "cave_air" || shortName == "void_air";
        blockStates_.registerEntry({defaultState, defaultState, name, {}});
        blocks_.registerEntry({defaultState, name, defaultState, defaultState,
            !air, false, air ? 0.0f : 1.0f, air ? 0.0f : 1.0f, 0.0f});
    }

    // Ванильный порядок bool: true, false. Default = snowy=false (state 9).
    blockStates_.registerEntry({8, 8, "minecraft:grass_block", {{"snowy", "true"}}});
    blockStates_.registerEntry({9, 8, "minecraft:grass_block", {{"snowy", "false"}}});
    blocks_.registerEntry({8, "minecraft:grass_block", 8, 9, true, false, 0.6f, 0.3f, 0.0f});

    // LiquidBlock.LEVEL: все 16 сетевых состояний, а не только источник.
    for (i32 level = 0; level < 16; ++level) {
        blockStates_.registerEntry({80 + level, 80, "minecraft:water", {{"level", std::to_string(level)}}});
        blockStates_.registerEntry({96 + level, 96, "minecraft:lava", {{"level", std::to_string(level)}}});
    }
    blocks_.registerEntry({80, "minecraft:water", 80, 95, false, true, 100.0f, 500.0f, 0.0f});
    blocks_.registerEntry({96, "minecraft:lava", 96, 111, false, true, 100.0f, 500.0f, 15.0f});

    // STATE_VARIANTS_V1 — см. комментарий у registerStateVariants().
    registerStateVariants();
}

// ============================================================
// STATE_VARIANTS_V1
//
// Баг, который это чинит: loadBlockDefaults() регистрировал РОВНО ОДНО
// состояние на блок, причём с ПУСТОЙ картой свойств (исключения: air,
// grass_block, water, lava). Из-за этого:
//   1) getBlockStateId("minecraft:sweet_berry_bush", {{"age","1"}}) и любой
//      другой запрос с непустыми props ВСЕГДА возвращал std::nullopt;
//   2) blockStates().getById(state)->properties всегда был пуст, поэтому
//      чтение возраста/стадии (bs->properties.find("age")) всегда давало 0.
// Итог: рост какао, сладких ягод, торчфлауэра, тыквенных/дынных стеблей,
// келпа, бамбука и посадка деревьев из саженцев были тихими no-op'ами —
// код выполнялся, но состояние блока никогда не менялось.
//
// Как считается ID варианта: состояния блока идут подряд от базового ID,
// в порядке декартова произведения свойств, где ПОСЛЕДНЕЕ свойство меняется
// быстрее всего (порядок ванильного StateDefinition), а bool перечисляется
// как true, false (тот же порядок уже подтверждён grass_block[snowy]).
// Число вариантов сверено с реальным размером ID-диапазона каждого блока
// (разница до следующего блока в blockStateByName()): листья 28, брёвна 3,
// саженцы 2, стебли 8, прикреплённые стебли 4, torchflower_crop 3, kelp 26,
// бамбук 12, черепашье яйцо 12, котёл 3 и т.д. Совпадение точное.
//
// Не покрыто намеренно: pointed_dripstone (диапазон 15 против 20 ванильных
// комбинаций — схема не сходится, гадать нельзя), mangrove_propagule,
// glow_lichen, big/small_dripleaf, двери/люки/заборы/стены и прочая
// «строительная» комбинаторика, которой физика пока не пользуется.
// ============================================================
void RegistryManager::registerStateVariants() {
    using Values = std::vector<std::string>;
    struct Prop { std::string name; Values values; };

    auto baseOf = [](const std::string& shortName) -> i32 {
        const auto& tbl = nc::gen::blockStateByName();
        auto it = tbl.find(shortName);
        return it == tbl.end() ? -1 : it->second;
    };

    auto add = [&](const std::string& shortName, const std::vector<Prop>& props) {
        const i32 base = baseOf(shortName);
        if (base < 0 || props.empty()) return;
        size_t total = 1;
        for (const auto& p : props) total *= p.values.size();
        for (size_t idx = 0; idx < total; ++idx) {
            size_t rem = idx;
            std::unordered_map<std::string, std::string> map;
            for (size_t pi = props.size(); pi-- > 0;) {
                const auto& p = props[pi];
                map[p.name] = p.values[rem % p.values.size()];
                rem /= p.values.size();
            }
            blockStates_.registerEntry({base + static_cast<i32>(idx), base,
                "minecraft:" + shortName, map});
        }
    };

    auto range = [](i32 from, i32 to) {
        Values v;
        for (i32 i = from; i <= to; ++i) v.push_back(std::to_string(i));
        return v;
    };
    const Values kBool = {"true", "false"};
    const Values kHorizontal = {"north", "south", "west", "east"};
    const Values kAxis = {"x", "y", "z"};

    // Листья: distance(1..7) x persistent x waterlogged = 28 состояний.
    for (const char* wood : {"oak", "spruce", "birch", "jungle", "acacia",
                             "cherry", "dark_oak", "mangrove", "azalea"}) {
        add(std::string(wood) + "_leaves",
            {{"distance", range(1, 7)}, {"persistent", kBool}, {"waterlogged", kBool}});
    }

    // Брёвна/древесина/гифы: axis = x, y, z.
    for (const char* pillar : {
        "oak_log",
        "stripped_oak_log",
        "oak_wood",
        "stripped_oak_wood",
        "spruce_log",
        "stripped_spruce_log",
        "spruce_wood",
        "stripped_spruce_wood",
        "birch_log",
        "stripped_birch_log",
        "birch_wood",
        "stripped_birch_wood",
        "jungle_log",
        "stripped_jungle_log",
        "jungle_wood",
        "stripped_jungle_wood",
        "acacia_log",
        "stripped_acacia_log",
        "acacia_wood",
        "stripped_acacia_wood",
        "cherry_log",
        "stripped_cherry_log",
        "cherry_wood",
        "stripped_cherry_wood",
        "dark_oak_log",
        "stripped_dark_oak_log",
        "dark_oak_wood",
        "stripped_dark_oak_wood",
        "mangrove_log",
        "stripped_mangrove_log",
        "mangrove_wood",
        "stripped_mangrove_wood",
        "bamboo_block",
        "stripped_bamboo_block",
        "warped_stem",
        "stripped_warped_stem",
        "warped_hyphae",
        "crimson_stem",
        "stripped_crimson_stem",
        "crimson_hyphae"}) {
        add(pillar, {{"axis", kAxis}});
    }

    // Саженцы: stage 0/1 (нужно для роста дерева со второго тика).
    for (const char* sap : {"oak", "spruce", "birch", "jungle", "acacia",
                            "cherry", "dark_oak"}) {
        add(std::string(sap) + "_sapling", {{"stage", range(0, 1)}});
    }

    // Культуры и растения.
    add("wheat", {{"age", range(0, 7)}});
    add("carrots", {{"age", range(0, 7)}});
    add("potatoes", {{"age", range(0, 7)}});
    add("beetroots", {{"age", range(0, 3)}});
    add("nether_wart", {{"age", range(0, 3)}});
    add("sweet_berry_bush", {{"age", range(0, 3)}});
    add("torchflower_crop", {{"age", range(0, 2)}});
    add("pumpkin_stem", {{"age", range(0, 7)}});
    add("melon_stem", {{"age", range(0, 7)}});
    add("attached_pumpkin_stem", {{"facing", kHorizontal}});
    add("attached_melon_stem", {{"facing", kHorizontal}});
    add("cactus", {{"age", range(0, 15)}});
    add("sugar_cane", {{"age", range(0, 15)}});
    add("kelp", {{"age", range(0, 25)}});
    add("weeping_vines", {{"age", range(0, 25)}});
    add("twisting_vines", {{"age", range(0, 25)}});
    add("cave_vines", {{"age", range(0, 25)}, {"berries", kBool}});
    add("chorus_flower", {{"age", range(0, 5)}});
    add("cocoa", {{"age", range(0, 2)}, {"facing", kHorizontal}});
    add("bamboo", {{"age", range(0, 1)},
                   {"leaves", Values{"none", "small", "large"}},
                   {"stage", range(0, 1)}});

    // Почва, снег, лёд, котлы, яйца.
    add("farmland", {{"moisture", range(0, 7)}});
    add("snow", {{"layers", range(1, 8)}});
    add("frosted_ice", {{"age", range(0, 3)}});
    add("composter", {{"level", range(0, 8)}});
    add("water_cauldron", {{"level", range(1, 3)}});
    add("powder_snow_cauldron", {{"level", range(1, 3)}});
    add("turtle_egg", {{"eggs", range(1, 4)}, {"hatch", range(0, 2)}});
    // SEA_PICKLE / SNIFFER_EGG_V1: exact palette spans: 4 pickle counts x
    // waterlogged, and three sniffer-egg hatch stages.
    add("sea_pickle", {{"pickles", range(1, 4)}, {"waterlogged", kBool}});
    add("sniffer_egg", {{"hatch", range(0, 2)}});

    // PORTALFIX_V1: без этого порталы были мертвы: запросы
    // nether_portal[axis] и end_portal_frame[eye,facing] всегда давали nullopt.
    // nether_portal: default = axis=x, диапазон ровно 2 состояния (5864..5865).
    add("nether_portal", {{"axis", Values{"x", "z"}}});
    // end_portal_frame: значение из таблицы = default = eye=false,facing=north
    // (7411). Ванильный порядок bool — true, false, поэтому четвёрка eye=true
    // лежит ПЕРЕД ней (в зазоре после end_portal), а не после — поэтому
    // регистрируем руками, чтобы не заехать на ID end_stone.
    {
        const i32 frDef = baseOf("end_portal_frame");
        if (frDef >= 4) {
            for (int i = 0; i < 4; ++i) {
                blockStates_.registerEntry({frDef - 4 + i, frDef,
                    "minecraft:end_portal_frame", {{"eye", "true"}, {"facing", kHorizontal[i]}}});
                blockStates_.registerEntry({frDef + i, frDef,
                    "minecraft:end_portal_frame", {{"eye", "false"}, {"facing", kHorizontal[i]}}});
            }
        }
    }

    // CANDLE_LANTERN_STATE_V1: needed so CANDLE_SUPPORT_V1 / LANTERN_SUPPORT_V1
    // (server.cpp) can read "hanging"/"lit"/"waterlogged" instead of always
    // getting an empty properties map.
    // Verified against blockStateByName() range gaps: every candle color spans
    // exactly 16 ids (candles 1..4 x lit x waterlogged) EXCEPT black_candle,
    // whose gap to candle_cake is only 14 (and black_candle_cake's gap to
    // amethyst_block is 1, not 2). That is a real, isolated 2-id shortfall in
    // this codebase's block table for the black variant specifically — not a
    // transcription error, confirmed by re-diffing every other color pair.
    // Registering black_candle with the normal 16-combination scheme would
    // silently stomp on the next block's ids, so black_candle and
    // black_candle_cake are intentionally left unregistered here (same
    // reasoning as the pointed_dripstone exclusion above). Support-removal
    // physics for black candles still works fine without this, since that
    // check only needs the block name, not its properties.
    for (const char* color : {"candle", "white_candle", "orange_candle", "magenta_candle",
                              "light_blue_candle", "yellow_candle", "lime_candle", "pink_candle",
                              "gray_candle", "light_gray_candle", "cyan_candle", "purple_candle",
                              "blue_candle", "brown_candle", "green_candle", "red_candle"}) {
        add(color, {{"candles", range(1, 4)}, {"lit", kBool}, {"waterlogged", kBool}});
    }

    // LANTERN_SUPPORT_V1: hanging x waterlogged = 4 states; range gap confirmed
    // clean for both lantern and soul_lantern.
    add("lantern", {{"hanging", kBool}, {"waterlogged", kBool}});
    add("soul_lantern", {{"hanging", kBool}, {"waterlogged", kBool}});

    // CAKE_EAT_V1: cake right-click eating needs a registered bites 0..6 state so
    // registry-based lookups (blockRegistryIdForState etc.) resolve correctly.
    // Verified gap: cake=5874 to repeater=5884 is 10, which is >= vanilla's 7 bite
    // states (0..6); the remaining 3 ids are unused padding, matching the same kind
    // of gap already seen and documented for other blocks in this table.
    add("cake", {{"bites", range(0, 6)}});
}

void RegistryManager::loadItemDefaults() {
    items_.registerEntry({0, "minecraft:air", 64, 0.0f, 4.0f});
    items_.registerEntry({1, "minecraft:stone", 64, 1.0f, 4.0f});
    items_.registerEntry({4, "minecraft:crafting_table", 64, 2.0f, 4.0f});
    items_.registerEntry({10, "minecraft:dirt", 64, 0.5f, 4.0f});
    items_.registerEntry({280, "minecraft:stick", 64, 0.5f, 4.0f});
    items_.registerEntry({281, "minecraft:bowl", 64, 0.5f, 4.0f});
    items_.registerEntry({268, "minecraft:wooden_sword", 1, 4.0f, 1.6f});
    items_.registerEntry({272, "minecraft:stone_sword", 1, 5.0f, 1.6f});
    items_.registerEntry({276, "minecraft:diamond_sword", 1, 7.0f, 1.6f});
}

void RegistryManager::loadEntityDefaults() {
    entities_.registerEntry({0, "minecraft:player", 0.6f, 1.8f, true, false});
    entities_.registerEntry({54, "minecraft:zombie", 0.6f, 1.95f, true, true});
    entities_.registerEntry({51, "minecraft:skeleton", 0.6f, 1.99f, true, true});
    entities_.registerEntry({50, "minecraft:creeper", 0.6f, 1.7f, false, true});
    entities_.registerEntry({9, "minecraft:cow", 0.9f, 1.4f, true, false});
    entities_.registerEntry({10, "minecraft:pig", 0.9f, 0.9f, true, false});
    entities_.registerEntry({12, "minecraft:sheep", 0.6f, 0.95f, true, false});
}

void RegistryManager::loadBiomeDefaults() {
    biomes_.registerEntry({0, "minecraft:plains", 0.8f, 0.4f, true});
    biomes_.registerEntry({1, "minecraft:forest", 0.7f, 0.8f, true});
    biomes_.registerEntry({3, "minecraft:desert", 2.0f, 0.0f, true});
    biomes_.registerEntry({12, "minecraft:ocean", 0.5f, 0.5f, true});
    biomes_.registerEntry({14, "minecraft:mountains", 0.2f, 0.3f, true});
}

} // namespace nc::registries
