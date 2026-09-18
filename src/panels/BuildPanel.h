#pragma once

#include "IPanel.h"

#include <string>
#include <vector>
#include <memory>
#include "PlatformEditorDialog.h"
#include "../services/BuildStripSettings.h"

namespace DekiEditor
{

class EditorApp;
class ITargetBuilder;

/**
 * @brief Build panel for firmware builds and uploads
 *
 * Provides UI for:
 * - Platform selection (V-Board, Simulator, etc.)
 * - Compact toolchain status summary
 * - Build/Flash/Clean buttons
 * - Build options
 */
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

    void StartBuild();
    void StartFlash();

    std::vector<std::string> EnumeratePorts();
    ITargetBuilder* GetActiveBuilder();

    EditorApp* m_App;

    // Stripping switches (services/BuildStripSettings) and the plan's one-line
    // summary, cached until a switch changes or the user asks again.
    BuildStripSettings m_Strip;
    std::string m_StripProject;
    std::string m_StripSummary;

    // Selected platform ID
    std::string m_SelectedPlatform;
    int m_SelectedPlatformIndex = 0;

    // COM port for flashing
    std::string m_SelectedPort = "COM3";
    std::vector<std::string> m_AvailablePorts;
    int m_SelectedPortIndex = 0;

    // Build state
    bool m_IsBuilding = false;
    bool m_IsUploading = false;
    float m_BuildProgress = 0.0f;
    std::string m_BuildStatus;

    // Build options
    bool m_EnableLogging = true;
    bool m_EnableInternalLogging = false;
    bool m_FlashAfterBuild = false;
    bool m_PendingFlash = false;

    // Platform editor dialog
    PlatformEditorDialog m_PlatformEditor;
    bool m_ShowDeleteConfirm = false;
    std::string m_PlatformToDelete;
};

}  // namespace DekiEditor
