#include "VoxelCollisionResolver.h"
#include "ChunkManager.h"
#include <algorithm>
#include <cmath>

// =========================================================================
//  resolve — главный цикл разрешения коллизий
// =========================================================================
CollisionResult VoxelCollisionResolver::resolve(float dt, const ax::Vec3& velocity, PlayerCapsule& capsule)
{
    CollisionResult result;
    result.resolvedPosition = capsule.bottomPos;
    result.isGrounded       = false;
    result.collisionNormal  = ax::Vec3::ZERO;
    // Явная инициализация нового поля
    result.hitCeiling = false;

    if (dt <= 0.0f || !_chunkMgr)
        return result;

    // Порядок проходов важен: Y первым для корректной гравитации и прыжков

    // Сохраняем флаг потолочного столкновения ДО проходов X/Z.
    // Y-проход может установить collisionNormal.y > 0 (потолок),
    // но последующий X или Z проход перезапишет collisionNormal на горизонтальный.
    // Флаг hitCeiling сохраняет информацию о потолке независимо от последующих осей.
    sweepAxis(dt, velocity, capsule, 1, result.isGrounded, result);  // Y
    if (result.collisionNormal.y > 0.5f)
    {
        result.hitCeiling = true;
    }
    sweepAxis(dt, velocity, capsule, 0, result.isGrounded, result);  // X
    sweepAxis(dt, velocity, capsule, 2, result.isGrounded, result);  // Z

    // Явный ground probe после sweep-проходов.
    //
    // Проблема: при очень высоком FPS (144+, 240+) шаг движения за кадр
    // может быть меньше минимального yOff (0.01) в checkCollisionAt.
    // Y-sweep "проскальзывает" и не детектирует коллизию с полом,
    // флаг isGrounded сбрасывается, и игрок "дребезжит" на земле.
    //
    // Решение: отдельная проверка чуть ниже подошвы (0.1 единицы).
    // Это не влияет на Y-sweep (который по-прежнему обрабатывает столкновения),
    // а лишь корректирует флаг isGrounded для граничного случая.
    if (!result.isGrounded)
    {
        float r = capsule.radius - 0.02f;
        // Проверяем несколько точек под подошвой с учётом радиуса капсулы
        ax::Vec3 probeCenter = result.resolvedPosition + ax::Vec3(0, -0.1f, 0);
        if (isSolidAt(probeCenter) || isSolidAt(probeCenter + ax::Vec3(r, 0, r)) ||
            isSolidAt(probeCenter + ax::Vec3(-r, 0, r)) || isSolidAt(probeCenter + ax::Vec3(r, 0, -r)) ||
            isSolidAt(probeCenter + ax::Vec3(-r, 0, -r)))
        {
            result.isGrounded = true;
        }
    }

    // Обновляем позицию капсулы после разрешения
    capsule.bottomPos = result.resolvedPosition;
    return result;
}

bool VoxelCollisionResolver::isSolidAt(const ax::Vec3& worldPos) const
{
    // Запрашиваем ID блока по мировым координатам
    BlockId id = _chunkMgr->getBlockAtWorldPos(worldPos);

    /**
    @brief Проверка на препятствие для капсулы игрока.
    @details Возвращает true только для твёрдых блоков (земля, камень, трава).
             Воздух (BLOCK_AIR && y >= SEA_LEVEL) и вода (BLOCK_WATER) трактуются как проходимые среды.
             Это позволяет игроку падать в воду и свободно перемещаться внутри неё.
    */
    return id != BLOCK_AIR && id != BLOCK_WATER;
}

// =========================================================================
//  sweepAxis — развёртка вдоль одной оси с SUB-STEPPING
// =========================================================================
void VoxelCollisionResolver::sweepAxis(float dt,
                                       const ax::Vec3& velocity,
                                       PlayerCapsule& capsule,
                                       int axis,
                                       bool& grounded,
                                       CollisionResult& outResult)
{
    float velAxis = (axis == 0) ? velocity.x : (axis == 1) ? velocity.y : velocity.z;
    if (std::abs(velAxis) < EPSILON)
        return;  // Движения нет, пропускаем

    float moveDelta = velAxis * dt;

    // === ЗАЩИТА ОТ ЛАГОВ ===
    // Ограничиваем максимальное смещение за кадр, чтобы при просадках FPS
    // игрок не пролетал сквозь стены и цикл не выполнялся слишком долго
    constexpr float MAX_DELTA = 10.0f;
    moveDelta                 = std::clamp(moveDelta, -MAX_DELTA, MAX_DELTA);

    // === ПРОСТРАНСТВЕННЫЙ SUB-STEPPING ===
    // Разбиваем движение на шаги не более SWEEP_STEP блоков.
    // При SWEEP_STEP = 0.5f гарантированно detecting столкновение со стеной толщиной 1 блок.
    constexpr float SWEEP_STEP = 0.5f;
    int steps                  = std::max(1, static_cast<int>(std::ceil(std::abs(moveDelta) / SWEEP_STEP)));
    float stepDelta            = moveDelta / static_cast<float>(steps);

    // === BOUNDING BOX ПРОВЕРКА ===
    // Проверяем несколько уровней высоты с учётом радиуса.
    // Skin Width (radius - 0.02f) предотвращает застревание на углах блоков (corner snagging)
    // checkBodyAt — проверка тела капсулы (уровни от подошвы вверх).
    // withGroundProbe добавляет точку на -0.05 ниже подошвы — это критично при высоком FPS,
    // когда шаг движения за кадр < 0.01, и точка yOff=0.01 не попадает в блок.
    // Для отрицательного смещения проверяем только центр (без углов радиуса),
    // т.к. капсула не расширяется вниз — это probe, а не физическое расширение.
    auto checkBodyAt = [&](const ax::Vec3& pos, bool withGroundProbe) -> bool {
        float yLevels[] = {0.01f, capsule.height * 0.5f, capsule.height - 0.01f};
        float r         = capsule.radius - 0.02f;  // Skin width

        if (withGroundProbe && isSolidAt(ax::Vec3(pos.x, pos.y - 0.05f, pos.z)))
            return true;

        for (float yOff : yLevels)
        {
            // 4 угла капсулы
            if (isSolidAt(pos + ax::Vec3(r, yOff, r)))
                return true;
            if (isSolidAt(pos + ax::Vec3(-r, yOff, r)))
                return true;
            if (isSolidAt(pos + ax::Vec3(r, yOff, -r)))
                return true;
            if (isSolidAt(pos + ax::Vec3(-r, yOff, -r)))
                return true;
            // Центр (для узких стен ровно по центру)
            if (isSolidAt(ax::Vec3(pos.x, pos.y + yOff, pos.z)))
                return true;
        }
        return false;
    };
    auto checkCollisionAt = [&](const ax::Vec3& pos) -> bool { return checkBodyAt(pos, true); };

    // === ПОДЪЁМ НА СТУПЕНЬКУ (step-up, «лесенка») ===
    // Макс. высота препятствия, на которое игрок поднимается без прыжка (1 блок).
    constexpr float STEP_UP_HEIGHT = 1.0f;
    constexpr float STEP_UP_PROBE  = 0.05f;  // Шаг сканирования высоты подъёма
    bool steppedUp = false;  // Не более одного подъёма на ось за кадр

    // === ИНКРЕМЕНТАЛЬНАЯ РАЗВЁРТКА ===
    for (int i = 0; i < steps; ++i)
    {
        ax::Vec3 testPos = outResult.resolvedPosition;
        if (axis == 0)
            testPos.x += stepDelta;
        else if (axis == 1)
            testPos.y += stepDelta;
        else
            testPos.z += stepDelta;

        if (checkCollisionAt(testPos))
        {
            // --- Подъём на ступеньку (только по горизонтали) ---
            // Сканируем высоту от STEP_UP_HEIGHT вниз: ищем наибольшую h, при которой
            // тело свободно, а на h - STEP_UP_PROBE уже коллизия — определяем высоту
            // верха препятствия над текущей подошвой (оставшуюся высоту подъёма).
            // Условие grounded || velocity.y > 0: подъём продолжается, пока капсула
            // уже взбирается (скорость поддерживается каждым кадром повторного
            // столкновения), но не запускается «с лету» при падении вдоль стены.
            if (axis != 1 && (grounded || velocity.y > 0.0f) && !steppedUp)
            {
                float stepY = -1.0f;
                for (float h = STEP_UP_HEIGHT; h > 0.0f; h -= STEP_UP_PROBE)
                {
                    if (checkBodyAt(testPos + ax::Vec3(0.0f, h, 0.0f), false))
                        break;
                    stepY = h;
                }
                if (stepY > 0.0f)
                {
                    // Плавный подъём («желе»): позицию НЕ меняем — сообщаем контроллеру
                    // оставшуюся высоту, он задаст вертикальную скорость, и капсула
                    // взойдёт на ступеньку за несколько кадров (как мини-прыжок).
                    outResult.stepUp       = true;
                    outResult.stepUpHeight = stepY;
                    steppedUp              = true;

                    // Горизонталь по-прежнему заблокирована до конца подъёма
                    float sign = (moveDelta > 0.0f) ? 1.0f : -1.0f;
                    outResult.collisionNormal =
                        ax::Vec3(axis == 0 ? sign : 0, axis == 1 ? sign : 0, axis == 2 ? sign : 0);
                    return;
                }
            }

            // Коллизия обнаружена на этом шаге!
            float sign = (moveDelta > 0.0f) ? 1.0f : -1.0f;

            // Вычисляем нормаль для скольжения (wall sliding)
            outResult.collisionNormal = ax::Vec3(axis == 0 ? sign : 0, axis == 1 ? sign : 0, axis == 2 ? sign : 0);

            // Если коллизия снизу (ось Y, движение вниз), помечаем grounded
            if (axis == 1 && moveDelta < 0.0f)
                grounded = true;

            // ВАЖНО: Не двигаем позицию! Остаёмся на outResult.resolvedPosition.
            // Прерываем цикл — движение по этой оси заблокировано.
            return;
        }

        // Если коллизии нет — принимаем шаг
        outResult.resolvedPosition = testPos;

        // Если двигаемся вниз и не уперлись в пол на этом шаге — сбрасываем grounded
        if (axis == 1 && moveDelta < 0.0f)
            grounded = false;
    }
}
