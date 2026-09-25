#include <deki-editor/EditorAssets.h>
#include <deki-editor/AssetDatabase.h>
#include <deki-editor/AssetData.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/TextureImporter.h>
#include <deki/assets/AssetManager.h>

// OpenGL headers
#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>

// GL_CLAMP_TO_EDGE may not be defined in older GL headers
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

// Debug logging
#ifndef DEKI_LOG_INFO
#define DEKI_LOG_INFO(...) \
    printf(__VA_ARGS__);   \
    printf("\n")
#endif

namespace DekiEditor
{

// Package-injected callbacks (registered by 2d package at load time)
static EditorAssets::ImageLoaderFn s_imageLoader = nullptr;
static EditorAssets::FontFromDfontFn s_fontFactory = nullptr;
static EditorAssets::FontDestroyFn s_fontDestroyer = nullptr;

EditorAssets* EditorAssets::s_Instance = nullptr;

void EditorAssets::RegisterImageLoader(ImageLoaderFn fn)
{
    s_imageLoader = fn;
}

void EditorAssets::RegisterFontFactory(FontFromDfontFn factory, FontDestroyFn destroyer)
{
    s_fontFactory = factory;
    s_fontDestroyer = destroyer;
}

void EditorAssets::ClearPackageInjectedCallbacks()
{
    s_imageLoader = nullptr;
    s_fontFactory = nullptr;
    s_fontDestroyer = nullptr;
}

EditorAssets::ImageLoaderFn EditorAssets::GetImageLoader()
{
    return s_imageLoader;
}

EditorAssets* EditorAssets::Get()
{
    if (!s_Instance)
    {
        s_Instance = new EditorAssets();
    }
    return s_Instance;
}

void EditorAssets::Shutdown()
{
    if (s_Instance)
    {
        s_Instance->ClearTextureCache();
        s_Instance->ClearFontCache();
        delete s_Instance;
        s_Instance = nullptr;
    }
}

EditorAssets::~EditorAssets()
{
    ClearTextureCache();
    ClearFontCache();
}

uint32_t EditorAssets::LoadTexture(const std::string& guid, uint32_t* outWidth, uint32_t* outHeight)
{
    if (guid.empty())
        return 0;

    // Check cache first
    auto it = m_TextureCache.find(guid);
    if (it != m_TextureCache.end())
    {
        if (outWidth) *outWidth = it->second.width;
        if (outHeight) *outHeight = it->second.height;
        return it->second.textureId;
    }

    if (!s_imageLoader)
        return 0;

    // Look up path via AssetManager
    Deki::AssetManager* assetMgr = Deki::AssetManager::Get();
    if (!assetMgr)
        return 0;

    std::string path = assetMgr->LookupPath(guid);
    if (path.empty())
        return 0;

    // Load via injected image loader
    int32_t w = 0, h = 0;
    bool hasAlpha = false;
    uint8_t* rgba = s_imageLoader(path.c_str(), w, h, hasAlpha);
    if (!rgba)
        return 0;

    // Upload to GPU
    uint32_t texId = UploadTextureToGPU(rgba, w, h);
    free(rgba);

    if (texId == 0)
        return 0;

    // Report the image's own size: frames and UVs are in its pixels, and a
    // texture shrunk by Max Size (the GPU copy is then smaller) records it.
    int imageW = w, imageH = h;
    TextureImporter::ReadSourceSize(path, imageW, imageH);

    // Cache it
    CachedTexture cached;
    cached.textureId = texId;
    cached.width = static_cast<uint32_t>(imageW);
    cached.height = static_cast<uint32_t>(imageH);
    m_TextureCache[guid] = cached;

    if (outWidth) *outWidth = cached.width;
    if (outHeight) *outHeight = cached.height;

    return texId;
}

void EditorAssets::InvalidateTexture(const std::string& guid)
{
    auto it = m_TextureCache.find(guid);
    if (it != m_TextureCache.end())
    {
        if (it->second.textureId != 0)
        {
            GLuint texId = static_cast<GLuint>(it->second.textureId);
            glDeleteTextures(1, &texId);
        }
        m_TextureCache.erase(it);
    }
}

void EditorAssets::ClearTextureCache()
{
    // Delete GUID-cached textures
    for (auto& [guid, cached] : m_TextureCache)
    {
        if (cached.textureId != 0)
        {
            GLuint texId = static_cast<GLuint>(cached.textureId);
            glDeleteTextures(1, &texId);
        }
    }
    m_TextureCache.clear();
}

// ===== Font Atlas Methods =====

uint32_t EditorAssets::LoadFontAtlas(const std::string& bakedGuid, uint32_t* outWidth, uint32_t* outHeight)
{
    if (bakedGuid.empty())
        return 0;

    // Check cache first
    auto it = m_FontAtlasCache.find(bakedGuid);
    if (it != m_FontAtlasCache.end() && it->second)
    {
        if (outWidth) *outWidth = it->second->width;
        if (outHeight) *outHeight = it->second->height;
        return it->second->textureId;
    }

    // Look up path via AssetManager
    Deki::AssetManager* assetMgr = Deki::AssetManager::Get();
    if (!assetMgr)
        return 0;

    std::string path = assetMgr->LookupPath(bakedGuid);
    if (path.empty())
        return 0;

    // Load font and cache by GUID
    CachedFontAtlas* cached = LoadFontAtlasInternal(path, bakedGuid);
    if (!cached)
        return 0;

    if (outWidth) *outWidth = cached->width;
    if (outHeight) *outHeight = cached->height;
    return cached->textureId;
}

uint32_t EditorAssets::LoadFontAtlasFromPath(const std::string& dfontPath, uint32_t* outWidth, uint32_t* outHeight)
{
    if (dfontPath.empty())
        return 0;

    // Check cache first (use path as key)
    auto it = m_FontAtlasCache.find(dfontPath);
    if (it != m_FontAtlasCache.end() && it->second)
    {
        if (outWidth) *outWidth = it->second->width;
        if (outHeight) *outHeight = it->second->height;
        return it->second->textureId;
    }

    // Load font and cache by path
    CachedFontAtlas* cached = LoadFontAtlasInternal(dfontPath, dfontPath);
    if (!cached)
        return 0;

    if (outWidth) *outWidth = cached->width;
    if (outHeight) *outHeight = cached->height;
    return cached->textureId;
}

Deki2D::BitmapFont* EditorAssets::GetBitmapFont(const std::string& bakedGuid)
{
    if (bakedGuid.empty())
        return nullptr;

    // Check cache
    auto it = m_FontAtlasCache.find(bakedGuid);
    if (it != m_FontAtlasCache.end() && it->second)
        return static_cast<Deki2D::BitmapFont*>(it->second->font);

    // Try to load it
    LoadFontAtlas(bakedGuid);

    it = m_FontAtlasCache.find(bakedGuid);
    if (it != m_FontAtlasCache.end() && it->second)
        return static_cast<Deki2D::BitmapFont*>(it->second->font);

    return nullptr;
}

void EditorAssets::InvalidateFontAtlas(const std::string& bakedGuid)
{
    auto it = m_FontAtlasCache.find(bakedGuid);
    if (it != m_FontAtlasCache.end())
    {
        if (it->second)
        {
            if (it->second->textureId != 0)
            {
                GLuint texId = static_cast<GLuint>(it->second->textureId);
                glDeleteTextures(1, &texId);
            }
            if (it->second->font && s_fontDestroyer)
                s_fontDestroyer(it->second->font);
        }
        m_FontAtlasCache.erase(it);
    }
}

void EditorAssets::ClearFontCache()
{
    for (auto& [key, cached] : m_FontAtlasCache)
    {
        if (cached)
        {
            if (cached->textureId != 0)
            {
                GLuint texId = static_cast<GLuint>(cached->textureId);
                glDeleteTextures(1, &texId);
            }
            if (cached->font && s_fontDestroyer)
                s_fontDestroyer(cached->font);
        }
    }
    m_FontAtlasCache.clear();
}

void EditorAssets::SetFontBakingCallbacks(FontBakeCallback bakeFont, FontAtlasCallback getAtlas)
{
    m_FontBakeCallback = bakeFont;
    m_FontAtlasCallback = getAtlas;
}

void EditorAssets::ClearFontBakingCallbacks()
{
    m_FontBakeCallback = nullptr;
    m_FontAtlasCallback = nullptr;
}

Deki2D::BitmapFont* EditorAssets::GetBitmapFontWithBaking(const std::string& sourceGuid, int fontSize)
{
    if (m_FontBakeCallback)
    {
        return m_FontBakeCallback(sourceGuid, fontSize);
    }
    return nullptr;
}

uint32_t EditorAssets::LoadFontAtlasWithBaking(const std::string& sourceGuid, int fontSize,
                                               uint32_t* outWidth, uint32_t* outHeight)
{
    if (m_FontAtlasCallback)
    {
        return m_FontAtlasCallback(sourceGuid, fontSize, outWidth, outHeight);
    }
    return 0;
}

EditorAssets::CachedFontAtlas* EditorAssets::LoadFontAtlasInternal(const std::string& dfontPath, const std::string& cacheKey)
{
    if (!s_fontFactory)
        return nullptr;

    namespace fs = std::filesystem;

    if (!fs::exists(dfontPath))
        return nullptr;

    // Delegate dfont parsing + Deki2D::BitmapFont creation to the 2d package
    uint8_t* atlasRGBA = nullptr;
    int32_t atlasW = 0, atlasH = 0;
    void* font = s_fontFactory(dfontPath.c_str(), &atlasRGBA, atlasW, atlasH);

    if (!font)
        return nullptr;

    // Upload atlas to GPU
    uint32_t texId = 0;
    if (atlasRGBA)
    {
        texId = UploadTextureToGPU(atlasRGBA, atlasW, atlasH);
        free(atlasRGBA);
    }

    if (texId == 0)
    {
        if (s_fontDestroyer) s_fontDestroyer(font);
        return nullptr;
    }

    // Create cache entry
    auto cached = std::make_unique<CachedFontAtlas>();
    cached->textureId = texId;
    cached->width = static_cast<uint32_t>(atlasW);
    cached->height = static_cast<uint32_t>(atlasH);
    cached->font = font;

    CachedFontAtlas* result = cached.get();
    m_FontAtlasCache[cacheKey] = std::move(cached);

    return result;
}

uint32_t EditorAssets::UploadTextureToGPU(const uint8_t* rgba, int width, int height)
{
    if (!rgba || width <= 0 || height <= 0)
        return 0;

    GLuint texId = 0;
    glGenTextures(1, &texId);
    if (texId == 0)
        return 0;

    glBindTexture(GL_TEXTURE_2D, texId);

    // Set texture parameters (pixel-art friendly: nearest filtering)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Unpack state is global; other GL users (e.g. ImGui glyph uploads) can leave
    // a row stride behind, which would shear/overread this tightly packed upload.
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    // Upload texture data
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    return static_cast<uint32_t>(texId);
}

// ===== Sprite Settings =====

bool EditorAssets::LoadSpriteSettings(const std::string& guid, int* outFrameWidth, int* outFrameHeight)
{
    namespace fs = std::filesystem;

    if (outFrameWidth) *outFrameWidth = 0;
    if (outFrameHeight) *outFrameHeight = 0;

    if (guid.empty())
    {
        return false;
    }

    // Look up source path via AssetDatabase (not cache path!)
    std::string assetPath = AssetDatabase::GUIDToAbsolutePath(guid);

    if (assetPath.empty())
    {
        return false;
    }

    // Construct .data file path
    std::string dataPath = assetPath + ".data";

    if (!fs::exists(dataPath))
    {
        return false;
    }

    // Read .data file (JSON format)
    std::ifstream dataFile(dataPath);
    if (!dataFile)
    {
        return false;
    }

    try
    {
        nlohmann::json data;
        dataFile >> data;

        if (!data.contains("settings") || !data["settings"].contains("sprite"))
        {
            return false;
        }

        auto& sprite = data["settings"]["sprite"];
        if (outFrameWidth && sprite.contains("frameWidth"))
            *outFrameWidth = sprite["frameWidth"].get<int>();
        if (outFrameHeight && sprite.contains("frameHeight"))
            *outFrameHeight = sprite["frameHeight"].get<int>();

        return true;
    }
    catch (const std::exception& e)
    {
        return false;
    }
    catch (...)
    {
        return false;
    }
}

// ===== Frame Sprite Support =====

uint32_t EditorAssets::LoadFrameTexture(const std::string& frameGuid, uint32_t* outWidth, uint32_t* outHeight)
{
    if (frameGuid.empty())
        return 0;

    // Check if this is a sub-asset
    const SubAssetInfo* subAsset = AssetDatabase::GetSubAsset(frameGuid);
    if (!subAsset)
    {
        // Not a sub-asset, treat as regular texture
        return LoadTexture(frameGuid, outWidth, outHeight);
    }

    // Load parent texture
    return LoadTexture(subAsset->parentGuid, outWidth, outHeight);
}

bool EditorAssets::GetFrameUVs(const std::string& frameGuid, float* outU0, float* outV0, float* outU1, float* outV1)
{
    if (frameGuid.empty())
    {
        DEKI_LOG_INFO("[GetFrameUVs] frameGuid is empty");
        return false;
    }

    DEKI_LOG_INFO("[GetFrameUVs] Looking up frame GUID: %s", frameGuid.c_str());

    // Initialize outputs to default (full texture)
    if (outU0) *outU0 = 0.0f;
    if (outV0) *outV0 = 0.0f;
    if (outU1) *outU1 = 1.0f;
    if (outV1) *outV1 = 1.0f;

    // Get sub-asset info to find parent GUID
    const SubAssetInfo* subAsset = AssetDatabase::GetSubAsset(frameGuid);
    if (!subAsset)
    {
        DEKI_LOG_INFO("[GetFrameUVs] SubAsset not found for GUID: %s", frameGuid.c_str());
        return false;
    }
    DEKI_LOG_INFO("[GetFrameUVs] Found SubAsset: parentGuid=%s, index=%d",
                  subAsset->parentGuid.c_str(), subAsset->subAssetIndex);

    // Get sprite frame data from TextureImporter
    const SpriteFrameData* frameData = TextureImporter::GetFrameData(frameGuid);
    if (!frameData)
    {
        DEKI_LOG_INFO("[GetFrameUVs] FrameData not found for GUID: %s", frameGuid.c_str());
        return false;
    }
    DEKI_LOG_INFO("[GetFrameUVs] Found FrameData: x=%d, y=%d, w=%d, h=%d",
                  frameData->x, frameData->y, frameData->width, frameData->height);

    // Load parent texture to get dimensions
    uint32_t parentWidth = 0, parentHeight = 0;
    uint32_t texId = LoadTexture(subAsset->parentGuid, &parentWidth, &parentHeight);
    if (texId == 0 || parentWidth == 0 || parentHeight == 0)
        return false;

    // Calculate UV coordinates
    float u0 = static_cast<float>(frameData->x) / static_cast<float>(parentWidth);
    float v0 = static_cast<float>(frameData->y) / static_cast<float>(parentHeight);
    float u1 = static_cast<float>(frameData->x + frameData->width) / static_cast<float>(parentWidth);
    float v1 = static_cast<float>(frameData->y + frameData->height) / static_cast<float>(parentHeight);

    if (outU0) *outU0 = u0;
    if (outV0) *outV0 = v0;
    if (outU1) *outU1 = u1;
    if (outV1) *outV1 = v1;

    return true;
}

}  // namespace DekiEditor
