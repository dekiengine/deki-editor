#pragma once

#include "IPanel.h"

#include <string>
#include <vector>
#include <map>
#include <chrono>

namespace DekiEditor
{

class EditorApp;
struct ProfileEvent;

// View modes for the profiler
enum class ProfilerViewMode
{
    Hierarchy,   // Tree view of call hierarchy
    Flat,        // Aggregated flat list sorted by time
    FlameGraph,  // Visual flame graph
    Memory       // Memory allocation timeline
};

// Display buffer frame data - snapshot for stacked bar rendering
struct DisplayFrameData
{
    uint64_t frameNumber;
    double activeTimeMs;
    double totalTimeMs;
    std::map<std::string, double> categoryTimeMs;  // For stacked bars
};

// Unity-style profiler panel with timeline and hierarchical breakdown
class ProfilerPanel : public IPanel
{
public:
    const char* Id() const override { return "Profiler"; }
    ProfilerPanel(EditorApp* app);
    ~ProfilerPanel() = default;

    void Draw();

    // UI state

    // Callbacks from EditorApp
    void OnPlayModePaused();
    void OnPlayModeResumed();

private:
    void DrawToolbar(float frameBudgetMs, int targetFps);
    void DrawFrameTimeline(float frameBudgetMs);
    void DrawHierarchyView(const ProfileEvent* root, double activeTimeMs);
    void DrawFlatView(const ProfileEvent* root, double activeTimeMs);
    void DrawFlameGraph(const ProfileEvent* root, double activeTimeMs);
    void DrawMemoryView();
    void RefreshDisplayBuffer();

    // Flame graph helpers
    uint32_t GetColorForEvent(const ProfileEvent* event, int depth);

    EditorApp* m_App;
    bool m_IsPaused = false;
    int m_SelectedFrame = -1;  // -1 = no selection, shows latest frame

    // View settings
    ProfilerViewMode m_ViewMode = ProfilerViewMode::Hierarchy;
    std::string m_SearchFilter;

    // Flame graph state
    float m_FlameGraphZoom = 1.0f;
    float m_FlameGraphScrollX = 0.0f;
    const ProfileEvent* m_HoveredEvent = nullptr;

    // 1 Hz UI refresh batching
    std::chrono::steady_clock::time_point m_LastRefreshTime;
    static constexpr float REFRESH_INTERVAL_SECONDS = 1.0f;
    std::vector<DisplayFrameData> m_DisplayBuffer;
    uint64_t m_CurrentFrameNumber = 0;  // Cached frame number, only updates with batch
};

}  // namespace DekiEditor
