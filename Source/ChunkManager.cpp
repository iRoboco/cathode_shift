#include "ChunkManager.h"
#include "ChunkMeshBuilder.h"
#include "VoronoiMeshBuilder.h"
#include "PerlinNoise.hpp"
#include "renderer/backend/ProgramManager.h"
#include "renderer/backend/ProgramState.h"
#include <algorithm>
#include <atomic>
#include <climits>

// =========================================================================
//  MeshJob — задача фоновой сборки меша
// =========================================================================
//  Воркер работает ТОЛЬКО со снимком (копии ChunkData), а не с живыми данными
//  _chunks: главный поток вправе менять блоки и выгружать соседей, пока задача
//  в очереди/выполняется. Результат — CPU-массивы; GPU-объекты (ax::Mesh, ноды)
//  создаются исключительно в главном потоке в processMeshResults().
struct MeshJob
{
    ChunkKey key{};
    bool voronoi = false;
    biome::BiomeConfig biomes{};  ///< Копия конфига биомов на момент постановки задачи

    // Вход: снимок чанка и 4 соседей по X/Z (nullptr — сосед не загружен → воздух)
    std::unique_ptr<ChunkData> self;
    std::unique_ptr<ChunkData> nbX0, nbX1, nbZ0, nbZ1;

    std::atomic<bool> cancelled{false};  ///< Выставляется главным потоком (выгрузка/пересоздание чанка)

    // Выход (пишет воркер, читает главный поток после передачи через _meshDone под мьютексом)
    bool built     = false;  ///< Есть геометрия
    bool truncated = false;  ///< Воронои-меш усечён по kMaxVertices
    std::vector<float> positions, normals, uvs;  ///< Воронои-путь
    std::vector<ChunkVertex> cubeVerts;          ///< Кубический путь
    ax::IndexArray indices;
};

namespace
{
/// Выполняется в воркере. Не трогает ничего, кроме полей задачи.
void runMeshJob(MeshJob& j)
{
    if (j.cancelled.load(std::memory_order_relaxed) || !j.self)
        return;

    if (j.voronoi)
    {
        j.indices = ax::IndexArray(ax::backend::IndexFormat::U_INT);
        std::vector<voronoi::Vertex> verts;
        j.truncated = !voronoi::buildChunkMesh(j.key, j.biomes, *j.self, j.nbX0.get(), j.nbX1.get(), j.nbZ0.get(),
                                               j.nbZ1.get(), verts, j.indices);
        voronoi::packArrays(verts, j.positions, j.normals, j.uvs);
        j.built = !verts.empty() && !j.indices.empty();
    }
    else
    {
        buildChunkMesh(*j.self, j.nbX0.get(), j.nbX1.get(), j.nbZ0.get(), j.nbZ1.get(), j.cubeVerts, j.indices);
        j.built = !j.cubeVerts.empty();
    }
}
}  // namespace

// =========================================================================
//  Конструктор / деструктор
// =========================================================================
ChunkManager::ChunkManager() = default;
ChunkManager::~ChunkManager()
{
    shutdown();
}

// =========================================================================
//  init
// =========================================================================
void ChunkManager::init(const Config& cfg)
{
    _cfg = cfg;
    AX_ASSERT(_cfg.workerThreadCount > 0);
    _running.store(true, std::memory_order_release);
    // Инициализация флагов pause/resume
    _paused      = false;
    _initialized = true;

    _genQueue.maxSize = static_cast<size_t>(_cfg.maxQueueSize);

    _terrainAtlas = ax::Director::getInstance()->getTextureCache()->addImage("textures/terrain_atlas.png");

    if (!_terrainAtlas)
    {
        AXLOGE("ChunkManager: terrain atlas not found, creating 1x1 white fallback");
        auto* image           = new ax::Image();
        unsigned char white[] = {255, 255, 255, 255};
        image->initWithRawData(white, sizeof(white), 1, 1, 8);
        _terrainAtlas = ax::Director::getInstance()->getTextureCache()->addImage(image, "fallback_white");
        image->release();
    }
    AX_ASSERT(_terrainAtlas != nullptr && "Failed to create even fallback texture!");

    // Применяем фильтрацию текстур из конфигурации
    applyTextureFilter();

    // Вынос запуска воркеров в отдельный метод
    startWorkers();
}

// =========================================================================
//  startWorkers — выделенная логика запуска воркеров.
//  Используется и в init(), и в resume(). Избегает дублирования кода.
// =========================================================================
void ChunkManager::startWorkers()
{
    // Защита: не запускать дублирующие потоки
    if (!_workers.empty())
        return;

    _running.store(true, std::memory_order_release);

    for (int i = 0; i < _cfg.workerThreadCount; ++i)
        _workers.emplace_back(&ChunkManager::workerLoop, this);
}

// =========================================================================
//  shutdown — только для полного уничтожения (деструктор)
// =========================================================================
void ChunkManager::shutdown()
{
    if (!_initialized)
        return;
    _initialized = false;

    // Останавливаем воркеры
    _running.store(false, std::memory_order_release);
    _genQueue.signalStop();

    for (auto& t : _workers)
        if (t.joinable())
            t.join();
    _workers.clear();

    // Очищаем очереди
    {
        std::lock_guard<std::mutex> lk(_readyMtx);
        _readyChunks.clear();
    }
    {
        std::lock_guard<std::mutex> lk(_meshDoneMtx);
        _meshDone.clear();
    }
    _meshInFlight = 0;
    _genPendingSet.clear();
    _unloadQueue.clear();

    // Удаляем все визуальные ноды из сцены
    for (auto& [key, entry] : _chunks)
    {
        if (entry.visualNode && _onUnload)
            _onUnload(entry.visualNode, key);
        if (entry.waterNode && _onWaterNodeDestroyed)
            _onWaterNodeDestroyed(entry.waterNode);
        if (entry.waterNode && _onUnload)
            _onUnload(entry.waterNode, key);
    }
    _chunks.clear();

    // Освобождаем общий водный материал (retain в getOrCreateWaterMaterial)
    AX_SAFE_RELEASE_NULL(_waterMaterial);

    // Сбрасываем текстуру (она может быть удалена из кэша при context lost)
    _terrainAtlas = nullptr;

    // Сброс флагов после полного уничтожения
    _paused = false;
}

// =========================================================================
//  pause — вызывается из onExit при сворачивании приложения.
//  Останавливает генерацию, но сохраняет ВСЕ данные чанков в памяти.
//  Визуальные ноды остаются в сцене (они просто не обновляются).
// =========================================================================
void ChunkManager::pause()
{
    if (_paused || !_initialized)
        return;
    _paused = true;

    // Шаг 1: Останавливаем приём новых задач в очередь генерации.
    // Воркеры завершат текущую генерацию чанка и выйдут из pop()
    // с false (т.к. stop=true). Это гарантирует, что ни один чанк
    // не останется в «наполовину сгенерированном» состоянии.
    _genQueue.signalStop();

    // Шаг 2: Дожидаемся завершения всех воркеров.
    // Это БЛОКИРУЮЩИЙ вызов, но при сворачивании приложения
    // задержка в несколько мс допустима — рендер уже остановлен.
    for (auto& t : _workers)
        if (t.joinable())
            t.join();
    _workers.clear();

    // Шаг 3: Обрабатываем чанки, которые воркеры успели сгенерировать
    // до остановки. Без этого при resume они будут «потеряны» в _readyChunks.
    // processReadyChunks() не создаёт новые задачи — просто визуализирует
    // уже готовые данные. Это быстро (создание меша + добавление в сцену).
    // Явная передача флага force=true для обработки оставшихся чанков
    processReadyChunks(true);

    // Шаг 3б: Применяем меши, которые воркеры успели собрать (force: без лимита per-frame).
    // signalStop() не обрывает очередь — воркеры выбирают её до конца, поэтому к этому
    // моменту все поставленные задачи сборки уже в _meshDone.
    processMeshResults(true);
    // Страховка: задачи, так и не дошедшие до результата, сбрасываем — чанк пересоберётся после resume.
    for (auto& [k, e] : _chunks)
    {
        if (e.meshJob)
        {
            cancelMeshJob(e);
            e.dirty = true;
        }
    }
    _meshInFlight = 0;

    // Шаг 4: Очищаем очередь генерации (при resume заполним заново).
    // НЕ очищаем _chunks — это ядро данных.
    // Используем clearAndReset вместо прямого доступа к полям. Сбрасываем флаг для будущего resume
    // _genQueue.queue.clear(); // TODO проверить как работало до и как теперь
    // _genQueue.stop = false;
    _genQueue.clearAndReset();
    _genPendingSet.clear();

    AXLOGI("ChunkManager: paused, {} chunks preserved", _chunks.size());
}

// =========================================================================
//  resume — вызывается из onEnter при разворачивании приложения.
//  Перезапускает воркеры и инициирует обновление чанков вокруг игрока.
// =========================================================================
void ChunkManager::resume()
{
    if (!_paused || !_initialized)
        return;
    _paused = false;

    // Шаг 1: Перезапускаем воркеры.
    // _running всё ещё true (не сбрасывался при pause),
    // очередь уже очищена от stop-флага.
    startWorkers();

    // Шаг 2: Сбрасываем _lastPlayerChunk чтобы при следующем update()
    // произошла полная переоценка — какие чанки нужны, какие нет.
    // За время сворачивания игрок мог «переместиться» (например,
    // при восстановлении из сохранения) или чанки могли устареть.
    _lastPlayerChunk = ChunkKey{INT32_MIN, INT32_MIN, INT32_MIN};

    // Шаг 3: Проверяем, не потеряна ли текстура (context loss на мобильных).
    // На Desktop/GL контекст может быть пересоздан при сворачивании —
    // текстура станет невалидной. Пересоздаём из кэша.
    if (!_terrainAtlas || _terrainAtlas->getPixelsWide() <= 0)
    {
        _terrainAtlas = ax::Director::getInstance()->getTextureCache()->addImage("textures/terrain_atlas.png");
        applyTextureFilter();
        // Общий водный материал держит старую (невалидную) текстуру → сбрасываем,
        // чтобы getOrCreateWaterMaterial пересоздал его с новым атласом.
        AX_SAFE_RELEASE_NULL(_waterMaterial);
        if (_terrainAtlas)
        {
            // Текстура обновлена, но материал чанков ссылается на старую.
            // Нужно перестроить визуальные ноды для чанков с материалом.
            AXLOGW("ChunkManager: texture reloaded after context loss, marking all chunks dirty");
            for (auto& [key, entry] : _chunks)
            {
                if (entry.status == ChunkStatus::Active && entry.visualNode)
                {
                    entry.dirty = true;
                }
            }
        }
    }

    AXLOGI("ChunkManager: resumed, {} chunks alive, workers restarted", _chunks.size());
}

// =========================================================================
//  workerLoop
// =========================================================================
void ChunkManager::workerLoop()
{
    TaskQueue::Item item;
    while (_genQueue.pop(item))
    {
        // --- Сборка меша (общая очередь и те же воркеры, что у генерации) ---
        if (item.mesh)
        {
            runMeshJob(*item.mesh);
            {
                std::lock_guard<std::mutex> lk(_meshDoneMtx);
                _meshDone.push_back(std::move(item.mesh));
            }
            continue;
        }

        // --- Генерация чанка ---
        const ChunkKey key = item.key;
        auto chunk         = std::make_unique<ChunkData>(key);
        if (_onGenerate)
        {
            _onGenerate(*chunk);
        }
        // Коллбек _onGenerate всегда установлен из GameScene::init(),
        // где создаётся свой thread_local PerlinNoise.
        {
            std::lock_guard<std::mutex> lk(_readyMtx);
            _readyChunks.push_back(std::move(chunk));
        }
    }
}

// =========================================================================
//  update — главный игровой цикл менеджера чанков
// =========================================================================
void ChunkManager::update(const ax::Vec3& playerWorldPos)
{
    // Пропускаем обновление, если менеджер на паузе.
    // Это предотвращает лишние вычисления при сворачивании приложения.
    if (_paused)
        return;

    _playerWorldPos = playerWorldPos;

    ChunkKey playerChunk = worldToChunk(playerWorldPos);

    // Пересобираем кандидатов КАЖДЫЙ кадр, а не только при смене чанка.
    // Чанки, отброшенные из-за переполнения очереди в предыдущем кадре,
    // получат второй шанс и корректно добавятся по мере освобождения слотов.
    collectChunksToLoad(playerChunk);

    if (playerChunk != _lastPlayerChunk)
    {
        _lastPlayerChunk = playerChunk;
        // Выгрузка запускается только при реальном перемещении игрока,
        // так как это тяжёлая операция с итерацией по всей карте _chunks.
        collectChunksToUnload(playerChunk);
    }

    processReadyChunks();
    processMeshResults();
    processDirtyChunks();
    processUnloadQueue();

    // Обновляем туман на всех чанках
    updateChunkFog();
}

// =========================================================================
//  getBlockAtWorldPos
// =========================================================================
BlockId ChunkManager::getBlockAtWorldPos(const ax::Vec3& worldPos) const
{
    ChunkKey key = worldToChunk(worldPos);
    auto it      = _chunks.find(key);
    if (it == _chunks.end() || it->second.status != ChunkStatus::Active)
        return BLOCK_AIR;
    if (!it->second.chunkData)
        return BLOCK_AIR;
    int lx = static_cast<int>(std::floor(worldPos.x)) - (key.x * CHUNK_SIZE_X);
    int ly = static_cast<int>(std::floor(worldPos.y)) - (key.y * CHUNK_SIZE_Y);
    int lz = static_cast<int>(std::floor(worldPos.z)) - (key.z * CHUNK_SIZE_Z);
    if (lx >= 0 && lx < CHUNK_SIZE_X && ly >= 0 && ly < CHUNK_SIZE_Y && lz >= 0 && lz < CHUNK_SIZE_Z)
    {
        return it->second.chunkData->getBlock(lx, ly, lz);
    }
    return BLOCK_AIR;
}

// =========================================================================
//  collectChunksToLoad — сбор и приоритизация чанков для генерации
// =========================================================================
void ChunkManager::collectChunksToLoad(const ChunkKey& playerChunk)
{
    // Не загружаем новые чанки, если на паузе
    if (_paused)
        return;

    int rd = _cfg.renderDistance;
    std::vector<std::pair<int, ChunkKey>> candidates;
    // Резервируем память под квадрат видимости, чтобы избежать реаллокаций в цикле
    candidates.reserve((rd * 2 + 1) * (rd * 2 + 1));

    for (int32_t dx = -rd; dx <= rd; ++dx)
        for (int32_t dz = -rd; dz <= rd; ++dz)
        {
            ChunkKey key{playerChunk.x + dx, 0, playerChunk.z + dz};
            // Отсеиваем чанки за пределами кругового радиуса видимости
            if (chunkDistance(key, playerChunk) > rd)
                continue;
            // Пропускаем уже известные чанки (любой статус кроме None)
            auto it = _chunks.find(key);
            if (it != _chunks.end() && it->second.status != ChunkStatus::None)
                continue;
            // Пропускаем чанки, уже стоящие в очереди на генерацию
            if (_genPendingSet.count(key))
                continue;
            // Сохраняем пару (расстояние, ключ) для последующей сортировки
            candidates.emplace_back(chunkDistance(key, playerChunk), key);
        }

    // Сортируем кандидатов: ближние к игроку будут идти первыми
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [dist, key] : candidates)
    {
        // Сначала проверяем вместимость очереди.
        // Метод push() теперь возвращает bool, что позволяет безопасно откатить транзакцию.
        if (!_genQueue.push(key, dist))
        {
            // Очередь переполнена → прерываем цикл текущего кадра.
            // Ближайшие чанки уже добавлены в приоритетную очередь первыми,
            // дальние автоматически подождут следующего update().
            break;
        }

        // push() вернул true → слот зарезервирован. Теперь безопасно модифицируем состояние.
        // try_emplace возвращает pair<iterator, bool>, где bool = true если запись новая.
        auto [it, inserted] = _chunks.try_emplace(key);
        auto& entry         = it->second;
        entry.status        = ChunkStatus::QueuedForGen;
        entry.visualNode    = nullptr;
        entry.chunkData.reset();
        entry.dirty = false;
        // Регистрируем чанк как "ожидающий генерации", чтобы не дублировать задачи
        _genPendingSet.insert(key);
    }
}

// =========================================================================
//  collectChunksToUnload
// =========================================================================
void ChunkManager::collectChunksToUnload(const ChunkKey& playerChunk)
{
    // Не выгружаем чанки, если на паузе
    if (_paused)
        return;

    int unloadDist = _cfg.renderDistance + _cfg.unloadMargin;
    for (auto it = _chunks.begin(); it != _chunks.end();)
    {
        const auto& key = it->first;
        auto& entry     = it->second;
        if (entry.status == ChunkStatus::None)
        {
            it = _chunks.erase(it);
            continue;
        }
        if (entry.status == ChunkStatus::QueuedForUnload)
        {
            ++it;
            continue;
        }
        if (chunkDistance(key, playerChunk) > unloadDist)
        {
            entry.status = ChunkStatus::QueuedForUnload;
            cancelMeshJob(entry);  // воркеру незачем собирать меш выгружаемого чанка
            _unloadQueue.push_back(key);
            ++it;
        }
        else
            ++it;
    }
}

// =========================================================================
//  buildChunkVisualNode
// =========================================================================
ax::Node* ChunkManager::buildChunkVisualNode(const ChunkKey& key, ChunkData& data)
{
    // Получение данных соседей для кросс-чанкового face culling
    const ChunkData* neighborX0 = nullptr;
    const ChunkData* neighborX1 = nullptr;
    const ChunkData* neighborZ0 = nullptr;
    const ChunkData* neighborZ1 = nullptr;
    auto getNeighborData        = [&](int dx, int dz) -> const ChunkData* {
        ChunkKey nk{key.x + dx, key.y, key.z + dz};
        auto nit = _chunks.find(nk);
        if (nit != _chunks.end() && nit->second.status == ChunkStatus::Active && nit->second.chunkData)
        {
            return nit->second.chunkData.get();
        }
        return nullptr;
    };
    neighborX0 = getNeighborData(-1, 0);
    neighborX1 = getNeighborData(1, 0);
    neighborZ0 = getNeighborData(0, -1);
    neighborZ1 = getNeighborData(0, 1);

    // Мешбилдинг
    // Используем ax::IndexArray вместо std::vector<uint16_t>
    // для совместимости с Mesh::create(const IndexArray& indices).
    // Конструктор по умолчанию IndexArray() инициализирует stride=2 (U_SHORT).
    ax::IndexArray inds;
    ax::Mesh* mesh = nullptr;
    if (_cfg.voronoiTerrain)
    {
        // Воронои-меш легко переваливает за 65535 вершин → 32-битные индексы.
        inds = ax::IndexArray(ax::backend::IndexFormat::U_INT);
        std::vector<voronoi::Vertex> vverts;
        if (!voronoi::buildChunkMesh(key, _cfg.biomes, data, neighborX0, neighborX1, neighborZ0, neighborZ1,
                                     vverts, inds))
            AXLOGW("ChunkManager: voronoi mesh truncated at {} vertices (chunk {},{},{})", vverts.size(), key.x,
                   key.y, key.z);
        if (vverts.empty())
            return nullptr;
        mesh = voronoi::createMesh(vverts, inds, _terrainAtlas);
    }
    else
    {
        std::vector<ChunkVertex> verts;
        buildChunkMesh(data, neighborX0, neighborX1, neighborZ0, neighborZ1, verts, inds);
        if (verts.empty())
            return nullptr;
        mesh = createMesh(verts, inds, _terrainAtlas);
    }
    if (!mesh)
        return nullptr;
    return finishChunkNode(key, mesh);
}

// =========================================================================
//  finishChunkNode — нод чанка из готового меша (общая часть sync/async путей)
// =========================================================================
ax::Node* ChunkManager::finishChunkNode(const ChunkKey& key, ax::Mesh* mesh)
{
    //  MeshRenderer — это Node (наследник Node), а НЕ компонент.
    auto* chunkNode = ax::MeshRenderer::create();
    chunkNode->addMesh(mesh);

    //  Используем MeshMaterial::createBuiltInMaterial()
    auto* material = ax::MeshMaterial::createBuiltInMaterial(ax::MeshMaterial::MaterialType::UNLIT, false);
    if (material && _terrainAtlas)
    {
        material->setTexture(_terrainAtlas, ax::NTextureData::Usage::Diffuse);
        // Включаем альфа-блендинг для тумана (opacity чанка по дистанции)
        auto& sb = material->getStateBlock();
        sb.setBlend(true);
        sb.setBlendFunc(ax::BlendFunc{ax::backend::BlendFactor::SRC_ALPHA, ax::backend::BlendFactor::ONE_MINUS_SRC_ALPHA});
    }
    chunkNode->setMaterial(material);

    // Кастомный шейдер террейна с antialiased-pixel-art выборкой (поверх базового
    // UNLIT-материала, который несёт render-state и привязку атласа). Программа
    // кэшируется ProgramManager, поэтому вызов на каждый чанк дёшев. Если шейдер
    // не собран (program == nullptr) — остаётся обычный UNLIT.
    // Воронои-террейну всегда нужен свой шейдер (освещение по нормали), независимо от pixelArtAA.
    if (_cfg.voronoiTerrain || _cfg.pixelArtAA)
    {
        auto* program = _cfg.voronoiTerrain
                            ? ax::ProgramManager::getInstance()->loadProgram(
                                  "custom/voronoi_terrain_vs", "custom/voronoi_terrain_fs",
                                  ax::backend::VertexLayoutType::Unspec)
                            : ax::ProgramManager::getInstance()->loadProgram("custom/terrain_vs", "custom/terrain_fs",
                                                                             ax::backend::VertexLayoutType::Unspec);
        if (program)
        {
            auto* ps = new ax::backend::ProgramState(program);
            if (_terrainAtlas)
                ps->setTexture(ps->getUniformLocation("u_tex0"), 0, _terrainAtlas->getBackendTexture());
            chunkNode->setProgramState(ps);  // заменяет программу, сохраняя state-block
            ps->release();                   // нод удерживает свою ссылку
        }
    }

    chunkNode->setPosition3D(chunkToWorld(key));
    return chunkNode;
}

// =========================================================================
//  getOrCreateWaterMaterial — общий render-state-материал для всей воды
// =========================================================================
//  Создаётся лениво один раз и retain'ится. Несёт только blend/depth/cull и
//  привязку атласа к u_tex0; программу водного шейдера каждый нод ставит сам
//  через setProgramState (нужен per-node u_chunkOrigin). Шаринг безопасен —
//  Mesh::setProgramState копирует state-block в свой материал, не меняя общий.
//  Освобождается в shutdown() и сбрасывается при потере контекста в resume().
ax::MeshMaterial* ChunkManager::getOrCreateWaterMaterial()
{
    if (_waterMaterial)
        return _waterMaterial;

    auto* mat = ax::MeshMaterial::createBuiltInMaterial(ax::MeshMaterial::MaterialType::UNLIT, false);
    if (!mat)
        return nullptr;

    if (_terrainAtlas)
        mat->setTexture(_terrainAtlas, ax::NTextureData::Usage::Diffuse);

    ax::BlendFunc blend{};
    blend.src = ax::backend::BlendFactor::SRC_ALPHA;
    blend.dst = ax::backend::BlendFactor::ONE_MINUS_SRC_ALPHA;
    mat->getStateBlock().setBlendFunc(blend);
    mat->getStateBlock().setDepthWrite(false);
    mat->getStateBlock().setDepthTest(true);
    mat->getStateBlock().setCullFace(false);
    mat->getStateBlock().setCullFaceSide(ax::CullFaceSide::BACK);

    mat->retain();
    _waterMaterial = mat;
    return _waterMaterial;
}

// =========================================================================
//  buildWaterVisualNode
// =========================================================================
ax::Node* ChunkManager::buildWaterVisualNode(const ChunkKey& key, ChunkData& data)
{
    // === Получение данных соседей для бесшовного стыка ===
    const ChunkData *neighborX0 = nullptr, *neighborX1 = nullptr;
    const ChunkData *neighborZ0 = nullptr, *neighborZ1 = nullptr;
    auto getNeighborData = [&](int dx, int dz) -> const ChunkData* {
        ChunkKey nk{key.x + dx, key.y, key.z + dz};
        auto nit = _chunks.find(nk);
        // Проверяем статус Active и наличие данных, чтобы избежать race-conditions
        if (nit != _chunks.end() && nit->second.status == ChunkStatus::Active && nit->second.chunkData)
            return nit->second.chunkData.get();
        return nullptr;
    };
    neighborX0 = getNeighborData(-1, 0);
    neighborX1 = getNeighborData(1, 0);
    neighborZ0 = getNeighborData(0, -1);
    neighborZ1 = getNeighborData(0, 1);

    // === Генерация геометрии воды ===
    std::vector<ChunkVertex> verts;
    ax::IndexArray inds;
    buildWaterMesh(data, neighborX0, neighborX1, neighborZ0, neighborZ1, verts, inds);
    if (verts.empty())
        return nullptr;  // Вода отсутствует → оптимизация

    ax::Mesh* mesh = createMesh(verts, inds, _terrainAtlas);
    if (!mesh)
        return nullptr;

    // === Создание рендерера ===
    auto* node = ax::MeshRenderer::create();
    node->addMesh(mesh);

    // --- 1. Базовый материал даёт корректный render-state (blend / depth / cull) ---
    // ОБЩИЙ на все водные чанки: Mesh::setProgramState ниже лишь КОПИРУЕТ его
    // state-block в свой per-node материал и не мутирует общий, поэтому шаринг
    // безопасен. Это убирает дорогой createBuiltInMaterial(UNLIT) на каждый чанк
    // (его всё равно тут же заменял setProgramState) — фикс просадки генерации.
    auto* mat = getOrCreateWaterMaterial();
    if (!mat)
        return nullptr;
    node->setMaterial(mat);

    // --- 2. Кастомный водный шейдер поверх базового материала ---
    auto* program = ax::ProgramManager::getInstance()->loadProgram("custom/water_vs", "custom/water_fs",
                                                                   ax::backend::VertexLayoutType::Unspec);
    if (program)
    {
        auto* ps = new ax::backend::ProgramState(program);

        // Статические uniform'ы (на весь срок жизни нода):
        ax::Vec3 origin = chunkToWorld(key);  // мировой угол чанка
        ax::Vec3 sunDir = ax::Vec3(0.07f, -0.8f, 0.49f);
        sunDir.normalize();  // ОТ солнца К сцене

        // Параметры тумана — соответствуют updateChunkFog()
        float maxDist = static_cast<float>(_cfg.renderDistance * CHUNK_SIZE_X);
        ax::Vec3 fogColor(0.55f, 0.78f, 0.95f);  // цвет неба
        float fogStart = maxDist * FOG_START_FACTOR;
        float fogEnd   = maxDist * FOG_END_FACTOR;

        auto locOrigin  = ps->getUniformLocation("u_chunkOrigin");
        auto locLight   = ps->getUniformLocation("u_lightDir");
        auto locFogClr  = ps->getUniformLocation("u_fogColor");
        auto locFogSt   = ps->getUniformLocation("u_fogStart");
        auto locFogEnd  = ps->getUniformLocation("u_fogEnd");
        ps->setUniform(locOrigin, &origin, sizeof(ax::Vec3));
        ps->setUniform(locLight, &sunDir, sizeof(ax::Vec3));
        ps->setUniform(locFogClr, &fogColor, sizeof(ax::Vec3));
        ps->setUniform(locFogSt, &fogStart, sizeof(float));
        ps->setUniform(locFogEnd, &fogEnd, sizeof(float));

        // Атлас в слот u_tex0 (на случай если материал не пробросит текстуру)
        if (_terrainAtlas)
            ps->setTexture(ps->getUniformLocation("u_tex0"), 0, _terrainAtlas->getBackendTexture());

        node->setProgramState(ps);  // заменяет программу, сохраняя state-block
        ps->release();              // нод удерживает свою ссылку
    }
    // Если program == nullptr (шейдер не собран) — остаётся базовый UNLIT-материал.

    node->setColor(ax::Color3B(40, 120, 200));
    node->setOpacity(178);
    node->setPosition3D(chunkToWorld(key));
    node->setTag(WATER_NODE_TAG);
    return node;
}

// =========================================================================
//  cancelMeshJob / scheduleMeshJob / processMeshResults — фоновая сборка мешей
// =========================================================================
void ChunkManager::cancelMeshJob(ChunkEntry& entry)
{
    if (entry.meshJob)
    {
        entry.meshJob->cancelled.store(true, std::memory_order_relaxed);
        entry.meshJob.reset();  // результат (если придёт) не совпадёт с entry.meshJob → будет отброшен
    }
}

bool ChunkManager::scheduleMeshJob(const ChunkKey& key, ChunkEntry& entry)
{
    if (_paused || !_cfg.asyncMeshing || !entry.chunkData)
        return false;
    if (entry.meshJob)
        return false;  // уже собирается; dirty (если выставят) сработает после применения результата
    if (_meshInFlight >= _cfg.maxMeshJobsInFlight)
        return false;

    auto job     = std::make_shared<MeshJob>();
    job->key     = key;
    job->voronoi = _cfg.voronoiTerrain;
    job->biomes  = _cfg.biomes;

    // Снимок: копия блоков. ~131 КБ на чанк (uint16_t × 16·256·16) — memcpy порядка десятков мкс.
    job->self = std::make_unique<ChunkData>(*entry.chunkData);

    auto snapshotNeighbor = [&](int dx, int dz) -> std::unique_ptr<ChunkData> {
        auto nit = _chunks.find(ChunkKey{key.x + dx, key.y, key.z + dz});
        if (nit != _chunks.end() && nit->second.status == ChunkStatus::Active && nit->second.chunkData)
            return std::make_unique<ChunkData>(*nit->second.chunkData);
        return nullptr;
    };
    job->nbX0 = snapshotNeighbor(-1, 0);
    job->nbX1 = snapshotNeighbor(1, 0);
    job->nbZ0 = snapshotNeighbor(0, -1);
    job->nbZ1 = snapshotNeighbor(0, 1);

    entry.meshJob = job;
    entry.dirty   = false;  // любая правка ПОСЛЕ снимка снова выставит флаг
    ++_meshInFlight;

    _genQueue.pushMesh(std::move(job), key, chunkDistance(key, worldToChunk(_playerWorldPos)));
    return true;
}

void ChunkManager::processMeshResults(bool force)
{
    if (_paused && !force)
        return;

    std::vector<std::shared_ptr<MeshJob>> done;
    {
        std::lock_guard<std::mutex> lk(_meshDoneMtx);
        if (_meshDone.empty())
            return;
        done.swap(_meshDone);
    }

    // Воркеры завершаются в произвольном порядке — ближние к игроку применяем первыми.
    const ChunkKey pc = worldToChunk(_playerWorldPos);
    std::sort(done.begin(), done.end(), [&](const auto& a, const auto& b) {
        return chunkDistance(a->key, pc) < chunkDistance(b->key, pc);
    });

    int applied = 0;
    for (size_t i = 0; i < done.size(); ++i)
    {
        if (!force && applied >= _cfg.maxMeshAppliesPerFrame)
        {
            // Остаток вернётся в очередь результатов; слот «в полёте» остаётся занятым до применения.
            std::lock_guard<std::mutex> lk(_meshDoneMtx);
            _meshDone.insert(_meshDone.end(), std::make_move_iterator(done.begin() + static_cast<long>(i)),
                             std::make_move_iterator(done.end()));
            break;
        }

        const std::shared_ptr<MeshJob>& job = done[i];
        --_meshInFlight;

        auto it = _chunks.find(job->key);
        if (it == _chunks.end() || it->second.meshJob != job)
            continue;  // чанк выгружен / задача отменена / чанк пересоздан
        ChunkEntry& e = it->second;
        e.meshJob.reset();
        if (job->cancelled.load(std::memory_order_relaxed) || e.status != ChunkStatus::Active || !e.chunkData)
            continue;

        const ChunkKey& key = job->key;

        // Новый нод (GPU-ресурсы — только здесь, в главном потоке)
        ax::Mesh* mesh = nullptr;
        if (job->built)
        {
            if (job->truncated)
                AXLOGW("ChunkManager: voronoi mesh truncated (chunk {},{},{})", key.x, key.y, key.z);
            mesh = job->voronoi ? voronoi::createMeshFromArrays(job->positions, job->normals, job->uvs, job->indices,
                                                                _terrainAtlas)
                                : createMesh(job->cubeVerts, job->indices, _terrainAtlas);
        }
        ax::Node* node = mesh ? finishChunkNode(key, mesh) : nullptr;

        // Заменяем старые ноды (старый меш виден до этого момента — без мерцания)
        if (e.visualNode && _onUnload)
            _onUnload(e.visualNode, key);
        e.visualNode = node;
        if (e.waterNode && _onWaterNodeDestroyed)
            _onWaterNodeDestroyed(e.waterNode);
        if (e.waterNode && _onUnload)
            _onUnload(e.waterNode, key);
        e.waterNode = buildWaterVisualNode(key, *e.chunkData);  // вода: дешёвый кубический меш, строим синхронно

        if (node && _onVisualize)
            _onVisualize(node, key);
        if (e.waterNode && _onVisualize)
            _onVisualize(e.waterNode, key);
        if (e.waterNode && _onWaterNodeCreated)
            _onWaterNodeCreated(e.waterNode);
        ++applied;
    }
}

// =========================================================================
//  processReadyChunks
// =========================================================================
void ChunkManager::processReadyChunks(bool force)
{
    // Явная проверка флага паузы с учетом параметра force
    if (_paused && !force)
        return;

    std::vector<std::unique_ptr<ChunkData>> ready;
    {
        std::lock_guard<std::mutex> lk(_readyMtx);
        if (_readyChunks.empty())
            return;
        ready.swap(_readyChunks);
    }

    // Воркеры работают параллельно и завершают задачи в случайном порядке
    // (зависит от нагрузки CPU, сложности террейна, кэш-локальности).
    // Без сортировки чанки появлялись бы хаотично, игнорируя приоритет очереди.
    // Сортируем готовые чанки по расстоянию до текущей позиции игрока.
    std::sort(ready.begin(), ready.end(), [this](const auto& a, const auto& b) {
        return chunkDistance(a->getKey(), _lastPlayerChunk) < chunkDistance(b->getKey(), _lastPlayerChunk);
    });

    int processed = 0;
    for (auto& chunkPtr : ready)
    {
        if (processed >= _cfg.maxGenerationsPerFrame)
        {
            std::lock_guard<std::mutex> lk(_readyMtx);
            // Возвращаем непроцессированные чанки обратно в очередь для следующего кадра
            _readyChunks.insert(_readyChunks.end(), std::make_move_iterator(ready.begin() + processed),
                                std::make_move_iterator(ready.end()));
            break;
        }

        const ChunkKey& key = chunkPtr->getKey();
        auto it             = _chunks.find(key);

        // Лямбда-хелпер для отмены генерации устаревших/выгружаемых чанков
        auto cancelGen = [&]() {
            _genPendingSet.erase(key);
            if (it != _chunks.end())
            {
                if (it->second.visualNode && _onUnload)
                {
                    _onUnload(it->second.visualNode, key);
                    it->second.visualNode = nullptr;
                }
                it->second.status = ChunkStatus::None;
            }
        };

        if (it == _chunks.end() || it->second.status == ChunkStatus::QueuedForUnload)
        {
            cancelGen();
            continue;
        }
        it->second.chunkData = std::move(chunkPtr);

        // Используем isAllAir() для пропуска пустых чанков
        if (it->second.chunkData->isAllAir())
        {
            it->second.status     = ChunkStatus::Active;
            it->second.visualNode = nullptr;
            it->second.dirty      = false;
            _genPendingSet.erase(key);
            ++processed;
            // Соседи всё равно нужно пометить dirty
            ChunkKey neighborKeys[] = {{key.x - 1, key.y, key.z},
                                       {key.x + 1, key.y, key.z},
                                       {key.x, key.y, key.z - 1},
                                       {key.x, key.y, key.z + 1}};
            for (const auto& nk : neighborKeys)
            {
                auto nit = _chunks.find(nk);
                if (nit != _chunks.end() && nit->second.status == ChunkStatus::Active && !nit->second.dirty)
                {
                    nit->second.dirty = true;
                }
            }
            continue;
        }
        if (_cfg.asyncMeshing)
        {
            // Меш соберёт воркер: чанк Active сразу (блоки доступны коллизиям), нода появится позже.
            cancelMeshJob(it->second);  // на случай задачи от прежнего «воплощения» чанка
            it->second.status     = ChunkStatus::Active;
            it->second.visualNode = nullptr;
            it->second.waterNode  = nullptr;
            _genPendingSet.erase(key);
            // Не вышло поставить (лимит в полёте) — dirty повторит попытку из processDirtyChunks().
            it->second.dirty = !scheduleMeshJob(key, it->second);
            ChunkKey nbKeys[] = {
                {key.x - 1, key.y, key.z}, {key.x + 1, key.y, key.z}, {key.x, key.y, key.z - 1}, {key.x, key.y, key.z + 1}};
            for (const auto& nk : nbKeys)
            {
                auto nit = _chunks.find(nk);
                if (nit != _chunks.end() && nit->second.status == ChunkStatus::Active && !nit->second.dirty)
                    nit->second.dirty = true;
            }
            ++processed;
            continue;
        }
        ax::Node* chunkNode   = buildChunkVisualNode(key, *it->second.chunkData);
        it->second.visualNode = chunkNode;
        it->second.waterNode  = buildWaterVisualNode(key, *it->second.chunkData);
        it->second.status     = ChunkStatus::Active;
        it->second.dirty      = false;
        _genPendingSet.erase(key);
        if (_onVisualize)
            _onVisualize(chunkNode, key);
        if (it->second.waterNode && _onVisualize)
            _onVisualize(it->second.waterNode, key);
        if (it->second.waterNode && _onWaterNodeCreated)
            _onWaterNodeCreated(it->second.waterNode);

        // Помечаем соседей dirty для перестройки стыковых граней
        ChunkKey neighborKeys[] = {
            {key.x - 1, key.y, key.z}, {key.x + 1, key.y, key.z}, {key.x, key.y, key.z - 1}, {key.x, key.y, key.z + 1}};
        for (const auto& nk : neighborKeys)
        {
            auto nit = _chunks.find(nk);
            if (nit != _chunks.end() && nit->second.status == ChunkStatus::Active && !nit->second.dirty)
            {
                nit->second.dirty = true;
            }
        }
        ++processed;
    }
}

// =========================================================================
//  processDirtyChunks
// =========================================================================
void ChunkManager::processDirtyChunks()
{
    // Не обрабатываем dirty-чанки, если на паузе
    if (_paused)
        return;

    // Откладываем перестройку, пока хоть один сосед по X/Z ещё стоит в очереди
    // генерации (_genPendingSet). Иначе чанк перестраивался бы заново на приход
    // КАЖДОГО из 4 соседей по отдельности (до 4 полных ребилдов вместо одного).
    // Дождавшись, пока соседи «осядут», схлопываем это в единственную перестройку.
    // Сосед вне радиуса видимости в набор не попадает — его ждать не будем, и
    // пограничная «стена» в сторону незагруженной области останется (это корректно).
    auto neighborsPending = [&](const ChunkKey& k) {
        const ChunkKey ns[4] = {
            {k.x - 1, k.y, k.z}, {k.x + 1, k.y, k.z}, {k.x, k.y, k.z - 1}, {k.x, k.y, k.z + 1}};
        for (const auto& nk : ns)
            if (_genPendingSet.count(nk))
                return true;
        return false;
    };

    if (_cfg.asyncMeshing)
    {
        // Кандидаты: dirty, без задачи в полёте, соседи «осели». Ставим ближайшие к игроку.
        const ChunkKey pc = worldToChunk(_playerWorldPos);
        std::vector<std::pair<int, ChunkKey>> cands;
        for (auto& [key, entry] : _chunks)
        {
            if (!entry.dirty || entry.status != ChunkStatus::Active || !entry.chunkData || entry.meshJob ||
                neighborsPending(key))
                continue;
            if (entry.chunkData->isAllAir())
            {
                // Пустой чанк: геометрии нет — снимаем старые ноды, если были, и гасим флаг.
                if (entry.visualNode && _onUnload)
                    _onUnload(entry.visualNode, key);
                entry.visualNode = nullptr;
                if (entry.waterNode && _onWaterNodeDestroyed)
                    _onWaterNodeDestroyed(entry.waterNode);
                if (entry.waterNode && _onUnload)
                    _onUnload(entry.waterNode, key);
                entry.waterNode = nullptr;
                entry.dirty     = false;
                continue;
            }
            cands.emplace_back(chunkDistance(key, pc), key);
        }
        std::sort(cands.begin(), cands.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        int scheduled = 0;
        for (const auto& [dist, key] : cands)
        {
            if (scheduled >= _cfg.maxDirtyRebuildsPerFrame)
                break;
            auto it = _chunks.find(key);
            if (it == _chunks.end() || !scheduleMeshJob(key, it->second))
                break;  // лимит в полёте/пауза — повторим в следующем кадре
            ++scheduled;
        }
        return;
    }

    std::vector<ChunkKey> dirtyKeys;
    for (auto& [key, entry] : _chunks)
    {
        if (entry.dirty && entry.status == ChunkStatus::Active && entry.chunkData && !neighborsPending(key))
        {
            dirtyKeys.push_back(key);
            if (static_cast<int>(dirtyKeys.size()) >= _cfg.maxDirtyRebuildsPerFrame)
                break;
        }
    }
    for (const auto& key : dirtyKeys)
    {
        auto it = _chunks.find(key);
        if (it == _chunks.end() || !it->second.dirty || it->second.status != ChunkStatus::Active ||
            !it->second.chunkData)
        {
            continue;
        }
        auto& entry = it->second;
        if (entry.visualNode && _onUnload)
        {
            _onUnload(entry.visualNode, key);
            entry.visualNode = nullptr;
        }
        entry.visualNode = buildChunkVisualNode(key, *entry.chunkData);
        if (entry.waterNode && _onWaterNodeDestroyed)
            _onWaterNodeDestroyed(entry.waterNode);
        if (entry.waterNode && _onUnload)
            _onUnload(entry.waterNode, key);
        entry.waterNode = buildWaterVisualNode(key, *entry.chunkData);
        entry.dirty     = false;
        if (_onVisualize)
            _onVisualize(entry.visualNode, key);
        if (entry.waterNode && _onVisualize)
            _onVisualize(entry.waterNode, key);
        if (entry.waterNode && _onWaterNodeCreated)
            _onWaterNodeCreated(entry.waterNode);
    }
}

// =========================================================================
//  processUnloadQueue
// =========================================================================
void ChunkManager::processUnloadQueue()
{
    // Не выгружаем чанки, если на паузе
    if (_paused)
        return;

    int processed = 0;
    auto it       = _unloadQueue.begin();
    while (it != _unloadQueue.end() && processed < _cfg.maxUnloadsPerFrame)
    {
        const ChunkKey& key = *it;
        auto mapIt          = _chunks.find(key);
        if (mapIt != _chunks.end() && mapIt->second.status == ChunkStatus::QueuedForUnload)
        {
            if (mapIt->second.visualNode && _onUnload)
                _onUnload(mapIt->second.visualNode, key);
            if (mapIt->second.waterNode && _onWaterNodeDestroyed)
                _onWaterNodeDestroyed(mapIt->second.waterNode);
            if (mapIt->second.waterNode && _onUnload)
                _onUnload(mapIt->second.waterNode, key);
            cancelMeshJob(mapIt->second);
            _chunks.erase(mapIt);
            _genPendingSet.erase(key);
            it = _unloadQueue.erase(it);
            ++processed;
        }
        else
        {
            ++it;
        }
    }
}

// =========================================================================
//  updateChunkFog — обновление тумана на чанках по дистанции до игрока
// =========================================================================
void ChunkManager::updateChunkFog()
{
    float maxDist = static_cast<float>(_cfg.renderDistance * CHUNK_SIZE_X);
    float fogStartDist = maxDist * FOG_START_FACTOR;
    float fogEndDist = maxDist * FOG_END_FACTOR;
    float fogRange = fogEndDist - fogStartDist;
    if (fogRange <= 0.0f)
        return;

    float px = _playerWorldPos.x;
    float pz = _playerWorldPos.z;

    for (auto& [key, entry] : _chunks)
    {
        if (entry.status != ChunkStatus::Active)
            continue;

        // Центр чанка в мировых координатах
        ax::Vec3 center = chunkToWorld(key);
        center.x += CHUNK_SIZE_X * 0.5f;
        center.z += CHUNK_SIZE_Z * 0.5f;

        float dx = center.x - px;
        float dz = center.z - pz;
        float dist = std::sqrt(dx * dx + dz * dz);

        float fogFactor = 1.0f - std::clamp((dist - fogStartDist) / fogRange, 0.0f, 1.0f);
        uint8_t opacity = static_cast<uint8_t>(fogFactor * 255.0f);

        if (entry.visualNode)
            entry.visualNode->setOpacity(opacity);
        // Вода не трогаем — у неё кастомный шейдер с per-pixel туманом
    }
}

// =========================================================================
//  Геттеры
// =========================================================================
ax::Node* ChunkManager::getChunkNode(const ChunkKey& key) const
{
    auto it = _chunks.find(key);
    if (it != _chunks.end() && it->second.status == ChunkStatus::Active)
        return it->second.visualNode;
    return nullptr;
}
bool ChunkManager::isChunkActive(const ChunkKey& key) const
{
    auto it = _chunks.find(key);
    return it != _chunks.end() && it->second.status == ChunkStatus::Active;
}

// =========================================================================
//  setPixelArtAA — переключение кастомного шейдера террейна на лету
// =========================================================================
void ChunkManager::setPixelArtAA(bool enabled)
{
    if (_cfg.pixelArtAA == enabled)
        return;
    _cfg.pixelArtAA = enabled;

    // 1) Сэмплер атласа: LINEAR_MIPMAP_LINEAR для AA-шейдера, иначе режим из конфига.
    //    Действует немедленно для всех нодов (текстура общая).
    applyTextureFilter();

    // 2) Помечаем активные чанки dirty — ноды пересоберутся в processDirtyChunks()
    //    уже с актуальным _cfg.pixelArtAA (с программой или на чистом UNLIT).
    for (auto& [key, entry] : _chunks)
    {
        if (entry.status == ChunkStatus::Active && entry.visualNode)
            entry.dirty = true;
    }
    AXLOGI("ChunkManager: pixelArtAA = %d (chunks marked for rebuild)", enabled ? 1 : 0);
}

// =========================================================================
//  Фильтрация текстур
// =========================================================================
void ChunkManager::applyTextureFilter()
{
    if (!_terrainAtlas)
        return;

    // Настройка параметров текстуры для пиксельной фильтрации
    ax::Texture2D::TexParams texParams;

    // Режим pixel-art AA: атлас должен быть ЛИНЕЙНЫМ (+ мипы), «резкость»
    // текселей восстанавливает фрагментный шейдер custom/terrain_fs по fwidth.
    if (_cfg.pixelArtAA)
    {
        texParams.magFilter = ax::backend::SamplerFilter::LINEAR;
        texParams.minFilter = ax::backend::SamplerFilter::LINEAR_MIPMAP_LINEAR;
        if (!_terrainAtlas->hasMipmaps())
        {
            _terrainAtlas->generateMipmap();
            AXLOGI("ChunkManager: mipmaps generated");
        }
        _terrainAtlas->setTexParameters(texParams);
        AXLOGI("ChunkManager: texture filter = LINEAR_MIPMAP_LINEAR (pixel-art AA shader)");
        return;
    }

    switch (_cfg.textureFilter)
    {
    case TextureFilterMode::NEAREST:
        texParams.magFilter = ax::backend::SamplerFilter::NEAREST;
        texParams.minFilter = ax::backend::SamplerFilter::NEAREST;
        AXLOGI("ChunkManager: texture filter = NEAREST");
        break;

    case TextureFilterMode::NEAREST_MIPMAP_NEAREST:
        texParams.magFilter = ax::backend::SamplerFilter::NEAREST;
        texParams.minFilter = ax::backend::SamplerFilter::NEAREST_MIPMAP_NEAREST;
        // Автоматическая генерация mipmaps при первом использовании
        if (!_terrainAtlas->hasMipmaps())
        {
            _terrainAtlas->generateMipmap();
            AXLOGI("ChunkManager: mipmaps generated");
        }
        AXLOGI("ChunkManager: texture filter = NEAREST_MIPMAP_NEAREST");
        break;

    case TextureFilterMode::NEAREST_MIPMAP_LINEAR:
        // NEAREST внутри мип-уровня (тексели резкие, как пиксель-арт), но ЛИНЕЙНО
        // между двумя ближайшими уровнями — убирает «скачки» мипов при движении
        // камеры (главный источник дрожания дальних кубов), без анизотропии.
        texParams.magFilter = ax::backend::SamplerFilter::NEAREST;
        texParams.minFilter = ax::backend::SamplerFilter::NEAREST_MIPMAP_LINEAR;
        if (!_terrainAtlas->hasMipmaps())
        {
            _terrainAtlas->generateMipmap();
            AXLOGI("ChunkManager: mipmaps generated");
        }
        AXLOGI("ChunkManager: texture filter = NEAREST_MIPMAP_LINEAR");
        break;
    }

    // Применяем параметры к текстуре-атласу
    _terrainAtlas->setTexParameters(texParams);
}

// =========================================================================
//  Установка типа блока по мировым координатам
// =========================================================================

bool ChunkManager::setBlockAtWorldPos(const ax::Vec3& worldPos, BlockId id)
{
    ChunkKey key = worldToChunk(worldPos);
    auto it      = _chunks.find(key);
    if (it == _chunks.end())
        return false;

    ChunkEntry& entry = it->second;
    if (entry.status != ChunkStatus::Active || !entry.chunkData)
        return false;

    ax::Vec3 base = chunkToWorld(key);
    int lx        = static_cast<int>(std::floor(worldPos.x)) - static_cast<int>(base.x);
    int ly        = static_cast<int>(std::floor(worldPos.y)) - static_cast<int>(base.y);
    int lz        = static_cast<int>(std::floor(worldPos.z)) - static_cast<int>(base.z);

    if (lx < 0 || lx >= CHUNK_SIZE_X)
        return false;
    if (ly < 0 || ly >= CHUNK_SIZE_Y)
        return false;
    if (lz < 0 || lz >= CHUNK_SIZE_Z)
        return false;

    entry.chunkData->setBlock(lx, ly, lz, id);
    entry.dirty = true;

    // Помечаем соседние чанки dirty если блок на границе
    const int lCoords[3]    = {lx, ly, lz};
    const int chunkSizes[3] = {CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z};

    const int kAxis[6] = {0, 0, 1, 1, 2, 2};
    const int kDir[6]  = {1, -1, 1, -1, 1, -1};

    for (int i = 0; i < 6; ++i)
    {
        int axis = kAxis[i];
        int dir  = kDir[i];

        bool onBoundary = (dir > 0) ? (lCoords[axis] == chunkSizes[axis] - 1) : (lCoords[axis] == 0);

        if (!onBoundary)
            continue;

        ChunkKey neighborKey = key;
        if (axis == 0)
            neighborKey.x += dir;
        else if (axis == 1)
            neighborKey.y += dir;
        else
            neighborKey.z += dir;

        auto nit = _chunks.find(neighborKey);
        if (nit != _chunks.end() && nit->second.status == ChunkStatus::Active)
        {
            nit->second.dirty = true;
        }
    }

    return true;
}
