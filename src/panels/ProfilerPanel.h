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

enum class ProfilerViewMode
{
    Hierarchy,  // tree of the call hierarchy
    Flat,       // flat list of totals, sorted by time
    FlameGraph,
    Memory  // memory allocation timeline
};

// One frame's numbers, copied for drawing the stacked timeline bars.
struct DisplayFrameData
{
    uint64_t frameNumber;
    double activeTimeMs;
    double totalTimeMs;
    std::map<std::string, double> categoryTimeMs;  // one stacked segment each
};

// Unity-style profiler panel: a frame timeline and a breakdown of the
// selected frame.
class ProfilerPanel : public IPanel
{
public:
    const char* Id() const override { return "Profiler"; }
    ProfilerPanel(EditorApp* app);
    ~ProfilerPanel() = default;

    void Draw();

    // Called by EditorApp
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

    // Flame graph
    uint32_t GetColorForEvent(const ProfileEvent* event, int depth);

    EditorApp* m_App;
    bool m_IsPaused = false;
    int m_SelectedFrame = -1;  // -1 = none; shows the latest frame

    ProfilerViewMode m_ViewMode = ProfilerViewMode::Hierarchy;
    std::string m_SearchFilter;

    // Flame graph state
    float m_FlameGraphZoom = 1.0f;
    float m_FlameGraphScrollX = 0.0f;
    const ProfileEvent* m_HoveredEvent = nullptr;

    // The displayed data refreshes once a second, in one batch
    std::chrono::steady_clock::time_point m_LastRefreshTime;
    static constexpr float kRefreshIntervalSeconds = 1.0f;
    std::vector<DisplayFrameData> m_DisplayBuffer;
    uint64_t m_CurrentFrameNumber = 0;  // updates only with each batch
};

}  // namespace DekiEditor
