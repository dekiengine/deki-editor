#include "ConsolePanel.h"
#include <deki-editor/ProcessLaunch.h>
#include "../PanelMaximize.h"
#include "../EditorApp.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/Motion.h>
#include <deki-editor/IconsTabler.h>
#include <functional>
#include "../core/Project.h"
#include <deki-editor/EditorSettings.h>
#include "../services/PackageLoader.h"
#include <deki/Scene.h>
#include <deki/Object.h>
#include "imgui.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace DekiEditor
{

ConsolePanel* ConsolePanel::s_Instance = nullptr;

ConsolePanel::ConsolePanel(EditorApp* app)
    : m_App(app)
{
    s_Instance = this;

    // Load persisted filter states
    m_ShowEditor = EditorSettings::GetBool("console.showEditor", false);
    m_ShowDebug = EditorSettings::GetBool("console.showDebug", false);
    m_ShowInfo = EditorSettings::GetBool("console.showInfo", true);
    m_ShowWarnings = EditorSettings::GetBool("console.showWarnings", true);
    m_ShowErrors = EditorSettings::GetBool("console.showErrors", true);
    m_Collapse = EditorSettings::GetBool("console.collapse", true);
    m_AutoScroll = EditorSettings::GetBool("console.autoScroll", true);
}

bool ConsolePanel::PassesFilter(const LogEntry& e, const std::string& filterLower) const
{
    switch (e.level)
    {
        case ::Deki::LogLevel::Editor:
            if (!m_ShowEditor)
            {
                return false;
            }
            break;
        case ::Deki::LogLevel::Debug:
            if (!m_ShowDebug)
            {
                return false;
            }
            break;
        case ::Deki::LogLevel::Warning:
            if (!m_ShowWarnings)
            {
                return false;
            }
            break;
        case ::Deki::LogLevel::Error:
            if (!m_ShowErrors)
            {
                return false;
            }
            break;
        case ::Deki::LogLevel::Info:
            if (!m_ShowInfo)
            {
                return false;
            }
            break;
        default: break;
    }
    if (!filterLower.empty())
    {
        if (e.messageLower.find(filterLower) == std::string::npos)
        {
            return false;
        }
    }
    return true;
}

std::string ConsolePanel::FormatLogLine(const LogEntry& e)
{
    auto t = std::chrono::system_clock::to_time_t(e.timestamp);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char tb[16];
    snprintf(tb, sizeof(tb), "[%02d:%02d:%02d] ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    const char* p = "";
    switch (e.level)
    {
        case ::Deki::LogLevel::Editor: p = "[EDITOR] "; break;
        case ::Deki::LogLevel::Debug: p = "[DEBUG] "; break;
        case ::Deki::LogLevel::Warning: p = "[WARN] "; break;
        case ::Deki::LogLevel::Error: p = "[ERROR] "; break;
        default: break;
    }
    return std::string(tb) + p + e.message;
}

void ConsolePanel::Draw()
{
    if (!m_Open)
    {
        return;
    }

    DekiEditor::FirstUseWindowPlacement(600, 300);

    if (m_FocusFrames > 0)
    {
        ImGui::SetNextWindowFocus();
        m_FocusFrames--;
    }

    // NoScrollWithMouse/NoScrollbar: the window itself never scrolls - the log
    // lives in its own child (which scrolls independently). This frees the mouse
    // wheel for the toolbar's horizontal scroll when the cursor is over the
    // toolbar, instead of ImGui scrolling the whole window (which dragged the
    // log along with it).
    if (!ImGui::Begin("CONSOLE###Console", &m_Open,
                      DekiEditor::MaximizedWindowFlags("CONSOLE###Console") | ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse))
    {
        ImGui::End();
        return;
    }

    // Toolbar - schematic style: dim ALL CAPS labels with accent check marks.
    // Mockup-style segmented filter row (mockup line 502-541):
    //   [ICO][ICO][ICO]  [Editor][Debug][Info][Warnings ▪][Errors ▪]   [search Filter…]
    // All cells share a 1px Line2 left divider; no internal padding gaps;
    // active chips/buttons get a subtle white-fade bg.
    {
        const float dpi = m_App->Dpi(1.0f);
        const float rowH = 34.0f * dpi;
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        const ImVec2 rowMax(rowMin.x + avail, rowMin.y + rowH);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 colLine = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
        const ImU32 colLine3 = ImGui::ColorConvertFloat4ToU32(Palette::Line3);
        const ImU32 colDim = ImGui::ColorConvertFloat4ToU32(Palette::Dim);
        const ImU32 colFg = ImGui::ColorConvertFloat4ToU32(Palette::Fg);
        const ImU32 colActiveBg = IM_COL32(255, 255, 255, 10);
        const ImU32 colHoverBg = IM_COL32(255, 255, 255, 6);
        const ImU32 colRedBg = IM_COL32(232, 90, 80, 18);

        float cursorX = rowMin.x;

        auto drawDivider = [&](float x) { dl->AddLine(ImVec2(x, rowMin.y), ImVec2(x, rowMax.y), colLine, 1.0f); };

        auto iconBtn =
            [&](const char* id, const char* icon, const char* tooltip, bool active, std::function<void()> onClick)
        {
            const ImVec2 iconSz = ImGui::CalcTextSize(icon);
            const float w = iconSz.x + 22.0f * dpi;
            ImGui::SetCursorScreenPos(ImVec2(cursorX, rowMin.y));
            ImGui::PushID(id);
            bool clicked = ImGui::InvisibleButton(id, ImVec2(w, rowH));
            bool hovered = ImGui::IsItemHovered();
            if (hovered && tooltip)
            {
                DekiEditor::Tooltip("%s", tooltip);
            }
            // Active and hover backgrounds fade; hover only while not active.
            const float hoverT = DekiEditor::Motion::ItemHover(hovered);
            const float pressT = DekiEditor::Motion::ItemPress();
            const float activeT = DekiEditor::Motion::EaseOut(
                DekiEditor::Motion::To(ImGui::GetItemID() ^ 0xAC71u, active ? 1.0f : 0.0f, DekiEditor::Motion::Fast));
            ImGui::PopID();
            if (activeT > 0.0f)
            {
                dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + w, rowMax.y),
                                  DekiEditor::Motion::WithAlpha(colActiveBg, activeT));
            }
            if (hoverT * (1.0f - activeT) > 0.0f)
            {
                dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + w, rowMax.y),
                                  DekiEditor::Motion::WithAlpha(colHoverBg, hoverT * (1.0f - activeT)));
            }
            if (pressT > 0.0f)
            {
                dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + w, rowMax.y),
                                  DekiEditor::Motion::PressShade(pressT));
            }
            const ImU32 fg = active ? colFg : colDim;
            const float iconX = cursorX + (w - iconSz.x) * 0.5f;
            const float iconY = rowMin.y + (rowH - iconSz.y) * 0.5f;
            dl->AddText(ImVec2(iconX, iconY), fg, icon);
            cursorX += w;
            drawDivider(cursorX);
            if (clicked && onClick)
            {
                onClick();
            }
        };

        auto chip =
            [&](const char* id, const char* label, ImU32 dotColor, bool* onPtr, const char* settingKey, bool useRedBg)
        {
            ImFont* lf = g_FontUI ? g_FontUI : ImGui::GetFont();
            const float lfSize = ImGui::GetFontSize();
            const ImVec2 labelSz = lf->CalcTextSizeA(lfSize, FLT_MAX, 0.0f, label);
            const float dotSize = 7.0f * dpi;
            const float w = labelSz.x + dotSize + 6.0f * dpi + 22.0f * dpi;
            ImGui::SetCursorScreenPos(ImVec2(cursorX, rowMin.y));
            ImGui::PushID(id);
            bool clicked = ImGui::InvisibleButton(id, ImVec2(w, rowH));
            bool hovered = ImGui::IsItemHovered();
            // On and hover backgrounds fade; hover only while off.
            const float hoverT = DekiEditor::Motion::ItemHover(hovered);
            const float pressT = DekiEditor::Motion::ItemPress();
            const float onT = DekiEditor::Motion::EaseOut(
                DekiEditor::Motion::To(ImGui::GetItemID() ^ 0xAC71u, *onPtr ? 1.0f : 0.0f, DekiEditor::Motion::Fast));
            ImGui::PopID();
            if (onT > 0.0f)
            {
                dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + w, rowMax.y),
                                  DekiEditor::Motion::WithAlpha(useRedBg ? colRedBg : colActiveBg, onT));
            }
            if (hoverT * (1.0f - onT) > 0.0f)
            {
                dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + w, rowMax.y),
                                  DekiEditor::Motion::WithAlpha(colHoverBg, hoverT * (1.0f - onT)));
            }
            if (pressT > 0.0f)
            {
                dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + w, rowMax.y),
                                  DekiEditor::Motion::PressShade(pressT));
            }

            const ImU32 textCol = *onPtr ? colFg : colDim;
            const float dotX = cursorX + 11.0f * dpi;
            const float dotY = rowMin.y + (rowH - dotSize) * 0.5f;
            if (*onPtr)
            {
                dl->AddRectFilled(ImVec2(dotX, dotY), ImVec2(dotX + dotSize, dotY + dotSize), dotColor);
            }
            else
            {
                dl->AddRect(ImVec2(dotX, dotY), ImVec2(dotX + dotSize, dotY + dotSize), colLine3);
            }
            const float labelX = dotX + dotSize + 6.0f * dpi;
            const float labelY = rowMin.y + (rowH - labelSz.y) * 0.5f;
            dl->AddText(ImVec2(labelX, labelY), textCol, label);
            cursorX += w;
            drawDivider(cursorX);
            if (clicked)
            {
                *onPtr = !*onPtr;
                if (settingKey)
                {
                    EditorSettings::SetBool(settingKey, *onPtr);
                }
            }
        };

        // The whole toolbar is one horizontal strip: icon buttons + level chips
        // + the search bar. The search bar stretches to fill the row width left
        // of the right edge, down to a 150px minimum. Until it's forced below
        // that minimum, everything fits and nothing scrolls; once the window is
        // too narrow to give the search bar 150px, the strip overflows and
        // scrolls horizontally (mouse wheel, like dock tabs).
        const float padR = 8.0f * dpi;
        const float gapFilter = 4.0f * dpi;  // gap between the chips and the search bar
        const float minFilterW = 150.0f * dpi;
        const float trackW = rowMax.x - rowMin.x;

        // Draw the strip offset by m_ToolbarScroll, clipped to the row so cells
        // scrolled out of view are neither drawn nor clickable.
        const float stripStartX = rowMin.x - m_ToolbarScroll;
        cursorX = stripStartX;
        ImGui::PushClipRect(ImVec2(rowMin.x, rowMin.y), ImVec2(rowMax.x, rowMax.y), true);

        // Leading divider at strip start.
        drawDivider(cursorX);

        // Action icon buttons.
        iconBtn("##clear", ICON_TI_ERASER, "Clear", false, [&] { Clear(); });
        iconBtn("##fold", ICON_TI_FOLD, "Collapse repeats", m_Collapse,
                [&]
                {
                    m_Collapse = !m_Collapse;
                    EditorSettings::SetBool("console.collapse", m_Collapse);
                });
        iconBtn("##autoscroll", ICON_TI_ARROW_DOWN, "Auto-scroll", m_AutoScroll,
                [&]
                {
                    m_AutoScroll = !m_AutoScroll;
                    EditorSettings::SetBool("console.autoScroll", m_AutoScroll);
                });

        // Level chips.
        const ImU32 dotAccent = ImGui::ColorConvertFloat4ToU32(Palette::Accent);
        const ImU32 dotDim = colDim;
        const ImU32 dotFg = colFg;
        const ImU32 dotAmber = ImGui::ColorConvertFloat4ToU32(Palette::Amber);
        const ImU32 dotRed = ImGui::ColorConvertFloat4ToU32(Palette::Red);
        // Editor and Debug are for working on the editor itself, so they sit
        // in the menu at the right end rather than in the row.
        (void)dotAccent;
        (void)dotDim;
        chip("##info", "Info", dotFg, &m_ShowInfo, "console.showInfo", false);
        chip("##warnings", "Warnings", dotAmber, &m_ShowWarnings, "console.showWarnings", false);
        chip("##errors", "Errors", dotRed, &m_ShowErrors, "console.showErrors", true);

        // Search bar: starts right after the chips and stretches to the row's
        // right edge (minus padding), down to a 150px minimum. When it stretches
        // (>= 150px of room) the strip exactly fills the row, so nothing scrolls;
        // when the window is too narrow for 150px it's clamped to 150px and the
        // strip overflows, enabling horizontal scroll.
        const float chipsW = cursorX - stripStartX;  // unscrolled
        // The menu cell at the right end, as wide as the other icon cells.
        const float moreW = ImGui::CalcTextSize(ICON_TI_DOTS_VERTICAL).x + 22.0f * dpi;
        const float availFilter = trackW - chipsW - gapFilter - padR - moreW;  // room to stretch
        const float filterW = std::max(minFilterW, availFilter);
        const float filterX = cursorX + gapFilter;  // right after chips
        cursorX = filterX + filterW + padR;         // trailing pad
        const float moreX = cursorX;
        cursorX += moreW;
        const float contentW = cursorX - stripStartX;

        // Search bar - borderless and full toolbar height, like the hierarchy
        // search: a dim magnifier + a transparent input, no box. The input frame
        // is padded to fill the whole row so its height/hit-area match the
        // toolbar, with the text kept vertically centered.
        ImFont* mf = g_FontUISmall ? g_FontUISmall : ImGui::GetFont();
        const float mfSize = Fonts::CssPx(mf, 14.0f) * dpi;
        const ImVec2 magSz = mf->CalcTextSizeA(mfSize, FLT_MAX, 0.0f, ICON_TI_SEARCH);
        const float magX = filterX + 4.0f * dpi;
        dl->AddText(mf, mfSize, ImVec2(magX, rowMin.y + (rowH - magSz.y) * 0.5f),
                    ImGui::ColorConvertFloat4ToU32(Palette::Dim2), ICON_TI_SEARCH);

        const float inputX = magX + magSz.x + 6.0f * dpi;
        const float framePadY = std::max(0.0f, (rowH - ImGui::GetFontSize()) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(inputX, rowMin.y));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, framePadY));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, Palette::Dim2);
        ImGui::SetNextItemWidth((filterX + filterW) - inputX);
        ImGui::InputTextWithHint("##filter", "Filter…", m_FilterText, sizeof(m_FilterText));
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);

        // More: the levels for working on the editor itself.
        cursorX = moreX;
        drawDivider(cursorX);
        iconBtn("##more", ICON_TI_DOTS_VERTICAL, "More filters", false, [&] { ImGui::OpenPopup("##consolemore"); });

        ImGui::PopClipRect();

        // Under the button, right edges aligned, so it opens toward the panel.
        ImGui::SetNextWindowPos(ImVec2(moreX + moreW, rowMax.y), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
        PushContextMenuPopupStyle();
        if (ImGui::BeginPopup("##consolemore"))
        {
            PushContextMenuItemStyle();
            if (ImGui::MenuItem("Editor messages", nullptr, &m_ShowEditor))
            {
                EditorSettings::SetBool("console.showEditor", m_ShowEditor);
            }
            if (ImGui::MenuItem("Debug messages", nullptr, &m_ShowDebug))
            {
                EditorSettings::SetBool("console.showDebug", m_ShowDebug);
            }
            ImGui::PopStyleVar();  // ItemSpacing
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();  // WindowPadding

        // Wheel over the toolbar scrolls the strip horizontally (vertical or
        // horizontal wheel), clamped to the overflow - like dock-tab scrolling.
        const float maxScroll = std::max(0.0f, contentW - trackW);
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 mp = io.MousePos;
        const bool overTrack = mp.x >= rowMin.x && mp.x < rowMax.x && mp.y >= rowMin.y && mp.y < rowMax.y;
        if (overTrack && maxScroll > 0.0f && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f))
        {
            const float wheel = (io.MouseWheelH != 0.0f) ? io.MouseWheelH : io.MouseWheel;
            m_ToolbarScroll -= wheel * 40.0f * dpi;
            io.MouseWheel = 0.0f;  // consume so the panel/log doesn't also scroll
            io.MouseWheelH = 0.0f;
        }
        m_ToolbarScroll = std::max(0.0f, std::min(m_ToolbarScroll, maxScroll));

        // Reserve the row in layout flow.
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMax.y));
        // 1px bottom hairline (full row, drawn after the strip clip is popped).
        dl->AddLine(ImVec2(rowMin.x, rowMax.y), ImVec2(rowMax.x, rowMax.y), colLine, 1.0f);
    }
    // The toolbar already draws its own 1px bottom hairline above; the log
    // child starts immediately under it (no gap / extra separator).

    // Log entries with colors (no horizontal scrollbar since we wrap text)
    ImGui::BeginChild("LogEntries", ImVec2(0, 0), false);

    // Zero the vertical item spacing so consecutive entry rows sit flush - each
    // row reserves its own InvisibleButton and the default ItemSpacing.y would
    // otherwise leave an unpainted gap between the zebra/severity backgrounds.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));

    std::lock_guard<std::mutex> lock(m_Mutex);

    std::string filterStr(m_FilterText);
    std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(), ::tolower);

    // Everything that does not depend on the entry is computed once per frame.
    // All sizes are logical px scaled by the window DPI - the custom draw
    // here bypasses ImGui's auto-DPI font scaling, so without this the log
    // text renders at 1x while the rest of the UI scales up (tiny text).
    const float dpi = m_App->Dpi(1.0f);
    const float gap = 12.0f * dpi;
    const float rowPadX = 16.0f * dpi;
    const float rowPadY = 10.0f * dpi;
    const float availWidth = ImGui::GetContentRegionAvail().x;

    ImFont* badgeFont = g_FontUIBold ? g_FontUIBold : ImGui::GetFont();
    ImFont* monoFont = g_FontMonoSmall ? g_FontMonoSmall : ImGui::GetFont();

    // Sizes are CSS px (matching the mockup). Convert per-font via CssPx:
    // ImGui's FreeType sizes glyphs by REAL_DIM (max extent) while CSS uses
    // the em-square, so a raw 12.5 renders ~1.3x smaller than a browser's
    // 12.5px. Then × dpi, since explicit AddText sizes bypass FontScaleDpi.
    const float badgeSize = Fonts::CssPx(badgeFont, 11.0f) * dpi;
    const float monoSize = Fonts::CssPx(monoFont, 11.5f) * dpi;
    const float bodySize = Fonts::CssPx(monoFont, 12.5f) * dpi;

    // Severity color + uppercase badge label (mockup line 549-553), and each
    // badge's width, measured once here rather than once per row.
    struct Severity
    {
        ImVec4 color;
        const char* badge;
        float width;
    };
    auto makeSeverity = [&](const ImVec4& c, const char* b)
    { return Severity{ c, b, badgeFont->CalcTextSizeA(badgeSize, FLT_MAX, 0.0f, b).x }; };
    const Severity sevEditor = makeSeverity(Palette::Accent, "EDITOR");
    const Severity sevDebug = makeSeverity(Palette::Dim, "DEBUG");
    const Severity sevWarn = makeSeverity(Palette::Amber, "WARN");
    const Severity sevError = makeSeverity(Palette::Red, "ERROR");
    const Severity sevInfo = makeSeverity(Palette::Fg, "INFO");
    const float timeW = monoFont->CalcTextSizeA(monoSize, FLT_MAX, 0.0f, "00:00:00").x;
    // One badge column for every row, as wide as the widest label, so the
    // time and message line up down the list whatever the severity.
    const float badgeW = std::max({ sevEditor.width, sevDebug.width, sevWarn.width, sevError.width, sevInfo.width });

    // Rows outside this band are advanced past with their cached height and
    // never formatted, measured or drawn. The console used to do all of that
    // for every one of its (up to) 1000 entries every frame.
    const float viewTop = ImGui::GetWindowPos().y - 64.0f;
    const float viewBottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y + 64.0f;

    int entryIndex = 0;
    for (int vecIndex = 0; vecIndex < (int)m_Entries.size(); ++vecIndex)
    {
        const auto& entry = m_Entries[vecIndex];
        if (!PassesFilter(entry, filterStr))
        {
            continue;
        }

        const Severity* sev = &sevInfo;
        switch (entry.level)
        {
            case ::Deki::LogLevel::Editor: sev = &sevEditor; break;
            case ::Deki::LogLevel::Debug: sev = &sevDebug; break;
            case ::Deki::LogLevel::Warning: sev = &sevWarn; break;
            case ::Deki::LogLevel::Error: sev = &sevError; break;
            case ::Deki::LogLevel::Info:
            default: sev = &sevInfo; break;
        }
        const ImVec4 sevColor = sev->color;
        const char* badge = sev->badge;

        ImGui::PushID(vecIndex);

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float bodyX = origin.x + rowPadX + badgeW + gap + timeW + gap;
        const float bodyW = std::max(40.0f, origin.x + availWidth - bodyX - rowPadX);
        const int shownCount = (m_Collapse && entry.count > 1) ? entry.count : 1;

        // Build the message body. Collapse-count prefix stays attached.
        auto buildBody = [&](std::string& out)
        {
            if (shownCount > 1)
            {
                char countBuf[16];
                snprintf(countBuf, sizeof(countBuf), "(%d) ", shownCount);
                out = std::string(countBuf) + entry.message;
            }
            else
            {
                out = entry.message;
            }
        };

        // Row height: from the cache when it was measured at this wrap width
        // and collapse count, else measured now (once per entry per width).
        std::string body;
        ImVec2 bodySize2;
        if (entry.cachedRowHeight >= 0.0f && entry.cachedRowWidth == bodyW && entry.cachedRowCount == shownCount)
        {
            bodySize2 = ImVec2(bodyW, entry.cachedRowHeight - rowPadY * 2.0f);
        }
        else
        {
            buildBody(body);
            bodySize2 = monoFont->CalcTextSizeA(bodySize, FLT_MAX, bodyW, body.c_str());
            entry.cachedRowHeight = bodySize2.y + rowPadY * 2.0f;
            entry.cachedRowWidth = bodyW;
            entry.cachedRowCount = shownCount;
        }
        const float rowHeight = entry.cachedRowHeight;

        // Off-screen: reserve the space and move on.
        if (origin.y + rowHeight < viewTop || origin.y > viewBottom)
        {
            ImGui::Dummy(ImVec2(availWidth, rowHeight));
            ImGui::PopID();
            entryIndex++;
            continue;
        }
        if (body.empty())
        {
            buildBody(body);
        }

        // Format timestamp HH:MM:SS - mono dim.
        auto time_t_val = std::chrono::system_clock::to_time_t(entry.timestamp);
        std::tm tm_val;
#ifdef _WIN32
        localtime_s(&tm_val, &time_t_val);
#else
        localtime_r(&time_t_val, &tm_val);
#endif
        char timeBuf[16];
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);

        // Zebra striping: every other visible row gets the slightly-lighter
        // alt-row background (Palette::Bg2). entryIndex counts only displayed
        // rows, so the pattern is colored / plain / colored / plain.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (entryIndex % 2 == 0)
        {
            dl->AddRectFilled(origin, ImVec2(origin.x + availWidth, origin.y + rowHeight),
                              ImGui::ColorConvertFloat4ToU32(Palette::Bg2));
        }

        // Row bg + left border per severity (layered on top of the zebra fill).
        if (entry.level == ::Deki::LogLevel::Error)
        {
            const ImU32 errBg = IM_COL32(232, 90, 80, 18);
            const ImU32 errLine = ImGui::ColorConvertFloat4ToU32(Palette::Red);
            dl->AddRectFilled(origin, ImVec2(origin.x + availWidth, origin.y + rowHeight), errBg);
            dl->AddRectFilled(origin, ImVec2(origin.x + 3.0f * dpi, origin.y + rowHeight), errLine);
        }
        else if (entry.level == ::Deki::LogLevel::Warning)
        {
            const ImU32 warnLine = ImGui::ColorConvertFloat4ToU32(Palette::Amber);
            dl->AddRectFilled(origin, ImVec2(origin.x + 3.0f * dpi, origin.y + rowHeight), warnLine);
        }

        // Selection highlight (translucent accent), on top of zebra/severity.
        const bool isSelected = m_SelectedRows.count(vecIndex) > 0;
        if (isSelected)
        {
            dl->AddRectFilled(origin, ImVec2(origin.x + availWidth, origin.y + rowHeight), IM_COL32(59, 195, 255, 45));
        }

        // Cell positions.
        const float badgeX = origin.x + rowPadX;
        const float badgeY = origin.y + rowPadY + 2.0f * dpi;  // small top inset per mockup
        const float timeX = badgeX + badgeW + gap;
        const float timeY = origin.y + rowPadY + (bodySize - monoSize) * 0.5f;
        const float bodyY = origin.y + rowPadY;

        const ImU32 sevU32 = ImGui::ColorConvertFloat4ToU32(sevColor);
        const ImU32 dimU32 = ImGui::ColorConvertFloat4ToU32(Palette::Dim);

        dl->AddText(badgeFont, badgeSize, ImVec2(badgeX, badgeY), sevU32, badge);
        dl->AddText(monoFont, monoSize, ImVec2(timeX, timeY), dimU32, timeBuf);
        dl->PushClipRect(ImVec2(bodyX, bodyY), ImVec2(bodyX + bodyW, bodyY + bodySize2.y + 1.0f), true);
        dl->AddText(monoFont, bodySize, ImVec2(bodyX, bodyY), sevU32, body.c_str(), nullptr, bodyW);
        dl->PopClipRect();

        // Reserve the row space + add an invisible button so hit-testing for
        // double-click / context-menu still works on the message area.
        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##logrow", ImVec2(availWidth, rowHeight));

        // Double-click to open source file in VS Code
        bool handledDoubleClick = false;
        if (entry.sourceLine > 0 && !entry.sourceFile.empty() && ImGui::IsItemHovered() &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            // Use full path directly if it exists, otherwise search by filename
            std::string fullPath = entry.sourceFile;
            if (!std::filesystem::exists(fullPath))
            {
                // Extract filename and search for it
                std::string filename = std::filesystem::path(fullPath).filename().string();
                fullPath = FindSourceFile(filename);
            }
            if (!fullPath.empty())
            {
                OpenInVSCode(fullPath, entry.sourceLine);
                handledDoubleClick = true;
            }
        }

        // Single click: row selection (Ctrl toggles, Shift selects the visible
        // range from the anchor) plus the existing scene-object select.
        if (!handledDoubleClick && ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            ImGuiIO& io = ImGui::GetIO();
            if (io.KeyShift && m_LastClickedRow >= 0)
            {
                m_SelectedRows.clear();
                const int lo = std::min(m_LastClickedRow, vecIndex);
                const int hi = std::max(m_LastClickedRow, vecIndex);
                for (int i = lo; i <= hi && i < (int)m_Entries.size(); ++i)
                {
                    if (PassesFilter(m_Entries[i], filterStr))
                    {
                        m_SelectedRows.insert(i);
                    }
                }
            }
            else if (io.KeyCtrl)
            {
                if (m_SelectedRows.count(vecIndex))
                {
                    m_SelectedRows.erase(vecIndex);
                }
                else
                {
                    m_SelectedRows.insert(vecIndex);
                }
                m_LastClickedRow = vecIndex;
            }
            else
            {
                m_SelectedRows.clear();
                m_SelectedRows.insert(vecIndex);
                m_LastClickedRow = vecIndex;
            }

            if (entry.objectId != 0)
            {
                auto* scene = m_App->GetCurrentScene();
                if (scene)
                {
                    Deki::Object* obj = scene->FindDekiObject(entry.objectId);
                    if (obj)
                    {
                        m_App->SetSelectedObject(obj);
                    }
                }
            }
        }

        // Show tooltip for entries with associated objects or source info
        if (ImGui::IsItemHovered())
        {
            bool hasObject = false;
            std::string objectName;
            if (entry.objectId != 0)
            {
                auto* scene = m_App->GetCurrentScene();
                if (scene)
                {
                    Deki::Object* obj = scene->FindDekiObject(entry.objectId);
                    if (obj)
                    {
                        hasObject = true;
                        objectName = obj->GetName();
                    }
                }
            }
            bool hasSource = entry.sourceLine > 0 && !entry.sourceFile.empty();

            std::string displayFilename = hasSource ? std::filesystem::path(entry.sourceFile).filename().string() : "";

            if (hasObject && hasSource)
            {
                DekiEditor::Tooltip("Click to select '%s' | Double-click to open %s:%d", objectName.c_str(),
                                    displayFilename.c_str(), entry.sourceLine);
            }
            else if (hasObject)
            {
                DekiEditor::Tooltip("Click to select '%s'", objectName.c_str());
            }
            else if (hasSource)
            {
                DekiEditor::Tooltip("Double-click to open %s:%d", displayFilename.c_str(), entry.sourceLine);
            }
        }

        // Right-click context menu for copying (use unique ID per entry)
        char popupId[32];
        snprintf(popupId, sizeof(popupId), "log_ctx_%d", vecIndex);
        PushContextMenuPopupStyle();
        if (ImGui::BeginPopupContextItem(popupId))
        {
            PushContextMenuItemStyle();
            // "Copy" copies the whole selection when right-clicking a selected
            // row, otherwise just this entry's message.
            const bool copySelection = isSelected && m_SelectedRows.size() > 1;
            if (ImGui::MenuItem(copySelection ? "Copy selected" : "Copy"))
            {
                if (copySelection)
                {
                    std::string out;
                    for (int idx : m_SelectedRows)
                    {
                        if (idx >= 0 && idx < (int)m_Entries.size())
                        {
                            out += FormatLogLine(m_Entries[idx]) + "\n";
                        }
                    }
                    ImGui::SetClipboardText(out.c_str());
                }
                else
                {
                    ImGui::SetClipboardText(entry.message.c_str());
                }
            }
            if (ImGui::MenuItem("Copy All (Visible)"))
            {
                std::string allText;
                for (const auto& e : m_Entries)
                {
                    if (PassesFilter(e, filterStr))
                    {
                        allText += FormatLogLine(e) + "\n";
                    }
                }
                ImGui::SetClipboardText(allText.c_str());
            }
            ImGui::PopStyleVar();  // context menu ItemSpacing
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();  // context menu WindowPadding

        ImGui::PopID();
        entryIndex++;
    }

    // Ctrl+C copies the selected entries (sorted ascending, one formatted line
    // each). Gated on the log child being focused (clicking a row focuses it) so
    // it doesn't hijack Ctrl+C while the search box is being edited.
    if (!m_SelectedRows.empty() && ImGui::IsWindowFocused() && ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_C, false))
    {
        std::string out;
        for (int idx : m_SelectedRows)  // std::set iterates ascending
        {
            if (idx >= 0 && idx < (int)m_Entries.size())
            {
                out += FormatLogLine(m_Entries[idx]) + "\n";
            }
        }
        if (!out.empty())
        {
            ImGui::SetClipboardText(out.c_str());
        }
    }

    // Auto-scroll to bottom
    if (m_AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
    {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::PopStyleVar();  // ItemSpacing
    ImGui::EndChild();

    ImGui::End();
}

void ConsolePanel::Clear()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(), [](const LogEntry& e) { return !e.pinned; }),
                    m_Entries.end());
    m_SelectedRows.clear();
    m_LastClickedRow = -1;
}

int ConsolePanel::GetErrorCount()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    int n = 0;
    for (const auto& e : m_Entries)
    {
        if (e.level == ::Deki::LogLevel::Error)
        {
            ++n;
        }
    }
    return n;
}

int ConsolePanel::GetBuildErrorCount()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    int n = 0;
    for (const auto& e : m_Entries)
    {
        if (e.level == ::Deki::LogLevel::Error && e.pinned)
        {
            ++n;
        }
    }
    return n;
}

void ConsolePanel::LogBuildError(const std::string& message, const std::string& buildKey)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    LogEntry entry;
    entry.message = message;
    entry.messageLower = message;
    std::transform(entry.messageLower.begin(), entry.messageLower.end(), entry.messageLower.begin(), ::tolower);
    entry.level = ::Deki::LogLevel::Error;
    entry.count = 1;
    entry.timestamp = std::chrono::system_clock::now();
    entry.pinned = true;
    entry.buildKey = buildKey;
    m_Entries.push_back(std::move(entry));
}

void ConsolePanel::ClearBuildErrors()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(), [](const LogEntry& e) { return e.pinned; }),
                    m_Entries.end());
    m_SelectedRows.clear();
    m_LastClickedRow = -1;
}

void ConsolePanel::ClearBuildErrorsForPackage(const std::string& buildKey)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
                                   [&buildKey](const LogEntry& e) { return e.pinned && e.buildKey == buildKey; }),
                    m_Entries.end());
    m_SelectedRows.clear();
    m_LastClickedRow = -1;
}

void ConsolePanel::AddEntry(const std::string& message, ::Deki::LogLevel level, uint32_t objectId,
                            const std::string& sourceFile, int sourceLine)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    // Check if we should collapse with previous entry
    if (m_Collapse && !m_Entries.empty())
    {
        auto& last = m_Entries.back();
        if (last.message == message && last.level == level)
        {
            last.count++;
            return;
        }
    }

    LogEntry entry;
    entry.message = message;
    entry.messageLower = message;
    std::transform(entry.messageLower.begin(), entry.messageLower.end(), entry.messageLower.begin(), ::tolower);
    entry.level = level;
    entry.count = 1;
    entry.timestamp = std::chrono::system_clock::now();
    entry.objectId = objectId;
    entry.sourceFile = sourceFile;
    entry.sourceLine = sourceLine;
    m_Entries.push_back(std::move(entry));

    // Limit entries to prevent memory growth
    const size_t maxEntries = 1000;
    if (m_Entries.size() > maxEntries)
    {
        const int removed = (int)(m_Entries.size() - maxEntries);
        m_Entries.erase(m_Entries.begin(), m_Entries.begin() + removed);

        // The front trim shifts every index down by `removed`; keep the row
        // selection pointing at the same entries (dropping any that fell off).
        if (!m_SelectedRows.empty())
        {
            std::set<int> shifted;
            for (int idx : m_SelectedRows)
            {
                if (idx - removed >= 0)
                {
                    shifted.insert(idx - removed);
                }
            }
            m_SelectedRows.swap(shifted);
        }
        if (m_LastClickedRow >= 0)
        {
            m_LastClickedRow = (m_LastClickedRow >= removed) ? (m_LastClickedRow - removed) : -1;
        }
    }
}

void ConsolePanel::Log(const std::string& message)
{
    AddEntry(message, ::Deki::LogLevel::Info);
}

void ConsolePanel::LogEditor(const std::string& message)
{
    AddEntry(message, ::Deki::LogLevel::Editor);
}

void ConsolePanel::LogDebug(const std::string& message)
{
    AddEntry(message, ::Deki::LogLevel::Debug);
}

void ConsolePanel::LogWarning(const std::string& message)
{
    AddEntry(message, ::Deki::LogLevel::Warning);
}

void ConsolePanel::LogError(const std::string& message)
{
    AddEntry(message, ::Deki::LogLevel::Error);
}

void ConsolePanel::LogWithLevel(::Deki::LogLevel level, const std::string& message)
{
    AddEntry(message, level);
}

void ConsolePanel::LogStatic(const std::string& message)
{
    if (s_Instance)
    {
        s_Instance->Log(message);
    }
}

void ConsolePanel::LogDebugStatic(const std::string& message)
{
    if (s_Instance)
    {
        s_Instance->LogDebug(message);
    }
}

void ConsolePanel::LogWarningStatic(const std::string& message)
{
    if (s_Instance)
    {
        s_Instance->LogWarning(message);
    }
}

void ConsolePanel::LogErrorStatic(const std::string& message)
{
    if (s_Instance)
    {
        s_Instance->LogError(message);
    }
}

void ConsolePanel::EngineLogCallback(::Deki::LogLevel level, const std::string& message, const char* file, int line)
{
    if (s_Instance)
    {
        uint32_t objectId = Deki::LogSystem::GetLogContextObjectId();

        // Strip the "filename:line -> " prefix from the display message
        std::string displayMessage = message;
        size_t arrowPos = message.find(" -> ");
        if (arrowPos != std::string::npos)
        {
            displayMessage = message.substr(arrowPos + 4);
        }

        // Use the full file path directly from __FILE__
        std::string sourceFile = file ? file : "";
        s_Instance->AddEntry(displayMessage, level, objectId, sourceFile, line);
    }
}

std::string ConsolePanel::FindSourceFile(const std::string& filename) const
{
    namespace fs = std::filesystem;
    if (filename.empty())
    {
        return "";
    }

    auto searchDir = [&](const std::string& dir) -> std::string
    {
        if (dir.empty() || !fs::exists(dir))
        {
            return "";
        }
        try
        {
            for (const auto& entry : fs::recursive_directory_iterator(dir))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }
                if (entry.path().filename().string() == filename)
                {
                    return entry.path().string();
                }
            }
        }
        catch (const fs::filesystem_error&)
        {
        }
        return "";
    };

    // Search package source directories first
    auto* packageLoader = m_App->GetPackageLoader();
    if (packageLoader)
    {
        for (const auto& pkg : packageLoader->GetLoadedPackages())
        {
            std::string projectPath = EditorSettings::GetLastProjectPath();
            std::string sourceDir = PackageLoader::ResolvePackageSourceDir(projectPath, pkg.dllFilename);
            std::string result = searchDir(sourceDir);
            if (!result.empty())
            {
                return result;
            }
        }
    }

    // Search project source directory
    auto* project = m_App->GetProject();
    if (project)
    {
        std::string result = searchDir(project->GetSrcPath());
        if (!result.empty())
        {
            return result;
        }
    }

    return "";
}

void ConsolePanel::OpenInVSCode(const std::string& filePath, int line)
{
    // Argument vector, not a command string: a source path can contain a quote
    // on POSIX, and the old system() form let that run as a shell command.
    OpenInCodeEditor(filePath, line);
}

}  // namespace DekiEditor
