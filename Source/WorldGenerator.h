#pragma once
#include "ChunkManager.h"   // BlockId, ChunkData, CHUNK_SIZE_*, BLOCK_*
#include "PerlinNoise.hpp"  // siv::PerlinNoise

#include <algorithm>

/**
 * @file WorldGenerator.h
 * @brief Генерация ландшафта чанка — общая для GameScene и MainMenuScene
 *        (раньше лямбда дублировалась в обеих сценах).
 *
 * Слои мира (сверху вниз):
 *   • Поверхность: каменные выступы → песчаное дно/пляжи у воды → гравийные
 *     пятна → земляная корка → трава.
 *   • Подповерхность (3 блока): продолжает материал поверхности (песок под
 *     пляжем, гравий под пятном, иначе земля).
 *   • Глубина: камень с жилами гравия и рудами по глубине:
 *     уголь (y<=60) → железо (y<=40) → золото (y<=24) → кристалл (y<=16).
 *     Чем реже руда, тем выше порог шума — кластеры 3D-октавами, жилами.
 *
 * ⚠️ SEA_LEVEL должен совпадать со значением в buildWaterMesh (ChunkMeshBuilder.h).
 */
namespace worldgen
{

constexpr int SEA_LEVEL = 38;

/// Заполняет чанк блоками. Детерминированно по мировым координатам (сиды шумов фиксированы).
inline void generate(ChunkData& chunk)
{
    const ChunkKey& key = chunk.getKey();
    auto basePos        = ChunkManager::chunkToWorld(key);

    thread_local siv::PerlinNoise perlin(42);       // рельеф + крупный «биомный» сдвиг
    thread_local siv::PerlinNoise rockNoise(137);   // каменные выступы
    thread_local siv::PerlinNoise dirtNoise(271);   // земляная корка
    thread_local siv::PerlinNoise sandNoise(311);   // пляжи и песчаное дно
    thread_local siv::PerlinNoise gravelNoise(397); // гравийные пятна
    // 3D-шумы жил (в блоках под поверхностью)
    thread_local siv::PerlinNoise gravel3D(401);
    thread_local siv::PerlinNoise coal3D(503);
    thread_local siv::PerlinNoise iron3D(509);
    thread_local siv::PerlinNoise gold3D(521);
    thread_local siv::PerlinNoise crystal3D(541);

    for (int x = 0; x < CHUNK_SIZE_X; ++x)
        for (int z = 0; z < CHUNK_SIZE_Z; ++z)
        {
            const float wx = basePos.x + x;
            const float wz = basePos.z + z;

            const float terrain = perlin.octave2D_01(wx * 0.02f, wz * 0.02f, 6) * 40.0f;
            const float biome   = perlin.octave2D_01(wx * 0.005f, wz * 0.005f, 4) * 15.0f - 5.0f;
            const int surfaceY  = std::clamp(static_cast<int>(terrain + biome + 30), 1, CHUNK_SIZE_Y - 2);

            const float rockVal   = rockNoise.octave2D_01(wx * 0.08f, wz * 0.08f, 3);
            const float dirtVal   = dirtNoise.octave2D_01(wx * 0.06f, wz * 0.06f, 2);
            const float sandVal   = sandNoise.octave2D_01(wx * 0.05f, wz * 0.05f, 2);
            const float gravelVal = gravelNoise.octave2D_01(wx * 0.09f, wz * 0.09f, 2);

            // --- Блок поверхности ---
            // Приоритет: Камень → Песок у воды → Гравий → Земля → Трава.
            // Трава не генерируется ниже уровня моря ([FIX #12]: под водой — дно).
            BlockId surfaceBlock;
            if (rockVal > 0.72f)
                surfaceBlock = BLOCK_STONE;  // каменные выступы (игнорируют уровень воды)
            else if (surfaceY <= SEA_LEVEL + 2 && sandVal > 0.42f)
                surfaceBlock = BLOCK_SAND;   // пляжи и отмели (включая дно под водой)
            else if (surfaceY < SEA_LEVEL)
                surfaceBlock = BLOCK_DIRT;   // под водой без песка — земляное дно
            else if (gravelVal > 0.74f)
                surfaceBlock = BLOCK_GRAVEL; // гравийные пятна на суше
            else if (dirtVal > 0.65f)
                surfaceBlock = BLOCK_DIRT;   // земляная корка на суше
            else
                surfaceBlock = BLOCK_GRASS;  // стандартная трава (только выше уровня моря)

            // Подповерхность продолжает материал верха (песок под пляжем и т.п.)
            BlockId subBlock = BLOCK_DIRT;
            if (surfaceBlock == BLOCK_SAND)
                subBlock = BLOCK_SAND;
            else if (surfaceBlock == BLOCK_GRAVEL)
                subBlock = BLOCK_GRAVEL;

            for (int y = 0; y <= surfaceY; ++y)
            {
                BlockId block;
                if (y == surfaceY)
                {
                    block = surfaceBlock;
                }
                else if (y > surfaceY - 3)
                {
                    block = subBlock;  // 3 блока подповерхности
                }
                else
                {
                    block = BLOCK_STONE;  // глубинный камень

                    // Жилы и руды — только внутри камня. Пороги редкости растут
                    // с ценностью; глубина ограничивает «этаж» залегания.
                    const float gx = wx * 0.14f, gy = y * 0.14f, gz = wz * 0.14f;
                    if (gravel3D.octave3D_01(gx, gy, gz, 2) > 0.84f)
                        block = BLOCK_GRAVEL;  // гравийные жилы на любой глубине
                    else if (y <= 60 && coal3D.octave3D_01(gx, gy, gz, 2) > 0.80f)
                        block = BLOCK_COAL_ORE;
                    else if (y <= 40 && iron3D.octave3D_01(gx, gy, gz, 2) > 0.82f)
                        block = BLOCK_IRON_ORE;
                    else if (y <= 24 && gold3D.octave3D_01(gx, gy, gz, 2) > 0.85f)
                        block = BLOCK_GOLD_ORE;
                    else if (y <= 16 && crystal3D.octave3D_01(gx, gy, gz, 2) > 0.87f)
                        block = BLOCK_CRYSTAL_ORE;
                }

                chunk.setBlock(x, y, z, block);
            }

            // Вода от поверхности террейна до уровня моря
            if (surfaceY < SEA_LEVEL)
            {
                for (int y = surfaceY + 1; y <= SEA_LEVEL; ++y)
                    chunk.setBlock(x, y, z, BLOCK_WATER);
            }
        }
}

}  // namespace worldgen
