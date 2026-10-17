#pragma once
#include "Biome.h"         // biome::BiomeConfig, IRegionModulator
#include "ChunkManager.h"  // BlockId, BLOCK_*

#include "rapidjson/document.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * @file WorldConfig.h
 * @brief Система уровней/карт: единый JSON-конфиг миров с микро- и макро-настройками.
 *
 * Иерархия настроек:
 *   WorldCatalog (worlds.json)
 *     └─ WorldDef — один «мир/уровень» для перехода между ними:
 *          • seed, точка спавна, уровень моря;
 *          • МИКРО: geometry — смесь биомов-геометрий (кубы / Вороной / желе),
 *            terrain — профиль рельефа (высоты, бугорки, озёра),
 *            materials — палитра поверхности/подповерхности и рудные жилы;
 *          • МАКРО: regions[] — регионы мира (кольцо от спавна и/или широтная
 *            полоса север/юг) с плавными переходами: свой рельеф, свои материалы,
 *            модуляция геометрии и ПОГОДА (пока заготовка: тип + температура).
 *
 * Региональный сэмплинг — ЧИСТАЯ функция позиции (wx, wz): потокобезопасно,
 * вызывается из воркеров генерации/мешинга. Веса регионов плавные (smoothstep
 * на границах шириной blend), базовый профиль мира получает остаточный вес,
 * поэтому «горы через 1000 блоков сменяются равниной» без швов.
 *
 * Геометрия регионов работает через biome::IRegionModulator: WorldDef вешается
 * на BiomeConfig::regionModulator, и ВСЕ существующие вызовы biome::sample
 * (мешер, рейкастер-прицел) автоматически учитывают регионы.
 */
namespace worldcfg
{

/// Максимум регионов в мире. Лишние при парсинге отбрасываются (с предупреждением):
/// веса лежат в стековых массивах float[kMaxRegions] в горячем пути генерации.
constexpr size_t kMaxRegions = 16;

/// Минимальная ширина blend, блоков: blend <= 0 дал бы деление на ноль в bandWeight.
constexpr float kMinBlend = 1.0f;

// ============================================================================
//  Погода (ЗАГОТОВКА под северные/южные регионы: снег, дождь)
// ============================================================================

enum class WeatherType : uint8_t
{
    Clear,  ///< Ясно
    Rain,   ///< Дождь (заготовка)
    Snow,   ///< Снег (заготовка: снежные регионы севера/юга)
};

struct WeatherZone
{
    WeatherType type = WeatherType::Clear;
    float temperature = 15.0f;  ///< °C, влияет на будущие эффекты/осадки
    float humidity    = 0.5f;   ///< 0..1, заготовка под интенсивность осадков
};

// ============================================================================
//  Материалы: поверхность, подповерхность, рудные жилы
// ============================================================================

/// Правило рудной жилы: блок, потолок залегания, порог и частота 3D-шума.
struct OreRule
{
    BlockId block   = BLOCK_COAL_ORE;
    int maxY        = 60;      ///< жила только на y <= maxY
    float threshold = 0.80f;   ///< порог шума (выше = реже)
    float freq      = 0.14f;   ///< масштаб 3D-шума
    int octaves     = 2;
    bool enabled    = true;
};

struct MaterialPalette
{
    // --- Поверхность (приоритет как в генераторе: камень → пляж → дно → гравий → корка → основной) ---
    BlockId surfacePrimary    = BLOCK_GRASS;  ///< основной блок (трава; для снега — BLOCK_SNOW)
    BlockId surfaceBeach      = BLOCK_SAND;   ///< пляжи и дно у воды
    BlockId surfaceUnderwater = BLOCK_DIRT;   ///< дно под водой без песка
    BlockId surfaceRock       = BLOCK_STONE;  ///< каменные выступы
    BlockId surfaceGravel     = BLOCK_GRAVEL; ///< гравийные пятна
    BlockId surfaceCrust      = BLOCK_DIRT;   ///< земляная корка на суше
    BlockId subSurface        = BLOCK_DIRT;   ///< подповерхность (пляж/гравий продолжают сами себя)
    int subDepth              = 3;            ///< толщина подповерхности, блоков

    // --- Пороги/частоты шумов поверхности ---
    float rockThreshold = 0.72f, rockFreq = 0.08f;
    float dirtThreshold = 0.65f, dirtFreq = 0.06f;
    float sandThreshold = 0.42f, sandFreq = 0.05f;
    float gravelThreshold = 0.74f, gravelFreq = 0.09f;

    // --- Руды (пустой вектор = стандартный набор генератора) ---
    std::vector<OreRule> ores;
};

// ============================================================================
//  Рельеф: профиль высот + бугорки + озёра
// ============================================================================

/// Бугорки — мелкая дополнительная неровность равнин (пороговый шум).
struct HillsProfile
{
    float amplitude = 0.0f;    ///< 0 = выключены
    float freq      = 0.09f;
    float threshold = 0.55f;   ///< шум выше порога поднимает бугорок
};

/// Озёра — редкие понижения рельефа ниже уровня моря.
struct LakesProfile
{
    bool enabled     = false;
    float freq       = 0.008f;  ///< крупный шум: размер озёрных котловин
    float threshold  = 0.62f;   ///< выше порога — котловина
    float depth      = 8.0f;    ///< глубина понижения, блоков
};

struct TerrainProfile
{
    float baseHeight  = 30.0f;    ///< базовая высота поверхности
    float amplitude   = 40.0f;    ///< амплитуда основного рельефа
    float terrainFreq = 0.02f;    ///< частота основного шума
    int octaves       = 6;

    float macroOffset  = 15.0f;   ///< амплитуда крупного «биомного» сдвига
    float macroShift   = -5.0f;   ///< его постоянное смещение
    float macroFreq    = 0.005f;
    int macroOctaves   = 4;

    HillsProfile hills;           ///< бугорки (для равнин)
    LakesProfile lakes;           ///< озёра
};

/// Региональная модуляция геометрии (мультипликаторы/сдвиги curvature/softness).
struct GeometryMod
{
    float curvatureScale = 1.0f;  ///< 0 → кубы, 1 → как в мире, >1 → резче Вороной
    float softnessScale  = 1.0f;  ///< 0 → без желе
    float curvatureAdd   = 0.0f;
    float softnessAdd    = 0.0f;
};

// ============================================================================
//  Регион (МАКРО-масштаб): кольцо от спавна и/или широтная полоса
// ============================================================================

struct Region
{
    std::string name = "region";

    // Зона действия. Оба условия могут сочетаться (пересечение).
    bool useRing      = false;             ///< кольцо по расстоянию от спавна мира
    float rMin        = 0.0f, rMax = 1e30f;
    bool useLatitude  = false;             ///< широтная полоса по Z (север z<zMin... по смыслу конфига)
    float zMin = -1e30f, zMax = 1e30f;

    float blend = 100.0f;                  ///< ширина плавного перехода на границах, блоков

    // Переопределения (при парсинге стартуют с копии базовых настроек мира).
    TerrainProfile terrain;
    MaterialPalette materials;
    GeometryMod geometry;
    WeatherZone weather;
    bool hasWeather = false;
};

// ============================================================================
//  Мир / уровень
// ============================================================================

struct WorldDef : public biome::IRegionModulator
{
    std::string id   = "world";
    std::string name = "World";
    uint32_t seed    = 1337u;
    float spawnX = 0.0f, spawnZ = 0.0f;

    biome::BiomeConfig biomes{};           ///< МИКРО-геометрия: смесь кубы/Вороной/желе

    bool waterEnabled = true;
    int seaLevel      = 38;                ///< должен совпадать с уровнем в buildWaterMesh

    TerrainProfile terrain;                ///< базовый профиль (вне регионов)
    MaterialPalette materials;
    WeatherZone weather;                   ///< базовая погода мира

    std::vector<Region> regions;           ///< МАКРО-регионы

    // ---- Региональный сэмплинг (чистые функции позиции, потокобезопасно) ----

    static float smooth01(float t) { return !(t > 0) ? 0 : (t >= 1 ? 1 : t * t * (3 - 2 * t)); }  // NaN → 0

    /// Вес региона в точке: полоса [min,max] со smooth-границами шириной blend.
    static float bandWeight(float v, float vMin, float vMax, float blend)
    {
        blend = std::max(blend, kMinBlend);  // защита от деления на 0 (blend: 0 в JSON)
        float w = 1.0f;
        if (vMin > -1e29f)
            w *= smooth01((v - (vMin - blend)) / (2.0f * blend));
        if (vMax < 1e29f)
            w *= 1.0f - smooth01((v - (vMax - blend)) / (2.0f * blend));
        return w;
    }

    float regionWeight(const Region& r, double wx, double wz) const
    {
        float w = 1.0f;
        if (r.useRing)
        {
            const double dx = wx - spawnX, dz = wz - spawnZ;
            const float dist = static_cast<float>(std::sqrt(dx * dx + dz * dz));
            const float bl   = std::max(r.blend, kMinBlend);
            if (r.rMin <= 0.0f)
            {
                // Кольцо от центра: внутренней кромки нет (dist >= 0 всегда) —
                // иначе спавн получал бы половинный вес от внутренней ramp'ы.
                if (r.rMax < 1e29f)
                    w *= 1.0f - smooth01((dist - (r.rMax - bl)) / (2.0f * bl));
            }
            else
                w *= bandWeight(dist, r.rMin, r.rMax, r.blend);
        }
        if (r.useLatitude)
            w *= bandWeight(static_cast<float>(wz), r.zMin, r.zMax, r.blend);
        if (!r.useRing && !r.useLatitude)
            w = 0.0f;  // регион без зоны не действует нигде
        return w;
    }

    /// Веса регионов в точке; возвращает вес БАЗОВОГО профиля мира (остаток).
    /// outW — минимум kMaxRegions элементов; заполняются первые min(regions.size(), kMaxRegions).
    float regionWeights(double wx, double wz, float* outW) const
    {
        const size_t n = std::min(regions.size(), kMaxRegions);
        float sum      = 0.0f;
        for (size_t i = 0; i < n; ++i)
        {
            outW[i] = regionWeight(regions[i], wx, wz);
            sum += outW[i];
        }
        if (sum > 1.0f)  // перекрытие регионов: нормализуем, база получает 0
        {
            for (size_t i = 0; i < n; ++i)
                outW[i] /= sum;
            return 0.0f;
        }
        return 1.0f - sum;
    }

    /// Веса колонки (wx, wz): вычисляются ОДИН раз и переиспользуются генератором
    /// для рельефа, материалов и т.д. Σ baseW + w[i] = 1.
    struct ColumnWeights
    {
        float baseW = 1.0f;
        float w[kMaxRegions] = {};
        int n   = 0;   ///< число учтённых регионов
        int dom = -1;  ///< доминирующий регион (-1 = базовый профиль мира)
    };

    ColumnWeights sampleColumn(double wx, double wz) const
    {
        ColumnWeights c;
        c.n     = static_cast<int>(std::min(regions.size(), kMaxRegions));
        c.baseW = regionWeights(wx, wz, c.w);
        float domW = c.baseW;
        for (int i = 0; i < c.n; ++i)
            if (c.w[i] > domW)
            {
                domW  = c.w[i];
                c.dom = i;
            }
        return c;
    }

    /// Палитра доминирующего региона колонки (блоки не блендятся).
    const MaterialPalette& materialsOf(const ColumnWeights& c) const
    {
        return c.dom >= 0 ? regions[c.dom].materials : materials;
    }

    /// Модуляция геометрии для biome::sample (крючок IRegionModulator).
    void modulate(double wx, double wz, float& curvature, float& softness) const override
    {
        if (regions.empty())
            return;
        float w[kMaxRegions];
        const size_t n    = std::min(regions.size(), kMaxRegions);
        const float baseW = regionWeights(wx, wz, w);
        float cs = baseW, ss = baseW, ca = 0.0f, sa = 0.0f;  // база: scale=1, add=0
        for (size_t i = 0; i < n; ++i)
        {
            cs += w[i] * regions[i].geometry.curvatureScale;
            ss += w[i] * regions[i].geometry.softnessScale;
            ca += w[i] * regions[i].geometry.curvatureAdd;
            sa += w[i] * regions[i].geometry.softnessAdd;
        }
        curvature = std::clamp(curvature * cs + ca, 0.0f, 1.0f);
        softness  = std::clamp(softness * ss + sa, 0.0f, 1.0f);
    }

    /// Сблендированный профиль рельефа в точке (база + регионы по весам).
    /// ⚠️ Диагностика/UI: числовые поля смешаны, а частоты/октавы/пороги взяты у
    /// доминирующего региона — по такому профилю НЕЛЬЗЯ считать высоту (на границе
    /// доминирования получится обрыв). Генератор считает высоту по каждому региону
    /// отдельно и смешивает уже ВЫСОТЫ (worldgen::columnHeight).
    TerrainProfile sampleTerrain(double wx, double wz) const
    {
        if (regions.empty())
            return terrain;
        float w[kMaxRegions];
        const size_t n    = std::min(regions.size(), kMaxRegions);
        const float baseW = regionWeights(wx, wz, w);
        TerrainProfile t  = terrain;
        // Числовые поля — взвешенная сумма база*baseW + Σ w_i·region_i.
        // (Последовательный lerp «от текущего значения» был бы зависим от
        // порядка регионов и терял бы вес базы на пересечениях.)
        t.baseHeight      = terrain.baseHeight * baseW;
        t.amplitude       = terrain.amplitude * baseW;
        t.macroOffset     = terrain.macroOffset * baseW;
        t.hills.amplitude = terrain.hills.amplitude * baseW;
        t.lakes.depth     = terrain.lakes.depth * baseW;
        for (size_t i = 0; i < n; ++i)
        {
            if (w[i] <= 0.0f)
                continue;
            const Region& r = regions[i];
            t.baseHeight      += w[i] * r.terrain.baseHeight;
            t.amplitude       += w[i] * r.terrain.amplitude;
            t.macroOffset     += w[i] * r.terrain.macroOffset;
            t.hills.amplitude += w[i] * r.terrain.hills.amplitude;
            t.lakes.depth     += w[i] * r.terrain.lakes.depth;
        }
        // Дискретные/шумовые параметры — от доминирующего региона.
        int dom = -1;
        float domW = baseW;
        for (size_t i = 0; i < n; ++i)
            if (w[i] > domW)
            {
                domW = w[i];
                dom  = static_cast<int>(i);
            }
        if (dom >= 0)
        {
            const TerrainProfile& d = regions[dom].terrain;
            t.terrainFreq = d.terrainFreq;
            t.octaves     = d.octaves;
            t.macroFreq   = d.macroFreq;
            t.macroShift  = d.macroShift;
            t.macroOctaves = d.macroOctaves;
            t.hills.freq = d.hills.freq;
            t.hills.threshold = d.hills.threshold;
            t.lakes.enabled   = d.lakes.enabled;
            t.lakes.freq      = d.lakes.freq;
            t.lakes.threshold = d.lakes.threshold;
        }
        return t;
    }

    /// Материалы в точке — от доминирующего региона (блендинг блоков бессмысленен).
    const MaterialPalette& sampleMaterials(double wx, double wz) const
    {
        if (regions.empty())
            return materials;
        return materialsOf(sampleColumn(wx, wz));
    }

    /// Погода в точке (ЗАГОТОВКА): тип — от доминирующего региона,
    /// температура/влажность — сблендированы по весам.
    WeatherZone sampleWeather(double wx, double wz) const
    {
        WeatherZone out = weather;
        if (regions.empty())
            return out;
        float w[kMaxRegions];
        const size_t n    = std::min(regions.size(), kMaxRegions);
        const float baseW = regionWeights(wx, wz, w);
        float temp = baseW * weather.temperature, hum = baseW * weather.humidity;
        int dom = -1;
        float domW = baseW;
        for (size_t i = 0; i < n; ++i)
        {
            temp += w[i] * regions[i].weather.temperature;
            hum += w[i] * regions[i].weather.humidity;
            if (regions[i].hasWeather && w[i] > domW)
            {
                domW = w[i];
                dom  = static_cast<int>(i);
            }
        }
        out.temperature = temp;
        out.humidity    = hum;
        if (dom >= 0)
            out.type = regions[dom].weather.type;
        return out;
    }
};

// ============================================================================
//  Парсинг JSON
// ============================================================================

namespace detail
{

/// Предупреждения парсинга (опечатки, значения вне диапазона). Собираются на время
/// loadFromString (thread_local) и доступны через WorldCatalog::warnings().
inline std::vector<std::string>& warnings()
{
    static thread_local std::vector<std::string> w;
    return w;
}
inline void warn(std::string msg) { warnings().push_back(std::move(msg)); }

inline BlockId blockFromName(const std::string& s, BlockId fallback = BLOCK_STONE)
{
    if (s.empty())          return fallback;  // поле не задано — молча дефолт
    if (s == "air")         return BLOCK_AIR;
    if (s == "grass")       return BLOCK_GRASS;
    if (s == "stone")       return BLOCK_STONE;
    if (s == "dirt")        return BLOCK_DIRT;
    if (s == "water")       return BLOCK_WATER;
    if (s == "sand")        return BLOCK_SAND;
    if (s == "gravel")      return BLOCK_GRAVEL;
    if (s == "coal_ore")    return BLOCK_COAL_ORE;
    if (s == "iron_ore")    return BLOCK_IRON_ORE;
    if (s == "gold_ore")    return BLOCK_GOLD_ORE;
    if (s == "crystal_ore") return BLOCK_CRYSTAL_ORE;
    if (s == "snow")        return BLOCK_SNOW;
    warn("неизвестный блок '" + s + "' — использован дефолт");
    return fallback;
}

inline WeatherType weatherFromName(const std::string& s)
{
    if (s == "rain")
        return WeatherType::Rain;
    if (s == "snow")
        return WeatherType::Snow;
    if (!s.empty() && s != "clear")
        warn("неизвестная погода '" + s + "' — использована 'clear'");
    return WeatherType::Clear;
}

inline float getF(const rapidjson::Value& o, const char* k, float def)
{
    return (o.IsObject() && o.HasMember(k) && o[k].IsNumber()) ? o[k].GetFloat() : def;
}
/// Целое из JSON-числа. Дробное усекается, выход за диапазон int — клампится
/// (rapidjson::GetInt на дробном/большом числе — assert/UB).
inline int getI(const rapidjson::Value& o, const char* k, int def)
{
    if (!(o.IsObject() && o.HasMember(k) && o[k].IsNumber()))
        return def;
    const double d = o[k].GetDouble();
    if (!std::isfinite(d))
        return def;
    return static_cast<int>(std::clamp(d, static_cast<double>(INT_MIN), static_cast<double>(INT_MAX)));
}
/// getI + кламп в [lo, hi] с предупреждением, если значение пришлось поправить.
inline int getIClamped(const rapidjson::Value& o, const char* k, int def, int lo, int hi)
{
    const int v = getI(o, k, def);
    const int c = std::clamp(v, lo, hi);
    if (c != v)
        warn(std::string("'") + k + "'=" + std::to_string(v) + " вне [" + std::to_string(lo) + ", " +
             std::to_string(hi) + "] — исправлено на " + std::to_string(c));
    return c;
}
/// uint32-сид: любое целое (в т.ч. > INT_MAX и отрицательное) берётся по модулю 2^32.
inline uint32_t getSeed(const rapidjson::Value& o, const char* k, uint32_t def)
{
    if (!(o.IsObject() && o.HasMember(k) && o[k].IsNumber()))
        return def;
    const auto& v = o[k];
    if (v.IsUint64())
        return static_cast<uint32_t>(v.GetUint64());
    if (v.IsInt64())
        return static_cast<uint32_t>(v.GetInt64());
    const double d = v.GetDouble();
    if (!std::isfinite(d))
        return def;
    return static_cast<uint32_t>(static_cast<int64_t>(d));
}
inline bool getB(const rapidjson::Value& o, const char* k, bool def)
{
    return (o.IsObject() && o.HasMember(k) && o[k].IsBool()) ? o[k].GetBool() : def;
}
inline std::string getS(const rapidjson::Value& o, const char* k, const std::string& def)
{
    return (o.IsObject() && o.HasMember(k) && o[k].IsString()) ? o[k].GetString() : def;
}

inline void parseHills(const rapidjson::Value& o, HillsProfile& h)
{
    if (!o.IsObject())
        return;
    h.amplitude = getF(o, "amplitude", h.amplitude);
    h.freq      = getF(o, "freq", h.freq);
    h.threshold = getF(o, "threshold", h.threshold);
}

inline void parseLakes(const rapidjson::Value& o, LakesProfile& l)
{
    if (!o.IsObject())
        return;
    l.enabled   = getB(o, "enabled", l.enabled);
    l.freq      = getF(o, "freq", l.freq);
    l.threshold = getF(o, "threshold", l.threshold);
    l.depth     = getF(o, "depth", l.depth);
}

inline void parseTerrain(const rapidjson::Value& o, TerrainProfile& t)
{
    if (!o.IsObject())
        return;
    t.baseHeight   = getF(o, "baseHeight", t.baseHeight);
    t.amplitude    = getF(o, "amplitude", t.amplitude);
    t.terrainFreq  = getF(o, "terrainFreq", t.terrainFreq);
    t.octaves      = getIClamped(o, "octaves", t.octaves, 1, 8);
    t.macroOffset  = getF(o, "macroOffset", t.macroOffset);
    t.macroShift   = getF(o, "macroShift", t.macroShift);
    t.macroFreq    = getF(o, "macroFreq", t.macroFreq);
    t.macroOctaves = getIClamped(o, "macroOctaves", t.macroOctaves, 1, 8);
    if (o.HasMember("hills"))
        parseHills(o["hills"], t.hills);
    if (o.HasMember("lakes"))
        parseLakes(o["lakes"], t.lakes);
}

inline void parseMaterials(const rapidjson::Value& o, MaterialPalette& m)
{
    if (!o.IsObject())
        return;
    m.surfacePrimary    = blockFromName(getS(o, "surfacePrimary", ""), m.surfacePrimary);
    m.surfaceBeach      = blockFromName(getS(o, "surfaceBeach", ""), m.surfaceBeach);
    m.surfaceUnderwater = blockFromName(getS(o, "surfaceUnderwater", ""), m.surfaceUnderwater);
    m.surfaceRock       = blockFromName(getS(o, "surfaceRock", ""), m.surfaceRock);
    m.surfaceGravel     = blockFromName(getS(o, "surfaceGravel", ""), m.surfaceGravel);
    m.surfaceCrust      = blockFromName(getS(o, "surfaceCrust", ""), m.surfaceCrust);
    m.subSurface        = blockFromName(getS(o, "subSurface", ""), m.subSurface);
    m.subDepth          = getIClamped(o, "subDepth", m.subDepth, 0, 64);
    m.rockThreshold     = getF(o, "rockThreshold", m.rockThreshold);
    m.rockFreq          = getF(o, "rockFreq", m.rockFreq);
    m.dirtThreshold     = getF(o, "dirtThreshold", m.dirtThreshold);
    m.dirtFreq          = getF(o, "dirtFreq", m.dirtFreq);
    m.sandThreshold     = getF(o, "sandThreshold", m.sandThreshold);
    m.sandFreq          = getF(o, "sandFreq", m.sandFreq);
    m.gravelThreshold   = getF(o, "gravelThreshold", m.gravelThreshold);
    m.gravelFreq        = getF(o, "gravelFreq", m.gravelFreq);
    if (o.HasMember("ores") && o["ores"].IsArray())
    {
        m.ores.clear();
        for (const auto& jo : o["ores"].GetArray())
        {
            OreRule r;
            r.block     = blockFromName(getS(jo, "block", ""), r.block);
            r.maxY      = getI(jo, "maxY", r.maxY);
            r.threshold = getF(jo, "threshold", r.threshold);
            r.freq      = getF(jo, "freq", r.freq);
            r.octaves   = getIClamped(jo, "octaves", r.octaves, 1, 8);
            r.enabled   = getB(jo, "enabled", r.enabled);
            m.ores.push_back(r);
        }
    }
}

inline void parseGeometryMod(const rapidjson::Value& o, GeometryMod& g)
{
    if (!o.IsObject())
        return;
    g.curvatureScale = getF(o, "curvatureScale", g.curvatureScale);
    g.softnessScale  = getF(o, "softnessScale", g.softnessScale);
    g.curvatureAdd   = getF(o, "curvatureAdd", g.curvatureAdd);
    g.softnessAdd    = getF(o, "softnessAdd", g.softnessAdd);
}

inline void parseWeather(const rapidjson::Value& o, WeatherZone& w)
{
    if (!o.IsObject())
        return;
    w.type        = weatherFromName(getS(o, "type", "clear"));
    w.temperature = getF(o, "temperature", w.temperature);
    w.humidity    = getF(o, "humidity", w.humidity);
}

inline void parseBiomes(const rapidjson::Value& o, biome::BiomeConfig& b)
{
    if (!o.IsObject())
        return;
    b.enabled       = getB(o, "enabled", b.enabled);
    b.macroCellSize = getF(o, "macroCellSize", b.macroCellSize);
    b.blendWidth    = getF(o, "blendWidth", b.blendWidth);
    if (!(b.macroCellSize >= 8.0f))  // 0/отрицательный размер ячейки → деление на 0 в biome::sample
    {
        warn("geometry.macroCellSize < 8 — исправлено на 8");
        b.macroCellSize = 8.0f;
    }
    b.blendWidth = std::max(b.blendWidth, 0.0f);
    const std::string forced = getS(o, "forced", "");
    if (forced == "classic")
        b.forced = biome::BiomeType::Classic;
    else if (forced == "voronoi")
        b.forced = biome::BiomeType::Voronoi;
    else if (forced == "jelly")
        b.forced = biome::BiomeType::Jelly;
    if (o.HasMember("frequency") && o["frequency"].IsObject())
    {
        const auto& f = o["frequency"];
        b.frequency[static_cast<int>(biome::BiomeType::Classic)] = getF(f, "classic", b.frequency[static_cast<int>(biome::BiomeType::Classic)]);
        b.frequency[static_cast<int>(biome::BiomeType::Voronoi)] = getF(f, "voronoi", b.frequency[static_cast<int>(biome::BiomeType::Voronoi)]);
        b.frequency[static_cast<int>(biome::BiomeType::Jelly)]   = getF(f, "jelly", b.frequency[static_cast<int>(biome::BiomeType::Jelly)]);
    }
}

inline WorldDef parseWorld(const rapidjson::Value& o)
{
    WorldDef w;
    w.id   = getS(o, "id", "world");
    w.name = getS(o, "name", w.id);
    w.seed = getSeed(o, "seed", 1337u);
    w.biomes.seed = w.seed;  // сид биомов/джиттера привязан к сиду мира
    if (o.HasMember("spawn") && o["spawn"].IsObject())
    {
        w.spawnX = getF(o["spawn"], "x", 0.0f);
        w.spawnZ = getF(o["spawn"], "z", 0.0f);
    }
    if (o.HasMember("geometry"))
        parseBiomes(o["geometry"], w.biomes);
    if (o.HasMember("water") && o["water"].IsObject())
    {
        w.waterEnabled = getB(o["water"], "enabled", w.waterEnabled);
        // seaLevel — индекс слоя воды: должен лежать внутри чанка (иначе запись за границы).
        w.seaLevel     = getIClamped(o["water"], "seaLevel", w.seaLevel, 1, CHUNK_SIZE_Y - 2);
    }
    if (o.HasMember("terrain"))
        parseTerrain(o["terrain"], w.terrain);
    if (o.HasMember("materials"))
        parseMaterials(o["materials"], w.materials);
    if (o.HasMember("weather"))
        parseWeather(o["weather"], w.weather);

    if (o.HasMember("regions") && o["regions"].IsArray())
    {
        for (const auto& jr : o["regions"].GetArray())
        {
            if (w.regions.size() >= kMaxRegions)
            {
                warn("мир '" + w.id + "': регионов больше " + std::to_string(kMaxRegions) + " — лишние отброшены");
                break;
            }
            Region r;
            // Регион стартует с копии базовых настроек мира — переопределяются
            // только явно заданные поля.
            r.terrain   = w.terrain;
            r.materials = w.materials;
            r.weather   = w.weather;

            r.name  = getS(jr, "name", "region");
            r.blend = getF(jr, "blend", r.blend);
            if (!(r.blend >= kMinBlend))
            {
                warn("регион '" + r.name + "': blend < " + std::to_string(static_cast<int>(kMinBlend)) +
                     " — исправлено на минимум");
                r.blend = kMinBlend;
            }
            if (jr.HasMember("ring") && jr["ring"].IsObject())
            {
                r.useRing = true;
                r.rMin    = getF(jr["ring"], "rMin", 0.0f);
                r.rMax    = getF(jr["ring"], "rMax", 1e30f);
            }
            if (jr.HasMember("latitude") && jr["latitude"].IsObject())
            {
                r.useLatitude = true;
                r.zMin        = getF(jr["latitude"], "zMin", -1e30f);
                r.zMax        = getF(jr["latitude"], "zMax", 1e30f);
            }
            if (jr.HasMember("terrain"))
                parseTerrain(jr["terrain"], r.terrain);
            if (jr.HasMember("materials"))
                parseMaterials(jr["materials"], r.materials);
            if (jr.HasMember("geometry"))
                parseGeometryMod(jr["geometry"], r.geometry);
            if (jr.HasMember("weather"))
            {
                parseWeather(jr["weather"], r.weather);
                r.hasWeather = true;
            }
            w.regions.push_back(std::move(r));
        }
    }
    return w;
}

}  // namespace detail

// ============================================================================
//  Каталог миров — единый конфиг уровней
// ============================================================================

class WorldCatalog
{
public:
    /// Загрузка из JSON-строки. @return false при ошибке парсинга или пустом списке
    /// миров (каталог при этом пуст). Нефатальные проблемы конфига (опечатки в
    /// именах блоков, значения вне диапазона, лишние регионы) попадают в warnings().
    bool loadFromString(const std::string& json)
    {
        _worlds.clear();
        _activeId.clear();
        detail::warnings().clear();

        rapidjson::Document doc;
        doc.Parse(json.c_str());
        if (doc.HasParseError() || !doc.IsObject())
        {
            _warnings = {"worlds.json: ошибка разбора JSON"};
            return false;
        }
        if (doc.HasMember("worlds") && doc["worlds"].IsArray())
            for (const auto& jw : doc["worlds"].GetArray())
            {
                auto w = std::make_unique<WorldDef>(detail::parseWorld(jw));
                if (byId(w->id))
                    detail::warn("повторный id мира '" + w->id + "' — byId вернёт первый");
                // Крючок региональной модуляции геометрии: все biome::sample мира
                // автоматически учитывают его регионы.
                w->biomes.regionModulator = w.get();
                _worlds.push_back(std::move(w));
            }

        // activeWorld: если id не найден (опечатка) — берём первый мир, а не аварийный.
        _activeId = detail::getS(doc, "activeWorld", "");
        if (!_worlds.empty() && !byId(_activeId))
        {
            if (!_activeId.empty())
                detail::warn("activeWorld '" + _activeId + "' не найден — взят первый мир '" +
                             _worlds.front()->id + "'");
            _activeId = _worlds.front()->id;
        }
        _warnings = detail::warnings();
        return !_worlds.empty();
    }

    /// Предупреждения последней загрузки (для лога). Пусто — конфиг чистый.
    const std::vector<std::string>& warnings() const { return _warnings; }

    const WorldDef* byId(const std::string& id) const
    {
        for (const auto& w : _worlds)
            if (w->id == id)
                return w.get();
        return nullptr;
    }

    const WorldDef* active() const { return byId(_activeId); }
    const std::string& activeId() const { return _activeId; }
    bool setActive(const std::string& id)
    {
        if (!byId(id))
            return false;
        _activeId = id;
        return true;
    }

    const std::vector<std::unique_ptr<WorldDef>>& worlds() const { return _worlds; }
    size_t size() const { return _worlds.size(); }

private:
    std::vector<std::unique_ptr<WorldDef>> _worlds;
    std::string _activeId;
    std::vector<std::string> _warnings;
};

}  // namespace worldcfg
