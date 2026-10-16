/**
 * @file SaveGameService.h
 * @brief Сервис сохранения/загрузки прогресса (production-заготовка под облако).
 *
 * Сохраняет состояние игрока в JSON в writable-каталоге платформы. Формат с явным
 * полем version заложен под будущую миграцию и синхронизацию (например, Steam Cloud):
 * достаточно подменить путь на облачный и добавить разбор новых полей.
 *
 * @note Сейчас сохраняется позиция игрока. Структура расширяема (сид мира, инвентарь,
 *       время суток) без изменения публичного API.
 */
#pragma once

#include <string>

class GameScene;

class SaveGameService
{
public:
    /// @brief Сохранить текущее состояние сцены. @return true при успешной записи.
    static bool saveGame(GameScene* scene);

    /// @brief Загрузить состояние в сцену. @return true если файл найден и применён.
    static bool loadGame(GameScene* scene);

    /// @brief Существует ли валидный файл сохранения (для активации пункта «Продолжить»).
    static bool hasSave();

    /// @brief Прочитать только id мира из сейва (без создания сцены).
    /// Нужен GameScene::createFromSave, чтобы сразу поднять правильный мир.
    /// @return id мира или пустая строка (нет сейва / поля / повреждённый JSON).
    static std::string readSavedWorldId();

private:
    /// @brief Абсолютный путь к файлу сохранения в writable-каталоге.
    static std::string savePath();

    /// @brief Версия формата сохранения (для будущих миграций).
    static constexpr int kSaveVersion = 1;
};
