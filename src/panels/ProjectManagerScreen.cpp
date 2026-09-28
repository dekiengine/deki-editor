#include "ProjectManagerScreen.h"

#include "EditorApp.h"
#include "EditorVersion.h"
#include "services/FileDialogs.h"
#include <deki-editor/ProcessLaunch.h>
#include "services/ProjectRegistry.h"

#include <deki-editor/EditorTheme.h>
#include <deki-editor/IconsTabler.h>

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

// The last-opened stamp, in the words a person would use. Deliberately coarse
// past a day - the list is sorted by recency, so the exact hour of a project
// touched last month tells the reader nothing.
std::string RelativeTime(int64_t stamp)
{
    if (stamp <= 0)
        return "never opened";

    using namespace std::chrono;
    const int64_t now = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    const int64_t delta = now - stamp;

    char buf[64];
    if (delta < 60) return "just now";
    if (delta < 3600)
    {
        std::snprintf(buf, sizeof(buf), "%lld min ago", (long long)(delta / 60));
        return buf;
    }
    if (delta < 86400)
    {
        std::snprintf(buf, sizeof(buf), "%lld hours ago", (long long)(delta / 3600));
        return buf;
    }
    if (delta < 172800) return "yesterday";
    if (delta < 2592000)
    {
        std::snprintf(buf, sizeof(buf), "%lld days ago", (long long)(delta / 86400));
        return buf;
    }
    if (delta < 31536000)
    {
        std::snprintf(buf, sizeof(buf), "%lld months ago", (long long)(delta / 2592000));
        return buf;
    }
    return "over a year ago";
}

bool ContainsNoCase(const std::string& haystack, const std::string& needle)
{
    if (needle.empty())
        return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char a, char b)
                          {
                              return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
                          });
    return it != haystack.end();
}

// Draw text in one of the theme fonts at a CSS-px size. PushFont is what
// applies FontScaleDpi, and GetFontSize() reads back the physical size that
// AddText wants - the same dance the menu bar's window controls do.
void TextAt(ImDrawList* dl, ImFont* font, float cssPx, ImVec2 pos, ImU32 col, const char* text)
{
    ImGui::PushFont(font ? font : ImGui::GetFont(), cssPx);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos, col, text);
    ImGui::PopFont();
}

ImVec2 MeasureIn(ImFont* font, float cssPx, const char* text)
{
    ImGui::PushFont(font ? font : ImGui::GetFont(), cssPx);
    const ImVec2 size = ImGui::CalcTextSize(text);
    ImGui::PopFont();
    return size;
}

// Secondary text on this screen: paths, times, labels. Palette::Dim is meant
// for chrome in dense panels and is too faint for text a person reads here.
constexpr ImVec4 kSecondary = ImVec4(0.72f, 0.71f, 0.69f, 1.0f);  // #b8b5b0

// A small filled label - the platform id, the engine version, or the
// MISSING marker. Right-aligned to rightEdge; returns the width it took.
float DrawChip(ImDrawList* dl, ImVec2 rightEdge, ImU32 fg, ImU32 border, const char* text, float S)
{
    const ImVec2 textSize = MeasureIn(g_FontUISmall, 12.0f, text);
    const float padX = 8.0f * S;
    const float padY = 4.0f * S;

    const ImVec2 max(rightEdge.x, rightEdge.y + textSize.y + padY * 2.0f);
    const ImVec2 min(max.x - (textSize.x + padX * 2.0f), rightEdge.y);

    dl->AddRectFilled(min, max, ImGui::GetColorU32(Palette::Line), 4.0f * S);
    dl->AddRect(min, max, border, 4.0f * S);
    TextAt(dl, g_FontUISmall, 12.0f, ImVec2(min.x + padX, min.y + padY), fg, text);
    return max.x - min.x;
}

}  // namespace

void ProjectManagerScreen::OnShown()
{
    ProjectRegistry::Get().Refresh();
    m_Error.clear();
    m_PendingOpen.clear();

    // Preselect the most recent project so Enter alone reopens what the user
    // was last in - the common case for a screen passed through every launch.
    m_Selected = ProjectRegistry::Get().Entries().empty() ? -1 : 0;
}

std::string ProjectManagerScreen::ConsumePendingOpen()
{
    std::string path;
    path.swap(m_PendingOpen);
    return path;
}

void ProjectManagerScreen::RebuildFilter()
{
    m_Filtered.clear();
    const std::string needle = m_Search;
    const auto& entries = ProjectRegistry::Get().Entries();
    for (int i = 0; i < (int)entries.size(); ++i)
    {
        if (ContainsNoCase(entries[i].name, needle) || ContainsNoCase(entries[i].path, needle))
            m_Filtered.push_back(i);
    }

    // A selection filtered out of view would leave Open and Remove acting on a
    // row that is not on screen.
    if (std::find(m_Filtered.begin(), m_Filtered.end(), m_Selected) == m_Filtered.end())
        m_Selected = m_Filtered.empty() ? -1 : m_Filtered.front();
}

void ProjectManagerScreen::ImportProject()
{
    // The result lands on a later frame, so everything it touches is member
    // state the next draw reads.
    FileDialogs::OpenFolder("Select a Deki project folder",
                            [this](const std::string& path)
                            {
                                if (path.empty())
                                    return;

                                if (!ProjectRegistry::IsProjectDir(path))
                                {
                                    m_Error = fs::path(path).filename().string() +
                                              " is not a Deki project - there is no deki.json in "
                                              "that folder.";
                                    return;
                                }

                                m_Error.clear();
                                ProjectRegistry::Get().Add(path);
                                m_PendingOpen = path;
                            });
}

void ProjectManagerScreen::Draw()
{
    const float S = ImGui::GetStyle().FontScaleDpi;

    RebuildFilter();

    const float pad = 40.0f * S;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 start = ImGui::GetCursorPos();

    // One centred column, capped in width: on a large monitor a full-width list
    // puts the name and the date a metre apart and leaves the middle empty.
    const float contentW = std::min(avail.x - pad * 2.0f, 960.0f * S);

    // The body is as tall as the projects need, not the window. Sized from the
    // whole list rather than the filtered one, so typing in the search box does
    // not make the column jump.
    const float headerH = 81.0f * S;
    const float rowStride = 82.0f * S;  // card height + gap, see DrawProjectList
    const float railMinH = 290.0f * S;
    const size_t entryCount = ProjectRegistry::Get().Entries().size();
    const float availBodyH = avail.y - headerH - pad * 1.4f;
    float bodyH = entryCount == 0
        ? 260.0f * S
        : std::max(railMinH, (float)entryCount * rowStride + 8.0f * S + (m_Error.empty() ? 0.0f : 26.0f * S));
    bodyH = std::max(std::min(bodyH, availBodyH), 120.0f * S);

    // Sit slightly above the middle, where the eye lands first.
    const float blockH = headerH + bodyH;
    const float top = std::max(pad * 0.7f, (avail.y - blockH) * 0.42f);
    ImGui::SetCursorPos(ImVec2(start.x + std::max(pad, (avail.x - contentW) * 0.5f), start.y + top));

    ImGui::BeginGroup();

    // --- Header: title on the left, search on the right ---
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        TextAt(dl, g_FontDisplay, 28.0f, origin, ImGui::GetColorU32(Palette::Fg), "Projects");

        const size_t total = ProjectRegistry::Get().Entries().size();
        char subtitle[96];
        std::snprintf(subtitle, sizeof(subtitle), "%zu project%s on this machine",
                      total, total == 1 ? "" : "s");
        TextAt(dl, g_FontUISmall, 13.0f, ImVec2(origin.x, origin.y + 36.0f * S),
               ImGui::GetColorU32(kSecondary), subtitle);

        // Search field, right-aligned with the list below it.
        const float searchW = 240.0f * S;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + contentW - searchW, origin.y + 6.0f * S));
        ImGui::SetNextItemWidth(searchW);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * S, 7.0f * S));
        // Deliberately not focused on arrival: with the search box active,
        // ImGui swallows the arrow keys and Enter that drive the list below.
        ImGui::InputTextWithHint("##ProjectSearch", ICON_TI_SEARCH "  Search projects",
                                 m_Search, sizeof(m_Search));
        ImGui::PopStyleVar();

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 62.0f * S));

        // Hairline under the header, matching the menu bar's separator weight.
        const float lineY = std::floor(ImGui::GetCursorScreenPos().y);
        dl->AddRectFilled(ImVec2(origin.x, lineY), ImVec2(origin.x + contentW, lineY + 1.0f),
                          ImGui::GetColorU32(Palette::Line2));
    }

    ImGui::Dummy(ImVec2(0.0f, 18.0f * S));

    // --- Body: the project list beside the action rail ---
    const float railW = 200.0f * S;
    const float gap = 20.0f * S;
    const float listW = contentW - railW - gap;

    if (entryCount == 0)
    {
        DrawEmptyState(contentW, bodyH);
    }
    else
    {
        DrawProjectList(listW, bodyH);
        ImGui::SameLine(0.0f, gap);
        DrawActionRail(railW, bodyH);
    }

    ImGui::EndGroup();

    // --- Keyboard: the screen is a list, so it should behave like one ---
    if (!ImGui::IsAnyItemActive() && !m_Filtered.empty())
    {
        auto pos = std::find(m_Filtered.begin(), m_Filtered.end(), m_Selected);
        const int cursor = (pos == m_Filtered.end()) ? 0 : (int)(pos - m_Filtered.begin());

        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            m_Selected = m_Filtered[std::min(cursor + 1, (int)m_Filtered.size() - 1)];
        else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            m_Selected = m_Filtered[std::max(cursor - 1, 0)];
        else if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
        {
            const auto& entries = ProjectRegistry::Get().Entries();
            if (m_Selected >= 0 && m_Selected < (int)entries.size() && !entries[m_Selected].missing)
                m_PendingOpen = entries[m_Selected].path;
        }
    }
}

void ProjectManagerScreen::DrawProjectList(float width, float height)
{
    const float S = ImGui::GetStyle().FontScaleDpi;
    const auto& entries = ProjectRegistry::Get().Entries();

    // Room for the error line, when there is one, so the list does not resize
    // under the cursor as it appears and clears.
    const float errorH = m_Error.empty() ? 0.0f : 26.0f * S;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, Palette::Bg);
    ImGui::BeginChild("##ProjectList", ImVec2(width, height - errorH), ImGuiChildFlags_None);

    if (m_Filtered.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kSecondary);
        ImGui::SetCursorPos(ImVec2(4.0f * S, 12.0f * S));
        ImGui::TextUnformatted("No project matches that search.");
        ImGui::PopStyleColor();
    }

    const float rowH = 72.0f * S;
    const float rounding = 6.0f * S;

    // Each project is a card with its own fill and outline, so rows read as
    // separate items rather than one striped block. The Selectable only
    // provides the hit box; the card draws every state itself.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 10.0f * S));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));

    for (int idx : m_Filtered)
    {
        const ProjectEntry& entry = entries[idx];
        ImGui::PushID(idx);

        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float rowW = ImGui::GetContentRegionAvail().x;
        const bool selected = (m_Selected == idx);

        if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(rowW, rowH)))
        {
            m_Selected = idx;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !entry.missing)
                m_PendingOpen = entry.path;
        }

        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            m_Selected = idx;

        PushContextMenuPopupStyle();
        const bool rowMenu = ImGui::BeginPopupContextItem("##rowMenu");
        ImGui::PopStyleVar();  // WindowPadding, baked by the Begin
        if (rowMenu)
        {
            PushContextMenuItemStyle();
            if (ImGui::MenuItem("Open", nullptr, false, !entry.missing))
                m_PendingOpen = entry.path;
            if (ImGui::MenuItem("Show in File Explorer", nullptr, false, !entry.missing))
                RevealInFileManager(entry.path);
            ImGui::Separator();
            if (ImGui::MenuItem("Remove from List"))
            {
                // Removing invalidates idx and the entries reference for the
                // rest of this frame, so the row loop is left immediately.
                ProjectRegistry::Get().Remove(entry.path);
                m_Selected = -1;
                ImGui::PopStyleVar();  // ItemSpacing
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            ImGui::PopStyleVar();  // ItemSpacing
            ImGui::EndPopup();
        }

        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 rowMax(rowMin.x + rowW, rowMin.y + rowH);

        // Card: fill, then an outline that brightens on hover and turns accent
        // when selected.
        {
            const ImVec4 border = selected ? Palette::Accent : (hovered ? Palette::Line3 : Palette::Line2);

            dl->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(hovered ? Palette::Line : Palette::Bg2), rounding);
            if (selected)
                dl->AddRectFilled(rowMin, rowMax,
                                  ImGui::GetColorU32(ImVec4(Palette::Accent.x, Palette::Accent.y, Palette::Accent.z, 0.08f)),
                                  rounding);
            dl->AddRect(ImVec2(rowMin.x + 0.5f, rowMin.y + 0.5f), ImVec2(rowMax.x - 0.5f, rowMax.y - 0.5f),
                        ImGui::GetColorU32(border), rounding, 0, selected ? 1.5f * S : 1.0f);
        }

        // Icon tile.
        {
            const float tile = 40.0f * S;
            const ImVec2 tileMin(rowMin.x + 16.0f * S, rowMin.y + (rowH - tile) * 0.5f);
            const ImVec2 tileMax(tileMin.x + tile, tileMin.y + tile);
            dl->AddRectFilled(tileMin, tileMax, ImGui::GetColorU32(Palette::Bg), 6.0f * S);
            dl->AddRect(tileMin, tileMax, ImGui::GetColorU32(Palette::Line2), 6.0f * S);

            const char* icon = entry.missing ? ICON_TI_ALERT_TRIANGLE : ICON_TI_FOLDER;
            const ImVec4 iconCol = entry.missing ? Palette::Red : Palette::FolderIcon;
            const ImVec2 iconSize = MeasureIn(nullptr, 20.0f, icon);
            TextAt(dl, nullptr, 20.0f,
                   ImVec2(tileMin.x + (tile - iconSize.x) * 0.5f,
                          tileMin.y + (tile - iconSize.y) * 0.5f),
                   ImGui::GetColorU32(iconCol), icon);
        }

        const float textX = rowMin.x + 72.0f * S;

        // Right column first - the name and path clip against what it leaves.
        float rightEdge = rowMax.x - 34.0f * S;  // room for the hover chevron
        {
            const std::string when = RelativeTime(entry.lastOpened);
            const ImVec2 whenSize = MeasureIn(g_FontUISmall, 13.0f, when.c_str());
            TextAt(dl, g_FontUISmall, 13.0f,
                   ImVec2(rightEdge - whenSize.x, rowMin.y + 13.0f * S),
                   ImGui::GetColorU32(kSecondary), when.c_str());

            const float chipY = rowMin.y + 37.0f * S;
            float chipsW = 0.0f;
            if (entry.missing)
            {
                chipsW = DrawChip(dl, ImVec2(rightEdge, chipY), ImGui::GetColorU32(Palette::Red),
                                  ImGui::GetColorU32(Palette::Red), "MISSING", S);
            }
            else
            {
                if (!entry.platformId.empty())
                    chipsW = DrawChip(dl, ImVec2(rightEdge, chipY), ImGui::GetColorU32(kSecondary),
                                      ImGui::GetColorU32(Palette::Line3), entry.platformId.c_str(), S) +
                             6.0f * S;

                // The engine the project was last saved with. Amber when it is
                // not the engine this editor carries.
                if (!entry.engineVersion.empty())
                {
                    const bool matches = entry.engineVersion == EngineVersionString();
                    const ImVec4 col = matches ? kSecondary : Palette::Amber;
                    const std::string label = "Deki " + entry.engineVersion;
                    chipsW += DrawChip(dl, ImVec2(rightEdge - chipsW, chipY), ImGui::GetColorU32(col),
                                       ImGui::GetColorU32(matches ? Palette::Line3 : Palette::Amber),
                                       label.c_str(), S);
                }
            }

            rightEdge -= std::max({ whenSize.x, chipsW, 90.0f * S }) + 16.0f * S;
        }

        // Name and path, clipped so a deep path can never run into the column
        // on its right.
        dl->PushClipRect(ImVec2(textX, rowMin.y), ImVec2(rightEdge, rowMax.y), true);
        const ImVec4 nameCol = entry.missing ? Palette::Dim : Palette::Fg;
        TextAt(dl, g_FontUIMed, 16.0f, ImVec2(textX, rowMin.y + 14.0f * S),
               ImGui::GetColorU32(nameCol), entry.name.c_str());
        TextAt(dl, g_FontMonoSmall, 12.5f, ImVec2(textX, rowMin.y + 41.0f * S),
               ImGui::GetColorU32(kSecondary), entry.path.c_str());
        dl->PopClipRect();

        // Hover affordance: a chevron on the right, so the row reads as
        // something you can enter.
        if (hovered && !entry.missing)
        {
            const ImVec2 chevSize = MeasureIn(nullptr, 14.0f, ICON_TI_CHEVRON_RIGHT);
            TextAt(dl, nullptr, 14.0f,
                   ImVec2(rowMax.x - 14.0f * S - chevSize.x, rowMin.y + (rowH - chevSize.y) * 0.5f),
                   ImGui::GetColorU32(Palette::Dim), ICON_TI_CHEVRON_RIGHT);
        }

        ImGui::PopID();
    }

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (!m_Error.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Red);
        ImGui::PushFont(g_FontUISmall, 13.0f);
        ImGui::TextWrapped("%s", m_Error.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
    }
}

void ProjectManagerScreen::DrawActionRail(float width, float height)
{
    const float S = ImGui::GetStyle().FontScaleDpi;
    const auto& entries = ProjectRegistry::Get().Entries();
    const bool hasSelection = (m_Selected >= 0 && m_Selected < (int)entries.size());
    const bool canOpen = hasSelection && !entries[m_Selected].missing;

    ImGui::BeginChild("##ProjectActions", ImVec2(width, height), ImGuiChildFlags_None);

    const ImVec2 buttonSize(-FLT_MIN, 34.0f * S);

    // Primary action, filled in the accent - the one thing on this screen a
    // first-time user has to be able to find.
    ImGui::PushStyleColor(ImGuiCol_Button, Palette::Accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.36f, 0.82f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.66f, 0.90f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, Palette::Bg);
    if (ImGui::Button(ICON_TI_PLUS "  New Project", buttonSize))
        m_App->ShowNewProjectDialog();
    ImGui::PopStyleColor(4);

    ImGui::Dummy(ImVec2(0.0f, 6.0f * S));

    if (ImGui::Button(ICON_TI_FOLDER_PLUS "  Import...", buttonSize))
        ImportProject();

    ImGui::Dummy(ImVec2(0.0f, 16.0f * S));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 16.0f * S));

    ImGui::BeginDisabled(!canOpen);
    if (ImGui::Button("Open", buttonSize))
        m_PendingOpen = entries[m_Selected].path;
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0.0f, 6.0f * S));

    ImGui::BeginDisabled(!canOpen);
    if (ImGui::Button(ICON_TI_EXTERNAL_LINK "  Show in Explorer", buttonSize))
        RevealInFileManager(entries[m_Selected].path);
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0.0f, 6.0f * S));

    ImGui::BeginDisabled(!hasSelection);
    if (ImGui::Button(ICON_TI_TRASH "  Remove from List", buttonSize))
    {
        ProjectRegistry::Get().Remove(entries[m_Selected].path);
        m_Selected = -1;
    }
    ImGui::EndDisabled();

    // Removing only forgets the entry; say so, because the button carries a
    // trash can.
    ImGui::Dummy(ImVec2(0.0f, 8.0f * S));
    ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
    ImGui::PushFont(g_FontUISmall, 12.0f);
    ImGui::TextWrapped("Removing a project only takes it off this list. Nothing is deleted from disk.");
    ImGui::PopFont();
    ImGui::PopStyleColor();

    ImGui::EndChild();
}

void ProjectManagerScreen::DrawEmptyState(float width, float height)
{
    const float S = ImGui::GetStyle().FontScaleDpi;

    ImGui::BeginChild("##ProjectEmpty", ImVec2(width, height), ImGuiChildFlags_None);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float blockW = 340.0f * S;
    ImGui::SetCursorPos(ImVec2((avail.x - blockW) * 0.5f, 40.0f * S));
    ImGui::BeginGroup();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    const ImVec2 iconSize = MeasureIn(nullptr, 34.0f, ICON_TI_DEVICE_GAMEPAD);
    TextAt(dl, nullptr, 34.0f, ImVec2(origin.x + (blockW - iconSize.x) * 0.5f, origin.y),
           ImGui::GetColorU32(Palette::Line3), ICON_TI_DEVICE_GAMEPAD);

    const char* title = "No projects yet";
    const ImVec2 titleSize = MeasureIn(g_FontDisplay, 20.0f, title);
    TextAt(dl, g_FontDisplay, 20.0f,
           ImVec2(origin.x + (blockW - titleSize.x) * 0.5f, origin.y + 52.0f * S),
           ImGui::GetColorU32(Palette::Fg), title);

    const char* hint = "Create one, or import a folder that already has a deki.json.";
    const ImVec2 hintSize = MeasureIn(g_FontUISmall, 13.0f, hint);
    TextAt(dl, g_FontUISmall, 13.0f,
           ImVec2(origin.x + (blockW - hintSize.x) * 0.5f, origin.y + 84.0f * S),
           ImGui::GetColorU32(kSecondary), hint);

    ImGui::Dummy(ImVec2(blockW, 116.0f * S));

    const float buttonW = 155.0f * S;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (blockW - buttonW * 2.0f - 10.0f * S) * 0.5f);

    ImGui::PushStyleColor(ImGuiCol_Button, Palette::Accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.36f, 0.82f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.66f, 0.90f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, Palette::Bg);
    if (ImGui::Button(ICON_TI_PLUS "  New Project", ImVec2(buttonW, 34.0f * S)))
        m_App->ShowNewProjectDialog();
    ImGui::PopStyleColor(4);

    ImGui::SameLine(0.0f, 10.0f * S);
    if (ImGui::Button(ICON_TI_FOLDER_PLUS "  Import...", ImVec2(buttonW, 34.0f * S)))
        ImportProject();

    if (!m_Error.empty())
    {
        ImGui::Dummy(ImVec2(0.0f, 10.0f * S));
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Red);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + blockW);
        ImGui::TextWrapped("%s", m_Error.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    ImGui::EndGroup();
    ImGui::EndChild();
}

}  // namespace DekiEditor
