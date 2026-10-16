#pragma once
#include "axmol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

/**
 * @file TextureFactory.h
 * @brief Процедурная генерация атласа террейна на лету (вместо Content/textures/terrain_atlas.png).
 *
 * Все 16 тайлов атласа 4x4 рисуются детерминированным value-noise / FBM / domain-warp
 * из сида мира — ни одного PNG на диске. Раскладка тайлов СОВМЕСТИМА со старым атласом
 * (calculateTileUV, шейдеры, инсеты не меняются):
 *
 *   0  трава (верх)    1  трава (бок)     2  земля         3  камень
 *   4  вода            5  песок           6  гравий        7  угольная руда
 *   8  железная руда   9  золотая руда   10  кристалл     11..15  запас (серый)
 *
 * Особенности:
 *   • Пиксельные элементы (соль-перец, галька, блёстки) — хэш целочисленной
 *     решётки, без RNG-состояния: один сид → одинаковый атлас на любой машине.
 *   • Края тайлов «мягкие» (bevel затемняет кромку), а UV на CPU зажаты инсетами
 *     внутрь тайла (kVoronoiUvInset/kCubeUvInset) — bleed соседних тайлов на
 *     mip-уровнях не достаёт до видимой области, как и у старого PNG-атласа.
 *   • Тонкая анимация (мерцание руд, ветер травы) живёт в voronoi_terrain.frag —
 *     здесь только статичная основа.
 */
namespace texgen
{

/// Размер атласа по умолчанию (как у старого PNG): 512x512, тайл 128x128.
constexpr int kAtlasPx = 512;
constexpr int kTiles   = 4;                    // сетка 4x4 (ATLAS_GRID в шейдерах)

/// Индексы тайлов (совпадают с getBlockTileIndex в ChunkMeshBuilder.h).
enum Tile : int
{
    TILE_GRASS_TOP = 0,
    TILE_GRASS_SIDE = 1,
    TILE_DIRT = 2,
    TILE_STONE = 3,
    TILE_WATER = 4,
    TILE_SAND = 5,
    TILE_GRAVEL = 6,
    TILE_COAL_ORE = 7,
    TILE_IRON_ORE = 8,
    TILE_GOLD_ORE = 9,
    TILE_CRYSTAL_ORE = 10,
};

// ---------------------------------------------------------------------------
//  Детерминированный шум
// ---------------------------------------------------------------------------
inline float hashL(int x, int y, uint64_t s)
{
    uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(x)) * 0x9E3779B185EBCA87ULL ^
                 static_cast<uint64_t>(static_cast<uint32_t>(y)) * 0xC2B2AE3D27D4EB4FULL ^ s;
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 31;
    return static_cast<float>(h & 0xFFFFFF) / 16777215.0f;
}

inline float smoothT(float t) { return t * t * (3.0f - 2.0f * t); }

/// Value-noise: билинейная интерполяция хэша решётки со smoothstep.
inline float vnoise(float x, float y, uint64_t s)
{
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    const float fx = x - ix, fy = y - iy;
    const float sx = smoothT(fx), sy = smoothT(fy);
    const float a = hashL(ix, iy, s), b = hashL(ix + 1, iy, s);
    const float c = hashL(ix, iy + 1, s), d = hashL(ix + 1, iy + 1, s);
    return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}

inline float fbm(float x, float y, int oct, uint64_t s, float gain = 0.5f, float lac = 2.0f)
{
    float a = 1.0f, f = 1.0f, sum = 0.0f, norm = 0.0f;
    for (int o = 0; o < oct; ++o)
    {
        sum += a * vnoise(x * f, y * f, s + static_cast<uint64_t>(o) * 101);
        norm += a;
        a *= gain;
        f *= lac;
    }
    return sum / norm;
}

/// Domain warp: органический «мраморный» рисунок вместо равномерной каши.
inline float warp(float x, float y, uint64_t s, float amt)
{
    const float qx = fbm(x, y, 4, s + 7);
    const float qy = fbm(x + 5.2f, y + 1.3f, 4, s + 13);
    return fbm(x + amt * qx, y + amt * qy, 4, s + 23);
}

// ---------------------------------------------------------------------------
//  Цветовые утилиты
// ---------------------------------------------------------------------------
using RGB = std::array<float, 3>;

inline RGB lerpRGB(const RGB& a, const RGB& b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

inline RGB mulRGB(const RGB& a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
inline RGB mulRGB(const RGB& a, const RGB& b) { return {a[0] * b[0], a[1] * b[1], a[2] * b[2]}; }

/// Пиксельный джиттер «соль-перец»: -1 / 0 / +1 по хэшу пиксельной решётки.
inline float saltPepper(float u, float v, float cells, uint64_t s, float lo = 0.04f, float hi = 0.96f)
{
    const float h = hashL(static_cast<int>(std::floor(u * cells)), static_cast<int>(std::floor(v * cells)), s);
    return (h < lo ? -1.0f : 0.0f) + (h > hi ? 1.0f : 0.0f);
}

/// Мягкая тёмная кромка по краю тайла (отделяет блоки друг от друга вдали).
inline RGB bevel(const RGB& c, float u, float v, float w, float k)
{
    const float d = std::min(std::min(u, v), std::min(1.0f - u, 1.0f - v));
    const float m = std::clamp(d / w, 0.0f, 1.0f) * (1.0f - k) + k;
    return mulRGB(c, m);
}

inline uint8_t to8(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

// ---------------------------------------------------------------------------
//  Маляры тайлов: (u, v) ∈ [0,1]², v = 1 — ВЕРХ тайла в мире (шейдер флипает V)
// ---------------------------------------------------------------------------

inline RGB grassTop(float u, float v, uint64_t seed)
{
    const float n = warp(u * 5.0f, v * 5.0f, seed, 1.1f);
    RGB col       = lerpRGB({0.30f, 0.55f, 0.18f}, {0.58f, 0.82f, 0.32f}, std::pow(n, 1.1f));
    // мшистые тёмные пятна
    if (fbm(u * 2.2f, v * 2.2f, 3, seed + 41) > 0.60f)
        col = mulRGB(col, {0.78f, 0.92f, 0.72f});
    col = mulRGB(col, 0.93f + 0.14f * fbm(u * 12.0f, v * 12.0f, 2, seed + 43));
    col = mulRGB(col, 1.0f + 0.07f * saltPepper(u, v, 128.0f, seed + 44));
    return bevel(col, u, v, 0.03f, 0.86f);
}

inline RGB dirt(float u, float v, uint64_t seed)
{
    const float n = warp(u * 5.0f, v * 5.0f, seed + 1, 1.2f);
    RGB col       = lerpRGB({0.33f, 0.22f, 0.13f}, {0.62f, 0.45f, 0.29f}, n);
    // редкая галька
    if (hashL(static_cast<int>(std::floor(u * 64)), static_cast<int>(std::floor(v * 64)), seed + 51) > 0.95f)
        col = lerpRGB({0.44f, 0.40f, 0.36f}, {0.55f, 0.52f, 0.48f}, n);
    col = mulRGB(col, 1.0f + 0.07f * saltPepper(u, v, 128.0f, seed + 52));
    return bevel(col, u, v, 0.03f, 0.86f);
}

inline RGB grassSide(float u, float v, uint64_t seed)
{
    const RGB d = dirt(u, v, seed);
    const RGB g = grassTop(u, v, seed);
    // волнистая кромка дерна сверху (v = 1 — верх)
    const float edge = 0.30f + (warp(u * 4.0f, 0.0f, seed + 71, 0.8f) - 0.5f) * 0.14f;
    const float h    = 1.0f - v;  // 0 сверху
    const float band = std::clamp((edge - h) * 8.0f, 0.0f, 1.0f);
    RGB col          = lerpRGB(d, g, band);
    // свисающие «травинки» ниже кромки
    const float drip = (fbm(u * 18.0f, v * 3.0f, 2, seed + 73) > 0.60f)
                           ? std::clamp((edge + 0.10f - h) * 7.0f, 0.0f, 1.0f)
                           : 0.0f;
    col              = lerpRGB(col, g, drip * 0.35f);
    return col;
}

inline RGB stone(float u, float v, uint64_t seed)
{
    const float strata = fbm(u * 1.5f, v * 9.0f, 4, seed + 61);  // горизонтальные слои
    const float n      = warp(u * 4.0f + strata, v * 4.0f, seed + 2, 0.9f);
    RGB col            = lerpRGB({0.32f, 0.35f, 0.41f}, {0.66f, 0.68f, 0.73f}, n);
    col                = mulRGB(col, 0.90f + 0.18f * strata);
    col                = mulRGB(col, 1.0f + 0.06f * saltPepper(u, v, 128.0f, seed + 62));
    // редкие минеральные блёстки (статичные; анимированные — в шейдере)
    if (hashL(static_cast<int>(std::floor(u * 128)), static_cast<int>(std::floor(v * 128)), seed + 63) > 0.996f)
        col = {0.85f, 0.83f, 0.70f};
    return bevel(col, u, v, 0.03f, 0.88f);
}

inline RGB water(float u, float v, uint64_t seed)
{
    const float n = fbm(u * 4.0f, v * 4.0f, 4, seed + 80);
    RGB col       = lerpRGB({0.13f, 0.32f, 0.58f}, {0.28f, 0.55f, 0.82f}, n);
    col           = mulRGB(col, 1.0f + 0.10f * (fbm(u * 9.0f, v * 9.0f, 2, seed + 85) - 0.5f));
    return col;  // без bevel: водная поверхность сплошная
}

inline RGB sand(float u, float v, uint64_t seed)
{
    const float n      = fbm(u * 5.0f, v * 5.0f, 3, seed + 90);
    RGB col            = lerpRGB({0.72f, 0.64f, 0.44f}, {0.93f, 0.85f, 0.64f}, n);
    // дюнные рябь-полосы, искривлённые warp'ом
    const float ripple = std::sin((v + warp(u * 3.0f, v * 3.0f, seed + 91, 1.0f) * 0.35f) * 40.0f);
    col                = mulRGB(col, 0.96f + 0.06f * (ripple * 0.5f + 0.5f));
    col                = mulRGB(col, 1.0f + 0.08f * saltPepper(u, v, 128.0f, seed + 92, 0.05f, 0.95f));
    return bevel(col, u, v, 0.03f, 0.88f);
}

inline RGB gravel(float u, float v, uint64_t seed)
{
    // Ячеистая «галька»: камешки 10x10 с собственным оттенком и тёмной каймой.
    // Координаты ячеек покороблены warp'ом — решётка не читается как «вафля».
    const float wu = u + 0.10f * (fbm(u * 3.0f, v * 3.0f, 3, seed + 103) - 0.5f);
    const float wv = v + 0.10f * (fbm(u * 3.0f, v * 3.0f, 3, seed + 104) - 0.5f);
    const float cells = 10.0f;
    const int cx      = static_cast<int>(std::floor(wu * cells));
    const int cy      = static_cast<int>(std::floor(wv * cells));
    const float h     = hashL(cx, cy, seed + 100);
    const float h2    = hashL(cx, cy, seed + 101);
    RGB pebble        = lerpRGB({0.34f, 0.32f, 0.30f}, {0.62f, 0.58f, 0.52f}, h);
    if (h2 > 0.75f)  // часть камешков — тёплые
        pebble = lerpRGB(pebble, {0.66f, 0.52f, 0.38f}, 0.5f);
    // расстояние до центра ячейки → тёмная кайма между камешками
    const float fx = wu * cells - cx - 0.5f, fy = wv * cells - cy - 0.5f;
    const float dist = std::sqrt(fx * fx + fy * fy) * 2.0f;  // 0 в центре, ~1.4 в углу
    RGB col          = mulRGB(pebble, 0.70f + 0.30f * (1.0f - smoothT(std::clamp(dist - 0.35f, 0.0f, 1.0f))));
    col              = mulRGB(col, 0.94f + 0.12f * fbm(u * 6.0f, v * 6.0f, 2, seed + 102));
    return bevel(col, u, v, 0.03f, 0.88f);
}

/// Общий маляр руд: каменная основа + вкрапления минерала с ярким ядром.
inline RGB ore(float u, float v, uint64_t seed, uint64_t oreSeed, const RGB& blob, const RGB& core, float thr)
{
    RGB col         = stone(u, v, seed);
    const float b   = fbm(u * 4.5f, v * 4.5f, 3, oreSeed);
    const float mVein = smoothT(std::clamp((b - thr) / 0.08f, 0.0f, 1.0f));
    const float mCore = smoothT(std::clamp((b - thr - 0.10f) / 0.12f, 0.0f, 1.0f));
    col             = lerpRGB(col, blob, mVein);
    col             = lerpRGB(col, core, mCore);
    return col;
}

inline RGB coalOre(float u, float v, uint64_t seed)
{
    // уголь — тёмно-серые (не чёрные!) вкрапления, повышенный порог → меньше площадь
    return ore(u, v, seed, seed + 110, {0.17f, 0.17f, 0.19f}, {0.10f, 0.10f, 0.11f}, 0.66f);
}
inline RGB ironOre(float u, float v, uint64_t seed)
{
    return ore(u, v, seed, seed + 120, {0.72f, 0.45f, 0.28f}, {0.86f, 0.60f, 0.38f}, 0.62f);
}
inline RGB goldOre(float u, float v, uint64_t seed)
{
    return ore(u, v, seed, seed + 130, {0.85f, 0.68f, 0.18f}, {1.00f, 0.86f, 0.36f}, 0.66f);
}
inline RGB crystalOre(float u, float v, uint64_t seed)
{
    return ore(u, v, seed, seed + 140, {0.45f, 0.85f, 0.95f}, {0.86f, 1.00f, 1.00f}, 0.68f);
}

inline RGB spare(float u, float v, uint64_t seed)
{
    const float n = fbm(u * 4.0f, v * 4.0f, 3, seed + 200);
    return lerpRGB({0.45f, 0.42f, 0.48f}, {0.60f, 0.57f, 0.63f}, n);
}

// ---------------------------------------------------------------------------
//  Растеризация атласа
// ---------------------------------------------------------------------------

/// Маляр тайла по его индексу: диспетчеризация switch'ом вместо std::function —
/// плоский C++11, дружелюбен к MSVC IntelliSense и не платит за type-erasure
/// в горячем цикле (262k вызовов на атлас).
inline RGB paintTilePx(int tile, float u, float v, uint64_t s)
{
    switch (tile)
    {
    case TILE_GRASS_TOP:   return grassTop(u, v, s);    // 0
    case TILE_GRASS_SIDE:  return grassSide(u, v, s);   // 1
    case TILE_DIRT:        return dirt(u, v, s);        // 2
    case TILE_STONE:       return stone(u, v, s);       // 3
    case TILE_WATER:       return water(u, v, s);       // 4
    case TILE_SAND:        return sand(u, v, s);        // 5
    case TILE_GRAVEL:      return gravel(u, v, s);      // 6
    case TILE_COAL_ORE:    return coalOre(u, v, s);     // 7
    case TILE_IRON_ORE:    return ironOre(u, v, s);     // 8
    case TILE_GOLD_ORE:    return goldOre(u, v, s);     // 9
    case TILE_CRYSTAL_ORE: return crystalOre(u, v, s);  // 10
    default:               return spare(u, v, s);       // 11..15 — запас
    }
}

/// Рисует тайл строго в своём слоте атласа (края не выходим — соседние тайлы
/// вплотную, защиту от bleed дают UV-инсеты на CPU и LOD-клэмп в terrain.frag).
inline void paintTile(std::vector<uint8_t>& img, int atlasPx, int tile, uint64_t seed)
{
    const int tilePx = atlasPx / kTiles;
    const int col    = tile % kTiles;
    // Строка картинки: верх PNG ↔ перевёрнутый V в шейдере (как у старого атласа).
    const int row = (kTiles - 1) - tile / kTiles;
    for (int r = 0; r < tilePx; ++r)
        for (int c = 0; c < tilePx; ++c)
        {
            const float u = (c + 0.5f) / tilePx;
            const float v = 1.0f - (r + 0.5f) / tilePx;
            const RGB rgb = paintTilePx(tile, u, v, seed);
            uint8_t* p    = &img[(static_cast<size_t>(row * tilePx + r) * atlasPx + col * tilePx + c) * 4];
            p[0]          = to8(rgb[0]);
            p[1]          = to8(rgb[1]);
            p[2]          = to8(rgb[2]);
            p[3]          = 255;
        }
}

/**
 * @brief Генерирует атлас террейна целиком в памяти и возвращает Texture2D (через TextureCache).
 * @param seed  Сид мира — узор стабилен между запусками и одинаков у всех игроков мира.
 * @param atlasPx Размер атласа (кратен 4·kGutter не требуется; по умолчанию 512).
 * @return nullptr при неудаче (вызывающий код обязан иметь fallback).
 */
inline ax::Texture2D* createTerrainAtlas(uint32_t seed, int atlasPx = kAtlasPx)
{
    const uint64_t s = seed;
    std::vector<uint8_t> img(static_cast<size_t>(atlasPx) * atlasPx * 4, 255);
    for (int t = 0; t < 16; ++t)
        paintTile(img, atlasPx, t, s);

    auto* image = new ax::Image();
    if (!image->initWithRawData(img.data(), static_cast<ssize_t>(img.size()), atlasPx, atlasPx, 8))
    {
        image->release();
        return nullptr;
    }

    // Через кэш — как PNG-путь: владение текстурой и пересоздание при context loss
    // остаются в привычном месте. Ключ фиксированный; при повторной генерации старую
    // запись сначала вытесняем (resume() после потери контекста).
    auto* cache = ax::Director::getInstance()->getTextureCache();
    cache->removeTextureForKey("proc_terrain_atlas");
    auto* tex = cache->addImage(image, "proc_terrain_atlas");
    image->release();
    if (tex)
        tex->generateMipmap();  // страховка: sampler с mipmap-фильтром без мипов = чёрный экран
    return tex;
}

}  // namespace texgen
