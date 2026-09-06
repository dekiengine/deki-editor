#include "SceneTextureCache.h"
#include "../../EditorApp.h"
#include "../../core/Project.h"
#include <deki-editor/AssetData.h>
#include <deki-editor/Paths.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/EditorAssets.h>
#include <SDL3/SDL_opengl.h>
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
            GLuint texId = tex.textureId;
            glDeleteTextures(1, &texId);
        }
    }
    m_Cache.clear();
    m_TotalBytes = 0;
}

void SceneTextureCache::EvictToBudget(const std::string& keep)
{
    // The cache used to grow without bound: one full RGBA copy per texture ever
    // shown in the scene view, for the whole session.
    while (m_TotalBytes > m_ByteBudget && m_Cache.size() > 1)
    {
        auto victim = m_Cache.end();
        for (auto it = m_Cache.begin(); it != m_Cache.end(); ++it)
        {
            if (it->first == keep)
                continue;
            if (victim == m_Cache.end() || it->second.lastUse < victim->second.lastUse)
                victim = it;
        }
        if (victim == m_Cache.end())
            break;
        if (victim->second.textureId != 0)
        {
            GLuint texId = victim->second.textureId;
            glDeleteTextures(1, &texId);
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
            GLuint texId = it->second.textureId;
            glDeleteTextures(1, &texId);
        }
        m_TotalBytes -= it->second.pixelData.size();
        m_Cache.erase(it);
    }
}

CachedTexture* SceneTextureCache::GetOrLoad(const std::string& assetRef)
{
    if (assetRef.empty())
        return nullptr;

    // Check cache first (keyed by the asset reference - could be GUID or path)
    auto it = m_Cache.find(assetRef);
    if (it != m_Cache.end())
    {
        it->second.lastUse = ++m_UseCounter;
        return it->second.textureId != 0 ? &it->second : nullptr;
    }

    // Need to load the texture
    auto* project = m_App->GetProject();
    if (!project)
        return nullptr;

    std::string cachePath;

    // Check if this looks like a GUID (contains dashes like "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx")
    bool isGuid = (assetRef.find('-') != std::string::npos && assetRef.length() >= 32);

    if (isGuid)
    {
        // Look up asset by GUID via AssetPipeline
        auto* pipeline = m_App->GetAssetPipeline();
        if (pipeline)
        {
            const AssetInfo* assetInfo = pipeline->GetAssetInfoByGuid(assetRef);
            if (assetInfo && !assetInfo->cachePath.empty())
            {
                cachePath = assetInfo->cachePath;
            }
        }

        // Fallback: try direct cache path lookup
        if (cachePath.empty())
        {
            cachePath = GetCachePath(project->GetPath(), assetRef);
        }
    }
    else
    {
        // Treat as asset path - build full path and get GUID
        std::string assetsPath = project->GetAssetsPath();
        std::string fullAssetPath = assetsPath + "/" + assetRef;

        // Normalize path separators
        for (char& c : fullAssetPath)
        {
            if (c == '\\') c = '/';
        }

        // Get asset GUID and find cached TEX
        std::string guid = ReadAssetGuid(fullAssetPath);
        if (!guid.empty())
        {
            cachePath = GetCachePath(project->GetPath(), guid);
        }
    }

    if (cachePath.empty() || !std::filesystem::exists(cachePath))
    {
        // No cached TEX, mark as failed
        m_Cache[assetRef] = CachedTexture{};
        return nullptr;
    }

    // Use injected image loader to load and convert the texture
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

    // Create OpenGL texture
    GLuint textureId;
    glGenTextures(1, &textureId);
    glBindTexture(GL_TEXTURE_2D, textureId);

    // Use GL_NEAREST for both filters for pixel-perfect rendering (no interpolation)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Unpack state is global; other GL users (e.g. ImGui glyph uploads) can leave
    // a row stride behind, which would shear/overread this tightly packed upload.
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 texWidth, texHeight,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaData);

    glBindTexture(GL_TEXTURE_2D, 0);

    // Store in cache
    CachedTexture cached;
    cached.textureId = textureId;
    cached.width = static_cast<uint32_t>(texWidth);
    cached.height = static_cast<uint32_t>(texHeight);
    cached.spritePath = assetRef;

    // Keep pixel data for alpha hit testing
    size_t dataSize = static_cast<size_t>(texWidth) * texHeight * 4;
    cached.pixelData.assign(rgbaData, rgbaData + dataSize);

    // Clean up the RGBA data (engine allocated with new[])
    delete[] rgbaData;

    cached.lastUse = ++m_UseCounter;
    m_TotalBytes += cached.pixelData.size();
    m_Cache[assetRef] = std::move(cached);
    EvictToBudget(assetRef);
    return &m_Cache[assetRef];
}

}  // namespace DekiEditor
