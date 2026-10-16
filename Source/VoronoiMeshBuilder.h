#pragma once
#include "axmol.h"
#include "Biome.h"
#include "ChunkManager.h"
#include "ChunkMeshBuilder.h"  // calculateTileUV, getBlockTileIndex
#include "cell.hh"             // Voro++: voro::voronoicell_neighbor

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <tuple>
#include <vector>

/**
 * @file VoronoiMeshBuilder.h
 * @brief Биом-зависимый меш террейна: кубы, ячейки Вороного, «желе» — в одном чанке.
 *
 * Геометрия каждого блока — ячейка Вороного его сида. Сид = центр блока + детерминированный
 * джиттер × curvature(биом в колонке блока):
 *   • curvature = 0  → сид ровно в центре → ячейка = ТОЧНЫЙ куб (диаграмма Вороного кубической
 *     решётки — это сама решётка). Это и есть «кривизна спадает на нет» у границы с классическим
 *     биомом: поле curvature непрерывно, значит сиды смещаются непрерывно, швов нет.
 *   • Если у блока и всех его соседей по кольцу 1 curvature == 0 (точное равенство, см. Biome.h),
 *     ячейка — гарантированно куб, и вместо клиппинга Voro++ берётся дешёвый путь из 6 квадов.
 *   • softness > 0 (желейный биом) → к вершинам применяется поле смещений D(p), непрерывное по
 *     позиции: общие вершины соседних ячеек смещаются одинаково → трещин нет.
 *
 * Желейное ДРОЖАНИЕ (анимация) живёт в вершинном шейдере voronoi_terrain.vert — событийная
 * модель: террейн неподвижен, а отдельные узлы решётки (целые мировые координаты) случайно
 * «вздрагивают» затухающей синусоидой по расписанию из хэша узла. Мешер запекает
 * в каждую вершину (атрибут a_texCoord1) маску дрожания k = softness·surf и нагрузку load.
 * Смещение — функция мировой позиции вершины (узел = round(worldPos)), поэтому дрожание
 * бесшовное и на границах чанков, и на границах биомов (k → 0).
 *
 * Материальные данные для «элегантного» фрагментника (a_texCoord2): вершинное AO
 * (схема side1/side2/corner от доминирующей оси нормали), индекс тайла атласа и вес
 * покачивания травы. Все трое — чистые функции НЕсмещённой позиции вершины, поэтому
 * одинаковы у всех граней, делящих угол: ни трещин, ни рассинхрона анимаций.
 *
 * Ячейки строятся только для приповерхностных блоков (твёрдый блок с воздухом/водой в окне 3×3×3).
 * Грань рисуется, если блок за ней не твёрдый. Вода/воздух — «не твёрдые».
 */
namespace voronoi
{

/// Макс. смещение сида от центра блока (в блоках) при curvature = 1. Должно быть < 0.5.
constexpr double kJitter = 0.30;

/// Радиус окна соседей, чьи сиды режут ячейку (2 → 5x5x5 = 124 кандидата).
constexpr int kRing = 2;

/// UV: масштаб планарной проекции (тайлов на блок) и отступ от края тайла (защита от bleed).
constexpr float kVoronoiUvScale = 0.8f;
constexpr float kVoronoiUvInset = 0.02f;
constexpr float kCubeUvScale    = 1.0f;
constexpr float kCubeUvInset    = 0.002f;

/// Желе: вынос поверхности по нормали и провисание под нагрузкой (в блоках), кап нагрузки (блоков сверху).
constexpr float kJellyBulge   = 0.16f;
constexpr float kJellySag     = 0.12f;
constexpr int   kJellyLoadCap = 12;

/// Страховочный лимит вершин на меш (индексы 32-битные).
constexpr size_t kMaxVertices = 4000000;

/// Вершина: позиция, UV атласа, нормаль грани (flat shading), параметры желе
/// (jm — маска дрожания k = softness·surf, jl — нагрузка 0..1; уходят в a_texCoord1
/// шейдера), а также материальные данные для «элегантного» фрагментника:
/// ao — вершинное ambient occlusion угла, tile — индекс тайла атласа,
/// sway — вес покачивания травы ветром (все трое едут в a_texCoord2).
struct Vertex
{
    float x, y, z;
    float u, v;
    float nx, ny, nz;
    float jm, jl;
    float ao, tile, sway;
};

struct Offset
{
    int dx, dy, dz;
};

/// Смещения соседей окна 5x5x5 (без нуля), по возрастанию расстояния.
inline const std::vector<Offset>& neighborOffsets()
{
    static const std::vector<Offset> offsets = [] {
        std::vector<Offset> v;
        for (int dz = -kRing; dz <= kRing; ++dz)
            for (int dy = -kRing; dy <= kRing; ++dy)
                for (int dx = -kRing; dx <= kRing; ++dx)
                    if (dx || dy || dz)
                        v.push_back({dx, dy, dz});
        std::stable_sort(v.begin(), v.end(), [](const Offset& a, const Offset& b) {
            return a.dx * a.dx + a.dy * a.dy + a.dz * a.dz < b.dx * b.dx + b.dy * b.dy + b.dz * b.dz;
        });
        return v;
    }();
    return offsets;
}

inline uint64_t mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/// Единичный джиттер сида блока (мировые координаты), каждая компонента в [-1, 1].
inline void seedJitterUnit(uint32_t seed, int gx, int gy, int gz, double& jx, double& jy, double& jz)
{
    uint64_t h = mix64(static_cast<uint64_t>(static_cast<uint32_t>(gx)) * 0x9E3779B185EBCA87ULL ^
                       static_cast<uint64_t>(static_cast<uint32_t>(gy)) * 0xC2B2AE3D27D4EB4FULL ^
                       static_cast<uint64_t>(static_cast<uint32_t>(gz)) * 0x165667B19E3779F9ULL ^ seed);
    auto f = [&](int shift) { return static_cast<double>((h >> shift) & 0xFFFFF) / 1048575.0 * 2.0 - 1.0; };
    jx = f(0);
    jy = f(20);
    jz = f(40);
}

/**
 * @brief Строит биом-зависимый меш чанка.
 *
 * @param key     Ключ чанка (мировые координаты сидов и биомов).
 * @param biomes  Конфиг биомов (+ сид мира). Чистые функции: одинаковый конфиг → одинаковая геометрия.
 * @param data    Блоки чанка.
 * @param neighborX0/X1/Z0/Z1 Соседние чанки (nullptr = не загружен → воздух). Диагональные чанки
 *                считаются воздухом (максимум — лишние невидимые грани в 4 угловых колонках).
 * @param vertices Выход: вершины (локальные координаты чанка).
 * @param indices  Выход: ОБЯЗАТЕЛЬНО ax::IndexArray(ax::backend::IndexFormat::U_INT).
 * @param allowCubePath false → кубический быстрый путь отключён, все ячейки идут через клиппинг Voro++
 *                      (для тестов/отладки: результат обязан совпасть с гибридным).
 * @return false, если упёрлись в kMaxVertices (меш усечён).
 */
inline bool buildChunkMesh(const ChunkKey& key,
                           const biome::BiomeConfig& biomes,
                           const ChunkData& data,
                           const ChunkData* neighborX0,
                           const ChunkData* neighborX1,
                           const ChunkData* neighborZ0,
                           const ChunkData* neighborZ1,
                           std::vector<Vertex>& vertices,
                           ax::IndexArray& indices,
                           bool allowCubePath = true)
{
    constexpr int CX = CHUNK_SIZE_X;
    constexpr int CY = CHUNK_SIZE_Y;
    constexpr int CZ = CHUNK_SIZE_Z;
    constexpr int R  = kRing;

    const int yMax = std::min(CY - 1, data.maxFilledY());
    if (yMax < 0)
        return true;

    // ---- 1. Паддинг-карта «твёрдости»: x,z ∈ [-R, C+R), y ∈ [-R, yMax+R] ----
    const int PX = CX + 2 * R, PZ = CZ + 2 * R, PY = yMax + 1 + 2 * R;
    const size_t N = static_cast<size_t>(PX) * PZ * PY;
    auto idx       = [&](int px, int pz, int py) { return (static_cast<size_t>(pz) * PX + px) * PY + py; };

    auto blockAt = [&](int x, int y, int z) -> BlockId {
        if (y < 0 || y >= CY)
            return BLOCK_AIR;
        const bool xin = x >= 0 && x < CX, zin = z >= 0 && z < CZ;
        if (xin && zin)
            return data.getBlock(x, y, z);
        if (xin)
        {
            const ChunkData* n = z < 0 ? neighborZ0 : neighborZ1;
            return n ? n->getBlock(x, y, z < 0 ? z + CZ : z - CZ) : BLOCK_AIR;
        }
        if (zin)
        {
            const ChunkData* n = x < 0 ? neighborX0 : neighborX1;
            return n ? n->getBlock(x < 0 ? x + CX : x - CX, y, z) : BLOCK_AIR;
        }
        return BLOCK_AIR;  // диагональный чанк
    };

    std::vector<uint8_t> solid(N, 0);
    for (int pz = 0; pz < PZ; ++pz)
        for (int px = 0; px < PX; ++px)
            for (int py = 0; py < PY; ++py)
            {
                BlockId b = blockAt(px - R, py - R, pz - R);
                solid[idx(px, pz, py)] = (b != BLOCK_AIR && b != BLOCK_WATER) ? 1 : 0;
            }

    // ---- 2. «Внутренность»: все 27 блоков окна твёрдые (сепарабельная эрозия) ----
    std::vector<uint8_t> tmp(N), interior(N);
    auto erode = [&](const std::vector<uint8_t>& src, std::vector<uint8_t>& dst, int axis) {
        const int lim       = axis == 0 ? PX : (axis == 1 ? PZ : PY);
        const size_t stride = axis == 0 ? static_cast<size_t>(PY) : (axis == 1 ? static_cast<size_t>(PX) * PY : 1);
        for (int pz = 0; pz < PZ; ++pz)
            for (int px = 0; px < PX; ++px)
                for (int py = 0; py < PY; ++py)
                {
                    const size_t i = idx(px, pz, py);
                    const int c    = axis == 0 ? px : (axis == 1 ? pz : py);
                    dst[i]         = (c >= 1 && c < lim - 1) ? (src[i - stride] & src[i] & src[i + stride]) : 0;
                }
    };
    erode(solid, tmp, 0);
    erode(tmp, interior, 1);
    erode(interior, tmp, 2);
    interior.swap(tmp);

    // ---- 3. Биомы по колонкам паддинга: curvature / softness (чистая функция мировых координат) ----
    const int baseGX = key.x * CX, baseGY = key.y * CY, baseGZ = key.z * CZ;
    const size_t NC  = static_cast<size_t>(PX) * PZ;
    std::vector<float> kap(NC), soft(NC);
    bool anySoft = false;
    for (int pz = 0; pz < PZ; ++pz)
        for (int px = 0; px < PX; ++px)
        {
            const auto s = biome::sample(biomes, baseGX + (px - R) + 0.5, baseGZ + (pz - R) + 0.5);
            kap[static_cast<size_t>(pz) * PX + px]  = s.curvature;
            soft[static_cast<size_t>(pz) * PX + px] = s.softness;
            anySoft |= s.softness > 0.0f;
        }
    // Колонка «чисто кубическая»: curvature == 0 у неё и у всех 8 соседних колонок (кольцо 1).
    // Тогда все шесть граней ячейки — плоскости x/y/z = ±0.5, а диагональные соседи куб не режут.
    std::vector<uint8_t> pureCol(NC, 0);
    for (int pz = 1; pz < PZ - 1; ++pz)
        for (int px = 1; px < PX - 1; ++px)
        {
            bool pure = true;
            for (int dz = -1; dz <= 1 && pure; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                    if (kap[static_cast<size_t>(pz + dz) * PX + (px + dx)] != 0.0f)
                    {
                        pure = false;
                        break;
                    }
            pureCol[static_cast<size_t>(pz) * PX + px] = pure ? 1 : 0;
        }

    // ---- 4. Таблица джиттера сидов (амплитуда = kJitter · curvature колонки; 0 → без хэша) ----
    std::vector<double> jit(N * 3, 0.0);
    for (int pz = 0; pz < PZ; ++pz)
        for (int px = 0; px < PX; ++px)
        {
            const float k = kap[static_cast<size_t>(pz) * PX + px];
            if (k == 0.0f)
                continue;
            const double amp = kJitter * k;
            for (int py = 0; py < PY; ++py)
            {
                double ux, uy, uz;
                seedJitterUnit(biomes.seed, baseGX + px - R, baseGY + py - R, baseGZ + pz - R, ux, uy, uz);
                double* j = &jit[idx(px, pz, py) * 3];
                j[0]      = ux * amp;
                j[1]      = uy * amp;
                j[2]      = uz * amp;
            }
        }

    // ---- 5. Желе: поле смещений узлов решётки (лениво, только если в чанке есть softness > 0) ----
    // Узел (a,b,c) — угол блоков (a-1..a, b-1..b, c-1..c) паддинга; в локальных координатах чанка
    // он лежит в (a-R, b-R, c-R). D — функция ТОЛЬКО позиции → общие вершины смещаются одинаково.
    // На узел храним 5 float: статическое смещение (3) + маска дрожания k + нагрузка load
    // (последние два — в вершинный атрибут a_texCoord1 для шейдерного дрожания).
    std::vector<uint8_t> over;       // нагрузка: сколько твёрдых блоков НАД ячейкой в колонке (кап)
    std::vector<float> nodeD;        // 5 float на узел: dx,dy,dz, маска k, нагрузка load
    std::vector<uint8_t> nodeReady;
    const int NA = PX + 1, NC2 = PZ + 1, NB = PY + 1;
    auto nidx = [&](int a, int b, int c) { return (static_cast<size_t>(c) * NA + a) * NB + b; };
    if (anySoft)
    {
        over.assign(N, 0);
        for (int pz = 0; pz < PZ; ++pz)
            for (int px = 0; px < PX; ++px)
            {
                int cnt = 0;
                for (int py = PY - 1; py >= 0; --py)
                {
                    over[idx(px, pz, py)] = static_cast<uint8_t>(std::min(cnt, kJellyLoadCap));
                    cnt += solid[idx(px, pz, py)];
                }
            }
        nodeD.assign(static_cast<size_t>(NA) * NC2 * NB * 5, 0.0f);
        nodeReady.assign(static_cast<size_t>(NA) * NC2 * NB, 0);
    }
    auto nodeDisp = [&](int a, int b, int c) -> const float* {
        a = std::clamp(a, 1, PX - 1);
        b = std::clamp(b, 1, PY - 1);
        c = std::clamp(c, 1, PZ - 1);
        const size_t ni = nidx(a, b, c);
        float* d        = &nodeD[ni * 5];
        if (nodeReady[ni])
            return d;
        nodeReady[ni] = 1;

        // 8 блоков вокруг узла: доля твёрдых, градиент твёрдости, средняя нагрузка
        int sum = 0, gx = 0, gy = 0, gz = 0, ov = 0;
        for (int dz = 0; dz <= 1; ++dz)
            for (int dy = 0; dy <= 1; ++dy)
                for (int dx = 0; dx <= 1; ++dx)
                {
                    const size_t bi = idx(a - 1 + dx, c - 1 + dz, b - 1 + dy);
                    const int s     = solid[bi];
                    sum += s;
                    gx += s * (dx ? 1 : -1);
                    gy += s * (dy ? 1 : -1);
                    gz += s * (dz ? 1 : -1);
                    ov += over[bi];
                }
        const float air  = 1.0f - static_cast<float>(sum) / 8.0f;
        const float surf = 4.0f * air * (1.0f - air);  // 1 на поверхности, 0 в массиве и в воздухе
        if (surf <= 0.0f)
            return d;
        const float load = static_cast<float>(ov) / (8.0f * kJellyLoadCap);
        // Мягкость в углу узла: биом по мировым координатам узла (чистая функция позиции)
        const float sft = biome::sample(biomes, baseGX + (a - R), baseGZ + (c - R)).softness;
        if (sft <= 0.0f)
            return d;
        // g смотрит в сторону твёрдого → «наружу» это −g. Вынос растёт с нагрузкой; провисание — вниз.
        const float k = sft * surf;
        d[0]          = -static_cast<float>(gx) * 0.25f * kJellyBulge * (0.5f + 0.5f * load) * k;
        d[1]          = (-static_cast<float>(gy) * 0.25f * kJellyBulge * (0.5f + 0.5f * load) - kJellySag * load) * k;
        d[2]          = -static_cast<float>(gz) * 0.25f * kJellyBulge * (0.5f + 0.5f * load) * k;
        d[3]          = k;     // маска шейдерного дрожания (та же весовая функция)
        d[4]          = load;  // нагрузка: в шейдере ей глушится дрожание под толщей
        return d;
    };
    // Трилинейная интерполяция поля смещений в локальной точке чанка p.
    // Попутно отдаёт интерполированные маску дрожания и нагрузку (можно передать nullptr).
    auto displace = [&](double& x, double& y, double& z, float* outMask, float* outLoad) {
        const double qx = x + R, qy = y + R, qz = z + R;  // координаты в пространстве узлов паддинга
        const int a = static_cast<int>(std::floor(qx)), b = static_cast<int>(std::floor(qy)),
                  c      = static_cast<int>(std::floor(qz));
        const double fx = qx - a, fy = qy - b, fz = qz - c;
        double dx = 0, dy = 0, dz = 0, dm = 0, dl = 0;
        for (int k = 0; k <= 1; ++k)
            for (int j = 0; j <= 1; ++j)
                for (int i = 0; i <= 1; ++i)
                {
                    const double w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy) * (k ? fz : 1 - fz);
                    if (w <= 0.0)
                        continue;
                    const float* d = nodeDisp(a + i, b + j, c + k);
                    dx += w * d[0];
                    dy += w * d[1];
                    dz += w * d[2];
                    dm += w * d[3];
                    dl += w * d[4];
                }
        x += dx;
        y += dy;
        z += dz;
        if (outMask)
            *outMask = static_cast<float>(dm);
        if (outLoad)
            *outLoad = static_cast<float>(dl);
    };

    // ---- 6. Эмиссия одной грани (общая для кубического и воронои-пути) ----
    bool truncated = false;
    auto emitFace  = [&](BlockId bid, double* px, double* py, double* pz, int n, float fnx, float fny, float fnz,
                         bool cubePath) {
        if (vertices.size() + static_cast<size_t>(n) > kMaxVertices)
        {
            truncated = true;
            return;
        }
        // Обход CCW снаружи: проверяем по Ньюэллу против номинальной нормали, при необходимости разворачиваем.
        double ax_ = 0, ay_ = 0, az_ = 0;
        for (int i = 0; i < n; ++i)
        {
            const int j = (i + 1) % n;
            ax_ += (py[i] - py[j]) * (pz[i] + pz[j]);
            ay_ += (pz[i] - pz[j]) * (px[i] + px[j]);
            az_ += (px[i] - px[j]) * (py[i] + py[j]);
        }
        if (ax_ * fnx + ay_ * fny + az_ * fnz < 0.0)
        {
            std::reverse(px, px + n);
            std::reverse(py, py + n);
            std::reverse(pz, pz + n);
        }

        // Тайл по номинальной нормали: верх / низ / бок.
        const int kind = fny > 0.5f ? 2 : (fny < -0.5f ? 3 : 0);
        const uint16_t tileIdx = getBlockTileIndex(bid, kind);
        const auto tuv = calculateTileUV(tileIdx);
        const float uvScale = cubePath ? kCubeUvScale : kVoronoiUvScale;
        const float uvInset = cubePath ? kCubeUvInset : kVoronoiUvInset;

        // UV — по НЕсмещённой геометрии: текстура «прилипает» к блоку и деформируется вместе с ним.
        double cx = 0, cy = 0, cz = 0;
        for (int i = 0; i < n; ++i)
        {
            cx += px[i];
            cy += py[i];
            cz += pz[i];
        }
        cx /= n;
        cy /= n;
        cz /= n;
        double t1x, t1y, t1z, t2x, t2y, t2z;
        if (kind != 0)
        {  // верх/низ: u ← x, v ← z
            t1x = 1; t1y = 0; t1z = 0;
            t2x = 0; t2y = 0; t2z = 1;
        }
        else
        {  // бок: u ← up × n (вправо при взгляде снаружи), v ← y
            double hx = fnz, hz = -fnx;
            const double hl = std::sqrt(hx * hx + hz * hz);
            hx = hl > 1e-6 ? hx / hl : 1.0;
            hz = hl > 1e-6 ? hz / hl : 0.0;
            t1x = hx; t1y = 0; t1z = hz;
            t2x = 0; t2y = 1; t2z = 0;
        }
        float U[16], V[16];
        const int m = std::min(n, 16);
        for (int i = 0; i < m; ++i)
        {
            const double dx = px[i] - cx, dy = py[i] - cy, dz = pz[i] - cz;
            float u = 0.5f + static_cast<float>(dx * t1x + dy * t1y + dz * t1z) * uvScale;
            float v = 0.5f + static_cast<float>(dx * t2x + dy * t2y + dz * t2z) * uvScale;
            u       = std::clamp(u, uvInset, 1.0f - uvInset);
            v       = std::clamp(v, uvInset, 1.0f - uvInset);
            U[i]    = tuv[0] + u * (tuv[2] - tuv[0]);
            V[i]    = tuv[1] + v * (tuv[3] - tuv[1]);
        }

        // --- Вершинное AO и вес ветра травы ---------------------------------
        // Считаются по НЕсмещённой (кубической) геометрии ДО желейной деформации
        // и являются ЧИСТЫМИ функциями позиции вершины: все грани, делящие угол,
        // получают одно и то же значение → ни AO, ни sway не создают трещин.
        // AO — классическая схема «side1/side2/corner» от доминирующей оси
        // номинальной нормали; sway = 1, если блок ПОД вершиной — трава.
        const float anx  = std::fabs(fnx), anyy = std::fabs(fny), anz = std::fabs(fnz);
        const int nAxis  = (anyy >= anx && anyy >= anz) ? 1 : (anx >= anz ? 0 : 2);
        const float nSgn = (nAxis == 0 ? fnx : (nAxis == 1 ? fny : fnz)) >= 0.0f ? 1.0f : -1.0f;
        const int t1     = (nAxis + 1) % 3, t2 = (nAxis + 2) % 3;
        auto axisOf      = [](double x, double y, double z, int a) { return a == 0 ? x : (a == 1 ? y : z); };
        auto occludes    = [&](int bx, int by, int bz) {
            BlockId b = blockAt(bx, by, bz);
            return b != BLOCK_AIR && b != BLOCK_WATER;
        };
        float AO[16], SWAY[16];
        for (int i = 0; i < m; ++i)
        {
            // Детерминированная «ячейка угла»: сдвиг −0.5 одинаков для всех
            // граней, делящих вершину (координаты углов целые/близки к целым).
            const int cx = static_cast<int>(std::floor(px[i] - 0.5));
            const int cy = static_cast<int>(std::floor(py[i] - 0.5));
            const int cz = static_cast<int>(std::floor(pz[i] - 0.5));
            const int cc[3] = {cx, cy, cz};

            // Воздушная ячейка перед гранью (шаг наружу по нормальной оси).
            int air[3] = {cx, cy, cz};
            air[nAxis] = nSgn > 0.0f ? cc[nAxis] + 1 : cc[nAxis];

            // Знаки угла по касательным осям (fract(p − 0.5) ≥ 0.5 → +1).
            const auto cornerSign = [&](int a) {
                const double p  = axisOf(px[i], py[i], pz[i], a);
                const double fr = (p - 0.5) - std::floor(p - 0.5);
                return fr >= 0.5 ? 1 : -1;
            };
            int s1[3] = {0, 0, 0}, s2[3] = {0, 0, 0};
            s1[t1] = cornerSign(t1);
            s2[t2] = cornerSign(t2);

            const bool o1 = occludes(air[0] + s1[0], air[1] + s1[1], air[2] + s1[2]);
            const bool o2 = occludes(air[0] + s2[0], air[1] + s2[1], air[2] + s2[2]);
            const bool oc = occludes(air[0] + s1[0] + s2[0], air[1] + s1[1] + s2[1], air[2] + s1[2] + s2[2]);
            // Классика: обе боковые стены закрыты → максимальное затенение угла.
            AO[i] = (o1 && o2) ? 0.45f : 1.0f - 0.18f * static_cast<float>(o1 + o2 + oc);

            // Ветер травы: вершина качается, если блок под ней — трава.
            // Верхняя кромка травяного блока (любая грань, делящая угол) получает
            // одинаковый вес → покачивание бесшовное.
            SWAY[i] = blockAt(cx, cy, cz) == BLOCK_GRASS ? 1.0f : 0.0f;
        }

        // Желейная деформация вершин (непрерывное поле по позиции) + запекание
        // маски дрожания и нагрузки в вершинный атрибут (для шейдерной анимации).
        float JM[16] = {}, JL[16] = {};
        if (anySoft)
            for (int i = 0; i < m; ++i)
                displace(px[i], py[i], pz[i], &JM[i], &JL[i]);

        // Нормаль грани: по итоговому (возможно смещённому) полигону; запасной вариант — номинальная.
        double nnx = 0, nny = 0, nnz = 0;
        for (int i = 0; i < m; ++i)
        {
            const int j = (i + 1) % m;
            nnx += (py[i] - py[j]) * (pz[i] + pz[j]);
            nny += (pz[i] - pz[j]) * (px[i] + px[j]);
            nnz += (px[i] - px[j]) * (py[i] + py[j]);
        }
        const double nl = std::sqrt(nnx * nnx + nny * nny + nnz * nnz);
        float ox = fnx, oy = fny, oz = fnz;
        if (nl > 1e-9)
        {
            ox = static_cast<float>(nnx / nl);
            oy = static_cast<float>(nny / nl);
            oz = static_cast<float>(nnz / nl);
        }

        const uint32_t base = static_cast<uint32_t>(vertices.size());
        for (int i = 0; i < m; ++i)
            vertices.push_back({static_cast<float>(px[i]), static_cast<float>(py[i]), static_cast<float>(pz[i]), U[i],
                                V[i], ox, oy, oz, JM[i], JL[i], AO[i], static_cast<float>(tileIdx), SWAY[i]});
        for (int i = 1; i + 1 < m; ++i)  // веер (грань выпуклая)
        {
            indices.emplace_back<uint32_t>(base);
            indices.emplace_back<uint32_t>(base + static_cast<uint32_t>(i));
            indices.emplace_back<uint32_t>(base + static_cast<uint32_t>(i) + 1u);
        }
    };

    // ---- 7. Ячейки приповерхностных блоков ----
    static const int kCube[6][4][3] = {
        {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}},  // +X
        {{0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {0, 0, 0}},  // -X
        {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}},  // +Y
        {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},  // -Y
        {{1, 0, 1}, {1, 1, 1}, {0, 1, 1}, {0, 0, 1}},  // +Z
        {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}},  // -Z
    };
    static const int kDir[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

    const auto& offsets = neighborOffsets();
    voro::voronoicell_neighbor cell;
    std::vector<double> vs, nm;
    std::vector<int> fv, nb;

    for (int z = 0; z < CZ && !truncated; ++z)
        for (int x = 0; x < CX && !truncated; ++x)
        {
            const size_t col = static_cast<size_t>(z + R) * PX + (x + R);
            const bool pure  = allowCubePath && pureCol[col] != 0;
            for (int y = 0; y <= yMax && !truncated; ++y)
            {
                const BlockId bid = data.getBlock(x, y, z);
                if (bid == BLOCK_AIR || bid == BLOCK_WATER)
                    continue;
                if (interior[idx(x + R, z + R, y + R)])
                    continue;

                if (pure)
                {
                    // --- Кубический путь: 6 квадов, видимые, если сосед не твёрдый ---
                    for (int f = 0; f < 6; ++f)
                    {
                        if (solid[idx(x + kDir[f][0] + R, z + kDir[f][2] + R, y + kDir[f][1] + R)])
                            continue;
                        double px[4], py[4], pz[4];
                        for (int i = 0; i < 4; ++i)
                        {
                            px[i] = x + kCube[f][i][0];
                            py[i] = y + kCube[f][i][1];
                            pz[i] = z + kCube[f][i][2];
                        }
                        emitFace(bid, px, py, pz, 4, static_cast<float>(kDir[f][0]), static_cast<float>(kDir[f][1]),
                                 static_cast<float>(kDir[f][2]), true);
                    }
                    continue;
                }

                // --- Путь Вороного: ячейка клиппингом полуплоскостей до сидов соседей ---
                const double* j0 = &jit[idx(x + R, z + R, y + R) * 3];
                const double j0x = j0[0], j0y = j0[1], j0z = j0[2];

                // Ячейка строится в системе координат с началом в собственном сиде.
                cell.init(-2.0, 2.0, -2.0, 2.0, -2.0, 2.0);
                double mrs = cell.max_radius_squared();
                bool ok    = true;
                for (size_t k = 0; k < offsets.size(); ++k)
                {
                    const Offset& o = offsets[k];
                    const double* jn = &jit[idx(x + o.dx + R, z + o.dz + R, y + o.dy + R) * 3];
                    const double rx = o.dx + (jn[0] - j0x), ry = o.dy + (jn[1] - j0y), rz = o.dz + (jn[2] - j0z);
                    const double rsq = rx * rx + ry * ry + rz * rz;
                    // Voro++ хранит вершины в УДВОЕННЫХ координатах: max_radius_squared() = 4·R².
                    // Плоскость на расстоянии |r|/2 заденет ячейку только при |r|² < 4·R² == mrs.
                    if (rsq > mrs + 1e-9)
                        continue;
                    if (!cell.nplane(rx, ry, rz, rsq, static_cast<int>(k)))
                    {
                        ok = false;
                        break;
                    }
                    mrs = cell.max_radius_squared();
                }
                if (!ok)
                    continue;

                cell.vertices(vs);
                cell.face_vertices(fv);
                cell.neighbors(nb);
                cell.normals(nm);

                const double sx = x + 0.5 + j0x, sy = y + 0.5 + j0y, sz = z + 0.5 + j0z;

                size_t pos = 0;
                for (size_t fi = 0; pos < fv.size() && !truncated; ++fi)
                {
                    const int cnt  = fv[pos];
                    const int* fvi = &fv[pos + 1];
                    pos += static_cast<size_t>(cnt) + 1;

                    const int k = nb[fi];
                    if (k < 0 || cnt < 3)
                        continue;  // стенка начального бокса (не должна случаться) / вырожденная грань
                    const Offset& o = offsets[static_cast<size_t>(k)];
                    if (solid[idx(x + o.dx + R, z + o.dz + R, y + o.dy + R)])
                        continue;  // за гранью твёрдый блок — не видна

                    double px[16], py[16], pz[16];
                    const int n = std::min(cnt, 16);
                    for (int i = 0; i < n; ++i)
                    {
                        px[i] = sx + vs[3 * fvi[i] + 0];
                        py[i] = sy + vs[3 * fvi[i] + 1];
                        pz[i] = sz + vs[3 * fvi[i] + 2];
                    }
                    emitFace(bid, px, py, pz, n, static_cast<float>(nm[3 * fi + 0]), static_cast<float>(nm[3 * fi + 1]),
                             static_cast<float>(nm[3 * fi + 2]), false);
                }
            }
        }
    return !truncated;
}

/// Акцессор блока по ЦЕЛЫМ мировым координатам (для одиночных запросов геометрии).
using WorldBlockAccessor = std::function<BlockId(int gx, int gy, int gz)>;

/**
 * @brief Точная геометрия видимой грани одного блока — для прицела/подсветки.
 *
 * Воспроизводит buildChunkMesh для единственной грани блока (bx,by,bz) в направлении
 * (dirX,dirY,dirZ):
 *   • «чистая» колонка (curvature == 0 у блока и кольца 1) → квадрат грани куба;
 *   • иначе — клиппинг ячейки Вороного полуплоскостями до сидов окна 5×5×5 с тем же
 *     джиттером (seedJitterUnit · curvature), выбор грани к соседу (dir) — той самой,
 *     которую эмитирует билдер чанка;
 *   • желейное статическое смещение D(p) — то же поле узлов решётки (surf/load/softness),
 *     трилинейно интерполированное в вершинах;
 *   • в вершины запекаются маска дрожания jm, нагрузка jl (a_texCoord1) и вес ветра
 *     sway (a_texCoord2.z) — с ними шейдер подсветки (highlight.vert) анимирует
 *     полигон СИНХРОННО с террейном.
 *
 * Вершины — в МИРОВЫХ координатах (рендерьте нодом в начале координат и задайте
 * u_chunkOrigin = 0, тогда хэши узлов в шейдере совпадут с террейном).
 *
 * @param pushOut    вынос полигона по итоговой нормали (защита от z-fighting).
 * @param insetScale масштаб полигона к центроиду ПОСЛЕ деформации (1.0 — полная грань;
 *                   <1 — для индикатора прогресса разрушения).
 * @return false, если грань не существует (например, срезана соседней ячейкой) —
 *         вызывающий код должен откатиться на сеточный квад.
 */
inline bool buildBlockFace(const biome::BiomeConfig& biomes,
                           const WorldBlockAccessor& blockAtWorld,
                           int bx,
                           int by,
                           int bz,
                           int dirX,
                           int dirY,
                           int dirZ,
                           float pushOut,
                           float insetScale,
                           std::vector<Vertex>& outVerts,
                           ax::IndexArray& outIndices)
{
    auto solidAtWorld = [&](int x, int y, int z) {
        BlockId b = blockAtWorld(x, y, z);
        return b != BLOCK_AIR && b != BLOCK_WATER;
    };

    const float fnx = static_cast<float>(dirX), fny = static_cast<float>(dirY),
                fnz = static_cast<float>(dirZ);

    // ---- 1. «Чистая» колонка: curvature == 0 у колонки блока и 8 соседей (как pureCol) ----
    bool pure = true;
    for (int dz = -1; dz <= 1 && pure; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
            if (biome::sample(biomes, bx + dx + 0.5, bz + dz + 0.5).curvature != 0.0f)
            {
                pure = false;
                break;
            }

    double px[16], py[16], pz[16];
    int n = 0;

    if (pure)
    {
        // ---- 2a. Кубический путь: квадрат грани (та же таблица kCube, что у билдера) ----
        static const int kCube[6][4][3] = {
            {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}},  // +X
            {{0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {0, 0, 0}},  // -X
            {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}},  // +Y
            {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},  // -Y
            {{1, 0, 1}, {1, 1, 1}, {0, 1, 1}, {0, 0, 1}},  // +Z
            {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}},  // -Z
        };
        const int f = dirY > 0 ? 2 : (dirY < 0 ? 3 : (dirX > 0 ? 0 : (dirX < 0 ? 1 : (dirZ > 0 ? 4 : 5))));
        for (int i = 0; i < 4; ++i)
        {
            px[i] = bx + kCube[f][i][0];
            py[i] = by + kCube[f][i][1];
            pz[i] = bz + kCube[f][i][2];
        }
        n = 4;
    }
    else
    {
        // ---- 2b. Путь Вороного: ячейка клиппингом полуплоскостей до сидов соседей ----
        // Джиттер сида по мировым координатам — тот же, что в таблице jit билдера.
        auto seedJit = [&](int gx, int gy, int gz, double* j) {
            j[0] = j[1] = j[2] = 0.0;
            const float k = biome::sample(biomes, gx + 0.5, gz + 0.5).curvature;
            if (k == 0.0f)
                return;
            double ux, uy, uz;
            seedJitterUnit(biomes.seed, gx, gy, gz, ux, uy, uz);
            const double amp = kJitter * static_cast<double>(k);
            j[0]         = ux * amp;
            j[1]         = uy * amp;
            j[2]         = uz * amp;
        };

        const auto& offsets = neighborOffsets();
        voro::voronoicell_neighbor cell;
        cell.init(-2.0, 2.0, -2.0, 2.0, -2.0, 2.0);
        double mrs = cell.max_radius_squared();
        double j0[3];
        seedJit(bx, by, bz, j0);
        bool ok = true;
        for (size_t k = 0; k < offsets.size(); ++k)
        {
            const Offset& o = offsets[k];
            double jn[3];
            seedJit(bx + o.dx, by + o.dy, bz + o.dz, jn);
            const double rx = o.dx + (jn[0] - j0[0]), ry = o.dy + (jn[1] - j0[1]), rz = o.dz + (jn[2] - j0[2]);
            const double rsq = rx * rx + ry * ry + rz * rz;
            // Как в билдере: плоскость на расстоянии |r|/2 заденет ячейку при |r|² < mrs.
            if (rsq > mrs + 1e-9)
                continue;
            if (!cell.nplane(rx, ry, rz, rsq, static_cast<int>(k)))
            {
                ok = false;
                break;
            }
            mrs = cell.max_radius_squared();
        }
        if (!ok)
            return false;

        std::vector<double> vs, nm;
        std::vector<int> fv, nb;
        cell.vertices(vs);
        cell.face_vertices(fv);
        cell.neighbors(nb);
        cell.normals(nm);

        const double sx = bx + 0.5 + j0[0], sy = by + 0.5 + j0[1], sz = bz + 0.5 + j0[2];

        // Искомая грань — к соседу ровно (dirX,dirY,dirZ): ту грань эмитирует билдер чанка.
        // Запасной вариант (грань срезана джиттером чужих сидов) — максимум скалярки с dir
        // среди граней с нетвёрдым соседом.
        int bestFi     = -1;
        double bestDot = -1e30;
        size_t pos     = 0;
        for (size_t fi = 0; pos < fv.size(); ++fi)
        {
            const int cnt = fv[pos];
            pos += static_cast<size_t>(cnt) + 1;
            const int k = nb[fi];
            if (k < 0 || cnt < 3)
                continue;
            const Offset& o = offsets[static_cast<size_t>(k)];
            if (solidAtWorld(bx + o.dx, by + o.dy, bz + o.dz))
                continue;  // за гранью твёрдый блок — билдер её не рисует
            const double d =
                nm[3 * fi + 0] * fnx + nm[3 * fi + 1] * fny + nm[3 * fi + 2] * fnz;
            if (o.dx == dirX && o.dy == dirY && o.dz == dirZ)
            {
                bestFi  = static_cast<int>(fi);
                bestDot = d;
                break;
            }
            if (d > bestDot)
            {
                bestDot = d;
                bestFi  = static_cast<int>(fi);
            }
        }
        if (bestFi < 0)
            return false;

        pos = 0;
        for (int fi = 0; fi <= bestFi; ++fi)
        {
            const int cnt = fv[pos];
            if (fi == bestFi)
            {
                const int* fvi = &fv[pos + 1];
                n              = std::min(cnt, 16);
                for (int i = 0; i < n; ++i)
                {
                    px[i] = sx + vs[3 * fvi[i] + 0];
                    py[i] = sy + vs[3 * fvi[i] + 1];
                    pz[i] = sz + vs[3 * fvi[i] + 2];
                }
            }
            pos += static_cast<size_t>(cnt) + 1;
        }
        if (n < 3)
            return false;
    }

    // ---- 3. Обход CCW снаружи: проверка по Ньюэллу против номинальной нормали ----
    {
        double ax_ = 0, ay_ = 0, az_ = 0;
        for (int i = 0; i < n; ++i)
        {
            const int j = (i + 1) % n;
            ax_ += (py[i] - py[j]) * (pz[i] + pz[j]);
            ay_ += (pz[i] - pz[j]) * (px[i] + px[j]);
            az_ += (px[i] - px[j]) * (py[i] + py[j]);
        }
        if (ax_ * fnx + ay_ * fny + az_ * fnz < 0.0)
        {
            std::reverse(px, px + n);
            std::reverse(py, py + n);
            std::reverse(pz, pz + n);
        }
    }

    // ---- 4. UV: планарная проекция по НЕсмещённой геометрии (как emitFace) ----
    double cx = 0, cy = 0, cz = 0;
    for (int i = 0; i < n; ++i)
    {
        cx += px[i];
        cy += py[i];
        cz += pz[i];
    }
    cx /= n;
    cy /= n;
    cz /= n;
    const int kind  = fny > 0.5f ? 2 : (fny < -0.5f ? 3 : 0);
    double t1x, t1y, t1z, t2x, t2y, t2z;
    if (kind != 0)
    {
        t1x = 1; t1y = 0; t1z = 0;
        t2x = 0; t2y = 0; t2z = 1;
    }
    else
    {
        double hx       = fnz, hz = -fnx;
        const double hl = std::sqrt(hx * hx + hz * hz);
        hx              = hl > 1e-6 ? hx / hl : 1.0;
        hz              = hl > 1e-6 ? hz / hl : 0.0;
        t1x = hx; t1y = 0; t1z = hz;
        t2x = 0; t2y = 1; t2z = 0;
    }
    const float uvScale = pure ? kCubeUvScale : kVoronoiUvScale;
    const float uvInset = pure ? kCubeUvInset : kVoronoiUvInset;
    float U[16], V[16];
    for (int i = 0; i < n; ++i)
    {
        const double dx = px[i] - cx, dy = py[i] - cy, dz = pz[i] - cz;
        float u = 0.5f + static_cast<float>(dx * t1x + dy * t1y + dz * t1z) * uvScale;
        float v = 0.5f + static_cast<float>(dx * t2x + dy * t2y + dz * t2z) * uvScale;
        U[i]    = std::clamp(u, uvInset, 1.0f - uvInset);
        V[i]    = std::clamp(v, uvInset, 1.0f - uvInset);
    }

    // ---- 5. Вес покачивания травы (чистая функция НЕсмещённой позиции, как в билдере) ----
    float SWAY[16];
    for (int i = 0; i < n; ++i)
    {
        const int wx = static_cast<int>(std::floor(px[i] - 0.5));
        const int wy = static_cast<int>(std::floor(py[i] - 0.5));
        const int wz = static_cast<int>(std::floor(pz[i] - 0.5));
        SWAY[i]      = blockAtWorld(wx, wy, wz) == BLOCK_GRASS ? 1.0f : 0.0f;
    }

    // ---- 6. Желейное статическое смещение: то же поле узлов решётки, что в билдере ----
    // Узел (a,b,c) — угол блоков (a-1..a, b-1..b, c-1..c) в МИРОВЫХ координатах.
    // Ленивый кэш: грань делит узлы между вершинами.
    std::map<std::tuple<int, int, int>, std::array<float, 5>> nodeCache;
    auto nodeDisp = [&](int a, int b, int c, float* d) {
        const auto key = std::make_tuple(a, b, c);
        if (auto it = nodeCache.find(key); it != nodeCache.end())
        {
            std::memcpy(d, it->second.data(), 5 * sizeof(float));
            return;
        }
        std::array<float, 5> v{0, 0, 0, 0, 0};

        int sum = 0, gx = 0, gy = 0, gz = 0;
        for (int dz = 0; dz <= 1; ++dz)
            for (int dy = 0; dy <= 1; ++dy)
                for (int dx = 0; dx <= 1; ++dx)
                {
                    const int s = solidAtWorld(a - 1 + dx, b - 1 + dy, c - 1 + dz) ? 1 : 0;
                    sum += s;
                    gx += s * (dx ? 1 : -1);
                    gy += s * (dy ? 1 : -1);
                    gz += s * (dz ? 1 : -1);
                }
        const float air  = 1.0f - static_cast<float>(sum) / 8.0f;
        const float surf = 4.0f * air * (1.0f - air);
        if (surf > 0.0f)
        {
            const float sft = biome::sample(biomes, static_cast<double>(a), static_cast<double>(c)).softness;
            if (sft > 0.0f)
            {
                // Нагрузка: число твёрдых блоков НАД блоком в его колонке чанка
                // (кап kJellyLoadCap) — как over[] билдера. Скан вверх до потолка чанка.
                int ov = 0;
                for (int dz = 0; dz <= 1; ++dz)
                    for (int dy = 0; dy <= 1; ++dy)
                        for (int dx = 0; dx <= 1; ++dx)
                        {
                            const int wx = a - 1 + dx, wy = b - 1 + dy, wz = c - 1 + dz;
                            const int yTop =
                                (static_cast<int>(std::floor(static_cast<double>(wy) / CHUNK_SIZE_Y)) + 1) *
                                    CHUNK_SIZE_Y -
                                1;
                            int cnt = 0;
                            for (int yy = wy + 1; yy <= yTop && cnt < kJellyLoadCap; ++yy)
                                if (solidAtWorld(wx, yy, wz))
                                    ++cnt;
                            ov += cnt;
                        }
                const float load = static_cast<float>(ov) / (8.0f * kJellyLoadCap);
                const float k    = sft * surf;
                v[0] = -static_cast<float>(gx) * 0.25f * kJellyBulge * (0.5f + 0.5f * load) * k;
                v[1] = (-static_cast<float>(gy) * 0.25f * kJellyBulge * (0.5f + 0.5f * load) - kJellySag * load) * k;
                v[2] = -static_cast<float>(gz) * 0.25f * kJellyBulge * (0.5f + 0.5f * load) * k;
                v[3] = k;
                v[4] = load;
            }
        }
        nodeCache.emplace(key, v);
        std::memcpy(d, v.data(), 5 * sizeof(float));
    };

    // Трилинейная интерполяция поля смещений (эквивалент displace() билдера в мировых
    // координатах: узел = floor(p), доля = fract(p)).
    float JM[16] = {}, JL[16] = {};
    for (int i = 0; i < n; ++i)
    {
        const int a = static_cast<int>(std::floor(px[i])), b = static_cast<int>(std::floor(py[i])),
                  c = static_cast<int>(std::floor(pz[i]));
        const double fx = px[i] - a, fy = py[i] - b, fz = pz[i] - c;
        double dx = 0, dy = 0, dz = 0, dm = 0, dl = 0;
        float d[5];
        for (int k = 0; k <= 1; ++k)
            for (int j = 0; j <= 1; ++j)
                for (int ii = 0; ii <= 1; ++ii)
                {
                    const double w = (ii ? fx : 1 - fx) * (j ? fy : 1 - fy) * (k ? fz : 1 - fz);
                    if (w <= 0.0)
                        continue;
                    nodeDisp(a + ii, b + j, c + k, d);
                    dx += w * d[0];
                    dy += w * d[1];
                    dz += w * d[2];
                    dm += w * d[3];
                    dl += w * d[4];
                }
        px[i] += dx;
        py[i] += dy;
        pz[i] += dz;
        JM[i] = static_cast<float>(dm);
        JL[i] = static_cast<float>(dl);
    }

    // ---- 7. Нормаль по итоговому (смещённому) полигону; запасной вариант — номинальная ----
    double nnx = 0, nny = 0, nnz = 0;
    for (int i = 0; i < n; ++i)
    {
        const int j = (i + 1) % n;
        nnx += (py[i] - py[j]) * (pz[i] + pz[j]);
        nny += (pz[i] - pz[j]) * (px[i] + px[j]);
        nnz += (px[i] - px[j]) * (py[i] + py[j]);
    }
    const double nl = std::sqrt(nnx * nnx + nny * nny + nnz * nnz);
    float ox = fnx, oy = fny, oz = fnz;
    if (nl > 1e-9)
    {
        ox = static_cast<float>(nnx / nl);
        oy = static_cast<float>(nny / nl);
        oz = static_cast<float>(nnz / nl);
    }

    // ---- 8. Масштаб к центроиду (прогресс разрушения) + вынос по нормали (z-fight) ----
    {
        double dcx = 0, dcy = 0, dcz = 0;
        for (int i = 0; i < n; ++i)
        {
            dcx += px[i];
            dcy += py[i];
            dcz += pz[i];
        }
        dcx /= n;
        dcy /= n;
        dcz /= n;
        const double s = std::clamp(static_cast<double>(insetScale), 0.0, 1.0);
        for (int i = 0; i < n; ++i)
        {
            px[i] = dcx + (px[i] - dcx) * s + static_cast<double>(ox) * pushOut;
            py[i] = dcy + (py[i] - dcy) * s + static_cast<double>(oy) * pushOut;
            pz[i] = dcz + (pz[i] - dcz) * s + static_cast<double>(oz) * pushOut;
        }
    }

    // ---- 9. Эмиссия веером (грань выпуклая) ----
    const uint32_t base = static_cast<uint32_t>(outVerts.size());
    for (int i = 0; i < n; ++i)
        outVerts.push_back({static_cast<float>(px[i]), static_cast<float>(py[i]), static_cast<float>(pz[i]),
                            U[i], V[i], ox, oy, oz, JM[i], JL[i], 1.0f /*ao*/, 0.0f /*tile*/, SWAY[i]});
    for (int i = 1; i + 1 < n; ++i)
    {
        outIndices.emplace_back<uint32_t>(base);
        outIndices.emplace_back<uint32_t>(base + static_cast<uint32_t>(i));
        outIndices.emplace_back<uint32_t>(base + static_cast<uint32_t>(i) + 1u);
    }
    return true;
}

/// Число float на вершину в интерливинге: pos(3) + normal(3) + uv(2) + jelly(2) + материал(3).
constexpr int kVertexStride = 13;

/**
 * @brief Вершинный layout воронои-меша для ax::Mesh::create(vertices, stride, indices, attribs).
 *
 * Желейные параметры (маска дрожания, нагрузка) едут в слоте VERTEX_ATTRIB_TEX_COORD1,
 * материальные (AO, индекс тайла, вес ветра травы) — в VERTEX_ATTRIB_TEX_COORD2.
 * ВАЖНО: axmol биндит атрибуты меша к программе по ИМЕНИ (VertexAttribBinding →
 * shaderinfos::getAttributeName), поэтому в шейдере эти атрибуты обязаны называться
 * ровно a_texCoord1 / a_texCoord2 и лежать на layout(location = TEXCOORD1) и
 * layout(location = 4) соответственно (макроса TEXCOORD2 в axslcc нет — слот 4
 * задан литералом и совпадает с shaderinfos::VertexKey::VERTEX_ATTRIB_TEX_COORD2).
 */
inline const std::vector<ax::MeshVertexAttrib>& vertexAttribs()
{
    static const std::vector<ax::MeshVertexAttrib> attribs = {
        {ax::backend::VertexFormat::FLOAT3, ax::shaderinfos::VertexKey::VERTEX_ATTRIB_POSITION},
        {ax::backend::VertexFormat::FLOAT3, ax::shaderinfos::VertexKey::VERTEX_ATTRIB_NORMAL},
        {ax::backend::VertexFormat::FLOAT2, ax::shaderinfos::VertexKey::VERTEX_ATTRIB_TEX_COORD},
        {ax::backend::VertexFormat::FLOAT2, ax::shaderinfos::VertexKey::VERTEX_ATTRIB_TEX_COORD1},
        {ax::backend::VertexFormat::FLOAT3, ax::shaderinfos::VertexKey::VERTEX_ATTRIB_TEX_COORD2},
    };
    return attribs;
}

/// Раскладывает вершины в интерливинг pos3+normal3+uv2+jelly2+mat3 для ax::Mesh::create.
/// Чистая функция — вызывается в ВОРКЕРЕ, чтобы в главном потоке остался только вызов Mesh::create.
inline void packVertices(const std::vector<Vertex>& vertices, std::vector<float>& out)
{
    const size_t n = vertices.size();
    out.resize(n * kVertexStride);
    for (size_t i = 0; i < n; ++i)
    {
        const Vertex& v = vertices[i];
        float* p        = &out[i * kVertexStride];
        p[0]            = v.x;
        p[1]            = v.y;
        p[2]            = v.z;
        p[3]            = v.nx;
        p[4]            = v.ny;
        p[5]            = v.nz;
        p[6]            = v.u;
        p[7]            = v.v;
        p[8]            = v.jm;
        p[9]            = v.jl;
        p[10]           = v.ao;
        p[11]           = v.tile;
        p[12]           = v.sway;
    }
}

/// Создаёт ax::Mesh из интерливингового буфера (ТОЛЬКО главный поток: аллоцирует GPU-буферы).
/// AABB считает движок по первым трём float вершины (позиции) — наш layout это удовлетворяет.
inline ax::Mesh* createMeshFromInterleaved(const std::vector<float>& verts,
                                           const ax::IndexArray& indices,
                                           ax::Texture2D* texture)
{
    if (verts.empty() || indices.empty())
        return nullptr;
    auto* mesh = ax::Mesh::create(verts, kVertexStride, indices, vertexAttribs());
    if (mesh && texture)
        mesh->setTexture(texture);
    return mesh;
}

/// Создаёт ax::Mesh с НАСТОЯЩИМИ нормалями и желейными атрибутами
/// (в отличие от createMesh() для кубов — там нормали-заглушки и нет a_texCoord1).
inline ax::Mesh* createMesh(const std::vector<Vertex>& vertices, const ax::IndexArray& indices, ax::Texture2D* texture)
{
    if (vertices.empty() || indices.empty())
        return nullptr;
    std::vector<float> packed;
    packVertices(vertices, packed);
    return createMeshFromInterleaved(packed, indices, texture);
}

}  // namespace voronoi
