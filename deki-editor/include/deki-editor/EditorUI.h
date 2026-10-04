#pragma once

#include <string>
#include <cstdint>
#include <functional>

#include <deki-editor/EditorExport.h>
namespace Deki
{
class Component;
}
namespace Deki
{
struct ComponentMeta;
}

namespace DekiEditor
{

/// The editor's UI controls and inspector drawing, for CustomEditor code:
///   auto& ui = EditorUI::Get();
///   ui.DrawDefaultInspector();
///   ui.PropertyField("speed");
///
/// Packages use this class instead of ImGui directly. The implementations
/// live in the editor executable and are installed through callbacks
/// (BeginComponent for property drawing, SetWidgetBackend for widgets), so
/// package editors draw with the editor's styled widgets.
///
/// InspectorPanel calls BeginComponent/EndComponent around
/// CustomEditor::OnInspectorGUI().
class DEKI_EDITOR_API EditorUI
{
public:
    static EditorUI& Get();

    // =========================================================================
    // Property Drawing (valid between BeginComponent/EndComponent)
    // =========================================================================

    /// Draws every property of the current component, like Unity's
    /// DrawDefaultInspector().
    void DrawDefaultInspector();

    /// Draws one property by member offset: offsetof(ComponentType, member).
    void PropertyField(size_t offset);

    /// Draws one property by name.
    void PropertyField(const char* propertyName);

    // =========================================================================
    // Asset Utilities
    // =========================================================================

    /// A readable path or name for an asset GUID.
    std::string ResolveAssetName(const std::string& guid);

    /// The OpenGL atlas texture of a font at a size, for previews, with its
    /// width and height, or 0 on failure.
    uint32_t GetFontAtlasTexture(const std::string& fontGuid, int fontSize, uint32_t* outWidth, uint32_t* outHeight);

    /// Changes an asset file (absolute path) from `oldJson` to `newJson`, as
    /// an undoable edit.
    void ModifyAsset(const std::string& assetPath, const std::string& assetGuid, const std::string& oldJson,
                     const std::string& newJson);

    // =========================================================================
    // Styled UI controls
    //
    // Packages call these instead of ImGui so their custom-editor fields match
    // the inspector's styled widgets. The implementations live in the editor
    // executable and are installed once via SetWidgetBackend(). Bespoke drawing
    // (popups, draw lists, fonts, tool windows) still uses ImGui in the package.
    // =========================================================================

    /// Collapsing section header (full-bleed band). Returns true if expanded.
    bool SectionHeader(const char* label);
    /// Starts a "label : value" row; the next control fills the value column.
    void PropertyRow(const char* label);

    bool DragInt(const char* id, int* v, float speed = 1.0f, int vmin = 0, int vmax = 0);
    bool DragFloat(const char* id, float* v, float speed = 1.0f, float vmin = 0.0f, float vmax = 0.0f,
                   const char* format = "%.3f");
    /// A visible label (e.g. "Antialiased") draws a whole labeled row, compact
    /// (sized to the checkbox, not the tall field frame). A hidden or empty
    /// label ("##id") draws just the styled box, for custom inline layouts.
    /// Returns true when toggled.
    bool Checkbox(const char* label, bool* v);
    bool ColorEdit4(const char* id, float col[4]);
    bool ColorEdit3(const char* id, float col[3]);
    bool Combo(const char* id, int* currentIndex, const char* const items[], int count);
    bool Combo(const char* id, int* currentIndex, const char* itemsSeparatedByZeros);
    bool SliderFloat(const char* id, float* v, float vmin, float vmax, const char* format = "%.3f");
    bool InputInt(const char* id, int* v, int step = 1, int stepFast = 100);
    bool InputText(const char* id, char* buf, size_t bufSize);
    bool Button(const char* label, float width = 0.0f, float height = 0.0f);
    bool SmallButton(const char* label);

    void Text(const char* text);
    void TextColored(float r, float g, float b, float a, const char* text);
    void TextDisabled(const char* text);
    void SameLine(float offsetFromStartX = 0.0f, float spacing = -1.0f);
    void Spacing();
    void Separator();

    // ID stack, indentation and section helpers, so package inspectors that
    // would use raw ImGui can go through EditorUI. Implemented by the
    // editor's widget backend.
    void PushID(int id);
    void PushID(const void* ptr);
    void PopID();
    void Indent();
    void Unindent();
    /// Indents by a given number of pixels (e.g. a panel's content gutter).
    /// Pair Indent(amount) with Unindent(amount).
    void Indent(float amount);
    void Unindent(float amount);
    void BulletText(const char* text);
    /// Collapsing header with an optional close button. With a null
    /// `pVisible` there is no close button; otherwise clicking it sets
    /// *pVisible to false. Returns true when the section is expanded.
    bool CollapsingHeader(const char* label, bool* pVisible = nullptr, bool defaultOpen = false);

    // =========================================================================
    // Custom drawing (into the current window)
    //
    // For tools that draw their own content: grids, previews, guides, atlases.
    // Coordinates are screen-space pixels; pack colors with Rgba().
    // (Scene-view gizmos draw with SceneView's Draw* methods instead, which
    // target the gizmo surface rather than the current window.)
    // =========================================================================

    /// Packs an RGBA color (components 0-255) for the Draw* methods.
    static uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
    {
        return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) |
               static_cast<uint32_t>(r);
    }

    // Custom drawing maps 1:1 to ImGui: draw at GetCursorScreenPos(), then call
    // Dummy() (or a widget) to reserve the space so following widgets flow
    // past it. The Draw* methods below draw on the current window's draw list,
    // exactly like ImGui::GetWindowDrawList()->Add*.

    /// Top-left screen position where the next widget or drawing goes.
    void GetCursorScreenPos(float* outX, float* outY);
    /// Puts the layout cursor at an absolute screen position, for custom
    /// chrome that places widgets and drawing by hand (e.g. toolbars).
    void SetCursorScreenPos(float x, float y);
    /// Advances the layout cursor by width x height, reserving space for drawing.
    void Dummy(float width, float height);

    void DrawLine(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness = 1.0f);
    void DrawRect(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness = 1.0f, float rounding = 0.0f);
    void DrawRectFilled(float x0, float y0, float x1, float y1, uint32_t rgba, float rounding = 0.0f);
    void DrawCircle(float cx, float cy, float radius, uint32_t rgba, float thickness = 1.0f);
    void DrawCircleFilled(float cx, float cy, float radius, uint32_t rgba);
    void DrawImage(uint32_t texture, float x0, float y0, float x1, float y1, float u0 = 0.0f, float v0 = 0.0f,
                   float u1 = 1.0f, float v1 = 1.0f, uint32_t tint = 0xFFFFFFFFu);
    // Named DrawTextAt, not DrawText: <windows.h> #defines DrawText as
    // DrawTextA/W, which would break any package that includes it.
    void DrawTextAt(float pixelSize, float x, float y, uint32_t rgba, const char* text);
    /// Text sized in CSS px: the backend converts with
    /// Fonts::CssPx(currentFont, cssPx) and applies the DPI scale, so
    /// hand-drawn labels match widget text. Use this rather than DrawTextAt
    /// (raw pixels) for any UI-like text.
    void DrawTextCss(float cssPx, float x, float y, uint32_t rgba, const char* text);
    /// Measures text exactly as DrawTextCss draws it.
    void MeasureTextCss(float cssPx, const char* text, float* outW, float* outH);
    void PushClipRect(float x0, float y0, float x1, float y1, bool intersectWithCurrent = true);
    void PopClipRect();

    /// Inline image widget that advances the layout, e.g. a texture or atlas preview.
    void Image(uint32_t texture, float w, float h, float u0 = 0.0f, float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f);

    // =========================================================================
    // Generic toolkit: queries, containers, popups, windows, styling, input.
    // These mirror the matching ImGui calls so tools can be built entirely
    // through EditorUI. Stable, append-only.
    // =========================================================================

    /// Editor style color slots, mapped to the theme.
    enum class Col
    {
        Text,
        Button,
        ButtonHovered,
        ButtonActive,
        FrameBg,
        FrameBgHovered,
        FrameBgActive,
        CheckMark
    };

    /// Tree node flags (bitwise-or).
    enum TreeFlags
    {
        TreeNone = 0,
        TreeDefaultOpen = 1 << 0,
        TreeLeaf = 1 << 1,
        TreeOpenOnArrow = 1 << 2,
        TreeSelected = 1 << 3
    };

    /// Window flags (bitwise-or).
    enum WinFlags
    {
        WinNone = 0,
        WinNoScrollbar = 1 << 0,
        WinAlwaysAutoResize = 1 << 1,
        WinNoSavedSettings = 1 << 2,
        WinNoCollapse = 1 << 3
    };

    /// Table flags (bitwise-or).
    enum TableFlags
    {
        TableNone = 0,
        TableResizable = 1 << 0,
        TableRowBg = 1 << 1,
        TableScrollY = 1 << 2,
        TableBordersInnerV = 1 << 3,
        TableBordersOuter = 1 << 4
    };
    /// Table column flags (bitwise-or).
    enum TableColumnFlags
    {
        TableColumnNone = 0,
        TableColumnWidthStretch = 1 << 0,
        TableColumnWidthFixed = 1 << 1
    };

    // --- Queries / layout ---
    bool IsItemHovered(bool allowWhenDisabled = false);
    bool IsItemClicked();
    void GetContentRegionAvail(float* outW, float* outH);
    void GetContentRegionMax(float* outX, float* outY);
    void GetFramePadding(float* outX, float* outY);
    void GetItemSpacing(float* outX, float* outY);
    void GetItemInnerSpacing(float* outX, float* outY);
    /// Bounds of the last submitted item (screen space), e.g. to hang a
    /// control off the right edge of a row just drawn.
    void GetItemRect(float* outMinX, float* outMinY, float* outMaxX, float* outMaxY);
    float GetFrameHeight();
    /// Width the next framed control would take with the current item width
    /// and indent, for splitting a row between two controls.
    float CalcItemWidth();
    float GetFontSize();
    void MeasureText(const char* text, float* outW, float* outH);
    void AlignTextToFramePadding();
    void SetTooltip(const char* text);
    float GetTextLineHeight();
    float GetFrameHeightWithSpacing();
    void PushItemWidth(float w);
    void PopItemWidth();
    void GetMainViewportRect(float* outX, float* outY, float* outW, float* outH);

    // --- More text and widgets ---
    void TextWrapped(const char* text);
    bool SliderInt(const char* id, int* v, int vmin, int vmax);
    bool RadioButton(const char* label, bool active);
    bool Selectable(const char* label, bool selected);
    /// Selectable spanning every column of the enclosing table (a full-width row).
    bool Selectable(const char* label, bool selected, bool spanAllColumns);
    bool ImageButton(const char* id, uint32_t texture, float w, float h, float u0 = 0.0f, float v0 = 0.0f,
                     float u1 = 1.0f, float v1 = 1.0f);
    bool InvisibleButton(const char* id, float w, float h);
    void SetNextItemWidth(float w);
    // Draws only the visible rows of a long list (one clipper at a time; do not nest):
    //   ui.BeginListClipper(count, rowHeight);
    //   int s, e; while (ui.ListClipperStep(&s, &e)) for (int i=s;i<e;++i) {...}
    //   ui.EndListClipper();
    void BeginListClipper(int itemCount, float itemHeight = -1.0f);
    bool ListClipperStep(int* outStart, int* outEnd);
    void EndListClipper();
    bool InputTextWithHint(const char* id, const char* hint, char* buf, int bufSize);
    bool BeginCombo(const char* label, const char* preview);
    void EndCombo();

    // --- Containers ---
    bool BeginChild(const char* id, float w, float h, bool border = false);
    void EndChild();
    bool BeginTabBar(const char* id);
    void EndTabBar();
    bool BeginTabItem(const char* label);
    void EndTabItem();
    bool TreeNodeEx(const char* label, int treeFlags = 0);
    void TreePop();

    /// Treats a run of widgets as one item (for SameLine, sizing and hit
    /// testing the whole block). Pair BeginGroup with EndGroup.
    void BeginGroup();
    void EndGroup();

    // --- Tables (columnar layout: headers, resizable columns, row backgrounds) ---
    // Usage:
    //   if (ui.BeginTable("id", 3, EditorUI::TableRowBg | EditorUI::TableScrollY, 0, h)) {
    //     ui.TableSetupColumn("A", EditorUI::TableColumnWidthStretch);
    //     ui.TableSetupColumn("B", EditorUI::TableColumnWidthFixed, 80);
    //     ui.TableHeadersRow();
    //     for (...) { ui.TableNextRow(); ui.TableNextColumn(); ...; ui.TableNextColumn(); ... }
    //     ui.EndTable();
    //   }
    bool BeginTable(const char* id, int columns, int tableFlags = 0, float outerW = 0.0f, float outerH = 0.0f);
    void TableSetupColumn(const char* label, int columnFlags = 0, float widthOrWeight = 0.0f);
    void TableSetupScrollFreeze(int cols, int rows);
    void TableHeadersRow();
    void TableNextRow();
    bool TableNextColumn();
    void EndTable();

    // --- Toolbar (the Console panel's full-bleed strip: cells with 1px dividers,
    // hover/active backgrounds, dim->bright text, and a bottom hairline) ---
    // Usage:
    //   ui.BeginToolbar();
    //   if (ui.ToolbarIconButton("##new", ICON_TI_PLUS, "New", enabled)) { ... }
    //   if (ui.ToolbarButton("Refresh")) { ... }
    //   ui.EndToolbar();
    // One toolbar at a time; it draws full width at the current cursor.
    void BeginToolbar();
    bool ToolbarButton(const char* label, bool enabled = true, bool active = false);
    /// `destructive` marks a cell that removes something: it is drawn in the
    /// palette's red with a red hover fill, so a delete never looks like the
    /// buttons beside it.
    bool ToolbarIconButton(const char* id, const char* icon, const char* tooltip = nullptr, bool enabled = true,
                           bool active = false, bool destructive = false);
    void EndToolbar();

    /// Sub-nav "tab links": a full-bleed row of text labels with a 2px accent
    /// underline on the active one (dim -> bright on hover) and a bottom
    /// hairline, like the Asset Browser's Assets/Scripts tabs. Pair
    /// BeginTabLinks with EndTabLinks; draw each with TabLink (returns true
    /// when clicked).
    void BeginTabLinks();
    bool TabLink(const char* label, bool active);
    void EndTabLinks();

    /// A richer list entry than Selectable: a full-width "card" with a bold
    /// title and an optional dim subtitle, rounded hover/selected backgrounds,
    /// and a left accent bar when selected. Reserves its own height, so cards
    /// stack vertically. Returns true when clicked; call IsItemHovered() /
    /// IsMouseDoubleClicked() right after for hover and double-click.
    /// `subtitle` may be null for a single line. `subtitleColor` (packed with
    /// Rgba()) tints the subtitle; 0 = the default dim.
    bool SelectableCard(const char* id, const char* title, const char* subtitle, bool selected,
                        uint32_t subtitleColor = 0);

    // --- Popups / menus ---
    void OpenPopup(const char* id);
    bool BeginPopup(const char* id);
    bool BeginPopupModal(const char* name, bool* open = nullptr, int winFlags = 0);
    bool BeginPopupContextItem(const char* id = nullptr);
    /// Opens `id` when the last submitted item is clicked with `mouseButton`
    /// (0 = left, 1 = right, 2 = middle). For context menus on a widget that
    /// already uses the click itself.
    void OpenPopupOnItemClick(const char* id, int mouseButton = 1);
    void EndPopup();
    void CloseCurrentPopup();
    bool MenuItem(const char* label, bool enabled = true);
    /// Submenu inside an open popup or menu. Call EndMenu() only when
    /// BeginMenu() returned true.
    bool BeginMenu(const char* label);
    void EndMenu();

    // --- Windows (standalone tool windows) ---
    bool Begin(const char* name, bool* open = nullptr, int winFlags = 0);
    void End();
    void SetNextWindowSize(float w, float h, bool firstUseEver = false);
    void SetNextWindowSizeConstraints(float minW, float minH, float maxW, float maxH);
    void SetNextWindowPos(float x, float y, float pivotX = 0.0f, float pivotY = 0.0f);

    // --- Styling ---
    /// Vec2 style vars for PushStyleVar (balance with PopStyleVar).
    enum class StyleVar
    {
        ItemSpacing,
        ItemInnerSpacing,
        FramePadding,
        CellPadding,
        WindowPadding
    };
    void PushStyleColor(Col col, uint32_t rgba);
    void PopStyleColor(int count = 1);
    uint32_t GetStyleColor(Col col);
    void PushButtonTextAlign(float x, float y);  // the one style-var packages use
    /// Pushes a Vec2 style var (e.g. table CellPadding, ItemSpacing); pop with PopStyleVar.
    void PushStyleVar(StyleVar var, float x, float y);
    void PopStyleVar(int count = 1);
    void TextColored(uint32_t rgba, const char* text);  // packed-color overload
    void PushBoldFont();                                // the editor's bold UI font; balance with PopFont
    void PopFont();

    // --- Input state (packages use this instead of ImGui's GetIO()) ---
    enum class Key
    {
        Z,
        Y,
        S,
        Delete
    };  // extend as tools need more named keys
    /// Mouse cursor shapes for SetMouseCursor (e.g. splitter resize handles).
    enum class MouseCursor
    {
        Arrow,
        TextInput,
        ResizeNS,
        ResizeEW,
        ResizeAll,
        Hand,
        NotAllowed
    };
    void GetMousePos(float* outX, float* outY);
    void GetMouseDelta(float* outX, float* outY);
    bool IsMouseDown(int button);
    bool IsMouseClicked(int button);
    bool IsMouseDoubleClicked(int button);
    bool IsMouseDragging(int button, float lockThreshold = -1.0f);
    void SetMouseCursor(MouseCursor cursor);
    float GetMouseWheel();
    bool IsKeyShift();
    bool IsKeyCtrl();
    /// The global DPI scale (FontScaleDpi). Multiply custom-drawn geometry by
    /// it to match widget sizes (DrawTextCss already applies it to text).
    float GetDpiScale();
    bool IsKeyAlt();
    bool IsKeyPressed(Key key, bool repeat = true);
    float GetDeltaTime();
    bool IsWindowFocused(bool rootAndChildWindows = false);
    bool IsItemActive();
    bool IsItemDeactivatedAfterEdit();
    /// Lost focus this frame, edited or not. IsItemDeactivatedAfterEdit stays
    /// false when the value was left alone, so it cannot close an inline edit
    /// field the user clicked away from without typing.
    bool IsItemDeactivated();
    /// Gives the NEXT widget keyboard focus, for fields that should appear
    /// focused (click-to-rename) so the user can type without a second click.
    void SetKeyboardFocusHere();
    /// InvisibleButton that takes which mouse buttons activate it.
    bool InvisibleButton(const char* id, float w, float h, bool allowLeft, bool allowMiddle, bool allowRight = false);
    /// Lets items submitted LATER in the same window win the hit test against
    /// the NEXT item, so submission order becomes front-to-back hit testing.
    /// Without it, a big hit surface (a canvas) takes the hovered id for the
    /// frame and every widget drawn on top of it cannot be clicked. Call it
    /// right before the surface, not before the widgets on top.
    void SetNextItemAllowOverlap();

    // Low-level styled controls without labels: building blocks for the
    // *Field helpers and for custom rows. DragFloat2/3 wrap N drag fields and
    // their axis stamps.
    bool DragFloat2(const char* id, float v[2], float speed = 1.0f, float vmin = 0.0f, float vmax = 0.0f,
                    const char* format = "%.3f");
    bool DragFloat3(const char* id, float v[3], float speed = 1.0f, float vmin = 0.0f, float vmax = 0.0f,
                    const char* format = "%.3f");
    void FullBleedSeparator();
    void BeginDisabled(bool disabled = true);
    void EndDisabled();
    void Label(const char* text) { Text(text); }
    void Space() { Spacing(); }

    // =========================================================================
    // Unity-style fields: label column and styled control in one call. Each
    // returns true the frame the value changed and records its own undo. Two
    // modes:
    //   - Pointer mode: pass T*. The field reads and writes it, and its
    //     command restores or reapplies the bytes; callbacks add side effects.
    //   - Value mode: pass the current value(s). The field keeps an internal
    //     copy and sends every change through the callbacks; its command
    //     stores old/new copies and calls onUndo(old)/onRedo(new). YOU apply
    //     the value in onChange/onUndo/onRedo (it never writes your storage).
    // Each of the five callbacks gets the value:
    //   onStartChange(begin) / onChange(live) / onEndChange(old,new) / onUndo / onRedo.
    // =========================================================================
    struct FloatCallbacks
    {
        std::function<void(float)> onStartChange, onChange, onUndo, onRedo;
        std::function<void(float, float)> onEndChange;
    };
    struct IntCallbacks
    {
        std::function<void(int64_t)> onStartChange, onChange, onUndo, onRedo;
        std::function<void(int64_t, int64_t)> onEndChange;
    };
    struct VecCallbacks
    {
        std::function<void(const float*)> onStartChange, onChange, onUndo, onRedo;
        std::function<void(const float*, const float*)> onEndChange;
    };
    struct EnumCallbacks
    {
        std::function<void(int)> onStartChange, onChange, onUndo, onRedo;
        std::function<void(int, int)> onEndChange;
    };
    struct TextCallbacks
    {
        std::function<void(const char*)> onStartChange, onChange, onUndo, onRedo;
        std::function<void(const char*, const char*)> onEndChange;
    };

    /// Physical unit of a float or vector field. The value is stored in
    /// canonical units (meters, radians); the editor shows it in the project's
    /// display unit (px/m, degrees) and snaps Distance to whole pixels in
    /// Pixels mode. Mirrors the engine's Deki::PhysicalUnit so the int passes
    /// straight through.
    enum class Unit
    {
        None = 0,
        Distance,
        Velocity,
        Acceleration,
        Angle
    };

    // --- Pointer mode (the field reads/writes your member) ---
    // Integer fields: the pointer type picks the width and signedness.
    bool IntField(const char* label, int32_t* v, float speed = 1.0f, int64_t vmin = 0, int64_t vmax = 0,
                  const IntCallbacks& cb = {});
    bool IntField(const char* label, uint32_t* v, float speed = 1.0f, int64_t vmin = 0, int64_t vmax = 0,
                  const IntCallbacks& cb = {});
    bool IntField(const char* label, int16_t* v, float speed = 1.0f, int64_t vmin = 0, int64_t vmax = 0,
                  const IntCallbacks& cb = {});
    bool IntField(const char* label, uint16_t* v, float speed = 1.0f, int64_t vmin = 0, int64_t vmax = 0,
                  const IntCallbacks& cb = {});
    bool IntField(const char* label, int8_t* v, float speed = 1.0f, int64_t vmin = 0, int64_t vmax = 0,
                  const IntCallbacks& cb = {});
    bool IntField(const char* label, uint8_t* v, float speed = 1.0f, int64_t vmin = 0, int64_t vmax = 0,
                  const IntCallbacks& cb = {});

    bool FloatField(const char* label, float* v, float speed = 1.0f, float vmin = 0.0f, float vmax = 0.0f,
                    const char* format = "%.3f", const FloatCallbacks& cb = {});
    bool Vector2Field(const char* label, float v[2], float speed = 1.0f, float vmin = 0.0f, float vmax = 0.0f,
                      const char* format = "%.3f", const VecCallbacks& cb = {});
    bool Vector3Field(const char* label, float v[3], float speed = 1.0f, float vmin = 0.0f, float vmax = 0.0f,
                      const char* format = "%.3f", const VecCallbacks& cb = {});
    bool FloatField(const char* label, float* v, Unit unit, const FloatCallbacks& cb = {});
    bool Vector2Field(const char* label, float v[2], Unit unit, const VecCallbacks& cb = {});
    bool Vector3Field(const char* label, float v[3], Unit unit, const VecCallbacks& cb = {});
    bool ColorField(const char* label, float v[4], const VecCallbacks& cb = {});
    bool ColorRGBField(const char* label, float v[3], const VecCallbacks& cb = {});
    bool EnumField(const char* label, int* index, const char* const items[], int count, const EnumCallbacks& cb = {});
    bool EnumField(const char* label, int* index, const char* itemsZeroSep, const EnumCallbacks& cb = {});
    bool InputField(const char* label, char* buf, size_t bufSize, const TextCallbacks& cb = {});
    bool SliderField(const char* label, float* v, float vmin, float vmax, const char* format = "%.3f",
                     const FloatCallbacks& cb = {});
    bool SliderField(const char* label, int* v, int vmin, int vmax, const EnumCallbacks& cb = {});

    // --- Value mode: pass the current value(s); apply them in the callbacks.
    // Vector components are passed one by one so a call cannot bind to the
    // pointer overloads. `Unit::None` for no unit. ---
    bool FloatField(const char* label, float value, Unit unit, const FloatCallbacks& cb);
    bool Vector2Field(const char* label, float x, float y, Unit unit, const VecCallbacks& cb);
    bool Vector3Field(const char* label, float x, float y, float z, Unit unit, const VecCallbacks& cb);
    // Value mode with explicit drag speed/range (no unit), for things like Scale.
    bool FloatField(const char* label, float value, float speed, float vmin, float vmax, const char* format,
                    const FloatCallbacks& cb);
    bool Vector2Field(const char* label, float x, float y, float speed, float vmin, float vmax, const char* format,
                      const VecCallbacks& cb);
    bool Vector3Field(const char* label, float x, float y, float z, float speed, float vmin, float vmax,
                      const char* format, const VecCallbacks& cb);

    // The ImGui implementations, installed once by the editor executable.
    // EditCallbacksBytes are the edit callbacks the backend calls, with the
    // values as raw bytes.
    struct EditCallbacksBytes
    {
        std::function<void(const void*)> onStartChange;             // begin (before-value bytes)
        std::function<void(const void*)> onChange;                  // live (current-value bytes)
        std::function<void(const void*, const void*)> onEndChange;  // commit (old,new bytes)
        std::function<void(const void*)> onUndo;                    // undo (old-value bytes)
        std::function<void(const void*)> onRedo;                    // redo (new-value bytes)
    };
    struct WidgetBackend
    {
        std::function<bool(const char*)> sectionHeader;
        std::function<void(const char*)> propertyRow;
        std::function<bool(const char*, int*, float, int, int)> dragInt;
        std::function<bool(const char*, float*, float, float, float, const char*)> dragFloat;
        std::function<bool(const char*, bool*)> checkbox;
        std::function<bool(const char*, float*)> colorEdit4;
        std::function<bool(const char*, float*)> colorEdit3;
        std::function<bool(const char*, int*, const char* const*, int)> combo;
        std::function<bool(const char*, int*, const char*)> comboStr;
        // Styled open/close for custom dropdowns (caller draws its own items).
        std::function<bool(const char*, const char*)> beginCombo;
        std::function<void()> endCombo;
        std::function<bool(const char*, float*, float, float, const char*)> sliderFloat;
        std::function<bool(const char*, int*, int, int)> sliderInt;
        std::function<bool(const char*, int*, int, int)> inputInt;
        std::function<bool(const char*, char*, size_t)> inputText;
        std::function<bool(const char*, float, float)> button;
        std::function<bool(const char*)> smallButton;
        std::function<void(const char*)> text;
        std::function<void(float, float, float, float, const char*)> textColored;
        std::function<void(const char*)> textDisabled;
        std::function<void(float, float)> sameLine;
        std::function<void()> spacing;
        std::function<void()> separator;
        std::function<void(int)> pushIdInt;
        std::function<void()> popId;
        std::function<void()> indent;
        std::function<void()> unindent;
        std::function<void(const char*)> bulletText;
        std::function<bool(const char*, bool*, bool)> collapsingHeader;
        std::function<void(float*, float*)> cursorScreenPos;
        std::function<void(float, float)> setCursorScreenPos;
        std::function<void(float, float)> dummy;
        std::function<void(float, float, float, float, uint32_t, float)> drawLine;
        std::function<void(float, float, float, float, uint32_t, float, float)> drawRect;
        std::function<void(float, float, float, float, uint32_t, float)> drawRectFilled;
        std::function<void(float, float, float, uint32_t, float)> drawCircle;
        std::function<void(float, float, float, uint32_t)> drawCircleFilled;
        std::function<void(uint32_t, float, float, float, float, float, float, float, float, uint32_t)> drawImage;
        std::function<void(float, float, float, uint32_t, const char*)> drawText;
        std::function<void(float, float, float, float, bool)> pushClipRect;
        std::function<void()> popClipRect;
        std::function<bool(const char*, float*, float, float, float, const char*)> dragFloat2;
        std::function<bool(const char*, float*, float, float, float, const char*)> dragFloat3;
        std::function<void()> fullBleedSeparator;
        std::function<void(bool)> beginDisabled;
        std::function<void()> endDisabled;
        // Console-style toolbar strip.
        std::function<void()> beginToolbar;
        std::function<bool(const char*, bool, bool)> toolbarButton;
        std::function<bool(const char*, const char*, const char*, bool, bool, bool)> toolbarIconButton;
        std::function<void()> endToolbar;
        std::function<void()> beginTabLinks;
        std::function<bool(const char*, bool)> tabLink;
        std::function<void()> endTabLinks;
        std::function<bool(const char*, const char*, const char*, bool, uint32_t)> selectableCard;
        // Undo handling for drag-style *Field helpers, run after the field's
        // control. `value` is the current value's bytes (the field's member in
        // pointer mode, an internal temp in value mode). With `ownsStorage`
        // (pointer mode) the pushed command memcpy's `value` on undo/redo;
        // without it (value mode) the command only calls cb.onUndo/onRedo.
        std::function<void(void* value, size_t size, bool ownsStorage, const void* before, const char* label,
                           const EditCallbacksBytes& cb)>
            recordEdit;
        // recordEdit for widgets that change in one step (combo): the DLL
        // calls it only on the frame the value changed.
        std::function<void(void* value, size_t size, bool ownsStorage, const void* before, const char* label,
                           const EditCallbacksBytes& cb)>
            commitEdit;
        // Float/vector field with a unit: draws, converts (px/m, degrees) and
        // snaps, then runs the drag-style undo handling. `value` is `count`
        // floats in canonical units.
        std::function<bool(const char* label, float* value, int count, int unitKind, bool ownsStorage,
                           const EditCallbacksBytes& cb)>
            unitField;
        // CSS-px text (see DrawTextCss): the backend applies Fonts::CssPx and the DPI scale.
        std::function<void(float, float, float, uint32_t, const char*)> drawTextCss;
        std::function<void(float, const char*, float*, float*)> measureTextCss;
    };
    static void SetWidgetBackend(const WidgetBackend& backend);

    /// Extra ImGui window flags for a window, by the name passed to Begin().
    /// The editor executable installs it to make a maximized panel
    /// non-resizable: the flag must reach ImGui::Begin() itself, which decides
    /// whether the resize grips, border highlight and resize cursor exist.
    /// Null (the default) adds nothing.
    static void SetExtraWindowFlagsProvider(std::function<int(const char*)> fn);

    // =========================================================================
    // Current Component (valid between BeginComponent/EndComponent)
    // =========================================================================

    Deki::Component* GetCurrentComponent() const { return m_CurrentComponent; }
    const Deki::ComponentMeta* GetCurrentMeta() const { return m_CurrentMeta; }

    // =========================================================================
    // Internal - Called by InspectorPanel
    // =========================================================================

    // Property drawing callbacks.
    using DrawPropertyByNameFunc = std::function<void(const char*)>;
    using DrawPropertyByOffsetFunc = std::function<void(size_t)>;
    using DrawDefaultPropertiesFunc = std::function<void()>;
    using ResolveAssetNameFunc = std::function<std::string(const std::string&)>;
    using GetFontAtlasFunc = std::function<uint32_t(const std::string&, int, uint32_t*, uint32_t*)>;
    using ModifyAssetFunc =
        std::function<void(const std::string&, const std::string&, const std::string&, const std::string&)>;

    /// Starts drawing a component's Inspector. Called by InspectorPanel.
    void BeginComponent(Deki::Component* comp, const Deki::ComponentMeta* meta, DrawPropertyByNameFunc drawByName,
                        DrawPropertyByOffsetFunc drawByOffset, DrawDefaultPropertiesFunc drawDefault,
                        ResolveAssetNameFunc resolveAsset, GetFontAtlasFunc getFontAtlas,
                        ModifyAssetFunc modifyAsset = nullptr);

    void EndComponent();

private:
    EditorUI() = default;
    ~EditorUI() = default;

    EditorUI(const EditorUI&) = delete;
    EditorUI& operator=(const EditorUI&) = delete;

    Deki::Component* m_CurrentComponent = nullptr;
    const Deki::ComponentMeta* m_CurrentMeta = nullptr;

    // Set by InspectorPanel.
    DrawPropertyByNameFunc m_DrawPropertyByName;
    DrawPropertyByOffsetFunc m_DrawPropertyByOffset;
    DrawDefaultPropertiesFunc m_DrawDefaultProperties;
    ResolveAssetNameFunc m_ResolveAssetName;
    GetFontAtlasFunc m_GetFontAtlas;
    ModifyAssetFunc m_ModifyAsset;
};

}  // namespace DekiEditor

/// Draws a property by member: DRAW_PROPERTY_FIELD(SpriteComponent, width);
#define DRAW_PROPERTY_FIELD(ComponentType, member)                                                                     \
    DekiEditor::EditorUI::Get().PropertyField(offsetof(ComponentType, member))
