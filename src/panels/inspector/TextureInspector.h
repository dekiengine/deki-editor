#pragma once

#include <functional>
#include <map>
#include <nlohmann/json_fwd.hpp>
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
    void DrawSettings();                            // Settings (chroma key, spritesheet button) and Targets
    // Targets section: every target's stored format, Default first. `write`
    // edits the image's settings.texture and reimports it.
    void DrawTargets(const std::function<void(const std::function<void(nlohmann::json&)>&)>& write);
    void EndScrollRegion();  // Closes scrollable child region

    // Draw a compact cache status bar at the bottom of any inspector
    // Shows: cached status, file size, small thumbnail

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
    std::string m_LastFormatAssetPath;  // Detect asset selection changes

    // Chroma key state (loaded from / written to .data sidecar's
    // settings.texture.chroma_key block)
    bool m_ChromaKeyEnabled = false;
    float m_ChromaKeyColor[3] = { 1.0f, 0.0f, 1.0f };  // magenta default

    void UpdatePreviewTexture(const std::string& cachePath);
    void CleanupPreviewTexture();
};

}  // namespace DekiEditor
