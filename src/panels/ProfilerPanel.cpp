#include "ProfilerPanel.h"
#include "inspector/InspectorUtils.h"
#include "../PanelMaximize.h"
#include "EditorApp.h"
#include "services/ProfilingService.h"
#include "core/Project.h"
#include "imgui.h"
#include <deki-editor/EditorUI.h>
#include <vector>
#include <map>
#include <algorithm>
#include <cctype>
#include <cmath>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace DekiEditor
{

// Case-insensitive string search
static bool ContainsIgnoreCase(const std::string& str, const std::string& search)
{
    if (search.empty()) return true;
    auto it = std::search(str.begin(), str.end(), search.begin(), search.end(),
                          [](char a, char b)
                          { return std::tolower(a) == std::tolower(b); });
    return it != str.end();
}

// Check if event or any children match the search filter
static bool EventMatchesFilter(const ProfileEvent* event, const std::string& filter)
{
    if (filter.empty()) return true;
    if (ContainsIgnoreCase(event->name, filter)) return true;
    for (const auto& child : event->children)
    {
        if (EventMatchesFilter(child.get(), filter)) return true;
    }
    return false;
}

// Category colors for stacked bar chart - consistent across frames
static ImU32 GetCategoryColor(const std::string& category)
{
    static const std::map<std::string, ImU32> s_CategoryColors = {
        { "rendering", IM_COL32(65, 150, 220, 255) },   // Blue
        { "lifecycle", IM_COL32(100, 200, 100, 255) },  // Green
        { "components", IM_COL32(220, 160, 50, 255) },  // Orange
        { "playmode", IM_COL32(200, 100, 200, 255) },   // Purple
        { "editor", IM_COL32(150, 150, 150, 255) },     // Gray
        { "deep", IM_COL32(200, 200, 100, 255) },       // Yellow
    };

    auto it = s_CategoryColors.find(category);
    if (it != s_CategoryColors.end())
        return it->second;
    return IM_COL32(100, 100, 100, 255);  // Default gray
}

// Recursively aggregate category times from event tree
static void AggregateCategoryTimes(const ProfileEvent* event,
                                   std::map<std::string, double>& categoryTimes)
{
    if (!event || event->isIdleTime)
        return;

    double eventMs = event->durationUs / 1000.0;
    double childrenMs = 0.0;
    for (const auto& child : event->children)
    {
        if (!child->isIdleTime)
            childrenMs += child->durationUs / 1000.0;
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

// Aggregated stats for flat view
struct AggregatedEvent
{
    std::string name;
    double totalMs = 0.0;
    double selfMs = 0.0;
    int calls = 0;
};

// Recursively collect all events for flat view aggregation
static void CollectAllEvents(const ProfileEvent* event, std::map<std::string, AggregatedEvent>& aggregated)
{
    if (!event || event->name.empty()) return;

    double eventMs = event->durationUs / 1000.0;
    double childrenMs = 0.0;
    for (const auto& child : event->children)
        childrenMs += child->durationUs / 1000.0;
    double selfMs = eventMs - childrenMs;

    auto& agg = aggregated[event->name];
    agg.name = event->name;
    agg.totalMs += eventMs;
    agg.selfMs += selfMs;
    agg.calls++;

    for (const auto& child : event->children)
        CollectAllEvents(child.get(), aggregated);
}

// Helper to get total ms for an event
static double GetEventTotalMs(const ProfileEvent* event)
{
    return event->durationUs / 1000.0;
}

// Helper to get self ms for an event
static double GetEventSelfMs(const ProfileEvent* event)
{
    double eventMs = event->durationUs / 1000.0;
    double childrenMs = 0.0;
    for (const auto& child : event->children)
        childrenMs += child->durationUs / 1000.0;
    double selfMs = eventMs - childrenMs;
    return selfMs > 0.0 ? selfMs : 0.0;
}

// Helper to get percent for an event
static float GetEventPercent(const ProfileEvent* event, double frameActiveMs)
{
    double eventMs = event->durationUs / 1000.0;
    return frameActiveMs > 0 ? (float)(eventMs / frameActiveMs * 100.0) : 0.0f;
}

// Sort column/direction for hierarchy view
static int s_HierarchySortColumn = 1;          // Default: Total (ms)
static bool s_HierarchySortAscending = false;  // Default: descending

// Compare two events based on current sort settings
static bool CompareEvents(const ProfileEvent* a, const ProfileEvent* b, double frameActiveMs)
{
    switch (s_HierarchySortColumn)
    {
        case 0: return s_HierarchySortAscending ? a->name < b->name : b->name < a->name;
        case 1: return s_HierarchySortAscending ? GetEventTotalMs(a) < GetEventTotalMs(b) : GetEventTotalMs(b) < GetEventTotalMs(a);
        case 2: return s_HierarchySortAscending ? GetEventSelfMs(a) < GetEventSelfMs(b) : GetEventSelfMs(b) < GetEventSelfMs(a);
        case 3: return s_HierarchySortAscending ? GetEventPercent(a, frameActiveMs) < GetEventPercent(b, frameActiveMs) : GetEventPercent(b, frameActiveMs) < GetEventPercent(a, frameActiveMs);
        default: return false;
    }
}

// Recursive helper to draw profile event tree (table row per event)
static bool DrawEventTree(const ProfileEvent* event, double frameActiveMs,
                          const std::string& searchFilter)
{
    if (!event || event->name.empty())
        return false;

    // Check if this event or children match filter
    if (!searchFilter.empty() && !EventMatchesFilter(event, searchFilter))
        return false;

    double eventMs = event->durationUs / 1000.0;
    float percent = frameActiveMs > 0 ? (float)(eventMs / frameActiveMs * 100.0) : 0.0f;
    bool hasChildren = !event->children.empty();

    // Calculate self time (total - children)
    double childrenMs = 0.0;
    for (const auto& child : event->children)
        childrenMs += child->durationUs / 1000.0;
    double selfMs = eventMs - childrenMs;
    if (selfMs < 0.0) selfMs = 0.0;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!hasChildren)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (!searchFilter.empty())
        flags |= ImGuiTreeNodeFlags_DefaultOpen;

    ImGui::TableNextRow();

    // Name column (with tree node)
    ImGui::TableNextColumn();
    ImGui::PushID(event);
    bool open = ImGui::TreeNodeEx("##event", flags, "%s", event->name.c_str());

    // Total ms column
    ImGui::TableNextColumn();
    ImGui::Text("%.3f", eventMs);

    // Self ms column
    ImGui::TableNextColumn();
    if (hasChildren && selfMs > 0.001)
        ImGui::Text("%.3f", selfMs);
    else
        ImGui::TextUnformatted("-");

    // Percent column
    ImGui::TableNextColumn();
    ImGui::Text("%.1f%%", percent);

    ImGui::PopID();

    if (hasChildren && open)
    {
        // Sort children by current sort column
        std::vector<const ProfileEvent*> sortedChildren;
        for (const auto& child : event->children)
            sortedChildren.push_back(child.get());
        std::sort(sortedChildren.begin(), sortedChildren.end(),
                  [frameActiveMs](const ProfileEvent* a, const ProfileEvent* b)
                  {
                      return CompareEvents(a, b, frameActiveMs);
                  });

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

    // Copy the LAST 100 frames (most recent), not the first 100
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

    // Update cached current frame number (only changes with batch refresh)
    if (!m_DisplayBuffer.empty())
    {
        m_CurrentFrameNumber = m_DisplayBuffer.back().frameNumber;
    }
}

void ProfilerPanel::Draw()
{
    if (!m_Open)
        return;

    DekiEditor::FirstUseWindowPlacement(900, 650);

    if (ImGui::Begin("PROFILER###Profiler", &m_Open, DekiEditor::MaximizedWindowFlags("PROFILER###Profiler")))
    {
        auto& profiler = ProfilingService::Get();

        // Get frame budget from project's Core.targetFps
        int targetFps = 60;
        if (auto* core = DekiEditor::GetCoreSettings(); core && core->targetFps > 0)
            targetFps = core->targetFps;
        float frameBudgetMs = 1000.0f / targetFps;

        // Status and budget as property rows, the way every panel lists facts.
        ImGui::Spacing();
        DekiEditor::BeginPropertyContext();
        InspectorUtils::BeginPropertyRow("Status");
        ImGui::AlignTextToFramePadding();
        if (profiler.IsRecording())
            ImGui::TextColored(DekiEditor::Palette::Green, "Recording");
        else
            ImGui::TextColored(DekiEditor::Palette::Dim, "Records in Play mode");
        InspectorUtils::SetNextRowTooltip("One frame at the project's target frame rate (Project Settings > Core).");
        InspectorUtils::BeginPropertyRow("Frame Budget");
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%.2f ms", frameBudgetMs);
        ImGui::SameLine();
        ImGui::TextColored(DekiEditor::Palette::Dim, "%d fps", targetFps);
        DekiEditor::EndPropertyContext();
        ImGui::Spacing();
        DekiEditor::FullBleedSeparator();

        // Frame history display
        const auto& history = profiler.GetFrameHistory();

        // 1 Hz refresh batching - refresh display buffer once per second
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - m_LastRefreshTime).count();
        if (elapsed >= REFRESH_INTERVAL_SECONDS || m_DisplayBuffer.empty())
        {
            RefreshDisplayBuffer();
            m_LastRefreshTime = now;
        }

        if (history.empty())
        {
            DekiEditor::BeginPropertyContext();
            InspectorUtils::BeginPropertyRow("");
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(DekiEditor::Palette::Dim, "Nothing recorded yet.");
            DekiEditor::EndPropertyContext();
        }
        else
        {
            int frameIdx = m_SelectedFrame >= 0 && m_SelectedFrame < (int)m_DisplayBuffer.size()
                               ? m_SelectedFrame
                               : (int)m_DisplayBuffer.size() - 1;

            // Timeline, padded like a section body; the views below run edge
            // to edge.
            const float pad = m_App->Dpi(12.0f);
            ImGui::Dummy(ImVec2(0.0f, m_App->Dpi(4.0f)));
            ImGui::Indent(pad);
            DekiEditor::SchematicSubHeader("Timeline");
            ImGui::TextColored(DekiEditor::Palette::Dim, "Click a bar to inspect that frame.");
            ImGui::Dummy(ImVec2(0.0f, m_App->Dpi(4.0f)));

            float fullWidth = ImGui::GetContentRegionAvail().x - pad;
            float labelWidth = m_App->Dpi(45.0f);  // Space for FPS labels on Y-axis
            float availWidth = fullWidth - labelWidth;
            float barHeight = m_App->Dpi(120.0f);
            ImVec2 barPos = ImGui::GetCursorScreenPos();
            barPos.x += labelWidth;  // Offset bars to make room for labels

            ImDrawList* drawList = ImGui::GetWindowDrawList();
            float barWidth = m_DisplayBuffer.empty() ? availWidth : availWidth / m_DisplayBuffer.size();
            float maxMs = 33.33f;
            const ImU32 accent = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Accent);

            // Draw stacked bars from display buffer
            for (size_t i = 0; i < m_DisplayBuffer.size(); i++)
            {
                const auto& frame = m_DisplayBuffer[i];
                float totalHeight = std::min((float)(frame.activeTimeMs / maxMs), 1.0f) * barHeight;
                float x = barPos.x + i * barWidth;
                bool isSelected = (int)i == m_SelectedFrame;

                if (frame.categoryTimeMs.empty() || isSelected)
                {
                    // Solid bar, or the selection in the accent
                    ImU32 color = isSelected ? accent : IM_COL32(120, 120, 120, 255);
                    float y = barPos.y + barHeight - totalHeight;
                    drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + barWidth - 1, barPos.y + barHeight), color);
                }
                else
                {
                    // Stacked segments by category
                    float yOffset = barPos.y + barHeight;
                    for (const auto& [category, timeMs] : frame.categoryTimeMs)
                    {
                        if (timeMs <= 0)
                            continue;
                        float segmentHeight = (float)(timeMs / maxMs) * barHeight;
                        segmentHeight = std::min(segmentHeight, yOffset - barPos.y);  // Clamp to available space
                        if (segmentHeight > 0.5f)
                        {
                            drawList->AddRectFilled(ImVec2(x, yOffset - segmentHeight),
                                                    ImVec2(x + barWidth - 1, yOffset), GetCategoryColor(category));
                            yOffset -= segmentHeight;
                        }
                    }
                }
            }

            // Reference lines with FPS labels on Y-axis
            const ImU32 dim = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Dim);
            float labelX = barPos.x - labelWidth;
            float line60fps = barPos.y + barHeight - (16.67f / maxMs) * barHeight;
            float line30fps = barPos.y + barHeight - (33.33f / maxMs) * barHeight;
            drawList->AddLine(ImVec2(barPos.x, line60fps), ImVec2(barPos.x + availWidth, line60fps),
                              IM_COL32(100, 100, 100, 150), 1.0f);
            drawList->AddLine(ImVec2(barPos.x, line30fps), ImVec2(barPos.x + availWidth, line30fps),
                              IM_COL32(100, 100, 100, 150), 1.0f);
            const float halfLine = ImGui::GetTextLineHeight() * 0.5f;
            drawList->AddText(ImVec2(labelX, line60fps - halfLine), dim, "60 fps");
            drawList->AddText(ImVec2(labelX, line30fps - halfLine), dim, "30 fps");

            // Click detection - click to select and pause recording
            ImGui::InvisibleButton("FrameTimeline", ImVec2(fullWidth, barHeight));
            if (ImGui::IsItemClicked())
            {
                float mouseX = ImGui::GetMousePos().x - barPos.x;  // barPos already offset by labelWidth
                int clickedFrame = (int)(mouseX / barWidth);
                if (clickedFrame >= 0 && clickedFrame < (int)m_DisplayBuffer.size() && m_SelectedFrame != clickedFrame)
                {
                    m_SelectedFrame = clickedFrame;
                    profiler.StopRecording();  // Pause recording when inspecting
                }
            }

            // Category legend: a swatch and a dim name each, on one line
            ImGui::Dummy(ImVec2(0.0f, m_App->Dpi(4.0f)));
            const float swatch = m_App->Dpi(10.0f);
            const char* categories[] = { "rendering", "lifecycle", "components", "playmode", "editor", "deep" };
            for (size_t c = 0; c < IM_ARRAYSIZE(categories); ++c)
            {
                if (c > 0)
                    ImGui::SameLine(0.0f, m_App->Dpi(14.0f));
                ImVec2 p = ImGui::GetCursorScreenPos();
                const float y = p.y + (ImGui::GetTextLineHeight() - swatch) * 0.5f;
                drawList->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + swatch, y + swatch), GetCategoryColor(categories[c]), 2.0f);
                ImGui::Dummy(ImVec2(swatch, ImGui::GetTextLineHeight()));
                ImGui::SameLine(0.0f, m_App->Dpi(6.0f));
                ImGui::TextColored(DekiEditor::Palette::Dim, "%s", categories[c]);
            }
            ImGui::Unindent(pad);
            ImGui::Dummy(ImVec2(0.0f, m_App->Dpi(4.0f)));

            // Selected frame info
            // Find frame in history by frame number (not by index, which shifts as history grows)
            const FrameData* displayFrame = nullptr;
            uint64_t targetFrameNumber = (m_SelectedFrame < 0)
                                             ? m_CurrentFrameNumber
                                             : (frameIdx >= 0 && frameIdx < (int)m_DisplayBuffer.size() ? m_DisplayBuffer[frameIdx].frameNumber : 0);

            // Search from end (most recent) for efficiency
            for (auto it = history.rbegin(); it != history.rend(); ++it)
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
                double activeTimeMs = frame.totalTimeMs - frame.idleTimeMs;
                float fps = activeTimeMs > 0 ? (float)(1000.0 / activeTimeMs) : 0.0f;

                DekiEditor::FullBleedSeparator();
                ImGui::Dummy(ImVec2(0.0f, m_App->Dpi(4.0f)));
                DekiEditor::BeginPropertyContext();
                InspectorUtils::BeginPropertyRow(m_SelectedFrame < 0 ? "Latest Frame" : "Frame");
                ImGui::AlignTextToFramePadding();
                ImGui::Text("#%llu", (unsigned long long)frame.frameNumber);
                InspectorUtils::BeginPropertyRow("Time");
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%.2f ms", activeTimeMs);
                ImGui::SameLine();
                ImGui::TextColored(DekiEditor::Palette::Dim, "%.1f fps", fps);
                if (frame.idleTimeMs > 0.1)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(DekiEditor::Palette::Dim, "+%.2f ms idle", frame.idleTimeMs);
                }
                DekiEditor::EndPropertyContext();
                ImGui::Dummy(ImVec2(0.0f, m_App->Dpi(4.0f)));

                // View switch: the Asset Browser's tab links
                auto& ui = EditorUI::Get();
                ui.BeginTabLinks();
                if (ui.TabLink("Hierarchy", m_ViewMode == ProfilerViewMode::Hierarchy))
                    m_ViewMode = ProfilerViewMode::Hierarchy;
                if (ui.TabLink("Flat", m_ViewMode == ProfilerViewMode::Flat))
                    m_ViewMode = ProfilerViewMode::Flat;
                if (ui.TabLink("Flame Graph", m_ViewMode == ProfilerViewMode::FlameGraph))
                    m_ViewMode = ProfilerViewMode::FlameGraph;
                if (ui.TabLink("Memory", m_ViewMode == ProfilerViewMode::Memory))
                    m_ViewMode = ProfilerViewMode::Memory;
                ui.EndTabLinks();

                // Search, for the views that list events
                if (m_ViewMode != ProfilerViewMode::Memory)
                {
                    ImGui::Dummy(ImVec2(0.0f, m_App->Dpi(4.0f)));
                    ImGui::Indent(pad);
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - pad);
                    char searchBuf[256] = {};
                    strncpy(searchBuf, m_SearchFilter.c_str(), sizeof(searchBuf) - 1);
                    if (ImGui::InputTextWithHint("##search", "Search events", searchBuf, sizeof(searchBuf)))
                        m_SearchFilter = searchBuf;
                    ImGui::Unindent(pad);
                    ImGui::Dummy(ImVec2(0.0f, m_App->Dpi(4.0f)));
                }

                // Content area
                if (ImGui::BeginChild("ProfilerContent", ImVec2(-1, -1), ImGuiChildFlags_None))
                {
                    if (m_ViewMode == ProfilerViewMode::Memory)
                    {
                        // Memory view doesn't need root event
                        DrawMemoryView();
                    }
                    else if (frame.rootEvent)
                    {
                        switch (m_ViewMode)
                        {
                            case ProfilerViewMode::Hierarchy:
                                DrawHierarchyView(frame.rootEvent.get(), activeTimeMs);
                                break;
                            case ProfilerViewMode::Flat:
                                DrawFlatView(frame.rootEvent.get(), activeTimeMs);
                                break;
                            case ProfilerViewMode::FlameGraph:
                                DrawFlameGraph(frame.rootEvent.get(), activeTimeMs);
                                break;
                            case ProfilerViewMode::Memory:
                                // Already handled above
                                break;
                        }
                    }
                    else
                    {
                        ImGui::TextDisabled("No hierarchy data for this frame.");
                    }
                }
                ImGui::EndChild();
            }
        }
    }

    ImGui::End();
}

void ProfilerPanel::DrawHierarchyView(const ProfileEvent* root, double activeTimeMs)
{
    if (ImGui::BeginTable("HierarchyView", 4, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Total (ms)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, m_App->Dpi(90.0f));
        ImGui::TableSetupColumn("Self (ms)", ImGuiTableColumnFlags_WidthFixed, m_App->Dpi(90.0f));
        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, m_App->Dpi(60.0f));
        ImGui::TableHeadersRow();

        // Read sort specs
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsCount > 0)
            {
                s_HierarchySortColumn = sortSpecs->Specs[0].ColumnIndex;
                s_HierarchySortAscending = sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            }
            sortSpecs->SpecsDirty = false;
        }

        // Sort root-level children
        std::vector<const ProfileEvent*> sortedChildren;
        for (const auto& child : root->children)
        {
            if (!child->isIdleTime)
                sortedChildren.push_back(child.get());
        }
        std::sort(sortedChildren.begin(), sortedChildren.end(),
                  [activeTimeMs](const ProfileEvent* a, const ProfileEvent* b)
                  {
                      return CompareEvents(a, b, activeTimeMs);
                  });

        for (const auto* child : sortedChildren)
        {
            DrawEventTree(child, activeTimeMs, m_SearchFilter);
        }

        ImGui::EndTable();
    }
}

void ProfilerPanel::DrawFlatView(const ProfileEvent* root, double activeTimeMs)
{
    // Aggregating the whole event tree into a map, copying it into a vector
    // and sorting it happened every frame. It is redone only when the capture
    // (root), the filter or the sort order changed.
    static const ProfileEvent* s_FlatRoot = nullptr;
    static std::string s_FlatFilter;
    static std::vector<AggregatedEvent> sorted;
    static int s_SortColumn = -1;
    static bool s_SortAscending = false;
    bool rebuilt = false;
    if (s_FlatRoot != root || s_FlatFilter != m_SearchFilter)
    {
        std::map<std::string, AggregatedEvent> aggregated;
        for (const auto& child : root->children)
            CollectAllEvents(child.get(), aggregated);
        sorted.clear();
        for (auto& pair : aggregated)
        {
            if (m_SearchFilter.empty() || ContainsIgnoreCase(pair.second.name, m_SearchFilter))
                sorted.push_back(pair.second);
        }
        s_FlatRoot = root;
        s_FlatFilter = m_SearchFilter;
        rebuilt = true;
    }

    // Draw table
    if (ImGui::BeginTable("FlatView", 4, ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Self (ms)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, m_App->Dpi(90.0f));
        ImGui::TableSetupColumn("Total (ms)", ImGuiTableColumnFlags_WidthFixed, m_App->Dpi(90.0f));
        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, m_App->Dpi(60.0f));
        ImGui::TableHeadersRow();

        // Sort when the data was rebuilt or the sort spec changed.
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsCount > 0 &&
                (rebuilt || sortSpecs->SpecsDirty ||
                 s_SortColumn != sortSpecs->Specs[0].ColumnIndex ||
                 s_SortAscending != (sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending)))
            {
                auto& spec = sortSpecs->Specs[0];
                bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
                s_SortColumn = spec.ColumnIndex;
                s_SortAscending = ascending;
                std::sort(sorted.begin(), sorted.end(), [&](const AggregatedEvent& a, const AggregatedEvent& b)
                          {
                    switch (spec.ColumnIndex)
                    {
                        case 0: return ascending ? a.name < b.name : b.name < a.name;
                        case 1: return ascending ? a.selfMs < b.selfMs : b.selfMs < a.selfMs;
                        case 2: return ascending ? a.totalMs < b.totalMs : b.totalMs < a.totalMs;
                        case 3: return ascending ? a.calls < b.calls : b.calls < a.calls;
                        default: return false;
                    } });
            }
            sortSpecs->SpecsDirty = false;
        }

        for (const auto& evt : sorted)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", evt.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", evt.selfMs);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", evt.totalMs);
            ImGui::TableNextColumn();
            ImGui::Text("%d", evt.calls);
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
    m_SelectedFrame = -1;  // Clear selection to follow latest frame
}

}  // namespace DekiEditor
