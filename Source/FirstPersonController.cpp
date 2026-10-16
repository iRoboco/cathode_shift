#include "FirstPersonController.h"
#include "ChunkManager.h"
#include "VoxelRaycaster.h"
#include <algorithm>
#include <cmath>

#if defined(GLFW_VERSION_MAJOR)
#    include <GLFW/glfw3.h>
#endif

USING_NS_AX;

// =========================================================================
//  Конструктор / Деструктор
// =========================================================================
FirstPersonController::FirstPersonController() = default;

FirstPersonController::~FirstPersonController()
{
    // Явное снятие listener'ов в деструкторе — защита от
    // dangling callback, если onExit не был вызван
    if (_eventDispatcher)
    {
        if (_keyboardListener)
            _eventDispatcher->removeEventListener(_keyboardListener);
        if (_mouseListener)
            _eventDispatcher->removeEventListener(_mouseListener);
        _keyboardListener = nullptr;
        _mouseListener    = nullptr;
    }
}

// =========================================================================
//  Фабрика / Инициализация
// =========================================================================
FirstPersonController* FirstPersonController::create(ax::Camera* camera, float moveSpeed, float mouseSensitivity)
{
    auto* ret = new (std::nothrow) FirstPersonController();
    if (ret && ret->init(camera, moveSpeed, mouseSensitivity))
    {
        ret->autorelease();
        return ret;
    }
    AX_SAFE_DELETE(ret);
    return nullptr;
}

bool FirstPersonController::init(ax::Camera* camera, float moveSpeed, float mouseSensitivity)
{
    if (!ax::Node::init())
        return false;
    if (!camera)
        return false;

    _camera           = camera;
    _baseMoveSpeed    = moveSpeed;
    _moveSpeed        = moveSpeed;
    _mouseSensitivity = mouseSensitivity;

    _capsule.bottomPos = ax::Vec3::ZERO;
    _capsule.radius    = 0.25f;
    _capsule.height    = 1.6f;

    setupEventListeners();
    setFreeFlightMode(false);
    scheduleUpdate();
    return true;
}

void FirstPersonController::setInitialPosition(const ax::Vec3& worldPos)
{
    _capsule.bottomPos = worldPos;
    if (_camera)
    {
        _camera->setPosition3D(worldPos + ax::Vec3(0, _capsule.height * 0.85f, 0));
    }
    Node::setPosition3D(worldPos);
}

// =========================================================================
//  Сеттеры физики
// =========================================================================

void FirstPersonController::setSpeedMultiplier(float multiplier)
{
    _moveSpeed = _baseMoveSpeed * std::max(0.0f, multiplier);
}

void FirstPersonController::setFrictionParams(float groundAccel, float groundDecel, float airAccel, float airDecel)
{
    _groundAccel = groundAccel;
    _groundDecel = groundDecel;
    _airAccel    = airAccel;
    _airDecel    = airDecel;
}

void FirstPersonController::resetVelocity()
{
    _currentVelocity = ax::Vec3::ZERO;
    _targetVelocity  = ax::Vec3::ZERO;
    _stepUpVelocity  = 0.0f;
    _isSteppingUp    = false;
}

// =========================================================================
//  Управление режимами
// =========================================================================
void FirstPersonController::setEnabled(bool enabled)
{
    _enabled = enabled;
    if (!_enabled)
    {
        _keyW = _keyA = _keyS = _keyD = _keySpace = _isLeftMousePressed = false;
        resetVelocity();
        _collisionResolver.reset();
    }
}

void FirstPersonController::setMouseCaptured(bool captured)
{
    auto glView = ax::Director::getInstance()->getGLView();
    if (!glView)
        return;

    // В свободном полёте курсор всегда видим — захват по запросу игнорируем.
    if (!captured || _freeFlightMode)
    {
        glView->setCursorVisible(true);
        _isLeftMousePressed = false;
#if defined(GLFW_VERSION_MAJOR)
        auto* glViewImpl = dynamic_cast<ax::RenderViewImpl*>(glView);
        if (glViewImpl && glViewImpl->getWindow())
            glfwSetInputMode(glViewImpl->getWindow(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
#endif
        return;
    }

    glView->setCursorVisible(false);
// Захват мыши в режиме FPC - курсор фиксируется в центре окна
#if defined(GLFW_VERSION_MAJOR)
    auto* glViewImpl = dynamic_cast<ax::RenderViewImpl*>(glView);
    if (glViewImpl)
    {
        auto window = glViewImpl->getWindow();
        if (window)
        {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            // Получаем текущую позицию курсора из GLFW для корректной инициализации
            // Это предотвращает скачок камеры при первом движении мыши после захвата
            double xpos = 0.0, ypos = 0.0;
            glfwGetCursorPos(window, &xpos, &ypos);
            _firstMouseMove = true;
            _lastMousePos.set(static_cast<float>(xpos), static_cast<float>(ypos));
        }
    }
#endif
}

void FirstPersonController::setFreeFlightMode(bool enabled)
{
    _freeFlightMode = enabled;
    // В свободном полёте курсор свободен; в FPS — захвачен.
    setMouseCaptured(!enabled);
}

void FirstPersonController::toggleFlightMode()
{
    setFreeFlightMode(!_freeFlightMode);
}

// =========================================================================
//   onExit
// =========================================================================
void FirstPersonController::onExit()
{
    // Keyboard listener зарегистрирован с Fixed Priority (не привязан к scene graph).
    // При удалении ноды из сцены, Axmol НЕ снимает его автоматически.
    // Безопасное снятие Fixed-priority listener
    if (_eventDispatcher)
    {
        if (_keyboardListener)
        {
            _eventDispatcher->removeEventListener(_keyboardListener);
            _keyboardListener = nullptr;
        }
        if (_mouseListener)
        {
            _eventDispatcher->removeEventListener(_mouseListener);
            _mouseListener = nullptr;
        }
    }
    ax::Node::onExit();
}

// =========================================================================
//  Векторы направлений камеры
// =========================================================================
ax::Vec3 FirstPersonController::getForwardVector() const
{
    if (!_camera)
        return ax::Vec3(0, 0, -1);
    float yaw = AX_DEGREES_TO_RADIANS(_camera->getRotation3D().y);
    ax::Vec3 fwd(-std::sinf(yaw), 0.0f, -std::cosf(yaw));

    if (_freeFlightMode)
    {
        float pitch = AX_DEGREES_TO_RADIANS(_camera->getRotation3D().x);
        fwd.y       = std::sinf(pitch);
        fwd.x *= std::cosf(pitch);
        fwd.z *= std::cosf(pitch);
    }
    fwd.normalize();
    return fwd;
}

ax::Vec3 FirstPersonController::getRightVector() const
{
    if (!_camera)
        return ax::Vec3(1, 0, 0);
    float yaw = AX_DEGREES_TO_RADIANS(_camera->getRotation3D().y);
    return ax::Vec3(std::cosf(yaw), 0.0f, -std::sinf(yaw)).getNormalized();
}

// =========================================================================
//  update
// =========================================================================
void FirstPersonController::update(float dt)
{
    if (!_enabled || !_camera || dt <= 0.0f)
        return;

    if (!_collisionResolver && _chunkMgr)
    {
        _collisionResolver = std::make_unique<VoxelCollisionResolver>(_chunkMgr);
    }

    // === РЕЖИМ СВОБОДНОГО ПОЛЁТА (FreeFlight / Noclip) ===
    if (_freeFlightMode)
    {
        /**
        @brief Множитель скорости для режима полёта.
        @details Вынесен в constexpr для удобной тонкой настройки без хардкода в логике.
                 Значение 2.5f делает полёт в 2.5 раза быстрее базового бега,
                 что соответствует стандартам воксельных игр (Minecraft, Valheim и т.п.).
                 При необходимости меняется в одном месте, не затрагивая FPS-физику.
        */
        constexpr float FLIGHT_SPEED_MULTIPLIER = 2.5f;

        // Эффективная скорость = базовая × множитель полёта
        float flightSpeed = _moveSpeed * FLIGHT_SPEED_MULTIPLIER;

        ax::Vec3 pos = _camera->getPosition3D();
        ax::Vec3 fwd = getForwardVector();
        ax::Vec3 rgt = getRightVector();

        // Применяем flightSpeed вместо _moveSpeed для всех осей перемещения
        if (_keyW)
            pos += fwd * flightSpeed * dt;
        if (_keyS)
            pos -= fwd * flightSpeed * dt;
        if (_keyD)
            pos += rgt * flightSpeed * dt;
        if (_keyA)
            pos -= rgt * flightSpeed * dt;

        _camera->setPosition3D(pos);
        // Синхронизация низа капсулы с камерой (высота глаз ≈ 85% высоты капсулы)
        _capsule.bottomPos = pos - ax::Vec3(0, _capsule.height * 0.85f, 0);
        this->setPosition3D(_capsule.bottomPos);
        return;
    }

    // === РЕЖИМ FPS ===

    // Защита от провала сквозь мир при старте.
    // Проблема: первые N кадров чанки ещё генерируются в фоновых потоках,
    // getBlockAtWorldPos() возвращает BLOCK_AIR для любой позиции,
    // коллизий нет, и игрок падает в бесконечность.
    // Решение: если чанк под игроком не загружен — замораживаем Y-скорость
    // и пропускаем гравитацию/коллизии. Камера обновляется, горизонтальное
    // движение обрабатывается как обычно (по freeFlight-логике).
    if (_chunkMgr)
    {
        auto playerChunk = ChunkManager::worldToChunk(_capsule.bottomPos);
        if (!_chunkMgr->isChunkActive(playerChunk))
        {
            // Чанк под игроком ещё не загружен — замораживаем вертикальную скорость
            _currentVelocity.y = 0.0f;
            _isGrounded        = true;

            // Горизонтальное движение (без коллизий, но это временно)
            ax::Vec3 inputDir(0, 0, 0);
            if (_keyW)
                inputDir += getForwardVector();
            if (_keyS)
                inputDir -= getForwardVector();
            if (_keyD)
                inputDir += getRightVector();
            if (_keyA)
                inputDir -= getRightVector();
            if (inputDir.lengthSquared() > 0.0001f)
            {
                inputDir.normalize();
                _capsule.bottomPos += inputDir * _moveSpeed * dt;
            }

            _camera->setPosition3D(_capsule.bottomPos + ax::Vec3(0, _capsule.height * 0.85f, 0));
            this->setPosition3D(_capsule.bottomPos);
            return;
        }
    }

    ax::Vec3 inputDir(0, 0, 0);
    if (_keyW)
        inputDir += getForwardVector();
    if (_keyS)
        inputDir -= getForwardVector();
    if (_keyD)
        inputDir += getRightVector();
    if (_keyA)
        inputDir -= getRightVector();

    bool isInputActive = inputDir.lengthSquared() > 0.0001f;
    if (isInputActive)
    {
        inputDir.normalize();
    }
    else
    {
        inputDir = ax::Vec3::ZERO;
    }

    _targetVelocity.x = inputDir.x * _moveSpeed;
    _targetVelocity.z = inputDir.z * _moveSpeed;

    float rate = isInputActive ? (_isGrounded ? _groundAccel : _airAccel) : (_isGrounded ? _groundDecel : _airDecel);

    float lerpFactor = std::min(rate * dt, 1.0f);

    _currentVelocity.x = std::lerp(_currentVelocity.x, _targetVelocity.x, lerpFactor);
    _currentVelocity.z = std::lerp(_currentVelocity.z, _targetVelocity.z, lerpFactor);

    if (!isInputActive)
    {
        if (std::abs(_currentVelocity.x) < VELOCITY_EPSILON)
            _currentVelocity.x = 0.0f;
        if (std::abs(_currentVelocity.z) < VELOCITY_EPSILON)
            _currentVelocity.z = 0.0f;
    }

    _currentVelocity.y += _gravity * dt;

    // Подъём на ступеньку: пока он активен, гравитация не должна «съедать»
    // вертикальную скорость между кадрами. Иначе при низком FPS (dt ≥ 1/25)
    // приращение гравитации за кадр (|g|·dt ≥ 1.0) превышает минимальную
    // скорость подъёма, резолвер отклоняет step-up (условие velocity.y > 0),
    // и игрок залипает у края блока — слегка приподнимается, но не перелезает.
    if (_isSteppingUp)
    {
        _currentVelocity.y = _stepUpVelocity;
    }

    if (_isGrounded && _keySpace)
    {
        _currentVelocity.y = _jumpForce;
        _isGrounded        = false;
    }

    if (_collisionResolver)
    {
        auto result = _collisionResolver->resolve(dt, _currentVelocity, _capsule);
        _isGrounded = result.isGrounded;

        _camera->setPosition3D(result.resolvedPosition + ax::Vec3(0, _capsule.height * 0.85f, 0));
        this->setPosition3D(result.resolvedPosition);

        if (result.isGrounded && _currentVelocity.y < 0)
        {
            _currentVelocity.y = 0.0f;
        }

        if (result.hitCeiling && _currentVelocity.y > 0)
        {
            _currentVelocity.y = 0.0f;
        }

        // Плавный подъём на ступеньку («желе»): пока капсула упирается в препятствие
        // высотой ≤1 блока, резолвер каждый кадр запрашивает step-up. Скорость
        // пропорциональна оставшейся высоте: быстрый старт, мягкая посадка наверху —
        // без инерционного «докидывания» над ступенькой.
        // Скорость запоминается в _stepUpVelocity: подъём идёт несколько кадров,
        // и гравитация не должна обнулять её раньше времени (см. выше).
        if (result.stepUp)
        {
            _stepUpVelocity    = std::clamp(result.stepUpHeight * 20.0f, 1.0f, _stepUpSpeed);
            _currentVelocity.y = _stepUpVelocity;
            _isSteppingUp      = true;
        }
        else
        {
            _isSteppingUp = false;
        }

        // Защитный clamp — предотвращает провал ниже Y=0.
        // Если игрок по какой-либо причине оказался ниже нулевого уровня
        // (просадка FPS, туннелирование, ещё не загруженные чанки),
        // телепортируем на поверхность и сбрасываем скорость.
        if (_capsule.bottomPos.y < 0.0f)
        {
            _capsule.bottomPos.y = 0.0f;
            _currentVelocity.y   = 0.0f;
            _isGrounded          = true;
            _camera->setPosition3D(_capsule.bottomPos + ax::Vec3(0, _capsule.height * 0.85f, 0));
            this->setPosition3D(_capsule.bottomPos);
        }
    }
    else
    {
        ax::Vec3 newPos = _camera->getPosition3D() + _currentVelocity * dt;
        _camera->setPosition3D(newPos);
        _capsule.bottomPos = newPos - ax::Vec3(0, _capsule.height * 0.85f, 0);
        this->setPosition3D(_capsule.bottomPos);
    }

    processBlockInteraction(dt);
}

// =========================================================================
//  Event Listeners —  Mouse listener привязан к scene graph
// =========================================================================
void FirstPersonController::setupEventListeners()
{
    _mouseListener              = ax::EventListenerMouse::create();
    _mouseListener->onMouseMove = [this](auto* event) {
        this->onMouseMove(event);
        return true;
    };
    _mouseListener->onMouseDown = [this](auto* event) {
        auto* e = static_cast<ax::EventMouse*>(event);
        if (e->getMouseButton() == ax::EventMouse::MouseButton::BUTTON_LEFT && _freeFlightMode)
        {
            _isLeftMousePressed = true;
            _lastMousePos.set(e->getCursorX(), e->getCursorY());
        }
        if (!_freeFlightMode && _raycaster)
        {
            if (e->getMouseButton() == ax::EventMouse::MouseButton::BUTTON_LEFT)
            {
                _raycaster->setInteractionMode(BlockInteractionMode::BreakHold);
                _breakAccum = 0.0f;
                _raycaster->setBreakProgress(0.0f);
            }
            else if (e->getMouseButton() == ax::EventMouse::MouseButton::BUTTON_RIGHT)
            {
                _raycaster->setInteractionMode(BlockInteractionMode::PlaceIntent);
            }
        }
        return true;
    };
    _mouseListener->onMouseUp = [this](auto* event) {
        auto* e = static_cast<ax::EventMouse*>(event);
        if (e->getMouseButton() == ax::EventMouse::MouseButton::BUTTON_LEFT)
        {
            _isLeftMousePressed = false;
            if (!_freeFlightMode && _raycaster)
            {
                _raycaster->setInteractionMode(BlockInteractionMode::Look);
                _breakAccum = 0.0f;
                _raycaster->setBreakProgress(0.0f);
            }
        }
        else if (e->getMouseButton() == ax::EventMouse::MouseButton::BUTTON_RIGHT)
        {
            if (!_freeFlightMode && _raycaster)
            {
                _raycaster->placeBlock(_selectedBlockId);
                _raycaster->setInteractionMode(BlockInteractionMode::Look);
            }
        }
        return true;
    };
    _eventDispatcher->addEventListenerWithSceneGraphPriority(_mouseListener, this);

    _keyboardListener                = ax::EventListenerKeyboard::create();
    _keyboardListener->onKeyPressed  = [this](auto code, auto* event) { onKeyPressed(code, event); };
    _keyboardListener->onKeyReleased = [this](auto code, auto* event) { onKeyReleased(code, event); };
    _eventDispatcher->addEventListenerWithFixedPriority(_keyboardListener, 10);
}

void FirstPersonController::onMouseMove(ax::Event* event)
{
    if (!_enabled || !_camera)
        return;
    auto* e = static_cast<ax::EventMouse*>(event);

    // Vec2 delta(e->getDelta());
    // Вычисляем дельту вручную через текущие и предыдущие координаты курсора.
    float currentX = e->getCursorX();
    float currentY = e->getCursorY();

    // Первый ивент после захвата курсора содержит виртуальные координаты GLFW,
    // которые могут резко отличаться от прежней позиции — пропускаем его,
    // только запоминая точку отсчёта для следующего движения.
    if (_firstMouseMove)
    {
        _lastMousePos.set(currentX, currentY);
        _firstMouseMove = false;
        return;
    }

    float deltaX = currentX - _lastMousePos.x;
    float deltaY = currentY - _lastMousePos.y;

    // Обновляем последнюю позицию курсора
    _lastMousePos.set(currentX, currentY);

    Vec2 delta(deltaX, deltaY);

    if (_freeFlightMode && !_isLeftMousePressed)
        return;

    // Инвертируем управление для режима FPC
    // В режиме полета: движение мыши влево → камера вправо, вверх → вниз
    // В режиме FPC: движение мыши влево → камера влево, вверх → вверх
    float directionMultiplier = _freeFlightMode ? 1.0f : -1.0f;

    float yaw   = _camera->getRotation3D().y + delta.x * _mouseSensitivity * directionMultiplier;
    float pitch = _camera->getRotation3D().x - delta.y * _mouseSensitivity * directionMultiplier;

    yaw = std::fmod(yaw, 360.0f);
    if (yaw > 180.0f)
        yaw -= 360.0f;
    if (yaw < -180.0f)
        yaw += 360.0f;

    if (!_freeFlightMode)
    {
        pitch = std::clamp(pitch, -89.0f, 89.0f);
    }

    _camera->setRotation3D(ax::Vec3(pitch, yaw, 0.0f));
}

void FirstPersonController::onKeyPressed(ax::EventKeyboard::KeyCode code, ax::Event* event)
{
    if (!_enabled)
        return;
    switch (code)
    {
    case ax::EventKeyboard::KeyCode::KEY_W:
        _keyW = true;
        break;
    case ax::EventKeyboard::KeyCode::KEY_S:
        _keyS = true;
        break;
    case ax::EventKeyboard::KeyCode::KEY_A:
        _keyA = true;
        break;
    case ax::EventKeyboard::KeyCode::KEY_D:
        _keyD = true;
        break;
    case ax::EventKeyboard::KeyCode::KEY_SPACE:
        _keySpace = true;
        break;
    case ax::EventKeyboard::KeyCode::KEY_F5:
        toggleFlightMode();
        break;
    case ax::EventKeyboard::KeyCode::KEY_1:
        _selectedBlockId = BLOCK_STONE;
        break;
    case ax::EventKeyboard::KeyCode::KEY_2:
        _selectedBlockId = BLOCK_DIRT;
        break;
    case ax::EventKeyboard::KeyCode::KEY_3:
        _selectedBlockId = BLOCK_GRASS;
        break;
    case ax::EventKeyboard::KeyCode::KEY_4:
        _selectedBlockId = BLOCK_SAND;
        break;
    case ax::EventKeyboard::KeyCode::KEY_5:
        _selectedBlockId = BLOCK_GRAVEL;
        break;
    case ax::EventKeyboard::KeyCode::KEY_6:
        _selectedBlockId = BLOCK_COAL_ORE;
        break;
    case ax::EventKeyboard::KeyCode::KEY_7:
        _selectedBlockId = BLOCK_IRON_ORE;
        break;
    case ax::EventKeyboard::KeyCode::KEY_8:
        _selectedBlockId = BLOCK_GOLD_ORE;
        break;
    case ax::EventKeyboard::KeyCode::KEY_9:
        _selectedBlockId = BLOCK_CRYSTAL_ORE;
        break;
    default:
        break;
    }
}

void FirstPersonController::onKeyReleased(ax::EventKeyboard::KeyCode code, ax::Event* event)
{
    if (!_enabled)
        return;
    switch (code)
    {
    case ax::EventKeyboard::KeyCode::KEY_W:
        _keyW = false;
        break;
    case ax::EventKeyboard::KeyCode::KEY_S:
        _keyS = false;
        break;
    case ax::EventKeyboard::KeyCode::KEY_A:
        _keyA = false;
        break;
    case ax::EventKeyboard::KeyCode::KEY_D:
        _keyD = false;
        break;
    case ax::EventKeyboard::KeyCode::KEY_SPACE:
        _keySpace = false;
        break;
    default:
        break;
    }
}

void FirstPersonController::processBlockInteraction(float dt)
{
    // Защита от выполнения при отключенном контроллере
    if (!_enabled)
        return;

    if (!_raycaster || _raycaster->getInteractionMode() != BlockInteractionMode::BreakHold)
    {
        return;
    }

    if (!_raycaster->hasHit())
    {
        _breakAccum = 0.0f;
        _raycaster->setBreakProgress(0.0f);
        return;
    }

    _breakAccum += dt;
    float progress = std::min(_breakAccum / BREAK_TIME, 1.0f);
    _raycaster->setBreakProgress(progress);

    if (progress >= 1.0f)
    {
        _raycaster->breakBlock();
        _breakAccum = 0.0f;
        _raycaster->setBreakProgress(0.0f);
    }
}
