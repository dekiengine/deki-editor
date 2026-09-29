#include "EngineEditorSetup.h"
#include "../PanelMaximize.h"

#include <nlohmann/json.hpp>
#include <deki/reflection/ComponentRegistry.h>
#include <deki-editor/EditorExtension.h>
#include <deki-editor/CustomEditor.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <deki-editor/EditorUI.h>
#include <deki/LogSystem.h>
#include "imgui.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/Motion.h>
#include "../panels/inspector/InspectorUtils.h"
#include <deki-editor/CommandHistory.h>
#include "../core/Commands.h"
#include "../core/Units.h"

#include <deki/reflection/Property.h>
#include <vector>
#include <cstring>
#include <memory>
#include <string>

// Engine provides functions to get all registered editor extensions
// This avoids hardcoded extern declarations for each editor type
extern "C"
{
    // Factory-based API (new)
    int DekiEngine_GetEditorFactoryCount();
    DekiEditor::EditorExtension* DekiEngine_GetEditorPrototype(int index);
    DekiEditor::EditorExtension* DekiEngine_CreateEditor(int index);

    // Legacy API (backward compat)
    int DekiEngine_GetEditorCount();
    DekiEditor::EditorExtension* DekiEngine_GetEditor(int index);
}

// All package DLLs (including deki-2d) are loaded dynamically by PackageLoader.

namespace DekiEditor
{

// Print registered components (auto-registration happens via DLL static initializers)
static void PrintRegisteredComponents()
{
    auto& registry = Deki::ComponentRegistry::Instance();

    // Debug: Print what was registered
    DEKI_LOG_EDITOR("ComponentRegistry: %zu components registered",
                    registry.GetAllComponents().size());
    for (const auto* meta : registry.GetAllComponents())
    {
        DEKI_LOG_EDITOR("  - %s (category: %s, abstract: %d)",
                        meta->name ? meta->name : "null",
                        meta->category ? meta->category : "none",
                        meta->isAbstract ? 1 : 0);
    }
}

// ----------------------------------------------------------------------------
// Console-style toolbar strip (shared by Console / Deploy Analysis / Platform
// Manager): a full-bleed row of cells with 1px dividers, hover/active fades,
// dim->bright text and a bottom hairline. Immediate-mode, one toolbar at a
// time, so the layout state is a single file-static updated between Begin/End.
// ----------------------------------------------------------------------------
namespace
{
struct ToolbarStrip
{
    ImVec2 rowMin{ 0.0f, 0.0f };
    ImVec2 rowMax{ 0.0f, 0.0f };
    float cursorX = 0.0f;
    float dpi = 1.0f;
};
ToolbarStrip g_toolbar;

// Draw one toolbar cell (icon glyph or text label). Returns true when clicked.
// `tooltip` may be null. Advances the strip cursor and draws the trailing divider.
bool DrawToolbarCell(const char* id, const char* text, const char* tooltip,
                     bool enabled, bool active, bool destructive = false)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dpi = g_toolbar.dpi;
    const float rowH = g_toolbar.rowMax.y - g_toolbar.rowMin.y;
    const ImVec2 textSz = ImGui::CalcTextSize(text);
    const float w = textSz.x + 22.0f * dpi;
    const float cx = g_toolbar.cursorX;

    ImGui::SetCursorScreenPos(ImVec2(cx, g_toolbar.rowMin.y));
    ImGui::PushID(id);
    bool clicked = false, hovered = false;
    if (enabled)
    {
        clicked = ImGui::InvisibleButton(id, ImVec2(w, rowH));
        hovered = ImGui::IsItemHovered();
        if (hovered && tooltip && tooltip[0]) DekiEditor::Tooltip("%s", tooltip);
    }
    else
    {
        ImGui::Dummy(ImVec2(w, rowH));
    }
    ImGui::PopID();

    const ImU32 colLine = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
    const ImU32 colDim = ImGui::ColorConvertFloat4ToU32(Palette::Dim);
    const ImU32 colDim2 = ImGui::ColorConvertFloat4ToU32(Palette::Dim2);
    const ImU32 colFg = ImGui::ColorConvertFloat4ToU32(Palette::Fg);
    const ImU32 colActive = IM_COL32(255, 255, 255, 10);
    const ImU32 colHover = IM_COL32(255, 255, 255, 6);

    // A cell whose verb removes something says so before it is pressed: red at
    // rest, full red and a red fill under the cursor. Same shape as its
    // neighbours - the color is the whole warning.
    const ImU32 colRed = ImGui::ColorConvertFloat4ToU32(Palette::Red);
    const ImU32 colRedDim = ImGui::ColorConvertFloat4ToU32(
        ImVec4(Palette::Red.x, Palette::Red.y, Palette::Red.z, 0.75f));
    const ImU32 colRedFill = ImGui::ColorConvertFloat4ToU32(
        ImVec4(Palette::Red.x, Palette::Red.y, Palette::Red.z, 0.14f));

    if (active)
        dl->AddRectFilled(ImVec2(cx, g_toolbar.rowMin.y), ImVec2(cx + w, g_toolbar.rowMax.y),
                          destructive ? colRedFill : colActive);
    else if (enabled && hovered)
        dl->AddRectFilled(ImVec2(cx, g_toolbar.rowMin.y), ImVec2(cx + w, g_toolbar.rowMax.y),
                          destructive ? colRedFill : colHover);

    const ImU32 fg = !enabled      ? colDim2
                     : destructive ? ((active || hovered) ? colRed : colRedDim)
                                   : ((active || hovered) ? colFg : colDim);
    const float tx = cx + (w - textSz.x) * 0.5f;
    const float ty = g_toolbar.rowMin.y + (rowH - textSz.y) * 0.5f;
    dl->AddText(ImVec2(tx, ty), fg, text);

    g_toolbar.cursorX += w;
    dl->AddLine(ImVec2(g_toolbar.cursorX, g_toolbar.rowMin.y),
                ImVec2(g_toolbar.cursorX, g_toolbar.rowMax.y), colLine, 1.0f);
    return clicked;
}

// Sub-nav "tab links" row (matches the Asset Browser's Assets/Scripts tabs):
// text labels with a 2px accent underline on the active one. Shares the strip
// state shape with the toolbar; only one row is built at a time.
ToolbarStrip g_tabLinks;

bool DrawTabLink(const char* label, bool active)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dpi = g_tabLinks.dpi;
    const float rowH = g_tabLinks.rowMax.y - g_tabLinks.rowMin.y;
    const float gapX = 18.0f * dpi;
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const float x0 = g_tabLinks.cursorX;
    const float x1 = x0 + textSize.x;

    ImGui::SetCursorScreenPos(ImVec2(x0, g_tabLinks.rowMin.y));
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton(label, ImVec2(textSize.x, rowH));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    const ImU32 colFg = ImGui::ColorConvertFloat4ToU32(Palette::Fg);
    const ImU32 colDim = ImGui::ColorConvertFloat4ToU32(Palette::Dim);
    const ImU32 colAccent = ImGui::ColorConvertFloat4ToU32(Palette::Accent);

    const ImU32 col = (active || hovered) ? colFg : colDim;
    const float textY = g_tabLinks.rowMin.y + (rowH - textSize.y) * 0.5f;
    dl->AddText(ImVec2(x0, textY), col, label);
    if (active)
        dl->AddRectFilled(ImVec2(x0, g_tabLinks.rowMax.y - 2.0f * dpi),
                          ImVec2(x1, g_tabLinks.rowMax.y), colAccent);

    g_tabLinks.cursorX = x1 + gapX;
    return clicked;
}

// A full-width two-line "card" list entry: bold title + optional dim subtitle,
// rounded hover/selected backgrounds, and a left accent bar when selected.
bool DrawSelectableCard(const char* id, const char* title, const char* subtitle, bool selected,
                        uint32_t subtitleColor)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const bool hasSub = subtitle && subtitle[0];

    const float padX = 10.0f * dpi;
    const float padY = 6.0f * dpi;
    const float gap = 2.0f * dpi;
    const float titleH = ImGui::GetTextLineHeight();
    const float subSz = ImGui::GetFontSize() * 0.88f;
    const float h = hasSub ? (padY * 2.0f + titleH + gap + subSz) : (padY * 2.0f + titleH);
    const float availW = ImGui::GetContentRegionAvail().x;

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(availW, h));
    bool hovered = ImGui::IsItemHovered();

    const ImVec2 pmin = p0;
    const ImVec2 pmax(p0.x + availW, p0.y + h);
    const float rounding = 4.0f * dpi;

    const ImU32 selBg = ImGui::ColorConvertFloat4ToU32(
        ImVec4(Palette::Accent.x, Palette::Accent.y, Palette::Accent.z, 0.12f));
    const ImU32 hovBg = IM_COL32(255, 255, 255, 8);
    // Selected and hover backgrounds fade; hover only while not selected.
    const float hoverT = Motion::ItemHover(hovered);
    const float selT = Motion::EaseOut(Motion::To(ImGui::GetItemID() ^ 0x5E1EC7u, selected ? 1.0f : 0.0f, Motion::Fast));
    if (selT > 0.0f)
        dl->AddRectFilled(pmin, pmax, Motion::WithAlpha(selBg, selT), rounding);
    if (hoverT * (1.0f - selT) > 0.0f)
        dl->AddRectFilled(pmin, pmax, Motion::WithAlpha(hovBg, hoverT * (1.0f - selT)), rounding);

    if (selected)
    {
        const ImU32 accent = ImGui::ColorConvertFloat4ToU32(Palette::Accent);
        dl->AddRectFilled(pmin, ImVec2(pmin.x + 2.5f * dpi, pmax.y), accent, rounding);
    }

    const ImU32 titleCol = ImGui::ColorConvertFloat4ToU32(Palette::Fg);
    const ImU32 subCol = subtitleColor ? (ImU32)subtitleColor
                                       : ImGui::ColorConvertFloat4ToU32(Palette::Dim);
    const float tx = pmin.x + padX;
    float ty = pmin.y + padY;
    dl->AddText(ImVec2(tx, ty), titleCol, title);
    if (hasSub)
    {
        ty += titleH + gap;
        dl->AddText(ImGui::GetFont(), subSz, ImVec2(tx, ty), subCol, subtitle);
    }
    return clicked;
}
}  // namespace

// Install the ImGui-backed implementations behind EditorUI's styled controls,
// so package custom-editors that call EditorUI::Get().X(...) render with the
// editor's widgets. Called once; the lambdas hold no state (they call the
// editor's free functions), so re-installing on hot reload is harmless.
static void InstallEditorUIBackend()
{
    EditorUI::WidgetBackend b;
    b.sectionHeader = [](const char* l)
    { return SchematicCollapsingHeader(l); };
    b.propertyRow = [](const char* l)
    { InspectorUtils::BeginPropertyRow(l); };
    b.dragInt = [](const char* id, int* v, float s, int mn, int mx)
    { return SchematicDragInt(id, v, s, mn, mx); };
    b.dragFloat = [](const char* id, float* v, float s, float mn, float mx, const char* fmt)
    { return SchematicDragFloat(id, v, s, mn, mx, fmt); };
    // Unified checkbox: a visible label draws the complete labeled row at the
    // SAME height as every other inspector field; a hidden/empty label ("##...")
    // draws just the bare styled box for custom inline layouts (e.g. the particle
    // per-emitter toggle), keeping its sibling alignment.
    b.checkbox = [](const char* label, bool* v) -> bool
    {
        const bool hasLabel = label && label[0] && !(label[0] == '#' && label[1] == '#');
        if (!hasLabel)
            return SchematicCheckbox(label && label[0] ? label : "##chk", v);

        // Keep the labeled checkbox at the full inspector field height so all rows
        // are uniform. We deliberately do NOT collapse FramePadding here: with the
        // ambient body FramePadding, GetFrameHeight matches the drag/string/combo
        // fields, and SchematicCheckbox centers its small box inside that frame,
        // leaving the unused vertical space as transparent padding above/below.
        ImGui::PushID(label);
        InspectorUtils::BeginPropertyRow(label);
        const bool changed = SchematicCheckbox("##chk", v);
        ImGui::PopID();
        return changed;
    };
    b.colorEdit4 = [](const char* id, float* c)
    { return SchematicColorEdit4(id, c); };
    b.colorEdit3 = [](const char* id, float* c)
    { return SchematicColorEdit3(id, c); };
    b.combo = [](const char* id, int* idx, const char* const* items, int count)
    { return SchematicCombo(id, idx, items, count); };
    b.comboStr = [](const char* id, int* idx, const char* items)
    { return SchematicCombo(id, idx, items); };
    b.beginCombo = [](const char* id, const char* preview)
    { return SchematicBeginCombo(id, preview); };
    b.endCombo = []()
    { SchematicEndCombo(); };
    b.sliderFloat = [](const char* id, float* v, float mn, float mx, const char* fmt)
    { return ImGui::SliderFloat(id, v, mn, mx, fmt); };
    b.sliderInt = [](const char* id, int* v, int mn, int mx)
    { return ImGui::SliderInt(id, v, mn, mx); };
    b.dragFloat2 = [](const char* id, float* v, float s, float mn, float mx, const char* fmt)
    { return SchematicDragFloat2(id, v, s, mn, mx, fmt); };
    b.dragFloat3 = [](const char* id, float* v, float s, float mn, float mx, const char* fmt)
    { return SchematicDragFloat3(id, v, s, mn, mx, fmt); };
    b.fullBleedSeparator = []()
    { FullBleedSeparator(); };
    b.beginDisabled = [](bool d)
    { ImGui::BeginDisabled(d); };
    b.endDisabled = []()
    { ImGui::EndDisabled(); };
    // Console-style toolbar strip.
    b.beginToolbar = []()
    {
        const float dpi = ImGui::GetStyle().FontScaleDpi;
        const float rowH = 34.0f * dpi;
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        g_toolbar.rowMin = rowMin;
        g_toolbar.rowMax = ImVec2(rowMin.x + avail, rowMin.y + rowH);
        g_toolbar.cursorX = rowMin.x;
        g_toolbar.dpi = dpi;
        // Leading divider at the strip start.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 colLine = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
        dl->AddLine(ImVec2(rowMin.x, rowMin.y), ImVec2(rowMin.x, g_toolbar.rowMax.y), colLine, 1.0f);
    };
    b.toolbarButton = [](const char* label, bool enabled, bool active)
    {
        return DrawToolbarCell(label, label, nullptr, enabled, active);
    };
    b.toolbarIconButton = [](const char* id, const char* icon, const char* tooltip,
                             bool enabled, bool active, bool destructive)
    {
        return DrawToolbarCell(id, icon, tooltip, enabled, active, destructive);
    };
    b.endToolbar = []()
    {
        // Reserve the row in layout flow and close the bottom edge with a hairline.
        //
        // The cursor lands one pixel BELOW that hairline, not on it. Everything
        // in the window is one draw list, so whatever is submitted next paints
        // over the strip's edge - a log row's background, a hovered row's fill
        // - and the boundary silently disappears exactly when the panel has
        // content near the top. A pixel of reserved space costs nothing and
        // keeps the strip bounded whatever follows it.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 colLine = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
        dl->AddLine(ImVec2(g_toolbar.rowMin.x, g_toolbar.rowMax.y),
                    ImVec2(g_toolbar.rowMax.x, g_toolbar.rowMax.y), colLine, 1.0f);
        // Reserved as an ITEM, not a bare cursor move: ImGui reports a window
        // whose last act was moving the cursor ("submit an item e.g. Dummy()"),
        // and a panel whose toolbar is the last thing it draws would trip that
        // every frame. Zero item spacing around it so the pixel is all it costs.
        ImGui::SetCursorScreenPos(ImVec2(g_toolbar.rowMin.x, g_toolbar.rowMax.y));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PopStyleVar();
    };
    b.selectableCard = [](const char* id, const char* title, const char* subtitle, bool selected, uint32_t subtitleColor)
    {
        return DrawSelectableCard(id, title, subtitle, selected, subtitleColor);
    };
    // Sub-nav tab links (Asset Browser style).
    b.beginTabLinks = []()
    {
        const float dpi = ImGui::GetStyle().FontScaleDpi;
        const float rowH = 34.0f * dpi;
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        g_tabLinks.rowMin = rowMin;
        g_tabLinks.rowMax = ImVec2(rowMin.x + avail, rowMin.y + rowH);
        g_tabLinks.cursorX = rowMin.x + 14.0f * dpi;  // left padding
        g_tabLinks.dpi = dpi;
    };
    b.tabLink = [](const char* label, bool active)
    { return DrawTabLink(label, active); };
    b.endTabLinks = []()
    {
        // Same one-pixel reservation as the toolbar above: the row's hairline
        // must survive whatever the panel draws under it.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 colLine = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
        dl->AddLine(ImVec2(g_tabLinks.rowMin.x, g_tabLinks.rowMax.y),
                    ImVec2(g_tabLinks.rowMax.x, g_tabLinks.rowMax.y), colLine, 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(g_tabLinks.rowMin.x, g_tabLinks.rowMax.y));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PopStyleVar();
    };
    using ECB = EditorUI::EditCallbacksBytes;
    // Drag-style lifecycle: onStartChange on activation, onChange on edited, and on
    // deactivate-after-edit fire onEndChange + push the undo command (carrying
    // onUndo/onRedo). ownsStorage=false (value mode) => command has a null target
    // and only replays via onUndo/onRedo; true (pointer mode) => it also memcpy's.
    b.recordEdit = [](void* value, size_t size, bool ownsStorage, const void* before,
                      const char* label, const ECB& cb)
    {
        static std::vector<uint8_t> s_old;
        if (ImGui::IsItemActivated())
        {
            s_old.assign(static_cast<const uint8_t*>(before),
                         static_cast<const uint8_t*>(before) + size);
            if (cb.onStartChange) cb.onStartChange(before);
        }
        if (ImGui::IsItemEdited() && cb.onChange)
            cb.onChange(value);
        if (ImGui::IsItemDeactivatedAfterEdit() && s_old.size() == size &&
            std::memcmp(s_old.data(), value, size) != 0)
        {
            std::vector<uint8_t> newBytes(static_cast<const uint8_t*>(value),
                                          static_cast<const uint8_t*>(value) + size);
            if (cb.onEndChange) cb.onEndChange(s_old.data(), newBytes.data());
            CommandHistory::Instance().ExecuteNoMerge(std::make_unique<ModifyValueCommand>(
                ownsStorage ? value : nullptr, size, s_old.data(), newBytes.data(),
                label ? label : "", cb.onUndo, cb.onRedo));
        }
    };
    // Atomic-widget lifecycle (combo): the DLL calls this only when changed.
    b.commitEdit = [](void* value, size_t size, bool ownsStorage, const void* before,
                      const char* label, const ECB& cb)
    {
        if (std::memcmp(before, value, size) == 0) return;
        std::vector<uint8_t> newBytes(static_cast<const uint8_t*>(value),
                                      static_cast<const uint8_t*>(value) + size);
        if (cb.onChange) cb.onChange(value);
        if (cb.onEndChange) cb.onEndChange(before, newBytes.data());
        CommandHistory::Instance().ExecuteNoMerge(std::make_unique<ModifyValueCommand>(
            ownsStorage ? value : nullptr, size, before, newBytes.data(),
            label ? label : "", cb.onUndo, cb.onRedo));
    };
    // Unit-aware float/vector field: canonical (meters/radians) -> display (px/deg)
    // with Distance pixel-snapping, plus the drag-style lifecycle above.
    b.unitField = [](const char* label, float* value, int count, int unitKind, bool ownsStorage,
                     const ECB& cb) -> bool
    {
        const Deki::PhysicalUnit unit = static_cast<Deki::PhysicalUnit>(unitKind);
        const UnitDisplay ud = GetUnitDisplay(unit);
        const bool snapPx = ShouldSnapToPixel(unit);

        float before[3] = { 0, 0, 0 };
        float disp[3] = { 0, 0, 0 };
        for (int i = 0; i < count; ++i)
        {
            before[i] = value[i];
            disp[i] = ToDisplay(value[i], ud);
        }

        InspectorUtils::BeginPropertyRow(label);
        const std::string id = std::string("##") + label;
        bool changed = false;
        if (count >= 3)
            changed = SchematicDragFloat3(id.c_str(), disp, ud.dragSpeed, 0.0f, 0.0f, ud.format);
        else if (count == 2)
            changed = SchematicDragFloat2(id.c_str(), disp, ud.dragSpeed, 0.0f, 0.0f, ud.format);
        else
            changed = SchematicDragFloat(id.c_str(), &disp[0], ud.dragSpeed, 0.0f, 0.0f, ud.format);

        for (int i = 0; i < count; ++i)
        {
            float edited = FromDisplay(disp[i], ud);
            if (snapPx) edited = SnapMetersToPixel(edited);
            value[i] = edited;
        }

        const size_t size = sizeof(float) * static_cast<size_t>(count);
        static std::vector<uint8_t> s_old;
        static int s_count = 0;
        if (ImGui::IsItemActivated())
        {
            s_old.assign(reinterpret_cast<const uint8_t*>(before),
                         reinterpret_cast<const uint8_t*>(before) + size);
            s_count = count;
            if (cb.onStartChange) cb.onStartChange(before);
        }
        if (ImGui::IsItemEdited() && cb.onChange)
            cb.onChange(value);
        if (ImGui::IsItemDeactivatedAfterEdit() && s_count == count && s_old.size() == size &&
            std::memcmp(s_old.data(), value, size) != 0)
        {
            std::vector<uint8_t> newBytes(reinterpret_cast<const uint8_t*>(value),
                                          reinterpret_cast<const uint8_t*>(value) + size);
            if (cb.onEndChange) cb.onEndChange(s_old.data(), newBytes.data());
            CommandHistory::Instance().ExecuteNoMerge(std::make_unique<ModifyValueCommand>(
                ownsStorage ? static_cast<void*>(value) : nullptr, size, s_old.data(), newBytes.data(),
                label ? label : "", cb.onUndo, cb.onRedo));
        }
        return changed;
    };
    b.inputInt = [](const char* id, int* v, int step, int fast)
    { return ImGui::InputInt(id, v, step, fast); };
    b.inputText = [](const char* id, char* buf, size_t sz)
    { return ImGui::InputText(id, buf, sz); };
    b.button = [](const char* label, float w, float h)
    { return ImGui::Button(label, ImVec2(w, h)); };
    b.smallButton = [](const char* label)
    { return ImGui::SmallButton(label); };
    b.text = [](const char* t)
    { ImGui::TextUnformatted(t); };
    b.textColored = [](float r, float g, float bl, float a, const char* t)
    { ImGui::TextColored(ImVec4(r, g, bl, a), "%s", t); };
    b.textDisabled = [](const char* t)
    { ImGui::TextDisabled("%s", t); };
    b.sameLine = [](float off, float sp)
    { ImGui::SameLine(off, sp); };
    b.spacing = []()
    { ImGui::Spacing(); };
    b.separator = []()
    { ImGui::Separator(); };
    b.pushIdInt = [](int id)
    { ImGui::PushID(id); };
    b.popId = []()
    { ImGui::PopID(); };
    b.indent = []()
    { ImGui::Indent(); };
    b.unindent = []()
    { ImGui::Unindent(); };
    b.bulletText = [](const char* t)
    { ImGui::BulletText("%s", t); };
    b.collapsingHeader = [](const char* label, bool* pVisible, bool defaultOpen)
    {
        return ImGui::CollapsingHeader(label, pVisible,
                                       defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    };
    // Custom drawing into the current window's draw list (tool grids, previews).
    b.cursorScreenPos = [](float* ox, float* oy)
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ox) *ox = p.x;
        if (oy) *oy = p.y;
    };
    b.setCursorScreenPos = [](float x, float y)
    { ImGui::SetCursorScreenPos(ImVec2(x, y)); };
    b.dummy = [](float w, float h)
    { ImGui::Dummy(ImVec2(w, h)); };
    b.drawLine = [](float x0, float y0, float x1, float y1, uint32_t c, float t)
    {
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), (ImU32)c, t);
    };
    b.drawRect = [](float x0, float y0, float x1, float y1, uint32_t c, float t, float r)
    {
        ImGui::GetWindowDrawList()->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), (ImU32)c, r, 0, t);
    };
    b.drawRectFilled = [](float x0, float y0, float x1, float y1, uint32_t c, float r)
    {
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), (ImU32)c, r);
    };
    b.drawCircle = [](float cx, float cy, float rad, uint32_t c, float t)
    {
        ImGui::GetWindowDrawList()->AddCircle(ImVec2(cx, cy), rad, (ImU32)c, 0, t);
    };
    b.drawCircleFilled = [](float cx, float cy, float rad, uint32_t c)
    {
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(cx, cy), rad, (ImU32)c);
    };
    b.drawImage = [](uint32_t tex, float x0, float y0, float x1, float y1,
                     float u0, float v0, float u1, float v1, uint32_t tint)
    {
        ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)tex, ImVec2(x0, y0), ImVec2(x1, y1),
                                             ImVec2(u0, v0), ImVec2(u1, v1), (ImU32)tint);
    };
    b.drawText = [](float px, float x, float y, uint32_t c, const char* s)
    {
        const float sz = px > 0.0f ? px : ImGui::GetFontSize();
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), sz, ImVec2(x, y), (ImU32)c, s);
    };
    b.drawTextCss = [](float cssPx, float x, float y, uint32_t c, const char* s)
    {
        // CSS px -> real font size (em-square correction) x DPI, matching how
        // panels size hand-drawn dl->AddText (see ConsolePanel / EditorTheme).
        ImFont* font = ImGui::GetFont();
        const float sz = Fonts::CssPx(font, cssPx) * ImGui::GetStyle().FontScaleDpi;
        ImGui::GetWindowDrawList()->AddText(font, sz, ImVec2(x, y), (ImU32)c, s);
    };
    b.measureTextCss = [](float cssPx, const char* s, float* w, float* h)
    {
        ImFont* font = ImGui::GetFont();
        const float sz = Fonts::CssPx(font, cssPx) * ImGui::GetStyle().FontScaleDpi;
        const ImVec2 v = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, s);
        if (w) *w = v.x;
        if (h) *h = v.y;
    };
    b.pushClipRect = [](float x0, float y0, float x1, float y1, bool intersect)
    {
        ImGui::GetWindowDrawList()->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), intersect);
    };
    b.popClipRect = []()
    { ImGui::GetWindowDrawList()->PopClipRect(); };
    EditorUI::SetWidgetBackend(b);

    // A maximized panel must not offer resize handles. EditorUI lives in
    // deki-editor.dll and cannot see the maximize state, which is the
    // executable's, so it asks through here.
    EditorUI::SetExtraWindowFlagsProvider([](const char* name) -> int
                                          { return MaximizedWindowFlags(name); });
}

void RegisterEngineEditors()
{
    // Install EditorUI's widget backend once so package custom-editors render
    // their fields with the editor's styled controls.
    static bool s_uiBackendInstalled = false;
    if (!s_uiBackendInstalled)
    {
        InstallEditorUIBackend();
        s_uiBackendInstalled = true;
    }

    // Register engine editor extensions by iterating through exported functions
    // This dispatches each editor to the appropriate registry based on its type
    auto& customEditorRegistry = DekiEditor::CustomEditorRegistry::Instance();
    auto& assetTypeEditorRegistry = DekiEditor::AssetTypeEditorRegistry::Instance();

    // Try factory-based API first (new system)
    int factoryCount = DekiEngine_GetEditorFactoryCount();
    if (factoryCount > 0)
    {
        for (int i = 0; i < factoryCount; ++i)
        {
            // Get prototype for metadata queries
            DekiEditor::EditorExtension* prototype = DekiEngine_GetEditorPrototype(i);
            if (!prototype)
                continue;

            switch (prototype->GetEditorType())
            {
                case DekiEditor::EditorType::CustomEditor:
                {
                    // Register a factory that creates fresh instances
                    const int factoryIndex = i;  // Capture index for lambda
                    customEditorRegistry.RegisterFactory(
                        prototype->GetName(),
                        [factoryIndex]() -> std::unique_ptr<DekiEditor::CustomEditor>
                        {
                            auto* rawPtr = DekiEngine_CreateEditor(factoryIndex);
                            if (rawPtr && rawPtr->GetEditorType() == DekiEditor::EditorType::CustomEditor)
                            {
                                return std::unique_ptr<DekiEditor::CustomEditor>(
                                    static_cast<DekiEditor::CustomEditor*>(rawPtr));
                            }
                            delete rawPtr;
                            return nullptr;
                        });
                    DEKI_LOG_EDITOR("Registered custom editor factory: %s", prototype->GetName());
                    break;
                }

                case DekiEditor::EditorType::AssetTypeEditor:
                {
                    // AssetTypeEditors remain singletons (they don't have per-component state)
                    auto* assetEditor = static_cast<DekiEditor::AssetTypeEditor*>(prototype);
                    assetTypeEditorRegistry.Register(assetEditor);
                    DEKI_LOG_EDITOR("Registered asset type editor: %s", prototype->GetName());

                    // Auto-create AssetTypeHandler for creation menu
                    DekiEditor::AssetTypeHandler handler;
                    handler.typeName = assetEditor->GetTypeName();
                    handler.displayName = assetEditor->GetDisplayName();
                    handler.extensions = assetEditor->GetExtensions();
                    handler.defaultContent = assetEditor->GetDefaultContent();

                    // Map compile target: 0=Texture, 1=Audio, 2=Data
                    int target = assetEditor->GetCompileTarget();
                    handler.compilesTo = (target == 0) ? DekiEditor::AssetCompileTarget::Texture : (target == 1) ? DekiEditor::AssetCompileTarget::Audio
                                                                                               : (target == 2)   ? DekiEditor::AssetCompileTarget::Data
                                                                                                                 : DekiEditor::AssetCompileTarget::None;
                    // Asset category is registered separately by each package's
                    // static initializer (see ImageCacheHandler.cpp pattern).
                    // We do not call assetEditor->GetCategory() because adding
                    // a virtual to AssetTypeEditor would break vtable ABI for
                    // already-loaded package DLLs.

                    // Wire compile callback to editor's Compile method
                    if (handler.compilesTo == DekiEditor::AssetCompileTarget::Texture)
                    {
                        handler.compile = [assetEditor](const nlohmann::json& data,
                                                        std::vector<uint8_t>& rgba,
                                                        int& width, int& height) -> bool
                        {
                            return assetEditor->Compile(data.dump(), rgba, width, height);
                        };
                    }
                    else if (handler.compilesTo == DekiEditor::AssetCompileTarget::Data)
                    {
                        handler.compileToFile = [assetEditor](const nlohmann::json& data,
                                                              const std::string& cachePath) -> bool
                        {
                            return assetEditor->CompileToFile(data.dump(), cachePath);
                        };
                    }

                    DekiEditor::AssetTypeRegistry::Instance().Register(handler);
                    break;
                }
                case DekiEditor::EditorType::CliTool:
                    // Found by name when a tool is called (McpTools.cpp), not
                    // registered here, so it goes with its package on unload.
                    break;
            }
        }
    }
    else
    {
        // Fall back to legacy singleton API
        int editorCount = DekiEngine_GetEditorCount();
        for (int i = 0; i < editorCount; ++i)
        {
            DekiEditor::EditorExtension* editor = DekiEngine_GetEditor(i);
            if (!editor)
                continue;

            switch (editor->GetEditorType())
            {
                case DekiEditor::EditorType::CustomEditor:
                    customEditorRegistry.Register(static_cast<DekiEditor::CustomEditor*>(editor));
                    DEKI_LOG_EDITOR("Registered custom editor (legacy): %s", editor->GetName());
                    break;

                case DekiEditor::EditorType::AssetTypeEditor:
                {
                    auto* assetEditor = static_cast<DekiEditor::AssetTypeEditor*>(editor);
                    assetTypeEditorRegistry.Register(assetEditor);
                    DEKI_LOG_EDITOR("Registered asset type editor: %s", editor->GetName());

                    // Auto-create AssetTypeHandler for creation menu
                    DekiEditor::AssetTypeHandler handler;
                    handler.typeName = assetEditor->GetTypeName();
                    handler.displayName = assetEditor->GetDisplayName();
                    handler.extensions = assetEditor->GetExtensions();
                    handler.defaultContent = assetEditor->GetDefaultContent();

                    // Map compile target: 0=Texture, 1=Audio, 2=Data
                    int target = assetEditor->GetCompileTarget();
                    handler.compilesTo = (target == 0) ? DekiEditor::AssetCompileTarget::Texture : (target == 1) ? DekiEditor::AssetCompileTarget::Audio
                                                                                               : (target == 2)   ? DekiEditor::AssetCompileTarget::Data
                                                                                                                 : DekiEditor::AssetCompileTarget::None;
                    // Asset category is registered separately by each package's
                    // static initializer (see ImageCacheHandler.cpp pattern).
                    // We do not call assetEditor->GetCategory() because adding
                    // a virtual to AssetTypeEditor would break vtable ABI for
                    // already-loaded package DLLs.

                    // Wire compile callback to editor's Compile method
                    if (handler.compilesTo == DekiEditor::AssetCompileTarget::Texture)
                    {
                        handler.compile = [assetEditor](const nlohmann::json& data,
                                                        std::vector<uint8_t>& rgba,
                                                        int& width, int& height) -> bool
                        {
                            return assetEditor->Compile(data.dump(), rgba, width, height);
                        };
                    }
                    else if (handler.compilesTo == DekiEditor::AssetCompileTarget::Data)
                    {
                        handler.compileToFile = [assetEditor](const nlohmann::json& data,
                                                              const std::string& cachePath) -> bool
                        {
                            return assetEditor->CompileToFile(data.dump(), cachePath);
                        };
                    }

                    DekiEditor::AssetTypeRegistry::Instance().Register(handler);
                    break;
                }
                case DekiEditor::EditorType::CliTool:
                    // Found by name when a tool is called (McpTools.cpp), not
                    // registered here, so it goes with its package on unload.
                    break;
            }
        }
    }

    // All package DLLs (including deki-2d) are loaded dynamically by PackageLoader.
    // Their REGISTER_EDITOR macros register into the shared EditorRegistry singleton
    // in deki-editor.dll, so they appear in the factory list queried above.

    // Register core components in the hierarchy "Create Object" menu.
    // Core components live in deki-engine-core (not a package DLL), so they can't
    // use REGISTER_CREATE_MENU_ITEM (which requires linking deki-editor at DLL load).
    auto& createMenu = DekiEditor::CreateMenuRegistry::Instance();
    createMenu.Register("Core", "Camera", "Camera", "CameraComponent");

    PrintRegisteredComponents();
}

}  // namespace DekiEditor
