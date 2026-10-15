#pragma once
#include "axmol.h"
#include "ChunkManager.h"
#include "FirstPersonController.h"
#include "VoxelRaycaster.h"

class PauseOverlay;

/**
@brief Основная игровая сцена (Gameplay Scene)
Архитектурные принципы (Steam-ready):
- Инкапсуляция всех игровых систем внутри Scene.
- Безопасный lifecycle: init -> update -> pause/resume -> destructor.
- Разделение графа сцены: камера, игрок, чанки, будущий HUD/UI.
- Потокобезопасная интеграция через коллбеки и lazy-инициализацию.
*/
class GameScene : public ax::Scene
{
public:
    static ax::Scene* create();

    /// @brief Создаёт игровую сцену и применяет к ней данные из файла сохранения.
    /// @return Готовая сцена (если сейв отсутствует/повреждён — мир со спавном по умолчанию).
    static GameScene* createFromSave();

    bool init() override;

    // Деструктор для корректного shutdown() ChunkManager
    // Вызывается только при полном уничтожении сцены (replaceScene, quit),
    // но НЕ при сворачивании приложения.
    ~GameScene() override;

    // Установка внешней ссылки на нод игрока (для HUD, инвентаря, сетевых систем)
    void setPlayerNode(ax::Node* player) { _playerNode = player; }
    ax::Vec3 getPlayerPosition() const;

    /// @brief Телепортирует игрока в указанную позицию (используется загрузкой сохранения).
    void setPlayerPosition(const ax::Vec3& pos);

    // === Пауза (оверлей поверх живой сцены) ===
    void pauseGame();                       ///< Показать меню паузы, заморозить ввод игрока
    void resumeGame();                      ///< Скрыть меню паузы, вернуть управление
    bool isPaused() const { return _paused; }

    /// @brief Переключает pixel-art AA шейдер вживую и сохраняет настройку (вызывается из паузы).
    void setPixelArtAA(bool enabled);

protected:
    // Axmol вызывает эти методы при сворачивании/разворачивании окна
    void onEnter() override;
    void onExit() override;

private:
    void update(float dt) override;
    // Обработчик клавиатуры для перезапуска уровня
    void onKeyPressed(ax::EventKeyboard::KeyCode keyCode, ax::Event* event);

    // Ядро генерации мира
    ChunkManager _chunkMgr;
    // Управление камерой и физикой
    FirstPersonController* _playerController = nullptr;
    ax::Camera* _mainCamera                  = nullptr;
    // Абстрактная ссылка для внешних систем (UI, звук, сеть)
    ax::Node* _playerNode          = nullptr;
    VoxelRaycasterNode* _raycaster = nullptr;
    float _animTime                  = 0.0f;  ///< Общие часы анимации (волны воды, дрожание желе)
    std::vector<ax::Node*> _waterNodes;     ///< Кэш активных водных нодов для анимации
    std::vector<ax::Node*> _terrainNodes;   ///< Кэш нодов террейна (дрожание желе: u_time в voronoi_terrain)
    bool _cameraUnderwater = false;         ///< Флаг: камера погружена в воду

    /// @brief Полноэкранный 2D-оверлей для имитации подводного освещения.
    /// Рендерится поверх 3D-сцены default-камерой (depth=0 > 3D-камеры depth=-1).
    /// setColor/setOpacity на LayerColor безопасны — это чистый 2D DrawNode без Z-буфера.
    ax::LayerColor* _underwaterOverlay = nullptr;

    /// @brief Плавный коэффициент перехода [0.0=над водой, 1.0=под водой]
    float _underwaterBlend = 0.0f;

    // === Состояние паузы ===
    bool _paused                = false;    ///< На паузе ли игра (ввод заморожен, меню видимо)
    PauseOverlay* _pauseOverlay = nullptr;  ///< Активный оверлей паузы (nullptr вне паузы)
};
