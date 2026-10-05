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

// The engine lists every registered editor extension, so no editor type needs
// its own extern declaration here.
extern "C"
{
    int DekiEngineGetEditorFactoryCount();
    DekiEditor::EditorExtension* DekiEngineGetEditorPrototype(int index);
    DekiEditor::EditorExtension* DekiEngineCreateEditor(int index);
}

namespace DekiEditor
{

// Prints the registered components. They register themselves through DLL
// static initializers.
static void PrintRegisteredComponents()
{
    auto& registry = Deki::ComponentRegistry::Instance();

    DEKI_LOG_EDITOR("ComponentRegistry: %zu components registered", registry.GetAllComponents().size());
    for (const auto* meta : registry.GetAllComponents())
    {
        DEKI_LOG_DEBUG("  - %s (category: %s, abstract: %d)", meta->name ? meta->name : "null",
                       meta->category ? meta->category : "none", meta->isAbstract ? 1 : 0);
    }
}

// ----------------------------------------------------------------------------
// Console-style toolbar strip (shared by Console, Deploy Analysis and Platform
// Manager): a full-width row of cells with 1px dividers, hover and active
// fades, dim-to-bright text and a bottom hairline. Only one toolbar is built
// at a time, so the layout state is one file-static updated between Begin/End.
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
ToolbarStrip s_Toolbar;

// Draws one toolbar cell (icon glyph or text label). Returns true when clicked.
// `tooltip` may be null. Advances the strip cursor and draws the trailing divider.
bool DrawToolbarCell(const char* id, const char* text, const char* tooltip, bool enabled, bool active,
                     bool destructive = false)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dpi = s_Toolbar.dpi;
    const float rowH = s_Toolbar.rowMax.y - s_Toolbar.rowMin.y;
    const ImVec2 textSz = ImGui::CalcTextSize(text);
    const float w = textSz.x + 22.0f * dpi;
    const float cx = s_Toolbar.cursorX;

    ImGui::SetCursorScreenPos(ImVec2(cx, s_Toolbar.rowMin.y));
    ImGui::PushID(id);
    bool clicked = false, hovered = false;
    if (enabled)
    {
        clicked = ImGui::InvisibleButton(id, ImVec2(w, rowH));
        hovered = ImGui::IsItemHovered();
        if (hovered && tooltip && tooltip[0])
        {
            DekiEditor::Tooltip("%s", tooltip);
        }
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

    // A cell that removes something says so before it is pressed: red at rest,
    // full red with a red fill under the cursor. Same shape as its neighbours;
    // the colour is the whole warning.
    const ImU32 colRed = ImGui::ColorConvertFloat4ToU32(Palette::Red);
    const ImU32 colRedDim =
        ImGui::ColorConvertFloat4ToU32(ImVec4(Palette::Red.x, Palette::Red.y, Palette::Red.z, 0.75f));
    const ImU32 colRedFill =
        ImGui::ColorConvertFloat4ToU32(ImVec4(Palette::Red.x, Palette::Red.y, Palette::Red.z, 0.14f));

    if (active)
    {
        dl->AddRectFilled(ImVec2(cx, s_Toolbar.rowMin.y), ImVec2(cx + w, s_Toolbar.rowMax.y),
                          destructive ? colRedFill : colActive);
    }
    else if (enabled && hovered)
    {
        dl->AddRectFilled(ImVec2(cx, s_Toolbar.rowMin.y), ImVec2(cx + w, s_Toolbar.rowMax.y),
                          destructive ? colRedFill : colHover);
    }

    const ImU32 fg = !enabled      ? colDim2
                     : destructive ? ((active || hovered) ? colRed : colRedDim)
                                   : ((active || hovered) ? colFg : colDim);
    const float tx = cx + (w - textSz.x) * 0.5f;
    const float ty = s_Toolbar.rowMin.y + (rowH - textSz.y) * 0.5f;
    dl->AddText(ImVec2(tx, ty), fg, text);

    s_Toolbar.cursorX += w;
    dl->AddLine(ImVec2(s_Toolbar.cursorX, s_Toolbar.rowMin.y), ImVec2(s_Toolbar.cursorX, s_Toolbar.rowMax.y), colLine,
                1.0f);
    return clicked;
}

// Sub-nav "tab links" row (matches the Asset Browser's Assets/Scripts tabs):
// text labels with a 2px accent underline on the active one. Shares the strip
// state shape with the toolbar; only one row is built at a time.
ToolbarStrip s_TabLinks;

bool DrawTabLink(const char* label, bool active)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dpi = s_TabLinks.dpi;
    const float rowH = s_TabLinks.rowMax.y - s_TabLinks.rowMin.y;
    const float gapX = 18.0f * dpi;
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const float x0 = s_TabLinks.cursorX;
    const float x1 = x0 + textSize.x;

    ImGui::SetCursorScreenPos(ImVec2(x0, s_TabLinks.rowMin.y));
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton(label, ImVec2(textSize.x, rowH));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    const ImU32 colFg = ImGui::ColorConvertFloat4ToU32(Palette::Fg);
    const ImU32 colDim = ImGui::ColorConvertFloat4ToU32(Palette::Dim);
    const ImU32 colAccent = ImGui::ColorConvertFloat4ToU32(Palette::Accent);

    const ImU32 col = (active || hovered) ? colFg : colDim;
    const float textY = s_TabLinks.rowMin.y + (rowH - textSize.y) * 0.5f;
    dl->AddText(ImVec2(x0, textY), col, label);
    if (active)
    {
        dl->AddRectFilled(ImVec2(x0, s_TabLinks.rowMax.y - 2.0f * dpi), ImVec2(x1, s_TabLinks.rowMax.y), colAccent);
    }

    s_TabLinks.cursorX = x1 + gapX;
    return clicked;
}

// A full-width two-line "card" list entry: bold title + optional dim subtitle,
// rounded hover/selected backgrounds, and a left accent bar when selected.
bool DrawSelectableCard(const char* id, const char* title, const char* subtitle, bool selected, uint32_t subtitleColor)
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

    const ImU32 selBg =
        ImGui::ColorConvertFloat4ToU32(ImVec4(Palette::Accent.x, Palette::Accent.y, Palette::Accent.z, 0.12f));
    const ImU32 hovBg = IM_COL32(255, 255, 255, 8);
    // Selected and hover backgrounds fade; hover only while not selected.
    const float hoverT = Motion::ItemHover(hovered);
    const float selT =
        Motion::EaseOut(Motion::To(ImGui::GetItemID() ^ 0x5E1EC7u, selected ? 1.0f : 0.0f, Motion::kFast));
    if (selT > 0.0f)
    {
        dl->AddRectFilled(pmin, pmax, Motion::WithAlpha(selBg, selT), rounding);
    }
    if (hoverT * (1.0f - selT) > 0.0f)
    {
        dl->AddRectFilled(pmin, pmax, Motion::WithAlpha(hovBg, hoverT * (1.0f - selT)), rounding);
    }
    if (const float pressT = Motion::ItemPress(); pressT > 0.0f)
    {
        dl->AddRectFilled(pmin, pmax, Motion::PressShade(pressT), rounding);
    }

    if (selected)
    {
        const ImU32 accent = ImGui::ColorConvertFloat4ToU32(Palette::Accent);
        dl->AddRectFilled(pmin, ImVec2(pmin.x + 2.5f * dpi, pmax.y), accent, rounding);
    }

    const ImU32 titleCol = ImGui::ColorConvertFloat4ToU32(Palette::Fg);
    const ImU32 subCol = subtitleColor ? (ImU32)subtitleColor : ImGui::ColorConvertFloat4ToU32(Palette::Dim);
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

// Installs the ImGui implementations behind EditorUI's styled controls, so
// package custom editors that call EditorUI::Get().X(...) draw with the
// editor's widgets. The lambdas hold no state (they call the editor's free
// functions), so installing again on hot reload is harmless.
static void InstallEditorUIBackend()
{
    EditorUI::WidgetBackend b;
    b.sectionHeader = [](const char* l) { return SchematicCollapsingHeader(l); };
    b.propertyRow = [](const char* l) { InspectorUtils::BeginPropertyRow(l); };
    b.dragInt = [](const char* id, int* v, float s, int mn, int mx) { return SchematicDragInt(id, v, s, mn, mx); };
    b.dragFloat = [](const char* id, float* v, float s, float mn, float mx, const char* fmt)
    { return SchematicDragFloat(id, v, s, mn, mx, fmt); };
    // Checkbox: a visible label draws the full labeled row at the same height
    // as every other inspector field; a hidden label ("##...") draws just the
    // styled box, for custom inline layouts such as the particle per-emitter
    // toggle.
    b.checkbox = [](const char* label, bool* v) -> bool
    {
        const bool hasLabel = label && label[0] && !(label[0] == '#' && label[1] == '#');
        if (!hasLabel)
        {
            return SchematicCheckbox(label && label[0] ? label : "##chk", v);
        }

        // The labeled checkbox keeps the full inspector field height so all rows
        // match. FramePadding is left as it is on purpose: with it,
        // GetFrameHeight matches the drag, string and combo fields, and
        // SchematicCheckbox centres its small box inside that frame.
        ImGui::PushID(label);
        InspectorUtils::BeginPropertyRow(label);
        const bool changed = SchematicCheckbox("##chk", v);
        ImGui::PopID();
        return changed;
    };
    b.colorEdit4 = [](const char* id, float* c) { return SchematicColorEdit4(id, c); };
    b.colorEdit3 = [](const char* id, float* c) { return SchematicColorEdit3(id, c); };
    b.combo = [](const char* id, int* idx, const char* const* items, int count)
    { return SchematicCombo(id, idx, items, count); };
    b.comboStr = [](const char* id, int* idx, const char* items) { return SchematicCombo(id, idx, items); };
    b.beginCombo = [](const char* id, const char* preview) { return SchematicBeginCombo(id, preview); };
    b.endCombo = []() { SchematicEndCombo(); };
    b.sliderFloat = [](const char* id, float* v, float mn, float mx, const char* fmt)
    { return ImGui::SliderFloat(id, v, mn, mx, fmt); };
    b.sliderInt = [](const char* id, int* v, int mn, int mx) { return ImGui::SliderInt(id, v, mn, mx); };
    b.dragFloat2 = [](const char* id, float* v, float s, float mn, float mx, const char* fmt)
    { return SchematicDragFloat2(id, v, s, mn, mx, fmt); };
    b.dragFloat3 = [](const char* id, float* v, float s, float mn, float mx, const char* fmt)
    { return SchematicDragFloat3(id, v, s, mn, mx, fmt); };
    b.fullBleedSeparator = []() { FullBleedSeparator(); };
    b.beginDisabled = [](bool d) { ImGui::BeginDisabled(d); };
    b.endDisabled = []() { ImGui::EndDisabled(); };
    // Console-style toolbar strip.
    b.beginToolbar = []()
    {
        const float dpi = ImGui::GetStyle().FontScaleDpi;
        const float rowH = 34.0f * dpi;
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        s_Toolbar.rowMin = rowMin;
        s_Toolbar.rowMax = ImVec2(rowMin.x + avail, rowMin.y + rowH);
        s_Toolbar.cursorX = rowMin.x;
        s_Toolbar.dpi = dpi;
        // Leading divider at the strip start.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 colLine = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
        dl->AddLine(ImVec2(rowMin.x, rowMin.y), ImVec2(rowMin.x, s_Toolbar.rowMax.y), colLine, 1.0f);
    };
    b.toolbarButton = [](const char* label, bool enabled, bool active)
    { return DrawToolbarCell(label, label, nullptr, enabled, active); };
    b.toolbarIconButton =
        [](const char* id, const char* icon, const char* tooltip, bool enabled, bool active, bool destructive)
    { return DrawToolbarCell(id, icon, tooltip, enabled, active, destructive); };
    b.endToolbar = []()
    {
        // Reserve the row in the layout and close the bottom edge with a
        // hairline.
        //
        // The cursor lands one pixel below that hairline, not on it. The whole
        // window is one draw list, so whatever is drawn next (a log row's
        // background, a hovered row's fill) would paint over the strip's edge
        // and hide it. One reserved pixel keeps the edge visible.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 colLine = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
        dl->AddLine(ImVec2(s_Toolbar.rowMin.x, s_Toolbar.rowMax.y), ImVec2(s_Toolbar.rowMax.x, s_Toolbar.rowMax.y),
                    colLine, 1.0f);
        // Reserved as an item, not a bare cursor move: ImGui complains about a
        // window whose last act was moving the cursor ("submit an item e.g.
        // Dummy()"), and a panel whose toolbar is the last thing it draws would
        // trigger that every frame. No item spacing, so it costs only the pixel.
        ImGui::SetCursorScreenPos(ImVec2(s_Toolbar.rowMin.x, s_Toolbar.rowMax.y));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PopStyleVar();
    };
    b.selectableCard =
        [](const char* id, const char* title, const char* subtitle, bool selected, uint32_t subtitleColor)
    { return DrawSelectableCard(id, title, subtitle, selected, subtitleColor); };
    // Sub-nav tab links (Asset Browser style).
    b.beginTabLinks = []()
    {
        const float dpi = ImGui::GetStyle().FontScaleDpi;
        const float rowH = 34.0f * dpi;
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        s_TabLinks.rowMin = rowMin;
        s_TabLinks.rowMax = ImVec2(rowMin.x + avail, rowMin.y + rowH);
        s_TabLinks.cursorX = rowMin.x + 14.0f * dpi;  // left padding
        s_TabLinks.dpi = dpi;
    };
    b.tabLink = [](const char* label, bool active) { return DrawTabLink(label, active); };
    b.endTabLinks = []()
    {
        // Same one-pixel reservation as the toolbar above: the row's hairline
        // must survive whatever the panel draws under it.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 colLine = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
        dl->AddLine(ImVec2(s_TabLinks.rowMin.x, s_TabLinks.rowMax.y), ImVec2(s_TabLinks.rowMax.x, s_TabLinks.rowMax.y),
                    colLine, 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(s_TabLinks.rowMin.x, s_TabLinks.rowMax.y));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PopStyleVar();
    };
    using ECB = EditorUI::EditCallbacksBytes;
    // Drag-style lifecycle: onStartChange on activation, onChange on each edit,
    // and on deactivation after an edit, onEndChange plus an undo command
    // carrying onUndo/onRedo. With ownsStorage=false (value mode) the command
    // has a null target and replays only through onUndo/onRedo; with true
    // (pointer mode) it also copies the bytes.
    b.recordEdit = [](void* value, size_t size, bool ownsStorage, const void* before, const char* label, const ECB& cb)
    {
        static std::vector<uint8_t> s_Old;
        static bool s_Started = false;
        if (ImGui::IsItemActivated())
        {
            s_Old.assign(static_cast<const uint8_t*>(before), static_cast<const uint8_t*>(before) + size);
            if (cb.onStartChange)
            {
                cb.onStartChange(before);
            }
            s_Started = true;
        }
        if (ImGui::IsItemEdited() && cb.onChange)
        {
            cb.onChange(value);
        }
        if (ImGui::IsItemDeactivated() && s_Started)
        {
            // Every start gets its end, changed or not. A click that leaves the
            // value alone, or the first click of a double-click into text
            // entry, would otherwise leave the live edit open for good, and the
            // Play View would re-render every frame.
            s_Started = false;
            if (ImGui::IsItemDeactivatedAfterEdit() && s_Old.size() == size &&
                std::memcmp(s_Old.data(), value, size) != 0)
            {
                std::vector<uint8_t> newBytes(static_cast<const uint8_t*>(value),
                                              static_cast<const uint8_t*>(value) + size);
                if (cb.onEndChange)
                {
                    cb.onEndChange(s_Old.data(), newBytes.data());
                }
                CommandHistory::Instance().ExecuteNoMerge(
                    std::make_unique<ModifyValueCommand>(ownsStorage ? value : nullptr, size, s_Old.data(),
                                                         newBytes.data(), label ? label : "", cb.onUndo, cb.onRedo));
            }
            else if (cb.onEndChange && s_Old.size() == size)
            {
                cb.onEndChange(s_Old.data(), s_Old.data());
            }
        }
    };
    // Atomic-widget lifecycle (combo): the DLL calls this only when changed.
    b.commitEdit = [](void* value, size_t size, bool ownsStorage, const void* before, const char* label, const ECB& cb)
    {
        if (std::memcmp(before, value, size) == 0)
        {
            return;
        }
        std::vector<uint8_t> newBytes(static_cast<const uint8_t*>(value), static_cast<const uint8_t*>(value) + size);
        if (cb.onChange)
        {
            cb.onChange(value);
        }
        if (cb.onEndChange)
        {
            cb.onEndChange(before, newBytes.data());
        }
        CommandHistory::Instance().ExecuteNoMerge(std::make_unique<ModifyValueCommand>(
            ownsStorage ? value : nullptr, size, before, newBytes.data(), label ? label : "", cb.onUndo, cb.onRedo));
    };
    // Unit-aware float or vector field: stored units (meters, radians) shown as
    // display units (px, degrees), Distance snapped to pixels, with the
    // drag-style lifecycle above.
    b.unitField = [](const char* label, float* value, int count, int unitKind, bool ownsStorage, const ECB& cb) -> bool
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
        {
            changed = SchematicDragFloat3(id.c_str(), disp, ud.dragSpeed, 0.0f, 0.0f, ud.format);
        }
        else if (count == 2)
        {
            changed = SchematicDragFloat2(id.c_str(), disp, ud.dragSpeed, 0.0f, 0.0f, ud.format);
        }
        else
        {
            changed = SchematicDragFloat(id.c_str(), &disp[0], ud.dragSpeed, 0.0f, 0.0f, ud.format);
        }

        for (int i = 0; i < count; ++i)
        {
            float edited = FromDisplay(disp[i], ud);
            if (snapPx)
            {
                edited = SnapMetersToPixel(edited);
            }
            value[i] = edited;
        }

        const size_t size = sizeof(float) * static_cast<size_t>(count);
        static std::vector<uint8_t> s_Old;
        static int s_Count = 0;
        static bool s_Started = false;
        if (ImGui::IsItemActivated())
        {
            s_Old.assign(reinterpret_cast<const uint8_t*>(before), reinterpret_cast<const uint8_t*>(before) + size);
            s_Count = count;
            if (cb.onStartChange)
            {
                cb.onStartChange(before);
            }
            s_Started = true;
        }
        if (ImGui::IsItemEdited() && cb.onChange)
        {
            cb.onChange(value);
        }
        if (ImGui::IsItemDeactivated() && s_Started)
        {
            // Every start gets its end, changed or not (see recordEdit).
            s_Started = false;
            if (ImGui::IsItemDeactivatedAfterEdit() && s_Count == count && s_Old.size() == size &&
                std::memcmp(s_Old.data(), value, size) != 0)
            {
                std::vector<uint8_t> newBytes(reinterpret_cast<const uint8_t*>(value),
                                              reinterpret_cast<const uint8_t*>(value) + size);
                if (cb.onEndChange)
                {
                    cb.onEndChange(s_Old.data(), newBytes.data());
                }
                CommandHistory::Instance().ExecuteNoMerge(std::make_unique<ModifyValueCommand>(
                    ownsStorage ? static_cast<void*>(value) : nullptr, size, s_Old.data(), newBytes.data(),
                    label ? label : "", cb.onUndo, cb.onRedo));
            }
            else if (cb.onEndChange && s_Old.size() == size)
            {
                cb.onEndChange(s_Old.data(), s_Old.data());
            }
        }
        return changed;
    };
    b.inputInt = [](const char* id, int* v, int step, int fast) { return ImGui::InputInt(id, v, step, fast); };
    b.inputText = [](const char* id, char* buf, size_t sz) { return ImGui::InputText(id, buf, sz); };
    b.button = [](const char* label, float w, float h) { return ImGui::Button(label, ImVec2(w, h)); };
    b.smallButton = [](const char* label) { return ImGui::SmallButton(label); };
    b.text = [](const char* t) { ImGui::TextUnformatted(t); };
    b.textColored = [](float r, float g, float bl, float a, const char* t)
    { ImGui::TextColored(ImVec4(r, g, bl, a), "%s", t); };
    b.textDisabled = [](const char* t) { ImGui::TextDisabled("%s", t); };
    b.sameLine = [](float off, float sp) { ImGui::SameLine(off, sp); };
    b.spacing = []() { ImGui::Spacing(); };
    b.separator = []() { ImGui::Separator(); };
    b.pushIdInt = [](int id) { ImGui::PushID(id); };
    b.popId = []() { ImGui::PopID(); };
    b.indent = []() { ImGui::Indent(); };
    b.unindent = []() { ImGui::Unindent(); };
    b.bulletText = [](const char* t) { ImGui::BulletText("%s", t); };
    b.collapsingHeader = [](const char* label, bool* pVisible, bool defaultOpen)
    { return ImGui::CollapsingHeader(label, pVisible, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0); };
    // Custom drawing into the current window's draw list (tool grids, previews).
    b.cursorScreenPos = [](float* ox, float* oy)
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ox)
        {
            *ox = p.x;
        }
        if (oy)
        {
            *oy = p.y;
        }
    };
    b.setCursorScreenPos = [](float x, float y) { ImGui::SetCursorScreenPos(ImVec2(x, y)); };
    b.dummy = [](float w, float h) { ImGui::Dummy(ImVec2(w, h)); };
    b.drawLine = [](float x0, float y0, float x1, float y1, uint32_t c, float t)
    { ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), (ImU32)c, t); };
    b.drawRect = [](float x0, float y0, float x1, float y1, uint32_t c, float t, float r)
    { ImGui::GetWindowDrawList()->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), (ImU32)c, r, 0, t); };
    b.drawRectFilled = [](float x0, float y0, float x1, float y1, uint32_t c, float r)
    { ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), (ImU32)c, r); };
    b.drawCircle = [](float cx, float cy, float rad, uint32_t c, float t)
    { ImGui::GetWindowDrawList()->AddCircle(ImVec2(cx, cy), rad, (ImU32)c, 0, t); };
    b.drawCircleFilled = [](float cx, float cy, float rad, uint32_t c)
    { ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(cx, cy), rad, (ImU32)c); };
    b.drawImage =
        [](uint32_t tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t tint)
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
        if (w)
        {
            *w = v.x;
        }
        if (h)
        {
            *h = v.y;
        }
    };
    b.pushClipRect = [](float x0, float y0, float x1, float y1, bool intersect)
    { ImGui::GetWindowDrawList()->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), intersect); };
    b.popClipRect = []() { ImGui::GetWindowDrawList()->PopClipRect(); };
    EditorUI::SetWidgetBackend(b);

    // A maximized panel must not offer resize handles. EditorUI lives in
    // deki-editor.dll and cannot see the maximize state, which is the
    // executable's, so it asks through here.
    EditorUI::SetExtraWindowFlagsProvider([](const char* name) -> int { return MaximizedWindowFlags(name); });
}

void RegisterEngineEditors()
{
    // Install EditorUI's widget backend once, so package custom editors draw
    // their fields with the editor's styled controls.
    static bool s_UiBackendInstalled = false;
    if (!s_UiBackendInstalled)
    {
        InstallEditorUIBackend();
        s_UiBackendInstalled = true;
    }

    // Register every editor extension the engine lists, each in the registry
    // for its type.
    auto& customEditorRegistry = DekiEditor::CustomEditorRegistry::Instance();
    auto& assetTypeEditorRegistry = DekiEditor::AssetTypeEditorRegistry::Instance();

    const int factoryCount = DekiEngineGetEditorFactoryCount();
    for (int i = 0; i < factoryCount; ++i)
    {
        // The prototype answers the metadata queries.
        DekiEditor::EditorExtension* prototype = DekiEngineGetEditorPrototype(i);
        if (!prototype)
        {
            continue;
        }

        switch (prototype->GetEditorType())
        {
            case DekiEditor::EditorType::CustomEditor:
            {
                // A factory that creates a new instance each time.
                const int factoryIndex = i;
                customEditorRegistry.RegisterFactory(prototype->GetName(),
                                                     [factoryIndex]() -> std::unique_ptr<DekiEditor::CustomEditor>
                                                     {
                                                         auto* rawPtr = DekiEngineCreateEditor(factoryIndex);
                                                         if (rawPtr && rawPtr->GetEditorType() ==
                                                                           DekiEditor::EditorType::CustomEditor)
                                                         {
                                                             return std::unique_ptr<DekiEditor::CustomEditor>(
                                                                 static_cast<DekiEditor::CustomEditor*>(rawPtr));
                                                         }
                                                         delete rawPtr;
                                                         return nullptr;
                                                     });
                DEKI_LOG_DEBUG("Registered custom editor factory: %s", prototype->GetName());
                break;
            }

            case DekiEditor::EditorType::AssetTypeEditor:
            {
                // AssetTypeEditors stay singletons: they have no per-component state.
                auto* assetEditor = static_cast<DekiEditor::AssetTypeEditor*>(prototype);
                assetTypeEditorRegistry.Register(assetEditor);
                DEKI_LOG_DEBUG("Registered asset type editor: %s", prototype->GetName());

                // An AssetTypeHandler for the creation menu.
                DekiEditor::AssetTypeHandler handler;
                handler.typeName = assetEditor->GetTypeName();
                handler.displayName = assetEditor->GetDisplayName();
                handler.extensions = assetEditor->GetExtensions();
                handler.defaultContent = assetEditor->GetDefaultContent();

                // Map compile target: 0=Texture, 1=Audio, 2=Data
                int target = assetEditor->GetCompileTarget();
                handler.compilesTo = (target == 0)   ? DekiEditor::AssetCompileTarget::Texture
                                     : (target == 1) ? DekiEditor::AssetCompileTarget::Audio
                                     : (target == 2) ? DekiEditor::AssetCompileTarget::Data
                                                     : DekiEditor::AssetCompileTarget::None;
                // Each package's static initializer registers the asset
                // category (as in ImageCacheHandler.cpp). There is no
                // assetEditor->GetCategory(): a new virtual on
                // AssetTypeEditor would break the vtable ABI of package DLLs
                // already loaded.

                // The compile callback calls the editor's Compile method.
                if (handler.compilesTo == DekiEditor::AssetCompileTarget::Texture)
                {
                    handler.compile = [assetEditor](const nlohmann::json& data, std::vector<uint8_t>& rgba, int& width,
                                                    int& height) -> bool
                    { return assetEditor->Compile(data.dump(), rgba, width, height); };
                }
                else if (handler.compilesTo == DekiEditor::AssetCompileTarget::Data)
                {
                    handler.compileToFile = [assetEditor](const nlohmann::json& data,
                                                          const std::string& cachePath) -> bool
                    { return assetEditor->CompileToFile(data.dump(), cachePath); };
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

    // PackageLoader loads every package DLL (deki-2d included). Their
    // REGISTER_EDITOR macros register into the shared EditorRegistry in
    // deki-editor.dll, so they appear in the factory list read above.

    // Core components in the hierarchy's "Create Object" menu. They live in
    // deki-engine-core, not a package DLL, so they cannot use
    // REGISTER_CREATE_MENU_ITEM, which needs deki-editor linked at DLL load.
    auto& createMenu = DekiEditor::CreateMenuRegistry::Instance();
    createMenu.Register("Core", "Camera", "Camera", "CameraComponent");

    PrintRegisteredComponents();
}

}  // namespace DekiEditor
