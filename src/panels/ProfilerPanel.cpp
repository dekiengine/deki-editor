#include "ProfilerPanel.h"
#include "inspector/InspectorUtils.h"
#include "../PanelMaximize.h"
#include "EditorApp.h"
#include "services/ProfilingService.h"
#include "core/Project.h"
#include "imgui.h"
#include <deki-editor/EditorUI.h>
#include <deki-editor/Localization.h>
#include <vector>
#include <map>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace DekiEditor
{

static bool ContainsIgnoreCase(const std::string& str, const std::string& search)
{
    if (search.empty())
    {
        return true;
    }
    auto it = std::search(str.begin(), str.end(), search.begin(), search.end(),
                          [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    return it != str.end();
}

// True when the event or any of its descendants matches the search filter.
static bool EventMatchesFilter(const ProfileEvent* event, const std::string& filter)
{
    if (filter.empty())
    {
        return true;
    }
    if (ContainsIgnoreCase(event->name, filter))
    {
        return true;
    }
    for (const auto& child : event->children)
    {
        if (EventMatchesFilter(child.get(), filter))
        {
            return true;
        }
    }
    return false;
}

// A fixed colour per category for the stacked timeline bars.
static ImU32 GetCategoryColor(const std::string& category)
{
    static const std::map<std::string, ImU32> kCategoryColors = {
        { "rendering", IM_COL32(65, 150, 220, 255) },   // blue
        { "lifecycle", IM_COL32(100, 200, 100, 255) },  // green
        { "components", IM_COL32(220, 160, 50, 255) },  // orange
        { "playmode", IM_COL32(200, 100, 200, 255) },   // purple
        { "editor", IM_COL32(150, 150, 150, 255) },     // grey
        { "deep", IM_COL32(200, 200, 100, 255) },       // yellow
    };

    auto it = kCategoryColors.find(category);
    if (it != kCategoryColors.end())
    {
        return it->second;
    }
    return IM_COL32(100, 100, 100, 255);  // unknown category: dark grey
}

// Adds each event's self time (its time minus its children's) to its
// category's total, for the whole tree. Idle time is left out.
static void AggregateCategoryTimes(const ProfileEvent* event, std::map<std::string, double>& categoryTimes)
{
    if (!event || event->isIdleTime)
    {
        return;
    }

    double eventMs = event->durationUs / 1000.0;
    double childrenMs = 0.0;
    for (const auto& child : event->children)
    {
        if (!child->isIdleTime)
        {
            childrenMs += child->durationUs / 1000.0;
        }
    }

    double selfMs = eventMs - childrenMs;
    if (selfMs > 0 && !event->category.empty())
    {
        categoryTimes[event->category] += selfMs;
    }

    for (const auto& child : event->children)
    {
        AggregateCategoryTimes(child.get(), categoryTimes);
    }
}

// One row of the flat view: all events with the same name, summed.
struct AggregatedEvent
{
    std::string name;
    double totalMs = 0.0;
    double selfMs = 0.0;
    int calls = 0;
};

// Sums every event in the tree into `aggregated`, keyed by name.
static void CollectAllEvents(const ProfileEvent* event, std::map<std::string, AggregatedEvent>& aggregated)
{
    if (!event || event->name.empty())
    {
        return;
    }

    double eventMs = event->durationUs / 1000.0;
    double childrenMs = 0.0;
    for (const auto& child : event->children)
    {
        childrenMs += child->durationUs / 1000.0;
    }
    double selfMs = eventMs - childrenMs;

    auto& agg = aggregated[event->name];
    agg.name = event->name;
    agg.totalMs += eventMs;
    agg.selfMs += selfMs;
    agg.calls++;

    for (const auto& child : event->children)
    {
        CollectAllEvents(child.get(), aggregated);
    }
}

static double GetEventTotalMs(const ProfileEvent* event)
{
    return event->durationUs / 1000.0;
}

// The event's time minus its children's, never below zero.
static double GetEventSelfMs(const ProfileEvent* event)
{
    double eventMs = event->durationUs / 1000.0;
    double childrenMs = 0.0;
    for (const auto& child : event->children)
    {
        childrenMs += child->durationUs / 1000.0;
    }
    double selfMs = eventMs - childrenMs;
    return selfMs > 0.0 ? selfMs : 0.0;
}

// The event's share of the frame's active time, in percent.
static float GetEventPercent(const ProfileEvent* event, double frameActiveMs)
{
    double eventMs = event->durationUs / 1000.0;
    return frameActiveMs > 0 ? (float)(eventMs / frameActiveMs * 100.0) : 0.0f;
}

// The hierarchy view's sort column and direction
static int s_HierarchySortColumn = 1;          // starts on Total (ms)
static bool s_HierarchySortAscending = false;  // starts descending

// Orders two events by the hierarchy view's current sort settings.
static bool CompareEvents(const ProfileEvent* a, const ProfileEvent* b, double frameActiveMs)
{
    switch (s_HierarchySortColumn)
    {
        case 0: return s_HierarchySortAscending ? a->name < b->name : b->name < a->name;
        case 1:
            return s_HierarchySortAscending ? GetEventTotalMs(a) < GetEventTotalMs(b)
                                            : GetEventTotalMs(b) < GetEventTotalMs(a);
        case 2:
            return s_HierarchySortAscending ? GetEventSelfMs(a) < GetEventSelfMs(b)
                                            : GetEventSelfMs(b) < GetEventSelfMs(a);
        case 3:
            return s_HierarchySortAscending ? GetEventPercent(a, frameActiveMs) < GetEventPercent(b, frameActiveMs)
                                            : GetEventPercent(b, frameActiveMs) < GetEventPercent(a, frameActiveMs);
        default: return false;
    }
}

// A table cell's number, right-aligned like a spreadsheet's.
static void RightAlignedCell(const char* text)
{
    const float w = ImGui::CalcTextSize(text).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w)
    {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    }
    ImGui::TextUnformatted(text);
}

// The share of the frame: a faint accent bar across the cell, the value on it.
static void PercentCell(float percent)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetTextLineHeight();
    ImVec4 c = DekiEditor::Palette::Accent;
    c.w = 0.22f;
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w * std::clamp(percent / 100.0f, 0.0f, 1.0f), p.y + h),
                                              ImGui::ColorConvertFloat4ToU32(c), 2.0f);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f%%", percent);
    RightAlignedCell(buf);
}

// Draws the event and its open descendants, one table row each. Returns false
// when the event was skipped (no name, or filtered out).
static bool DrawEventTree(const ProfileEvent* event, double frameActiveMs, const std::string& searchFilter)
{
    if (!event || event->name.empty())
    {
        return false;
    }

    if (!searchFilter.empty() && !EventMatchesFilter(event, searchFilter))
    {
        return false;
    }

    double eventMs = event->durationUs / 1000.0;
    float percent = frameActiveMs > 0 ? (float)(eventMs / frameActiveMs * 100.0) : 0.0f;
    bool hasChildren = !event->children.empty();

    // Self time: total minus children
    double childrenMs = 0.0;
    for (const auto& child : event->children)
    {
        childrenMs += child->durationUs / 1000.0;
    }
    double selfMs = eventMs - childrenMs;
    if (selfMs < 0.0)
    {
        selfMs = 0.0;
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!hasChildren)
    {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (!searchFilter.empty())
    {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }

    ImGui::TableNextRow();

    // Name column, holding the tree node
    ImGui::TableNextColumn();
    ImGui::PushID(event);
    bool open = ImGui::TreeNodeEx("##event", flags, "%s", event->name.c_str());

    char num[32];
    // Total ms column
    ImGui::TableNextColumn();
    std::snprintf(num, sizeof(num), "%.3f", eventMs);
    RightAlignedCell(num);

    // Self ms column
    ImGui::TableNextColumn();
    if (hasChildren && selfMs > 0.001)
    {
        std::snprintf(num, sizeof(num), "%.3f", selfMs);
    }
    else
    {
        std::snprintf(num, sizeof(num), "-");
    }
    ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Dim);
    RightAlignedCell(num);
    ImGui::PopStyleColor();

    // Percent column
    ImGui::TableNextColumn();
    PercentCell(percent);

    ImGui::PopID();

    if (hasChildren && open)
    {
        // Children in the current sort order
        std::vector<const ProfileEvent*> sortedChildren;
        for (const auto& child : event->children)
        {
            sortedChildren.push_back(child.get());
        }
        std::sort(sortedChildren.begin(), sortedChildren.end(),
                  [frameActiveMs](const ProfileEvent* a, const ProfileEvent* b)
                  { return CompareEvents(a, b, frameActiveMs); });

        for (const auto* child : sortedChildren)
        {
            DrawEventTree(child, frameActiveMs, searchFilter);
        }
        ImGui::TreePop();
    }

    return true;
}

ProfilerPanel::ProfilerPanel(EditorApp* app)
    : m_App(app)
{
}

void ProfilerPanel::RefreshDisplayBuffer()
{
    auto& profiler = ProfilingService::Get();
    const auto& history = profiler.GetFrameHistory();

    m_DisplayBuffer.clear();
    size_t numFrames = std::min(history.size(), size_t(100));
    m_DisplayBuffer.reserve(numFrames);

    // The most recent 100 frames
    size_t startIdx = history.size() > 100 ? history.size() - 100 : 0;
    for (size_t i = startIdx; i < history.size(); i++)
    {
        const auto& frame = history[i];
        DisplayFrameData displayFrame;
        displayFrame.frameNumber = frame.frameNumber;
        displayFrame.totalTimeMs = frame.totalTimeMs;
        displayFrame.activeTimeMs = frame.totalTimeMs - frame.idleTimeMs;

        if (frame.rootEvent)
        {
            AggregateCategoryTimes(frame.rootEvent.get(), displayFrame.categoryTimeMs);
        }

        m_DisplayBuffer.push_back(std::move(displayFrame));
    }

    if (!m_DisplayBuffer.empty())
    {
        m_CurrentFrameNumber = m_DisplayBuffer.back().frameNumber;
    }
}

void ProfilerPanel::Draw()
{
    if (!isOpen)
    {
        return;
    }

    DekiEditor::FirstUseWindowPlacement(m_App->Dpi(900.0f), m_App->Dpi(650.0f));

    if (ImGui::Begin(TrLabel("PROFILER###Profiler"), &isOpen, DekiEditor::MaximizedWindowFlags("PROFILER###Profiler")))
    {
        auto& profiler = ProfilingService::Get();

        // Frame budget from the project's Core.targetFps
        int targetFps = 60;
        if (auto* core = DekiEditor::GetCoreSettings(); core && core->targetFps > 0)
        {
            targetFps = core->targetFps;
        }
        const float frameBudgetMs = 1000.0f / targetFps;

        // The display buffer refreshes once a second
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - m_LastRefreshTime).count();
        if (elapsed >= kRefreshIntervalSeconds || m_DisplayBuffer.empty())
        {
            RefreshDisplayBuffer();
            m_LastRefreshTime = now;
        }

        DrawToolbar(frameBudgetMs, targetFps);

        const auto& history = profiler.GetFrameHistory();
        if (history.empty() || m_DisplayBuffer.empty())
        {
            DekiEditor::EmptyStateText(profiler.IsRecording()
                                           ? Tr("Waiting for the first frame...")
                                           : Tr("Nothing recorded yet. Press Play: the profiler records every frame of "
                                                "the game while it runs."));
        }
        else
        {
            DrawFrameTimeline(frameBudgetMs);

            // The frame shown below: the selected one, or the newest.
            const int frameIdx = m_SelectedFrame >= 0 && m_SelectedFrame < (int)m_DisplayBuffer.size()
                                     ? m_SelectedFrame
                                     : (int)m_DisplayBuffer.size() - 1;
            const uint64_t targetFrameNumber =
                (m_SelectedFrame < 0) ? m_CurrentFrameNumber : m_DisplayBuffer[frameIdx].frameNumber;
            const FrameData* displayFrame = nullptr;
            for (auto it = history.rbegin(); it != history.rend(); ++it)  // newest first
            {
                if (it->frameNumber == targetFrameNumber)
                {
                    displayFrame = &(*it);
                    break;
                }
            }

            if (displayFrame)
            {
                const auto& frame = *displayFrame;
                const double activeTimeMs = frame.totalTimeMs - frame.idleTimeMs;
                const float fps = activeTimeMs > 0 ? (float)(1000.0 / activeTimeMs) : 0.0f;

                // One summary line: which frame, and its time against the budget.
                const float pad = m_App->Dpi(12.0f);
                DekiEditor::FullBleedSeparator();
                ImGui::Dummy(ImVec2(0.0f, m_App->Dpi(6.0f)));
                ImGui::Indent(pad);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(DekiEditor::Palette::Dim, "%s",
                                   m_SelectedFrame < 0 ? Tr("Latest frame") : Tr("Frame"));
                ImGui::SameLine(0.0f, m_App->Dpi(8.0f));
                ImGui::Text("#%llu", (unsigned long long)frame.frameNumber);
                ImGui::SameLine(0.0f, m_App->Dpi(20.0f));
                const ImVec4 timeCol = activeTimeMs <= frameBudgetMs          ? DekiEditor::Palette::Green
                                       : activeTimeMs <= frameBudgetMs * 2.0f ? DekiEditor::Palette::Amber
                                                                              : DekiEditor::Palette::Red;
                ImGui::TextColored(timeCol, "%.2f ms", activeTimeMs);
                ImGui::SameLine(0.0f, m_App->Dpi(10.0f));
                ImGui::TextColored(DekiEditor::Palette::Dim, "%.0f fps", fps);
                if (frame.idleTimeMs > 0.1)
                {
                    ImGui::SameLine(0.0f, m_App->Dpi(10.0f));
                    ImGui::TextColored(DekiEditor::Palette::Dim, Tr("+%.2f ms idle"), frame.idleTimeMs);
                }
                ImGui::Unindent(pad);
                ImGui::Dummy(ImVec2(0.0f, m_App->Dpi(6.0f)));

                // View switch, in the Asset Browser's tab-link style
                auto& ui = EditorUI::Get();
                ui.BeginTabLinks();
                if (ui.TabLink(Tr("Hierarchy"), m_ViewMode == ProfilerViewMode::Hierarchy))
                {
                    m_ViewMode = ProfilerViewMode::Hierarchy;
                }
                if (ui.TabLink(Tr("Flat"), m_ViewMode == ProfilerViewMode::Flat))
                {
                    m_ViewMode = ProfilerViewMode::Flat;
                }
                if (ui.TabLink(Tr("Flame Graph"), m_ViewMode == ProfilerViewMode::FlameGraph))
                {
                    m_ViewMode = ProfilerViewMode::FlameGraph;
                }
                if (ui.TabLink(Tr("Memory"), m_ViewMode == ProfilerViewMode::Memory))
                {
                    m_ViewMode = ProfilerViewMode::Memory;
                }
                ui.EndTabLinks();

                // Search, for the views that list events
                if (m_ViewMode == ProfilerViewMode::Hierarchy || m_ViewMode == ProfilerViewMode::Flat)
                {
                    ImGui::Dummy(ImVec2(0.0f, m_App->Dpi(6.0f)));
                    ImGui::Indent(pad);
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - pad);
                    char searchBuf[256] = {};
                    strncpy(searchBuf, m_SearchFilter.c_str(), sizeof(searchBuf) - 1);
                    if (ImGui::InputTextWithHint("##search", Tr("Search events"), searchBuf, sizeof(searchBuf)))
                    {
                        m_SearchFilter = searchBuf;
                    }
                    ImGui::Unindent(pad);
                    ImGui::Dummy(ImVec2(0.0f, m_App->Dpi(6.0f)));
                }

                if (ImGui::BeginChild("ProfilerContent", ImVec2(-1, -1), ImGuiChildFlags_None))
                {
                    if (m_ViewMode == ProfilerViewMode::Memory)
                    {
                        DrawMemoryView();  // needs no event tree
                    }
                    else if (frame.rootEvent)
                    {
                        switch (m_ViewMode)
                        {
                            case ProfilerViewMode::Hierarchy:
                                DrawHierarchyView(frame.rootEvent.get(), activeTimeMs);
                                break;
                            case ProfilerViewMode::Flat: DrawFlatView(frame.rootEvent.get(), activeTimeMs); break;
                            case ProfilerViewMode::FlameGraph:
                                DrawFlameGraph(frame.rootEvent.get(), activeTimeMs);
                                break;
                            case ProfilerViewMode::Memory: break;
                        }
                    }
                    else
                    {
                        DekiEditor::EmptyStateText(Tr("No breakdown was recorded for this frame."));
                    }
                }
                ImGui::EndChild();
            }
        }
    }

    ImGui::End();
}

void ProfilerPanel::DrawToolbar(float frameBudgetMs, int targetFps)
{
    auto& ui = EditorUI::Get();
    auto& profiler = ProfilingService::Get();
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const float rowW = ImGui::GetContentRegionAvail().x;
    const float rowH = m_App->Dpi(34.0f);  // the toolbar strip's height

    ui.BeginToolbar();
    // Back to the newest frame, recording again if the game is running.
    if (ui.ToolbarButton(Tr("Latest"), m_SelectedFrame >= 0))
    {
        m_SelectedFrame = -1;
        if (m_App->IsPlaying() && !m_IsPaused)
        {
            profiler.StartRecording();
        }
    }
    if (ui.ToolbarButton(Tr("Clear"), !profiler.GetFrameHistory().empty()))
    {
        profiler.ClearHistory();
        m_DisplayBuffer.clear();
        m_SelectedFrame = -1;
        m_CurrentFrameNumber = 0;
    }
    ui.EndToolbar();

    // State and budget, right-aligned on the strip.
    char budget[64];
    std::snprintf(budget, sizeof(budget), Tr("Budget %.2f ms  %d fps"), frameBudgetMs, targetFps);
    const char* state = profiler.IsRecording() ? Tr("Recording")
                        : m_SelectedFrame >= 0 ? Tr("Inspecting a frame")
                                               : Tr("Records in Play mode");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = m_App->Dpi(12.0f);
    const float lineH = ImGui::GetTextLineHeight();
    const float y = rowMin.y + (rowH - lineH) * 0.5f;
    const float budgetW = ImGui::CalcTextSize(budget).x;
    const float stateW = ImGui::CalcTextSize(state).x;
    float x = rowMin.x + rowW - pad - budgetW;
    dl->AddText(ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Dim), budget);
    x -= m_App->Dpi(24.0f) + stateW;
    const bool recording = profiler.IsRecording();
    dl->AddText(ImVec2(x, y),
                ImGui::ColorConvertFloat4ToU32(recording ? DekiEditor::Palette::Green : DekiEditor::Palette::Dim),
                state);
    if (recording)
    {
        dl->AddCircleFilled(ImVec2(x - m_App->Dpi(9.0f), y + lineH * 0.5f), m_App->Dpi(3.5f),
                            ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Green));
    }
}

void ProfilerPanel::DrawFrameTimeline(float frameBudgetMs)
{
    auto& profiler = ProfilingService::Get();
    const float pad = m_App->Dpi(12.0f);
    ImGui::Dummy(ImVec2(0.0f, m_App->Dpi(8.0f)));
    ImGui::Indent(pad);

    // Chart: a fixed 100 slots, newest at the right, so bars do not change
    // width as the history fills. The scale is two budgets tall.
    const int kSlots = 100;
    const float fullWidth = ImGui::GetContentRegionAvail().x - pad;
    const float labelW = m_App->Dpi(56.0f);
    const float chartH = m_App->Dpi(110.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 chartMin(origin.x + labelW, origin.y);
    const ImVec2 chartMax(origin.x + fullWidth, origin.y + chartH);
    const float chartW = chartMax.x - chartMin.x;
    const float slotW = chartW / kSlots;
    const float maxMs = frameBudgetMs * 2.0f;
    const int count = (int)m_DisplayBuffer.size();
    const int firstSlot = kSlots - std::min(count, kSlots);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 dim = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Dim);
    const ImU32 line = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Line2);
    dl->AddRectFilled(chartMin, chartMax, IM_COL32(255, 255, 255, 6), m_App->Dpi(3.0f));

    // Gridlines at the budget and at twice it, labelled in ms.
    auto yFor = [&](double ms) { return chartMax.y - (float)std::min(ms / maxMs, 1.0) * chartH; };
    const float halfLine = ImGui::GetTextLineHeight() * 0.5f;
    for (float ms : { frameBudgetMs, maxMs })
    {
        const float y = yFor(ms);
        dl->AddLine(ImVec2(chartMin.x, y), ImVec2(chartMax.x, y), line, 1.0f);
        char label[24];
        std::snprintf(label, sizeof(label), "%.1f ms", ms);
        const float w = ImGui::CalcTextSize(label).x;
        dl->AddText(ImVec2(chartMin.x - w - m_App->Dpi(8.0f), std::max(origin.y, y - halfLine)), dim, label);
    }

    // The bar under the mouse, -1 for none
    const ImVec2 mouse = ImGui::GetMousePos();
    int hovered = -1;
    if (ImGui::IsWindowHovered() && mouse.x >= chartMin.x && mouse.x < chartMax.x && mouse.y >= chartMin.y &&
        mouse.y < chartMax.y)
    {
        const int slot = (int)((mouse.x - chartMin.x) / slotW);
        if (slot >= firstSlot)
        {
            hovered = slot - firstSlot;
        }
    }

    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Accent);
    const ImU32 over = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Red);
    for (int i = 0; i < count; ++i)
    {
        const auto& frame = m_DisplayBuffer[i];
        const float x0 = chartMin.x + (firstSlot + i) * slotW;
        const float x1 = x0 + std::max(1.0f, slotW - 1.0f);
        const bool selected = i == m_SelectedFrame;
        if (selected || i == hovered)
        {
            dl->AddRectFilled(ImVec2(x0, chartMin.y), ImVec2(x1, chartMax.y),
                              selected ? IM_COL32(255, 255, 255, 18) : IM_COL32(255, 255, 255, 10));
        }

        if (frame.categoryTimeMs.empty() || selected)
        {
            dl->AddRectFilled(ImVec2(x0, yFor(frame.activeTimeMs)), ImVec2(x1, chartMax.y),
                              selected ? accent : IM_COL32(120, 120, 120, 255));
        }
        else
        {
            // Stacked by category
            float yOffset = chartMax.y;
            for (const auto& [category, timeMs] : frame.categoryTimeMs)
            {
                if (timeMs <= 0)
                {
                    continue;
                }
                const float h = std::min((float)(timeMs / maxMs) * chartH, yOffset - chartMin.y);
                if (h > 0.5f)
                {
                    dl->AddRectFilled(ImVec2(x0, yOffset - h), ImVec2(x1, yOffset), GetCategoryColor(category));
                    yOffset -= h;
                }
            }
        }
        // Over budget: a red cap on the bar
        if (frame.activeTimeMs > frameBudgetMs)
        {
            const float top = yFor(frame.activeTimeMs);
            dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, top + m_App->Dpi(2.0f)), over);
        }
    }

    // A click selects a frame and stops recording, so the frame stays put
    ImGui::InvisibleButton("FrameTimeline", ImVec2(fullWidth, chartH));
    if (ImGui::IsItemClicked() && hovered >= 0 && hovered != m_SelectedFrame)
    {
        m_SelectedFrame = hovered;
        profiler.StopRecording();
    }
    if (hovered >= 0 && hovered < count)
    {
        const auto& f = m_DisplayBuffer[hovered];
        char tip[96];
        std::snprintf(tip, sizeof(tip), Tr("Frame #%llu\n%.2f ms  (%.0f fps)"), (unsigned long long)f.frameNumber,
                      f.activeTimeMs, f.activeTimeMs > 0 ? 1000.0 / f.activeTimeMs : 0.0);
        DekiEditor::Tooltip("%s", tip);
    }

    // Legend, under the bars
    ImGui::SetCursorScreenPos(ImVec2(chartMin.x, ImGui::GetCursorScreenPos().y + m_App->Dpi(6.0f)));
    const float swatch = m_App->Dpi(9.0f);
    const char* categories[] = { TrMark("rendering"), TrMark("lifecycle"), TrMark("components"),
                                 TrMark("playmode"),  TrMark("editor"),    TrMark("deep") };
    for (size_t c = 0; c < IM_ARRAYSIZE(categories); ++c)
    {
        if (c > 0)
        {
            ImGui::SameLine(0.0f, m_App->Dpi(14.0f));
        }
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float y = p.y + (ImGui::GetTextLineHeight() - swatch) * 0.5f;
        dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + swatch, y + swatch), GetCategoryColor(categories[c]), 2.0f);
        ImGui::Dummy(ImVec2(swatch, ImGui::GetTextLineHeight()));
        ImGui::SameLine(0.0f, m_App->Dpi(6.0f));
        ImGui::TextColored(DekiEditor::Palette::Dim, "%s", Tr(categories[c]));
    }
    ImGui::Unindent(pad);
    ImGui::Dummy(ImVec2(0.0f, m_App->Dpi(8.0f)));
}

void ProfilerPanel::DrawHierarchyView(const ProfileEvent* root, double activeTimeMs)
{
    if (ImGui::BeginTable("HierarchyView", 4,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_PadOuterX))
    {
        ImGui::TableSetupColumn(TrLabel("Name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(TrLabel("Total (ms)"),
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
                                    ImGuiTableColumnFlags_PreferSortDescending,
                                m_App->Dpi(90.0f));
        ImGui::TableSetupColumn(TrLabel("Self (ms)"), ImGuiTableColumnFlags_WidthFixed, m_App->Dpi(90.0f));
        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, m_App->Dpi(60.0f));
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsCount > 0)
            {
                s_HierarchySortColumn = sortSpecs->Specs[0].ColumnIndex;
                s_HierarchySortAscending = sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            }
            sortSpecs->SpecsDirty = false;
        }

        // Top-level events, idle time left out, in sort order
        std::vector<const ProfileEvent*> sortedChildren;
        for (const auto& child : root->children)
        {
            if (!child->isIdleTime)
            {
                sortedChildren.push_back(child.get());
            }
        }
        std::sort(sortedChildren.begin(), sortedChildren.end(),
                  [activeTimeMs](const ProfileEvent* a, const ProfileEvent* b)
                  { return CompareEvents(a, b, activeTimeMs); });

        for (const auto* child : sortedChildren)
        {
            DrawEventTree(child, activeTimeMs, m_SearchFilter);
        }

        ImGui::EndTable();
    }
}

void ProfilerPanel::DrawFlatView(const ProfileEvent* root, double activeTimeMs)
{
    // Aggregating the whole tree and sorting it is costly, so it is redone
    // only when the capture (root), the filter or the sort order changes.
    static const ProfileEvent* s_FlatRoot = nullptr;
    static std::string s_FlatFilter;
    static std::vector<AggregatedEvent> s_Sorted;
    static int s_SortColumn = -1;
    static bool s_SortAscending = false;
    bool rebuilt = false;
    if (s_FlatRoot != root || s_FlatFilter != m_SearchFilter)
    {
        std::map<std::string, AggregatedEvent> aggregated;
        for (const auto& child : root->children)
        {
            CollectAllEvents(child.get(), aggregated);
        }
        s_Sorted.clear();
        for (auto& pair : aggregated)
        {
            if (m_SearchFilter.empty() || ContainsIgnoreCase(pair.second.name, m_SearchFilter))
            {
                s_Sorted.push_back(pair.second);
            }
        }
        s_FlatRoot = root;
        s_FlatFilter = m_SearchFilter;
        rebuilt = true;
    }

    if (ImGui::BeginTable("FlatView", 4,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX))
    {
        ImGui::TableSetupColumn(TrLabel("Name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(TrLabel("Self (ms)"),
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
                                    ImGuiTableColumnFlags_PreferSortDescending,
                                m_App->Dpi(90.0f));
        ImGui::TableSetupColumn(TrLabel("Total (ms)"), ImGuiTableColumnFlags_WidthFixed, m_App->Dpi(90.0f));
        ImGui::TableSetupColumn(TrLabel("Calls"), ImGuiTableColumnFlags_WidthFixed, m_App->Dpi(60.0f));
        ImGui::TableHeadersRow();

        // Sort when the data was rebuilt or the sort spec changed.
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsCount > 0 &&
                (rebuilt || sortSpecs->SpecsDirty || s_SortColumn != sortSpecs->Specs[0].ColumnIndex ||
                 s_SortAscending != (sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending)))
            {
                auto& spec = sortSpecs->Specs[0];
                bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
                s_SortColumn = spec.ColumnIndex;
                s_SortAscending = ascending;
                std::sort(s_Sorted.begin(), s_Sorted.end(),
                          [&](const AggregatedEvent& a, const AggregatedEvent& b)
                          {
                              switch (spec.ColumnIndex)
                              {
                                  case 0: return ascending ? a.name < b.name : b.name < a.name;
                                  case 1: return ascending ? a.selfMs < b.selfMs : b.selfMs < a.selfMs;
                                  case 2: return ascending ? a.totalMs < b.totalMs : b.totalMs < a.totalMs;
                                  case 3: return ascending ? a.calls < b.calls : b.calls < a.calls;
                                  default: return false;
                              }
                          });
            }
            sortSpecs->SpecsDirty = false;
        }

        for (const auto& evt : s_Sorted)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", evt.name.c_str());
            char num[32];
            ImGui::TableNextColumn();
            std::snprintf(num, sizeof(num), "%.3f", evt.selfMs);
            RightAlignedCell(num);
            ImGui::TableNextColumn();
            std::snprintf(num, sizeof(num), "%.3f", evt.totalMs);
            ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Dim);
            RightAlignedCell(num);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            std::snprintf(num, sizeof(num), "%d", evt.calls);
            RightAlignedCell(num);
        }

        ImGui::EndTable();
    }
}

void ProfilerPanel::OnPlayModePaused()
{
    m_IsPaused = true;
}

void ProfilerPanel::OnPlayModeResumed()
{
    m_IsPaused = false;
    m_SelectedFrame = -1;  // follow the latest frame again
}

}  // namespace DekiEditor
