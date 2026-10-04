#pragma once

#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include "imgui.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/EditorNaming.h>
#include <nlohmann/json.hpp>

// Forward declarations
namespace Deki
{
class Component;
}
namespace Deki
{
class Scene;
}
namespace Deki
{
struct PropertyInfo;
}
namespace Deki
{
struct ComponentMeta;
}

namespace Deki
{
class Object;
}

namespace DekiEditor
{

namespace InspectorUtils
{
// Identifier-to-label conversion lives in deki-editor.dll, so package tool
// windows label reflected properties like the Inspector does.
using EditorNaming::NicifyName;
using EditorNaming::GetDisplayName;

/// True when the property's DEKI_VISIBLE_WHEN condition lets it show.
bool EvaluateVisibility(Deki::Component* comp, const Deki::PropertyInfo& prop, const Deki::ComponentMeta* meta);

/// True if `comp` is of type `targetTypeName` or derives from it, following
/// the reflected baseTypeId chain. A componentRef to a base type accepts any
/// derived component.
bool ComponentIsOrDerivesFrom(Deki::Component* comp, const char* targetTypeName);

/// Comma-separated names of the components on `obj` that are, or derive from,
/// `targetTypeName`, for ObjectRef display. Empty when there are none.
std::string GetMatchingComponentName(Deki::Object* obj, const char* targetTypeName);

/// Tooltip for the next BeginPropertyRow only. DrawProperty sets the
/// DEKI_TOOLTIP here so it reaches the row label whichever path draws the row.
/// BeginPropertyRow takes it; DrawProperty also clears it after the field, in
/// case no labelled row was drawn.
inline const char* g_NextRowTooltip = nullptr;
inline void SetNextRowTooltip(const char* tooltip)
{
    g_NextRowTooltip = tooltip;
}

/// Marks the next row as holding an unsaved value (changed since the scene
/// was last written to disk), which draws an accent bar in the gutter. Used
/// once, like the tooltip above.
inline bool g_NextRowUnsaved = false;
inline void SetNextRowUnsaved(bool unsaved)
{
    g_NextRowUnsaved = unsaved;
}

// Set by BeginPropertyRow for the row it just began. The label column is a
// handle: dragging it scrubs numeric values (numeric editors add
// RowLabelDragX to their field), and right-clicking opens the row menu (the
// editor takes the click with TakeRowMenuClick, where the property and its
// default are known).
inline bool g_RowLabelDragging = false;
inline float g_RowLabelDragX = 0.0f;
inline bool g_RowLabelMenuClick = false;
inline bool g_RowLabelHovered = false;
inline bool RowLabelHovered()
{
    return g_RowLabelHovered;
}
inline bool RowLabelDragging()
{
    return g_RowLabelDragging;
}
inline float RowLabelDragX()
{
    return g_RowLabelDragX;
}
inline bool TakeRowMenuClick()
{
    const bool clicked = g_RowLabelMenuClick;
    g_RowLabelMenuClick = false;
    return clicked;
}

// The Inspector's property filter, one for the whole object, since you
// rarely know which component owns the field you are looking for. Kept here
// so row drawing can use it without every caller passing it along.
inline std::string g_PropertyFilter;
// Lowercased once here, since the match runs per property per frame.
inline std::string g_PropertyFilterLower;
// Set while drawing a component whose name the filter matched. Searching
// "rigidbody" shows the whole rigidbody, not only fields called rigidbody.
inline bool g_ComponentNameMatched = false;
// How many things the filter let through this frame, so the panel can say a
// query matched nothing instead of just showing an empty body.
inline int g_FilterHits = 0;

inline std::string LowerAscii(const char* text)
{
    std::string out(text ? text : "");
    for (char& c : out)
    {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

inline void SetPropertyFilter(const char* filter)
{
    g_PropertyFilter = filter ? filter : "";
    g_PropertyFilterLower = LowerAscii(g_PropertyFilter.c_str());
    g_ComponentNameMatched = false;
    g_FilterHits = 0;
}
inline bool PropertyFilterActive()
{
    return !g_PropertyFilter.empty();
}
inline const std::string& PropertyFilter()
{
    return g_PropertyFilter;
}

/// Case-insensitive substring match; an empty filter matches everything.
inline bool TextMatchesFilter(const char* text)
{
    if (g_PropertyFilterLower.empty())
    {
        return true;
    }
    if (!text)
    {
        return false;
    }
    return LowerAscii(text).find(g_PropertyFilterLower) != std::string::npos;
}

/// Whether a property row passes the filter. Every row of a component the
/// filter names passes.
inline bool PropertyMatchesFilter(const char* displayName)
{
    if (g_PropertyFilter.empty())
    {
        return true;
    }
    if (g_ComponentNameMatched)
    {
        return true;
    }
    return TextMatchesFilter(displayName);
}

/// Set while drawing one component whose name the filter matched (see
/// g_ComponentNameMatched), and cleared after its rows.
inline void SetComponentNameMatched(bool matched)
{
    g_ComponentNameMatched = matched;
}

/// Called by whoever draws something the filter let through: a property row,
/// or a component header whose name matched.
inline void NoteFilterHit()
{
    ++g_FilterHits;
}
inline int FilterHits()
{
    return g_FilterHits;
}

// --- saved-vs-unsaved -------------------------------------------------
//
// The value every reflected property had when the scene was last read from
// or written to disk. The Inspector marks rows whose value differs, which
// shows what you changed and have not saved.
//
// Captured on open and on every save (see EditorApp::OpenScene /
// SaveScene). Keyed by object id and the component's index on it, never by
// pointer: a new component at a freed one's address would get its saved
// values.
void CaptureSavedState(::Deki::Scene* scene);
void ClearSavedState();
// Bumped by both of the above, so code that caches comparisons with the
// saved state knows when it changed.
uint64_t SavedStateEpoch();
const nlohmann::json* SavedValueFor(const Deki::Component* comp, const Deki::PropertyInfo& prop);

/// True if the filter names this component type: its label or its type
/// name, matched as the Add Component list does.
bool ComponentNameMatchesFilter(const Deki::ComponentMeta* meta);

/// True if the filter shows any property of this component type, or names
/// the type itself. A component with no match is skipped, header and all.
bool ComponentHasFilterMatch(const Deki::ComponentMeta* meta);

/// The left edge a section's content shares with its band's title text.
///
/// A band's left edge is its chevron, one step left of the content (but at
/// least a margin from the panel edge); its text starts one chevron width
/// further in. Content lines up with the text, so the band contains its rows.
/// The chevron width is added even for a band without a chevron, so every
/// band and its content share one column.
///
/// The width is Metrics::kSectionLabelIndent, as the theme's bands use, not
/// GetTreeNodeToLabelSpacing(): that one includes the caller's FramePadding,
/// so the column would differ between panels that push different padding.
///
/// Used by BeginPropertyRow and anything else that titles a run of rows (a
/// DEKI_GROUP sub-header). Window-relative x, ready for SetCursorPosX.
inline float SectionAlignedCursorX()
{
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float x = ImGui::GetCursorPosX();
    const float floorX = ::DekiEditor::Metrics::kSectionHeaderMargin * dpi;
    const float want = x - ::DekiEditor::Metrics::kSectionHeaderOutdent * dpi;
    const float bandLeft = want < floorX ? floorX : want;
    return bandLeft + ::DekiEditor::Metrics::kSectionLabelIndent * dpi;
}

/// Width of a row's label column: 42% of the row, kept between 5.5em and
/// 9em. A wide Inspector gives long names room, a narrow one still leaves the
/// value usable, and below 5.5em names are cut instead.
inline float PropertyLabelWidth(float labelStartX)
{
    const float font = ImGui::GetFontSize();
    const float rightPad = ::DekiEditor::Metrics::kInspectorRightPad * ImGui::GetStyle().FontScaleDpi;
    const float rowWidth = ImGui::GetContentRegionMax().x - labelStartX - rightPad;
    float w = rowWidth * 0.42f;
    const float minW = font * 5.5f;
    const float maxW = font * 9.0f;
    if (w < minW)
    {
        w = minW;
    }
    if (w > maxW)
    {
        w = maxW;
    }
    return w;
}

/// Starts a property row: a label column in Inter Medium 13 CSS px, colour
/// rgb(165,166,164), then a bordered value control filling the rest.
///
/// The label must pass an explicit size to PushFont: PushFont(font) alone
/// keeps the inherited size, not the size the font was loaded at.
/// Fonts::CssPx converts 13 CSS px to this font's FreeType size; DPI is
/// applied at draw time (FontSizeBase * FontScaleDpi).
inline void BeginPropertyRow(const char* displayName)
{
    const char* tooltip = g_NextRowTooltip;
    const bool unsaved = g_NextRowUnsaved;
    g_NextRowTooltip = nullptr;
    g_NextRowUnsaved = false;
    g_RowLabelDragging = false;
    g_RowLabelHovered = false;
    g_RowLabelDragX = 0.0f;

    // Rows start on the left edge of the title text in the band above, so a
    // header and its fields read as one group.
    const float labelStartX = SectionAlignedCursorX();
    ImGui::SetCursorPosX(labelStartX);

    const float labelWidth = PropertyLabelWidth(labelStartX);
    const float rowH = ImGui::GetFrameHeight();
    const ImVec2 rowPos = ImGui::GetCursorScreenPos();

    // Unsaved marker: a short accent bar in the chevron gutter, where
    // nothing else is drawn.
    if (unsaved)
    {
        const float dpi = ImGui::GetStyle().FontScaleDpi;
        const float barW = 2.0f * dpi;
        const float barX = rowPos.x - ::DekiEditor::Metrics::kSectionLabelIndent * dpi * 0.5f;
        const float inset = rowH * 0.22f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(barX, rowPos.y + inset),
                                                  ImVec2(barX + barW, rowPos.y + rowH - inset),
                                                  ImGui::ColorConvertFloat4ToU32(::DekiEditor::Palette::Accent));
    }

    // The label column is also a handle: drag it sideways to scrub a numeric
    // value (the editors read RowLabelDragX), right-click it for the row menu.
    // The invisible button goes first and the label is painted over it, so
    // the button gets the hover, drag and click. The id includes the display
    // name because not every caller wraps its rows in an ID scope.
    const float labelAvail = labelWidth - ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::PushID(displayName);
    ImGui::InvisibleButton("##rowlabel", ImVec2(labelAvail > 1.0f ? labelAvail : 1.0f, rowH),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool labelHovered = ImGui::IsItemHovered();
    g_RowLabelHovered = labelHovered;
    g_RowLabelDragging = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (g_RowLabelDragging)
    {
        g_RowLabelDragX = ImGui::GetIO().MouseDelta.x;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        g_RowLabelMenuClick = true;
    }
    ImGui::PopID();

    // The label is painted, not an ImGui item. The button above already
    // handles input, and an item here would move CursorPosPrevLine off the
    // row top, which SameLine below reads to put the value on this line.
    //
    // Centred on the label font's own size, not placed at FramePadding.y:
    // AlignTextToFramePadding centres text only in the ambient font. The body
    // font is a raw 13px while this label is CSS 13px (~15.7px for Inter), so
    // frame padding would put it ~1.4px below the checkbox, which is centred
    // in the same rowH.
    if (::DekiEditor::g_FontUIMed)
    {
        ImGui::PushFont(::DekiEditor::g_FontUIMed, ::DekiEditor::Fonts::CssPx(::DekiEditor::g_FontUIMed, 13.0f));
    }
    const float labelH = ImGui::GetFontSize();
    const ImVec2 labelPos(rowPos.x, rowPos.y + std::floor((rowH - labelH) * 0.5f));
    // GetColorU32, not ColorConvertFloat4ToU32: it folds in style.Alpha, so
    // the label keeps dimming with the rest of the row inside BeginDisabled.
    const ImU32 labelCol = ImGui::GetColorU32(ImVec4(165.0f / 255.0f, 166.0f / 255.0f, 164.0f / 255.0f, 1.0f));

    // A name too long for its column is cut with "...", and hovering shows
    // the full name (and the tooltip, if any).
    const bool truncated = ImGui::CalcTextSize(displayName).x > labelAvail;
    std::string clipped;
    if (truncated)
    {
        const float fitWidth = labelAvail - ImGui::CalcTextSize("...").x;
        const size_t len = std::strlen(displayName);
        size_t keep = 0;
        while (keep < len)
        {
            size_t next = keep + 1;
            while (next < len && (static_cast<unsigned char>(displayName[next]) & 0xC0) == 0x80)
            {
                ++next;  // never split a UTF-8 sequence
            }
            if (ImGui::CalcTextSize(displayName, displayName + next).x > fitWidth)
            {
                break;
            }
            keep = next;
        }
        clipped.assign(displayName, keep);
        clipped += "...";
    }
    // Font and size passed explicitly: the draw list's own font is not the
    // one just pushed.
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), labelPos, labelCol,
                                        truncated ? clipped.c_str() : displayName);
    if (::DekiEditor::g_FontUIMed)
    {
        ImGui::PopFont();
    }

    if (labelHovered && (truncated || tooltip))
    {
        if (truncated && tooltip)
        {
            DekiEditor::Tooltip("%s\n\n%s", displayName, tooltip);
        }
        else if (truncated)
        {
            DekiEditor::Tooltip("%s", displayName);
        }
        else
        {
            DekiEditor::Tooltip("%s", tooltip);
        }
    }

    ImGui::SameLine(labelStartX + labelWidth);
    // The last item on this line is the invisible button, which leaves the
    // text baseline offset at zero. Without this, a value that draws plain
    // text would sit at the top of the row instead of at its frame padding.
    ImGui::AlignTextToFramePadding();
    // Right gutter (DPI-scaled) so value controls do not touch the panel edge
    // or scrollbar. This sets the Inspector's right padding for every field;
    // controls that size themselves (asset and object ref buttons) read it
    // back with ImGui::CalcItemWidth(). Full-bleed section headers draw from
    // the window bounds and ignore it.
    ImGui::SetNextItemWidth(-(::DekiEditor::Metrics::kInspectorRightPad * ImGui::GetStyle().FontScaleDpi));
}

}  // namespace InspectorUtils

}  // namespace DekiEditor
