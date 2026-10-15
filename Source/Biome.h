#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

/**
 * @file Biome.h
 * @brief Механизм биомов: тип по координатам мира + непрерывные веса для плавных границ.
 *
 * Биом — ЧИСТАЯ функция (seed, wx, wz): ничего не хранится ни в чанках, ни в сейве
 * (как и сиды ячеек Вороного). Чанку нужен только тип блока.
 *
 * Разбиение на биомы — крупная ячеистая сетка (macro-Voronoi, по умолчанию ~192 блока
 * на ячейку); каждой макро-ячейке хэшем назначается тип. Чтобы на границах не было
 * обрыва, в каждой точке считаются непрерывные ВЕСА типов:
 *
 *     raw_i = max(0, blendWidth − (d_i − d_min)),   w_type += raw_i / Σ raw
 *
 * где d_i — расстояние до сида i-й макро-ячейки. Вдали от границ вес ровно 1.0 у одного
 * типа и ровно 0.0 у остальных (не «почти 0»): это важно — геометрия классического
 * биома становится точными кубами, и меш берёт дешёвый кубический путь.
 *
 * Свойства биома задаются таблицей kTraits (без ветвлений по типам в мешере):
 *   - curvature ∈ [0,1] — амплитуда джиттера сидов Вороного (0 → ячейки = точные кубы);
 *   - softness  ∈ [0,1] — сила «желейной» деформации вершин (0 → без деформации).
 * Итоговые curvature/softness в точке = взвешенная сумма по типам, поэтому у границы
 * с классическим биомом кривизна плавно спадает до нуля, и блоки снова прямоугольные.
 */
namespace biome
{

/// Типы биомов. Reserved* — зарезервированные слоты под будущие идеи: частота по умолчанию 0
/// (в мир не попадают), свойства — как у Classic, пока для них не заполнена строка kTraits.
enum class BiomeType : uint8_t
{
    Classic   = 0,  ///< Классический: прямоугольные кубы
    Voronoi   = 1,  ///< Ячейки Вороного (многогранники); у границы с Classic кривизна → 0
    Jelly     = 2,  ///< Желейный: кубы «оплывают» от веса (визуальная заглушка, физики пока нет)
    Reserved3 = 3,
    Reserved4 = 4,
};
constexpr int kBiomeCount = 5;

struct BiomeTraits
{
    const char* name;
    float curvature;  ///< масштаб джиттера сидов Вороного: 0 — кубы, 1 — полный джиттер
    float softness;   ///< масштаб желейной деформации вершин
};

inline constexpr std::array<BiomeTraits, kBiomeCount> kTraits{{
    {"Classic", 0.0f, 0.0f},
    {"Voronoi", 1.0f, 0.0f},
    {"Jelly", 0.0f, 1.0f},
    {"Reserved3", 0.0f, 0.0f},
    {"Reserved4", 0.0f, 0.0f},
}};

struct BiomeConfig
{
    uint32_t seed = 1337u;  ///< Сид мира для биомов и джиттера ячеек (привяжите к сиду мира)

    /// false → весь мир одного типа `forced` (отладка / режим «как раньше»: Voronoi везде).
    bool enabled     = true;
    BiomeType forced = BiomeType::Voronoi;

    float macroCellSize = 192.0f;  ///< Размер макро-ячейки биомов, блоков
    float blendWidth    = 32.0f;   ///< Ширина полосы смешивания (по разности расстояний до сидов), блоков

    /// Относительные частоты выбора типа макро-ячейкой. Reserved выключены (0).
    std::array<float, kBiomeCount> frequency{{0.40f, 0.40f, 0.20f, 0.0f, 0.0f}};
};

struct BiomeSample
{
    std::array<float, kBiomeCount> weights{};  ///< Σ = 1
    float curvature = 0.0f;                    ///< Σ w_i · curvature_i
    float softness  = 0.0f;                    ///< Σ w_i · softness_i
    BiomeType dominant = BiomeType::Classic;
};

namespace detail
{
inline uint64_t hash64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

inline uint64_t cellHash(uint32_t seed, int ix, int iz)
{
    return hash64(static_cast<uint64_t>(static_cast<uint32_t>(ix)) * 0x9E3779B185EBCA87ULL ^
                  static_cast<uint64_t>(static_cast<uint32_t>(iz)) * 0xC2B2AE3D27D4EB4FULL ^
                  (static_cast<uint64_t>(seed) << 17) ^ 0x51ED270B0B1F4C5DULL);
}

inline double unit(uint64_t h, int shift)  // [0,1]
{
    return static_cast<double>((h >> shift) & 0xFFFFF) / 1048575.0;
}

inline BiomeType pickType(const BiomeConfig& c, uint64_t h)
{
    float total = 0.0f;
    for (float f : c.frequency)
        total += std::max(0.0f, f);
    if (total <= 0.0f)
        return BiomeType::Classic;
    float r = static_cast<float>(unit(h, 44)) * total;
    for (int i = 0; i < kBiomeCount; ++i)
    {
        const float f = std::max(0.0f, c.frequency[i]);
        if (r < f)
            return static_cast<BiomeType>(i);
        r -= f;
    }
    return BiomeType::Classic;
}
}  // namespace detail

/// Сид макро-ячейки (ix, iz): позиция (в блоках) и тип. Сид сдвинут от краёв ячейки (0.15..0.85),
/// чтобы ближайший сид гарантированно искался в окне 3×3.
inline void macroSeed(const BiomeConfig& c, int ix, int iz, double& sx, double& sz, BiomeType& type)
{
    const uint64_t h = detail::cellHash(c.seed, ix, iz);
    sx               = (ix + 0.15 + 0.70 * detail::unit(h, 0)) * c.macroCellSize;
    sz               = (iz + 0.15 + 0.70 * detail::unit(h, 22)) * c.macroCellSize;
    type             = detail::pickType(c, h);
}

/// Веса биомов, curvature и softness в точке мира (wx, wz). Потокобезопасно, без состояния.
inline BiomeSample sample(const BiomeConfig& c, double wx, double wz)
{
    BiomeSample s;
    if (!c.enabled)
    {
        s.weights[static_cast<int>(c.forced)] = 1.0f;
        s.curvature                           = kTraits[static_cast<int>(c.forced)].curvature;
        s.softness                            = kTraits[static_cast<int>(c.forced)].softness;
        s.dominant                            = c.forced;
        return s;
    }

    const int cx = static_cast<int>(std::floor(wx / c.macroCellSize));
    const int cz = static_cast<int>(std::floor(wz / c.macroCellSize));

    double d[9];
    BiomeType t[9];
    double dmin = 1e300;
    int n       = 0;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx, ++n)
        {
            double sx, sz;
            macroSeed(c, cx + dx, cz + dz, sx, sz, t[n]);
            d[n] = std::sqrt((wx - sx) * (wx - sx) + (wz - sz) * (wz - sz));
            dmin = std::min(dmin, d[n]);
        }

    double raw[9], sum = 0.0;
    for (int i = 0; i < 9; ++i)
    {
        raw[i] = std::max(0.0, static_cast<double>(c.blendWidth) - (d[i] - dmin));  // у ближайшего = blendWidth
        sum += raw[i];
    }
    for (int i = 0; i < 9; ++i)
        if (raw[i] > 0.0)
            s.weights[static_cast<int>(t[i])] += static_cast<float>(raw[i] / sum);

    int best = 0;
    for (int i = 0; i < kBiomeCount; ++i)
    {
        s.curvature += s.weights[i] * kTraits[i].curvature;
        s.softness += s.weights[i] * kTraits[i].softness;
        if (s.weights[i] > s.weights[best])
            best = i;
    }
    s.dominant = static_cast<BiomeType>(best);
    return s;
}

}  // namespace biome
