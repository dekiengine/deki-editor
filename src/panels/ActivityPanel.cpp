#include "ActivityPanel.h"
#include "../EditorVersion.h"
#include "../PanelMaximize.h"
#include "EditorApp.h"
#include "ConsolePanel.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/Motion.h>
#include <deki-editor/IconsTabler.h>
#include "imgui.h"
#include <algorithm>

namespace DekiEditor
{

ActivityPanel* ActivityPanel::s_Instance = nullptr;

ActivityPanel::ActivityPanel(EditorApp* app)
    : m_App(app)
{
    s_Instance = this;
}

// ============================================================================
// Draw
// ============================================================================

static void FormatTime(char* buf, size_t bufSize, std::chrono::system_clock::time_point tp)
{
    auto timeTVal = std::chrono::system_clock::to_time_t(tp);
    std::tm tmVal;
#ifdef _WIN32
    localtime_s(&tmVal, &timeTVal);
#else
    localtime_r(&timeTVal, &tmVal);
#endif
    snprintf(buf, bufSize, "%02d:%02d:%02d", tmVal.tm_hour, tmVal.tm_min, tmVal.tm_sec);
}

void ActivityPanel::Draw()
{
    if (!isOpen)
    {
        return;
    }

    DekiEditor::FirstUseWindowPlacement(450, 350);

    if (!ImGui::Begin("ACTIVITY###Activity", &isOpen, DekiEditor::MaximizedWindowFlags("ACTIVITY###Activity")))
    {
        ImGui::End();
        return;
    }

    // Console-style toolbar strip: a flat Clear cell (eraser icon) with 1px
    // dividers and a bottom hairline, matching ConsolePanel's toolbar chrome.
    {
        const float dpi = m_App->Dpi(1.0f);
        const float rowH = 34.0f * dpi;
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        const ImVec2 rowMax(rowMin.x + avail, rowMin.y + rowH);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 colLine = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
        const ImU32 colDim = ImGui::ColorConvertFloat4ToU32(Palette::Dim);
        const ImU32 colHoverBg = IM_COL32(255, 255, 255, 6);

        float cursorX = rowMin.x;
        auto drawDivider = [&](float x) { dl->AddLine(ImVec2(x, rowMin.y), ImVec2(x, rowMax.y), colLine, 1.0f); };

        // Leading divider at strip start.
        drawDivider(cursorX);

        // Clear cell.
        {
            const char* icon = ICON_TI_ERASER;
            const ImVec2 iconSz = ImGui::CalcTextSize(icon);
            const float w = iconSz.x + 22.0f * dpi;
            ImGui::SetCursorScreenPos(ImVec2(cursorX, rowMin.y));
            bool clicked = ImGui::InvisibleButton("##clear", ImVec2(w, rowH));
            bool hovered = ImGui::IsItemHovered();
            if (hovered)
            {
                DekiEditor::Tooltip("Clear");
            }
            if (const float hoverT = DekiEditor::Motion::ItemHover(hovered); hoverT > 0.0f)
            {
                dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + w, rowMax.y),
                                  DekiEditor::Motion::WithAlpha(colHoverBg, hoverT));
            }
            if (const float pressT = DekiEditor::Motion::ItemPress(); pressT > 0.0f)
            {
                dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + w, rowMax.y),
                                  DekiEditor::Motion::PressShade(pressT));
            }
            dl->AddText(ImVec2(cursorX + (w - iconSz.x) * 0.5f, rowMin.y + (rowH - iconSz.y) * 0.5f), colDim, icon);
            cursorX += w;
            drawDivider(cursorX);
            if (clicked)
            {
                Clear();
            }
        }

        // Reserve the row in layout flow and close the bottom edge with a hairline.
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMax.y));
        dl->AddLine(ImVec2(rowMin.x, rowMax.y), ImVec2(rowMax.x, rowMax.y), colLine, 1.0f);
    }

    ImGui::BeginChild("ProcessList", ImVec2(0, 0), false);

    std::lock_guard<std::mutex> lock(m_Mutex);

    // Draw each process as a card
    for (int i = 0; i < (int)m_Processes.size(); i++)
    {
        auto& proc = m_Processes[i];
        ImGui::PushID(i);

        // Status icon + color
        const char* icon = ICON_TI_LOADER;
        ImVec4 iconColor(0.5f, 0.7f, 1.0f, 1.0f);  // blue for running
        if (proc.status == ProcessStatus::Completed)
        {
            icon = ICON_TI_CHECK;
            iconColor = DekiEditor::Palette::Green;  // green
        }
        else if (proc.status == ProcessStatus::Failed)
        {
            icon = ICON_TI_X;
            iconColor = DekiEditor::Palette::Red;  // red
        }
        else if (proc.status == ProcessStatus::Cancelled)
        {
            icon = ICON_TI_BAN;
            iconColor = DekiEditor::Palette::Amber;  // yellow
        }

        // Card background
        ImVec2 cardPos = ImGui::GetCursorScreenPos();
        float cardWidth = ImGui::GetContentRegionAvail().x;
        float lineH = ImGui::GetTextLineHeight();
        float cardPadY = 10.0f;
        float cardHeight = lineH * 2.0f + 4.0f + cardPadY * 2.0f;
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        ImU32 bgColor = proc.expanded ? IM_COL32(50, 55, 60, 255) : IM_COL32(40, 42, 45, 255);
        drawList->AddRectFilled(cardPos, ImVec2(cardPos.x + cardWidth, cardPos.y + cardHeight), bgColor, 4.0f);

        // Clickable area
        if (ImGui::InvisibleButton("##card", ImVec2(cardWidth, cardHeight)))
        {
            proc.expanded = !proc.expanded;
        }

        // Icon (vertically centered)
        float padding = 12.0f;
        float iconY = cardPos.y + (cardHeight - lineH) * 0.5f;
        drawList->AddText(ImVec2(cardPos.x + padding, iconY), ImGui::GetColorU32(iconColor), icon);

        // Process name (top row)
        float textX = cardPos.x + padding + 36.0f;
        float nameY = cardPos.y + cardPadY;
        drawList->AddText(ImVec2(textX, nameY), ImGui::GetColorU32(ImGuiCol_Text), proc.name.c_str());

        // Cancel button (right-aligned, for running processes)
        float rightEdge = cardPos.x + cardWidth - padding;
        if (proc.status == ProcessStatus::Running && proc.onCancel)
        {
            const char* cancelIcon = ICON_TI_X;
            ImVec2 cancelSize = ImGui::CalcTextSize(cancelIcon);
            float cancelBtnSize = cancelSize.y + 4.0f;
            float cancelX = rightEdge - cancelBtnSize;
            float cancelY = cardPos.y + (cardHeight - cancelBtnSize) * 0.5f;

            ImGui::SetCursorScreenPos(ImVec2(cancelX, cancelY));
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 30, 30, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(120, 40, 40, 255));
            if (ImGui::Button("##cancel", ImVec2(cancelBtnSize, cancelBtnSize)))
            {
                if (proc.onCancel)
                {
                    proc.onCancel();
                }
                proc.status = ProcessStatus::Cancelled;
                proc.lastMessage = "Cancelled";
            }
            ImGui::PopStyleColor(3);

            // Draw the X icon centered on the button
            drawList->AddText(ImVec2(cancelX + (cancelBtnSize - cancelSize.x) * 0.5f,
                                     cancelY + (cancelBtnSize - cancelSize.y) * 0.5f),
                              ImGui::GetColorU32(DekiEditor::Palette::Red), cancelIcon);

            rightEdge = cancelX - 8.0f;
        }

        // Timestamp (right-aligned, top row)
        char timeBuf[16];
        FormatTime(timeBuf, sizeof(timeBuf), proc.startTime);
        ImVec2 timeSize = ImGui::CalcTextSize(timeBuf);
        drawList->AddText(ImVec2(rightEdge - timeSize.x, nameY), ImGui::GetColorU32(ImGuiCol_TextDisabled), timeBuf);

        // Last message (subtitle, second row)
        if (!proc.lastMessage.empty())
        {
            float subtitleY = nameY + lineH + 4.0f;

            // Truncate if too long
            float maxSubWidth = cardWidth - textX + cardPos.x - padding;
            const char* subtitleEnd = nullptr;
            ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), maxSubWidth, maxSubWidth, proc.lastMessage.c_str(),
                                            nullptr, &subtitleEnd);

            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(textX, subtitleY),
                              ImGui::GetColorU32(ImGuiCol_TextDisabled), proc.lastMessage.c_str(), subtitleEnd);
        }

        // Expanded: show log entries
        if (proc.expanded && !proc.entries.empty())
        {
            float logHeight = (std::min)((float)proc.entries.size() * (ImGui::GetTextLineHeightWithSpacing()), 300.0f);
            ImGui::BeginChild("##log", ImVec2(0, logHeight), true);

            for (int e = 0; e < (int)proc.entries.size(); e++)
            {
                const auto& entry = proc.entries[e];
                char entryTime[16];
                FormatTime(entryTime, sizeof(entryTime), entry.timestamp);

                ImVec4 color = entry.isError ? DekiEditor::Palette::Red : DekiEditor::Palette::Dim;

                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::PushID(e);
                ImGui::PushTextWrapPos(0.0f);
                std::string fullLine = std::string(entryTime) + "  " + entry.message;
                ImGui::TextUnformatted(fullLine.c_str());
                ImGui::PopTextWrapPos();

                // Right-click context menu on each entry
                PushContextMenuPopupStyle();
                if (ImGui::BeginPopupContextItem("##entry_ctx"))
                {
                    PushContextMenuItemStyle();
                    if (ImGui::MenuItem("Copy"))
                    {
                        std::string line = std::string(entryTime) + "  " + entry.message;
                        ImGui::SetClipboardText(line.c_str());
                    }
                    if (ImGui::MenuItem("Copy All"))
                    {
                        std::string allText;
                        for (const auto& e2 : proc.entries)
                        {
                            char t[16];
                            FormatTime(t, sizeof(t), e2.timestamp);
                            allText += t;
                            allText += "  ";
                            allText += e2.message;
                            allText += "\n";
                        }
                        ImGui::SetClipboardText(allText.c_str());
                    }
                    ImGui::PopStyleVar();  // context menu ItemSpacing
                    ImGui::EndPopup();
                }
                ImGui::PopStyleVar();  // context menu WindowPadding

                ImGui::PopID();
                ImGui::PopStyleColor();
            }

            // Auto-scroll to bottom
            if (proc.status == ProcessStatus::Running)
            {
                ImGui::SetScrollHereY(1.0f);
            }

            ImGui::EndChild();
        }

        ImGui::Spacing();
        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::End();
}

// ============================================================================
// Data
// ============================================================================

ActivityProcess& ActivityPanel::FindOrCreateProcess(const std::string& name)
{
    // Only reuse a process that is still Running
    for (auto& proc : m_Processes)
    {
        if (proc.name == name && proc.status == ProcessStatus::Running)
        {
            return proc;
        }
    }

    // Always create a new entry (newest first)
    m_Processes.insert(m_Processes.begin(), ActivityProcess());
    auto& proc = m_Processes.front();
    proc.name = name;
    proc.startTime = std::chrono::system_clock::now();
    return proc;
}

void ActivityPanel::AddEntry(const std::string& process, const std::string& message, bool isError)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    auto& proc = FindOrCreateProcess(process);

    ProcessLogEntry entry;
    entry.message = message;
    entry.isError = isError;
    entry.timestamp = std::chrono::system_clock::now();
    proc.entries.push_back(entry);
    proc.lastMessage = message;

    // Cap entries per process
    const size_t maxEntries = 500;
    if (proc.entries.size() > maxEntries)
    {
        proc.entries.erase(proc.entries.begin(), proc.entries.begin() + (int)(proc.entries.size() - maxEntries));
    }
}

void ActivityPanel::Clear()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Processes.erase(std::remove_if(m_Processes.begin(), m_Processes.end(),
                                     [](const ActivityProcess& p) { return p.status != ProcessStatus::Running; }),
                      m_Processes.end());
}

// ============================================================================
// Static API
// ============================================================================

void ActivityPanel::Log(const std::string& process, const std::string& message)
{
    if (s_Instance)
    {
        s_Instance->AddEntry(process, message, false);
    }
}

void ActivityPanel::LogError(const std::string& process, const std::string& message)
{
    if (s_Instance)
    {
        s_Instance->AddEntry(process, message, true);
    }
}

void ActivityPanel::CompleteProcess(const std::string& process)
{
    if (!s_Instance)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(s_Instance->m_Mutex);

    for (auto& proc : s_Instance->m_Processes)
    {
        if (proc.name == process && proc.status == ProcessStatus::Running)
        {
            proc.status = ProcessStatus::Completed;
            proc.lastMessage = "Completed";
            return;
        }
    }
}

void ActivityPanel::FailProcess(const std::string& process, const std::string& error)
{
    if (!s_Instance)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(s_Instance->m_Mutex);

    for (auto& proc : s_Instance->m_Processes)
    {
        if (proc.name == process && proc.status == ProcessStatus::Running)
        {
            proc.status = ProcessStatus::Failed;
            proc.lastMessage = error;

            ProcessLogEntry entry;
            entry.message = error;
            entry.isError = true;
            entry.timestamp = std::chrono::system_clock::now();
            proc.entries.push_back(entry);
            return;
        }
    }
}

void ActivityPanel::CancelProcess(const std::string& process)
{
    if (!s_Instance)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(s_Instance->m_Mutex);

    for (auto& proc : s_Instance->m_Processes)
    {
        if (proc.name == process && proc.status == ProcessStatus::Running)
        {
            if (proc.onCancel)
            {
                proc.onCancel();
            }
            proc.status = ProcessStatus::Cancelled;
            proc.lastMessage = "Cancelled";
            return;
        }
    }
}

bool ActivityPanel::IsCancelled(const std::string& process)
{
    if (!s_Instance)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_Instance->m_Mutex);

    for (const auto& proc : s_Instance->m_Processes)
    {
        if (proc.name == process && proc.status == ProcessStatus::Cancelled)
        {
            return true;
        }
    }
    return false;
}

void ActivityPanel::SetCancelCallback(const std::string& process, std::function<void()> callback)
{
    if (!s_Instance)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(s_Instance->m_Mutex);

    for (auto& proc : s_Instance->m_Processes)
    {
        if (proc.name == process && proc.status == ProcessStatus::Running)
        {
            proc.onCancel = std::move(callback);
            return;
        }
    }
}

// ============================================================================
// Status Bar (drawn in the main dockspace window)
// ============================================================================

void ActivityPanel::DrawStatusBar()
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    float barHeight = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
    float padding = 8.0f;

    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();
    ImVec2 barMin(windowPos.x, windowPos.y + windowSize.y - barHeight);
    ImVec2 barMax(windowPos.x + windowSize.x, windowPos.y + windowSize.y);

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Pre-calculate MCP button dimensions so we can exclude its area from the status bar click zone
    bool mcpRunning = m_App->IsMcpServerRunning();
    const char* mcpLabel = mcpRunning ? ICON_TI_PLUG_CONNECTED " MCP connected" : ICON_TI_PLUG " MCP";
    ImVec2 mcpSize = ImGui::CalcTextSize(mcpLabel);
    float mcpBtnWidth = mcpSize.x + 12.0f;

    // Clickable area - opens the Activity panel (excludes MCP button on the right)
    ImGui::SetCursorScreenPos(barMin);
    if (ImGui::InvisibleButton("##StatusBarClick", ImVec2(barMax.x - barMin.x - mcpBtnWidth, barHeight)))
    {
        isOpen = true;
    }

    // Bar background (schematic Panel) + hairline top border
    const ImU32 bgCol = ImGui::ColorConvertFloat4ToU32(Palette::Panel);
    const ImU32 borderCol = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
    const ImU32 accentCol = ImGui::ColorConvertFloat4ToU32(Palette::Accent);
    drawList->AddRectFilled(barMin, barMax, bgCol);
    drawList->AddLine(barMin, ImVec2(barMax.x, barMin.y), borderCol);

    float textY = barMin.y + (barHeight - ImGui::GetTextLineHeight()) * 0.5f;
    float cursorX = barMin.x + padding;

    // Count running processes
    int runningCount = 0;
    const ActivityProcess* latestRunning = nullptr;
    for (const auto& proc : m_Processes)
    {
        if (proc.status == ProcessStatus::Running)
        {
            runningCount++;
            if (!latestRunning)
            {
                latestRunning = &proc;
            }
        }
    }

    if (runningCount > 0)
    {
        // Running indicator icon
        drawList->AddText(ImVec2(cursorX, textY), ImGui::GetColorU32(ImVec4(0.5f, 0.7f, 1.0f, 1.0f)), ICON_TI_LOADER);
        cursorX += ImGui::CalcTextSize(ICON_TI_LOADER).x + 6.0f;

        // Task count
        char countBuf[32];
        snprintf(countBuf, sizeof(countBuf), "%d task%s", runningCount, runningCount == 1 ? "" : "s");
        drawList->AddText(ImVec2(cursorX, textY), ImGui::GetColorU32(ImGuiCol_Text), countBuf);
        cursorX += ImGui::CalcTextSize(countBuf).x + 24.0f;

        // Separator
        drawList->AddLine(ImVec2(cursorX, barMin.y + 4.0f), ImVec2(cursorX, barMax.y - 4.0f),
                          ImGui::ColorConvertFloat4ToU32(Palette::Line2));
        cursorX += 24.0f;

        // Latest running process name + status
        if (latestRunning)
        {
            drawList->AddText(ImVec2(cursorX, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                              latestRunning->name.c_str());
            cursorX += ImGui::CalcTextSize(latestRunning->name.c_str()).x + 24.0f;

            if (!latestRunning->lastMessage.empty())
            {
                float maxMsgWidth = barMax.x - cursorX - padding;
                if (maxMsgWidth > 40.0f)
                {
                    const char* msgEnd = nullptr;
                    ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), maxMsgWidth, maxMsgWidth,
                                                    latestRunning->lastMessage.c_str(), nullptr, &msgEnd);
                    drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(cursorX, textY),
                                      ImGui::GetColorU32(DekiEditor::Palette::Dim), latestRunning->lastMessage.c_str(),
                                      msgEnd);
                }
            }
        }
    }
    else if (!m_Processes.empty())
    {
        // Show most recent completed/failed process
        const auto& proc = m_Processes.front();
        const char* statusIcon = ICON_TI_X;
        ImVec4 statusColor(1.0f, 0.3f, 0.3f, 1.0f);
        if (proc.status == ProcessStatus::Completed)
        {
            statusIcon = ICON_TI_CHECK;
            statusColor = DekiEditor::Palette::Green;
        }
        else if (proc.status == ProcessStatus::Cancelled)
        {
            statusIcon = ICON_TI_BAN;
            statusColor = DekiEditor::Palette::Amber;
        }

        drawList->AddText(ImVec2(cursorX, textY), ImGui::GetColorU32(statusColor), statusIcon);
        cursorX += ImGui::CalcTextSize(statusIcon).x + 6.0f;

        std::string summary = proc.name + ": " + proc.lastMessage;
        float maxWidth = barMax.x - cursorX - padding;
        const char* summaryEnd = nullptr;
        ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), maxWidth, maxWidth, summary.c_str(), nullptr,
                                        &summaryEnd);
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(cursorX, textY),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), summary.c_str(), summaryEnd);
    }
    else
    {
        // Accent square + "Ready" - schematic idle state.
        const float dotSize = 7.0f;
        const float lineH = ImGui::GetTextLineHeight();
        drawList->AddRectFilled(ImVec2(cursorX, textY + (lineH - dotSize) * 0.5f),
                                ImVec2(cursorX + dotSize, textY + (lineH + dotSize) * 0.5f), accentCol);
        cursorX += dotSize + 6.0f;
        drawList->AddText(ImVec2(cursorX, textY), accentCol, "Ready");
        cursorX += ImGui::CalcTextSize("Ready").x;

        // "Build failed · N error(s)" in red when a build left errors in the
        // Console; any other errors are just counted. A scene that failed to
        // load used to read as a failed build.
        if (auto* console = ConsolePanel::GetInstance())
        {
            const int buildErrs = console->GetBuildErrorCount();
            const int errs = buildErrs > 0 ? buildErrs : console->GetErrorCount();
            if (errs > 0)
            {
                char buildBuf[48];
                snprintf(buildBuf, sizeof(buildBuf), buildErrs > 0 ? "Build failed \xc2\xb7 %d error%s" : "%d error%s",
                         errs, errs == 1 ? "" : "s");
                cursorX += 20.0f;
                const ImU32 redCol = ImGui::ColorConvertFloat4ToU32(Palette::Red);
                drawList->AddText(ImVec2(cursorX, textY), redCol, buildBuf);
                cursorX += ImGui::CalcTextSize(buildBuf).x;
            }
        }
    }

    // Version, right-aligned left of the MCP button: the number a bug report
    // needs, always on screen. Help > About has the full line.
    {
        char verBuf[48];
        snprintf(verBuf, sizeof(verBuf), "v%s", EditorVersionString());
        const float verX = barMax.x - mcpBtnWidth - padding - 16.0f - ImGui::CalcTextSize(verBuf).x;
        if (verX > cursorX + 24.0f)
        {
            drawList->AddText(ImVec2(verX, textY), ImGui::ColorConvertFloat4ToU32(Palette::Dim), verBuf);
        }
    }

    // MCP server toggle button (right-aligned)
    {
        ImVec4 mcpColor = mcpRunning ? Palette::Accent : Palette::Dim;

        float mcpX = barMax.x - mcpSize.x - padding;
        float mcpBtnX = mcpX - 6.0f;

        // Draw clickable button area
        ImGui::SetCursorScreenPos(ImVec2(mcpBtnX, barMin.y));
        ImGui::PushID("##McpToggle");
        if (ImGui::InvisibleButton("##McpBtn", ImVec2(mcpBtnWidth, barHeight)))
        {
            if (mcpRunning)
            {
                m_App->StopMcpServer();
            }
            else
            {
                m_App->StartMcpServer();
            }
        }
        ImGui::PopID();

        // Right-click copies the connection URL. The server binds loopback and
        // mints a fresh token each start, so the URL cannot be written down in
        // advance - the user needs a way to get the current one.
        if (mcpRunning && ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            ImGui::SetClipboardText(m_App->GetMcpSseUrl().c_str());
        }

        // Hover highlight
        if (ImGui::IsItemHovered())
        {
            drawList->AddRectFilled(ImVec2(mcpBtnX, barMin.y), ImVec2(mcpBtnX + mcpBtnWidth, barMax.y),
                                    ImGui::ColorConvertFloat4ToU32(Palette::Bg2));
            DekiEditor::Tooltip(mcpRunning ? "MCP server running on 127.0.0.1:3000\nRight-click to copy the connection "
                                             "URL (includes session token)"
                                           : "Start MCP server (local only, port 3000)");
        }

        drawList->AddText(ImVec2(mcpX, textY), ImGui::GetColorU32(mcpColor), mcpLabel);
    }
}

float ActivityPanel::GetStatusBarHeight()
{
    return ImGui::GetTextLineHeightWithSpacing() + 4.0f;
}

}  // namespace DekiEditor
