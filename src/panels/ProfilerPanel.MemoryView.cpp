#include "ProfilerPanel.h"
#include "services/ProfilingService.h"
#include "imgui.h"
#include <algorithm>

namespace DekiEditor
{

// Writes `bytes` as B, KB or MB.
static void FormatBytes(uint32_t bytes, char* buffer, size_t bufferSize)
{
    if (bytes >= 1024 * 1024)
    {
        snprintf(buffer, bufferSize, "%.2f MB", bytes / (1024.0f * 1024.0f));
    }
    else if (bytes >= 1024)
    {
        snprintf(buffer, bufferSize, "%.2f KB", bytes / 1024.0f);
    }
    else
    {
        snprintf(buffer, bufferSize, "%u B", bytes);
    }
}

void ProfilerPanel::DrawMemoryView()
{
    auto& profiler = ProfilingService::Get();
    const auto& history = profiler.GetFrameHistory();

    if (history.empty())
    {
        ImGui::TextDisabled("No memory data available");
        return;
    }

    // The selected frame, or the latest when none is selected
    int frameIdx =
        m_SelectedFrame >= 0 && m_SelectedFrame < (int)history.size() ? m_SelectedFrame : (int)history.size() - 1;

    const auto& currentFrame = history[frameIdx];
    const auto& memStats = currentFrame.memoryStats;

    ImGui::Text("Memory Stats (Frame #%llu)", currentFrame.frameNumber);
    ImGui::Separator();

    char totalBuf[32], externalBuf[32], internalBuf[32];
    FormatBytes(memStats.totalAllocated, totalBuf, sizeof(totalBuf));
    FormatBytes(memStats.externalAllocated, externalBuf, sizeof(externalBuf));
    FormatBytes(memStats.internalRamAllocated, internalBuf, sizeof(internalBuf));

    // Each value in its own colour, matching the timeline below
    ImGui::Columns(2, "MemoryColumns", false);
    ImGui::SetColumnWidth(0, 200);

    ImGui::Text("Total Allocated:");
    ImGui::NextColumn();
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", totalBuf);
    ImGui::NextColumn();

    ImGui::Text("External Used:");
    ImGui::NextColumn();
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", externalBuf);
    ImGui::NextColumn();

    ImGui::Text("Internal RAM Used:");
    ImGui::NextColumn();
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%s", internalBuf);
    ImGui::NextColumn();

#ifdef DEBUG
    ImGui::Text("Active Allocations:");
    ImGui::NextColumn();
    ImGui::Text("%u", memStats.activeAllocations);
    ImGui::NextColumn();

    ImGui::Text("Peak Allocations:");
    ImGui::NextColumn();
    ImGui::Text("%u", memStats.peakAllocations);
    ImGui::NextColumn();
#endif

    ImGui::Columns(1);
    ImGui::Spacing();

    // Memory timeline graph
    ImGui::Text("Memory Timeline:");
    ImGui::Separator();

    float availWidth = ImGui::GetContentRegionAvail().x;
    float graphHeight = 150.0f;
    ImVec2 graphPos = ImGui::GetCursorScreenPos();

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // The graph's top is the peak total plus 10% headroom.
    uint32_t maxMemory = 0;
    for (const auto& frame : history)
    {
        maxMemory = std::max(maxMemory, frame.memoryStats.totalAllocated);
    }

    if (maxMemory > 0)
    {
        maxMemory = (uint32_t)(maxMemory * 1.1f);
    }
    else
    {
        maxMemory = 1024;  // 1 KB when nothing is allocated
    }

    drawList->AddRectFilled(graphPos, ImVec2(graphPos.x + availWidth, graphPos.y + graphHeight),
                            IM_COL32(30, 30, 30, 255));

    // One bar per frame, at most 100 frames
    size_t numFrames = std::min(history.size(), size_t(100));
    float barWidth = availWidth / numFrames;

    for (size_t i = 0; i < numFrames; i++)
    {
        const auto& frame = history[i];
        float x = graphPos.x + i * barWidth;

        // Total memory, blue
        float totalHeight = (float)frame.memoryStats.totalAllocated / maxMemory * graphHeight;
        drawList->AddRectFilled(ImVec2(x, graphPos.y + graphHeight - totalHeight),
                                ImVec2(x + barWidth - 1, graphPos.y + graphHeight),
                                (int)i == m_SelectedFrame ? IM_COL32(100, 150, 255, 255) : IM_COL32(60, 120, 200, 200));

        // The external-memory part, a green overlay
        if (frame.memoryStats.externalAllocated > 0)
        {
            float externalHeight = (float)frame.memoryStats.externalAllocated / maxMemory * graphHeight;
            drawList->AddRectFilled(ImVec2(x, graphPos.y + graphHeight - externalHeight),
                                    ImVec2(x + barWidth - 1, graphPos.y + graphHeight), IM_COL32(60, 180, 60, 150));
        }
    }

    // Scale lines at each quarter
    for (int i = 1; i <= 4; i++)
    {
        float y = graphPos.y + graphHeight * (1.0f - i / 4.0f);
        drawList->AddLine(ImVec2(graphPos.x, y), ImVec2(graphPos.x + availWidth, y), IM_COL32(100, 100, 100, 100));

        char label[32];
        FormatBytes((uint32_t)(maxMemory * i / 4), label, sizeof(label));
        drawList->AddText(ImVec2(graphPos.x + 5, y + 2), IM_COL32(150, 150, 150, 255), label);
    }

    // Clicking a frame selects it and stops recording, so it can be inspected
    ImGui::InvisibleButton("MemoryTimeline", ImVec2(availWidth, graphHeight));
    if (ImGui::IsItemClicked())
    {
        float mouseX = ImGui::GetMousePos().x - graphPos.x;
        int clickedFrame = (int)(mouseX / barWidth);
        if (clickedFrame >= 0 && clickedFrame < (int)history.size() && m_SelectedFrame != clickedFrame)
        {
            m_SelectedFrame = clickedFrame;
            profiler.StopRecording();
        }
    }

    if (ImGui::IsItemHovered())
    {
        float mouseX = ImGui::GetMousePos().x - graphPos.x;
        int hoverFrame = (int)(mouseX / barWidth);
        if (hoverFrame >= 0 && hoverFrame < (int)history.size())
        {
            const auto& frame = history[hoverFrame];
            ImGui::BeginTooltip();
            ImGui::Text("Frame #%llu", frame.frameNumber);

            char total[32], external[32], internal[32];
            FormatBytes(frame.memoryStats.totalAllocated, total, sizeof(total));
            FormatBytes(frame.memoryStats.externalAllocated, external, sizeof(external));
            FormatBytes(frame.memoryStats.internalRamAllocated, internal, sizeof(internal));

            ImGui::Text("Total: %s", total);
            ImGui::Text("External: %s", external);
            ImGui::Text("Internal: %s", internal);
            ImGui::EndTooltip();
        }
    }

    // Legend
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.3f, 0.5f, 0.8f, 1.0f), "[Total Memory]");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.3f, 0.7f, 0.3f, 1.0f), "[External]");
}

}  // namespace DekiEditor
