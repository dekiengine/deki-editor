#pragma once

// Shared across DLLs: the editor app, deki-editor.dll and every package tool
// window draw with this one theme (palette, fonts, schematic widgets), so the
// implementations live in deki-editor.dll and are imported everywhere else.
#include <deki-editor/EditorExport.h>
#include "imgui.h"
#include <initializer_list>

namespace DekiEditor
{

// Apply Deki Editor Schematic theme to ImGui.
// Must be called after ImGui::CreateContext() and before first frame.
DEKI_EDITOR_API void SetupEditorTheme(float dpiScale);

// Schematic / datasheet palette. Used by panels for chrome rendering
// (banners, breadcrumbs, level chips) so accent colors stay consistent.
namespace Palette
{
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

// Schematic accent - oklch(0.78 0.16 240) converted to sRGB.
// Reads as cyan-blue at #3ac2ff. Matches the mockup exactly.
constexpr ImVec4 Accent = ImVec4(0.230f, 0.764f, 1.000f, 1.0f);
constexpr ImVec4 AccentDim = ImVec4(0.230f, 0.764f, 1.000f, 0.40f);

// Folder icon tint - oklch(0.62 0.13 240) converted to sRGB (#248fcc).
// A deeper, less saturated cousin of Accent on the same 240 hue.
constexpr ImVec4 FolderIcon = ImVec4(0.141f, 0.561f, 0.800f, 1.0f);

// Status signals
constexpr ImVec4 Red = ImVec4(0.910f, 0.420f, 0.310f, 1.0f);    // errors / destructive
constexpr ImVec4 Amber = ImVec4(0.870f, 0.690f, 0.310f, 1.0f);  // warnings
constexpr ImVec4 Green = ImVec4(0.451f, 0.831f, 0.451f, 1.0f);  // success / in-use
}  // namespace Palette

// Inspector layout tokens (the sibling of Palette for spacing). Values are in
// unscaled CSS-style px; multiply by the DPI scale at each call site (e.g.
// EditorApp::Dpi(x) or ImGui::GetStyle().FontScaleDpi). Keeping them here makes
// the inspector's left/right margins and row metrics one source of truth instead
// of literals scattered across InspectorPanel / InspectorUtils / TextureInspector.
namespace Metrics
{
constexpr float InspectorIndent = 18.0f;  // left body indent (and texture scroll-region indent)
// A section band's label and the property rows under it share one left
// edge: both sit this far OUTSIDE the content-left, floored at a margin
// from the panel edge (so a band drawn at no indent still has air).
constexpr float SectionHeaderOutdent = 10.0f;
constexpr float SectionHeaderMargin = 8.0f;
// The step from a band's left edge (its chevron column) to the column its
// TITLE TEXT and its CONTENT share. One fixed number on purpose: this used
// to be ImGui::GetTreeNodeToLabelSpacing(), which is fontSize +
// FramePadding.x * 2 and therefore different in every panel that pushes its
// own frame padding - the Inspector's 8 and the theme's 14 put the "same"
// aligned column 12 px apart, so a header in the node graph and a header in
// the Inspector did not line up even though both claimed to be aligned.
// Wide enough to clear the chevron glyph (one font size) with air on both
// sides.
constexpr float SectionLabelIndent = 22.0f;
constexpr float InspectorRightPad = 14.0f;  // right gutter so value controls don't touch the edge
constexpr float RowSpacing = 12.0f;         // vertical gap between property rows
constexpr float FieldPadding = 8.0f;        // FramePadding.y inside value frames (top+bottom) -> taller fields
constexpr float PreviewMaxHeight = 200.0f;  // cap for the anchored texture preview (bounds both up- and downscale)
}  // namespace Metrics

// Schematic typography ladder, loaded in main.cpp. Three families across the
// pixel sizes the mockup uses; merged with Tabler Icons on the UI sizes.
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

// Backwards-compatible aliases for existing callsites in InspectorPanel /
// ComponentManager that referenced the older display-font handles.
#define g_DisplayFont g_FontDisplay
#define g_DisplaySmallFont g_FontDisplaySmall

namespace Fonts
{
// Register a loaded font with its TTF source path so we can compute the
// CSS-pixel-to-FreeType-pixel conversion ratio. Call ONCE per font
// immediately after ImFontAtlas::AddFontFromFileTTF returns. The ratio
// comes from the font's own metrics - no hardcoded constants per font.
DEKI_EDITOR_API void RegisterTtf(ImFont* font, const char* ttfPath);

// Convert a CSS pixel size to the ImGui+FreeType size that produces an
// em-square matching CSS's font-size convention.
//
// Why this is needed: ImGui's FreeType wrapper uses
// FT_SIZE_REQUEST_TYPE_REAL_DIM, which treats `pixel_size` as the max
// glyph extent (ascender + |descender|). CSS treats `font-size` as the
// em-square. The ratio differs per font (Inter ≈ 1.21, JetBrains Mono
// ≈ 1.32, Space Grotesk ≈ 1.26). For example to ask for "13px CSS" of
// Inter, call CssPx(g_FontUI, 13.0f).
//
// A fixed-width font is moved to the nearest size whose character advance
// is a whole pixel (12.5 CSS px JetBrains Mono becomes 13.33, 8px per
// character), since FreeType rounds the advance and a fractional one spreads
// the letters apart.
//
// Returns cssPx unchanged if the font wasn't registered.
DEKI_EDITOR_API float CssPx(ImFont* font, float cssPx);
}  // namespace Fonts

// Every tooltip in the editor goes through here, and none of them appear until
// the pointer has rested on the thing for TooltipDelaySeconds. A tooltip that
// pops the instant the cursor crosses a control turns any sweep across a
// toolbar into a flicker of boxes; a delay makes it an answer to a question the
// user actually asked by stopping.
//
// Same signature as ImGui::SetTooltip, so a call site only changes name. The
// wait is measured on the hovered item, and - for tooltips drawn over
// hand-drawn regions that are not ImGui items - on the pointer standing still,
// so no call site has to prove which kind it is.
constexpr float TooltipDelaySeconds = 1.0f;
DEKI_EDITOR_API void Tooltip(const char* fmt, ...) IM_FMTARGS(1);

// Where a window first appears, called just before its Begin: `width` x
// `height`, centred over the editor. Only the first time (FirstUseEver), so a
// window the user moved, resized or docked keeps that. Without it a floating
// window opened at ImGui's fixed (60, 60), over the toolbar.
DEKI_EDITOR_API void FirstUseWindowPlacement(float width, float height);

// A panel's "nothing here" line (empty list, search with no result): dim,
// wrapped, inset like the rows above it rather than against the panel edge.
DEKI_EDITOR_API void EmptyStateText(const char* text);

// One entry in a SchematicPicker list.
struct PickerItem
{
    const char* label = nullptr;     // the row's text, and what search ranks
    const char* altName = nullptr;   // a second searchable string (a type name)
    const char* category = nullptr;  // the group it browses under; null = ungrouped
    // Shown UNDER the label, on its own line. What the picker is for: a list of
    // names alone leaves you guessing which of five similarly named scripts is
    // the one you meant. Falls back to `altName` when empty, so a row still
    // says something about itself.
    const char* description = nullptr;
    const char* tooltip = nullptr;      // shown on hover; the description otherwise
    bool disabled = false;              // listed, but not choosable
    const char* disabledTip = nullptr;  // and why
};

// The editor's search-first picker: two-level browsing (categories, then what
// is inside one), ranked search across every level at once, full keyboard
// control, and a slide when you step between levels.
//
// Draws the BODY of a popup the caller has already begun and placed, so the
// same list serves Add Component in the Inspector and Add Action in a node
// graph stack. Returns the index of the item chosen this frame, or -1.
// `justOpened` resets the query and level and takes keyboard focus.
DEKI_EDITOR_API int SchematicPicker(const char* id, const char* title, const PickerItem* items, int itemCount,
                                    bool justOpened, float listHeight, const char* itemIcon = nullptr,
                                    const char* searchHint = nullptr, const char* emptyText = nullptr);

// Open the shared property-list context: the body indent, the row gap and the
// field padding every list of properties in the editor is laid out with.
//
// Any panel that shows property rows opens this rather than setting its own
// numbers. Doing it by hand is how the Inspector and the package tool windows
// drifted apart: the Inspector pushed FramePadding 8 and a 12px row gap, a tool
// window inherited the theme's 14/10 and the default gap, and the same widget
// came out a different height with different spacing in each - a difference no
// user could name a reason for. Must be paired with EndPropertyContext.
DEKI_EDITOR_API void BeginPropertyContext();
DEKI_EDITOR_API void EndPropertyContext();

// Schematic collapsing header for inspector sections. Renders the label in
// Space Grotesk (Title Case as-typed). Returns true if expanded.
// Preserves ImGui's CollapsingHeader behavior so BeginPopupContextItem
// / OpenPopupOnItemClick still attach correctly.
// With ImGuiTreeNodeFlags_Leaf (and DefaultOpen) it is the same band without
// the chevron and without folding: a column title in a tool window.
DEKI_EDITOR_API bool SchematicCollapsingHeader(const char* label,
                                               ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen);

// The same band as SchematicCollapsingHeader, with the body SLIDING open and
// shut instead of appearing and vanishing.
//
//     if (SchematicSectionBegin("Transform")) { ...rows...; SchematicSectionEnd(); }
//
// Returns true whenever the body must be submitted - which includes the frames
// where the section is closing, since a body that is not submitted cannot be
// animated away. During those frames the body is clipped to the animated
// height, which also makes the hidden part unhittable, and the layout below is
// pulled up to match. Call SchematicSectionEnd() ONLY when this returned true
// (the same convention as SchematicBeginCombo / SchematicEndCombo).
//
// A section opened for the very first time has no measured height yet and so
// opens instantly; every toggle after that animates.
// `outHeaderId` receives the band's ImGui id, in every case including the ones
// that return false. A caller that wants to force a section open (the property
// filter does, so a match is never hidden under a collapsed header) needs it to
// read and later restore the state ImGui keeps for that id.
DEKI_EDITOR_API bool SchematicSectionBegin(const char* label, ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen,
                                           ImGuiID* outHeaderId = nullptr);
DEKI_EDITOR_API void SchematicSectionEnd();

// The children of a tree node, sliding open and shut the same way a section
// body does. Wraps whatever the caller draws between the node and its TreePop:
//
//     const bool open   = ImGui::TreeNodeEx(...);
//     const ImGuiID nid = ImGui::GetItemID();      // capture straight after
//     if (SchematicTreeBodyBegin(open, nid)) { ...children... }
//     SchematicTreeBodyEnd();                      // ALWAYS, whatever Begin said
//     if (open) ImGui::TreePop();                  // ImGui's own push, unchanged
//
// Returns true whenever the children must be submitted, which includes the
// frames where the node is closing. While closing, ImGui has already dropped
// the indent and id scope it pushes for an open node, so this pushes them back
// (and pops them in End) - otherwise the children would lay out at the wrong
// depth and change identity for the length of the animation.
//
// Unlike the section pair, End is called unconditionally: `open` and "submit
// the children" are different questions here, and pairing on the wrong one is
// exactly the mistake worth designing out.
DEKI_EDITOR_API bool SchematicTreeBodyBegin(bool open, ImGuiID nodeId);
DEKI_EDITOR_API void SchematicTreeBodyEnd();

// The same band without the fold: identical height, full bleed and uppercase
// letter-spaced label as SchematicCollapsingHeader, minus the chevron and the
// collapse behavior. For a header that TITLES a panel rather than grouping
// something worth folding away. The whole row is clickable (returns true when
// clicked, e.g. to start an inline rename) and lights up on hover like the
// header does; pass an id-only "###id" label to draw the band empty and place
// your own control on it.
// `outLabelX` receives the screen x the label was drawn at, for callers that
// place their own control on the band and want it to start where the text did.
DEKI_EDITOR_API bool SchematicSectionBand(const char* label, float* outLabelX = nullptr);

// An inline rename field drawn AS a band title: no frame, no background, no
// padding, in the band's own font, size and color, centered on the band's line.
// Pass the band's own label x (SchematicSectionBand's `outLabelX`) and its item
// rect, so the text lands exactly where the title was and the header does not
// change appearance when a rename starts. Returns true when the text changed
// this frame; the caller keeps using the usual IsItem* queries after it.
DEKI_EDITOR_API bool SchematicBandTitleField(const char* id, char* buf, int bufSize, float x, float bandTop,
                                             float bandBottom, float width, const char* hint);

// One rung below both of those: a DEKI_GROUP-style subdivision INSIDE a
// section. Same uppercase letter-spaced treatment, smaller and dimmer, with a
// rule running from the end of the label to the content edge. Draws at the
// current cursor and reserves its own row.
DEKI_EDITOR_API void SchematicSubHeader(const char* label);

// The "add one more" affordance at the end of a list (Add Component, Add
// Transition): a centered outlined box - transparent fill, hairline border, dim
// label - that takes the accent only on hover. Deliberately quieter than a
// filled button: at the bottom of a list, an invitation, not another control to
// fill in. `width` 0 uses a comfortable default, clamped to the space available.
// Returns true when clicked.
DEKI_EDITOR_API bool SchematicAddButton(const char* label, float width = 0.0f);

// Draw a tree node's expand chevron (Tabler chevron, down=open / right=closed)
// and its label, replacing ImGui's built-in filled triangle. Call IMMEDIATELY
// after an empty-label TreeNodeEx that was drawn with a transparent
// ImGuiCol_Text (so the triangle is hidden) and while any pushed FramePadding
// is still active. `expandable` = node can open (leaves draw no chevron);
// `labelColor` is the color the label text should use (capture
// GetColorU32(ImGuiCol_Text) BEFORE pushing the transparent color).
DEKI_EDITOR_API void DrawTreeNodeChevronLabel(bool open, bool expandable, const char* label, ImU32 labelColor);

// Like ImGui::Separator(), but the hairline spans the full panel width, past
// the indent on the left and past the scrollbar on the right, so it touches
// both dock seams. Use it to bound a component section like a window edge.
DEKI_EDITOR_API void FullBleedSeparator();

// Under-tab toolbar hosting the PIN (lock-to-selection) control: a full-width
// strip with a PIN toggle on the left and a "..." (kebab) menu on the right,
// plus a bottom hairline. Call once immediately after ImGui::Begin(...), before
// the panel's own content. `pinned` is the panel-owned flag the PIN button
// toggles; pinning is only meaningful for the Inspector, so a null flag draws
// nothing at all (the toolbar is skipped). Returns the current pin state
// (true = the panel should freeze its content instead of following new
// selections). Note: the bar currently scrolls with content.
DEKI_EDITOR_API bool PanelToolbar(bool* pinned = nullptr);

// Hierarchy / Asset-Browser-style search field: a dim magnifier icon followed by
// a borderless, transparent input (no frame box) hinting "Search", vertically
// centered and reserving its own full-width band. Call once where the search row
// should sit; content below flows under it. `buf`/`bufSize` back the text.
//
// `rightReserve` (px) keeps that much space free at the right of the row for an
// adjacent control (e.g. a filter dropdown), flush against the window's right
// edge. The band keeps the search input's (short) height regardless; the reserved
// slot's full rect is reported via outSlotX / outSlotY / outSlotW / outSlotH (it
// spans the whole band height, flush right) so the caller can fill it with a
// toolbar-style control. `bottomBorder` draws a full-width hairline beneath the row.
//
// `hint` is the placeholder. Say what THIS field searches: a bare "Search" in a
// panel that already has a title is a wasted line, and the same field means
// different things in the inspector and in a picker popup.
//
// While there is text, a dim x appears at the right of the input; clicking it or
// pressing Escape in the field empties the buffer. A borderless search box gives
// no other way back to "no filter" than selecting the text and deleting it, and
// a filter you cannot see the end of looks like a panel with things missing.
DEKI_EDITOR_API void SearchField(const char* id, char* buf, size_t bufSize, float rightReserve = 0.0f,
                                 bool bottomBorder = false, float* outSlotX = nullptr, float* outSlotY = nullptr,
                                 float* outSlotW = nullptr, float* outSlotH = nullptr, const char* hint = "Search");

// Flat, toolbar-styled dropdown matching the panel toolbar's cells: a transparent
// box with a subtle white hover background, no border, squared, `height` px tall
// with the value + chevron centered. `items` is a "A\0B\0" zero-separated list.
// `leftDivider` draws a 1px separator on the box's left edge (the toolbar cell
// divider) to set it off from a neighbour. Returns true the frame the selection
// changes (writes *currentIndex).
DEKI_EDITOR_API bool SchematicToolbarCombo(const char* id, int* currentIndex, const char* items, float height,
                                           bool leftDivider = false);

// The same toolbar dropdown with rows of your own (sections, separators, items
// that do more than pick an index): Begin draws the box and returns true while
// the list is open; draw ImGui::Selectable rows, then call
// SchematicEndToolbarCombo() - only when Begin returned true. Size the box with
// SetNextItemWidth; SchematicToolbarComboWidth(preview) fits it to its text.
DEKI_EDITOR_API bool SchematicBeginToolbarCombo(const char* id, const char* preview, float height,
                                                bool leftDivider = false);
DEKI_EDITOR_API void SchematicEndToolbarCombo();
DEKI_EDITOR_API float SchematicToolbarComboWidth(const char* preview);

// Right-aligned `EDIT ->` style action link. Call after a section header on
// the same row; renders ALL CAPS mono accent text with 1px underline.
// Returns true if clicked.
DEKI_EDITOR_API bool SchematicActionLink(const char* label);

// Drag widgets with LEFT-aligned value text (ImGui's defaults center it).
// Same signature as ImGui::DragFloat / DragInt. Internally calls the ImGui
// widget with an empty format so it draws no value text, then overlays the
// formatted value left-aligned via the window draw list. Edit mode (Ctrl+
// click to type) falls through to ImGui's normal text input.
DEKI_EDITOR_API bool SchematicDragFloat(const char* id, float* v, float v_speed = 1.0f, float v_min = 0.0f,
                                        float v_max = 0.0f, const char* format = "%.3f");
DEKI_EDITOR_API bool SchematicDragInt(const char* id, int* v, float v_speed = 1.0f, int v_min = 0, int v_max = 0,
                                      const char* format = "%d");

// Multi-component drag widgets. Components share the remaining row width and
// are wrapped in a BeginGroup/EndGroup so callers can still use
// ImGui::IsItemActivated / IsItemDeactivatedAfterEdit on the group as a whole
// (for command-history capture). Returns true if any component changed.
DEKI_EDITOR_API bool SchematicDragFloat2(const char* id, float v[2], float v_speed = 1.0f, float v_min = 0.0f,
                                         float v_max = 0.0f, const char* format = "%.3f");
DEKI_EDITOR_API bool SchematicDragFloat3(const char* id, float v[3], float v_speed = 1.0f, float v_min = 0.0f,
                                         float v_max = 0.0f, const char* format = "%.3f");
// One box of those, with its axis stamp (0 = X, 1 = Y, 2 = Z), as an item of
// its own: for a caller that needs each box's activation, or a different
// format per box (a dash for values that differ across a selection).
DEKI_EDITOR_API bool SchematicDragFloatAxis(const char* id, float* v, int axis, float v_speed = 1.0f,
                                            float v_min = 0.0f, float v_max = 0.0f, const char* format = "%.3f");

// Schematic color editors. Same semantics as ImGui::ColorEdit4 / ColorEdit3
// (rgba / rgb in 0-1 floats). Render one cell per channel (each a vertical
// color stripe + 0-255 integer value), plus a combined swatch on the right;
// click the swatch to open the full picker.
DEKI_EDITOR_API bool SchematicColorEdit4(const char* id, float col[4]);
DEKI_EDITOR_API bool SchematicColorEdit3(const char* id, float col[3]);

// Schematic dropdown matching the drag-field look: near-black frame, the
// current item drawn left-aligned in the mono value font, and a Tabler chevron
// on the right. Same semantics as ImGui::Combo. The second overload accepts a
// single "Item1\0Item2\0\0" zero-separated string.
DEKI_EDITOR_API bool SchematicCombo(const char* id, int* currentIndex, const char* const items[], int count);
DEKI_EDITOR_API bool SchematicCombo(const char* id, int* currentIndex, const char* itemsSeparatedByZeros);

// Styled open/close for custom dropdowns whose items aren't a fixed string
// array (e.g. dynamic lists, custom preview text). Same look as SchematicCombo
// (near-black frame, left-aligned mono preview, right chevron); the caller draws
// its own ImGui::Selectable items between Begin and End. Returns true if open;
// only call SchematicEndCombo() when it returned true. `preview` is the closed-
// box text (may be empty).
DEKI_EDITOR_API bool SchematicBeginCombo(const char* id, const char* preview);
DEKI_EDITOR_API void SchematicEndCombo();

// Walk every dock node with a tab bar and draw a 1px accent line along the
// top edge of each selected tab. Call once per frame after all panels have
// rendered (right before ImGui::Render).
DEKI_EDITOR_API void DrawAllTabTopAccents();

// Re-draw the 1px frame for standalone floating windows (dialogs, undocked
// tool windows). With WindowBorderSize = 0 (so docked panels don't double the
// dock separator) those windows would otherwise lose their border. Docked
// panels need nothing here - ImGui's own 1px dock separator is their seam line.
// Call once per frame after all panels have rendered (right before
// ImGui::Render). Pairs with the border settings in SetupEditorTheme.
DEKI_EDITOR_API void DrawDockSeams();

// Schematic checkbox: 1px square border with a smaller filled inner square
// when checked. Replaces ImGui::Checkbox where the schematic look matters.
// `fillColor` is the inner square color (default = Accent).
DEKI_EDITOR_API bool SchematicCheckbox(const char* label, bool* v);
DEKI_EDITOR_API bool SchematicCheckbox(const char* label, bool* v, const ImVec4& fillColor);

// Unified right-click context-menu styling. Every context menu in the editor
// (hierarchy, asset browser, inspector, console, ...) uses these so the popups
// look like the top menu bar's File/Edit dropdowns: a real WindowPadding inset
// plus, once open, widened ItemSpacing for taller rows and full-width hover.
//
// The split is required by ImGui: WindowPadding must be pushed BEFORE the popup
// begins (the window bakes it at Begin() time), while ItemSpacing is pushed
// AFTER it opens (it reads back the popup's WindowPadding.x and only applies to
// the open scope). Sizes are GetFontSize() multiples so they track DPI. Each
// Push has one matching ImGui::PopStyleVar() at the call site:
//
//   PushContextMenuPopupStyle();                 // before Begin*
//   if (ImGui::BeginPopupContextItem()) {
//       PushContextMenuItemStyle();              // after Begin*
//       ImGui::MenuItem(...);
//       ImGui::PopStyleVar();                    // ItemSpacing
//       ImGui::EndPopup();
//   }
//   ImGui::PopStyleVar();                        // WindowPadding
DEKI_EDITOR_API void PushContextMenuPopupStyle();
DEKI_EDITOR_API void PushContextMenuItemStyle();

// Schematic modal dialog shell - centered over the main viewport, fixed
// css-px width (auto height), generous window padding, no saved settings.
// Same look as the Package Manager's Add Package dialog so every modal in the
// editor reads the same. Follows Begin/End popup semantics: only call
// EndSchematicModal() (which wraps ImGui::EndPopup()) when it returned true.
//
//   OpenPopup("Create New Folder");               // elsewhere, as usual
//   if (BeginSchematicModal("Create New Folder", 440.0f)) {
//       ...content...
//       EndSchematicModal();
//   }
DEKI_EDITOR_API bool BeginSchematicModal(const char* title, float cssWidth);
DEKI_EDITOR_API void EndSchematicModal();

// The button row at the foot of a dialog: equal-width buttons across the
// dialog, text centred, the safe choice first and the action last. Primary is
// the accent button that does what the dialog is for; Danger is theme red,
// for deleting or discarding. Returns the index clicked, or -1.
//
//   switch (SchematicDialogButtons({ { "Cancel" }, { "Delete", DialogButton::Danger } }))
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
