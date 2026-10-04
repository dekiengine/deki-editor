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
    // Hash the name for consistent colors
    uint32_t hash = 0;
    for (char c : event->name)
    {
        hash = hash * 31 + c;
    }

    // Generate color from hash with good saturation
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

    // Calculate total frame time for scaling
    double frameTimeUs = root->durationUs;
    if (frameTimeUs <= 0)
    {
        // Sum children if root has no duration
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

    // Calculate content size
    float contentWidth = availWidth * m_FlameGraphZoom;
    double pixelsPerUs = contentWidth / frameTimeUs;

    // Begin scrollable region
    if (ImGui::BeginChild("FlameGraphContent", ImVec2(-1, -1), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar))
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Get base position BEFORE adding any content
        ImVec2 basePos = ImGui::GetCursorScreenPos();

        // Calculate max depth for sizing
        int maxDepth = 15;
        float totalHeight = maxDepth * (barHeight + spacing);

        // Reserve space for flame graph (invisible)
        ImGui::InvisibleButton("FlameGraphArea", ImVec2(contentWidth, totalHeight));

        // Check for mouse position
        ImVec2 mousePos = ImGui::GetMousePos();

        // Draw bars using a stack-based approach
        struct StackEntry
        {
            const ProfileEvent* event;
            double startUs;
            int depth;
        };
        std::vector<StackEntry> stack;

        // Start with root's children
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

            // Calculate bar position relative to base
            float x = (float)(startUs * pixelsPerUs);
            float w = (float)(event->durationUs * pixelsPerUs);
            float y = depth * (barHeight + spacing);

            // Skip very small bars
            if (w < 1.0f)
            {
                continue;
            }

            float x1 = basePos.x + x;
            float y1 = basePos.y + y;
            float x2 = x1 + w;
            float y2 = y1 + barHeight;

            // Get color based on event name
            uint32_t color = GetColorForEvent(event, depth);
            uint32_t borderColor = IM_COL32(40, 40, 40, 255);

            // Draw bar
            drawList->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, y2), color);
            if (w > 3.0f)
            {
                drawList->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), borderColor);
            }

            // Draw text if bar is wide enough
            if (w > 50)
            {
                float textY = y1 + (barHeight - fontSize) * 0.5f;
                ImGui::PushClipRect(ImVec2(x1 + 3, y1), ImVec2(x2 - 3, y2), true);
                drawList->AddText(ImVec2(x1 + 4, textY), IM_COL32(255, 255, 255, 255), event->name.c_str());
                ImGui::PopClipRect();
            }

            // Hover detection
            if (mousePos.x >= x1 && mousePos.x < x2 && mousePos.y >= y1 && mousePos.y < y2)
            {
                m_HoveredEvent = event;
                // Highlight hovered bar
                drawList->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(255, 255, 255, 200), 0, 0, 2.0f);
            }

            // Add children to stack (in reverse order so they're processed left-to-right)
            double childStartUs = startUs;
            for (int i = (int)event->children.size() - 1; i >= 0; i--)
            {
                const auto& child = event->children[i];
                double childStart = startUs;
                // Calculate proper start position for this child
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

    // Show tooltip for hovered event
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
