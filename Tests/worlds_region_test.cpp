// Автономный тест системы миров (WorldConfig/WorldGenerator): парсинг worlds.json,
// веса регионов, модуляция геометрии, сэмплинг террейна/материалов/погоды, генерация.
//
// Сборка (без движка, нужен только rapidjson из thirdparty):
//   mkdir build_test && cd build_test
//   cp ../Source/Biome.h ../Source/WorldConfig.h ../Source/WorldGenerator.h ../Source/PerlinNoise.hpp .
//   cp ../Content/config/worlds.json .
//   g++ -std=c++17 -O1 -I. -I../Tests/stub -I<путь_к_rapidjson/include> ../Tests/worlds_region_test.cpp -o test_worlds
//   ./test_worlds
//
// Заголовки копируются рядом со stub'ом, потому что quote-include "ChunkManager.h"
// сначала ищется рядом с включающим файлом — так stub подменяет движковый заголовок.
#include "WorldConfig.h"
#include "WorldGenerator.h"

#include <cstdio>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, ...)                                        \
    do                                                          \
    {                                                           \
        ++g_checks;                                             \
        if (!(cond))                                            \
        {                                                       \
            ++g_failures;                                       \
            printf("FAIL [%s:%d]: ", __FILE__, __LINE__);       \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static std::string readFile(const char* path)
{
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::map<BlockId, int> histogram(const ChunkData& c)
{
    std::map<BlockId, int> h;
    for (int z = 0; z < CHUNK_SIZE_Z; ++z)
        for (int y = 0; y < CHUNK_SIZE_Y; ++y)
            for (int x = 0; x < CHUNK_SIZE_X; ++x)
                ++h[c.getBlock(x, y, z)];
    return h;
}

int main(int argc, char** argv)
{
    const char* jsonPath = argc > 1 ? argv[1] : "worlds.json";
    const std::string json = readFile(jsonPath);
    CHECK(!json.empty(), "worlds.json не прочитан: %s", jsonPath);

    worldcfg::WorldCatalog catalog;
    CHECK(catalog.loadFromString(json), "каталог не распарсился");
    CHECK(catalog.size() == 2, "ожидалось 2 мира, получено %zu", catalog.size());
    CHECK(catalog.activeId() == "overworld", "activeWorld != overworld: '%s'", catalog.activeId().c_str());

    const worldcfg::WorldDef* ow = catalog.byId("overworld");
    const worldcfg::WorldDef* jd = catalog.byId("jelly_depths");
    CHECK(ow && jd, "миры overworld/jelly_depths не найдены");
    if (!ow || !jd)
        return 1;

    CHECK(ow->regions.size() == 4, "overworld: ожидалось 4 региона, получено %zu", ow->regions.size());
    CHECK(ow->seed == 1337, "overworld seed != 1337");
    CHECK(ow->seaLevel == 38, "overworld seaLevel != 38");
    CHECK(jd->seaLevel == 34, "jelly_depths seaLevel != 34");
    CHECK(jd->biomes.regionModulator == jd, "jd: regionModulator не установлен");
    CHECK(ow->biomes.regionModulator == ow, "ow: regionModulator не установлен");

    // ===== Веса регионов (overworld: ring 0..1000 горы, 1000..2000 равнина) =====
    auto weightAt = [&](double wx, double wz, size_t idx) {
        float w[16] = {0};
        ow->regionWeights(wx, wz, w);
        return w[idx];
    };
    float wtmp[16] = {0};

    CHECK(weightAt(0, 0, 0) > 0.99f, "центр: вес гор %.3f (ожидалось ~1)", weightAt(0, 0, 0));
    CHECK(weightAt(1500, 0, 1) > 0.99f, "r=1500: вес равнины %.3f (ожидалось ~1)", weightAt(1500, 0, 1));
    {
        // Граница колец r=1000: оба региона ~0.5 (blend 150, smoothstep симметричен)
        const float wm = weightAt(1000, 0, 0), wp = weightAt(1000, 0, 1);
        CHECK(std::fabs(wm - wp) < 0.05f, "r=1000: горы %.3f vs равнина %.3f (ожидалось ~0.5/0.5)", wm, wp);
    }
    CHECK(weightAt(0, -2200, 2) > 0.99f, "z=-2200: вес сев.снегов %.3f", weightAt(0, -2200, 2));
    CHECK(weightAt(0, 3000, 3) > 0.99f, "z=3000: вес юж.снегов %.3f", weightAt(0, 3000, 3));
    {
        // Настоящее перекрытие: (0,-1500) — r=1500 (равнина ≈1) и граница сев.снегов (≈0.5).
        // Σ сырых весов > 1 → нормализация, база получает 0, Σ = 1.
        const float baseW = ow->regionWeights(0, -1500, wtmp);
        float sum = 0;
        for (int i = 0; i < 4; ++i)
            sum += wtmp[i];
        CHECK(wtmp[1] > 0.3f && wtmp[2] > 0.2f, "перекрытие не воспроизведено: равнина %.3f, снега %.3f", wtmp[1], wtmp[2]);
        CHECK(baseW == 0.0f, "перекрытие: база должна получить 0, получила %.4f", baseW);
        CHECK(std::fabs(sum - 1.0f) < 1e-4f, "перекрытие: Σ регионов %.4f != 1", sum);
        // Вне перекрытия: Σ + база = 1
        const float baseW2 = ow->regionWeights(2300, 0, wtmp);  // за внешней кромкой равнины
        float sum2 = 0;
        for (int i = 0; i < 4; ++i)
            sum2 += wtmp[i];
        CHECK(std::fabs(baseW2 + sum2 - 1.0f) < 1e-4f, "r=2300: база %.3f + регионы %.3f != 1", baseW2, sum2);
    }

    // ===== Модуляция геометрии регионами =====
    {
        float cur = 0.7f, soft = 0.0f;
        ow->modulate(0, 0, cur, soft);  // горы: curvatureScale 1.2
        CHECK(std::fabs(cur - 0.84f) < 1e-3f, "горы: curvature %.3f != 0.84", cur);

        cur = 0.7f; soft = 0.3f;
        ow->modulate(0, -2200, cur, soft);  // сев.снега: scale 0/0
        CHECK(std::fabs(cur) < 1e-3f && std::fabs(soft) < 1e-3f, "снега: cur %.3f soft %.3f != 0", cur, soft);

        cur = 0.8f; soft = 0.8f;
        ow->modulate(1500, 0, cur, soft);  // равнина: curvatureScale 0.5
        CHECK(std::fabs(cur - 0.4f) < 1e-3f, "равнина: curvature %.3f != 0.4", cur);

        // Кламп в [0,1]
        cur = 1.0f; soft = 1.0f;
        ow->modulate(0, 0, cur, soft);  // 1.0 * 1.2 -> clamp 1.0
        CHECK(cur <= 1.0f + 1e-6f, "модуляция не ограничена: %.3f", cur);
    }

    // ===== Террейн: плавный лерп baseHeight горы(55) -> равнина(40) =====
    {
        const float h0    = ow->sampleTerrain(0, 0).baseHeight;
        const float h1500 = ow->sampleTerrain(1500, 0).baseHeight;
        const float h1000 = ow->sampleTerrain(1000, 0).baseHeight;
        CHECK(std::fabs(h0 - 55.0f) < 0.5f, "baseHeight в центре %.2f != 55", h0);
        CHECK(std::fabs(h1500 - 36.0f) < 0.5f, "baseHeight r=1500 %.2f != 36", h1500);
        CHECK(h1000 > 42.0f && h1000 < 49.0f, "baseHeight r=1000 %.2f вне (42,49)", h1000);

        // Монотонность перехода (без скачков)
        float prev = ow->sampleTerrain(900, 0).baseHeight;
        bool mono = true;
        for (double r = 920; r <= 1100; r += 20)
        {
            const float cur = ow->sampleTerrain(r, 0).baseHeight;
            if (cur > prev + 0.01f)
                mono = false;
            prev = cur;
        }
        CHECK(mono, "baseHeight не монотонно убывает в переходе 900..1100");
    }

    // ===== Материалы: снег-заготовка на севере/юге =====
    CHECK(ow->sampleMaterials(0, 0).surfacePrimary == BLOCK_GRASS, "центр: surfacePrimary != grass");
    CHECK(ow->sampleMaterials(0, -2200).surfacePrimary == BLOCK_SNOW, "север: surfacePrimary != snow");
    CHECK(ow->sampleMaterials(0, 3000).surfacePrimary == BLOCK_SNOW, "юг: surfacePrimary != snow");

    // ===== Погода: заготовка снежных регионов =====
    {
        const auto wNorth  = ow->sampleWeather(0, -2200);
        const auto wCenter = ow->sampleWeather(0, 0);
        CHECK(wNorth.type == worldcfg::WeatherType::Snow, "север: погода != Snow");
        CHECK(wNorth.temperature < -10.0f, "север: температура %.1f (ожидалось ~-15)", wNorth.temperature);
        CHECK(wCenter.type == worldcfg::WeatherType::Clear, "центр: погода != Clear");
        // Промежуточная точка: температура блендится
        const auto wBlend = ow->sampleWeather(0, -1500);
        CHECK(wBlend.temperature < 15.0f && wBlend.temperature > -16.0f,
              "граница погоды: температура %.1f вне диапазона", wBlend.temperature);
    }

    // ===== Генерация чанков =====
    {
        // Центр (горы): чанк не пуст, есть камень
        ChunkData c0({0, 0, 0});
        worldgen::generate(c0, *ow);
        auto h0 = histogram(c0);
        CHECK(h0[BLOCK_STONE] > 0, "центр: нет камня");
        CHECK(h0[BLOCK_AIR] > 0, "центр: нет воздуха (переполнение чанка?)");

        // Детерминизм: та же колонка -> тот же чанк
        ChunkData c0b({0, 0, 0});
        worldgen::generate(c0b, *ow);
        bool same = true;
        for (int z = 0; z < CHUNK_SIZE_Z && same; ++z)
            for (int y = 0; y < CHUNK_SIZE_Y && same; ++y)
                for (int x = 0; x < CHUNK_SIZE_X && same; ++x)
                    if (c0.getBlock(x, y, z) != c0b.getBlock(x, y, z))
                        same = false;
        CHECK(same, "генерация недетерминирована");

        // Северные снега: z=-2000 -> чанк z = -125; поверхность должна быть снежной
        ChunkData cn({0, 0, -138});  // z=-2200 -> чанк -138
        worldgen::generate(cn, *ow);
        auto hn = histogram(cn);
        CHECK(hn[BLOCK_SNOW] > 0, "север: нет снежных блоков (заготовка не работает)");

        // Равнина с озёрами: сканируем полосу чанков r=1400..2000, ищем воду
        int waterChunks = 0;
        for (int cx = 88; cx <= 124 && waterChunks < 3; ++cx)
        {
            ChunkData cp({cx, 0, 0});
            worldgen::generate(cp, *ow);
            if (histogram(cp)[BLOCK_WATER] > 0)
                ++waterChunks;
        }
        CHECK(waterChunks > 0, "равнина: озёра не сгенерировались ни в одном чанке полосы");

        // jelly_depths: другой сид -> другой ландшафт; есть блоки
        ChunkData cj({0, 0, 0});
        worldgen::generate(cj, *jd);
        auto hj = histogram(cj);
        CHECK(hj[BLOCK_STONE] + hj[BLOCK_GRASS] + hj[BLOCK_DIRT] + hj[BLOCK_SAND] > 0,
              "jelly_depths: чанк пуст");
        CHECK(hj[BLOCK_WATER] > 0 || hj[BLOCK_STONE] > 0, "jelly_depths: нет содержимого");

        // Легаси-обёртка без профиля работает
        ChunkData cl({0, 0, 0});
        worldgen::generate(cl);
        CHECK(cl.maxFilledY() > 0, "legacy generate: чанк пуст");
    }

    // ===== Регрессии (исправления системы миров) =====

    // Верхний твёрдый блок колонки по реально сгенерированному чанку.
    auto topSolid = [](const worldcfg::WorldDef& w, int wx, int wz) {
        const int cx = (int)std::floor(wx / 16.0), cz = (int)std::floor(wz / 16.0);
        ChunkData c({cx, 0, cz});
        worldgen::generate(c, w);
        int y = CHUNK_SIZE_Y - 1;
        while (y > 0 && (c.getBlock(wx - cx * 16, y, wz - cz * 16) == BLOCK_AIR ||
                         c.getBlock(wx - cx * 16, y, wz - cz * 16) == BLOCK_WATER))
            --y;
        return y;
    };

    // 1) Нет обрыва рельефа на границе регионов (раньше: ~15 блоков при r=1000).
    {
        int prev = topSolid(*ow, 900, 5), worst = 0;
        for (int x = 901; x <= 1100; ++x)
        {
            const int y = topSolid(*ow, x, 5);
            worst = std::max(worst, std::abs(y - prev));
            prev = y;
        }
        CHECK(worst <= 5, "шов на границе гор/равнины: перепад %d блоков между соседними колонками", worst);
    }

    // surfaceHeight() == то, что реально строит generate() (спавн опирается на него).
    {
        bool ok = true;
        for (int x = -40; x <= 40 && ok; x += 8)
            for (int z = -40; z <= 40 && ok; z += 8)
                ok = worldgen::surfaceHeight(*ow, x, z) == topSolid(*ow, x, z) ||
                     worldgen::surfaceHeight(*ow, x, z) < ow->seaLevel;  // вода сверху не считается
        CHECK(ok, "surfaceHeight расходится с generate()");
    }

    // 5) Спавн над реальной поверхностью (для нескольких сидов и спавнов в горах/на равнине).
    {
        bool ok = true;
        for (uint32_t seed : {1u, 7u, 42u, 1337u, 9021u, 123456u})
            for (float sx : {0.0f, 500.0f, 1500.0f})
            {
                worldcfg::WorldDef w = *ow;
                w.seed = seed;
                w.spawnX = sx;
                w.spawnZ = 0;
                const float y = worldgen::spawnHeight(w);
                const int surf = worldgen::surfaceHeight(w, sx, 0);
                if (!(y >= surf + 2))
                {
                    ok = false;
                    printf("  спавн seed=%u x=%.0f: y=%.0f, поверхность %d\n", seed, sx, y, surf);
                }
            }
        CHECK(ok, "spawnHeight ниже/вплотную к поверхности (игрок в земле)");
    }

    // 2) Более 16 регионов: лишние отбрасываются, переполнения стека нет.
    {
        std::string js = R"({"worlds":[{"id":"many","regions":[)";
        for (int i = 0; i < 20; ++i)
        {
            if (i) js += ",";
            js += R"({"name":"r)" + std::to_string(i) + R"(","ring":{"rMin":)" + std::to_string(i * 100) +
                  R"(,"rMax":)" + std::to_string(i * 100 + 100) + R"(},"blend":50})";
        }
        js += "]}]}";
        worldcfg::WorldCatalog cat;
        CHECK(cat.loadFromString(js), "каталог с 20 регионами не загрузился");
        const auto* mw = cat.byId("many");
        CHECK(mw && mw->regions.size() == worldcfg::kMaxRegions, "регионов %zu, ожидалось %zu",
              mw ? mw->regions.size() : 0, worldcfg::kMaxRegions);
        CHECK(!cat.warnings().empty(), "нет предупреждения об отброшенных регионах");
        if (mw)
        {
            float cur = 0.5f, soft = 0.5f;
            mw->modulate(950, 0, cur, soft);
            (void)mw->sampleTerrain(950, 0);
            (void)mw->sampleMaterials(950, 0);
            ChunkData c({59, 0, 0});
            worldgen::generate(c, *mw);
            CHECK(std::isfinite(cur) && std::isfinite(soft), "модуляция дала NaN");
        }
    }

    // 3) blend = 0 не даёт NaN (в JSON и при ручном создании региона).
    {
        worldcfg::WorldCatalog cat;
        CHECK(cat.loadFromString(R"({"worlds":[{"id":"b0","regions":[
            {"name":"z","ring":{"rMin":100,"rMax":200},"blend":0,
             "terrain":{"baseHeight":80}}]}]})"), "мир с blend=0 не загрузился");
        const auto* bw = cat.byId("b0");
        CHECK(bw && bw->regions[0].blend >= worldcfg::kMinBlend, "blend не исправлен");
        CHECK(!cat.warnings().empty(), "нет предупреждения про blend");
        worldcfg::WorldDef w;
        worldcfg::Region r;
        r.useRing = true; r.rMin = 100; r.rMax = 200; r.blend = 0.0f;  // в обход парсера
        w.regions.push_back(r);
        bool finite = true;
        for (double d = 0; d <= 300; d += 0.5)
        {
            float ww[worldcfg::kMaxRegions];
            const float bw0 = w.regionWeights(d, 0, ww);
            finite = finite && std::isfinite(bw0) && std::isfinite(ww[0]);
        }
        CHECK(finite, "regionWeights при blend=0 вернул NaN/inf");
    }

    // 4) Опечатка в activeWorld → первый мир, а не аварийный мир.
    {
        worldcfg::WorldCatalog cat;
        CHECK(cat.loadFromString(R"({"activeWorld":"overwrold","worlds":[{"id":"a"},{"id":"b"}]})"),
              "каталог не загрузился");
        CHECK(cat.active() != nullptr && cat.activeId() == "a", "activeWorld с опечаткой: active()=%s",
              cat.active() ? cat.activeId().c_str() : "nullptr");
        CHECK(!cat.warnings().empty(), "нет предупреждения про activeWorld");
        worldcfg::WorldCatalog empty;
        CHECK(!empty.loadFromString("{ not json"), "битый JSON принят");
        CHECK(empty.size() == 0 && empty.active() == nullptr, "после битого JSON каталог не пуст");
    }

    // 6) Некорректные числа и имена в JSON не роняют и не ломают мир.
    {
        worldcfg::WorldCatalog cat;
        CHECK(cat.loadFromString(R"({"worlds":[{"id":"w","seed":4000000000,
            "water":{"seaLevel":9999},
            "geometry":{"macroCellSize":0,"blendWidth":-5},
            "terrain":{"octaves":4.7,"macroOctaves":99},
            "materials":{"surfacePrimary":"gras","subDepth":-3,
                         "ores":[{"block":"unobtainium","octaves":0}]}}]})"),
              "каталог с кривыми числами не загрузился");
        const auto* w = cat.byId("w");
        if (w)
        {
            CHECK(w->seed == 4000000000u, "seed > INT_MAX испорчен: %u", w->seed);
            CHECK(w->seaLevel == CHUNK_SIZE_Y - 2, "seaLevel не зажат: %d", w->seaLevel);
            CHECK(w->biomes.macroCellSize >= 8.0f && w->biomes.blendWidth >= 0.0f, "биомы: cell %.1f blend %.1f",
                  w->biomes.macroCellSize, w->biomes.blendWidth);
            CHECK(w->terrain.octaves == 4, "octaves 4.7 -> %d (ожидалось 4)", w->terrain.octaves);
            CHECK(w->terrain.macroOctaves == 8, "macroOctaves 99 -> %d (ожидалось 8)", w->terrain.macroOctaves);
            CHECK(w->materials.subDepth == 0, "subDepth -3 -> %d", w->materials.subDepth);
            CHECK(w->materials.surfacePrimary == BLOCK_GRASS, "опечатка 'gras' должна дать дефолт grass");
            ChunkData c({0, 0, 0});
            worldgen::generate(c, *w);  // seaLevel зажат → запись в чанк в границах
            CHECK(c.maxFilledY() < CHUNK_SIZE_Y, "генерация вышла за границы чанка");
        }
        // gras, unobtainium, seaLevel, macroCellSize, octaves 99, subDepth ≥ 6 предупреждений
        CHECK(cat.warnings().size() >= 6, "предупреждений %zu, ожидалось >= 6", cat.warnings().size());
    }

    printf("---\n%d проверок, %d провалов\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
