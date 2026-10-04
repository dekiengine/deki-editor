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
// Identifier -> label conversion now lives in deki-editor.dll so package
// tool windows label reflected properties exactly like the Inspector does.
using EditorNaming::NicifyName;
using EditorNaming::GetDisplayName;

// Evaluate visibility condition for a property
// Returns true if property should be visible based on its visibilityProperty metadata
bool EvaluateVisibility(Deki::Component* comp, const Deki::PropertyInfo& prop, const Deki::ComponentMeta* meta);

/// Is @p comp of type @p targetTypeName, or does it derive from it?
/// Walks the reflected baseTypeId chain. Used to resolve componentRef
/// property targets, where a reference to a base type must accept any
/// derived component on the object.
bool ComponentIsOrDerivesFrom(Deki::Component* comp, const char* targetTypeName);

// Comma-separated names of the components on `obj` that are (or derive from)
// `targetTypeName`, for ObjectRef display. Empty when there are none.
std::string GetMatchingComponentName(Deki::Object* obj, const char* targetTypeName);

// One-shot tooltip for the next BeginPropertyRow. DrawProperty publishes the
// reflected DEKI_TOOLTIP here so it reaches the row label no matter which
// path draws it (direct call, EditorUI checkbox/unit-field backend, asset
// editor). Consumed by the next BeginPropertyRow; the publisher clears it
// after the field in case the drawn widget never rendered a labeled row.
inline const char* g_NextRowTooltip = nullptr;
inline void SetNextRowTooltip(const char* tooltip)
{
    g_NextRowTooltip = tooltip;
}

// Marks the next row as holding an UNSAVED value (one that has moved since
// the scene was last written to disk), which earns it an accent bar in the
// gutter. Same one-shot contract as the tooltip above: published by the
// property editor, consumed by the row.
inline bool g_NextRowUnsaved = false;
inline void SetNextRowUnsaved(bool unsaved)
{
    g_NextRowUnsaved = unsaved;
}

// Filled by BeginPropertyRow, describing the row it has just begun. The
// label column is a handle: dragging it scrubs numeric values (the numeric
// editors add RowLabelDragX to their field), right-clicking it opens the
// row menu (the editor claims the click with TakeRowMenuClick, so the menu
// is built where the property and its default are known).
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

// Inspector-wide property filter. One field over the whole object rather
// than per component: you rarely know which component owns the field you
// are looking for, which is the reason to search in the first place. Held
// here so the row-drawing side can honour it without every call site
// threading it through.
inline std::string g_PropertyFilter;
// Folded once here rather than on every comparison: the match runs per
// property per component per frame, and it was lowercasing the query again
// each time.
inline std::string g_PropertyFilterLower;
// Set while drawing a component whose own NAME is what the filter matched.
// Searching "rigidbody" means "show me the rigidbody", not "show me fields
// called rigidbody" - of which there are none, so the honest reading of the
// query left the inspector blank.
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

// Case-insensitive substring match; no filter matches everything.
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

// Whether a property row survives the filter. Everything inside a component
// the query named survives whole: you asked for that component.
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

// Drawing a component the query named by name (see g_ComponentNameMatched).
// Set around one component's rows and cleared after it.
inline void SetComponentNameMatched(bool matched)
{
    g_ComponentNameMatched = matched;
}

// Counted by whoever draws something the filter allowed through - a property
// row, or a component header whose name was the match.
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
// The value every reflected property had the last time the scene was read
// from or written to disk. The inspector marks a row whose value has moved
// away from it, which answers the question you actually have in front of an
// object ("what have I changed and not saved?") rather than how it compares
// to a type's constructor.
//
// Captured on open and on every save (see EditorApp::OpenScene /
// SaveScene). Keyed by object id + the component's index on that object,
// never by pointer: a component freed and a new one allocated at the same
// address would otherwise inherit its predecessor's saved values.
void CaptureSavedState(::Deki::Scene* scene);
void ClearSavedState();
// Bumped by both of the above: consumers that cache comparisons against the
// saved state use it to know when the baseline moved.
uint64_t SavedStateEpoch();
const nlohmann::json* SavedValueFor(const Deki::Component* comp, const Deki::PropertyInfo& prop);

// Does the filter name this component type itself - its label or its type
// name, the way the Add Component list matches them?
bool ComponentNameMatchesFilter(const Deki::ComponentMeta* meta);

// Does this component type have any property the filter would show (or is it
// the component the filter names)? Lets a component with no match be skipped
// whole, header and all, instead of leaving a row of empty sections to
// scroll past.
bool ComponentHasFilterMatch(const Deki::ComponentMeta* meta);

// The left edge a section's CONTENT shares with its band's TITLE TEXT.
//
// A band's left edge is its chevron, one step outside the content-left
// (floored at a margin from the panel edge); its text starts one chevron
// zone further in. Content lines up with the text, not the chevron, so the
// band reads as containing its rows - align to the chevron instead and the
// rows stick out to the left of the very title that names them. The chevron
// zone is added even for a band that has no chevron, so both kinds of band
// and their content sit on one column.
//
// The zone is Metrics::kSectionLabelIndent, the same token the theme's bands
// use, and deliberately NOT GetTreeNodeToLabelSpacing(): that one carries
// the caller's FramePadding, so the column moved between panels that push
// different padding (the Inspector's and the node graph's disagreed by 12px)
// even though every one of them ran this same function.
//
// Used by BeginPropertyRow and by anything else that titles a run of rows
// (a DEKI_GROUP sub-header). Window-relative x, ready for SetCursorPosX.
inline float SectionAlignedCursorX()
{
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float x = ImGui::GetCursorPosX();
    const float floorX = ::DekiEditor::Metrics::kSectionHeaderMargin * dpi;
    const float want = x - ::DekiEditor::Metrics::kSectionHeaderOutdent * dpi;
    const float bandLeft = want < floorX ? floorX : want;
    return bandLeft + ::DekiEditor::Metrics::kSectionLabelIndent * dpi;
}

// Width of a row's label column. Proportional rather than fixed, so a wide
// inspector gives long names room instead of clipping them at a constant
// 9em, and a narrow one still leaves the value control usable. Clamped both
// ways: the mockup's ~9em is the ceiling, and under ~5.5em a name stops
// being readable at all, at which point truncation is the honest answer.
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

// Schematic property row: proportional left label column in Inter Medium
// 13 CSS px, color rgb(165,166,164), bordered value control filling the rest
// of the row.
//
// The label MUST pass an explicit size to PushFont: PushFont(font) alone
// keeps the inherited size, ignoring the size the font was loaded at.
// Fonts::CssPx converts 13 CSS px to this font's FreeType size; DPI is then
// applied automatically (FontSizeBase * FontScaleDpi at draw time).
inline void BeginPropertyRow(const char* displayName)
{
    const char* tooltip = g_NextRowTooltip;
    const bool unsaved = g_NextRowUnsaved;
    g_NextRowTooltip = nullptr;
    g_NextRowUnsaved = false;
    g_RowLabelDragging = false;
    g_RowLabelHovered = false;
    g_RowLabelDragX = 0.0f;

    // Rows start on the same left edge as the title text of the band above
    // them, so a header and its fields read as one group. Applied here
    // because every field in the editor flows through this helper.
    const float labelStartX = SectionAlignedCursorX();
    ImGui::SetCursorPosX(labelStartX);

    const float labelWidth = PropertyLabelWidth(labelStartX);
    const float rowH = ImGui::GetFrameHeight();
    const ImVec2 rowPos = ImGui::GetCursorScreenPos();

    // Unsaved marker: a short accent bar in the chevron gutter, where
    // nothing else is drawn. Reading down that gutter answers "what have I
    // changed since the last save?" without opening or diffing anything.
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

    // The label column doubles as a handle: drag it sideways to scrub a
    // numeric value (the editors read RowLabelDragX), right-click it for the
    // row menu. The button goes down FIRST and the label is painted over it,
    // so the invisible button underneath keeps the hover, the drag and the
    // click while the label stays plain text. The id folds in the display
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

    // The label is PAINTED, not submitted as an ImGui item. Two reasons:
    // it is not interactive anyway (the invisible button above owns the
    // hover, the drag and the right-click), and an item here would move
    // CursorPosPrevLine off the row top - SameLine below reads exactly that
    // to put the value widget back on this line, so a shifted label would
    // drag the value widget down with it.
    //
    // Centered on the label font's OWN size rather than placed at
    // FramePadding.y: AlignTextToFramePadding centers text only when the
    // text font matches the ambient one, and this one does not. The body
    // font is loaded at a raw 13px while this label is CSS 13px (~15.7px
    // for Inter), so frame padding left it sitting ~1.4px below the
    // checkbox box, which is centered in this same rowH.
    if (::DekiEditor::g_FontUIMed)
    {
        ImGui::PushFont(::DekiEditor::g_FontUIMed, ::DekiEditor::Fonts::CssPx(::DekiEditor::g_FontUIMed, 13.0f));
    }
    const float labelH = ImGui::GetFontSize();
    const ImVec2 labelPos(rowPos.x, rowPos.y + std::floor((rowH - labelH) * 0.5f));
    // GetColorU32, not ColorConvertFloat4ToU32: it folds in style.Alpha, so
    // the label keeps dimming with the rest of the row inside BeginDisabled.
    const ImU32 labelCol = ImGui::GetColorU32(ImVec4(165.0f / 255.0f, 166.0f / 255.0f, 164.0f / 255.0f, 1.0f));

    // Keep the label inside its column: a name too long for the label font
    // is cut with "..." instead of running under the value widget, and
    // hovering it reveals the full name (plus the tooltip when one exists).
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
    // one we just pushed to measure with.
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
    // Restore the text baseline offset the value column used to inherit
    // from the label's text item. Now that the label is painted, the last
    // item on this line is the invisible button, which leaves the offset at
    // zero - without this, a value control that draws plain text would ride
    // the top of the row instead of its frame padding.
    ImGui::AlignTextToFramePadding();
    // Right gutter (DPI-scaled) so framed value controls don't sit flush
    // against the panel edge / scrollbar. This is the single source of truth for
    // the inspector's right padding: every field flows through this row helper,
    // and the few controls that size themselves explicitly (asset/object ref
    // buttons) read this same width back via ImGui::CalcItemWidth(). Full-bleed
    // section headers draw from the window bounds, so they are unaffected.
    ImGui::SetNextItemWidth(-(::DekiEditor::Metrics::kInspectorRightPad * ImGui::GetStyle().FontScaleDpi));
}

}  // namespace InspectorUtils

}  // namespace DekiEditor
