#pragma once

#include "IPanel.h"
#include "services/MemoryReport.h"
#include "services/SceneMemoryEstimate.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace DekiEditor
{

class EditorApp;

// Where memory goes. Two views:
//  - Scene: what the open scene would take on a target, worked out in the
//    editor (SceneMemoryEstimate), against that target's RAM. Estimates.
//  - Live: what a running game holds, by call site and grouped into
//    categories, with peaks (MemoryReport), from Play mode or from a board
//    over its serial port. Measured.
//
// A panel of its own rather than a Profiler tab: the Scene view needs nothing
// running, and both need room for a table sorted by size.
class MemoryPanel : public IPanel
{
public:
    const char* Id() const override { return "Memory"; }
    MemoryPanel(EditorApp* app);
    ~MemoryPanel() override;

    void Draw() override;

    /// Shows the Live view, as the Profiler's Memory tab asks.
    void ShowLive() { m_View = View::Live; }

    /// The last report read from a board, or null. Its internal RAM size is
    /// what the Scene view measures a target against.
    const MemoryReport* LastDeviceReport() const { return m_HasDeviceReport ? &m_DeviceReport : nullptr; }

private:
    enum class View
    {
        Scene,
        Live
    };

    void DrawSceneView();
    void DrawLiveView();
    void DrawRegionBar(const char* label, uint64_t used, uint64_t budget, bool measured);
    void RefreshEstimate();
    void RefreshLive(bool force);
    void StartDeviceQuery(const std::string& port);

    EditorApp* m_App;
    View m_View = View::Scene;

    // Scene view
    std::string m_TargetId;  // "" is the editor's own desktop
    SceneMemoryEstimate m_Estimate;
    bool m_EstimateValid = false;
    const void* m_EstimateScene = nullptr;
    std::chrono::steady_clock::time_point m_EstimateTime;

    // Live view
    int m_Source = 0;  // 0 is Play mode; 1.. a serial port in m_Ports
    std::vector<std::string> m_Ports;
    bool m_SincePlayStart = true;
    bool m_Auto = true;
    MemoryReport m_Live;
    bool m_HasLive = false;
    std::string m_LiveError;
    std::chrono::steady_clock::time_point m_LiveTime;
    std::vector<std::string> m_OpenCategories;

    MemoryReport m_DeviceReport;
    bool m_HasDeviceReport = false;
    std::string m_DevicePort;

    struct DeviceJob;
    std::shared_ptr<DeviceJob> m_Job;
};

}  // namespace DekiEditor
