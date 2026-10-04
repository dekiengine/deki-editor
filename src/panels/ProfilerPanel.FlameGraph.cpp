#include "ProfilerPanel.h"
#include "services/ProfilingService.h"
#include "imgui.h"
#include <vector>
#include <algorithm>
#include <cmath>

namespace DekiEditor
{

uint32_t ProfilerPanel::GetColorForEvent(const ProfileEvent* event, int depth)
{
    // The hue comes from a hash of the name, so an event keeps its colour
    // across frames.
    uint32_t hash = 0;
    for (char c : event->name)
    {
        hash = hash * 31 + c;
    }

    float hue = (hash % 360) / 360.0f;
    float sat = 0.6f + (depth % 3) * 0.1f;
    float val = 0.9f - depth * 0.05f;
    val = std::max(val, 0.5f);

    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, sat, val, r, g, b);

    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
}

void ProfilerPanel::DrawFlameGraph(const ProfileEvent* root, double activeTimeMs)
{
    if (!root || root->children.empty())
    {
        ImGui::TextDisabled("No data for flame graph");
        return;
    }

    m_HoveredEvent = nullptr;

    // Total frame time, which sets the horizontal scale. When the root has no
    // duration, its children's durations are summed.
    double frameTimeUs = root->durationUs;
    if (frameTimeUs <= 0)
    {
        for (const auto& child : root->children)
        {
            frameTimeUs += child->durationUs;
        }
    }

    if (frameTimeUs <= 0)
    {
        ImGui::TextDisabled("No timing data");
        return;
    }

    float availWidth = ImGui::GetContentRegionAvail().x;
    float fontSize = ImGui::GetFontSize();
    float barHeight = fontSize + 10.0f;
    float spacing = 3.0f;

    // Zoom controls
    ImGui::Text("Zoom:");
    ImGui::SameLine();
    if (ImGui::Button("-"))
    {
        m_FlameGraphZoom = std::max(0.5f, m_FlameGraphZoom / 1.5f);
    }
    ImGui::SameLine();
    if (ImGui::Button("+"))
    {
        m_FlameGraphZoom = std::min(10.0f, m_FlameGraphZoom * 1.5f);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset"))
    {
        m_FlameGraphZoom = 1.0f;
        m_FlameGraphScrollX = 0.0f;
    }
    ImGui::SameLine();
    ImGui::Text("%.1fx", m_FlameGraphZoom);
    ImGui::SameLine();
    ImGui::TextDisabled("| Frame: %.2f ms", frameTimeUs / 1000.0);

    float contentWidth = availWidth * m_FlameGraphZoom;
    double pixelsPerUs = contentWidth / frameTimeUs;

    if (ImGui::BeginChild("FlameGraphContent", ImVec2(-1, -1), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar))
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Read before any content is added; bars are placed relative to it.
        ImVec2 basePos = ImGui::GetCursorScreenPos();

        // Deeper levels are not drawn.
        int maxDepth = 15;
        float totalHeight = maxDepth * (barHeight + spacing);

        // Reserves the graph's area so the child window scrolls over it.
        ImGui::InvisibleButton("FlameGraphArea", ImVec2(contentWidth, totalHeight));

        ImVec2 mousePos = ImGui::GetMousePos();

        // Walk the event tree with an explicit stack.
        struct StackEntry
        {
            const ProfileEvent* event;
            double startUs;
            int depth;
        };
        std::vector<StackEntry> stack;

        // The root's children form the bottom row; idle time is left out.
        double currentStartUs = 0;
        for (const auto& child : root->children)
        {
            if (child->isIdleTime)
            {
                continue;
            }
            stack.push_back({ child.get(), currentStartUs, 0 });
            currentStartUs += child->durationUs;
        }

        while (!stack.empty())
        {
            auto entry = stack.back();
            stack.pop_back();

            const ProfileEvent* event = entry.event;
            double startUs = entry.startUs;
            int depth = entry.depth;

            if (depth >= maxDepth)
            {
                continue;
            }

            float x = (float)(startUs * pixelsPerUs);
            float w = (float)(event->durationUs * pixelsPerUs);
            float y = depth * (barHeight + spacing);

            // Bars under a pixel wide are skipped, with their children.
            if (w < 1.0f)
            {
                continue;
            }

            float x1 = basePos.x + x;
            float y1 = basePos.y + y;
            float x2 = x1 + w;
            float y2 = y1 + barHeight;

            uint32_t color = GetColorForEvent(event, depth);
            uint32_t borderColor = IM_COL32(40, 40, 40, 255);

            drawList->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, y2), color);
            if (w > 3.0f)
            {
                drawList->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), borderColor);
            }

            // The name only fits on wider bars.
            if (w > 50)
            {
                float textY = y1 + (barHeight - fontSize) * 0.5f;
                ImGui::PushClipRect(ImVec2(x1 + 3, y1), ImVec2(x2 - 3, y2), true);
                drawList->AddText(ImVec2(x1 + 4, textY), IM_COL32(255, 255, 255, 255), event->name.c_str());
                ImGui::PopClipRect();
            }

            // Hovered: outline the bar and remember it for the tooltip.
            if (mousePos.x >= x1 && mousePos.x < x2 && mousePos.y >= y1 && mousePos.y < y2)
            {
                m_HoveredEvent = event;
                drawList->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(255, 255, 255, 200), 0, 0, 2.0f);
            }

            // Children are pushed in reverse so they pop left to right. Each
            // starts where the siblings before it end.
            double childStartUs = startUs;
            for (int i = (int)event->children.size() - 1; i >= 0; i--)
            {
                const auto& child = event->children[i];
                double childStart = startUs;
                for (int j = 0; j < i; j++)
                {
                    childStart += event->children[j]->durationUs;
                }

                if (!child->isIdleTime && child->durationUs > 0)
                {
                    stack.push_back({ child.get(), childStart, depth + 1 });
                }
            }
        }
    }
    ImGui::EndChild();

    if (m_HoveredEvent)
    {
        ImGui::BeginTooltip();
        double eventMs = m_HoveredEvent->durationUs / 1000.0;
        float percent = activeTimeMs > 0 ? (float)(eventMs / activeTimeMs * 100.0) : 0.0f;

        ImGui::Text("%s", m_HoveredEvent->name.c_str());
        ImGui::Separator();
        ImGui::Text("Duration: %.3f ms", eventMs);
        ImGui::Text("Frame %%: %.1f%%", percent);

        if (!m_HoveredEvent->children.empty())
        {
            double childrenMs = 0;
            for (const auto& child : m_HoveredEvent->children)
            {
                childrenMs += child->durationUs / 1000.0;
            }
            ImGui::Text("Self: %.3f ms", eventMs - childrenMs);
        }

        ImGui::EndTooltip();
    }
}

}  // namespace DekiEditor
