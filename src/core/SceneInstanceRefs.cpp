#include "SceneInstanceRefs.h"

#include <algorithm>
#include <regex>

namespace DekiEditor
{

std::vector<std::string> InstancedSceneGuids(const std::string& sceneText)
{
    // Scene::SaveObjectToJson writes an instance as "instance": "<guid>".
    static const std::regex kInstance(
        "\"instance\"\\s*:\\s*\"([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})\"");

    std::vector<std::string> guids;
    for (auto m = std::sregex_iterator(sceneText.begin(), sceneText.end(), kInstance); m != std::sregex_iterator(); ++m)
    {
        guids.push_back((*m)[1].str());
    }
    std::sort(guids.begin(), guids.end());
    guids.erase(std::unique(guids.begin(), guids.end()), guids.end());
    return guids;
}

}  // namespace DekiEditor
