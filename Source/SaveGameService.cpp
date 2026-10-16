#include "SaveGameService.h"
#include "GameScene.h"

#include "axmol.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

using namespace ax;

std::string SaveGameService::savePath()
{
    return FileUtils::getInstance()->getWritablePath() + "savegame.json";
}

bool SaveGameService::saveGame(GameScene* scene)
{
    if (!scene)
        return false;

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();

    doc.AddMember("version", kSaveVersion, alloc);

    // Мир, в котором сделано сохранение: при «Продолжить» сцена поднимается
    // сразу с этим миром (GameScene::createFromSave читает поле до init).
    const std::string& worldId = scene->getWorldId();
    rapidjson::Value worldIdVal;
    worldIdVal.SetString(worldId.c_str(), static_cast<rapidjson::SizeType>(worldId.size()), alloc);
    doc.AddMember("worldId", worldIdVal, alloc);

    const Vec3 pos = scene->getPlayerPosition();
    doc.AddMember("playerX", pos.x, alloc);
    doc.AddMember("playerY", pos.y, alloc);
    doc.AddMember("playerZ", pos.z, alloc);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    const std::string path = savePath();
    const bool ok          = FileUtils::getInstance()->writeStringToFile(buffer.GetString(), path);
    AXLOGI("SaveGameService: save {} -> {}", ok ? "OK" : "FAILED", path);
    return ok;
}

bool SaveGameService::loadGame(GameScene* scene)
{
    if (!scene || !hasSave())
        return false;

    const std::string json = FileUtils::getInstance()->getStringFromFile(savePath());

    rapidjson::Document doc;
    doc.Parse(json.c_str());
    if (doc.HasParseError() || !doc.IsObject())
    {
        AXLOGW("SaveGameService: повреждённый файл сохранения, игнорируем");
        return false;
    }

    // Защитное чтение: при отсутствии/неверном типе поля используем 0.
    auto readFloat = [&doc](const char* key) -> float {
        return (doc.HasMember(key) && doc[key].IsNumber()) ? doc[key].GetFloat() : 0.0f;
    };

    // Контроль соответствия мира: createFromSave поднимает сцену с миром из
    // сейва, поэтому расхождение возможно лишь если мир удалили из конфига.
    if (doc.HasMember("worldId") && doc["worldId"].IsString())
    {
        const std::string savedWorld = doc["worldId"].GetString();
        if (!savedWorld.empty() && savedWorld != scene->getWorldId())
            AXLOGW("SaveGameService: сейв из мира '{}', активен '{}' — позиция может не соответствовать местности",
                   savedWorld, scene->getWorldId());
    }

    const Vec3 pos{readFloat("playerX"), readFloat("playerY"), readFloat("playerZ")};
    scene->setPlayerPosition(pos);
    return true;
}

bool SaveGameService::hasSave()
{
    return FileUtils::getInstance()->isFileExist(savePath());
}

std::string SaveGameService::readSavedWorldId()
{
    if (!hasSave())
        return {};

    const std::string json = FileUtils::getInstance()->getStringFromFile(savePath());
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    // Поле опциональное (формат v1 допускает сейвы без worldId).
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("worldId") || !doc["worldId"].IsString())
        return {};
    return doc["worldId"].GetString();
}
