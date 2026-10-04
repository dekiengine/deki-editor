#pragma once

#include "IPanel.h"
#include <cstddef>
#include <vector>
#include <string>

namespace DekiEditor
{

class EditorApp;
class Project;

class ProjectSettingsPanel : public IPanel
{
public:
    const char* Id() const override { return "ProjectSettings"; }
    bool PersistOpenState() const override { return false; }
    ProjectSettingsPanel(EditorApp* app);
    ~ProjectSettingsPanel() = default;

    void Draw();

private:
    void DrawStartupSceneSection(Project* project);
    void DrawRenderPipelineSection(Project* project);

    // Lists the scenes and render pipelines in the assets tree for the two
    // pickers. Walking the tree and parsing every .asset is slow, so the lists
    // are rebuilt only when the asset count changes or every two seconds.
    void RefreshScans(Project* project);
    std::vector<std::string> m_SceneFiles;
    std::vector<std::string> m_PipelineFiles;
    std::vector<std::string> m_PipelineGuids;
    size_t m_ScannedAssetCount = static_cast<size_t>(-1);
    float m_ScanAge = 1.0e9f;

    EditorApp* m_App;
};

}  // namespace DekiEditor
