#pragma once

// The editor app, deki-editor.dll and every package tool window draw with
// this one theme (palette, fonts, schematic widgets), so the implementations
// live in deki-editor.dll and are imported everywhere else.
#include <deki-editor/EditorExport.h>
#include "imgui.h"
#include <initializer_list>

namespace DekiEditor
{

/// Applies the Deki schematic theme to ImGui. Call after
/// ImGui::CreateContext() and before the first frame.
DEKI_EDITOR_API void SetupEditorTheme(float dpiScale);

// Schematic / datasheet palette, used by panels for chrome (banners,
// breadcrumbs, level chips) so accent colors stay consistent.
namespace Palette
{
// Named values, used like enum values: PascalCase, as the code style allows.
// NOLINTBEGIN(readability-identifier-naming)
// Backgrounds (near black with subtle stops)
constexpr ImVec4 Bg = ImVec4(0.039f, 0.043f, 0.051f, 1.0f);     // #0a0b0d main panel bg
constexpr ImVec4 Bg2 = ImVec4(0.063f, 0.067f, 0.075f, 1.0f);    // #101113 hover/alt row
constexpr ImVec4 Bg3 = ImVec4(0.051f, 0.055f, 0.063f, 1.0f);    // #0d0e10 input field bg
constexpr ImVec4 Panel = ImVec4(0.047f, 0.051f, 0.063f, 1.0f);  // #0c0d10 panel header bg

// Hairlines / borders
constexpr ImVec4 Line = ImVec4(0.114f, 0.122f, 0.137f, 1.0f);   // #1d1f23 faintest
constexpr ImVec4 Line2 = ImVec4(0.149f, 0.157f, 0.176f, 1.0f);  // #26282d standard border
constexpr ImVec4 Line3 = ImVec4(0.200f, 0.212f, 0.235f, 1.0f);  // #33363c emphasized

// Text tiers
constexpr ImVec4 Fg = ImVec4(0.910f, 0.902f, 0.882f, 1.0f);    // #e8e6e1 primary
constexpr ImVec4 Dim = ImVec4(0.478f, 0.478f, 0.463f, 1.0f);   // #7a7a76 secondary
constexpr ImVec4 Dim2 = ImVec4(0.322f, 0.322f, 0.310f, 1.0f);  // #52524f tertiary

// Schematic accent: oklch(0.78 0.16 240) in sRGB, a cyan-blue #3ac2ff.
constexpr ImVec4 Accent = ImVec4(0.230f, 0.764f, 1.000f, 1.0f);
constexpr ImVec4 AccentDim = ImVec4(0.230f, 0.764f, 1.000f, 0.40f);

// Folder icon tint: oklch(0.62 0.13 240) in sRGB (#248fcc), a deeper, less
// saturated cousin of Accent on the same 240 hue.
constexpr ImVec4 FolderIcon = ImVec4(0.141f, 0.561f, 0.800f, 1.0f);

// The agent pointer (AgentCursor): what an AI agent is doing over MCP. Violet,
// so it is never mistaken for the user's own selection (Accent) or a status.
constexpr ImVec4 Agent = ImVec4(0.706f, 0.549f, 1.000f, 1.0f);  // #b48cff

// Status signals
constexpr ImVec4 Red = ImVec4(0.910f, 0.420f, 0.310f, 1.0f);    // errors / destructive
constexpr ImVec4 Amber = ImVec4(0.870f, 0.690f, 0.310f, 1.0f);  // warnings
constexpr ImVec4 Green = ImVec4(0.451f, 0.831f, 0.451f, 1.0f);  // success / in-use
// NOLINTEND(readability-identifier-naming)
}  // namespace Palette

// Inspector spacing, the sibling of Palette. Values are unscaled CSS-style
// px; multiply by the DPI scale where used (e.g. EditorApp::Dpi(x) or
// ImGui::GetStyle().FontScaleDpi). The one source for the inspector's margins
// and row metrics.
namespace Metrics
{
constexpr float kInspectorIndent = 18.0f;  // left body indent (and texture scroll-region indent)
// A section band's label and the property rows under it share one left
// edge: both sit this far OUTSIDE the content-left, but at least a margin
// from the panel edge (so a band drawn at no indent still has air).
constexpr float kSectionHeaderOutdent = 10.0f;
constexpr float kSectionHeaderMargin = 8.0f;
// The step from a band's left edge (its chevron column) to the column its
// TITLE TEXT and its CONTENT share. Must stay a fixed number, not
// ImGui::GetTreeNodeToLabelSpacing(): that is fontSize + FramePadding.x * 2,
// and panels push different frame padding, so headers in the node graph and
// the Inspector would not line up. Wide enough to clear the chevron glyph
// (one font size) with air on both sides.
constexpr float kSectionLabelIndent = 22.0f;
constexpr float kInspectorRightPad = 14.0f;  // right gutter so value controls don't touch the edge
constexpr float kRowSpacing = 12.0f;         // vertical gap between property rows
constexpr float kFieldPadding = 8.0f;        // FramePadding.y inside value frames (top+bottom) -> taller fields
constexpr float kPreviewMaxHeight = 200.0f;  // cap for the anchored texture preview (bounds both up- and downscale)
}  // namespace Metrics

// Schematic type scale, loaded in main.cpp: three families at the pixel
// sizes the mockup uses, merged with Tabler Icons on the UI sizes.
//
// UI - Inter
extern DEKI_EDITOR_API ImFont* g_FontUI;       // Regular 13px, DEFAULT body (rows, breadcrumbs)
extern DEKI_EDITOR_API ImFont* g_FontUISmall;  // Regular 11.5px, status bar / filter chips / sub-labels
extern DEKI_EDITOR_API ImFont* g_FontUIMed;    // Medium 12.5px, inspector property labels
extern DEKI_EDITOR_API ImFont* g_FontUIBold;   // SemiBold 11px, tab labels + section headers (rendered uppercased)
//
// Mono - JetBrains Mono
extern DEKI_EDITOR_API ImFont* g_FontMono;       // Regular 12px, inspector value cells / code
extern DEKI_EDITOR_API ImFont* g_FontMonoSmall;  // Regular 11.5px, log timestamps / hash / stats
extern DEKI_EDITOR_API ImFont* g_FontMonoBold;   // Medium 12px, menubar DEKI/EDITOR wordmark
//
// Display - Space Grotesk Medium
extern DEKI_EDITOR_API ImFont* g_FontDisplay;       // 20px, inspector selection banner ("Hero / Sprite")
extern DEKI_EDITOR_API ImFont* g_FontDisplaySmall;  // 12px, section-header alt usage
extern DEKI_EDITOR_API ImFont* g_FontLogo;          // 13px, the "D" letter in the menubar logo box

namespace Fonts
{
/// Records a loaded font's TTF path so CssPx can work out its
/// CSS-to-FreeType size ratio from the font's own metrics. Call ONCE per font,
/// right after ImFontAtlas::AddFontFromFileTTF returns.
DEKI_EDITOR_API void RegisterTtf(ImFont* font, const char* ttfPath);

/// The ImGui+FreeType size whose em-square matches a CSS font-size.
///
/// ImGui's FreeType wrapper uses FT_SIZE_REQUEST_TYPE_REAL_DIM, which takes
/// `pixel_size` as the full glyph extent (ascender + |descender|), while CSS
/// takes `font-size` as the em-square. The ratio differs per font (Inter
/// ≈ 1.21, JetBrains Mono ≈ 1.32, Space Grotesk ≈ 1.26). For "13px CSS" of
/// Inter, call CssPx(g_FontUI, 13.0f).
///
/// A fixed-width font moves to the nearest size whose character advance is a
/// whole pixel (12.5 CSS px JetBrains Mono becomes 13.33, 8px per character),
/// since FreeType rounds the advance and a fractional one spreads the letters
/// apart.
///
/// Returns cssPx unchanged if the font was not registered.
DEKI_EDITOR_API float CssPx(ImFont* font, float cssPx);
}  // namespace Fonts

/// Every tooltip in the editor goes through here. None appears until the
/// pointer has rested for kTooltipDelaySeconds, so sweeping across a toolbar
/// does not flicker boxes.
///
/// Same signature as ImGui::SetTooltip. The wait is measured on the hovered
/// item, or, over hand-drawn regions that are not ImGui items, on the pointer
/// standing still; callers need not say which.
constexpr float kTooltipDelaySeconds = 1.0f;
DEKI_EDITOR_API void Tooltip(const char* fmt, ...) IM_FMTARGS(1);

/// Where a window first appears: `width` x `height`, centred over the editor.
/// Call just before its Begin. Applies only the first time (FirstUseEver), so
/// a window the user moved, resized or docked stays there. Otherwise ImGui
/// opens a floating window at (60, 60), over the toolbar.
DEKI_EDITOR_API void FirstUseWindowPlacement(float width, float height);

/// A panel's "nothing here" line (empty list, search with no result): dim,
/// wrapped, inset like the rows above it rather than against the panel edge.
DEKI_EDITOR_API void EmptyStateText(const char* text);

/// One entry in a SchematicPicker list.
struct PickerItem
{
    const char* label = nullptr;     // the row's text, and what search ranks
    const char* altName = nullptr;   // a second searchable string (a type name)
    const char* category = nullptr;  // the group it browses under; null = ungrouped
    // Shown UNDER the label, on its own line, so the user can tell similarly
    // named entries apart. Falls back to `altName` when empty.
    const char* description = nullptr;
    const char* tooltip = nullptr;      // shown on hover; the description otherwise
    bool disabled = false;              // listed, but not choosable
    const char* disabledTip = nullptr;  // and why
};

/// The editor's search-first picker: two-level browsing (categories, then
/// what is inside one), ranked search across every level at once, full
/// keyboard control, and a slide when you step between levels.
///
/// Draws the BODY of a popup the caller has already begun and placed, so the
/// same list serves Add Component in the Inspector and Add Action in a node
/// graph stack. Returns the index of the item chosen this frame, or -1.
/// `justOpened` resets the query and level and takes keyboard focus.
DEKI_EDITOR_API int SchematicPicker(const char* id, const char* title, const PickerItem* items, int itemCount,
                                    bool justOpened, float listHeight, const char* itemIcon = nullptr,
                                    const char* searchHint = nullptr, const char* emptyText = nullptr);

/// Opens the shared property-list layout: the body indent, row gap and field
/// padding of every property list in the editor.
///
/// Any panel that shows property rows uses this instead of its own numbers,
/// so a widget has the same height and spacing in the Inspector and in
/// package tool windows. Pair with EndPropertyContext.
DEKI_EDITOR_API void BeginPropertyContext();
DEKI_EDITOR_API void EndPropertyContext();

/// Schematic collapsing header for inspector sections, with the label in
/// Space Grotesk (Title Case as typed). Returns true if expanded. Keeps
/// ImGui's CollapsingHeader behavior, so BeginPopupContextItem /
/// OpenPopupOnItemClick still attach to it.
/// With ImGuiTreeNodeFlags_Leaf (and DefaultOpen) it is the same band with no
/// chevron and no folding: a column title in a tool window.
DEKI_EDITOR_API bool SchematicCollapsingHeader(const char* label,
                                               ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen);

/// The same band as SchematicCollapsingHeader, with the body SLIDING open and
/// shut instead of appearing and vanishing.
///
///     if (SchematicSectionBegin("Transform")) { ...rows...; SchematicSectionEnd(); }
///
/// Returns true whenever the body must be submitted, including the frames
/// where the section is closing, since a body that is not submitted cannot be
/// animated away. During those frames the body is clipped to the animated
/// height (so the hidden part cannot be hit) and the layout below is pulled up
/// to match. Call SchematicSectionEnd() ONLY when this returned true (as with
/// SchematicBeginCombo / SchematicEndCombo).
///
/// A section opened for the very first time has no measured height yet, so it
/// opens instantly; every toggle after that animates.
/// `outHeaderId` receives the band's ImGui id, even when this returns false.
/// A caller that forces a section open (the property filter does, so a match
/// is never hidden under a collapsed header) needs it to read and later
/// restore the state ImGui keeps for that id.
DEKI_EDITOR_API bool SchematicSectionBegin(const char* label, ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen,
                                           ImGuiID* outHeaderId = nullptr);
DEKI_EDITOR_API void SchematicSectionEnd();

/// The children of a tree node, sliding open and shut like a section body.
/// Wraps whatever the caller draws between the node and its TreePop:
///
///     const bool open   = ImGui::TreeNodeEx(...);
///     const ImGuiID nid = ImGui::GetItemID();      // capture straight after
///     if (SchematicTreeBodyBegin(open, nid)) { ...children... }
///     SchematicTreeBodyEnd();                      // ALWAYS, whatever Begin said
///     if (open) ImGui::TreePop();                  // ImGui's own push, unchanged
///
/// Returns true whenever the children must be submitted, including the frames
/// where the node is closing. While it closes, ImGui has already dropped the
/// indent and id scope it pushes for an open node, so this pushes them back
/// (and pops them in End); otherwise the children would lay out at the wrong
/// depth and change identity during the animation.
///
/// Unlike the section pair, End is called every time: `open` and "submit the
/// children" are different questions here, and pairing on the wrong one is an
/// easy mistake.
DEKI_EDITOR_API bool SchematicTreeBodyBegin(bool open, ImGuiID nodeId);
DEKI_EDITOR_API void SchematicTreeBodyEnd();

/// The same band without the fold: same height, full bleed and uppercase
/// letter-spaced label as SchematicCollapsingHeader, with no chevron and no
/// collapsing. For a header that TITLES a panel rather than grouping
/// something to fold away. The whole row is clickable (returns true when
/// clicked, e.g. to start an inline rename) and lights up on hover like the
/// header; pass an id-only "###id" label to draw the band empty and place
/// your own control on it.
/// `outLabelX` receives the screen x of the label, for callers that place
/// their own control on the band and want it to start where the text did.
DEKI_EDITOR_API bool SchematicSectionBand(const char* label, float* outLabelX = nullptr);

/// An inline rename field drawn AS a band title: no frame, background or
/// padding, in the band's own font, size and color, centered on the band's
/// line. Pass the band's label x (SchematicSectionBand's `outLabelX`) and its
/// item rect, so the text lands exactly where the title was and the header
/// looks the same when a rename starts. Returns true when the text changed
/// this frame; the usual IsItem* queries work after it.
DEKI_EDITOR_API bool SchematicBandTitleField(const char* id, char* buf, int bufSize, float x, float bandTop,
                                             float bandBottom, float width, const char* hint);

/// One rung below both of those: a DEKI_GROUP-style subdivision INSIDE a
/// section. Same uppercase letter-spaced style, smaller and dimmer, with a
/// rule from the end of the label to the content edge. Draws at the current
/// cursor and reserves its own row.
DEKI_EDITOR_API void SchematicSubHeader(const char* label);

/// The "add one more" button at the end of a list (Add Component, Add
/// Transition): a centered outlined box (transparent fill, hairline border,
/// dim label) that takes the accent only on hover. Quieter than a filled
/// button on purpose: at the bottom of a list it is an invitation, not
/// another control to fill in. `width` 0 uses a comfortable default, clamped
/// to the space available. Returns true when clicked.
DEKI_EDITOR_API bool SchematicAddButton(const char* label, float width = 0.0f);

/// Draws a tree node's expand chevron (Tabler chevron, down=open /
/// right=closed) and its label, in place of ImGui's filled triangle. Call
/// IMMEDIATELY after an empty-label TreeNodeEx drawn with a transparent
/// ImGuiCol_Text (so the triangle is hidden), while any pushed FramePadding is
/// still active. `expandable`: the node can open (leaves draw no chevron).
/// `labelColor`: the label's color; capture GetColorU32(ImGuiCol_Text) BEFORE
/// pushing the transparent color.
DEKI_EDITOR_API void DrawTreeNodeChevronLabel(bool open, bool expandable, const char* label, ImU32 labelColor);

/// Like ImGui::Separator(), but the hairline spans the full panel width, past
/// the indent on the left and past the scrollbar on the right, so it touches
/// both dock seams. Use it to bound a component section like a window edge.
DEKI_EDITOR_API void FullBleedSeparator();

/// The toolbar under a panel's tab: a full-width strip with a PIN
/// (lock-to-selection) toggle on the left, a "..." menu on the right and a
/// hairline below. Call once right after ImGui::Begin(...), before the
/// panel's content. `pinned` is the panel's own flag that PIN toggles; only
/// the Inspector pins, so a null flag skips the toolbar. Returns the pin
/// state (true = the panel freezes its content instead of following the
/// selection). The bar currently scrolls with the content.
DEKI_EDITOR_API bool PanelToolbar(bool* pinned = nullptr);

/// Search field in the style of the Hierarchy and Asset Browser: a dim
/// magnifier, then a borderless transparent input, vertically centered in a
/// full-width band of its own. Call once where the search row goes; content
/// below flows under it. `buf`/`bufSize` hold the text.
///
/// `rightReserve` (px) keeps that much room free at the right of the row for
/// a neighbouring control (e.g. a filter dropdown), flush with the window's
/// right edge. The band keeps the input's short height; the reserved slot's
/// rect (full band height, flush right) comes back in outSlotX / outSlotY /
/// outSlotW / outSlotH so the caller can put a toolbar-style control there.
/// `bottomBorder` draws a full-width hairline under the row.
///
/// `hint` is the placeholder. Say what THIS field searches: a bare "Search"
/// in a panel that already has a title wastes the line, and the same field
/// means different things in the inspector and in a picker popup.
///
/// While there is text, a dim x shows at the right of the input; clicking it
/// or pressing Escape in the field empties the buffer, so the user can always
/// see how to clear the filter.
/// The hint is English and shown in the editor's language.
DEKI_EDITOR_API void SearchField(const char* id, char* buf, size_t bufSize, float rightReserve = 0.0f,
                                 bool bottomBorder = false, float* outSlotX = nullptr, float* outSlotY = nullptr,
                                 float* outSlotW = nullptr, float* outSlotH = nullptr, const char* hint = "Search");

/// Flat dropdown matching the panel toolbar's cells: a transparent, square,
/// borderless box with a faint white hover, `height` px tall, the value and
/// chevron centered. `items` is a "A\0B\0" zero-separated list. `leftDivider`
/// draws a 1px line on the box's left edge (the toolbar cell divider) to set
/// it off from a neighbour. Returns true the frame the selection changes
/// (and writes *currentIndex).
DEKI_EDITOR_API bool SchematicToolbarCombo(const char* id, int* currentIndex, const char* items, float height,
                                           bool leftDivider = false);

/// The same toolbar dropdown with rows of your own (sections, separators,
/// items that do more than pick an index): Begin draws the box and returns
/// true while the list is open; draw ImGui::Selectable rows, then call
/// SchematicEndToolbarCombo(), only when Begin returned true. Size the box
/// with SetNextItemWidth; SchematicToolbarComboWidth(preview) fits it to its
/// text.
DEKI_EDITOR_API bool SchematicBeginToolbarCombo(const char* id, const char* preview, float height,
                                                bool leftDivider = false);
DEKI_EDITOR_API void SchematicEndToolbarCombo();
DEKI_EDITOR_API float SchematicToolbarComboWidth(const char* preview);

/// Right-aligned `EDIT ->` style action link, in ALL CAPS mono accent text
/// with a 1px underline. Call after a section header, on the same row.
/// Returns true if clicked.
DEKI_EDITOR_API bool SchematicActionLink(const char* label);

/// Drag widgets with LEFT-aligned value text (ImGui centers it). Same
/// signature as ImGui::DragFloat / DragInt. They call the ImGui widget with an
/// empty format, so it draws no value, and draw the formatted value
/// left-aligned on the window draw list. Edit mode (Ctrl+click to type) is
/// ImGui's normal text input.
DEKI_EDITOR_API bool SchematicDragFloat(const char* id, float* v, float vSpeed = 1.0f, float vMin = 0.0f,
                                        float vMax = 0.0f, const char* format = "%.3f");
DEKI_EDITOR_API bool SchematicDragInt(const char* id, int* v, float vSpeed = 1.0f, int vMin = 0, int vMax = 0,
                                      const char* format = "%d");

/// Drag widgets with several components sharing the rest of the row. They
/// sit in a BeginGroup/EndGroup, so callers can use ImGui::IsItemActivated /
/// IsItemDeactivatedAfterEdit on the group as a whole (for command history).
/// Returns true if any component changed.
DEKI_EDITOR_API bool SchematicDragFloat2(const char* id, float v[2], float vSpeed = 1.0f, float vMin = 0.0f,
                                         float vMax = 0.0f, const char* format = "%.3f");
DEKI_EDITOR_API bool SchematicDragFloat3(const char* id, float v[3], float vSpeed = 1.0f, float vMin = 0.0f,
                                         float vMax = 0.0f, const char* format = "%.3f");
/// One box of those, with its axis stamp (0 = X, 1 = Y, 2 = Z), as an item of
/// its own: for a caller that needs each box's activation, or a different
/// format per box (a dash for values that differ across a selection).
DEKI_EDITOR_API bool SchematicDragFloatAxis(const char* id, float* v, int axis, float vSpeed = 1.0f, float vMin = 0.0f,
                                            float vMax = 0.0f, const char* format = "%.3f");

/// Schematic color editors, with the semantics of ImGui::ColorEdit4 /
/// ColorEdit3 (rgba / rgb as 0-1 floats). One cell per channel (a vertical
/// color stripe and a 0-255 value), plus a combined swatch on the right that
/// opens the full picker when clicked.
DEKI_EDITOR_API bool SchematicColorEdit4(const char* id, float col[4]);
DEKI_EDITOR_API bool SchematicColorEdit3(const char* id, float col[3]);

/// Dropdown matching the drag fields: near-black frame, the current item
/// left-aligned in the mono value font, a Tabler chevron on the right. Same
/// semantics as ImGui::Combo. The second overload takes one
/// "Item1\0Item2\0\0" zero-separated string.
DEKI_EDITOR_API bool SchematicCombo(const char* id, int* currentIndex, const char* const items[], int count);
DEKI_EDITOR_API bool SchematicCombo(const char* id, int* currentIndex, const char* itemsSeparatedByZeros);

/// Open/close for custom dropdowns whose items are not a fixed string array
/// (dynamic lists, custom preview text), with SchematicCombo's look. The
/// caller draws its own ImGui::Selectable items between Begin and End.
/// Returns true if open; call SchematicEndCombo() only then. `preview` is the
/// closed box's text (may be empty).
DEKI_EDITOR_API bool SchematicBeginCombo(const char* id, const char* preview);
DEKI_EDITOR_API void SchematicEndCombo();

/// Draws a 1px accent line along the top of the selected tab in every dock
/// node with a tab bar. Call once per frame after all panels have rendered
/// (right before ImGui::Render).
DEKI_EDITOR_API void DrawAllTabTopAccents();

/// Draws the 1px frame of standalone floating windows (dialogs, undocked tool
/// windows). WindowBorderSize is 0 so docked panels do not double the dock
/// separator, which leaves those windows without a border. Docked panels need
/// nothing here; ImGui's 1px dock separator is their seam. Call once per
/// frame after all panels have rendered (right before ImGui::Render). Pairs
/// with the border settings in SetupEditorTheme.
DEKI_EDITOR_API void DrawDockSeams();

/// Schematic checkbox: a 1px square border with a smaller filled square
/// inside when checked. Use instead of ImGui::Checkbox where the schematic
/// look matters. `fillColor` is the inner square's color (default Accent).
DEKI_EDITOR_API bool SchematicCheckbox(const char* label, bool* v);
DEKI_EDITOR_API bool SchematicCheckbox(const char* label, bool* v, const ImVec4& fillColor);

/// Styling for right-click context menus. Every context menu in the editor
/// (hierarchy, asset browser, inspector, console, ...) uses these so popups
/// look like the menu bar's File/Edit dropdowns: real WindowPadding, and once
/// open, wider ItemSpacing for taller rows and full-width hover.
///
/// ImGui forces the split: WindowPadding must be pushed BEFORE the popup
/// begins (the window takes it at Begin()), while ItemSpacing is pushed AFTER
/// it opens (it reads back the popup's WindowPadding.x and applies only to
/// the open scope). Sizes are GetFontSize() multiples, so they follow DPI.
/// Each Push has one matching ImGui::PopStyleVar() at the call site:
///
///   PushContextMenuPopupStyle();                 // before Begin*
///   if (ImGui::BeginPopupContextItem()) {
///       PushContextMenuItemStyle();              // after Begin*
///       ImGui::MenuItem(...);
///       ImGui::PopStyleVar();                    // ItemSpacing
///       ImGui::EndPopup();
///   }
///   ImGui::PopStyleVar();                        // WindowPadding
DEKI_EDITOR_API void PushContextMenuPopupStyle();
DEKI_EDITOR_API void PushContextMenuItemStyle();

/// Schematic modal dialog: centered over the main viewport, a fixed CSS-px
/// width (auto height), generous padding, no saved settings. Every modal in
/// the editor uses it so they all look like the Package Manager's Add
/// Package dialog. Begin/End popup rules: call EndSchematicModal() (which
/// wraps ImGui::EndPopup()) only when this returned true.
///
///   OpenPopup("Create New Folder");               // elsewhere, as usual
///   if (BeginSchematicModal("Create New Folder", 440.0f)) {
///       ...content...
///       EndSchematicModal();
///   }
DEKI_EDITOR_API bool BeginSchematicModal(const char* title, float cssWidth);
DEKI_EDITOR_API void EndSchematicModal();

/// The button row at the foot of a dialog: equal-width buttons across the
/// dialog, text centred, the safe choice first and the action last. Primary
/// is the accent button that does what the dialog is for; Danger is theme
/// red, for deleting or discarding. Returns the index clicked, or -1.
///
///   switch (SchematicDialogButtons({ { "Cancel" }, { "Delete", DialogButton::Danger } }))
enum class DialogButton
{
    Normal,
    Primary,
    Danger,
};
struct DialogButtonSpec
{
    const char* label;
    DialogButton kind = DialogButton::Normal;
    bool enabled = true;
};
DEKI_EDITOR_API int SchematicDialogButtons(std::initializer_list<DialogButtonSpec> buttons);

}  // namespace DekiEditor
