#include "SceneTextureCache.h"
#include "../../EditorApp.h"
#include "../../core/Project.h"
#include <deki-editor/AssetData.h>
#include <deki-editor/Paths.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/EditorAssets.h>
#include <deki-editor/EditorTexture.h>
#include <filesystem>

namespace DekiEditor
{

SceneTextureCache::SceneTextureCache(EditorApp* app)
    : m_App(app)
{
}

SceneTextureCache::~SceneTextureCache()
{
    Cleanup();
}

void SceneTextureCache::Cleanup()
{
    for (auto& [path, tex] : m_Cache)
    {
        if (tex.textureId != 0)
        {
            EditorTexture::Destroy(tex.textureId);
        }
    }
    m_Cache.clear();
    m_TotalBytes = 0;
}

void SceneTextureCache::EvictToBudget(const std::string& keep)
{
    // Evict the least recently used entries, never `keep`, until within
    // budget. Each entry holds a full RGBA copy.
    while (m_TotalBytes > m_ByteBudget && m_Cache.size() > 1)
    {
        auto victim = m_Cache.end();
        for (auto it = m_Cache.begin(); it != m_Cache.end(); ++it)
        {
            if (it->first == keep)
            {
                continue;
            }
            if (victim == m_Cache.end() || it->second.lastUse < victim->second.lastUse)
            {
                victim = it;
            }
        }
        if (victim == m_Cache.end())
        {
            break;
        }
        if (victim->second.textureId != 0)
        {
            EditorTexture::Destroy(victim->second.textureId);
        }
        m_TotalBytes -= victim->second.pixelData.size();
        m_Cache.erase(victim);
    }
}

void SceneTextureCache::Invalidate(const std::string& assetRef)
{
    auto it = m_Cache.find(assetRef);
    if (it != m_Cache.end())
    {
        if (it->second.textureId != 0)
        {
            EditorTexture::Destroy(it->second.textureId);
        }
        m_TotalBytes -= it->second.pixelData.size();
        m_Cache.erase(it);
    }
}

CachedTexture* SceneTextureCache::GetOrLoad(const std::string& assetRef)
{
    if (assetRef.empty())
    {
        return nullptr;
    }

    // Keyed by the asset reference, a GUID or a path.
    auto it = m_Cache.find(assetRef);
    if (it != m_Cache.end())
    {
        it->second.lastUse = ++m_UseCounter;
        return it->second.textureId != 0 ? &it->second : nullptr;
    }

    auto* project = m_App->GetProject();
    if (!project)
    {
        return nullptr;
    }

    std::string cachePath;

    // A GUID has dashes and at least 32 characters.
    bool isGuid = (assetRef.find('-') != std::string::npos && assetRef.length() >= 32);

    if (isGuid)
    {
        auto* pipeline = m_App->GetAssetPipeline();
        if (pipeline)
        {
            const AssetInfo* assetInfo = pipeline->GetAssetInfoByGuid(assetRef);
            if (assetInfo && !assetInfo->cachePath.empty())
            {
                cachePath = assetInfo->cachePath;
            }
        }

        // Else try the cache path for the GUID directly.
        if (cachePath.empty())
        {
            cachePath = GetCachePath(project->GetPath(), assetRef);
        }
    }
    else
    {
        // An asset path: read the GUID from its sidecar.
        std::string assetsPath = project->GetAssetsPath();
        std::string fullAssetPath = assetsPath + "/" + assetRef;

        for (char& c : fullAssetPath)
        {
            if (c == '\\')
            {
                c = '/';
            }
        }

        std::string guid = ReadAssetGuid(fullAssetPath);
        if (!guid.empty())
        {
            cachePath = GetCachePath(project->GetPath(), guid);
        }
    }

    if (cachePath.empty() || !std::filesystem::exists(cachePath))
    {
        // No cached TEX: remember the failure.
        m_Cache[assetRef] = CachedTexture{};
        return nullptr;
    }

    // The image loader the editor provides decodes the texture.
    auto imageLoader = DekiEditor::EditorAssets::GetImageLoader();
    if (!imageLoader)
    {
        m_Cache[assetRef] = CachedTexture{};
        return nullptr;
    }
    int32_t texWidth = 0, texHeight = 0;
    bool hasAlpha = false;
    uint8_t* rgbaData = imageLoader(cachePath.c_str(), texWidth, texHeight, hasAlpha);

    if (!rgbaData || texWidth <= 0 || texHeight <= 0)
    {
        m_Cache[assetRef] = CachedTexture{};
        return nullptr;
    }

    const uint32_t textureId = EditorTexture::CreateRgba(texWidth, texHeight, rgbaData);

    CachedTexture cached;
    cached.textureId = textureId;
    cached.width = static_cast<uint32_t>(texWidth);
    cached.height = static_cast<uint32_t>(texHeight);
    cached.spritePath = assetRef;

    // Pixels kept for alpha hit testing.
    size_t dataSize = static_cast<size_t>(texWidth) * texHeight * 4;
    cached.pixelData.assign(rgbaData, rgbaData + dataSize);

    // The engine allocated the RGBA data with new[].
    delete[] rgbaData;

    cached.lastUse = ++m_UseCounter;
    m_TotalBytes += cached.pixelData.size();
    m_Cache[assetRef] = std::move(cached);
    EvictToBudget(assetRef);
    return &m_Cache[assetRef];
}

}  // namespace DekiEditor
