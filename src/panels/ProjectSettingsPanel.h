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

    // The startup-scene and render-pipeline pickers list files from the assets
    // tree. Both sections used to walk the whole tree on every frame they were
    // open, and the pipeline one opened and JSON-parsed every .asset file on
    // each of those frames. The lists are rebuilt when the pipeline's asset
    // count changes or every couple of seconds, whichever comes first.
    void RefreshScans(Project* project);
    std::vector<std::string> m_SceneFiles;
    std::vector<std::string> m_PipelineFiles;
    std::vector<std::string> m_PipelineGuids;
    size_t m_ScannedAssetCount = static_cast<size_t>(-1);
    float m_ScanAge = 1.0e9f;

    EditorApp* m_App;
};

}  // namespace DekiEditor
