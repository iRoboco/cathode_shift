#include "MenuScene.h"
#include "GameScene.h"
#include "PerlinNoise.hpp"
#include <cmath>
#include <functional>
#include <algorithm>

using namespace ax;

Scene* MenuScene::create()
{
    auto* scene = new (std::nothrow) MenuScene();
    if (scene && scene->init())
    {
        scene->autorelease();
        return scene;
    }
    AX_SAFE_DELETE(scene);
    return nullptr;
}

bool MenuScene::init()
{
    if (!Scene::init())
        return false;

    // Инициализируем менеджер чанков для фонового 3D-мира
    setupChunkManager();

    // Создаём и настраиваем орбитальную камеру
    _orbitCamera = Camera::create();
    // Начальная позиция камеры на "севере" орбиты
    _orbitCamera->setPosition3D({0.0f, _orbitHeight, -_orbitRadius});

    //_orbitCamera = Camera::createPerspective(60.0f, aspect, 0.1f, 1000.0f);
    //_orbitCamera->setCameraFlag(CameraFlag::USER1);

    // Камера всегда смотрит в центр мира, ось Y направлена вверх
    _orbitCamera->lookAt(_worldCenter, Vec3::UNIT_Y);
    addChild(_orbitCamera);

    _orbitCamera->setDepth(-1);

    // Создаём UI-меню поверх 3D-сцены
    setupUI();

    // Активируем игровой цикл для обновления камеры и менеджера чанков
    scheduleUpdate();

    return true;
}

MenuScene::~MenuScene()
{
    // Полный останов и очистка ресурсов только при реальном уничтожении сцены
    // (не вызывается при pause/resume сворачивания приложения)
    _chunkMgr.shutdown();
}

void MenuScene::onEnter()
{
    Scene::onEnter();
    // При возврате на сцену возобновляем генерацию и рендеринг чанков
    _chunkMgr.resume();
}

void MenuScene::onExit()
{
    Scene::onExit();
    // При уходе со сцены (сворачивание или переход) ставим мир на паузу
    _chunkMgr.pause();
}

void MenuScene::update(float dt)
{
    // =========================================================================
    // 1. Орбитальное движение камеры
    // Вычисляем новую позицию на окружности: x = R*cos(θ), z = R*sin(θ)
    // Y остаётся фиксированным для сохранения "bird's-eye" ракурса.
    // =========================================================================
    _orbitAngle += _orbitSpeed * dt;
    float camX = std::cos(_orbitAngle) * _orbitRadius;
    float camZ = std::sin(_orbitAngle) * _orbitRadius;

    // Применяем новые координаты и обновляем вектор взгляда
    _orbitCamera->setPosition3D({camX, _orbitHeight, camZ});
    _orbitCamera->lookAt(_worldCenter, Vec3::UNIT_Y);

    // =========================================================================
    // 2. Обновление менеджера чанков
    // Передаём фиксированную позицию центра. ChunkManager самостоятельно
    // определит новые чанки в радиусе renderDistance и запустит их загрузку.
    // =========================================================================
    _chunkMgr.update(_worldCenter);
}

void MenuScene::setupChunkManager()
{
    // Конфигурация менеджера: оптимизирована для превью, а не для геймплея
    ChunkManager::Config cfg;
    cfg.renderDistance           = 5;
    cfg.unloadMargin             = 3;
    cfg.workerThreadCount        = std::max(1u, std::thread::hardware_concurrency() / 2);
    cfg.maxGenerationsPerFrame   = 8;
    cfg.maxUnloadsPerFrame       = 4;
    cfg.maxQueueSize             = 128;
    cfg.maxDirtyRebuildsPerFrame = 2;
    cfg.textureFilter            = TextureFilterMode::NEAREST_MIPMAP_NEAREST;

    _chunkMgr.init(cfg);

    // Коллбек генерации: выполняется в фоновом потоке воркера.
    // Заполняет воксельные данные без сложных вычислений шума.
    _chunkMgr.setOnGenerate([](ChunkData& chunk) {
        const ChunkKey& key = chunk.getKey();
        auto basePos        = ChunkManager::chunkToWorld(key);
        thread_local siv::PerlinNoise perlin(42);
        thread_local siv::PerlinNoise rockNoise(137);  // ← новый
        thread_local siv::PerlinNoise dirtNoise(271);  // ← новый

        for (int x = 0; x < CHUNK_SIZE_X; ++x)
        {
            for (int z = 0; z < CHUNK_SIZE_Z; ++z)
            {
                float wx      = basePos.x + x;
                float wz      = basePos.z + z;
                float terrain = perlin.octave2D_01(wx * 0.02, wz * 0.02, 6) * 40.0f;
                float biome   = perlin.octave2D_01(wx * 0.005, wz * 0.005, 4) * 15.0f - 5.0f;
                int surfaceY  = std::clamp(static_cast<int>(terrain + biome + 30), 1, CHUNK_SIZE_Y - 2);

                // Шум для выходов породы на поверхность
                float rockVal = rockNoise.octave2D_01(wx * 0.08, wz * 0.08, 3);
                float dirtVal = dirtNoise.octave2D_01(wx * 0.06, wz * 0.06, 2);

                for (int y = 0; y <= surfaceY; ++y)
                {
                    BlockId block;
                    if (y == surfaceY)
                    {
                        // Камень выбивается там, где rockVal > 0.72
                        // Земля выбивается там, где dirtVal > 0.65 (но не там, где камень)
                        if (rockVal > 0.72f)
                            block = BLOCK_STONE;
                        else if (dirtVal > 0.65f)
                            block = BLOCK_DIRT;
                        else
                            block = BLOCK_GRASS;
                    }
                    else if (y > surfaceY - 4)
                    {
                        block = BLOCK_DIRT;
                    }
                    else
                    {
                        block = BLOCK_STONE;
                    }
                    chunk.setBlock(x, y, z, block);
                }
                // Генерация морей
                constexpr int SEA_LEVEL = 38;
                if (surfaceY < SEA_LEVEL)
                {
                    for (int y = surfaceY + 1; y <= SEA_LEVEL; ++y)
                        chunk.setBlock(x, y, z, BLOCK_WATER);
                }
            }
        }
    });

    // Коллбек визуализации: выполняется в главном потоке.
    // Добавляет созданный MeshRenderer в граф сцены.
    _chunkMgr.setOnVisualize([this](Node* node, const ChunkKey&) {
        if (node)
        {
            // Глобальный Z-Order 0 гарантирует, что чанки рисуются до UI
            node->setGlobalZOrder(0);
            addChild(node);
        }
    });

    // Коллбек выгрузки: выполняется в главном потоке.
    // Безопасно удаляет нод из сцены и освобождает VRAM-буферы.
    _chunkMgr.setOnUnload([this](Node* node, const ChunkKey&) {
        if (node)
        {
            // removeFromParent корректно обрабатывает ref-count в Axmol
            node->removeFromParent();
        }
    });

    // Принудительно запускаем процесс загрузки чанков вокруг центра.
    // Без этого первый кадр будет пустым до первого обновления позиции.
    _chunkMgr.forceUpdate();
}

void MenuScene::setupUI()
{
    auto visibleSize = Director::getInstance()->getVisibleSize();
    auto origin      = Director::getInstance()->getVisibleOrigin();
    Vec2 center      = origin + Vec2(visibleSize.width / 2, visibleSize.height / 2);

    // Полупрозрачный фон для контраста с 3D-сценой
    auto bg = LayerColor::create(Color4B(15, 15, 25, 190));
    bg->setContentSize(Size(420, 300));
    bg->setPosition(center - Vec2(210, 150));  // Смещение от центра к левому нижнему углу прямоугольника
    bg->setGlobalZOrder(1000);  ///< UI всегда поверх 3D-мира
    addChild(bg);

    _mainMenu = Menu::create();
    _mainMenu->setPosition(Vec2::ZERO);
    _mainMenu->setGlobalZOrder(1001);

    // Лямбда-фабрика элементов меню
    // Создаёт Label, оборачивает в MenuItemLabel, привязывает коллбек и настраивает начальный масштаб
    auto createMenuItem = [this](const std::string& text, std::function<void(Object*)> callback) -> MenuItemLabel* {
        auto label = Label::createWithTTF(text, "fonts/Marker Felt.ttf", 32);
        if (!label)
            return nullptr;

        // Коллбек нажатия: добавляем лёгкую анимацию "пружинистости" через Action
        auto item = MenuItemLabel::create(label, callback);
        item->setCallback([item, callback](Object* sender) {
            // Кратковременное увеличение и возврат к исходному размеру
            item->runAction(Sequence::create(ScaleTo::create(0.1f, 1.1f), ScaleTo::create(0.15f, 1.0f),
                                             CallFunc::create([callback, item, sender]() { callback(sender); }),
                                             nullptr));
        });
        return item;
    };

    // Формируем элементы меню с лямбда-обработчиками
    auto playItem = createMenuItem("let's Play!", [this](Object*) {
        AXLOGI("MenuScene: Переход в GameScene");
        auto* game = GameScene::create();
        if (game)
            Director::getInstance()->replaceScene(TransitionFade::create(0.8f, game));
    });

    auto saveItem = createMenuItem("Save", [this](Object*) { AXLOGI("MenuScene: Сохранение прогресса (заглушка)"); });

    auto loadItem = createMenuItem("Load", [this](Object*) { AXLOGI("MenuScene: Загрузка прогресса (заглушка)"); });

    auto settingsItem =
        createMenuItem("Settings", [this](Object*) { AXLOGI("MenuScene: Открытие меню настроек (заглушка)"); });

    // Добавляем в Menu и расставляем вертикально по центру фона
    if (playItem && saveItem && loadItem && settingsItem)
    {
        _mainMenu->addChild(playItem, 0, 1);
        _mainMenu->addChild(saveItem, 0, 2);
        _mainMenu->addChild(loadItem, 0, 3);
        _mainMenu->addChild(settingsItem, 0, 4);

        // Ручное позиционирование элементов внутри Menu
        float startY  = center.y + 90;
        float spacing = 55.0f;
        int tag       = 1;
        for (auto* item : {playItem, saveItem, loadItem, settingsItem})
        {
            item->setPosition(Vec2(center.x, startY));
            startY -= spacing;
            item->setTag(tag++);
        }
    }

    addChild(_mainMenu);
}
