#pragma once

#include <string>
#include <cstdint>
#include <functional>

#include <deki-editor/EditorExport.h>
// Forward declarations
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

/**
 * @brief Singleton providing the editor UI controls + inspector drawing.
 *
 * Unity-like static access for CustomEditor implementations:
 *   auto& ui = EditorUI::Get();
 *   ui.DrawDefaultInspector();
 *   ui.PropertyField("speed");
 *
 * Packages depend on this class instead of ImGui directly: the implementations
 * live in the editor executable and are installed via the callback bridge
 * (BeginComponent for property drawing; widget methods added in later phases),
 * so package editors render with the editor's styled widgets.
 *
 * The InspectorPanel calls BeginComponent/EndComponent to set up context
 * before invoking CustomEditor::OnInspectorGUI().
 */
class DEKI_EDITOR_API EditorUI
{
public:
    /**
     * @brief Get singleton instance
     */
    static EditorUI& Get();

    // =========================================================================
    // Property Drawing (valid between BeginComponent/EndComponent)
    // =========================================================================

    /**
     * @brief Draw all default properties for current component
     *
     * Equivalent to Unity's DrawDefaultInspector().
     */
    void DrawDefaultInspector();

    /**
     * @brief Draw a specific property by member offset
     * @param offset offsetof(ComponentType, member)
     */
    void PropertyField(size_t offset);

    /**
     * @brief Draw a specific property by name
     * @param propertyName Name of the property to draw
     */
    void PropertyField(const char* propertyName);

    // =========================================================================
    // Asset Utilities
    // =========================================================================

    /**
     * @brief Resolve asset GUID to display name
     * @param guid Asset GUID
     * @return Human-readable asset path/name
     */
    std::string ResolveAssetName(const std::string& guid);

    /**
     * @brief Get font atlas texture for preview
     * @param fontGuid Font asset GUID
     * @param fontSize Font size
     * @param outWidth Output atlas width
     * @param outHeight Output atlas height
     * @return OpenGL texture ID, or 0 on failure
     */
    uint32_t GetFontAtlasTexture(const std::string& fontGuid, int fontSize, uint32_t* outWidth, uint32_t* outHeight);

    /**
     * @brief Modify an asset file with undo/redo support
     * @param assetPath Absolute path to the asset file
     * @param assetGuid Asset GUID
     * @param oldJson Old JSON state (for undo)
     * @param newJson New JSON state (to apply)
     */
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

    // Collapsing section header (full-bleed band). Returns true if expanded.
    bool SectionHeader(const char* label);
    // Begin a "label : value" row; the next control fills the value column.
    void PropertyRow(const char* label);

    bool DragInt(const char* id, int* v, float speed = 1.0f, int vmin = 0, int vmax = 0);
    bool DragFloat(const char* id, float* v, float speed = 1.0f, float vmin = 0.0f, float vmax = 0.0f,
                   const char* format = "%.3f");
    // Unified checkbox. A visible label (e.g. "Antialiased") draws a complete
    // labeled row, compact (sized to the checkbox box, not the tall field frame).
    // A hidden/empty label ("##id") draws just the bare styled box for custom
    // inline layouts. Returns true when toggled.
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

    // ID stack, indentation, and section helpers. These exist so package
    // inspectors that were written against raw ImGui can route through EditorUI
    // instead. Implemented by the editor's widget backend.
    void PushID(int id);
    void PushID(const void* ptr);
    void PopID();
    void Indent();
    void Unindent();
    // Indent/unindent by an explicit pixel amount (e.g. for a panel's content
    // gutter). Pair Indent(amount) with Unindent(amount).
    void Indent(float amount);
    void Unindent(float amount);
    void BulletText(const char* text);
    // Collapsing header with an optional close button. pVisible may be null (no
    // close button); when non-null and the user clicks the close button it is
    // set to false. Returns true when the section is expanded.
    bool CollapsingHeader(const char* label, bool* pVisible = nullptr, bool defaultOpen = false);

    // =========================================================================
    // Custom drawing (into the current window)
    //
    // For tools that draw their own content: grids, previews, guides, atlases.
    // Coordinates are screen-space pixels; pack colors with Rgba(). Reserve a
    // region first with BeginCanvas() to get an origin and flow layout around it.
    // (Scene-view gizmos draw via SceneView's Draw* methods instead, which
    // target the gizmo surface rather than the current window.)
    // =========================================================================

    /** @brief Pack an RGBA color (components 0-255) for the Draw* methods. */
    static uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
    {
        return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) |
               static_cast<uint32_t>(r);
    }

    // Custom drawing maps 1:1 to ImGui: draw at GetCursorScreenPos(), then call
    // Dummy() (or a widget) to reserve the layout space so following widgets flow
    // past it. The Draw* methods below target the current window's draw list,
    // exactly like ImGui::GetWindowDrawList()->Add*.

    /** @brief Top-left screen position where the next widget/drawing goes. */
    void GetCursorScreenPos(float* outX, float* outY);
    /** @brief Place the layout cursor at an absolute screen position (for custom
     *  chrome that positions widgets/draws by hand, e.g. toolbars). */
    void SetCursorScreenPos(float x, float y);
    /** @brief Advance the layout cursor by w x h (reserve space for drawing). */
    void Dummy(float width, float height);

    void DrawLine(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness = 1.0f);
    void DrawRect(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness = 1.0f, float rounding = 0.0f);
    void DrawRectFilled(float x0, float y0, float x1, float y1, uint32_t rgba, float rounding = 0.0f);
    void DrawCircle(float cx, float cy, float radius, uint32_t rgba, float thickness = 1.0f);
    void DrawCircleFilled(float cx, float cy, float radius, uint32_t rgba);
    void DrawImage(uint32_t texture, float x0, float y0, float x1, float y1, float u0 = 0.0f, float v0 = 0.0f,
                   float u1 = 1.0f, float v1 = 1.0f, uint32_t tint = 0xFFFFFFFFu);
    // Named DrawTextAt (not DrawText) on purpose: <windows.h> #defines DrawText
    // as DrawTextA/W, which would break any package that includes it.
    void DrawTextAt(float pixelSize, float x, float y, uint32_t rgba, const char* text);
    // CSS-px text: the backend converts via Fonts::CssPx(currentFont, cssPx) and
    // applies the DPI scale, so hand-drawn labels match widget text sizing.
    // Prefer this over DrawTextAt for any UI-like text (DrawTextAt is raw pixels).
    void DrawTextCss(float cssPx, float x, float y, uint32_t rgba, const char* text);
    // Measure text exactly as DrawTextCss will draw it (same size conversion).
    void MeasureTextCss(float cssPx, const char* text, float* outW, float* outH);
    void PushClipRect(float x0, float y0, float x1, float y1, bool intersectWithCurrent = true);
    void PopClipRect();

    /** @brief Inline image widget (advances layout), e.g. a texture/atlas preview. */
    void Image(uint32_t texture, float w, float h, float u0 = 0.0f, float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f);

    // =========================================================================
    // Generic toolkit: queries, containers, popups, windows, styling, input.
    // These mirror the corresponding ImGui calls so tools can be built entirely
    // through EditorUI. Stable, append-only.
    // =========================================================================

    /** @brief Editor style color slots (mapped internally to the theme). */
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

    /** @brief Tree node flags (bitwise-or). */
    enum TreeFlags
    {
        TreeNone = 0,
        TreeDefaultOpen = 1 << 0,
        TreeLeaf = 1 << 1,
        TreeOpenOnArrow = 1 << 2,
        TreeSelected = 1 << 3
    };

    /** @brief Window flags (bitwise-or). */
    enum WinFlags
    {
        WinNone = 0,
        WinNoScrollbar = 1 << 0,
        WinAlwaysAutoResize = 1 << 1,
        WinNoSavedSettings = 1 << 2,
        WinNoCollapse = 1 << 3
    };

    /** @brief Table flags (bitwise-or). */
    enum TableFlags
    {
        TableNone = 0,
        TableResizable = 1 << 0,
        TableRowBg = 1 << 1,
        TableScrollY = 1 << 2,
        TableBordersInnerV = 1 << 3,
        TableBordersOuter = 1 << 4
    };
    /** @brief Table column flags (bitwise-or). */
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
    // Bounds of the last submitted item (screen space), e.g. to hang a control
    // off the right edge of a row that was just drawn.
    void GetItemRect(float* outMinX, float* outMinY, float* outMaxX, float* outMaxY);
    float GetFrameHeight();
    // Width the next framed control would take with the current item width /
    // indent settings - the basis for splitting a row between two controls.
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

    // --- Text / widgets not yet covered ---
    void TextWrapped(const char* text);
    bool SliderInt(const char* id, int* v, int vmin, int vmax);
    bool RadioButton(const char* label, bool active);
    bool Selectable(const char* label, bool selected);
    // Selectable that spans every column of the enclosing table (full-width row).
    bool Selectable(const char* label, bool selected, bool spanAllColumns);
    bool ImageButton(const char* id, uint32_t texture, float w, float h, float u0 = 0.0f, float v0 = 0.0f,
                     float u1 = 1.0f, float v1 = 1.0f);
    bool InvisibleButton(const char* id, float w, float h);
    void SetNextItemWidth(float w);
    // Virtualized list helper (one active clipper at a time; do not nest):
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

    // Layout group: treat a run of widgets as one item (for SameLine / sizing /
    // hit-testing the whole block). Pair BeginGroup with EndGroup.
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
    // `destructive` marks a cell whose verb removes something: it is drawn in
    // the palette's red instead of the neutral tier, with a red hover fill, so
    // a delete never looks like the buttons beside it.
    bool ToolbarIconButton(const char* id, const char* icon, const char* tooltip = nullptr, bool enabled = true,
                           bool active = false, bool destructive = false);
    void EndToolbar();

    // Sub-nav "tab links": a full-bleed row of text labels with a 2px accent
    // underline on the active one (dim -> bright on hover) and a bottom hairline.
    // Matches the Asset Browser's Assets/Scripts tabs. Pair BeginTabLinks with
    // EndTabLinks; draw each with TabLink (returns true when clicked).
    void BeginTabLinks();
    bool TabLink(const char* label, bool active);
    void EndTabLinks();

    // A richer list entry than Selectable: a full-width "card" with a bold title
    // and an optional dim subtitle, hover/selected backgrounds (rounded), and a
    // left accent bar when selected. Reserves its own height and advances layout,
    // so cards stack vertically. Returns true when clicked; query IsItemHovered()
    // / IsMouseDoubleClicked() right after for hover/double-click. subtitle may be
    // null for a single-line entry. subtitleColor (packed via Rgba()) tints the
    // subtitle; 0 = the default dim.
    bool SelectableCard(const char* id, const char* title, const char* subtitle, bool selected,
                        uint32_t subtitleColor = 0);

    // --- Popups / menus ---
    void OpenPopup(const char* id);
    bool BeginPopup(const char* id);
    bool BeginPopupModal(const char* name, bool* open = nullptr, int winFlags = 0);
    bool BeginPopupContextItem(const char* id = nullptr);
    // Open `id` when the last submitted item is clicked with `mouseButton`
    // (0 = left, 1 = right, 2 = middle). For context menus hung off a widget
    // that already consumes the click itself.
    void OpenPopupOnItemClick(const char* id, int mouseButton = 1);
    void EndPopup();
    void CloseCurrentPopup();
    bool MenuItem(const char* label, bool enabled = true);
    // Nested submenu inside an open popup/menu. Only call EndMenu() when
    // BeginMenu() returned true.
    bool BeginMenu(const char* label);
    void EndMenu();

    // --- Windows (standalone tool windows) ---
    bool Begin(const char* name, bool* open = nullptr, int winFlags = 0);
    void End();
    void SetNextWindowSize(float w, float h, bool firstUseEver = false);
    void SetNextWindowSizeConstraints(float minW, float minH, float maxW, float maxH);
    void SetNextWindowPos(float x, float y, float pivotX = 0.0f, float pivotY = 0.0f);

    // --- Styling ---
    /** @brief Vec2-valued style vars adjustable via PushStyleVar (balance with PopStyleVar). */
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
    // Push a Vec2 style var (e.g. table CellPadding, ItemSpacing); pop with PopStyleVar.
    void PushStyleVar(StyleVar var, float x, float y);
    void PopStyleVar(int count = 1);
    void TextColored(uint32_t rgba, const char* text);  // packed-color overload
    void PushBoldFont();                                // push the editor's bold UI font (balanced by PopFont)
    void PopFont();

    // --- Input state (replaces package GetIO() access) ---
    enum class Key
    {
        Z,
        Y,
        S,
        Delete
    };  // extend as tools need more named keys
    /** @brief Mouse cursor shapes for SetMouseCursor (e.g. splitter resize handles). */
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
    // Global DPI scale (FontScaleDpi). Custom-drawn geometry that should track
    // widget sizing multiplies by this (DrawTextCss already applies it to text).
    float GetDpiScale();
    bool IsKeyAlt();
    bool IsKeyPressed(Key key, bool repeat = true);
    float GetDeltaTime();
    bool IsWindowFocused(bool rootAndChildWindows = false);
    bool IsItemActive();
    bool IsItemDeactivatedAfterEdit();
    // Lost focus this frame, edited or not. The ...AfterEdit variant above stays
    // silent when the value was left alone, which is not enough to close an
    // inline edit field that the user clicked away from without typing.
    bool IsItemDeactivated();
    // Give the NEXT widget keyboard focus. For fields that appear already
    // focused (click-to-rename), where asking for a second click to type is the
    // difference between a rename and a chore.
    void SetKeyboardFocusHere();
    // InvisibleButton variant that accepts which mouse buttons activate it.
    bool InvisibleButton(const char* id, float w, float h, bool allowLeft, bool allowMiddle, bool allowRight = false);
    // Let items submitted LATER in the same window win the hit test against the
    // NEXT item, turning submission order into front-to-back hit testing. Without
    // it, a big hit surface (a canvas) claims the hovered id for the frame and
    // every widget drawn on top of it becomes unclickable. Call it immediately
    // before the surface, not before the widgets on top.
    void SetNextItemAllowOverlap();

    // Low-level styled controls (label-less; building blocks for the *Field
    // helpers and for custom rows). DragFloat2/3 wrap N drag fields + axis stamps.
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
    // Unity-style fields: label column + styled control as one call. Returns true
    // the frame the value changed. The field always records its own undo. Two
    // modes:
    //   - Pointer mode: pass T* - the field reads/writes it and its command
    //     restores/reapplies the bytes; callbacks add side-effects.
    //   - Value mode: pass the current value(s) by value - the field holds an
    //     internal copy and routes every change through the callbacks; its command
    //     stores old/new copies and calls onUndo(old)/onRedo(new). YOU apply the
    //     value in onChange/onUndo/onRedo (it never writes your storage).
    // The five callbacks are notifications/appliers, each carrying the value:
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

    // Physical unit for a float/vector field. The value is stored canonical
    // (meters / radians); the editor converts to the project's display unit
    // (px/m, degrees) and snaps Distance to whole pixels in Pixels mode. Mirrors
    // the engine's Deki::PhysicalUnit so the int passes straight through.
    enum class Unit
    {
        None = 0,
        Distance,
        Velocity,
        Acceleration,
        Angle
    };

    // --- Pointer mode (the field reads/writes your member) ---
    // Integer field: the width/signedness is chosen by the pointer overload.
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

    // --- Value mode (pointerless): pass the current value(s) by value; apply via
    // callbacks. Vector components are passed individually to avoid binding a
    // non-const array to the pointer overloads. `Unit::None` for no unit. ---
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

    // ImGui-backed implementations, installed once by the editor executable.
    // Byte-level edit callbacks the backend invokes during the lifecycle.
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
        // Undo lifecycle for drag-style *Field helpers, run after the field's
        // control. `value` is the current value bytes (the field's member in
        // pointer mode, an internal temp in value mode). `ownsStorage` true =>
        // the pushed command memcpy's `value` on undo/redo (pointer mode);
        // false => value mode, the command only fires cb.onUndo/onRedo.
        std::function<void(void* value, size_t size, bool ownsStorage, const void* before, const char* label,
                           const EditCallbacksBytes& cb)>
            recordEdit;
        // Same as recordEdit but for atomic widgets (combo): the DLL calls it only
        // on the frame the value changed.
        std::function<void(void* value, size_t size, bool ownsStorage, const void* before, const char* label,
                           const EditCallbacksBytes& cb)>
            commitEdit;
        // Unit-aware float/vector field: draws + converts (px/m, degrees) + snaps,
        // then runs the drag-style lifecycle. `value` is `count` canonical floats.
        std::function<bool(const char* label, float* value, int count, int unitKind, bool ownsStorage,
                           const EditCallbacksBytes& cb)>
            unitField;
        // CSS-px text (see DrawTextCss): backend applies Fonts::CssPx + DPI scale.
        std::function<void(float, float, float, uint32_t, const char*)> drawTextCss;
        std::function<void(float, const char*, float*, float*)> measureTextCss;
    };
    static void SetWidgetBackend(const WidgetBackend& backend);

    // Extra ImGui window flags for a window, by the name passed to Begin().
    // Installed by the editor executable, which uses it to make a maximized
    // panel non-resizable: the flag has to reach ImGui::Begin() itself, since
    // that is what decides whether the resize grips, border highlight and
    // resize cursor exist at all. Null (the default) adds nothing.
    static void SetExtraWindowFlagsProvider(std::function<int(const char*)> fn);

    // =========================================================================
    // Current Component (valid between BeginComponent/EndComponent)
    // =========================================================================

    Deki::Component* GetCurrentComponent() const { return m_CurrentComponent; }
    const Deki::ComponentMeta* GetCurrentMeta() const { return m_CurrentMeta; }

    // =========================================================================
    // Internal - Called by InspectorPanel
    // =========================================================================

    /**
     * @brief Callback types for property drawing
     */
    using DrawPropertyByNameFunc = std::function<void(const char*)>;
    using DrawPropertyByOffsetFunc = std::function<void(size_t)>;
    using DrawDefaultPropertiesFunc = std::function<void()>;
    using ResolveAssetNameFunc = std::function<std::string(const std::string&)>;
    using GetFontAtlasFunc = std::function<uint32_t(const std::string&, int, uint32_t*, uint32_t*)>;
    using ModifyAssetFunc =
        std::function<void(const std::string&, const std::string&, const std::string&, const std::string&)>;

    /**
     * @brief Begin editing a component (called by InspectorPanel)
     */
    void BeginComponent(Deki::Component* comp, const Deki::ComponentMeta* meta, DrawPropertyByNameFunc drawByName,
                        DrawPropertyByOffsetFunc drawByOffset, DrawDefaultPropertiesFunc drawDefault,
                        ResolveAssetNameFunc resolveAsset, GetFontAtlasFunc getFontAtlas,
                        ModifyAssetFunc modifyAsset = nullptr);

    /**
     * @brief End editing a component
     */
    void EndComponent();

private:
    EditorUI() = default;
    ~EditorUI() = default;

    // Prevent copying
    EditorUI(const EditorUI&) = delete;
    EditorUI& operator=(const EditorUI&) = delete;

    // Current state
    Deki::Component* m_CurrentComponent = nullptr;
    const Deki::ComponentMeta* m_CurrentMeta = nullptr;

    // Callbacks (set by InspectorPanel)
    DrawPropertyByNameFunc m_DrawPropertyByName;
    DrawPropertyByOffsetFunc m_DrawPropertyByOffset;
    DrawDefaultPropertiesFunc m_DrawDefaultProperties;
    ResolveAssetNameFunc m_ResolveAssetName;
    GetFontAtlasFunc m_GetFontAtlas;
    ModifyAssetFunc m_ModifyAsset;
};

}  // namespace DekiEditor

/**
 * @brief Helper macro to draw a property by member offset
 *
 * Usage: DRAW_PROPERTY_FIELD(SpriteComponent, width);
 */
#define DRAW_PROPERTY_FIELD(ComponentType, member)                                                                     \
    DekiEditor::EditorUI::Get().PropertyField(offsetof(ComponentType, member))
