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

    // The asset inspector calls these in order to lay out the panel.
    void DrawPreview();        // the texture preview, fixed above the scrolling part
    void BeginScrollRegion();  // opens the scrolling region
    void DrawSettings();       // Settings (chroma key, spritesheet button) and Targets
    /// The Targets section: a Default card with the settings every target
    /// uses, then one card per target that can override them. `write` edits
    /// the image's settings.texture and reimports it.
    void DrawTargets(const std::function<void(const std::function<void(nlohmann::json&)>&)>& write);
    void EndScrollRegion();  // closes the scrolling region

    /// Drops the preview. Call it when the asset is recompiled.
    void InvalidatePreview() { m_PreviewTexturePath.clear(); }

    /// True if the file is an image format the importer reads.
    static bool IsTextureFile(const std::string& path);

private:
    EditorApp* m_App;

    // Texture preview state
    uint32_t m_PreviewTextureId = 0;
    std::string m_PreviewTexturePath;
    uint32_t m_PreviewWidth = 0;  // the image's own size (frames and Max Size are in it)
    uint32_t m_PreviewHeight = 0;
    uint32_t m_StoredWidth = 0;  // the cached texture's, smaller when Max Size shrank it
    uint32_t m_StoredHeight = 0;

    // Format selection state
    int m_SelectedFormatIndex = 0;                   // 0 = Automatic
    std::map<std::string, int> m_TargetFormatIndex;  // platform id -> index; absent/0 = the asset's own
    int m_MaxSizeIndex = 0;                          // into kTextureMaxSizes; 0 = no limit
    std::map<std::string, int> m_TargetMaxIndex;     // platform id -> index; absent = the asset's own
    bool m_DefaultOpen = true;                       // the Default card unfolded
    std::map<std::string, bool> m_TargetOpen;        // platform id -> card unfolded (first seen: open if overridden)
    bool m_SourceHasAlpha = false;                   // from the cached header, for showing what Automatic gives
    std::string m_LastFormatAssetPath;               // to notice a new selection

    // Chroma key, kept in the .data sidecar's settings.texture.chroma_key.
    bool m_ChromaKeyEnabled = false;
    float m_ChromaKeyColor[3] = { 1.0f, 0.0f, 1.0f };  // magenta default

    void UpdatePreviewTexture(const std::string& cachePath);
    void CleanupPreviewTexture();
};

}  // namespace DekiEditor
