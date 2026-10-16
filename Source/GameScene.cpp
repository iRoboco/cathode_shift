#include "GameScene.h"
#include "PauseOverlay.h"
#include "SaveGameService.h"
#include "UiTheme.h"
#include "PerlinNoise.hpp"
#include "WorldGenerator.h"
#include <algorithm>
#include <thread>
#include <climits>

USING_NS_AX;

namespace
{
/// @name Ключи пользовательских настроек (UserDefault) — общие с MainMenuScene.
/// @{
constexpr const char* kKeyRenderDistance = "settings.renderDistance";
constexpr const char* kKeyMouseSens      = "settings.mouseSensitivity";
constexpr const char* kKeyPixelArtAA     = "settings.pixelArtAA";
constexpr const char* kKeyTilePixelate   = "settings.tilePixelate";
constexpr const char* kKeyTilePixelRes   = "settings.tilePixelRes";
/// @}
}  // namespace

// =========================================================================
//  Фабричный метод
// =========================================================================
ax::Scene* GameScene::create()
{
    auto* ret = new (std::nothrow) GameScene();
    if (ret && ret->init())
    {
        ret->autorelease();
        return ret;
    }
    AX_SAFE_DELETE(ret);
    return nullptr;
}

// =========================================================================
//  Создание из сохранения
// =========================================================================
GameScene* GameScene::createFromSave()
{
    auto* ret = new (std::nothrow) GameScene();
    if (ret && ret->init())
    {
        ret->autorelease();
        // Применяем сохранённые данные поверх инициализированного мира.
        // При отсутствии/повреждении файла остаёмся на спавне по умолчанию.
        SaveGameService::loadGame(ret);
        return ret;
    }
    AX_SAFE_DELETE(ret);
    return nullptr;
}

// =========================================================================
//  Деструктор
// =========================================================================
GameScene::~GameScene()
{
    // Полная очистка только при уничтожении объекта.
    // onExit() вызывает pause(), который сохраняет данные.
    // shutdown() здесь гарантирует, что ресурсы освобождены
    // при replaceScene() или завершении приложения.
    _chunkMgr.shutdown();
}

// =========================================================================
//  init
// =========================================================================
bool GameScene::init()
{
    if (!Scene::init())
        return false;

    auto glView  = ax::Director::getInstance()->getGLView();
    float w      = glView->getFrameSize().width;
    float h      = glView->getFrameSize().height;
    float aspect = (h > 0.0f) ? (w / h) : 16.0f / 9.0f;
    _mainCamera  = ax::Camera::createPerspective(60.0f, aspect, 0.1f, 1000.0f);
    AX_ASSERT(_mainCamera && "Failed to create perspective camera");

    // Камера с USER1 должна рендерить только 3D-ноды.
    // Устанавливаем depth < 0 чтобы 3D-камера рендерилась ПЕРЕД 2D default-камерой.
    _mainCamera->setCameraFlag(ax::CameraFlag::USER1);
    _mainCamera->setDepth(-1);

    this->addChild(_mainCamera);

    // Создаем небо (Skybox) с текстурным кубом
    auto _textureCube = TextureCube::create("envmap_miramar/miramar_lf.tga", "envmap_miramar/miramar_rt.tga",
                                            "envmap_miramar/miramar_up.tga", "envmap_miramar/miramar_dn.tga",
                                            "envmap_miramar/miramar_ft.tga", "envmap_miramar/miramar_bk.tga");
    auto _skyBox      = Skybox::create();
    _skyBox->setTexture(_textureCube);
    _skyBox->setCameraMask((unsigned short)CameraFlag::USER1);
    this->addChild(_skyBox);

    // Пользовательские настройки (заданы в меню «Настройки»), с безопасными дефолтами.
    auto* ud                  = UserDefault::getInstance();
    const int userRenderDist  = std::clamp(ud->getIntegerForKey(kKeyRenderDistance, 10), 4, 32);
    const float userMouseSens = std::clamp(ud->getFloatForKey(kKeyMouseSens, 0.1f), 0.02f, 0.40f);

    // Конфигурация чанков
    ChunkManager::Config cfg;
    cfg.renderDistance           = userRenderDist;
    cfg.unloadMargin             = 3;
    cfg.workerThreadCount        = std::max(1u, std::thread::hardware_concurrency() / 2);
    cfg.maxGenerationsPerFrame   = 2;
    cfg.maxUnloadsPerFrame       = 4;
    cfg.maxQueueSize             = 128;
    // Подняли с 2 до 4: после фиксов мешинга (реалистичный reserve + обрезка по Y)
    // и отложенной перестройки до «оседания» соседей каждый ребилд стал заметно
    // дешевле и реже, поэтому осевшую пачку чанков можно разгребать быстрее.
    cfg.maxDirtyRebuildsPerFrame = 4;
    // Кастомный шейдер террейна: antialiased pixel art (резко вблизи, без дрожания
    // вдали). Атлас при этом грузится с LINEAR_MIPMAP_LINEAR. Управляется из меню
    // паузы и сохраняется в UserDefault (по умолчанию включён).
    cfg.pixelArtAA               = ud->getBoolForKey(kKeyPixelArtAA, true);
    // Пикселизация тайлов: uniform-переключатель «на лету» (клавиша P / меню паузы),
    // без ребилда мешей и перегенерации атласа. Настройка сохраняется.
    cfg.pixelateTiles            = ud->getBoolForKey(kKeyTilePixelate, false);
    cfg.tilePixelRes             = std::clamp(ud->getFloatForKey(kKeyTilePixelRes, 32.0f), 4.0f, 128.0f);
 
    cfg.biomes.frequency     = {0.40f, 0.20f, 0.40f, 0.0f, 0.0f};
    cfg.biomes.macroCellSize = 64;
    cfg.biomes.blendWidth    = 16;
	_chunkMgr.init(cfg);

    // Генерация мира — общий генератор (WorldGenerator.h): рельеф, пляжи,
    // гравийные пятна и рудные жилы по глубине. Используется и в главном меню.
    _chunkMgr.setOnGenerate([](ChunkData& chunk) { worldgen::generate(chunk); });

    // CameraFlag — маска USER1 на визуализируемые ноды
    _chunkMgr.setOnVisualize([this](ax::Node* node, const ChunkKey& key) {
        if (!node)
            return;
        // ВАЖНО: водный нод определяем ДО перезаписи тега.
        // buildWaterVisualNode помечает воду тегом WATER_NODE_TAG. Если затереть
        // его chunk-key тегом, нод не попадёт в _waterNodes, и цикл анимации в
        // update() (u_time / u_camWorld) будет работать по пустому списку — вода
        // станет статичной. Поэтому водному ноду тег НЕ перезаписываем.
        const bool isWater = (node->getTag() == WATER_NODE_TAG);
        if (!isWater)
        {
            int tag = ((key.x & 0xFFFF) << 16) | (key.z & 0xFFFF);
            node->setTag(tag);
        }
        // Маска USER1 чтобы 3D-камера рендерила этот нод
        node->setCameraMask(static_cast<int>(ax::CameraFlag::USER1));
        this->addChild(node);

        // Если это водный нод — добавляем в список для анимации
        if (isWater)
            _waterNodes.push_back(node);
        else
            _terrainNodes.push_back(node);  // террейн: покадровый u_time для дрожания желе
    });

    _chunkMgr.setOnUnload([this](ax::Node* node, const ChunkKey& key) {
        if (!node)
            return;
        node->removeFromParentAndCleanup(true);
        // Удаляем ссылку из кэшей анимации (remove-erase idiom)
        _waterNodes.erase(std::remove(_waterNodes.begin(), _waterNodes.end(), node), _waterNodes.end());
        _terrainNodes.erase(std::remove(_terrainNodes.begin(), _terrainNodes.end(), node), _terrainNodes.end());
    });

    // Создание контроллера (чувствительность мыши — из настроек)
    _playerController = FirstPersonController::create(_mainCamera, 10.0f, userMouseSens);
    AX_ASSERT(_playerController && "Failed to create FirstPersonController");
    _playerController->setChunkManager(&_chunkMgr);
    _playerController->setInitialPosition(ax::Vec3(0.0f, 60.0f, 0.0f));
    this->addChild(_playerController);

    _playerNode = _playerController;

    // ============================================================================
    // Рейкастер — подсветка грани и редактирование мира
    // ============================================================================
    _raycaster = VoxelRaycasterNode::create(_mainCamera, &_chunkMgr, 8.0f);
    AX_ASSERT(_raycaster && "Failed to create VoxelRaycasterNode");

    // Настройка визуала подсветки
    _raycaster->setCameraMask(static_cast<unsigned short>(CameraFlag::USER1));
    _raycaster->setFaceHighlightColor({1.0f, 1.0f, 1.0f, 0.25f});  // Подсветка грани
    _raycaster->setPlacePreviewColor({0.2f, 0.9f, 0.2f, 0.2f});    // Выбор места кубика
    _raycaster->setPlacePreviewVisible(true);

    // Передаем ссылку на капсулу игрока в рейкастер
    _raycaster->setPlayerCapsule(&_playerController->getCapsule());

    // Коллбек при наведении на блок — меняем цвет подсветки по типу блока
    _raycaster->setOnHit([this](const VoxelHit& hit) {
        switch (hit.blockId)
        {
        case BLOCK_GRASS:
            _raycaster->setFaceHighlightColor({0.2f, 0.8f, 0.2f, 0.3f});
            break;
        case BLOCK_STONE:
            _raycaster->setFaceHighlightColor({0.6f, 0.6f, 0.6f, 0.3f});
            break;
        case BLOCK_DIRT:
            _raycaster->setFaceHighlightColor({0.5f, 0.35f, 0.2f, 0.3f});
            break;
        default:
            _raycaster->setFaceHighlightColor({1.0f, 1.0f, 1.0f, 0.25f});
            break;
        }
    });
    _raycaster->setOnMiss([this]() {
        // Можно скрыть HUD-подсказку
    });
    this->addChild(_raycaster);
    _playerController->setRaycaster(_raycaster);

    // ============================================================================
    // Подводный оверлей — полноэкранный 2D-слой синезелёного тинта
    // Рендерится default 2D-камерой поверх всей 3D-сцены (depth=0 > depth=-1).
    // Начинаем полностью прозрачным (opacity=0), плавно проявляем в update().
    // ============================================================================
    {
        auto glView = ax::Director::getInstance()->getGLView();
        float sw    = glView->getFrameSize().width;
        float sh    = glView->getFrameSize().height;
        // Синезелёный цвет воды: R=20 G=80 B=120 — холодный подводный оттенок
        _underwaterOverlay = ax::LayerColor::create(ax::Color4B(20, 80, 120, 0), sw, sh);
        AX_ASSERT(_underwaterOverlay);
        // Не назначаем CameraFlag::USER1 — оверлей рендерит default 2D-камера
        // Высокий z-order чтобы перекрывать любые другие 2D-элементы (HUD и т.д.)
        this->addChild(_underwaterOverlay, 100);
    }

    // Включаем обработку клавиатуры для перезапуска уровня
    auto listener          = ax::EventListenerKeyboard::create();
    listener->onKeyPressed = AX_CALLBACK_2(GameScene::onKeyPressed, this);
    _eventDispatcher->addEventListenerWithSceneGraphPriority(listener, this);

    // Изменен приоритет, чтобы GameScene::update вызывалась после FPC
    scheduleUpdateWithPriority(10);

    // Сбрасываем _lastPlayerChunk чтобы чанки начали грузиться сразу,
    // а не после первого движения игрока (нажатия клавиши).
    // Вызываем ПОСЛЕ scheduleUpdate() чтобы update() сработал с правильным флагом.
    _chunkMgr.forceUpdate();

    return true;
}

// =========================================================================
//  onEnter
// =========================================================================
void GameScene::onEnter()
{
    Scene::onEnter();

    // Глобальная идемпотентная инициализация ImGui — нужна для меню паузы,
    // даже если в игру вошли напрямую (минуя главное меню).
    ui_theme::setup();

    // При возврате на сцену не «расхватываем» управление, если игра на паузе.
    if (_playerController && !_paused)
    {
        _playerController->setEnabled(true);
    }

    // resume вместо повторного init.
    // Перезапускает воркеры, инициирует обновление чанков.
    // Если после shutdown() (полное уничтожение) — resume() ничего не сделает
    // (проверка _initialized внутри).
    _chunkMgr.resume();
}

// =========================================================================
//  onExit
// =========================================================================
void GameScene::onExit()
{
    // pause вместо shutdown.
    // При сворачивании приложения нужно ЗАМОРОЗИТЬ мир, а не УНИЧТОЖИТЬ его.
    // shutdown() вызывается только в деструкторе.
    _chunkMgr.pause();

    if (_playerController)
    {
        _playerController->setEnabled(false);
    }

    Scene::onExit();
}

// =========================================================================
//  update
// =========================================================================
void GameScene::update(float dt)
{
    Scene::update(dt);

    // На паузе мир заморожен: не двигаем чанки/воду/игрока. Рендер и меню ImGui
    // продолжают работать (ImGui рисуется вне планировщика), поэтому FPS не падает.
    if (_paused)
        return;

    // === Анимация воды и детектирование погружения ===
    _animTime += dt;

    // Определяем, погружена ли камера в воду по позиции глаз игрока.
    // Глаза располагаются на высоте 0.85 × высота капсулы (1.8) = ~1.53 над подошвой.
    bool newUnderwater = false;
    if (_playerController)
    {
        ax::Vec3 eyePos  = _playerController->getPlayerPosition() + ax::Vec3(0.0f, 1.53f, 0.0f);
        BlockId eyeBlock = _chunkMgr.getBlockAtWorldPos(eyePos);
        newUnderwater    = (eyeBlock == BLOCK_WATER);
    }
    _cameraUnderwater = newUnderwater;

    // === Подводный оверлей — плавный синезелёный тинт поверх экрана ===
    // Не трогаем terrain-ноды или материалы: LayerColor — чистый 2D без Z-буфера,
    // не имеет никакого влияния на 3D depth/blending.
    if (_underwaterOverlay)
    {
        // Плавный переход: 5 ед./сек → полное погружение за ~0.2 сек
        constexpr float BLEND_SPEED = 5.0f;
        float target                = _cameraUnderwater ? 1.0f : 0.0f;
        _underwaterBlend += (target - _underwaterBlend) * std::min(1.0f, BLEND_SPEED * dt);
        _underwaterBlend = std::clamp(_underwaterBlend, 0.0f, 1.0f);

        // Максимальная opacity оверлея: 160/255 (~63%) — достаточно для ощущения глубины,
        // но не настолько плотно, чтобы полностью скрыть блоки.
        uint8_t overlayOpacity = static_cast<uint8_t>(_underwaterBlend * 160.0f);
        _underwaterOverlay->setOpacity(overlayOpacity);
    }

    // Базовая opacity:
    // - Над водой: 165 (~65%) — полупрозрачная поверхность, дно видно.
    // - Под водой: 220 (~86%) — инвертированная грань почти непрозрачна,
    //   затеняет блоки выше уровня моря.
    uint8_t baseOpacity = _cameraUnderwater ? 220u : 165u;

    // Плавная пульсация (0.15 Гц) — живость поверхности
    uint8_t waterOpacity =
        static_cast<uint8_t>(baseOpacity + std::clamp(static_cast<int>(std::sin(_animTime * 2.0f) * 12.0f), 0, 35));

    const ax::Vec3 camWorld = _mainCamera ? _mainCamera->getPosition3D() : ax::Vec3::ZERO;

    // Пикселизация тайлов: uniform-only (0 = выкл), переключается на лету —
    // общий переключатель для террейна и воды.
    const float tilePx = _chunkMgr.isTilePixelate() ? _chunkMgr.getTilePixelRes() : 0.0f;

    for (auto* node : _waterNodes)
    {
        if (!node)
            continue;

        // Opacity влияет на воду через авто-биндинг u_color в шейдере.
        node->setOpacity(waterOpacity);

        // Прокидываем время и позицию камеры в водный шейдер (анимация волн,
        // Fresnel/specular). У каждого нода собственный ProgramState.
        auto* mr = static_cast<ax::MeshRenderer*>(node);
        if (auto* ps = mr->getProgramState())
        {
            auto locTime = ps->getUniformLocation("u_time");
            auto locCam  = ps->getUniformLocation("u_camWorld");
            ps->setUniform(locTime, &_animTime, sizeof(float));
            ps->setUniform(locCam, &camWorld, sizeof(ax::Vec3));
            auto locPx = ps->getUniformLocation("u_tilePx");
            if (locPx)
                ps->setUniform(locPx, &tilePx, sizeof(float));
        }
    }

    // === Дрожание желе: те же часы анимации уходят в шейдер террейна ===
    // Смещение считается в вершинном шейдере по мировым координатам (маска
    // запечена в вершинах), поэтому здесь только тикаем u_time для каждого нода.
    // u_camWorld нужен фрагментнику для френеля желе-шиммера.
    for (auto* node : _terrainNodes)
    {
        if (!node)
            continue;
        auto* mr = static_cast<ax::MeshRenderer*>(node);
        if (auto* ps = mr->getProgramState())
        {
            // У pixel-art шейдера (terrain_vs) и встроенного UNLIT u_time нет —
            // getUniformLocation вернёт невалидный location, пропускаем их.
            auto locTime = ps->getUniformLocation("u_time");
            if (locTime)
                ps->setUniform(locTime, &_animTime, sizeof(float));
            auto locCam = ps->getUniformLocation("u_camWorld");
            if (locCam)
                ps->setUniform(locCam, &camWorld, sizeof(ax::Vec3));
            auto locPx = ps->getUniformLocation("u_tilePx");
            if (locPx)
                ps->setUniform(locPx, &tilePx, sizeof(float));
        }
    }

    ax::Vec3 playerPos = _playerController ? _playerController->getPlayerPosition() : ax::Vec3::ZERO;
    _chunkMgr.update(playerPos);
}

ax::Vec3 GameScene::getPlayerPosition() const
{
    // Возвращаем позицию из контроллера (низ капсулы) — она же используется при сохранении.
    return _playerController ? _playerController->getPlayerPosition()
                             : (_playerNode ? _playerNode->getPosition3D() : ax::Vec3::ZERO);
}

void GameScene::setPlayerPosition(const ax::Vec3& pos)
{
    if (_playerController)
        _playerController->setInitialPosition(pos);
    // Принудительно пересчитать видимые чанки вокруг новой позиции.
    _chunkMgr.forceUpdate();
}

// =========================================================================
//  Пауза / возобновление
// =========================================================================
void GameScene::pauseGame()
{
    if (_paused)
        return;
    _paused = true;

    // Замораживаем ввод игрока и освобождаем курсор для взаимодействия с меню.
    if (_playerController)
    {
        _playerController->setEnabled(false);
        _playerController->setMouseCaptured(false);
    }

    // Затемняющий оверлей + меню паузы на ImGui поверх живой сцены.
    _pauseOverlay = PauseOverlay::create(this);
    if (_pauseOverlay)
        addChild(_pauseOverlay, 999);  // выше подводного оверлея (z=100) и 3D-мира
}

void GameScene::resumeGame()
{
    if (!_paused)
        return;
    _paused = false;

    if (_pauseOverlay)
    {
        _pauseOverlay->removeFromParent();  // onExit() снимет ImGui render loop
        _pauseOverlay = nullptr;
    }

    // Возвращаем управление: захват курсора восстанавливаем только для FPS-режима.
    if (_playerController)
    {
        _playerController->setEnabled(true);
        _playerController->setMouseCaptured(!_playerController->isFreeFlightMode());
    }
}

// =========================================================================
//  setPixelArtAA — переключение pixel-art AA из меню паузы (вживую + сохранение)
// =========================================================================
void GameScene::setPixelArtAA(bool enabled)
{
    _chunkMgr.setPixelArtAA(enabled);
    auto* ud = ax::UserDefault::getInstance();
    ud->setBoolForKey(kKeyPixelArtAA, enabled);
    ud->flush();
}

// =========================================================================
//  setTilePixelate / setTilePixelRes — пикселизация тайлов (вживую + сохранение)
// =========================================================================
void GameScene::setTilePixelate(bool enabled)
{
    _chunkMgr.setTilePixelate(enabled);  // uniform-only: без ребилда мешей, мгновенно
    auto* ud = ax::UserDefault::getInstance();
    ud->setBoolForKey(kKeyTilePixelate, enabled);
    ud->flush();
}

void GameScene::setTilePixelRes(float px)
{
    _chunkMgr.setTilePixelRes(px);
    auto* ud = ax::UserDefault::getInstance();
    ud->setFloatForKey(kKeyTilePixelRes, _chunkMgr.getTilePixelRes());
    ud->flush();
}

// =========================================================================
//  onKeyPressed
// =========================================================================
void GameScene::onKeyPressed(ax::EventKeyboard::KeyCode keyCode, ax::Event* event)
{
    // ESC — переключение паузы (показ/скрытие меню паузы).
    if (keyCode == ax::EventKeyboard::KeyCode::KEY_ESCAPE)
    {
        if (_paused)
            resumeGame();
        else
            pauseGame();
        return;
    }

    // Перезапуск уровня по нажатию F12
    if (keyCode == ax::EventKeyboard::KeyCode::KEY_F12)
    {
        auto scene = GameScene::create();
        ax::Director::getInstance()->replaceScene(scene);
    }

    // P — пикселизация тайлов на лету (ретро-режим). Живое применение + сохранение.
    if (keyCode == ax::EventKeyboard::KeyCode::KEY_P)
        setTilePixelate(!_chunkMgr.isTilePixelate());
}
