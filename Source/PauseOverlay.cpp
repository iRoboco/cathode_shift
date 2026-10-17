#include "PauseOverlay.h"
#include "GameScene.h"
#include "SceneManager.h"
#include "SaveGameService.h"
#include "UiTheme.h"

using namespace ax;
USING_NS_AX_EXT;

namespace
{
/// @brief FOURCC-идентификатор render loop меню паузы.
constexpr std::string_view kPauseLoopId = "#paus";

/// @brief Ключ настройки pixel-art AA (общий с GameScene/MainMenuScene).
constexpr const char* kKeyPixelArtAA = "settings.pixelArtAA";

/// @brief Ключи настройки пикселизации тайлов (общие с GameScene/MainMenuScene).
constexpr const char* kKeyTilePixelate = "settings.tilePixelate";
constexpr const char* kKeyTilePixelRes = "settings.tilePixelRes";
}  // namespace

PauseOverlay* PauseOverlay::create(GameScene* owner)
{
    auto* ret = new (std::nothrow) PauseOverlay();
    if (ret && ret->initWithOwner(owner))
    {
        ret->autorelease();
        return ret;
    }
    AX_SAFE_DELETE(ret);
    return nullptr;
}

bool PauseOverlay::initWithOwner(GameScene* owner)
{
    // Затемняющая подложка на весь экран (чистый 2D, без влияния на 3D depth).
    if (!LayerColor::initWithColor(Color4B(0, 0, 0, 140)))
        return false;

    _owner = owner;

    // Один раз читаем текущие значения (дальше — кэш, см. PauseOverlay.h).
    {
        auto* ud  = UserDefault::getInstance();
        _aa       = ud->getBoolForKey(kKeyPixelArtAA, true);
        _pixelate = ud->getBoolForKey(kKeyTilePixelate, false);
        _pixelRes = static_cast<int>(ud->getFloatForKey(kKeyTilePixelRes, 32.0f));
    }

    // ImGui уже настроен (GameScene::onEnter вызывает ui_theme::setup()),
    // но вызов идемпотентен — на случай прямого входа в паузу подстрахуемся.
    ui_theme::setup();

    // Меню паузы рисуем как render loop на сцене-владельце (target = owner),
    // чтобы трекер событий ImGui был привязан к актуальной игровой сцене.
    ImGuiPresenter::getInstance()->addRenderLoop(kPauseLoopId, AX_CALLBACK_0(PauseOverlay::onDrawPauseMenu, this),
                                                 _owner);
    _loopRegistered = true;

    return true;
}

void PauseOverlay::onExit()
{
    if (_loopRegistered)
    {
        ImGuiPresenter::getInstance()->removeRenderLoop(kPauseLoopId);
        _loopRegistered = false;
    }
    LayerColor::onExit();
}

void PauseOverlay::onDrawPauseMenu()
{
    const ImVec2 screen = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_Always);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##pause", nullptr, flags);

    ImGui::PushFont(nullptr, 38.0f);
    ui_theme::textCentered("Пауза");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 20.0f));

    // --- Графика: переключатель pixel-art AA (применяется вживую) ---
    if (ImGui::Checkbox("Сглаживание пикселей (AA)", &_aa) && _owner)
        _owner->setPixelArtAA(_aa);  // живое применение + сохранение

    // --- Графика: пикселизация тайлов (ретро-режим), применяется вживую ---
    // Uniform-only: переключение мгновенное, без ребилда чанков. В игре та же
    // функция на клавише P.
    if (ImGui::Checkbox("Пикселизация тайлов (ретро)", &_pixelate) && _owner)
        _owner->setTilePixelate(_pixelate);  // живое применение + сохранение
    if (_pixelate)
    {
        if (ImGui::SliderInt("Разрешение тайлов", &_pixelRes, 8, 64) && _owner)
            _owner->setTilePixelRes(static_cast<float>(_pixelRes));  // вживую, без записи на диск
        // Запись настроек на диск — один раз при отпускании слайдера, а не каждый кадр drag.
        if (ImGui::IsItemDeactivatedAfterEdit())
            UserDefault::getInstance()->flush();
    }
    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    ImGui::PushFont(nullptr, 22.0f);
    if (ui_theme::menuButton("Продолжить") && _owner)
        _owner->resumeGame();

    if (ui_theme::menuButton("Сохранить") && _owner)
        SaveGameService::saveGame(_owner);

    if (ui_theme::menuButton("Сохранить и выйти") && _owner)
    {
        SaveGameService::saveGame(_owner);
        SceneManager::goToMainMenu();
    }

    if (ui_theme::menuButton("В меню (без сохранения)"))
        SceneManager::goToMainMenu();
    ImGui::PopFont();

    ImGui::End();
}
