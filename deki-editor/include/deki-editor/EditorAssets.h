#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>
#include <memory>
#include <functional>

// The editor holds bitmap fonts only as opaque handles. The type belongs to
// deki-2d, so it is declared in that namespace: a second declaration at
// global scope would be ambiguous wherever both are visible.
namespace Deki2D
{
class BitmapFont;
}

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

/// Loads assets onto the GPU for the editor, cached by GUID:
///   uint32_t texId = EditorAssets::Get()->LoadTexture(guid, &w, &h);
///   Deki2D::BitmapFont* font = EditorAssets::Get()->GetBitmapFont(bakedGuid);
///
/// Paths come from AssetManager's GUID table, which the editor must fill when
/// a project loads.
class DEKI_EDITOR_API EditorAssets
{
public:
    // ===== Package Injection (registered by 2d package at load time) =====

    /// Loads a .dtex image file as RGBA pixels. The caller must free() the buffer.
    using ImageLoaderFn = uint8_t* (*)(const char* path, int32_t& outWidth, int32_t& outHeight, bool& outHasAlpha);

    /// Loads a .dfont file, returning an opaque Deki2D::BitmapFont* and the
    /// atlas RGBA data to upload. The caller owns the font and must free()
    /// outAtlasRGBA after uploading.
    using FontFromDfontFn = void* (*)(const char* dfontPath, uint8_t** outAtlasRGBA, int32_t& outAtlasW,
                                      int32_t& outAtlasH);

    /// Destroys a font returned by FontFromDfontFn.
    using FontDestroyFn = void (*)(void* font);

    static void RegisterImageLoader(ImageLoaderFn fn);
    static void RegisterFontFactory(FontFromDfontFn factory, FontDestroyFn destroyer);
    static ImageLoaderFn GetImageLoader();

    /// Clears every function pointer packages registered. Must be called
    /// before a package DLL unloads, or the next LoadTexture/LoadFontAtlas
    /// calls code that is gone and the editor crashes.
    static void ClearPackageInjectedCallbacks();

    static EditorAssets* Get();

    /// Frees every GPU resource.
    static void Shutdown();

    /// The EditorTexture for an asset GUID (resolved through AssetManager),
    /// uploading it on first use, or 0 on failure. Width and height are
    /// written to the optional outputs.
    uint32_t LoadTexture(const std::string& guid, uint32_t* outWidth = nullptr, uint32_t* outHeight = nullptr);

    /// Drops a cached texture so the next access reloads it.
    void InvalidateTexture(const std::string& guid);

    /// Drops every cached texture, freeing GPU memory.
    void ClearTextureCache();

    // ===== Font Atlas Methods =====

    /// The EditorTexture atlas for a baked font (font.guid, resolved to a
    /// .dfont through AssetManager::LookupPath()), or 0 on failure.
    uint32_t LoadFontAtlas(const std::string& bakedGuid, uint32_t* outWidth = nullptr, uint32_t* outHeight = nullptr);

    /// The EditorTexture atlas for the .dfont at a full path, or 0 on failure.
    uint32_t LoadFontAtlasFromPath(const std::string& dfontPath, uint32_t* outWidth = nullptr,
                                   uint32_t* outHeight = nullptr);

    /// The BitmapFont of a baked font, for glyph metrics, or nullptr if it is
    /// not cached.
    Deki2D::BitmapFont* GetBitmapFont(const std::string& bakedGuid);

    /// Drops a cached font so the next access reloads it.
    void InvalidateFontAtlas(const std::string& bakedGuid);

    /// Drops every cached font, freeing GPU memory.
    void ClearFontCache();

    // ===== Font Baking (for TTF → BitmapFont conversion) =====

    /// Bakes a TTF asset (GUID) at a pixel size. Returns the font, owned by
    /// the caller's cache, or nullptr on failure.
    using FontBakeCallback = std::function<Deki2D::BitmapFont*(const std::string&, int)>;

    /// The EditorTexture atlas of a TTF asset (GUID) at a pixel size, with
    /// its width and height, or 0 on failure.
    using FontAtlasCallback = std::function<uint32_t(const std::string&, int, uint32_t*, uint32_t*)>;

    /// Called by the editor at startup.
    void SetFontBakingCallbacks(FontBakeCallback bakeFont, FontAtlasCallback getAtlas);

    /// Must be called BEFORE FreeLibrary on the package that set the
    /// callbacks: the std::function destructor runs the callable's manager
    /// function, which must still be mapped.
    void ClearFontBakingCallbacks();

    /// The BitmapFont of a TTF asset at a pixel size, baking it if needed, or
    /// nullptr if not found.
    Deki2D::BitmapFont* GetBitmapFontWithBaking(const std::string& sourceGuid, int fontSize);

    /// The EditorTexture atlas of a TTF asset at a pixel size, baking it if
    /// needed, or 0 on failure.
    uint32_t LoadFontAtlasWithBaking(const std::string& sourceGuid, int fontSize, uint32_t* outWidth = nullptr,
                                     uint32_t* outHeight = nullptr);

    // ===== Sprite Settings =====

    /// Reads a spritesheet's frame size from the asset's .data file. Returns
    /// false for a texture without sprite settings.
    bool LoadSpriteSettings(const std::string& guid, int* outFrameWidth = nullptr, int* outFrameHeight = nullptr);

    // ===== Frame Sprite Support =====

    /// The EditorTexture of a frame's parent spritesheet, or 0 on failure.
    /// `frameGuid` has the form "parent:frame-N"; the size outputs get the
    /// parent texture's size. Use GetFrameUVs() for the frame's place in it.
    uint32_t LoadFrameTexture(const std::string& frameGuid, uint32_t* outWidth = nullptr,
                              uint32_t* outHeight = nullptr);

    /// The normalized UVs (0 to 1) of a frame ("parent:frame-N") within its
    /// parent texture: (U0, V0) top-left, (U1, V1) bottom-right. Returns false
    /// when the frame is not found.
    bool GetFrameUVs(const std::string& frameGuid, float* outU0, float* outV0, float* outU1, float* outV1);

private:
    EditorAssets() = default;
    ~EditorAssets();

    EditorAssets(const EditorAssets&) = delete;
    EditorAssets& operator=(const EditorAssets&) = delete;

    uint32_t UploadTextureToGPU(const uint8_t* rgba, int width, int height);

    struct CachedTexture
    {
        uint32_t textureId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // Owns its BitmapFont through the opaque pointer.
    struct CachedFontAtlas
    {
        uint32_t textureId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        void* font = nullptr;  // Deki2D::BitmapFont*, destroyed via s_FontDestroyer
    };

    CachedFontAtlas* LoadFontAtlasInternal(const std::string& dfontPath, const std::string& cacheKey);

    // Keyed by GUID.
    std::unordered_map<std::string, CachedTexture> m_TextureCache;

    // Keyed by GUID or path.
    std::unordered_map<std::string, std::unique_ptr<CachedFontAtlas>> m_FontAtlasCache;

    FontBakeCallback m_FontBakeCallback;
    FontAtlasCallback m_FontAtlasCallback;

    static EditorAssets* s_Instance;
};

}  // namespace DekiEditor
