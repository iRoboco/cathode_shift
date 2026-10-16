#pragma once
#include "axmol.h"
#include "ChunkManager.h"
#include "FirstPersonController.h"
#include "VoxelRaycaster.h"
#include "WorldConfig.h"
#include <memory>
#include <string>

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

    /// @brief Инициализация с явным выбором мира (id из Content/config/worlds.json).
    /// Пустая строка — выбор по умолчанию (UserDefault → activeWorld из JSON).
    bool initWithWorldId(const std::string& worldId);

    // Деструктор для корректного shutdown() ChunkManager
    // Вызывается только при полном уничтожении сцены (replaceScene, quit),
    // но НЕ при сворачивании приложения.
    ~GameScene() override;

    // Установка внешней ссылки на нод игрока (для HUD, инвентаря, сетевых систем)
    void setPlayerNode(ax::Node* player) { _playerNode = player; }
    ax::Vec3 getPlayerPosition() const;

    /// @brief Телепортирует игрока в указанную позицию (используется загрузкой сохранения).
    void setPlayerPosition(const ax::Vec3& pos);

    // === Система миров (Content/config/worlds.json) ===
    /// @brief Переключает активный мир: запоминает выбор и пересоздаёт сцену
    /// (полный teardown чанк-менеджера через деструктор, затем подъём нового мира
    /// с его сидом, профилем террейна и регионами). @return false если id неизвестен.
    bool switchWorld(const std::string& worldId);
    /// @brief id активного мира (пишется в файл сохранения).
    const std::string& getWorldId() const { return _worldId; }
    /// @brief Каталог всех миров из конфига (для UI выбора мира).
    const worldcfg::WorldCatalog& getWorldCatalog() const { return _worldCatalog; }
    /// @brief Активный мир (принадлежит каталогу / фолбэку сцены).
    const worldcfg::WorldDef* getActiveWorld() const { return _world; }

    // === Пауза (оверлей поверх живой сцены) ===
    void pauseGame();                       ///< Показать меню паузы, заморозить ввод игрока
    void resumeGame();                      ///< Скрыть меню паузы, вернуть управление
    bool isPaused() const { return _paused; }

    /// @brief Переключает pixel-art AA шейдер вживую и сохраняет настройку (вызывается из паузы).
    void setPixelArtAA(bool enabled);

    /// @brief Вкл/выкл пикселизацию тайлов («занижение» разрешения атласа) вживую + сохранение
    /// (клавиша P, чекбокс в меню паузы). Uniform-only — меши не перестраиваются.
    void setTilePixelate(bool enabled);

    /// @brief Задаёт «разрешение» тайла при пикселизации (ячеек на сторону) вживую + сохранение.
    void setTilePixelRes(float px);

protected:
    // Axmol вызывает эти методы при сворачивании/разворачивании окна
    void onEnter() override;
    void onExit() override;

private:
    void update(float dt) override;
    // Обработчик клавиатуры для перезапуска уровня
    void onKeyPressed(ax::EventKeyboard::KeyCode keyCode, ax::Event* event);

    // === Система миров ===
    // ВАЖНО: порядок объявления имеет значение. _chunkMgr объявлен ПОСЛЕ каталога,
    // значит разрушается ПЕРВЫМ — его shutdown() в деструкторе гарантированно
    // останавливает воркеры до уничтожения _worldCatalog, на WorldDef которого
    // ссылается лямбда генерации чанков.
    worldcfg::WorldCatalog _worldCatalog;              ///< Каталог миров из Content/config/worlds.json
    std::unique_ptr<worldcfg::WorldDef> _fallbackWorld;  ///< Аварийный мир, если конфиг не загрузился
    const worldcfg::WorldDef* _world = nullptr;        ///< Активный мир (не владеем)
    std::string _worldId;                              ///< id активного мира (для сейва)

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
