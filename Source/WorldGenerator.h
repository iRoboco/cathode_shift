#pragma once
#include "ChunkManager.h"   // BlockId, ChunkData, CHUNK_SIZE_*, BLOCK_*
#include "WorldConfig.h"    // worldcfg::WorldDef — профиль мира, регионы
#include "PerlinNoise.hpp"  // siv::PerlinNoise

#include <algorithm>
#include <array>

/**
 * @file WorldGenerator.h
 * @brief Генерация ландшафта чанка по профилю активного мира (WorldConfig.h).
 *
 * Все параметры — из worldcfg::WorldDef: сиды шумов производны от world.seed,
 * рельеф/материалы/руды/уровень моря настраиваются в worlds.json. МАКРО-регионы
 * мира (кольца от спавна, широтные полосы) блендятся по колонке через
 * WorldDef::sampleTerrain / sampleMaterials — плавные переходы без швов.
 *
 * Слои мира (сверху вниз):
 *   • Поверхность: каменные выступы → песчаное дно/пляжи у воды → гравийные
 *     пятна → земляная корка → основной блок (трава / снег-заготовка / ...).
 *   • Подповерхность (subDepth блоков): продолжает материал поверхности.
 *   • Глубина: камень с жилами по списку OreRule палитры.
 *
 * ⚠️ world.seaLevel прокидывается в ChunkManager::Config::seaLevel (вода меша).
 */
namespace worldgen
{

constexpr int SEA_LEVEL = 38;  ///< дефолт, если мир не задаёт water.seaLevel

/// Стандартный набор руд (прежнее поведение), если палитра мира не задаёт свой.
inline const std::vector<worldcfg::OreRule>& defaultOres()
{
    static const std::vector<worldcfg::OreRule> ores = {
        {BLOCK_GRAVEL, 255, 0.84f, 0.14f, 2, true},   // гравийные жилы на любой глубине
        {BLOCK_COAL_ORE, 60, 0.80f, 0.14f, 2, true},  // уголь
        {BLOCK_IRON_ORE, 40, 0.82f, 0.14f, 2, true},  // железо
        {BLOCK_GOLD_ORE, 24, 0.85f, 0.14f, 2, true},  // золото
        {BLOCK_CRYSTAL_ORE, 16, 0.87f, 0.14f, 2, true},  // кристалл
    };
    return ores;
}

/**
 * @brief Заполняет чанк блоками по профилю мира. Детерминированно по мировым
 *        координатам и world.seed (чистая функция — безопасно в воркерах).
 */
inline void generate(ChunkData& chunk, const worldcfg::WorldDef& world)
{
    const ChunkKey& key = chunk.getKey();
    auto basePos        = ChunkManager::chunkToWorld(key);
    const int seaLevel  = world.waterEnabled ? world.seaLevel : -1;

    // Сиды шумов производны от сида мира — у каждого мира свой рельеф.
    // Пересоздание PerlinNoise — это лишь тасование таблицы 256 байт, дёшево
    // на фоне генерации чанка.
    siv::PerlinNoise perlin(world.seed);            // рельеф + крупный «биомный» сдвиг
    siv::PerlinNoise rockNoise(world.seed + 137);   // каменные выступы
    siv::PerlinNoise dirtNoise(world.seed + 271);   // земляная корка
    siv::PerlinNoise sandNoise(world.seed + 311);   // пляжи и песчаное дно
    siv::PerlinNoise gravelNoise(world.seed + 397); // гравийные пятна
    siv::PerlinNoise hillsNoise(world.seed + 613);  // бугорки равнин
    siv::PerlinNoise lakeNoise(world.seed + 719);   // озёрные котловины
    // 3D-шумы жил (до 8 правил руд; лишние правила делят последний шум)
    std::array<siv::PerlinNoise, 8> ore3D = {siv::PerlinNoise(world.seed + 401), siv::PerlinNoise(world.seed + 503),
                                             siv::PerlinNoise(world.seed + 509), siv::PerlinNoise(world.seed + 521),
                                             siv::PerlinNoise(world.seed + 541), siv::PerlinNoise(world.seed + 557),
                                             siv::PerlinNoise(world.seed + 569), siv::PerlinNoise(world.seed + 571)};

    for (int x = 0; x < CHUNK_SIZE_X; ++x)
        for (int z = 0; z < CHUNK_SIZE_Z; ++z)
        {
            const float wx = basePos.x + x;
            const float wz = basePos.z + z;

            // --- МАКРО: региональный профиль точки (плавный блендинг) ---
            const worldcfg::TerrainProfile tp      = world.sampleTerrain(wx, wz);
            const worldcfg::MaterialPalette& mp    = world.sampleMaterials(wx, wz);
            const std::vector<worldcfg::OreRule>& ores = mp.ores.empty() ? defaultOres() : mp.ores;

            // --- Рельеф ---
            const float terrain =
                perlin.octave2D_01(wx * tp.terrainFreq, wz * tp.terrainFreq, tp.octaves) * tp.amplitude;
            const float macro =
                perlin.octave2D_01(wx * tp.macroFreq, wz * tp.macroFreq, tp.macroOctaves) * tp.macroOffset +
                tp.macroShift;
            float h = terrain + macro + tp.baseHeight;

            // Бугорки (равнины): пороговый шум добавляет локальные холмики.
            if (tp.hills.amplitude > 0.0f)
            {
                const float hn = hillsNoise.octave2D_01(wx * tp.hills.freq, wz * tp.hills.freq, 3);
                if (hn > tp.hills.threshold)
                    h += (hn - tp.hills.threshold) / (1.0f - tp.hills.threshold) * tp.hills.amplitude;
            }

            // Озёра: редкие котловины ниже уровня моря.
            if (tp.lakes.enabled && seaLevel >= 0)
            {
                const float ln = lakeNoise.octave2D_01(wx * tp.lakes.freq, wz * tp.lakes.freq, 3);
                if (ln > tp.lakes.threshold)
                    h -= (ln - tp.lakes.threshold) / (1.0f - tp.lakes.threshold) * tp.lakes.depth;
            }

            const int surfaceY = std::clamp(static_cast<int>(h), 1, CHUNK_SIZE_Y - 2);

            const float rockVal   = rockNoise.octave2D_01(wx * mp.rockFreq, wz * mp.rockFreq, 3);
            const float dirtVal   = dirtNoise.octave2D_01(wx * mp.dirtFreq, wz * mp.dirtFreq, 2);
            const float sandVal   = sandNoise.octave2D_01(wx * mp.sandFreq, wz * mp.sandFreq, 2);
            const float gravelVal = gravelNoise.octave2D_01(wx * mp.gravelFreq, wz * mp.gravelFreq, 2);

            // --- Блок поверхности ---
            // Приоритет: Камень → Песок у воды → Дно → Гравий → Земляная корка → Основной.
            // Основной блок (трава/снег) не генерируется ниже уровня моря.
            BlockId surfaceBlock;
            if (rockVal > mp.rockThreshold)
                surfaceBlock = mp.surfaceRock;
            else if (surfaceY <= seaLevel + 2 && sandVal > mp.sandThreshold)
                surfaceBlock = mp.surfaceBeach;
            else if (surfaceY < seaLevel)
                surfaceBlock = mp.surfaceUnderwater;
            else if (gravelVal > mp.gravelThreshold)
                surfaceBlock = mp.surfaceGravel;
            else if (dirtVal > mp.dirtThreshold)
                surfaceBlock = mp.surfaceCrust;
            else
                surfaceBlock = mp.surfacePrimary;

            // Подповерхность продолжает материал верха (песок под пляжем и т.п.)
            BlockId subBlock = mp.subSurface;
            if (surfaceBlock == mp.surfaceBeach)
                subBlock = mp.surfaceBeach;
            else if (surfaceBlock == mp.surfaceGravel)
                subBlock = mp.surfaceGravel;

            for (int y = 0; y <= surfaceY; ++y)
            {
                BlockId block;
                if (y == surfaceY)
                {
                    block = surfaceBlock;
                }
                else if (y > surfaceY - mp.subDepth)
                {
                    block = subBlock;
                }
                else
                {
                    block = BLOCK_STONE;  // глубинный камень

                    // Жилы и руды — по списку OreRule палитры мира/региона.
                    for (size_t i = 0; i < ores.size(); ++i)
                    {
                        const worldcfg::OreRule& r = ores[i];
                        if (!r.enabled || y > r.maxY)
                            continue;
                        siv::PerlinNoise& n3 = ore3D[std::min(i, ore3D.size() - 1)];
                        if (n3.octave3D_01(wx * r.freq, y * r.freq, wz * r.freq, r.octaves) > r.threshold)
                        {
                            block = r.block;
                            break;  // первое сработавшее правило выигрывает (порядок = приоритет)
                        }
                    }
                }

                chunk.setBlock(x, y, z, block);
            }

            // Вода от поверхности террейна до уровня моря мира
            if (seaLevel >= 0 && surfaceY < seaLevel)
            {
                for (int y = surfaceY + 1; y <= seaLevel; ++y)
                    chunk.setBlock(x, y, z, BLOCK_WATER);
            }
        }
}

/// Обратная совместимость: генерация с настройками «как раньше» (дефолтный мир).
inline void generate(ChunkData& chunk)
{
    static const worldcfg::WorldDef legacy;  // дефолты совпадают с прежними константами
    generate(chunk, legacy);
}

}  // namespace worldgen
