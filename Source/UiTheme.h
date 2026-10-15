/**
 * @file UiTheme.h
 * @brief Единый минималистичный стиль и инициализация Dear ImGui для UI игры.
 *
 * Централизует:
 * - Ленивую, идемпотентную настройку ImGuiPresenter (разрешение, DPI, шрифт с кириллицей).
 * - Тёмную «продакшн»-палитру: прозрачные панели, плоские кнопки с акцентом при наведении.
 * - Вспомогательные виджеты: центрированный текст, полноширинная кнопка-пункт меню.
 *
 * @note ImGui 1.92 поддерживает динамическую растеризацию глифов: кириллица из arial.ttf
 *       подгружается по требованию, отдельные glyph-ranges не нужны.
 *       Приложение собирается с MSVC /utf-8 (см. axmol ax_setup_app_props),
 *       поэтому строковые литералы в UTF-8 корректно доходят до ImGui.
 */
#pragma once

#include "axmol.h"
#include "ImGui/ImGuiPresenter.h"

namespace ui_theme
{
/// @brief Акцентный цвет интерфейса (холодный синий) — наведение/активные элементы.
inline const ImVec4 kAccent      = ImVec4(0.22f, 0.52f, 0.92f, 1.00f);
inline const ImVec4 kAccentHover = ImVec4(0.30f, 0.60f, 0.98f, 1.00f);

/**
 * @brief Применяет минималистичную тёмную тему к глобальному стилю ImGui.
 * @note Вызывается один раз из setup(). Стиль ImGui глобальный и persist между сценами.
 */
inline void applyStyle()
{
    ImGuiStyle& s      = ImGui::GetStyle();
    s.WindowRounding   = 10.0f;
    s.FrameRounding    = 8.0f;
    s.GrabRounding     = 8.0f;
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize  = 0.0f;
    s.WindowPadding    = ImVec2(30.0f, 30.0f);
    s.FramePadding     = ImVec2(16.0f, 12.0f);
    s.ItemSpacing      = ImVec2(14.0f, 12.0f);
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    ImVec4* c                    = s.Colors;
    c[ImGuiCol_Text]             = ImVec4(0.92f, 0.94f, 0.97f, 1.00f);
    c[ImGuiCol_TextDisabled]     = ImVec4(0.45f, 0.48f, 0.54f, 1.00f);
    c[ImGuiCol_WindowBg]         = ImVec4(0.05f, 0.06f, 0.09f, 0.88f);  // полупрозрачная панель
    c[ImGuiCol_ChildBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_Border]           = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.13f, 0.15f, 0.20f, 0.85f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.18f, 0.21f, 0.28f, 0.90f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.20f, 0.24f, 0.32f, 1.00f);
    c[ImGuiCol_Button]           = ImVec4(1.00f, 1.00f, 1.00f, 0.04f);  // почти невидимая кнопка
    c[ImGuiCol_ButtonHovered]    = kAccent;
    c[ImGuiCol_ButtonActive]     = ImVec4(0.18f, 0.44f, 0.80f, 1.00f);
    c[ImGuiCol_SliderGrab]       = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccentHover;
    c[ImGuiCol_CheckMark]        = kAccentHover;
    c[ImGuiCol_Header]           = ImVec4(0.18f, 0.21f, 0.28f, 0.80f);
    c[ImGuiCol_HeaderHovered]    = kAccent;
    c[ImGuiCol_HeaderActive]     = kAccentHover;
}

/**
 * @brief Идемпотентная глобальная инициализация ImGui для всего приложения.
 *
 * Безопасно вызывать из onEnter() любой сцены: при повторных вызовах — no-op.
 * Настраивает логическое разрешение под дизайн-резолюцию, DPI-масштаб,
 * подгружает шрифт с поддержкой кириллицы и применяет тему.
 *
 * @note Презентер живёт всё время работы приложения; здесь его не уничтожаем.
 */
inline void setup()
{
    static bool s_initialized = false;
    if (s_initialized)
        return;
    s_initialized = true;

    using namespace ax;
    auto* presenter = extension::ImGuiPresenter::getInstance();

    auto designSize = Director::getInstance()->getRenderView()->getDesignResolutionSize();
#if (AX_TARGET_PLATFORM == AX_PLATFORM_ANDROID)
    presenter->setViewResolution(designSize.width, designSize.height);
#endif
    presenter->enableDPIScale();

    // отключаем Viewports, чтобы не было «плавающих» окон вне игрового окна
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;

    // arial.ttf содержит кириллицу — глифы растеризуются по требованию (ImGui 1.92).
    auto fontPath = FileUtils::getInstance()->fullPathForFilename("fonts/arial.ttf");
    if (!fontPath.empty())
        presenter->addFont(fontPath);

    applyStyle();
}

/// @brief Рисует горизонтально центрированный по текущему окну текст.
inline void textCentered(const char* text)
{
    float avail  = ImGui::GetContentRegionAvail().x;
    float textW  = ImGui::CalcTextSize(text).x;
    float offset = (avail - textW) * 0.5f;
    if (offset > 0.0f)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
    ImGui::TextUnformatted(text);
}

/// @brief Полноширинная кнопка-пункт меню фиксированной высоты.
/// @return true если кнопка нажата в этом кадре.
inline bool menuButton(const char* label, float height = 48.0f)
{
    float width = ImGui::GetContentRegionAvail().x;
    return ImGui::Button(label, ImVec2(width, height));
}

}  // namespace ui_theme
