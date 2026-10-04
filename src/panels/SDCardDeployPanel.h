#pragma once

#include "IPanel.h"

#include <string>
#include <vector>
#include <memory>
#include <mutex>

namespace DekiEditor
{

class EditorApp;
class AssetDeployer;

class SDCardDeployPanel : public IPanel
{
public:
    const char* Id() const override { return "SDCardDeploy"; }
    SDCardDeployPanel(EditorApp* app);
    ~SDCardDeployPanel();

    void Draw();

private:
    EditorApp* m_App;
    std::unique_ptr<AssetDeployer> m_Deployer;

    // Drive selection
    std::vector<std::pair<std::string, std::string>> m_RemovableDrives;  // path, display name
    int m_SelectedDriveIndex = -1;
    std::string m_SelectedDrivePath;
    bool m_NeedsDriveRefresh = true;

    // Deploy state
    // Written by the deploy thread's progress callback and read while drawing,
    // so both sides take m_DeployMutex.
    bool m_IsDeploying = false;
    float m_DeployProgress = 0.0f;
    std::string m_DeployStatus;
    mutable std::mutex m_DeployMutex;

    // Output log
    struct OutputLine
    {
        std::string text;
        bool isError;
    };
    std::vector<OutputLine> m_OutputLines;
    mutable std::mutex m_OutputMutex;
    bool m_AutoScroll = true;

    void AddOutputLine(const std::string& line, bool isError = false);
    void ClearOutput();
};

}  // namespace DekiEditor
