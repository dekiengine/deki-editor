#pragma once

#include "IPanel.h"

#include <string>
#include <vector>
#include <cstdint>

namespace DekiEditor
{

class EditorApp;

class DeployAnalysisPanel : public IPanel
{
public:
    const char* Id() const override { return "DeployAnalysis"; }
    DeployAnalysisPanel(EditorApp* app);
    ~DeployAnalysisPanel();

    void Draw();

private:
    EditorApp* m_App;

    // Asset scan results
    struct AssetEntry
    {
        std::string guid;
        std::string displayPath;     // e.g. "textures/player.png"
        std::string projectRelPath;  // e.g. "project/assets/textures/player.png"
        std::string cachePath;       // project-relative cache path
        bool hasCached;
        uintmax_t cacheSize;   // cache file size in bytes
        bool isUsed;           // referenced by a scene
        bool forceIncluded;    // user override
        std::string packName;  // name of .dpack this asset belongs to (empty = individual)
    };
    std::vector<AssetEntry> m_AssetEntries;
    int m_SelectedIndex = -1;
    bool m_HasScannedAssets = false;
    uintmax_t m_TotalUsedSize = 0;
    uintmax_t m_TotalForceIncludedSize = 0;
    int m_UsedCount = 0;
    int m_ForceIncludedCount = 0;
    void* m_LastProject = nullptr;

    void ScanAssets();
    void RecalculateTotals();
    static std::string FormatFileSize(uintmax_t bytes);
};

}  // namespace DekiEditor
