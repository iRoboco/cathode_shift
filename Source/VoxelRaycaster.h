/**
 * @file VoxelRaycaster.h
 * @brief Воксельный рейкастер с intent-driven подсветкой (паттерн A+C)
 *
 * Архитектура:
 * - VoxelRaycasterNode: только физика луча + рендер фидбека по режиму
 * - FirstPlayerController: управление режимами, тайминги, игровая логика
 *
 * Режимы (BlockInteractionMode):
 *   Look        → только highlight грани
 *   PlaceIntent → highlight + preview (fade-in при зажатом RMB)
 *   BreakHold   → highlight + прогресс разрушения (LMB hold)
 */

#pragma once
#include "axmol.h"
#include "ChunkManager.h"

namespace ax::backend
{
class ProgramState;
}

#include <optional>
#include <functional>
#include <cstdint>
#include <cmath>
#include <limits>
#include <VoxelCollisionResolver.h>

// ============================================================================
//  VoxelHit
// ============================================================================

struct VoxelHit
{
    int bx, by, bz;
    ax::Vec3 hitPos;
    ax::Vec3 normal;
    float t;
    BlockId blockId;

    int adjX() const noexcept { return bx + static_cast<int>(normal.x); }
    int adjY() const noexcept { return by + static_cast<int>(normal.y); }
    int adjZ() const noexcept { return bz + static_cast<int>(normal.z); }

    ax::Vec3 adjacentCenter() const noexcept
    {
        return {static_cast<float>(adjX()) + 0.5f, static_cast<float>(adjY()) + 0.5f,
                static_cast<float>(adjZ()) + 0.5f};
    }

    // Оператор сравнения для кэширования
    bool operator==(const VoxelHit& o) const noexcept
    {
        return bx == o.bx && by == o.by && bz == o.bz && normal == o.normal;
    }
    bool operator!=(const VoxelHit& o) const noexcept { return !(*this == o); }
};

// ============================================================================
//  namespace VoxelRay — stateless DDA
// ============================================================================

namespace VoxelRay
{

namespace detail
{
inline double safe_inv(double v) noexcept
{
    return (std::abs(v) < 1e-12) ? std::numeric_limits<double>::infinity() : 1.0 / v;
}

inline double tMaxInit(double origin, int step, double inv_dir) noexcept
{
    double boundary = (step > 0) ? std::ceil(origin) : std::floor(origin);
    if (std::abs(boundary - origin) < 1e-9)
        boundary += step;
    return std::abs(boundary - origin) * std::abs(inv_dir);
}
}  // namespace detail

template <typename IsSolidFn>
std::optional<VoxelHit> castRay(const ax::Vec3& origin, const ax::Vec3& dir, float maxDistance, IsSolidFn&& isSolid)
{
    using namespace detail;

    const double ox = origin.x, oy = origin.y, oz = origin.z;
    const double dx = dir.x, dy = dir.y, dz = dir.z;
    const double maxT = static_cast<double>(std::max(0.0f, maxDistance));

    int vx = static_cast<int>(std::floor(ox));
    int vy = static_cast<int>(std::floor(oy));
    int vz = static_cast<int>(std::floor(oz));

    if (isSolid(vx, vy, vz))
    {
        VoxelHit hit;
        hit.bx      = vx;
        hit.by      = vy;
        hit.bz      = vz;
        hit.hitPos  = origin;
        hit.normal  = ax::Vec3::ZERO;
        hit.t       = 0.0f;
        hit.blockId = BLOCK_AIR;
        return hit;
    }

    const int stepX = (dx > 0.0) ? 1 : (dx < 0.0 ? -1 : 0);
    const int stepY = (dy > 0.0) ? 1 : (dy < 0.0 ? -1 : 0);
    const int stepZ = (dz > 0.0) ? 1 : (dz < 0.0 ? -1 : 0);

    const double inv_dx = safe_inv(dx);
    const double inv_dy = safe_inv(dy);
    const double inv_dz = safe_inv(dz);

    const double tDeltaX = (stepX != 0) ? std::abs(inv_dx) : std::numeric_limits<double>::infinity();
    const double tDeltaY = (stepY != 0) ? std::abs(inv_dy) : std::numeric_limits<double>::infinity();
    const double tDeltaZ = (stepZ != 0) ? std::abs(inv_dz) : std::numeric_limits<double>::infinity();

    double tMaxX = (stepX != 0) ? tMaxInit(ox, stepX, inv_dx) : std::numeric_limits<double>::infinity();
    double tMaxY = (stepY != 0) ? tMaxInit(oy, stepY, inv_dy) : std::numeric_limits<double>::infinity();
    double tMaxZ = (stepZ != 0) ? tMaxInit(oz, stepZ, inv_dz) : std::numeric_limits<double>::infinity();

    int lastAxis = -1;

    while (true)
    {
        double t;
        if (tMaxX < tMaxY)
        {
            if (tMaxX < tMaxZ)
            {
                t = tMaxX;
                if (t > maxT)
                    break;
                vx += stepX;
                tMaxX += tDeltaX;
                lastAxis = 0;
            }
            else
            {
                t = tMaxZ;
                if (t > maxT)
                    break;
                vz += stepZ;
                tMaxZ += tDeltaZ;
                lastAxis = 2;
            }
        }
        else
        {
            if (tMaxY < tMaxZ)
            {
                t = tMaxY;
                if (t > maxT)
                    break;
                vy += stepY;
                tMaxY += tDeltaY;
                lastAxis = 1;
            }
            else
            {
                t = tMaxZ;
                if (t > maxT)
                    break;
                vz += stepZ;
                tMaxZ += tDeltaZ;
                lastAxis = 2;
            }
        }

        if (!isSolid(vx, vy, vz))
            continue;

        ax::Vec3 normal = ax::Vec3::ZERO;
        if (lastAxis == 0)
            normal.x = static_cast<float>(-stepX);
        else if (lastAxis == 1)
            normal.y = static_cast<float>(-stepY);
        else
            normal.z = static_cast<float>(-stepZ);

        VoxelHit hit;
        hit.bx = vx;
        hit.by = vy;
        hit.bz = vz;
        hit.hitPos =
            ax::Vec3{static_cast<float>(ox + dx * t), static_cast<float>(oy + dy * t), static_cast<float>(oz + dz * t)};
        hit.normal  = normal;
        hit.t       = static_cast<float>(t);
        hit.blockId = BLOCK_AIR;
        return hit;
    }

    return std::nullopt;
}

// Оптимизация двойного getBlockAtWorldPos — кэшируем ID в лямбде
inline std::optional<VoxelHit> castWorld(const ax::Vec3& origin,
                                         const ax::Vec3& dir,
                                         float maxDistance,
                                         const ChunkManager* mgr)
{
    if (!mgr)
        return std::nullopt;

    BlockId lastSolidId = BLOCK_AIR;  // Кэш ID твердого блока
    auto result         = castRay(origin, dir, maxDistance, [mgr, &lastSolidId](int x, int y, int z) -> bool {
        BlockId id = mgr->getBlockAtWorldPos(
            ax::Vec3{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(z) + 0.5f});
        // Игнорируем воду: луч проходит сквозь неё до твердого блока
        if (id != BLOCK_AIR && id != BLOCK_WATER)
        {
            lastSolidId = id;
            return true;
        }
        return false;
    });

    if (result.has_value())
    {
        result->blockId = lastSolidId;  // Используем кэш вместо повторного запроса
    }
    return result;
}

template <typename IsSolidFn>
uint8_t exposedFaceMask(int bx, int by, int bz, IsSolidFn&& isSolidNeighbor)
{
    uint8_t mask = 0;
    if (!isSolidNeighbor(bx + 1, by, bz))
        mask |= (1u << 0);
    if (!isSolidNeighbor(bx - 1, by, bz))
        mask |= (1u << 1);
    if (!isSolidNeighbor(bx, by + 1, bz))
        mask |= (1u << 2);
    if (!isSolidNeighbor(bx, by - 1, bz))
        mask |= (1u << 3);
    if (!isSolidNeighbor(bx, by, bz + 1))
        mask |= (1u << 4);
    if (!isSolidNeighbor(bx, by, bz - 1))
        mask |= (1u << 5);
    return mask;
}

}  // namespace VoxelRay

// ============================================================================
//  BlockInteractionMode — игровые режимы взаимодействия
// ============================================================================

enum class BlockInteractionMode
{
    Look,         ///< Пассивный осмотр: только highlight грани
    PlaceIntent,  ///< Намерение поставить: highlight + preview (RMB hold)
    BreakHold,    ///< Разрушение: highlight + прогресс-бар (LMB hold)
};

// ============================================================================
//  VoxelRaycasterNode — рендер фидбека по режиму
// ============================================================================

class VoxelRaycasterNode : public ax::Node
{
public:
    using HitCallback  = std::function<void(const VoxelHit&)>;
    using MissCallback = std::function<void()>;

    static VoxelRaycasterNode* create(ax::Camera* camera, ChunkManager* mgr, float maxDist = 8.0f);

    bool init(ax::Camera* camera, ChunkManager* mgr, float maxDist);

    void setMaxDistance(float d) noexcept { _maxDist = d; }
    float getMaxDistance() const noexcept { return _maxDist; }

    // === Управление режимом извне (GameScene) ===
    void setInteractionMode(BlockInteractionMode mode);
    BlockInteractionMode getInteractionMode() const noexcept { return _mode; }

    // === Прогресс разрушения [0..1] ===
    // Хранится для тайминга (контроллер накапливает hold), ВИЗУАЛЬНО не отображается:
    // заполняющая грань (break progress fill) полностью исключена из рендера.
    void setBreakProgress(float t) noexcept { _breakProgress = std::clamp(t, 0.0f, 1.0f); }

    // === Цвета подсветки ===
    void setFaceHighlightColor(const ax::Color4F& c) noexcept { _faceColor = c; }
    void setPlacePreviewColor(const ax::Color4F& c) noexcept { _previewColor = c; }

    // Управление видимостью превью блока
    void setPlacePreviewVisible(bool visible) noexcept { _placePreviewVisible = visible; }
    bool isPlacePreviewVisible() const noexcept { return _placePreviewVisible; }

    // Установка капсулы игрока для проверки коллизий
    void setPlayerCapsule(const PlayerCapsule* capsule) { _playerCapsule = capsule; }

    // === Коллбеки ===
    void setOnHit(HitCallback cb) { _onHit = std::move(cb); }
    void setOnMiss(MissCallback cb) { _onMiss = std::move(cb); }

    // === Состояние ===
    const std::optional<VoxelHit>& getLastHit() const noexcept { return _lastHit; }
    bool hasHit() const noexcept { return _lastHit.has_value(); }

    // === Действия (вызываются GameScene по таймингу) ===
    bool breakBlock();
    bool placeBlock(BlockId id);

    void update(float dt) override;

private:
    // === Рендер компоненты ===
    /// Тикает u_time/u_tintColor в шейдере прицела (те же часы, что у террейна)
    void tickShaderUniforms();
    void updateFaceHighlight(const VoxelHit& hit);
    void updatePlacePreview(const VoxelHit& hit);

    void hideFaceHighlight();
    void hidePlacePreview();
    void hideAll();

    // === Меш-билдинг ===
    /// Воронои-путь: точный полигон видимой грани (nullptr → откат на сеточный квад)
    ax::Mesh* createVoronoiFaceMesh(const VoxelHit& hit, float pushOut, float insetScale);
    ax::Mesh* createFaceHighlightMesh(const VoxelHit& hit);
    ax::Mesh* createPlacePreviewMesh(const VoxelHit& hit);

    // Материалы кэшируются отдельно. Применяются только после addMesh(),
    // так как setMaterial() в Axmol работает только с существующими мешами в _meshes.
    // _highlightPSMaterial — материал, созданный ОДИН РАЗ из ProgramState прицела.
    // Намеренно НЕ используем MeshRenderer::setProgramState() при каждом хите:
    // Node::setProgramState() возвращает false для уже установленного PS и шейдер
    // не пропагируется на свежедобавленный меш (плюс меш createMesh() нёс атлас
    // и при незаписанном материале сэмплировал весь атлас UV'ками 0..1).
    ax::Material* _highlightMaterial   = nullptr;
    ax::Material* _highlightPSMaterial = nullptr;
    ax::Material* _previewMaterial     = nullptr;

    ~VoxelRaycasterNode() override;  // Для безопасного release() кэшированных материалов

    static ax::Vec3 cameraForward(const ax::Camera* cam);

    // === Данные ===
    ax::Camera* _camera = nullptr;
    ChunkManager* _mgr  = nullptr;
    float _maxDist      = 8.0f;

    std::optional<VoxelHit> _lastHit;
    BlockInteractionMode _mode = BlockInteractionMode::Look;
    float _breakProgress       = 0.0f;

    // ============================================================================
    //  Вспомогательный класс для динамических мешей.
    //  Axmol::MeshRenderer не имеет публичного clear(), но хранит мешки в protected _meshes.
    //  Наследуемся, чтобы безопасно очищать массивы и сбрасывать AABB без хаков.
    // ============================================================================
    class DynamicMeshRenderer : public ax::MeshRenderer
    {
    public:
        // Очищает внутренние векторы мешей и данных вершин.
        // ax::Vector::clear() автоматически вызывает release() для каждого элемента,
        // поэтому утечек памяти не возникает.
        void clearMeshes()
        {
            _meshes.clear();           // Очищает Vector<Mesh*>, вызывает release()
            _meshVertexDatas.clear();  // Очищает Vector<MeshVertexData*>
            _aabbDirty = true;         // Помечаем AABB как невалидный для пересчёта в следующем кадре
        }
    };

    // Заменяем тип указателей на наш наследник
    DynamicMeshRenderer* _faceHighlightRenderer = nullptr;
    DynamicMeshRenderer* _placePreviewRenderer  = nullptr;

    // Кэш последнего хита для предотвращения пересоздания мешей каждый кадр
    VoxelHit _lastFaceHighlightHit{};
    bool _faceHighlightDirty = true;

    // ============================================================================
    //  Прицел по реальной грани (воронои/желе террейн)
    //
    //  Когда включён voronoiTerrain, геометрия подсветки строится
    //  voronoi::buildBlockFace() — точная ячейка Вороного + желейное смещение.
    //  Анимация (дрожание желе, покачивание травы) повторяется шейдером
    //  custom/highlight_* синхронно с террейном: _animTime тикает тем же dt,
    //  что часы GameScene. Если программа не собрана — откат на сеточный квад.
    //  Подсветка — строго tint-only: ни меш, ни материал атлас не содержат.
    // ============================================================================
    ax::backend::ProgramState* _highlightPS = nullptr;  ///< retain'ится
    float _animTime = 0.0f;                             ///< часы анимации шейдера прицела
    bool _faceMeshIsVoronoi = false;                    ///< последний highlight-меш — воронои?

    // Флаг видимости превью
    bool _placePreviewVisible = true;

    // Флаг для контроля единовременной инициализации кэшированных рендереров.
    // Позволяет отделить этап настройки от этапа обновления в update().
    bool _renderersInitialized = false;

    // Указатель на капсулу игрока
    const PlayerCapsule* _playerCapsule = nullptr;

    // Цвета
    ax::Color4F _faceColor{1.0f, 1.0f, 1.0f, 0.25f};
    ax::Color4F _previewColor{0.2f, 0.9f, 0.2f, 0.2f};

    ax::Texture2D* _terrainAtlas = nullptr;

    HitCallback _onHit;
    MissCallback _onMiss;

    // ============================================================================
    //  Система частиц разрушения
    // ============================================================================
    struct BreakParticle
    {
        ax::Vec3 position;
        ax::Vec3 velocity;
        float lifetime;     // текущее оставшееся время жизни
        float maxLifetime;  // изначальное время жизни
        float size;         // размер кубика
        BlockId blockId;
        ax::MeshRenderer* renderer = nullptr;
    };

    struct BreakFragment
    {
        ax::Vec3 position;
        ax::Vec3 velocity;
        ax::Vec3 angularVelocity;  // вращение
        ax::Vec3 rotation;
        float lifetime;
        float maxLifetime;
        float size;
        BlockId blockId;
        ax::MeshRenderer* renderer = nullptr;
    };

    void updateParticles(float dt);
    void spawnDustParticle(const VoxelHit& hit);
    void spawnBreakFragments(const VoxelHit& hit);
    void cleanupParticles();

    ax::Mesh* createCubeMesh(float size, BlockId blockId);

    std::vector<BreakParticle> _particles;
    std::vector<BreakFragment> _fragments;

    // Последний блок, для которого спавнились частицы
    int _lastParticleSpawnBx = INT_MIN;
    int _lastParticleSpawnBy = INT_MIN;
    int _lastParticleSpawnBz = INT_MIN;
    float _dustSpawnAccum    = 0.0f;  // накопитель для спавна пыли
};
