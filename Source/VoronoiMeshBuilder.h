#pragma once
#include "axmol.h"
#include "ChunkManager.h"
#include "ChunkMeshBuilder.h"  // calculateTileUV, getBlockTileIndex
#include "cell.hh"             // Voro++: voro::voronoicell_neighbor

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

/**
 * @file VoronoiMeshBuilder.h
 * @brief Меш террейна из ячеек Вороного (Voro++) вместо кубов.
 *
 * Идея: каждый блок сетки владеет одним сидом = центр блока + детерминированный джиттер
 * (хэш от МИРОВЫХ координат блока и worldSeed). Ячейка блока — выпуклый многогранник,
 * вырезанный полуплоскостями до сидов соседей в окне 5x5x5. Для хранения нужен только
 * тип блока (как и раньше): сиды и геометрия вычисляются на лету, поэтому швов между
 * чанками нет — соседний чанк даёт те же сиды.
 *
 * Строятся только приповерхностные ячейки: твёрдый блок, у которого в окне 3x3x3 есть
 * воздух/вода. Внутренность массива не трогается (в типичном чанке это ~1-3 тыс. ячеек
 * вместо ~20 тыс. твёрдых блоков).
 *
 * Грань ячейки рисуется, если блок-сосед за ней (id плоскости = смещение соседа) не твёрдый.
 * Вода/воздух — «не твёрдые», как и в buildChunkMesh().
 */
namespace voronoi
{

/// Макс. смещение сида от центра блока (в блоках). Должно быть < 0.5; чем меньше, тем реже
/// в ячейку попадают соседи дальше 1-го кольца (см. kExposureRing).
constexpr double kJitter = 0.30;

/// Радиус окна соседей, чьи сиды режут ячейку (2 → 5x5x5 = 124 кандидата).
constexpr int kRing = 2;

/// Масштаб планарной проекции UV (тайлов на блок) и отступ от края тайла (защита от bleed).
constexpr float kUvScale = 0.8f;
constexpr float kUvInset = 0.02f;

/// Страховочный лимит вершин на меш. Индексы 32-битные (IndexFormat::U_INT), поэтому реальный
/// чанк в него не упирается; лимит защищает только от патологических данных.
constexpr size_t kMaxVertices = 4000000;

/// Вершина воронои-меша: позиция, UV атласа, нормаль грани (flat shading).
struct Vertex
{
    float x, y, z;
    float u, v;
    float nx, ny, nz;
};

struct Offset
{
    int dx, dy, dz;
};

/// Смещения соседей окна 5x5x5 (без нуля), отсортированы по расстоянию — ближние режут первыми,
/// поэтому дальние кандидаты почти всегда отбрасываются проверкой радиуса.
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

/// Детерминированный джиттер сида блока (мировые координаты) в [-kJitter, +kJitter] по каждой оси.
inline void seedJitter(uint32_t worldSeed, int gx, int gy, int gz, double& jx, double& jy, double& jz)
{
    uint64_t h = mix64(static_cast<uint64_t>(static_cast<uint32_t>(gx)) * 0x9E3779B185EBCA87ULL ^
                       static_cast<uint64_t>(static_cast<uint32_t>(gy)) * 0xC2B2AE3D27D4EB4FULL ^
                       static_cast<uint64_t>(static_cast<uint32_t>(gz)) * 0x165667B19E3779F9ULL ^ worldSeed);
    auto f = [&](int shift) { return (static_cast<double>((h >> shift) & 0xFFFFF) / 1048575.0 - 0.5) * 2.0 * kJitter; };
    jx = f(0);
    jy = f(20);
    jz = f(40);
}

/**
 * @brief Строит воронои-меш чанка.
 *
 * @param key        Ключ чанка (нужен для МИРОВЫХ координат сидов).
 * @param worldSeed  Сид мира для джиттера (одинаковый сид → одинаковая геометрия).
 * @param data       Блоки чанка.
 * @param neighborX0/X1/Z0/Z1 Соседние чанки (nullptr = не загружен → считается воздухом).
 *                   Диагональные чанки не передаются и считаются воздухом: максимум это даст
 *                   лишние (невидимые внутри массива) грани в 4 угловых колонках.
 * @param vertices   Выход: вершины (локальные координаты чанка, как у buildChunkMesh).
 * @param indices    Выход: индексы. ОБЯЗАТЕЛЬНО ax::IndexArray(ax::backend::IndexFormat::U_INT)
 *                   (emplace_back<uint32_t> в Axmol 2.x ассертит совпадение stride).
 * @return false, если упёрлись в kMaxVertices (меш усечён).
 */
inline bool buildChunkMesh(const ChunkKey& key,
                           uint32_t worldSeed,
                           const ChunkData& data,
                           const ChunkData* neighborX0,
                           const ChunkData* neighborX1,
                           const ChunkData* neighborZ0,
                           const ChunkData* neighborZ1,
                           std::vector<Vertex>& vertices,
                           ax::IndexArray& indices)
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
        const int lim = axis == 0 ? PX : (axis == 1 ? PZ : PY);
        const size_t stride = axis == 0 ? static_cast<size_t>(PY) : (axis == 1 ? static_cast<size_t>(PX) * PY : 1);
        for (int pz = 0; pz < PZ; ++pz)
            for (int px = 0; px < PX; ++px)
                for (int py = 0; py < PY; ++py)
                {
                    const size_t i = idx(px, pz, py);
                    const int c    = axis == 0 ? px : (axis == 1 ? pz : py);
                    dst[i] = (c >= 1 && c < lim - 1) ? (src[i - stride] & src[i] & src[i + stride]) : 0;
                }
    };
    erode(solid, tmp, 0);
    erode(tmp, interior, 1);
    erode(interior, tmp, 2);
    interior.swap(tmp);

    // ---- 2b. Таблица джиттера на всю паддинг-область (хэш каждого блока считаем 1 раз, а не ~124) ----
    const int baseGX = key.x * CX, baseGY = key.y * CY, baseGZ = key.z * CZ;
    std::vector<double> jit(N * 3);
    for (int pz = 0; pz < PZ; ++pz)
        for (int px = 0; px < PX; ++px)
            for (int py = 0; py < PY; ++py)
            {
                double* j = &jit[idx(px, pz, py) * 3];
                seedJitter(worldSeed, baseGX + px - R, baseGY + py - R, baseGZ + pz - R, j[0], j[1], j[2]);
            }

    // ---- 3. Ячейки приповерхностных блоков ----
    const auto& offsets = neighborOffsets();
    voro::voronoicell_neighbor cell;
    std::vector<double> vs, nm;
    std::vector<int> fv, nb;

    bool truncated = false;

    for (int z = 0; z < CZ && !truncated; ++z)
        for (int x = 0; x < CX && !truncated; ++x)
            for (int y = 0; y <= yMax; ++y)
            {
                const BlockId bid = data.getBlock(x, y, z);
                if (bid == BLOCK_AIR || bid == BLOCK_WATER)
                    continue;
                if (interior[idx(x + R, z + R, y + R)])
                    continue;

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
                for (size_t fi = 0; pos < fv.size(); ++fi)
                {
                    const int cnt = fv[pos];
                    const int* fvi = &fv[pos + 1];
                    pos += static_cast<size_t>(cnt) + 1;

                    const int k = nb[fi];
                    if (k < 0 || cnt < 3)
                        continue;  // стенка начального бокса (не должна случаться) / вырожденная грань
                    const Offset& o = offsets[static_cast<size_t>(k)];
                    if (solid[idx(x + o.dx + R, z + o.dz + R, y + o.dy + R)])
                        continue;  // за гранью твёрдый блок — не видна

                    if (vertices.size() + static_cast<size_t>(cnt) > kMaxVertices)
                    {
                        truncated = true;
                        break;
                    }

                    // Позиции вершин грани в координатах чанка.
                    double px[16], py[16], pz[16];
                    const int n = std::min(cnt, 16);
                    for (int i = 0; i < n; ++i)
                    {
                        px[i] = sx + vs[3 * fvi[i] + 0];
                        py[i] = sy + vs[3 * fvi[i] + 1];
                        pz[i] = sz + vs[3 * fvi[i] + 2];
                    }

                    // Нормаль из Voro++ + проверка обхода по Ньюэллу (CCW снаружи).
                    float fnx = static_cast<float>(nm[3 * fi + 0]);
                    float fny = static_cast<float>(nm[3 * fi + 1]);
                    float fnz = static_cast<float>(nm[3 * fi + 2]);
                    double ax_ = 0, ay_ = 0, az_ = 0;
                    for (int i = 0; i < n; ++i)
                    {
                        const int j = (i + 1) % n;
                        ax_ += (py[i] - py[j]) * (pz[i] + pz[j]);
                        ay_ += (pz[i] - pz[j]) * (px[i] + px[j]);
                        az_ += (px[i] - px[j]) * (py[i] + py[j]);
                    }
                    const bool reverse = ax_ * fnx + ay_ * fny + az_ * fnz < 0.0;

                    // Тайл по направлению нормали: верх / низ / бок.
                    const int kind = fny > 0.5f ? 2 : (fny < -0.5f ? 3 : 0);
                    const auto tuv = calculateTileUV(getBlockTileIndex(bid, kind));

                    // Планарная проекция UV вокруг центра грани.
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

                    const uint32_t base = static_cast<uint32_t>(vertices.size());
                    for (int i = 0; i < n; ++i)
                    {
                        const int s = reverse ? (n - 1 - i) : i;
                        const double dx = px[s] - cx, dy = py[s] - cy, dz = pz[s] - cz;
                        float u = 0.5f + static_cast<float>(dx * t1x + dy * t1y + dz * t1z) * kUvScale;
                        float v = 0.5f + static_cast<float>(dx * t2x + dy * t2y + dz * t2z) * kUvScale;
                        u       = std::clamp(u, kUvInset, 1.0f - kUvInset);
                        v       = std::clamp(v, kUvInset, 1.0f - kUvInset);
                        vertices.push_back({static_cast<float>(px[s]), static_cast<float>(py[s]), static_cast<float>(pz[s]),
                                            tuv[0] + u * (tuv[2] - tuv[0]), tuv[1] + v * (tuv[3] - tuv[1]), fnx, fny, fnz});
                    }
                    for (int i = 1; i + 1 < n; ++i)  // веер (грань выпуклая)
                    {
                        indices.emplace_back<uint32_t>(base);
                        indices.emplace_back<uint32_t>(base + static_cast<uint32_t>(i));
                        indices.emplace_back<uint32_t>(base + static_cast<uint32_t>(i) + 1u);
                    }
                }
            }
    return !truncated;
}

/// Создаёт ax::Mesh с НАСТОЯЩИМИ нормалями (в отличие от createMesh() для кубов — заглушка).
inline ax::Mesh* createMesh(const std::vector<Vertex>& vertices, const ax::IndexArray& indices, ax::Texture2D* texture)
{
    if (vertices.empty() || indices.empty())
        return nullptr;

    const size_t n = vertices.size();
    std::vector<float> positions(n * 3), normals(n * 3), uvs(n * 2);
    for (size_t i = 0; i < n; ++i)
    {
        positions[i * 3 + 0] = vertices[i].x;
        positions[i * 3 + 1] = vertices[i].y;
        positions[i * 3 + 2] = vertices[i].z;
        normals[i * 3 + 0]   = vertices[i].nx;
        normals[i * 3 + 1]   = vertices[i].ny;
        normals[i * 3 + 2]   = vertices[i].nz;
        uvs[i * 2 + 0]       = vertices[i].u;
        uvs[i * 2 + 1]       = vertices[i].v;
    }
    auto* mesh = ax::Mesh::create(positions, normals, uvs, indices);
    if (mesh && texture)
        mesh->setTexture(texture);
    return mesh;
}

}  // namespace voronoi
