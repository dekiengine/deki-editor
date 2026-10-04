#pragma once

#include "IPanel.h"

#include <string>
#include <vector>
#include <memory>
#include "PlatformEditorDialog.h"
#include <deki-editor/BuildStripSettings.h>
#include <deki-editor/build/TargetBuilder.h>
#include <deki-editor/build/PlatformTemplates.h>

namespace DekiEditor
{

class EditorApp;
class ITargetBuilder;

/// Builds and deploys the project for a platform: platform choice, a short
/// toolchain status, Build, Clean, the active backend's deploy step, and the
/// build options.
class BuildPanel : public IPanel
{
public:
    const char* Id() const override { return "Build"; }
    BuildPanel(EditorApp* app);
    ~BuildPanel();

    void Draw();

    const std::string& GetSelectedPlatform() const { return m_SelectedPlatform; }
    void SetSelectedPlatform(const std::string& platformId);

private:
    void DrawToolbar();
    void DrawPlatformSelector();
    void DrawToolchainStatusSummary();
    void DrawPlatformDetails();
    void DrawDeployTarget();
    void DrawStripping();

    void StartBuild();
    void StartDeploy();

    /// Asks the active backend where a deploy can go, keeping the current
    /// choice when it is still offered.
    void RefreshDeployTargets();
    ITargetBuilder* GetActiveBuilder();

    EditorApp* m_App;

    // Stripping switches (services/BuildStripSettings) and the plan's one-line
    // summary, cached until a switch changes or the user asks again.
    BuildStripSettings m_Strip;
    std::string m_StripProject;
    std::string m_StripSummary;

    std::string m_SelectedPlatform;
    int m_SelectedPlatformIndex = 0;

    // What the active backend offers to deploy to, and the one chosen. Both
    // belong to that backend, so they are re-read whenever it changes.
    std::vector<DeployTarget> m_DeployTargets;
    std::string m_SelectedDeployTarget;
    ITargetBuilder* m_DeployTargetsOwner = nullptr;

    // Build state
    bool m_IsBuilding = false;
    bool m_IsUploading = false;
    float m_BuildProgress = 0.0f;
    std::string m_BuildStatus;

    // Build options
    bool m_EnableLogging = true;
    bool m_EnableInternalLogging = false;
    // Build the board's firmware for its backend's simulator (QEMU for ESP32)
    // in its own build directory; deploy then runs it there.
    bool m_Simulate = false;
    bool m_DeployAfterBuild = false;
    bool m_PendingDeploy = false;

    PlatformEditorDialog m_PlatformEditor;
    bool m_ShowDeleteConfirm = false;
    std::string m_PlatformToDelete;

    // Where the selected platform came from (build/PlatformTemplates), read
    // again only when the platforms change: it compares files.
    void DrawPlatformOrigin();
    void RegenerateBuildFiles(const PlatformConfig& config);
    PlatformOriginStatus m_Origin;
    std::string m_OriginFor;
    unsigned m_OriginRevision = 0;

    // Whether the active builder's toolchain is installed. Asking reads the
    // disk (for ESP-IDF, its export script and version file), too slow on
    // Windows to do every frame. So the answer is kept, and asked again when
    // the panel appears, the builder changes, or a few seconds pass.
    const void* m_ToolchainFor = nullptr;
    bool m_ToolchainReady = false;
    double m_ToolchainCheckedAt = -1.0;
    std::string m_OriginNotice;  // what the last update or restore did
    bool m_ShowRestoreConfirm = false;
};

}  // namespace DekiEditor
