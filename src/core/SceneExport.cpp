#include "SceneExport.h"

#include <deki/Object.h>
#include <deki/Scene.h>

#include <filesystem>

namespace DekiEditor
{
namespace SceneExport
{

std::string SaveObjectAsScene(Deki::Object* obj, const std::string& folderPath)
{
    namespace fs = std::filesystem;
    if (!obj)
    {
        return "";
    }

    // Unique target path
    const std::string baseName = obj->GetName();
    fs::path targetPath = fs::path(folderPath) / (baseName + ".scene");
    int suffix = 1;
    while (fs::exists(targetPath))
    {
        targetPath = fs::path(folderPath) / (baseName + "_" + std::to_string(suffix) + ".scene");
        suffix++;
    }

    // Temporarily strip instance markers so the object serialises as a
    // standalone scene (otherwise only the instance link is saved).
    const std::string savedGuid = obj->GetSourceSceneGuid();
    const std::string savedOverrides = obj->GetInstanceOverridesJson();
    obj->SetSourceSceneGuid("");
    obj->SetInstanceOverridesJson("");

    // Save/restore the owner scene so the source object is not corrupted.
    Deki::Scene* originalOwner = obj->GetOwnerScene();
    Deki::Scene tempScene;
    tempScene.AddObject(obj);
    const bool saved = tempScene.SaveToFile(targetPath.string());
    tempScene.RemoveObject(obj);
    obj->SetOwnerScene(originalOwner);

    obj->SetSourceSceneGuid(savedGuid);
    obj->SetInstanceOverridesJson(savedOverrides);

    return saved ? targetPath.string() : std::string();
}

}  // namespace SceneExport
}  // namespace DekiEditor
