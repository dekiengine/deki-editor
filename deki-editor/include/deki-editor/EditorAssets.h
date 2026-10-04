#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>
#include <memory>
#include <functional>

// Forward declarations
// The editor holds bitmap fonts only as opaque handles; the type itself
// belongs to deki-2d, so name it there rather than declaring a second one at
// global scope that is ambiguous wherever both are visible.
namespace Deki2D
{
class BitmapFont;
}

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

/**
 * @brief Editor-only asset utilities for GPU resource management
 *
 * Provides Unity-like asset loading for editor use:
 *   uint32_t texId = EditorAssets::Get()->LoadTexture(guid, &w, &h);
 *   Deki2D::BitmapFont* font = EditorAssets::Get()->GetBitmapFont(bakedGuid);
 *
 * Features:
 * - GPU texture caching by GUID
 * - Automatic texture upload to OpenGL
 * - Font atlas loading and caching
 *
 * Note: Uses AssetManager's GUID table for path resolution.
 * The editor must populate the GUID table on project load.
 */
class DEKI_EDITOR_API EditorAssets
{
public:
    // ===== Package Injection (registered by 2d package at load time) =====

    /// Load a .dtex image file as RGBA pixel data. Caller must free() the returned buffer.
    using ImageLoaderFn = uint8_t* (*)(const char* path, int32_t& outWidth, int32_t& outHeight, bool& outHasAlpha);

    /// Load a .dfont file, returning an opaque Deki2D::BitmapFont* and the atlas RGBA data for GPU upload.
    /// The caller must free() outAtlasRGBA after uploading. The returned font is owned by the caller.
    using FontFromDfontFn = void* (*)(const char* dfontPath, uint8_t** outAtlasRGBA, int32_t& outAtlasW,
                                      int32_t& outAtlasH);

    /// Destroy an opaque Deki2D::BitmapFont* returned by FontFromDfontFn.
    using FontDestroyFn = void (*)(void* font);

    static void RegisterImageLoader(ImageLoaderFn fn);
    static void RegisterFontFactory(FontFromDfontFn factory, FontDestroyFn destroyer);
    static ImageLoaderFn GetImageLoader();

    /// Null all package-injected function pointers. Must be called before a package
    /// DLL is unloaded - otherwise any subsequent LoadTexture/LoadFontAtlas would
    /// invoke code that no longer exists, crashing the editor.
    static void ClearPackageInjectedCallbacks();

    /**
     * @brief Get singleton instance
     */
    static EditorAssets* Get();

    /**
     * @brief Shutdown and cleanup all GPU resources
     */
    static void Shutdown();

    /**
     * @brief Load texture by GUID and upload to GPU
     * @param guid Asset GUID (looked up via AssetManager)
     * @param outWidth Optional output for texture width
     * @param outHeight Optional output for texture height
     * @return OpenGL texture ID, or 0 on failure
     *
     * Textures are cached - subsequent calls with same GUID return cached texture.
     */
    uint32_t LoadTexture(const std::string& guid, uint32_t* outWidth = nullptr, uint32_t* outHeight = nullptr);

    /**
     * @brief Invalidate cached texture (forces reload on next access)
     * @param guid Asset GUID
     */
    void InvalidateTexture(const std::string& guid);

    /**
     * @brief Clear all cached textures (frees GPU memory)
     */
    void ClearTextureCache();

    // ===== Font Atlas Methods =====

    /**
     * @brief Load font atlas by baked GUID and upload to GPU
     * @param bakedGuid Deterministic GUID of the baked font (font.guid)
     * @param outWidth Optional output for atlas width
     * @param outHeight Optional output for atlas height
     * @return OpenGL texture ID, or 0 on failure
     *
     * Uses AssetManager::LookupPath() to resolve the baked GUID to a .dfont path.
     */
    uint32_t LoadFontAtlas(const std::string& bakedGuid, uint32_t* outWidth = nullptr, uint32_t* outHeight = nullptr);

    /**
     * @brief Load font atlas from filesystem path
     * @param dfontPath Full path to the .dfont file
     * @param outWidth Optional output for atlas width
     * @param outHeight Optional output for atlas height
     * @return OpenGL texture ID, or 0 on failure
     */
    uint32_t LoadFontAtlasFromPath(const std::string& dfontPath, uint32_t* outWidth = nullptr,
                                   uint32_t* outHeight = nullptr);

    /**
     * @brief Get BitmapFont by baked GUID (for glyph metrics)
     * @param bakedGuid Deterministic GUID of the baked font
     * @return Pointer to BitmapFont, or nullptr if not cached
     */
    Deki2D::BitmapFont* GetBitmapFont(const std::string& bakedGuid);

    /**
     * @brief Invalidate cached font (forces reload on next access)
     * @param bakedGuid Baked font GUID
     */
    void InvalidateFontAtlas(const std::string& bakedGuid);

    /**
     * @brief Clear all cached fonts (frees GPU memory)
     */
    void ClearFontCache();

    // ===== Font Baking (for TTF → BitmapFont conversion) =====

    /**
     * @brief Callback for font baking
     * @param sourceGuid TTF asset GUID
     * @param fontSize Font size in pixels
     * @return BitmapFont pointer (owned by caller's cache), or nullptr on failure
     */
    using FontBakeCallback = std::function<Deki2D::BitmapFont*(const std::string&, int)>;

    /**
     * @brief Callback for getting font atlas texture
     * @param sourceGuid TTF asset GUID
     * @param fontSize Font size in pixels
     * @param outWidth Output atlas width
     * @param outHeight Output atlas height
     * @return OpenGL texture ID, or 0 on failure
     */
    using FontAtlasCallback = std::function<uint32_t(const std::string&, int, uint32_t*, uint32_t*)>;

    /**
     * @brief Set callbacks for font baking (called by editor on startup)
     */
    void SetFontBakingCallbacks(FontBakeCallback bakeFont, FontAtlasCallback getAtlas);

    /**
     * @brief Clear font baking callbacks before package DLL unload
     *
     * Must be called BEFORE FreeLibrary on the package that set the callbacks,
     * so the std::function destructor can safely invoke the old callable's
     * manager function while the DLL code is still mapped.
     */
    void ClearFontBakingCallbacks();

    /**
     * @brief Get BitmapFont by source GUID and size (triggers baking if needed)
     * @param sourceGuid TTF asset GUID
     * @param fontSize Font size in pixels
     * @return BitmapFont pointer, or nullptr if not found
     */
    Deki2D::BitmapFont* GetBitmapFontWithBaking(const std::string& sourceGuid, int fontSize);

    /**
     * @brief Load font atlas by source GUID and size (triggers baking if needed)
     * @param sourceGuid TTF asset GUID
     * @param fontSize Font size in pixels
     * @param outWidth Optional output for atlas width
     * @param outHeight Optional output for atlas height
     * @return OpenGL texture ID, or 0 on failure
     */
    uint32_t LoadFontAtlasWithBaking(const std::string& sourceGuid, int fontSize, uint32_t* outWidth = nullptr,
                                     uint32_t* outHeight = nullptr);

    // ===== Sprite Settings =====

    /**
     * @brief Load sprite settings (frame dimensions) from .data file
     * @param guid Asset GUID
     * @param outFrameWidth Optional output for frame width
     * @param outFrameHeight Optional output for frame height
     * @return True if sprite settings exist and were loaded
     *
     * For spritesheets, this returns the individual frame dimensions.
     * For regular textures without sprite settings, returns false.
     */
    bool LoadSpriteSettings(const std::string& guid, int* outFrameWidth = nullptr, int* outFrameHeight = nullptr);

    // ===== Frame Sprite Support =====

    /**
     * @brief Load texture for a frame sprite (loads parent spritesheet texture)
     * @param frameGuid Frame GUID (format: "parent:frame-N")
     * @param outWidth Optional output for parent texture width
     * @param outHeight Optional output for parent texture height
     * @return OpenGL texture ID of parent spritesheet, or 0 on failure
     *
     * Frame sprites reference the same texture as their parent spritesheet.
     * Use GetFrameUVs() to get the UV coordinates for the specific frame.
     */
    uint32_t LoadFrameTexture(const std::string& frameGuid, uint32_t* outWidth = nullptr,
                              uint32_t* outHeight = nullptr);

    /**
     * @brief Get UV coordinates for a frame sprite
     * @param frameGuid Frame GUID (format: "parent:frame-N")
     * @param outU0 Output for top-left U coordinate
     * @param outV0 Output for top-left V coordinate
     * @param outU1 Output for bottom-right U coordinate
     * @param outV1 Output for bottom-right V coordinate
     * @return True if frame info was found and UVs calculated
     *
     * UV coordinates are normalized (0.0 to 1.0) based on frame position within parent texture.
     */
    bool GetFrameUVs(const std::string& frameGuid, float* outU0, float* outV0, float* outU1, float* outV1);

private:
    EditorAssets() = default;
    ~EditorAssets();

    // Prevent copying
    EditorAssets(const EditorAssets&) = delete;
    EditorAssets& operator=(const EditorAssets&) = delete;

    // Internal texture upload
    uint32_t UploadTextureToGPU(const uint8_t* rgba, int width, int height);

    // Cached texture info
    struct CachedTexture
    {
        uint32_t textureId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // Cached font info (owns the BitmapFont via opaque pointer)
    struct CachedFontAtlas
    {
        uint32_t textureId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        void* font = nullptr;  // Opaque Deki2D::BitmapFont*, destroyed via s_FontDestroyer
    };

    // Internal font loading
    CachedFontAtlas* LoadFontAtlasInternal(const std::string& dfontPath, const std::string& cacheKey);

    // Cache by GUID
    std::unordered_map<std::string, CachedTexture> m_TextureCache;

    // Font cache by GUID or path
    std::unordered_map<std::string, std::unique_ptr<CachedFontAtlas>> m_FontAtlasCache;

    // Font baking callbacks
    FontBakeCallback m_FontBakeCallback;
    FontAtlasCallback m_FontAtlasCallback;

    // Singleton instance
    static EditorAssets* s_Instance;
};

}  // namespace DekiEditor
