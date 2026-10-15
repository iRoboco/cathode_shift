#include "MainMenuScene.h"
#include "SceneManager.h"
#include "SaveGameService.h"
#include "UiTheme.h"
#include "PerlinNoise.hpp"

#include <cmath>
#include <algorithm>

using namespace ax;
USING_NS_AX_EXT;

namespace
{
/// @brief FOURCC-идентификатор render loop меню в ImGuiPresenter (начинается с '#').
constexpr std::string_view kMenuLoopId = "#menu";

/// @name Ключи пользовательских настроек (UserDefault) — общие с GameScene.
/// @{
constexpr const char* kKeyRenderDistance = "settings.renderDistance";
constexpr const char* kKeyMouseSens      = "settings.mouseSensitivity";
constexpr const char* kKeyPixelArtAA     = "settings.pixelArtAA";
/// @}
}  // namespace

// =========================================================================
//  Фабрика / Lifecycle
// =========================================================================
MainMenuScene* MainMenuScene::create()
{
    auto* scene = new (std::nothrow) MainMenuScene();
    if (scene && scene->init())
    {
        scene->autorelease();
        return scene;
    }
    AX_SAFE_DELETE(scene);
    return nullptr;
}

bool MainMenuScene::init()
{
    if (!Scene::init())
        return false;

    setupPreviewWorld();

    // Запускаем игровой цикл для облёта камеры и обновления чанков фона.
    scheduleUpdate();

    return true;
}

MainMenuScene::~MainMenuScene()
{
    // Полная остановка только при реальном уничтожении сцены.
    _chunkMgr.shutdown();
}

void MainMenuScene::onEnter()
{
    Scene::onEnter();

    // Глобальная (идемпотентная) инициализация ImGui + регистрация render loop меню.
    ui_theme::setup();
    ImGuiPresenter::getInstance()->addRenderLoop(kMenuLoopId, AX_CALLBACK_0(MainMenuScene::onDrawMenu, this), this);

    _chunkMgr.resume();
}

void MainMenuScene::onExit()
{
    // Снимаем render loop, иначе ImGuiPresenter будет звать метод уничтоженной сцены.
    ImGuiPresenter::getInstance()->removeRenderLoop(kMenuLoopId);

    _chunkMgr.pause();
    Scene::onExit();
}

// =========================================================================
//  3D-превью мира
// =========================================================================
void MainMenuScene::setupPreviewWorld()
{
    auto glView  = Director::getInstance()->getGLView();
    float w      = glView->getFrameSize().width;
    float h      = glView->getFrameSize().height;
    float aspect = (h > 0.0f) ? (w / h) : 16.0f / 9.0f;

    // Перспективная камера с флагом USER1 рендерит только 3D-ноды и идёт ПЕРЕД 2D
    // (depth = -1), чтобы меню ImGui рисовалось поверх мира.
    _orbitCamera = Camera::createPerspective(55.0f, aspect, 0.1f, 1000.0f);
    AX_ASSERT(_orbitCamera && "Failed to create orbit camera");
    _orbitCamera->setCameraFlag(CameraFlag::USER1);
    _orbitCamera->setDepth(-1);
    _orbitCamera->setPosition3D({_worldCenter.x + std::cos(_orbitAngle) * _orbitRadius,
                                   _orbitHeight,
                                   _worldCenter.z + std::sin(_orbitAngle) * _orbitRadius});
    _orbitCamera->lookAt(_worldCenter, Vec3::UNIT_Y);
    addChild(_orbitCamera);

    // Небо для глубины и красоты превью.
    auto textureCube = TextureCube::create("envmap_miramar/miramar_lf.tga", "envmap_miramar/miramar_rt.tga",
                                           "envmap_miramar/miramar_up.tga", "envmap_miramar/miramar_dn.tga",
                                           "envmap_miramar/miramar_ft.tga", "envmap_miramar/miramar_bk.tga");
    if (textureCube)
    {
        auto skyBox = Skybox::create();
        skyBox->setTexture(textureCube);
        skyBox->setCameraMask(static_cast<unsigned short>(CameraFlag::USER1));
        addChild(skyBox);
    }

    setupChunkManager();
}

void MainMenuScene::setupChunkManager()
{
    // Конфигурация: увеличенный preview для впечатляющего фона меню.
    ChunkManager::Config cfg;
    cfg.renderDistance         = 10;  // x2 от предыдущего 5 → диаметр 21 чанк
    cfg.workerThreadCount      = 2;
    cfg.maxGenerationsPerFrame = 4;
    cfg.unloadMargin           = 1;
    cfg.textureFilter          = TextureFilterMode::NEAREST_MIPMAP_LINEAR;
    // Antialiased-pixel-art шейдер: атлас грузится с LINEAR_MIPMAP_LINEAR,
    // textureFilter выше остаётся фолбэком, если выключить pixelArtAA.
    // Значение берём из настроек (общее с игрой), по умолчанию включено.
    cfg.pixelArtAA             = UserDefault::getInstance()->getBoolForKey(kKeyPixelArtAA, true);

    cfg.biomes.frequency = {0.40f, 0.20f, 0.40f, 0.0f, 0.0f};
    cfg.biomes.macroCellSize = 64;
    cfg.biomes.blendWidth = 16;

    _chunkMgr.init(cfg);

    // Генерация в фоновом потоке: полный перлиновый террейн (как в GameScene).
    _chunkMgr.setOnGenerate([](ChunkData& chunk) {
        constexpr int SEA_LEVEL = 38;

        const ChunkKey& key = chunk.getKey();
        auto basePos        = ChunkManager::chunkToWorld(key);

        thread_local siv::PerlinNoise perlin(42);
        thread_local siv::PerlinNoise rockNoise(137);
        thread_local siv::PerlinNoise dirtNoise(271);

        for (int x = 0; x < CHUNK_SIZE_X; ++x)
            for (int z = 0; z < CHUNK_SIZE_Z; ++z)
            {
                float wx      = basePos.x + x;
                float wz      = basePos.z + z;
                float terrain = perlin.octave2D_01(wx * 0.02, wz * 0.02, 6) * 40.0f;
                float biome   = perlin.octave2D_01(wx * 0.005, wz * 0.005, 4) * 15.0f - 5.0f;
                int surfaceY  = std::clamp(static_cast<int>(terrain + biome + 30), 1, CHUNK_SIZE_Y - 2);

                float rockVal = rockNoise.octave2D_01(wx * 0.08, wz * 0.08, 3);
                float dirtVal = dirtNoise.octave2D_01(wx * 0.06, wz * 0.06, 2);

                for (int y = 0; y <= surfaceY; ++y)
                {
                    BlockId block;
                    if (y == surfaceY)
                    {
                        if (rockVal > 0.72f)
                            block = BLOCK_STONE;
                        else if (surfaceY < SEA_LEVEL)
                            block = BLOCK_DIRT;
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

                if (surfaceY < SEA_LEVEL)
                {
                    for (int y = surfaceY + 1; y <= SEA_LEVEL; ++y)
                        chunk.setBlock(x, y, z, BLOCK_WATER);
                }
            }
    });

    // Визуализация в главном потоке: маска USER1, добавление в граф сцены.
    // Водный нод помечен WATER_NODE_TAG в buildWaterVisualNode — различаем по тегу.
    _chunkMgr.setOnVisualize([this](Node* node, const ChunkKey&) {
        if (!node)
            return;
        node->setCameraMask(static_cast<unsigned short>(CameraFlag::USER1));
        addChild(node);
        if (node->getTag() != WATER_NODE_TAG)
            _terrainNodes.push_back(node);  // террейн: покадровый u_time для дрожания желе
    });

    // Выгрузка в главном потоке: безопасное удаление нода.
    _chunkMgr.setOnUnload([this](Node* node, const ChunkKey&) {
        if (node)
            node->removeFromParentAndCleanup(true);
        _terrainNodes.erase(std::remove(_terrainNodes.begin(), _terrainNodes.end(), node), _terrainNodes.end());
    });

    // === Колбеки водных нодов для анимации ===
    _chunkMgr.setOnWaterNodeCreated([this](Node* node) {
        _waterNodes.push_back(node);
    });

    _chunkMgr.setOnWaterNodeDestroyed([this](Node* node) {
        _waterNodes.erase(std::remove(_waterNodes.begin(), _waterNodes.end(), node), _waterNodes.end());
    });

    // Чанки начинают грузиться немедленно вокруг центра превью.
    _chunkMgr.forceUpdate();
}

// =========================================================================
//  Игровой цикл — облёт камеры + обновление чанков
// =========================================================================
void MainMenuScene::update(float dt)
{
    // Параметрическое движение по окружности: x = cx + R·cos(θ), z = cz + R·sin(θ).
    // Высота фиксирована → стабильный вид «с птичьего полёта».
    _orbitAngle += _orbitSpeed * dt;
    const float camX = _worldCenter.x + std::cos(_orbitAngle) * _orbitRadius;
    const float camZ = _worldCenter.z + std::sin(_orbitAngle) * _orbitRadius;

    if (_orbitCamera)
    {
        _orbitCamera->setPosition3D({camX, _orbitHeight, camZ});
        _orbitCamera->lookAt(_worldCenter, Vec3::UNIT_Y);
    }

    // === Анимация воды и желе ===
    _animTime += dt;

    // Базовая opacity воды (надводный режим, как в GameScene)
    uint8_t waterOpacity = static_cast<uint8_t>(165u + std::clamp(
        static_cast<int>(std::sin(_animTime * 2.0f) * 12.0f), 0, 35));

    const ax::Vec3 camWorld = _orbitCamera ? _orbitCamera->getPosition3D() : ax::Vec3::ZERO;

    for (auto* node : _waterNodes)
    {
        if (!node)
            continue;

        // Явно выставляем opacity каждый кадр — без этого u_color.a
        // в шейдере может быть нулевым, и вода становится прозрачной.
        node->setOpacity(waterOpacity);

        auto* mr = static_cast<ax::MeshRenderer*>(node);
        if (auto* ps = mr->getProgramState())
        {
            auto locTime = ps->getUniformLocation("u_time");
            auto locCam  = ps->getUniformLocation("u_camWorld");
            ps->setUniform(locTime, &_animTime, sizeof(float));
            ps->setUniform(locCam, &camWorld, sizeof(ax::Vec3));

            // Расширяем fogEnd (по умолчанию = renderDist*CHUNK_SIZE = 160),
            // чтобы вода не исчезала в пределах видимой области превью.
            // Камера на орбите R=78, мир расходится на 160 блоков от центра →
            // макс. дистанция до дальней воды ≈ 240. Берём с запасом.
            float fogEnd   = 400.0f;
            float fogStart = fogEnd * 0.55f;
            auto locFogSt  = ps->getUniformLocation("u_fogStart");
            auto locFogEnd = ps->getUniformLocation("u_fogEnd");
            ps->setUniform(locFogSt, &fogStart, sizeof(float));
            ps->setUniform(locFogEnd, &fogEnd, sizeof(float));
        }
    }

    // === Дрожание желе: те же часы анимации уходят в шейдер террейна ===
    for (auto* node : _terrainNodes)
    {
        if (!node)
            continue;
        auto* mr = static_cast<ax::MeshRenderer*>(node);
        if (auto* ps = mr->getProgramState())
        {
            // У шейдеров без u_time (terrain_vs, встроенный UNLIT) — пропускаем.
            auto locTime = ps->getUniformLocation("u_time");
            if (locTime)
                ps->setUniform(locTime, &_animTime, sizeof(float));
        }
    }

    // Фиксированный центр: ChunkManager держит загруженной область вокруг превью.
    _chunkMgr.update(_worldCenter);
}

// =========================================================================
//  ImGui — главное меню
// =========================================================================
void MainMenuScene::onDrawMenu()
{
    if (_showSettings)
    {
        onDrawSettings();
        return;
    }

    const ImVec2 screen = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(380.0f, 0.0f), ImGuiCond_Always);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##mainmenu", nullptr, flags);

    // Заголовок крупным кеглем (динамический размер шрифта ImGui 1.92).
    ImGui::PushFont(nullptr, 46.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui_theme::kAccentHover);
    ui_theme::textCentered("CATHODE SHIFT");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0.0f, 22.0f));

    ImGui::PushFont(nullptr, 22.0f);
    if (ui_theme::menuButton("Новая игра"))
        SceneManager::startNewGame();

    // «Продолжить» доступно только при наличии сохранения.
    const bool hasSave = SaveGameService::hasSave();
    ImGui::BeginDisabled(!hasSave);
    if (ui_theme::menuButton("Продолжить"))
        SceneManager::loadGame();
    ImGui::EndDisabled();

    if (ui_theme::menuButton("Настройки"))
        _showSettings = true;

    if (ui_theme::menuButton("Выход"))
        SceneManager::exitGame();
    ImGui::PopFont();

    ImGui::End();
}

// =========================================================================
//  ImGui — настройки
// =========================================================================
void MainMenuScene::onDrawSettings()
{
    auto* ud = UserDefault::getInstance();

    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Always);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##settings", nullptr, flags);

    ImGui::PushFont(nullptr, 34.0f);
    ui_theme::textCentered("Настройки");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 16.0f));

    // Дальность прорисовки (в чанках) — читается GameScene при старте новой игры.
    int renderDistance = ud->getIntegerForKey(kKeyRenderDistance, 10);
    if (ImGui::SliderInt("Дальность прорисовки", &renderDistance, 4, 32))
        ud->setIntegerForKey(kKeyRenderDistance, std::clamp(renderDistance, 4, 32));

    // Чувствительность мыши (градусы/пиксель).
    float mouseSens = ud->getFloatForKey(kKeyMouseSens, 0.1f);
    if (ImGui::SliderFloat("Чувствительность мыши", &mouseSens, 0.02f, 0.40f, "%.3f"))
        ud->setFloatForKey(kKeyMouseSens, std::clamp(mouseSens, 0.02f, 0.40f));

    // Сглаживание пикселей (pixel-art AA) — применяется к превью сразу.
    bool pixelAA = ud->getBoolForKey(kKeyPixelArtAA, true);
    if (ImGui::Checkbox("Сглаживание пикселей (AA)", &pixelAA))
    {
        ud->setBoolForKey(kKeyPixelArtAA, pixelAA);
        _chunkMgr.setPixelArtAA(pixelAA);  // живое применение к фону меню
    }

    ImGui::Dummy(ImVec2(0.0f, 18.0f));

    ImGui::PushFont(nullptr, 22.0f);
    if (ui_theme::menuButton("Назад"))
    {
        ud->flush();  // фиксируем изменения на диск
        _showSettings = false;
    }
    ImGui::PopFont();

    ImGui::End();
}
