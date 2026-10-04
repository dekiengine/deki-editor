#include <deki-editor/AssetDatabase.h>
#include <deki-editor/AssetPipeline.h>
#include <algorithm>
#include <cctype>

namespace DekiEditor
{

AssetPipeline* AssetDatabase::s_Pipeline = nullptr;

void AssetDatabase::Initialize(AssetPipeline* pipeline)
{
    s_Pipeline = pipeline;
}

void AssetDatabase::Shutdown()
{
    s_Pipeline = nullptr;
}

void AssetDatabase::ImportAsset(const std::string& path)
{
    if (s_Pipeline)
    {
        s_Pipeline->RefreshAsset(path);
    }
}

void AssetDatabase::Refresh()
{
    // Drains pending OS-watcher events. The watcher is the only source of
    // file changes; nothing rescans the tree.
    if (s_Pipeline)
    {
        s_Pipeline->Tick();
    }
}

std::string AssetDatabase::AssetPathToGUID(const std::string& path)
{
    if (s_Pipeline)
    {
        auto* info = s_Pipeline->GetAssetInfo(path);
        if (info)
        {
            return info->guid;
        }
    }
    return "";
}

std::string AssetDatabase::GUIDToAssetPath(const std::string& guid)
{
    if (s_Pipeline)
    {
        auto* info = s_Pipeline->GetAssetInfoByGuid(guid);
        if (info)
        {
            return info->path;
        }
    }
    return "";
}

std::string AssetDatabase::GUIDToAbsolutePath(const std::string& guid)
{
    if (s_Pipeline)
    {
        auto* info = s_Pipeline->GetAssetInfoByGuid(guid);
        if (info)
        {
            return s_Pipeline->GetAbsolutePath(info->path);
        }
    }
    return "";
}

const SubAssetInfo* AssetDatabase::GetSubAsset(const std::string& guid)
{
    if (s_Pipeline)
    {
        return s_Pipeline->GetSubAsset(guid);
    }
    return nullptr;
}

const std::vector<SubAssetInfo>* AssetDatabase::GetSubAssets(const std::string& parentGuid)
{
    if (s_Pipeline)
    {
        return s_Pipeline->GetSubAssets(parentGuid);
    }
    return nullptr;
}

void AssetDatabase::GetSpritesheetAssets(std::vector<std::pair<std::string, std::string>>& outAssets)
{
    outAssets.clear();

    if (!s_Pipeline)
    {
        return;
    }

    // Spritesheets are textures that have sub-assets (frames).
    const auto& allAssets = s_Pipeline->GetAllAssets();
    for (const auto& [path, info] : allAssets)
    {
        size_t dotPos = path.rfind('.');
        if (dotPos == std::string::npos)
        {
            continue;
        }

        std::string ext = path.substr(dotPos);
        for (char& c : ext)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg")
        {
            continue;
        }

        const auto* subAssets = s_Pipeline->GetSubAssets(info.guid);
        if (subAssets && !subAssets->empty())
        {
            outAssets.emplace_back(info.guid, path);
        }
    }

    // Sorted by path so the list order is stable.
    std::sort(outAssets.begin(), outAssets.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
}

}  // namespace DekiEditor
