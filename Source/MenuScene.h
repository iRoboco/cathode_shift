#pragma once
#include "axmol.h"
#include "ChunkManager.h"

/**
 * @file MenuScene.h
 * @brief Главное меню с живым 3D-превью мира на фоне.
 * 
 * Архитектурные особенности:
 * - Переиспользует ChunkManager для асинхронной генерации и отрисовки чанков в реальном времени.
 * - Камера совершает плавное орбитальное движение вокруг центра мира (bird's-eye view).
 * - UI-меню наложено поверх 3D-сцены с использованием global Z-order для гарантированного перекрытия.
 * - Корректно обрабатывает жизненный цикл: pause/resume при сворачивании, shutdown при уничтожении.
 */
class MenuScene : public ax::Scene
{
public:
    static ax::Scene* create();
    bool init() override;
    ~MenuScene() override;

private:
    void update(float dt) override;
    void onEnter() override;
    void onExit() override;

    // === 3D Мир и Менеджер Чанков ===
    ChunkManager _chunkMgr;
    ax::Vec3 _worldCenter{0.0f, 0.0f, 0.0f}; ///< Точка привязки генерации чанков (центр превью)

    // === Камера и Орбита ===
    ax::Camera* _orbitCamera = nullptr;
    float _orbitAngle = 0.0f;           ///< Текущий угол вращения камеры (радианы)
    float _orbitRadius = 100.0f;         ///< Горизонтальный радиус орбиты камеры от центра
    float _orbitHeight = 80.0f;         ///< Фиксированная высота камеры (птичий полёт)
    float _orbitSpeed = 0.12f;          ///< Угловая скорость вращения (рад/сек)

    // === UI ===
    ax::Menu* _mainMenu = nullptr;

    // === Внутренние методы инициализации ===
    void setupChunkManager();
    void setupUI();
};

class MenuClass
{};
