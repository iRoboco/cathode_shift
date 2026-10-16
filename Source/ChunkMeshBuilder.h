#pragma once
#include "axmol.h"
#include "ChunkManager.h"

/**
 * @file ChunkMeshBuilder.h
 * @brief Модуль построения геометрии чанков для воксельного мира.
 *
 * Реализует алгоритм генерации мешей с оптимизациями:
 * - Face Culling (отсечение невидимых граней)
 * - Кросс-чанковая проверка соседей для устранения щелей на стыках
 * - Триангуляция квадов в индексный буфер
 * - UV-развёртка для текстурного атласа
 *
 * Все функции inline для минимизации накладных расходов вызова.
 */

// ============================================================================
/** @name Структуры данных
 *  Базовые типы для хранения вершинной геометрии
 */
/// @{

/**
 * @struct ChunkVertex
 * @brief Интерливинговая структура вершины для воксельного меша.
 *
 * Оптимизирована под UNLIT-рендеринг:
 * - Позиция (x,y,z): локальные координаты внутри чанка [0..16]×[0..256]×[0..16]
 * - Текстура (u,v): нормированные UV-координаты для атласа [0.0..1.0]
 *
 * Размер: 5 × float = 20 байт. Нормали не хранятся — вычисляются в шейдере
 * или задаются константой для UNLIT-материала.
 */
struct ChunkVertex
{
    float x, y, z;  ///< Позиция вершины в локальном пространстве чанка
    float u, v;     ///< UV-координаты для выборки из текстурного атласа
};

/// @}
// ============================================================================

/** @name Вспомогательные функции
 *  Утилиты для расчёта параметров рендеринга
 */
/// @{

/**
 * @brief Рассчитывает UV-координаты тайла для заданного blockId в атласе.
 *
 * @param blockId Идентификатор блока (индекс тайла в атласе)
 * @param atlasSize Размер сетки атласа (по умолчанию 16×16)
 * @return std::array<float, 4> Массив [u_min, v_min, u_max, v_max]
 *
 * @details
 * Математика развёртки:
 * @code
 * col = blockId % atlasSize          // номер столбца (0..atlasSize-1)
 * row = blockId / atlasSize          // номер строки
 * tileU = col / atlasSize            // левая граница тайла по U
 * tileV = row / atlasSize            // нижняя граница тайла по V
 * tileSize = 1.0 / atlasSize         // размер одного тайла в нормированных координатах
 * @endcode
 *
 * Возвращаемые координаты позволяют "натянуть" текстуру тайла ровно на грань блока
 * без искажений. Для BLOCK_AIR (id=0) принудительно возвращается первый тайл.
 *
 * @note Функция inline — компилируется в месте вызова, нулевые накладные расходы.
 */
// Новая функция: вычисляет UV для конкретного индекса тайла в атласе
inline std::array<float, 4> calculateTileUV(uint16_t tileIndex, int atlasSize = 4)
{
    // Защита от деления на 0 и корректная обработка невалидных индексов
    uint16_t idx   = (tileIndex == 0 || tileIndex >= 256) ? 0 : tileIndex;
    float tileU    = (idx % atlasSize) / static_cast<float>(atlasSize);
    float tileV    = (idx / atlasSize) / static_cast<float>(atlasSize);
    float tileSize = 1.0f / static_cast<float>(atlasSize);
    return {tileU, tileV, tileU + tileSize, tileV + tileSize};
}

// Обёртка для обратной совместимости: ранее называлась calculateBlockUV
inline std::array<float, 4> calculateBlockUV(uint16_t blockId, int atlasSize = 4)
{
    return calculateTileUV(blockId, atlasSize);
}

// Возвращает индекс тайла в атласе для конкретной грани блока
// face: 0=+X, 1=-X, 2=+Y(top), 3=-Y(bottom), 4=+Z, 5=-Z
// Atlas 4x4 layout: 0=Grass, 1=Grass with dirt, 2=Dirt, 3=Stone, 4=Water,
//                   5=Sand, 6=Gravel, 7=Coal, 8=Iron, 9=Gold, 10=Crystal
// (совпадает с texgen::Tile в TextureFactory.h)
inline uint16_t getBlockTileIndex(uint16_t blockId, int face)
{
    switch (blockId)
    {
    case BLOCK_GRASS:
    {
        // Трава (BLOCK_GRASS):
        // - Верх (+Y): травяной тайл (0)
        // - Низ (-Y): земля (2)
        // - Бока (±X, ±Z): земля с травой (1)
        const uint16_t TILE_GRASS_TOP  = 0;  // grass top texture
        const uint16_t TILE_GRASS_SIDE = 1;  // grass side (earth with grass border)
        const uint16_t TILE_DIRT       = 2;  // dirt texture
        if (face == 2)
            return TILE_GRASS_TOP;  // +Y face
        if (face == 3)
            return TILE_DIRT;    // -Y face
        return TILE_GRASS_SIDE;  // ±X and ±Z faces
    }
    case BLOCK_STONE:
        return 3;  // stone texture on all faces
    case BLOCK_DIRT:
        return 2;  // dirt texture on all faces
    case BLOCK_SAND:
        return 5;  // песок на всех гранях
    case BLOCK_GRAVEL:
        return 6;  // гравий на всех гранях
    case BLOCK_COAL_ORE:
        return 7;  // угольная руда
    case BLOCK_IRON_ORE:
        return 8;  // железная руда
    case BLOCK_GOLD_ORE:
        return 9;  // золотая руда
    case BLOCK_CRYSTAL_ORE:
        return 10;  // кристаллическая руда
    case BLOCK_SNOW:
        return 11;  // снег (заготовка) на всех гранях
    default:
        return 0;  // fallback: use grass tile
    }
}

/// @}
// ============================================================================
//  buildChunkMesh — генерация террейн-меша
// ============================================================================

/** @name Основные алгоритмы
 *  Ядро генерации геометрии чанка
 */
/// @{

/**
 * @brief Генерирует вершины и индексы для видимых граней чанка.
 *
 * @param data Ссылка на данные текущего чанка (источник блоков)
 * @param neighborX0 Данные чанка-соседа по -X (nullptr если не активен)
 * @param neighborX1 Данные чанка-соседа по +X (nullptr если не активен)
 * @param neighborZ0 Данные чанка-соседа по -Z (nullptr если не активен)
 * @param neighborZ1 Данные чанка-соседа по +Z (nullptr если не активен)
 * @param vertices Выходной вектор вершин формата ChunkVertex
 * @param indices Выходной индексный буфер Axmol (тип uint16_t)
 * @param defaultBlockUV Резервный UV-индекс для блоков без текстуры
 *
 * @details
 * ## Механизм работы:
 *
 * ### 1. Face Culling (отсечение граней)
 * Для каждого непрозрачного блока проверяются 6 соседей. Грань генерируется
 * только если соседний воксель = BLOCK_AIR или находится за пределами карты.
 * Лямбда `isSolid` обрабатывает кросс-чанковые переходы: если блок на границе
 * чанка, запрашиваются данные из соседнего чанка (если он активен).
 *
 * ### 2. Порядок вершин (CCW — Counter-Clockwise)
 * Все грани добавляются в порядке против часовой стрелки относительно
 * внешней нормали. Это критично для Backface Culling: GPU автоматически
 * отбрасывает невидимые обратные грани, экономя ~50% вершинного трафика.
 *
 * ### 3. Триангуляция
 * Каждый квад (4 вершины) разбивается на 2 треугольника по диагонали:
 * @code
 * Треугольник 1: v0 → v1 → v2  (индексы: base+0, +1, +2)
 * Треугольник 2: v0 → v2 → v3  (индексы: base+0, +2, +3)
 * @endcode
 * Индексный буфер позволяет переиспользовать вершины, экономя память.
 *
 * ### 4. UV-координаты
 * Вычисляются один раз на блок (не на грань!), затем применяются ко всем
 * вершинам грани. Это гарантирует корректное наложение текстуры без швов.
 *
 * ### 5. Оптимизация прохода
 * Циклы организованы в порядке Y-major (Z → Y → X) для лучшей кэш-локальности
 * при доступе к линейному массиву блоков в ChunkData.
 *
 * @note Функция не создаёт графические объекты — только подготавливает
 * сырые данные для последующей передачи в ax::Mesh::create().
 */
inline void buildChunkMesh(const ChunkData& data,
                           const ChunkData* neighborX0,
                           const ChunkData* neighborX1,
                           const ChunkData* neighborZ0,
                           const ChunkData* neighborZ1,
                           std::vector<ChunkVertex>& vertices,
                           ax::IndexArray& indices,
                           uint16_t defaultBlockUV = 1)
{
    constexpr int CX = CHUNK_SIZE_X;  ///< Размер чанка по X (16)
    constexpr int CY = CHUNK_SIZE_Y;  ///< Размер чанка по Y (256)
    constexpr int CZ = CHUNK_SIZE_Z;  ///< Размер чанка по Z (16)

    // Предварительное резервирование под РЕАЛИСТИЧНУЮ оценку, а не теоретический максимум.
    // Раньше тут было CX*CY*CZ*24 ≈ 1.5 млн вершин (~31 МБ), которые выделялись и
    // выбрасывались на КАЖДОМ построении меша — основной источник фризов при генерации
    // (2 генерации + 2 dirty-перестройки за кадр ⇒ ~125 МБ malloc/free в кадр на старте).
    // Реальный меш чанка — единицы–десятки тысяч вершин: видимая поверхность + торцевые
    // «стены» по краям (пока не подъехали соседи). CX*CZ*128 = 32768 вершин (~640 КБ)
    // с запасом покрывает типичный случай; при недоборе std::vector дорастёт сам.
    vertices.reserve(CX * CZ * 128);

    /**
     * @brief Лямбда-проверка "сплошности" вокселя для Face Culling.
     * @param lx,ly,lz Локальные координаты проверяемого соседа
     * @return true если блок непрозрачный (грань не нужна)
     *
     * @details
     * Обрабатывает два случая:
     * 1. Сосед внутри текущего чанка — прямой доступ к data.getBlock()
     * 2. Сосед за границей чанка — запрос к соответствующему neighbor-чанку
     * 3. Вода (BLOCK_WATER) приравнивается к воздуху для отсечения граней.
          Это гарантирует, что подводные блоки террейна будут иметь полноценные грани,
          а не будут "схлопываться" из-за соседней воды.
     * Если соседний чанк не передан (nullptr) или не активен — считаем
     * область за границей "воздухом", чтобы сгенерировать торцевую грань.
     */
    auto isSolid = [&](int lx, int ly, int lz) -> bool {
        // Вложенная лямбда для безопасного чтения ID блока с учётом границ чанка
        auto getBlock = [&](int x, int y, int z) -> BlockId {
            if (x >= 0 && x < CX && y >= 0 && y < CY && z >= 0 && z < CZ)
                return data.getBlock(x, y, z);
            // Кросс-чанковые переходы: если соседний чанк не загружен, считаем воздух
            if (x < 0 && neighborX0)
                return neighborX0->getBlock(CX - 1, y, z);
            if (x >= CX && neighborX1)
                return neighborX1->getBlock(0, y, z);
            if (z < 0 && neighborZ0)
                return neighborZ0->getBlock(x, y, CZ - 1);
            if (z >= CZ && neighborZ1)
                return neighborZ1->getBlock(x, y, 0);
            return BLOCK_AIR;
        };
        BlockId b = getBlock(lx, ly, lz);
        // Твердыми считаем только блоки, не являющиеся воздухом или водой
        return b != BLOCK_AIR && b != BLOCK_WATER;
    };

    /**
     * @brief Добавляет одну грань (квад) в буферы вершин и индексов.
     * @param v0,v1,v2,v3 Вершины квада в строгом CCW-порядке
     *
     * @details
     * 1. Запоминает стартовый индекс вершин (baseIdx)
     * 2. Добавляет 4 вершины в конец вектора
     * 3. Генерирует 6 индексов для двух треугольников:
     *    - Треугольник 1: (baseIdx+0, +1, +2)
     *    - Треугольник 2: (baseIdx+0, +2, +3)
     *
     * CCW-порядок сохраняется в обоих треугольниках — критично для
     * корректного определения направления нормали и Backface Culling.
     */
    auto addFace = [&](const ChunkVertex& v0, const ChunkVertex& v1, const ChunkVertex& v2, const ChunkVertex& v3) {
        uint16_t baseIdx = static_cast<uint16_t>(vertices.size());  ///< Стартовый индекс для текущей грани
        vertices.insert(vertices.end(), {v0, v1, v2, v3});

        // Триангуляция: квад → 2 треугольника по диагонали v0→v2
        indices.emplace_back<uint16_t>(baseIdx + 0);
        indices.emplace_back<uint16_t>(baseIdx + 1);
        indices.emplace_back<uint16_t>(baseIdx + 2);
        indices.emplace_back<uint16_t>(baseIdx + 0);
        indices.emplace_back<uint16_t>(baseIdx + 2);
        indices.emplace_back<uint16_t>(baseIdx + 3);
    };

    // Обрезаем проход по высоте: выше самого высокого непустого блока чанка только
    // воздух — там геометрии заведомо нет. Для типичного террейна (~80 из 256) это
    // срезает 2/3 итераций. Грани смотрят только на блоки ЭТОГО чанка, поэтому
    // обрезка по локальному максимуму ничего не теряет.
    const int yMax = std::min(CY - 1, data.maxFilledY());

    // Цикл генерации -> Y для кэш-локальности Y-major layout
    for (int z = 0; z < CZ; ++z)
        for (int x = 0; x < CX; ++x)
            for (int y = 0; y <= yMax; ++y)
            {
                uint16_t bid = data.getBlock(x, y, z);
                if (bid == BLOCK_AIR || bid == BLOCK_WATER)
                    continue;  ///< Пропускаем воздух и воду — геометрия не нужна

                // Для мультитайлов: разные грани могут использовать разные тайлы.
                auto uv_top    = calculateTileUV(getBlockTileIndex(bid, 2));  // +Y
                auto uv_bottom = calculateTileUV(getBlockTileIndex(bid, 3));  // -Y
                auto uv_side =
                    calculateTileUV(getBlockTileIndex(bid, 0));  // боковые грани (используем face 0 как representative)
                float x0 = x, x1 = x + 1;
                float y0 = y, y1 = y + 1;
                float z0 = z, z1 = z + 1;

                // === Генерация граней по 6 направлениям ===
                // Каждая грань создаётся только если сосед — воздух (!isSolid)
                // Вершины задаются в CCW-порядке относительно внешней нормали

                // +X (правая грань, нормаль (1,0,0)) — боковая грань
                if (!isSolid(x + 1, y, z))
                    addFace({x1, y0, z0, uv_side[0], uv_side[1]}, {x1, y1, z0, uv_side[0], uv_side[3]},
                            {x1, y1, z1, uv_side[2], uv_side[3]}, {x1, y0, z1, uv_side[2], uv_side[1]});

                // -X (левая грань, нормаль (-1,0,0)) — боковая грань
                if (!isSolid(x - 1, y, z))
                    addFace({x0, y0, z1, uv_side[2], uv_side[1]}, {x0, y1, z1, uv_side[2], uv_side[3]},
                            {x0, y1, z0, uv_side[0], uv_side[3]}, {x0, y0, z0, uv_side[0], uv_side[1]});

                // +Y (верхняя грань, нормаль (0,1,0)) — верхняя текстура
                if (!isSolid(x, y + 1, z))
                    addFace({x0, y1, z1, uv_top[0], uv_top[3]}, {x1, y1, z1, uv_top[2], uv_top[3]},
                            {x1, y1, z0, uv_top[2], uv_top[1]}, {x0, y1, z0, uv_top[0], uv_top[1]});

                // -Y (нижняя грань, нормаль (0,-1,0)) — нижняя текстура
                if (!isSolid(x, y - 1, z))
                    addFace({x0, y0, z0, uv_bottom[0], uv_bottom[3]}, {x1, y0, z0, uv_bottom[2], uv_bottom[3]},
                            {x1, y0, z1, uv_bottom[2], uv_bottom[1]}, {x0, y0, z1, uv_bottom[0], uv_bottom[1]});

                // +Z (передняя грань, нормаль (0,0,1)) — боковая грань
                if (!isSolid(x, y, z + 1))
                    addFace({x0, y0, z1, uv_side[0], uv_side[1]}, {x1, y0, z1, uv_side[2], uv_side[1]},
                            {x1, y1, z1, uv_side[2], uv_side[3]}, {x0, y1, z1, uv_side[0], uv_side[3]});

                // -Z (задняя грань, нормаль (0,0,-1)) — боковая грань
                if (!isSolid(x, y, z - 1))
                    addFace({x1, y0, z0, uv_side[2], uv_side[1]}, {x0, y0, z0, uv_side[0], uv_side[1]},
                            {x0, y1, z0, uv_side[0], uv_side[3]}, {x1, y1, z0, uv_side[2], uv_side[3]});
            }
}
// ============================================================================
//  buildWaterMesh — генерация водного меша
// ============================================================================
/**
@brief Генерирует вершины и индексы для поверхности воды.
@details Отрисовывает только верхнюю грань (если сверху воздух) и боковые грани,
         контактирующие с воздухом. Использует кросс-чанковые проверки для бесшовного стыка.
         UV-координаты смещены для анимации в шейдере/коде.
*/
inline void buildWaterMesh(const ChunkData& data,
                           const ChunkData* neighborX0,
                           const ChunkData* neighborX1,
                           const ChunkData* neighborZ0,
                           const ChunkData* neighborZ1,
                           std::vector<ChunkVertex>& vertices,
                           ax::IndexArray& indices,
                           int seaLevel = 38)  ///< уровень моря активного мира (WorldConfig)
{
    constexpr int CX = CHUNK_SIZE_X;
    constexpr int CY = CHUNK_SIZE_Y;
    constexpr int CZ = CHUNK_SIZE_Z;
    // Уровень моря задаётся конфигом мира (ChunkManager::Config::seaLevel);
    // значение по умолчанию 38 соответствует прежнему поведению.
    const int SEA_LEVEL = seaLevel;

    // UV тайла воды (индекс 4 в атласе 4x4)
    auto [u, v, u2, v2] = calculateTileUV(4);

    /**
    @brief Лямбда добавления квада в буферы.
    @details Триангуляция в CCW-порядке для корректного определения внешних нормалей.
    */
    auto addFace = [&](const ChunkVertex& v0, const ChunkVertex& v1, const ChunkVertex& v2, const ChunkVertex& v3) {
        uint16_t base = static_cast<uint16_t>(vertices.size());
        vertices.insert(vertices.end(), {v0, v1, v2, v3});
        // Разбиение квада на 2 треугольника
        indices.emplace_back<uint16_t>(base + 0);
        indices.emplace_back<uint16_t>(base + 1);
        indices.emplace_back<uint16_t>(base + 2);
        indices.emplace_back<uint16_t>(base + 0);
        indices.emplace_back<uint16_t>(base + 2);
        indices.emplace_back<uint16_t>(base + 3);
    };

    /**
    @brief Кросс-чанковая проверка соседей для водного меша.
    @details Гарантирует отсутствие вертикальных граней на стыках чанков,
             если оба чанка содержат воду на одинаковой высоте.
    */
    auto getWaterNeighbor = [&](int lx, int ly, int lz) -> BlockId {
        if (lx >= 0 && lx < CX && ly >= 0 && ly < CY && lz >= 0 && lz < CZ)
            return data.getBlock(lx, ly, lz);
        if (lx < 0 && neighborX0)
            return neighborX0->getBlock(CX - 1, ly, lz);
        if (lx >= CX && neighborX1)
            return neighborX1->getBlock(0, ly, lz);
        if (lz < 0 && neighborZ0)
            return neighborZ0->getBlock(lx, ly, CZ - 1);
        if (lz >= CZ && neighborZ1)
            return neighborZ1->getBlock(lx, ly, 0);
        return BLOCK_AIR;  // За пределами карты — воздух
    };

    // Вода не поднимается выше самого высокого непустого блока чанка — обрезаем по нему.
    const int yMax = std::min(CY - 1, data.maxFilledY());

    // Порядок циксов Z→X→Y для кэш-локальности (аналогично террейну)
    for (int z = 0; z < CZ; ++z)
        for (int x = 0; x < CX; ++x)
            for (int y = 0; y <= yMax; ++y)
            {
                if (data.getBlock(x, y, z) != BLOCK_WATER)
                    continue;

                float x0 = x, x1 = x + 1.0f;
                float y0 = y, y1 = y + 0.875f;  // Вода опускается на 1/8 блока ниже кромки
                float z0 = z, z1 = z + 1.0f;

                /**
                 * @brief Фильтрация внутренних граней воды.
                 * @details Грань генерируется ТОЛЬКО если:
                 * 1. Соседний воксель — воздух (BLOCK_AIR && y >= SEA_LEVEL)
                 * 2. Высота грани >= SEA_LEVEL
                 * Это отсекает стенки в подводных пещерах, пузырьках и полостях,
                 * создавая эффект "единой прозрачной среды" при погружении.
                 * Внешняя поверхность и береговые линии остаются видимыми.
                 */

                // Верхняя грань снаружи (+Y, нормаль вверх): видна сверху, только если сверху воздух.
                // Порядок вершин CCW при взгляде сверху.
                if (getWaterNeighbor(x, y + 1, z) == BLOCK_AIR && (y + 1) >= SEA_LEVEL)
                {
                    addFace({x0, y1, z1, u, v2}, {x1, y1, z1, u2, v2}, {x1, y1, z0, u2, v}, {x0, y1, z0, u, v});

                    // Инвертированная верхняя грань (нормаль вниз): видна снизу, когда камера
                    // погружена в воду. Та же геометрия, но вершины в обратном порядке —
                    // CCW при взгляде снизу-вверх. Затеняет блоки выше уровня воды.
                    addFace({x0, y1, z0, u, v}, {x1, y1, z0, u2, v}, {x1, y1, z1, u2, v2}, {x0, y1, z1, u, v2});
                }

                // Боковые грани: рисуем только там, где соседний блок — воздух
                if (getWaterNeighbor(x + 1, y, z) == BLOCK_AIR && y >= SEA_LEVEL)
                    addFace({x1, y0, z0, u, v}, {x1, y1, z0, u, v2}, {x1, y1, z1, u2, v2}, {x1, y0, z1, u2, v});
                if (getWaterNeighbor(x - 1, y, z) == BLOCK_AIR && y >= SEA_LEVEL)
                    addFace({x0, y0, z1, u2, v}, {x0, y1, z1, u2, v2}, {x0, y1, z0, u, v2}, {x0, y0, z0, u, v});
                if (getWaterNeighbor(x, y, z + 1) == BLOCK_AIR && y >= SEA_LEVEL)
                    addFace({x0, y0, z1, u, v}, {x1, y0, z1, u2, v}, {x1, y1, z1, u2, v2}, {x0, y1, z1, u, v2});
                if (getWaterNeighbor(x, y, z - 1) == BLOCK_AIR && y >= SEA_LEVEL)
                    addFace({x1, y0, z0, u2, v}, {x0, y0, z0, u, v}, {x0, y1, z0, u, v2}, {x1, y1, z0, u2, v2});
            }
}

/// @}
/// @}
// ============================================================================

/** @name Интеграция с Axmol
 *  Функции создания графических объектов движка
 */
/// @{

/**
 * @brief Создаёт объект ax::Mesh из сырых вершин и индексов.
 *
 * @param vertices Вектор вершин формата ChunkVertex (интерливинг)
 * @param indices Индексный буфер ax::IndexArray (триангуляция)
 * @param texture Указатель на текстуру-атлас для привязки к мешу
 * @return ax::Mesh* Указатель на созданный меш или nullptr при ошибке
 *
 * @details
 * ## Механизм деинтерливинга:
 * Axmol API требует раздельные массивы атрибутов для Mesh::create(),
 * тогда как ChunkVertex хранит данные в интерливинговом формате.
 * Функция выполняет преобразование:
 *
 * @code
 * Вход:  [x0,y0,z0,u0,v0, x1,y1,z1,u1,v1, ...]  (ChunkVertex[])
 * Выход:
 *   positions[] = [x0,y0,z0, x1,y1,z1, ...]     // 3 float на вершину
 *   normals[]   = [0,1,0, 0,1,0, ...]           // заглушка для UNLIT
 *   uvs[]       = [u0,v0, u1,v1, ...]           // 2 float на вершину
 * @endcode
 *
 * ## Что делает ax::Mesh::create():
 * 1. Создаёт VBO (Vertex Buffer Object) и IBO (Index Buffer Object)
 * 2. Рассчитывает AABB (Axis-Aligned Bounding Box) для Frustum Culling
 * 3. Копирует данные в VRAM и подготавливает MeshCommand для рендерера
 *
 * @note Для UNLIT-материалов нормали-заглушки не влияют на визуализацию.
 * Если в будущем потребуется освещение — заменить на расчёт реальных нормалей.
 */
inline ax::Mesh* createMesh(const std::vector<ChunkVertex>& vertices,
                            const ax::IndexArray& indices,
                            ax::Texture2D* texture)
{
    if (vertices.empty() || indices.empty())
        return nullptr;  ///< Пустые данные — меш не создаётся

    const size_t vertCount = vertices.size();

    // === Деинтерливинг: разделение атрибутов для Axmol API ===
    std::vector<float> positions(vertCount * 3);      ///< Массив позиций: [x0,y0,z0, x1,y1,z1, ...]
    std::vector<float> normals(vertCount * 3, 0.0f);  ///< Заглушка нормалей (для UNLIT не используется)
    std::vector<float> uvs(vertCount * 2);            ///< Массив UV: [u0,v0, u1,v1, ...]

    for (size_t i = 0; i < vertCount; ++i)
    {
        positions[i * 3 + 0] = vertices[i].x;
        positions[i * 3 + 1] = vertices[i].y;
        positions[i * 3 + 2] = vertices[i].z;

        normals[i * 3 + 1] = 1.0f;  ///< Заглушка: нормаль "вверх" (не влияет на UNLIT-шейдер)

        uvs[i * 2 + 0] = vertices[i].u;
        uvs[i * 2 + 1] = vertices[i].v;
    }

    // === Сборка меша через Axmol API ===
    // Движок автоматически: создаёт буферы, считает AABB, оптимизирует данные
    auto* mesh = ax::Mesh::create(positions, normals, uvs, indices);

    if (mesh && texture)
    {
        mesh->setTexture(texture);  ///< Прямая привязка диффузной текстуры к мешу
    }

    return mesh;
}

/// @}
// ============================================================================
