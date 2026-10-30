#ifdef DEKI_EDITOR

#include <deki-editor/EditorTheme.h>  // DekiEditor::Tooltip (delayed)
#include <deki-editor/EditorTexture.h>
#include <vector>
#include <deki-editor/EditorUI.h>
#include "imgui.h"  // generic toolkit methods below call ImGui on the shared context

namespace DekiEditor
{

// ============================================================================
// Singleton Implementation
// ============================================================================

EditorUI& EditorUI::Get()
{
    static EditorUI s_Instance;
    return s_Instance;
}

// ============================================================================
// Property Drawing
// ============================================================================

void EditorUI::DrawDefaultInspector()
{
    if (m_DrawDefaultProperties)
    {
        m_DrawDefaultProperties();
    }
}

void EditorUI::PropertyField(size_t offset)
{
    if (m_DrawPropertyByOffset)
    {
        m_DrawPropertyByOffset(offset);
    }
}

void EditorUI::PropertyField(const char* propertyName)
{
    if (m_DrawPropertyByName)
    {
        m_DrawPropertyByName(propertyName);
    }
}

// ============================================================================
// Asset Utilities
// ============================================================================

std::string EditorUI::ResolveAssetName(const std::string& guid)
{
    if (m_ResolveAssetName)
    {
        return m_ResolveAssetName(guid);
    }
    return guid;
}

uint32_t EditorUI::GetFontAtlasTexture(const std::string& fontGuid, int fontSize, uint32_t* outWidth,
                                       uint32_t* outHeight)
{
    if (m_GetFontAtlas)
    {
        return m_GetFontAtlas(fontGuid, fontSize, outWidth, outHeight);
    }
    return 0;
}

void EditorUI::ModifyAsset(const std::string& assetPath, const std::string& assetGuid, const std::string& oldJson,
                           const std::string& newJson)
{
    if (m_ModifyAsset)
    {
        m_ModifyAsset(assetPath, assetGuid, oldJson, newJson);
    }
}

// ============================================================================
// State Management
// ============================================================================

void EditorUI::BeginComponent(Deki::Component* comp, const Deki::ComponentMeta* meta, DrawPropertyByNameFunc drawByName,
                              DrawPropertyByOffsetFunc drawByOffset, DrawDefaultPropertiesFunc drawDefault,
                              ResolveAssetNameFunc resolveAsset, GetFontAtlasFunc getFontAtlas,
                              ModifyAssetFunc modifyAsset)
{
    m_CurrentComponent = comp;
    m_CurrentMeta = meta;
    m_DrawPropertyByName = std::move(drawByName);
    m_DrawPropertyByOffset = std::move(drawByOffset);
    m_DrawDefaultProperties = std::move(drawDefault);
    m_ResolveAssetName = std::move(resolveAsset);
    m_GetFontAtlas = std::move(getFontAtlas);
    m_ModifyAsset = std::move(modifyAsset);
}

void EditorUI::EndComponent()
{
    m_CurrentComponent = nullptr;
    m_CurrentMeta = nullptr;
    m_DrawPropertyByName = nullptr;
    m_DrawPropertyByOffset = nullptr;
    m_DrawDefaultProperties = nullptr;
    m_ResolveAssetName = nullptr;
    m_GetFontAtlas = nullptr;
    m_ModifyAsset = nullptr;
}

// ============================================================================
// Styled UI controls - forward to the editor-installed backend
// ============================================================================

static EditorUI::WidgetBackend s_WidgetBackend;

void EditorUI::SetWidgetBackend(const WidgetBackend& backend)
{
    s_WidgetBackend = backend;
}

bool EditorUI::SectionHeader(const char* label)
{
    return s_WidgetBackend.sectionHeader ? s_WidgetBackend.sectionHeader(label) : false;
}

void EditorUI::PropertyRow(const char* label)
{
    if (s_WidgetBackend.propertyRow)
    {
        s_WidgetBackend.propertyRow(label);
    }
}

bool EditorUI::DragInt(const char* id, int* v, float speed, int vmin, int vmax)
{
    return s_WidgetBackend.dragInt ? s_WidgetBackend.dragInt(id, v, speed, vmin, vmax) : false;
}

bool EditorUI::DragFloat(const char* id, float* v, float speed, float vmin, float vmax, const char* format)
{
    return s_WidgetBackend.dragFloat ? s_WidgetBackend.dragFloat(id, v, speed, vmin, vmax, format) : false;
}

bool EditorUI::Checkbox(const char* id, bool* v)
{
    return s_WidgetBackend.checkbox ? s_WidgetBackend.checkbox(id, v) : false;
}

bool EditorUI::ColorEdit4(const char* id, float col[4])
{
    return s_WidgetBackend.colorEdit4 ? s_WidgetBackend.colorEdit4(id, col) : false;
}

bool EditorUI::ColorEdit3(const char* id, float col[3])
{
    return s_WidgetBackend.colorEdit3 ? s_WidgetBackend.colorEdit3(id, col) : false;
}

bool EditorUI::Combo(const char* id, int* currentIndex, const char* const items[], int count)
{
    return s_WidgetBackend.combo ? s_WidgetBackend.combo(id, currentIndex, items, count) : false;
}

bool EditorUI::Combo(const char* id, int* currentIndex, const char* itemsSeparatedByZeros)
{
    return s_WidgetBackend.comboStr ? s_WidgetBackend.comboStr(id, currentIndex, itemsSeparatedByZeros) : false;
}

bool EditorUI::SliderFloat(const char* id, float* v, float vmin, float vmax, const char* format)
{
    return s_WidgetBackend.sliderFloat ? s_WidgetBackend.sliderFloat(id, v, vmin, vmax, format) : false;
}

bool EditorUI::InputInt(const char* id, int* v, int step, int stepFast)
{
    return s_WidgetBackend.inputInt ? s_WidgetBackend.inputInt(id, v, step, stepFast) : false;
}

bool EditorUI::InputText(const char* id, char* buf, size_t bufSize)
{
    return s_WidgetBackend.inputText ? s_WidgetBackend.inputText(id, buf, bufSize) : false;
}

bool EditorUI::Button(const char* label, float width, float height)
{
    return s_WidgetBackend.button ? s_WidgetBackend.button(label, width, height) : false;
}

bool EditorUI::SmallButton(const char* label)
{
    return s_WidgetBackend.smallButton ? s_WidgetBackend.smallButton(label) : false;
}

void EditorUI::Text(const char* text)
{
    if (s_WidgetBackend.text)
    {
        s_WidgetBackend.text(text);
    }
}

void EditorUI::TextColored(float r, float g, float b, float a, const char* text)
{
    if (s_WidgetBackend.textColored)
    {
        s_WidgetBackend.textColored(r, g, b, a, text);
    }
}

void EditorUI::TextDisabled(const char* text)
{
    if (s_WidgetBackend.textDisabled)
    {
        s_WidgetBackend.textDisabled(text);
    }
}

void EditorUI::SameLine(float offsetFromStartX, float spacing)
{
    if (s_WidgetBackend.sameLine)
    {
        s_WidgetBackend.sameLine(offsetFromStartX, spacing);
    }
}

void EditorUI::Spacing()
{
    if (s_WidgetBackend.spacing)
    {
        s_WidgetBackend.spacing();
    }
}

void EditorUI::Separator()
{
    if (s_WidgetBackend.separator)
    {
        s_WidgetBackend.separator();
    }
}

void EditorUI::PushID(int id)
{
    if (s_WidgetBackend.pushIdInt)
    {
        s_WidgetBackend.pushIdInt(id);
    }
}

void EditorUI::PopID()
{
    if (s_WidgetBackend.popId)
    {
        s_WidgetBackend.popId();
    }
}

void EditorUI::Indent()
{
    if (s_WidgetBackend.indent)
    {
        s_WidgetBackend.indent();
    }
}

void EditorUI::Unindent()
{
    if (s_WidgetBackend.unindent)
    {
        s_WidgetBackend.unindent();
    }
}

void EditorUI::Indent(float amount)
{
    ImGui::Indent(amount);
}
void EditorUI::Unindent(float amount)
{
    ImGui::Unindent(amount);
}

void EditorUI::BulletText(const char* text)
{
    if (s_WidgetBackend.bulletText)
    {
        s_WidgetBackend.bulletText(text);
    }
}

bool EditorUI::CollapsingHeader(const char* label, bool* pVisible, bool defaultOpen)
{
    return s_WidgetBackend.collapsingHeader ? s_WidgetBackend.collapsingHeader(label, pVisible, defaultOpen) : false;
}

void EditorUI::GetCursorScreenPos(float* outX, float* outY)
{
    if (s_WidgetBackend.cursorScreenPos)
    {
        s_WidgetBackend.cursorScreenPos(outX, outY);
    }
    else
    {
        if (outX)
        {
            *outX = 0.0f;
        }
        if (outY)
        {
            *outY = 0.0f;
        }
    }
}

void EditorUI::SetCursorScreenPos(float x, float y)
{
    if (s_WidgetBackend.setCursorScreenPos)
    {
        s_WidgetBackend.setCursorScreenPos(x, y);
    }
}

void EditorUI::Dummy(float width, float height)
{
    if (s_WidgetBackend.dummy)
    {
        s_WidgetBackend.dummy(width, height);
    }
}

void EditorUI::DrawLine(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness)
{
    if (s_WidgetBackend.drawLine)
    {
        s_WidgetBackend.drawLine(x0, y0, x1, y1, rgba, thickness);
    }
}

void EditorUI::DrawRect(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness, float rounding)
{
    if (s_WidgetBackend.drawRect)
    {
        s_WidgetBackend.drawRect(x0, y0, x1, y1, rgba, thickness, rounding);
    }
}

void EditorUI::DrawRectFilled(float x0, float y0, float x1, float y1, uint32_t rgba, float rounding)
{
    if (s_WidgetBackend.drawRectFilled)
    {
        s_WidgetBackend.drawRectFilled(x0, y0, x1, y1, rgba, rounding);
    }
}

void EditorUI::DrawCircle(float cx, float cy, float radius, uint32_t rgba, float thickness)
{
    if (s_WidgetBackend.drawCircle)
    {
        s_WidgetBackend.drawCircle(cx, cy, radius, rgba, thickness);
    }
}

void EditorUI::DrawCircleFilled(float cx, float cy, float radius, uint32_t rgba)
{
    if (s_WidgetBackend.drawCircleFilled)
    {
        s_WidgetBackend.drawCircleFilled(cx, cy, radius, rgba);
    }
}

void EditorUI::DrawImage(uint32_t texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                         float v1, uint32_t tint)
{
    if (s_WidgetBackend.drawImage)
    {
        s_WidgetBackend.drawImage(texture, x0, y0, x1, y1, u0, v0, u1, v1, tint);
    }
}

void EditorUI::DrawTextAt(float pixelSize, float x, float y, uint32_t rgba, const char* text)
{
    if (s_WidgetBackend.drawText)
    {
        s_WidgetBackend.drawText(pixelSize, x, y, rgba, text);
    }
}

void EditorUI::DrawTextCss(float cssPx, float x, float y, uint32_t rgba, const char* text)
{
    if (s_WidgetBackend.drawTextCss)
    {
        s_WidgetBackend.drawTextCss(cssPx, x, y, rgba, text);
    }
}

void EditorUI::MeasureTextCss(float cssPx, const char* text, float* outW, float* outH)
{
    if (outW)
    {
        *outW = 0.0f;
    }
    if (outH)
    {
        *outH = 0.0f;
    }
    if (s_WidgetBackend.measureTextCss)
    {
        s_WidgetBackend.measureTextCss(cssPx, text, outW, outH);
    }
}

void EditorUI::PushClipRect(float x0, float y0, float x1, float y1, bool intersectWithCurrent)
{
    if (s_WidgetBackend.pushClipRect)
    {
        s_WidgetBackend.pushClipRect(x0, y0, x1, y1, intersectWithCurrent);
    }
}

void EditorUI::PopClipRect()
{
    if (s_WidgetBackend.popClipRect)
    {
        s_WidgetBackend.popClipRect();
    }
}

// ============================================================================
// Generic toolkit. These call ImGui directly: deki-editor.dll links ImGui and
// shares the editor's context (set via EditorToolRegistry), so calls here run
// on the same context as the rest of the frame.
// ============================================================================

static ImGuiCol ToImCol(EditorUI::Col c)
{
    switch (c)
    {
        case EditorUI::Col::Text: return ImGuiCol_Text;
        case EditorUI::Col::Button: return ImGuiCol_Button;
        case EditorUI::Col::ButtonHovered: return ImGuiCol_ButtonHovered;
        case EditorUI::Col::ButtonActive: return ImGuiCol_ButtonActive;
        case EditorUI::Col::FrameBg: return ImGuiCol_FrameBg;
        case EditorUI::Col::FrameBgHovered: return ImGuiCol_FrameBgHovered;
        case EditorUI::Col::FrameBgActive: return ImGuiCol_FrameBgActive;
        case EditorUI::Col::CheckMark: return ImGuiCol_CheckMark;
    }
    return ImGuiCol_Text;
}

// --- Queries / layout ---
bool EditorUI::IsItemHovered(bool allowWhenDisabled)
{
    return ImGui::IsItemHovered(allowWhenDisabled ? ImGuiHoveredFlags_AllowWhenDisabled : 0);
}
bool EditorUI::IsItemClicked()
{
    return ImGui::IsItemClicked();
}
void EditorUI::GetContentRegionAvail(float* w, float* h)
{
    ImVec2 a = ImGui::GetContentRegionAvail();
    if (w)
    {
        *w = a.x;
    }
    if (h)
    {
        *h = a.y;
    }
}
void EditorUI::GetContentRegionMax(float* x, float* y)
{
    ImVec2 a = ImGui::GetContentRegionMax();
    if (x)
    {
        *x = a.x;
    }
    if (y)
    {
        *y = a.y;
    }
}
void EditorUI::GetFramePadding(float* x, float* y)
{
    const ImGuiStyle& s = ImGui::GetStyle();
    if (x)
    {
        *x = s.FramePadding.x;
    }
    if (y)
    {
        *y = s.FramePadding.y;
    }
}
void EditorUI::GetItemSpacing(float* x, float* y)
{
    const ImGuiStyle& s = ImGui::GetStyle();
    if (x)
    {
        *x = s.ItemSpacing.x;
    }
    if (y)
    {
        *y = s.ItemSpacing.y;
    }
}
void EditorUI::GetItemInnerSpacing(float* x, float* y)
{
    const ImGuiStyle& s = ImGui::GetStyle();
    if (x)
    {
        *x = s.ItemInnerSpacing.x;
    }
    if (y)
    {
        *y = s.ItemInnerSpacing.y;
    }
}
void EditorUI::GetItemRect(float* minX, float* minY, float* maxX, float* maxY)
{
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    if (minX)
    {
        *minX = mn.x;
    }
    if (minY)
    {
        *minY = mn.y;
    }
    if (maxX)
    {
        *maxX = mx.x;
    }
    if (maxY)
    {
        *maxY = mx.y;
    }
}
float EditorUI::GetFrameHeight()
{
    return ImGui::GetFrameHeight();
}
float EditorUI::CalcItemWidth()
{
    return ImGui::CalcItemWidth();
}
float EditorUI::GetFontSize()
{
    return ImGui::GetFontSize();
}
void EditorUI::PushID(const void* ptr)
{
    ImGui::PushID(ptr);
}
void EditorUI::MeasureText(const char* text, float* w, float* h)
{
    ImVec2 s = ImGui::CalcTextSize(text);
    if (w)
    {
        *w = s.x;
    }
    if (h)
    {
        *h = s.y;
    }
}
void EditorUI::AlignTextToFramePadding()
{
    ImGui::AlignTextToFramePadding();
}
void EditorUI::SetTooltip(const char* text)
{
    DekiEditor::Tooltip("%s", text ? text : "");
}
float EditorUI::GetTextLineHeight()
{
    return ImGui::GetTextLineHeight();
}
float EditorUI::GetFrameHeightWithSpacing()
{
    return ImGui::GetFrameHeightWithSpacing();
}
void EditorUI::PushItemWidth(float w)
{
    ImGui::PushItemWidth(w);
}
void EditorUI::PopItemWidth()
{
    ImGui::PopItemWidth();
}
void EditorUI::GetMainViewportRect(float* x, float* y, float* w, float* h)
{
    const ImGuiViewport* v = ImGui::GetMainViewport();
    if (x)
    {
        *x = v->Pos.x;
    }
    if (y)
    {
        *y = v->Pos.y;
    }
    if (w)
    {
        *w = v->Size.x;
    }
    if (h)
    {
        *h = v->Size.y;
    }
}

// --- Text / widgets ---
void EditorUI::TextWrapped(const char* text)
{
    ImGui::TextWrapped("%s", text ? text : "");
}
bool EditorUI::SliderInt(const char* id, int* v, int mn, int mx)
{
    return ImGui::SliderInt(id, v, mn, mx);
}
bool EditorUI::RadioButton(const char* label, bool active)
{
    return ImGui::RadioButton(label, active);
}
bool EditorUI::Selectable(const char* label, bool selected)
{
    return ImGui::Selectable(label, selected);
}
bool EditorUI::Selectable(const char* label, bool selected, bool spanAllColumns)
{
    return ImGui::Selectable(label, selected, spanAllColumns ? ImGuiSelectableFlags_SpanAllColumns : 0);
}
bool EditorUI::ImageButton(const char* id, uint32_t tex, float w, float h, float u0, float v0, float u1, float v1)
{
    return ImGui::ImageButton(id, EditorTexture::ToImGui(tex), ImVec2(w, h), ImVec2(u0, v0), ImVec2(u1, v1));
}
bool EditorUI::InvisibleButton(const char* id, float w, float h)
{
    return ImGui::InvisibleButton(id, ImVec2(w, h));
}
void EditorUI::SetNextItemWidth(float w)
{
    ImGui::SetNextItemWidth(w);
}
static ImGuiListClipper s_ListClipper;
void EditorUI::BeginListClipper(int itemCount, float itemHeight)
{
    s_ListClipper.Begin(itemCount, itemHeight);
}
bool EditorUI::ListClipperStep(int* s, int* e)
{
    bool r = s_ListClipper.Step();
    if (s)
    {
        *s = s_ListClipper.DisplayStart;
    }
    if (e)
    {
        *e = s_ListClipper.DisplayEnd;
    }
    return r;
}
void EditorUI::EndListClipper()
{
    s_ListClipper.End();
}
bool EditorUI::InputTextWithHint(const char* id, const char* hint, char* buf, int bufSize)
{
    return ImGui::InputTextWithHint(id, hint, buf, static_cast<size_t>(bufSize));
}
bool EditorUI::BeginCombo(const char* label, const char* preview)
{
    return s_WidgetBackend.beginCombo ? s_WidgetBackend.beginCombo(label, preview) : ImGui::BeginCombo(label, preview);
}
void EditorUI::EndCombo()
{
    if (s_WidgetBackend.endCombo)
    {
        s_WidgetBackend.endCombo();
    }
    else
    {
        ImGui::EndCombo();
    }
}

// --- Containers ---
bool EditorUI::BeginChild(const char* id, float w, float h, bool border)
{
    return ImGui::BeginChild(id, ImVec2(w, h), border ? ImGuiChildFlags_Borders : 0);
}
void EditorUI::EndChild()
{
    ImGui::EndChild();
}
bool EditorUI::BeginTabBar(const char* id)
{
    return ImGui::BeginTabBar(id);
}
void EditorUI::EndTabBar()
{
    ImGui::EndTabBar();
}
bool EditorUI::BeginTabItem(const char* label)
{
    return ImGui::BeginTabItem(label);
}
void EditorUI::EndTabItem()
{
    ImGui::EndTabItem();
}
bool EditorUI::TreeNodeEx(const char* label, int f)
{
    int im = 0;
    if (f & TreeDefaultOpen)
    {
        im |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    if (f & TreeLeaf)
    {
        im |= ImGuiTreeNodeFlags_Leaf;
    }
    if (f & TreeOpenOnArrow)
    {
        im |= ImGuiTreeNodeFlags_OpenOnArrow;
    }
    if (f & TreeSelected)
    {
        im |= ImGuiTreeNodeFlags_Selected;
    }
    return ImGui::TreeNodeEx(label, im);
}
void EditorUI::TreePop()
{
    ImGui::TreePop();
}

// --- Layout groups ---
void EditorUI::BeginGroup()
{
    ImGui::BeginGroup();
}
void EditorUI::EndGroup()
{
    ImGui::EndGroup();
}

// --- Tables ---
bool EditorUI::BeginTable(const char* id, int columns, int f, float w, float h)
{
    int im = 0;
    if (f & TableResizable)
    {
        im |= ImGuiTableFlags_Resizable;
    }
    if (f & TableRowBg)
    {
        im |= ImGuiTableFlags_RowBg;
    }
    if (f & TableScrollY)
    {
        im |= ImGuiTableFlags_ScrollY;
    }
    if (f & TableBordersInnerV)
    {
        im |= ImGuiTableFlags_BordersInnerV;
    }
    if (f & TableBordersOuter)
    {
        im |= ImGuiTableFlags_BordersOuter;
    }
    return ImGui::BeginTable(id, columns, im, ImVec2(w, h));
}
void EditorUI::TableSetupColumn(const char* label, int f, float width)
{
    int im = 0;
    if (f & TableColumnWidthStretch)
    {
        im |= ImGuiTableColumnFlags_WidthStretch;
    }
    if (f & TableColumnWidthFixed)
    {
        im |= ImGuiTableColumnFlags_WidthFixed;
    }
    ImGui::TableSetupColumn(label, im, width);
}
void EditorUI::TableSetupScrollFreeze(int cols, int rows)
{
    ImGui::TableSetupScrollFreeze(cols, rows);
}
void EditorUI::TableHeadersRow()
{
    ImGui::TableHeadersRow();
}
void EditorUI::TableNextRow()
{
    ImGui::TableNextRow();
}
bool EditorUI::TableNextColumn()
{
    return ImGui::TableNextColumn();
}
void EditorUI::EndTable()
{
    ImGui::EndTable();
}

// --- Toolbar (backend-drawn: the strip chrome lives in the editor exe) ---
void EditorUI::BeginToolbar()
{
    if (s_WidgetBackend.beginToolbar)
    {
        s_WidgetBackend.beginToolbar();
    }
}
bool EditorUI::ToolbarButton(const char* label, bool enabled, bool active)
{
    return s_WidgetBackend.toolbarButton ? s_WidgetBackend.toolbarButton(label, enabled, active) : false;
}
bool EditorUI::ToolbarIconButton(const char* id, const char* icon, const char* tooltip, bool enabled, bool active,
                                 bool destructive)
{
    return s_WidgetBackend.toolbarIconButton
               ? s_WidgetBackend.toolbarIconButton(id, icon, tooltip, enabled, active, destructive)
               : false;
}
void EditorUI::EndToolbar()
{
    if (s_WidgetBackend.endToolbar)
    {
        s_WidgetBackend.endToolbar();
    }
}
void EditorUI::BeginTabLinks()
{
    if (s_WidgetBackend.beginTabLinks)
    {
        s_WidgetBackend.beginTabLinks();
    }
}
bool EditorUI::TabLink(const char* label, bool active)
{
    return s_WidgetBackend.tabLink ? s_WidgetBackend.tabLink(label, active) : false;
}
void EditorUI::EndTabLinks()
{
    if (s_WidgetBackend.endTabLinks)
    {
        s_WidgetBackend.endTabLinks();
    }
}
bool EditorUI::SelectableCard(const char* id, const char* title, const char* subtitle, bool selected,
                              uint32_t subtitleColor)
{
    return s_WidgetBackend.selectableCard ? s_WidgetBackend.selectableCard(id, title, subtitle, selected, subtitleColor)
                                          : false;
}

// --- Popups / menus ---
void EditorUI::OpenPopup(const char* id)
{
    ImGui::OpenPopup(id);
}
// Packages open their menus and dialogs through these, so the editor's style
// is applied here once rather than left to every caller: menus get the context
// menu look (PushContextMenuPopupStyle / PushContextMenuItemStyle), dialogs the
// padding of BeginSchematicModal. Window padding is baked when the popup
// begins, so it is popped straight after; a menu's row spacing stays pushed
// until EndPopup, which this stack remembers per open popup.
static std::vector<bool> s_PopupRowStyle;

static bool BeginStyledMenu(bool open)
{
    ImGui::PopStyleVar();  // WindowPadding, baked by the Begin just made
    if (open)
    {
        DekiEditor::PushContextMenuItemStyle();
        s_PopupRowStyle.push_back(true);
    }
    return open;
}

bool EditorUI::BeginPopup(const char* id)
{
    DekiEditor::PushContextMenuPopupStyle();
    return BeginStyledMenu(ImGui::BeginPopup(id));
}
bool EditorUI::BeginPopupModal(const char* name, bool* open, int f)
{
    int im = 0;
    if (f & WinNoScrollbar)
    {
        im |= ImGuiWindowFlags_NoScrollbar;
    }
    if (f & WinAlwaysAutoResize)
    {
        im |= ImGuiWindowFlags_AlwaysAutoResize;
    }
    if (f & WinNoSavedSettings)
    {
        im |= ImGuiWindowFlags_NoSavedSettings;
    }
    if (f & WinNoCollapse)
    {
        im |= ImGuiWindowFlags_NoCollapse;
    }
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * dpi, 16.0f * dpi));
    const bool shown = ImGui::BeginPopupModal(name, open, im);
    ImGui::PopStyleVar();
    if (shown)
    {
        s_PopupRowStyle.push_back(false);
    }
    return shown;
}
bool EditorUI::BeginPopupContextItem(const char* id)
{
    DekiEditor::PushContextMenuPopupStyle();
    return BeginStyledMenu(ImGui::BeginPopupContextItem(id));
}
void EditorUI::EndPopup()
{
    if (!s_PopupRowStyle.empty())
    {
        if (s_PopupRowStyle.back())
        {
            ImGui::PopStyleVar();  // ItemSpacing from PushContextMenuItemStyle
        }
        s_PopupRowStyle.pop_back();
    }
    ImGui::EndPopup();
}
void EditorUI::CloseCurrentPopup()
{
    ImGui::CloseCurrentPopup();
}
bool EditorUI::MenuItem(const char* label, bool enabled)
{
    return ImGui::MenuItem(label, nullptr, false, enabled);
}
bool EditorUI::BeginMenu(const char* label)
{
    return ImGui::BeginMenu(label);
}
void EditorUI::EndMenu()
{
    ImGui::EndMenu();
}
void EditorUI::OpenPopupOnItemClick(const char* id, int mouseButton)
{
    ImGuiPopupFlags flags = ImGuiPopupFlags_MouseButtonLeft;
    if (mouseButton == 1)
    {
        flags = ImGuiPopupFlags_MouseButtonRight;
    }
    else if (mouseButton == 2)
    {
        flags = ImGuiPopupFlags_MouseButtonMiddle;
    }
    ImGui::OpenPopupOnItemClick(id, flags);
}

// --- Windows ---
static std::function<int(const char*)> s_ExtraWindowFlags;
void EditorUI::SetExtraWindowFlagsProvider(std::function<int(const char*)> fn)
{
    s_ExtraWindowFlags = std::move(fn);
}

bool EditorUI::Begin(const char* name, bool* open, int f)
{
    int im = 0;
    if (f & WinNoScrollbar)
    {
        im |= ImGuiWindowFlags_NoScrollbar;
    }
    if (f & WinAlwaysAutoResize)
    {
        im |= ImGuiWindowFlags_AlwaysAutoResize;
    }
    if (f & WinNoSavedSettings)
    {
        im |= ImGuiWindowFlags_NoSavedSettings;
    }
    if (f & WinNoCollapse)
    {
        im |= ImGuiWindowFlags_NoCollapse;
    }
    // Whatever the editor wants to add for this window by name - NoResize while
    // it is the maximized panel. Every EditorUI window passes here, the panels
    // that use it and the package tool windows alike.
    if (s_ExtraWindowFlags)
    {
        im |= s_ExtraWindowFlags(name);
    }
    return ImGui::Begin(name, open, im);
}
void EditorUI::End()
{
    ImGui::End();
}
void EditorUI::SetNextWindowSize(float w, float h, bool firstUseEver)
{
    if (firstUseEver)
    {
        DekiEditor::FirstUseWindowPlacement(w, h);  // centred too, not at (60, 60)
    }
    else
    {
        ImGui::SetNextWindowSize(ImVec2(w, h), 0);
    }
}
void EditorUI::SetNextWindowSizeConstraints(float a, float b, float c, float d)
{
    ImGui::SetNextWindowSizeConstraints(ImVec2(a, b), ImVec2(c, d));
}
void EditorUI::SetNextWindowPos(float x, float y, float px, float py)
{
    ImGui::SetNextWindowPos(ImVec2(x, y), 0, ImVec2(px, py));
}

// --- Styling ---
void EditorUI::PushStyleColor(Col col, uint32_t rgba)
{
    ImGui::PushStyleColor(ToImCol(col), static_cast<ImU32>(rgba));
}
void EditorUI::PopStyleColor(int count)
{
    ImGui::PopStyleColor(count);
}
uint32_t EditorUI::GetStyleColor(Col col)
{
    return ImGui::GetColorU32(ToImCol(col));
}
void EditorUI::PushButtonTextAlign(float x, float y)
{
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(x, y));
}
void EditorUI::PushStyleVar(StyleVar var, float x, float y)
{
    ImGuiStyleVar v = ImGuiStyleVar_ItemSpacing;
    switch (var)
    {
        case StyleVar::ItemSpacing: v = ImGuiStyleVar_ItemSpacing; break;
        case StyleVar::ItemInnerSpacing: v = ImGuiStyleVar_ItemInnerSpacing; break;
        case StyleVar::FramePadding: v = ImGuiStyleVar_FramePadding; break;
        case StyleVar::CellPadding: v = ImGuiStyleVar_CellPadding; break;
        case StyleVar::WindowPadding: v = ImGuiStyleVar_WindowPadding; break;
    }
    ImGui::PushStyleVar(v, ImVec2(x, y));
}
void EditorUI::PopStyleVar(int count)
{
    ImGui::PopStyleVar(count);
}
void EditorUI::TextColored(uint32_t rgba, const char* text)
{
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(static_cast<ImU32>(rgba)), "%s", text ? text : "");
}
void EditorUI::PushBoldFont()
{
    ImGuiIO& io = ImGui::GetIO();
    ImFont* bold = (io.Fonts->Fonts.Size > 1) ? io.Fonts->Fonts[1] : ImGui::GetFont();
    ImGui::PushFont(bold);
}
void EditorUI::PopFont()
{
    ImGui::PopFont();
}
void EditorUI::Image(uint32_t tex, float w, float h, float u0, float v0, float u1, float v1)
{
    ImGui::Image(EditorTexture::ToImGui(tex), ImVec2(w, h), ImVec2(u0, v0), ImVec2(u1, v1));
}

// --- Input state ---
void EditorUI::GetMousePos(float* x, float* y)
{
    ImVec2 p = ImGui::GetMousePos();
    if (x)
    {
        *x = p.x;
    }
    if (y)
    {
        *y = p.y;
    }
}
void EditorUI::GetMouseDelta(float* x, float* y)
{
    ImVec2 d = ImGui::GetIO().MouseDelta;
    if (x)
    {
        *x = d.x;
    }
    if (y)
    {
        *y = d.y;
    }
}
bool EditorUI::IsMouseDown(int b)
{
    return ImGui::IsMouseDown(b);
}
bool EditorUI::IsMouseClicked(int b)
{
    return ImGui::IsMouseClicked(b);
}
bool EditorUI::IsMouseDoubleClicked(int b)
{
    return ImGui::IsMouseDoubleClicked(b);
}
bool EditorUI::IsMouseDragging(int b, float lock)
{
    return ImGui::IsMouseDragging(b, lock);
}
void EditorUI::SetMouseCursor(MouseCursor cursor)
{
    ImGuiMouseCursor c = ImGuiMouseCursor_Arrow;
    switch (cursor)
    {
        case MouseCursor::Arrow: c = ImGuiMouseCursor_Arrow; break;
        case MouseCursor::TextInput: c = ImGuiMouseCursor_TextInput; break;
        case MouseCursor::ResizeNS: c = ImGuiMouseCursor_ResizeNS; break;
        case MouseCursor::ResizeEW: c = ImGuiMouseCursor_ResizeEW; break;
        case MouseCursor::ResizeAll: c = ImGuiMouseCursor_ResizeAll; break;
        case MouseCursor::Hand: c = ImGuiMouseCursor_Hand; break;
        case MouseCursor::NotAllowed: c = ImGuiMouseCursor_NotAllowed; break;
    }
    ImGui::SetMouseCursor(c);
}
float EditorUI::GetMouseWheel()
{
    return ImGui::GetIO().MouseWheel;
}
bool EditorUI::IsKeyShift()
{
    return ImGui::GetIO().KeyShift;
}
bool EditorUI::IsKeyCtrl()
{
    return ImGui::GetIO().KeyCtrl;
}
float EditorUI::GetDpiScale()
{
    return ImGui::GetStyle().FontScaleDpi;
}
bool EditorUI::IsKeyAlt()
{
    return ImGui::GetIO().KeyAlt;
}
bool EditorUI::IsKeyPressed(Key key, bool repeat)
{
    ImGuiKey k = ImGuiKey_None;
    switch (key)
    {
        case Key::Z: k = ImGuiKey_Z; break;
        case Key::Y: k = ImGuiKey_Y; break;
        case Key::S: k = ImGuiKey_S; break;
        case Key::Delete: k = ImGuiKey_Delete; break;
    }
    return ImGui::IsKeyPressed(k, repeat);
}
float EditorUI::GetDeltaTime()
{
    return ImGui::GetIO().DeltaTime;
}
bool EditorUI::IsWindowFocused(bool rootAndChild)
{
    return ImGui::IsWindowFocused(rootAndChild ? ImGuiFocusedFlags_RootAndChildWindows : 0);
}
bool EditorUI::IsItemActive()
{
    return ImGui::IsItemActive();
}
bool EditorUI::IsItemDeactivatedAfterEdit()
{
    return ImGui::IsItemDeactivatedAfterEdit();
}
bool EditorUI::IsItemDeactivated()
{
    return ImGui::IsItemDeactivated();
}
void EditorUI::SetKeyboardFocusHere()
{
    ImGui::SetKeyboardFocusHere();
}
bool EditorUI::InvisibleButton(const char* id, float w, float h, bool l, bool m, bool r)
{
    int f = 0;
    if (l)
    {
        f |= ImGuiButtonFlags_MouseButtonLeft;
    }
    if (m)
    {
        f |= ImGuiButtonFlags_MouseButtonMiddle;
    }
    if (r)
    {
        f |= ImGuiButtonFlags_MouseButtonRight;
    }
    return ImGui::InvisibleButton(id, ImVec2(w, h), f);
}
void EditorUI::SetNextItemAllowOverlap()
{
    ImGui::SetNextItemAllowOverlap();
}

// ============================================================================
// Low-level styled controls (building blocks)
// ============================================================================

bool EditorUI::DragFloat2(const char* id, float v[2], float speed, float vmin, float vmax, const char* format)
{
    return s_WidgetBackend.dragFloat2 ? s_WidgetBackend.dragFloat2(id, v, speed, vmin, vmax, format) : false;
}

bool EditorUI::DragFloat3(const char* id, float v[3], float speed, float vmin, float vmax, const char* format)
{
    return s_WidgetBackend.dragFloat3 ? s_WidgetBackend.dragFloat3(id, v, speed, vmin, vmax, format) : false;
}

void EditorUI::FullBleedSeparator()
{
    if (s_WidgetBackend.fullBleedSeparator)
    {
        s_WidgetBackend.fullBleedSeparator();
    }
}

void EditorUI::BeginDisabled(bool disabled)
{
    if (s_WidgetBackend.beginDisabled)
    {
        s_WidgetBackend.beginDisabled(disabled);
    }
}

void EditorUI::EndDisabled()
{
    if (s_WidgetBackend.endDisabled)
    {
        s_WidgetBackend.endDisabled();
    }
}

// ============================================================================
// Unity-style fields: PropertyRow(label) + styled control + undo lifecycle.
// Each takes a per-shape callbacks struct (all five optional). Two modes:
//   pointer (field reads/writes *v, ownsStorage=true) and value (internal temp,
//   ownsStorage=false, the caller applies via callbacks).
// ============================================================================

namespace
{
using ECB = EditorUI::EditCallbacksBytes;

inline std::string FieldId(const char* label)
{
    return std::string("##") + label;
}

ECB AdaptFloat(const EditorUI::FloatCallbacks& cb)
{
    ECB b;
    if (cb.onStartChange)
    {
        b.onStartChange = [cb](const void* v) { cb.onStartChange(*static_cast<const float*>(v)); };
    }
    if (cb.onChange)
    {
        b.onChange = [cb](const void* v) { cb.onChange(*static_cast<const float*>(v)); };
    }
    if (cb.onUndo)
    {
        b.onUndo = [cb](const void* v) { cb.onUndo(*static_cast<const float*>(v)); };
    }
    if (cb.onRedo)
    {
        b.onRedo = [cb](const void* v) { cb.onRedo(*static_cast<const float*>(v)); };
    }
    if (cb.onEndChange)
    {
        b.onEndChange = [cb](const void* o, const void* n)
        { cb.onEndChange(*static_cast<const float*>(o), *static_cast<const float*>(n)); };
    }
    return b;
}
ECB AdaptVec(const EditorUI::VecCallbacks& cb)
{
    ECB b;
    if (cb.onStartChange)
    {
        b.onStartChange = [cb](const void* v) { cb.onStartChange(static_cast<const float*>(v)); };
    }
    if (cb.onChange)
    {
        b.onChange = [cb](const void* v) { cb.onChange(static_cast<const float*>(v)); };
    }
    if (cb.onUndo)
    {
        b.onUndo = [cb](const void* v) { cb.onUndo(static_cast<const float*>(v)); };
    }
    if (cb.onRedo)
    {
        b.onRedo = [cb](const void* v) { cb.onRedo(static_cast<const float*>(v)); };
    }
    if (cb.onEndChange)
    {
        b.onEndChange = [cb](const void* o, const void* n)
        { cb.onEndChange(static_cast<const float*>(o), static_cast<const float*>(n)); };
    }
    return b;
}
template <typename T>
ECB AdaptInt(const EditorUI::IntCallbacks& cb)
{
    ECB b;
    if (cb.onStartChange)
    {
        b.onStartChange = [cb](const void* v) { cb.onStartChange(static_cast<int64_t>(*static_cast<const T*>(v))); };
    }
    if (cb.onChange)
    {
        b.onChange = [cb](const void* v) { cb.onChange(static_cast<int64_t>(*static_cast<const T*>(v))); };
    }
    if (cb.onUndo)
    {
        b.onUndo = [cb](const void* v) { cb.onUndo(static_cast<int64_t>(*static_cast<const T*>(v))); };
    }
    if (cb.onRedo)
    {
        b.onRedo = [cb](const void* v) { cb.onRedo(static_cast<int64_t>(*static_cast<const T*>(v))); };
    }
    if (cb.onEndChange)
    {
        b.onEndChange = [cb](const void* o, const void* n)
        {
            cb.onEndChange(static_cast<int64_t>(*static_cast<const T*>(o)),
                           static_cast<int64_t>(*static_cast<const T*>(n)));
        };
    }
    return b;
}
ECB AdaptEnum(const EditorUI::EnumCallbacks& cb)
{
    ECB b;
    if (cb.onStartChange)
    {
        b.onStartChange = [cb](const void* v) { cb.onStartChange(*static_cast<const int*>(v)); };
    }
    if (cb.onChange)
    {
        b.onChange = [cb](const void* v) { cb.onChange(*static_cast<const int*>(v)); };
    }
    if (cb.onUndo)
    {
        b.onUndo = [cb](const void* v) { cb.onUndo(*static_cast<const int*>(v)); };
    }
    if (cb.onRedo)
    {
        b.onRedo = [cb](const void* v) { cb.onRedo(*static_cast<const int*>(v)); };
    }
    if (cb.onEndChange)
    {
        b.onEndChange = [cb](const void* o, const void* n)
        { cb.onEndChange(*static_cast<const int*>(o), *static_cast<const int*>(n)); };
    }
    return b;
}
ECB AdaptText(const EditorUI::TextCallbacks& cb)
{
    ECB b;
    if (cb.onStartChange)
    {
        b.onStartChange = [cb](const void* v) { cb.onStartChange(static_cast<const char*>(v)); };
    }
    if (cb.onChange)
    {
        b.onChange = [cb](const void* v) { cb.onChange(static_cast<const char*>(v)); };
    }
    if (cb.onUndo)
    {
        b.onUndo = [cb](const void* v) { cb.onUndo(static_cast<const char*>(v)); };
    }
    if (cb.onRedo)
    {
        b.onRedo = [cb](const void* v) { cb.onRedo(static_cast<const char*>(v)); };
    }
    if (cb.onEndChange)
    {
        b.onEndChange = [cb](const void* o, const void* n)
        { cb.onEndChange(static_cast<const char*>(o), static_cast<const char*>(n)); };
    }
    return b;
}

template <typename T>
bool IntFieldImpl(const char* label, T* v, float speed, int64_t vmin, int64_t vmax, const EditorUI::IntCallbacks& cb)
{
    const T before = *v;
    int tmp = static_cast<int>(*v);
    EditorUI& ui = EditorUI::Get();
    ui.PropertyRow(label);
    const bool changed =
        ui.DragInt(FieldId(label).c_str(), &tmp, speed, static_cast<int>(vmin), static_cast<int>(vmax));
    if (changed)
    {
        if (vmin != vmax)
        {
            if (tmp < static_cast<int>(vmin))
            {
                tmp = static_cast<int>(vmin);
            }
            if (tmp > static_cast<int>(vmax))
            {
                tmp = static_cast<int>(vmax);
            }
        }
        *v = static_cast<T>(tmp);
    }
    if (s_WidgetBackend.recordEdit)
    {
        s_WidgetBackend.recordEdit(v, sizeof(T), /*ownsStorage*/ true, &before, label, AdaptInt<T>(cb));
    }
    return changed;
}
}  // namespace

bool EditorUI::IntField(const char* label, int32_t* v, float s, int64_t mn, int64_t mx, const IntCallbacks& cb)
{
    return IntFieldImpl(label, v, s, mn, mx, cb);
}
bool EditorUI::IntField(const char* label, uint32_t* v, float s, int64_t mn, int64_t mx, const IntCallbacks& cb)
{
    return IntFieldImpl(label, v, s, mn, mx, cb);
}
bool EditorUI::IntField(const char* label, int16_t* v, float s, int64_t mn, int64_t mx, const IntCallbacks& cb)
{
    return IntFieldImpl(label, v, s, mn, mx, cb);
}
bool EditorUI::IntField(const char* label, uint16_t* v, float s, int64_t mn, int64_t mx, const IntCallbacks& cb)
{
    return IntFieldImpl(label, v, s, mn, mx, cb);
}
bool EditorUI::IntField(const char* label, int8_t* v, float s, int64_t mn, int64_t mx, const IntCallbacks& cb)
{
    return IntFieldImpl(label, v, s, mn, mx, cb);
}
bool EditorUI::IntField(const char* label, uint8_t* v, float s, int64_t mn, int64_t mx, const IntCallbacks& cb)
{
    return IntFieldImpl(label, v, s, mn, mx, cb);
}

bool EditorUI::FloatField(const char* label, float* v, float speed, float vmin, float vmax, const char* format,
                          const FloatCallbacks& cb)
{
    const float before = *v;
    PropertyRow(label);
    const bool changed = DragFloat(FieldId(label).c_str(), v, speed, vmin, vmax, format);
    if (s_WidgetBackend.recordEdit)
    {
        s_WidgetBackend.recordEdit(v, sizeof(float), true, &before, label, AdaptFloat(cb));
    }
    return changed;
}

bool EditorUI::Vector2Field(const char* label, float v[2], float speed, float vmin, float vmax, const char* format,
                            const VecCallbacks& cb)
{
    const float before[2] = { v[0], v[1] };
    PropertyRow(label);
    const bool changed = DragFloat2(FieldId(label).c_str(), v, speed, vmin, vmax, format);
    if (s_WidgetBackend.recordEdit)
    {
        s_WidgetBackend.recordEdit(v, sizeof(float) * 2, true, before, label, AdaptVec(cb));
    }
    return changed;
}

bool EditorUI::Vector3Field(const char* label, float v[3], float speed, float vmin, float vmax, const char* format,
                            const VecCallbacks& cb)
{
    const float before[3] = { v[0], v[1], v[2] };
    PropertyRow(label);
    const bool changed = DragFloat3(FieldId(label).c_str(), v, speed, vmin, vmax, format);
    if (s_WidgetBackend.recordEdit)
    {
        s_WidgetBackend.recordEdit(v, sizeof(float) * 3, true, before, label, AdaptVec(cb));
    }
    return changed;
}

// --- Unit-aware, pointer mode ---
bool EditorUI::FloatField(const char* label, float* v, Unit unit, const FloatCallbacks& cb)
{
    if (unit == Unit::None)
    {
        return FloatField(label, v, 1.0f, 0.0f, 0.0f, "%.3f", cb);
    }
    return s_WidgetBackend.unitField
               ? s_WidgetBackend.unitField(label, v, 1, static_cast<int>(unit), /*ownsStorage*/ true, AdaptFloat(cb))
               : false;
}
bool EditorUI::Vector2Field(const char* label, float v[2], Unit unit, const VecCallbacks& cb)
{
    if (unit == Unit::None)
    {
        return Vector2Field(label, v, 1.0f, 0.0f, 0.0f, "%.3f", cb);
    }
    return s_WidgetBackend.unitField
               ? s_WidgetBackend.unitField(label, v, 2, static_cast<int>(unit), true, AdaptVec(cb))
               : false;
}
bool EditorUI::Vector3Field(const char* label, float v[3], Unit unit, const VecCallbacks& cb)
{
    if (unit == Unit::None)
    {
        return Vector3Field(label, v, 1.0f, 0.0f, 0.0f, "%.3f", cb);
    }
    return s_WidgetBackend.unitField
               ? s_WidgetBackend.unitField(label, v, 3, static_cast<int>(unit), true, AdaptVec(cb))
               : false;
}

bool EditorUI::ColorField(const char* label, float v[4], const VecCallbacks& cb)
{
    const float before[4] = { v[0], v[1], v[2], v[3] };
    PropertyRow(label);
    const bool changed = ColorEdit4(FieldId(label).c_str(), v);
    if (s_WidgetBackend.recordEdit)
    {
        s_WidgetBackend.recordEdit(v, sizeof(float) * 4, true, before, label, AdaptVec(cb));
    }
    return changed;
}

bool EditorUI::ColorRGBField(const char* label, float v[3], const VecCallbacks& cb)
{
    const float before[3] = { v[0], v[1], v[2] };
    PropertyRow(label);
    const bool changed = ColorEdit3(FieldId(label).c_str(), v);
    if (s_WidgetBackend.recordEdit)
    {
        s_WidgetBackend.recordEdit(v, sizeof(float) * 3, true, before, label, AdaptVec(cb));
    }
    return changed;
}

bool EditorUI::EnumField(const char* label, int* index, const char* const items[], int count, const EnumCallbacks& cb)
{
    const int before = *index;
    PropertyRow(label);
    const bool changed = Combo(FieldId(label).c_str(), index, items, count);
    if (changed && s_WidgetBackend.commitEdit)
    {
        s_WidgetBackend.commitEdit(index, sizeof(int), true, &before, label, AdaptEnum(cb));
    }
    return changed;
}

bool EditorUI::EnumField(const char* label, int* index, const char* itemsZeroSep, const EnumCallbacks& cb)
{
    const int before = *index;
    PropertyRow(label);
    const bool changed = Combo(FieldId(label).c_str(), index, itemsZeroSep);
    if (changed && s_WidgetBackend.commitEdit)
    {
        s_WidgetBackend.commitEdit(index, sizeof(int), true, &before, label, AdaptEnum(cb));
    }
    return changed;
}

bool EditorUI::InputField(const char* label, char* buf, size_t bufSize, const TextCallbacks& cb)
{
    std::string before(buf);
    before.resize(bufSize, '\0');  // snapshot the whole fixed buffer
    PropertyRow(label);
    const bool changed = InputText(FieldId(label).c_str(), buf, bufSize);
    if (s_WidgetBackend.recordEdit)
    {
        s_WidgetBackend.recordEdit(buf, bufSize, true, before.data(), label, AdaptText(cb));
    }
    return changed;
}

bool EditorUI::SliderField(const char* label, float* v, float vmin, float vmax, const char* format,
                           const FloatCallbacks& cb)
{
    const float before = *v;
    PropertyRow(label);
    const bool changed = SliderFloat(FieldId(label).c_str(), v, vmin, vmax, format);
    if (s_WidgetBackend.recordEdit)
    {
        s_WidgetBackend.recordEdit(v, sizeof(float), true, &before, label, AdaptFloat(cb));
    }
    return changed;
}

bool EditorUI::SliderField(const char* label, int* v, int vmin, int vmax, const EnumCallbacks& cb)
{
    const int before = *v;
    PropertyRow(label);
    const bool changed =
        s_WidgetBackend.sliderInt ? s_WidgetBackend.sliderInt(FieldId(label).c_str(), v, vmin, vmax) : false;
    if (s_WidgetBackend.recordEdit)
    {
        s_WidgetBackend.recordEdit(v, sizeof(int), true, &before, label, AdaptEnum(cb));
    }
    return changed;
}

// --- Value mode (pointerless): internal temp, ownsStorage=false; apply via cb. ---
bool EditorUI::FloatField(const char* label, float value, Unit unit, const FloatCallbacks& cb)
{
    float temp = value;
    const float before = value;
    if (unit != Unit::None)
    {
        return s_WidgetBackend.unitField ? s_WidgetBackend.unitField(label, &temp, 1, static_cast<int>(unit),
                                                                     /*ownsStorage*/ false, AdaptFloat(cb))
                                         : false;
    }
    PropertyRow(label);
    const bool changed = DragFloat(FieldId(label).c_str(), &temp, 1.0f, 0.0f, 0.0f, "%.3f");
    if (s_WidgetBackend.recordEdit)
    {
        s_WidgetBackend.recordEdit(&temp, sizeof(float), false, &before, label, AdaptFloat(cb));
    }
    return changed;
}

bool EditorUI::Vector2Field(const char* label, float x, float y, Unit unit, const VecCallbacks& cb)
{
    float temp[2] = { x, y };
    const float before[2] = { x, y };
    if (unit != Unit::None)
    {
        return s_WidgetBackend.unitField
                   ? s_WidgetBackend.unitField(label, temp, 2, static_cast<int>(unit), false, AdaptVec(cb))
                   : false;
    }
    PropertyRow(label);
    const bool changed = DragFloat2(FieldId(label).c_str(), temp, 1.0f, 0.0f, 0.0f, "%.3f");
    if (s_WidgetBackend.recordEdit)
    {
        s_WidgetBackend.recordEdit(temp, sizeof(float) * 2, false, before, label, AdaptVec(cb));
    }
    return changed;
}

bool EditorUI::Vector3Field(const char* label, float x, float y, float z, Unit unit, const VecCallbacks& cb)
{
    float temp[3] = { x, y, z };
    const float before[3] = { x, y, z };
    if (unit != Unit::None)
    {
        return s_WidgetBackend.unitField
                   ? s_WidgetBackend.unitField(label, temp, 3, static_cast<int>(unit), false, AdaptVec(cb))
                   : false;
    }
    PropertyRow(label);
    const bool changed = DragFloat3(FieldId(label).c_str(), temp, 1.0f, 0.0f, 0.0f, "%.3f");
    if (s_WidgetBackend.recordEdit)
    {
        s_WidgetBackend.recordEdit(temp, sizeof(float) * 3, false, before, label, AdaptVec(cb));
    }
    return changed;
}

bool EditorUI::FloatField(const char* label, float value, float speed, float vmin, float vmax, const char* format,
                          const FloatCallbacks& cb)
{
    float temp = value;
    const float before = value;
    PropertyRow(label);
    const bool changed = DragFloat(FieldId(label).c_str(), &temp, speed, vmin, vmax, format);
    if (s_WidgetBackend.recordEdit)
    {
        s_WidgetBackend.recordEdit(&temp, sizeof(float), false, &before, label, AdaptFloat(cb));
    }
    return changed;
}

bool EditorUI::Vector2Field(const char* label, float x, float y, float speed, float vmin, float vmax,
                            const char* format, const VecCallbacks& cb)
{
    float temp[2] = { x, y };
    const float before[2] = { x, y };
    PropertyRow(label);
    const bool changed = DragFloat2(FieldId(label).c_str(), temp, speed, vmin, vmax, format);
    if (s_WidgetBackend.recordEdit)
    {
        s_WidgetBackend.recordEdit(temp, sizeof(float) * 2, false, before, label, AdaptVec(cb));
    }
    return changed;
}

bool EditorUI::Vector3Field(const char* label, float x, float y, float z, float speed, float vmin, float vmax,
                            const char* format, const VecCallbacks& cb)
{
    float temp[3] = { x, y, z };
    const float before[3] = { x, y, z };
    PropertyRow(label);
    const bool changed = DragFloat3(FieldId(label).c_str(), temp, speed, vmin, vmax, format);
    if (s_WidgetBackend.recordEdit)
    {
        s_WidgetBackend.recordEdit(temp, sizeof(float) * 3, false, before, label, AdaptVec(cb));
    }
    return changed;
}

}  // namespace DekiEditor

#endif  // DEKI_EDITOR
