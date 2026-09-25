#pragma once

#include <map>
#include <string>
#include <cstdint>

namespace DekiEditor
{

class EditorApp;

class TextureInspector
{
public:
    TextureInspector(EditorApp* app);
    ~TextureInspector();

    // Granular draw methods for orchestrated layout
    void DrawPreview();                             // Anchored texture preview (always visible)
    void BeginScrollRegion();                       // Opens scrollable child region
    void DrawSettings();                            // Settings section (format dropdown + spritesheet button)
    void DrawData();                                // Data section (sizes, alpha, source info)
    void DrawCacheStatus(int overrideCached = -1);  // Cache status indicator (-1 = use default, 0/1 = override)
    static float GetCacheStatusBarHeight();
    void EndScrollRegion();  // Closes scrollable child region

    // Draw a compact cache status bar at the bottom of any inspector
    // Shows: cached status, file size, small thumbnail
    void DrawCacheStatusBar();

    // Invalidate preview (call when asset is recompiled)
    void InvalidatePreview() { m_PreviewTexturePath.clear(); }

    // Check if a file is a supported texture format
    static bool IsTextureFile(const std::string& path);

private:
    EditorApp* m_App;

    // Texture preview state
    uint32_t m_PreviewTextureId = 0;
    std::string m_PreviewTexturePath;
    uint32_t m_PreviewWidth = 0;
    uint32_t m_PreviewHeight = 0;

    // Format selection state
    int m_SelectedFormatIndex = 0;      // 0 = Automatic
    std::map<std::string, int> m_TargetFormatIndex;  // platform id -> index; absent/0 = the asset's own
    bool m_SourceHasAlpha = false;       // from the cached header, for showing what Automatic gives
    char m_OverrideSearch[64] = {};      // filter in the Add override popup
    std::string m_LastFormatAssetPath;  // Detect asset selection changes

    // Chroma key state (loaded from / written to .data sidecar's
    // settings.texture.chroma_key block)
    bool m_ChromaKeyEnabled = false;
    float m_ChromaKeyColor[3] = { 1.0f, 0.0f, 1.0f };  // magenta default

    void UpdatePreviewTexture(const std::string& cachePath);
    void CleanupPreviewTexture();
};

}  // namespace DekiEditor
