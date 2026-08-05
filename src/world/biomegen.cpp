#include "biomegen.hpp"

// STRESS_FIX_V2 / BUILD_FIX_V1: сборка не видит папку thirdparty/cubiomes в своём глобе исходников
// (она знает только src/, core/, entity/, network/, protocol/**, registries/, world/), поэтому вместо
// отдельных .cpp-файлов (которые никто не скомпилирует) мы вклеиваем весь cubiomes
// прямо сюда — в единственный world/biomegen.cpp, который сборка уже точно собирает и линкует.
// Никаких правок CMakeLists.txt больше не нужно. Файлы .inc нарочно не попадают ни в один
// известный CMake glob для *.cpp — никакого риска двойной компиляции/символов.
// WARNFIX_V1: cubiomes — чужой C-код, его предупреждения мы не правим в исходниках (чтобы не ломать
// обновления upstream), а глушим ровно на время включения его файлов.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable: 4201) // безымянные struct/union
#  pragma warning(disable: 4244) // сужение типов
#  pragma warning(disable: 4245) // signed/unsigned
#  pragma warning(disable: 4305) // double -> float
#  pragma warning(disable: 4701) // возможно неинициализированная переменная
#  pragma warning(disable: 4702) // недостижимый код
#  pragma warning(disable: 4706) // присваивание в условии
#endif
extern "C" {
#include "../thirdparty/cubiomes/generator.h"
#include "../thirdparty/cubiomes/util.h"

// Сами реализации тоже внутри extern "C" — их символы должны иметь то же C-связывание,
// что и их прототипы в generator.h/util.h выше, иначе будет то же LNK2019 из-за несовпадения C vs C++ mangling.
#include "../thirdparty/cubiomes/noise.inc"
#include "../thirdparty/cubiomes/layers.inc"
#include "../thirdparty/cubiomes/biomenoise.inc"
#include "../thirdparty/cubiomes/biomes.inc"
#include "../thirdparty/cubiomes/util.inc"
#include "../thirdparty/cubiomes/generator.inc"
}
#if defined(_MSC_VER)
#  pragma warning(pop) // WARNFIX_V1: дальше снова полный /W4 для нашего кода
#endif

namespace nc::world::biome {

namespace {

// Caches one cubiomes Generator per (thread, seed). setupGenerator()/
// applySeed() do nontrivial one-time work (SHA-hashing the seed, setting up
// octave noise) that we don't want to repeat for every single 4x4x4 cell
// lookup during a world export.
struct CachedGen {
    bool valid = false;
    i64 seed = 0;
    Generator g{};
};

CachedGen& cache() {
    static thread_local CachedGen c;
    return c;
}

Generator& generatorFor(i64 seed) {
    CachedGen& c = cache();
    if (!c.valid || c.seed != seed) {
        setupGenerator(&c.g, MC_1_21_1, 0);
        applySeed(&c.g, DIM_OVERWORLD, static_cast<uint64_t>(seed));
        c.seed = seed;
        c.valid = true;
    }
    return c.g;
}

} // namespace

i32 getBiomeIdAt(i64 seed, i32 x, i32 y, i32 z) {
    Generator& g = generatorFor(seed);
    return ::getBiomeAt(&g, 1, x, y, z);
}

std::string getBiomeName(i64 seed, i32 x, i32 y, i32 z) {
    i32 id = getBiomeIdAt(seed, x, y, z);
    if (id < 0) return "minecraft:plains";
    const char* name = ::biome2str(MC_1_21_1, id);
    if (!name) return "minecraft:plains";
    return std::string("minecraft:") + name;
}

} // namespace nc::world::biome
