#pragma once
#include "axmol.h"
#include "renderer/backend/ProgramState.h"
#include "renderer/backend/ProgramManager.h"
#include "3d/MeshMaterial.h"

/**
 * @file WaterShader.h
 * @brief Утилита для создания и обновления кастомного шейдера анимированной воды.
 *
 * @par Архитектура шейдера:
 * Вертексный шейдер (water.vert):
 *   - Реализует суперпозицию 4 волн Gerstner (физически корректная модель)
 *   - Вычисляет аналитическую нормаль поверхности для точного освещения
 *   - Передаёт v_worldPos и v_crest (высота гребня) во фрагментный шейдер
 *
 * Фрагментный шейдер (water.frag):
 *   - Fresnel-Schlick: отражение неба на краях/горизонте
 *   - Blinn-Phong specular: острые солнечные блики с пульсацией
 *   - Procedural caustics: каустика через FBM без текстуры шума
 *   - Procedural foam: пена на гребнях волн через smoothstep + FBM
 *   - Depth-based color: градиент мелководье ↔ глубина
 *
 * @par Размещение файлов:
 * @code
 * Resources/
 *   shaders/
 *     custom/
 *       water.vert   ← вертексный шейдер
 *       water.frag   ← фрагментный шейдер
 * @endcode
 *
 * @par UBO-структуры (std140):
 * VS (vs_ub):  { mat4 u_MVPMatrix; float u_time; float[3] pad; }
 * FS (fs_ub):  { vec4 u_color;     float u_time; float[3] pad; }
 *
 * u_MVPMatrix и u_color заполняются Axmol автоматически через Pass.
 * u_time нужно обновлять вручную каждый кадр через WaterShader::updateTime().
 *
 * @par Потокобезопасность:
 * Все методы должны вызываться только из главного потока.
 */
class WaterShader
{
public:
    // =========================================================================
    // Пути к файлам шейдеров
    // =========================================================================

    /// Путь к вертексному шейдеру (относительно Resources/)
    static constexpr const char* VERT_PATH = "shaders/custom/water.vert";
    /// Путь к фрагментному шейдеру (относительно Resources/)
    static constexpr const char* FRAG_PATH = "shaders/custom/water.frag";

    // =========================================================================
    // Создание программы
    // =========================================================================

    /**
     * @brief Создаёт и возвращает ProgramState для водного шейдера.
     *
     * Использует ProgramManager::loadProgram с VertexLayoutType::Terrain3D,
     * который настраивает атрибуты layout: position(3f) + texcoord(2f) + normal(3f).
     * Это точно соответствует тому, что генерирует ChunkMeshBuilder::createMesh().
     *
     * Программа кешируется в ProgramManager — повторный вызов loadProgram
     * возвращает тот же объект без перекомпиляции шейдеров.
     *
     * @return backend::ProgramState* (autoreleased) или nullptr при ошибке загрузки.
     */
    static ax::backend::ProgramState* createProgramState()
    {
        auto* mgr = ax::backend::ProgramManager::getInstance();
        auto* prog = mgr->loadProgram(
            VERT_PATH, FRAG_PATH,
            ax::backend::VertexLayoutType::Terrain3D   // position3f + texcoord2f + normal3f
        );
        if (!prog)
        {
            AXLOGE("WaterShader: не удалось загрузить шейдеры:\n  VS: {}\n  FS: {}",
                   VERT_PATH, FRAG_PATH);
            return nullptr;
        }
        auto* ps = new ax::backend::ProgramState(prog);
        ps->autorelease();
        return ps;
    }

    // =========================================================================
    // Создание материала (удобный хелпер)
    // =========================================================================

    /**
     * @brief Создаёт полностью сконфигурированный MeshMaterial для водного меша.
     *
     * Настройки render state:
     *   - Alpha blending: SRC_ALPHA / ONE_MINUS_SRC_ALPHA (стандарт прозрачности)
     *   - Depth write: OFF  (прозрачные поверхности не пишут в Z-буфер)
     *   - Depth test:  ON   (правильный порядок прозрачных объектов)
     *   - Cull face:   OFF  (обе грани видны: вид снизу при погружении в воду)
     *
     * @param atlas Текстурный атлас террейна (привязывается к u_tex0).
     *              Шейдер воды не использует атлас для цвета, но движок требует
     *              текстуру для корректной инициализации семплера.
     *              Можно передать nullptr — материал создастся без текстуры.
     * @return MeshMaterial* (autoreleased) или nullptr при ошибке.
     */
    static ax::MeshMaterial* createMaterial(ax::Texture2D* atlas)
    {
        auto* ps = createProgramState();
        if (!ps)
            return nullptr;

        auto* mat = ax::MeshMaterial::createWithProgramState(ps);
        if (!mat)
            return nullptr;

        if (atlas)
            mat->setTexture(atlas, ax::NTextureData::Usage::Diffuse);

        // Alpha blending
        ax::BlendFunc blend{};
        blend.src = ax::backend::BlendFactor::SRC_ALPHA;
        blend.dst = ax::backend::BlendFactor::ONE_MINUS_SRC_ALPHA;
        mat->getStateBlock().setBlendFunc(blend);
        mat->getStateBlock().setDepthWrite(false);
        mat->getStateBlock().setDepthTest(true);
        mat->getStateBlock().setCullFace(false);
        mat->getStateBlock().setCullFaceSide(ax::CullFaceSide::BACK);

        return mat;
    }

    // =========================================================================
    // Обновление юниформ
    // =========================================================================

    /**
     * @brief Обновляет u_time для водного шейдера (VS + FS).
     *
     * Вызывается каждый кадр из GameScene::update() для каждого водного нода.
     * ProgramState кеширует измённые данные и отправляет их в GPU при следующем draw call.
     *
     * @par Как получить ProgramState из Node:
     * @code
     * auto* mr  = dynamic_cast<ax::MeshRenderer*>(node);
     * auto* mat = dynamic_cast<ax::MeshMaterial*>(mr->getMaterial());
     * auto* ps  = mat->getTechnique()->getPassByIndex(0)->getProgramState();
     * WaterShader::updateTime(ps, _animTime);
     * @endcode
     *
     * @param ps   ProgramState водного нода (может быть nullptr — тогда вызов игнорируется)
     * @param time Накопленное время в секундах (_animTime из GameScene)
     */
    static void updateTime(ax::backend::ProgramState* ps, float time)
    {
        if (!ps)
            return;
        // getUniformLocation обращается по имени члена UBO (не имени блока).
        // Axmol распознаёт "u_time" в обоих шейдерах и маршрутизирует по стадии.
        auto loc = ps->getUniformLocation("u_time");
        if (loc)
            ps->setUniform(loc, &time, sizeof(float));
    }

    /**
     * @brief Удобный хелпер: обновляет u_time напрямую из Node.
     *
     * Автоматически достаёт ProgramState из MeshRenderer → MeshMaterial → Pass.
     * Безопасен при nullptr и при несовпадении типов (dynamic_cast).
     *
     * @param node Водный нод (MeshRenderer с водным шейдером)
     * @param time Накопленное время в секундах
     */
    static void updateTimeFromNode(ax::Node* node, float time)
    {
        if (!node)
            return;
        auto* mr = dynamic_cast<ax::MeshRenderer*>(node);
        if (!mr)
            return;
        auto* mat = dynamic_cast<ax::MeshMaterial*>(mr->getMaterial());
        if (!mat)
            return;
        auto* tech = mat->getTechnique();
        if (!tech || tech->getPassCount() == 0)
            return;
        updateTime(tech->getPassByIndex(0)->getProgramState(), time);
    }
};
