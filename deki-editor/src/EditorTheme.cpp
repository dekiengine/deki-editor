// EditorTheme: the schematic / datasheet look.
// Near-black backgrounds, square 1px hairline borders, steel-blue accent.

#include <deki-editor/EditorTheme.h>
#include <deki-editor/Localization.h>
#include <deki-editor/Motion.h>
#include "imgui.h"
#include "imgui_internal.h"
#include <deki-editor/IconsTabler.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include <cmath>
#include <unordered_map>
#include <cstdarg>

namespace DekiEditor
{

// Per-font CSS-px → FreeType-REAL_DIM-px ratio, computed from each font's
// own TTF metrics at RegisterTtf() time.
static std::unordered_map<ImFont*, float> s_FontCssRatios;
// Fixed-width fonts only: character advance / unitsPerEm (0.6 for JetBrains
// Mono), so CssPx can pick a size whose advance is a whole pixel.
static std::unordered_map<ImFont*, float> s_FontMonoAdvance;

// ---------------------------------------------------------------------------
// Widget animation: the shared Motion helpers (deki-editor/Motion.h) hold the
// per-widget values and honour Reduce motion. A widget seen for the first time
// snaps to its target: a panel that has just opened shows its state instead
// of animating every foldout in it from shut.
// ---------------------------------------------------------------------------
namespace
{
// Defined below, next to the table it prunes.
void PruneSectionHeights();

float AnimateTo(ImGuiID id, float target, float seconds)
{
    // The section-height table outlives frames too; sweep it as often as
    // Motion sweeps its own.
    if ((ImGui::GetFrameCount() & 255) == 0)
    {
        PruneSectionHeights();
    }
    return Motion::To(id, target, seconds);
}

// Ease in-out, so the motion starts and stops softly.
float Smooth(float t)
{
    return Motion::Smooth(t);
}

// Rotates everything appended since `vtxStart` around its own center. ImGui
// cannot draw rotated text, so the glyph is drawn normally and its vertices
// are turned afterwards; the icon stays the same glyph at any angle.
void RotateVerticesFrom(ImDrawList* dl, int vtxStart, float rad)
{
    if (rad == 0.0f || dl->VtxBuffer.Size <= vtxStart)
    {
        return;
    }

    ImVec2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
    for (int i = vtxStart; i < dl->VtxBuffer.Size; ++i)
    {
        lo = ImMin(lo, dl->VtxBuffer[i].pos);
        hi = ImMax(hi, dl->VtxBuffer[i].pos);
    }
    const ImVec2 center((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);

    // IMGUI_DEFINE_MATH_OPERATORS is not set in this file, so the rotation is
    // written out by hand. ImRotate takes the cosine first, then the sine;
    // swapped, it rotates by (90° - angle) and turns the icon backwards.
    const float s = ImSin(rad), c = ImCos(rad);
    const ImVec2 rc = ImRotate(center, c, s);
    const float offX = rc.x - center.x;
    const float offY = rc.y - center.y;
    for (int i = vtxStart; i < dl->VtxBuffer.Size; ++i)
    {
        const ImVec2 p = ImRotate(dl->VtxBuffer[i].pos, c, s);
        dl->VtxBuffer[i].pos = ImVec2(p.x - offX, p.y - offY);
    }
}

// Moves everything appended since `vtxStart` so its bounding box starts on
// whole pixels. A glyph on a half pixel samples between texels and comes out
// soft and thick. Only for an icon at rest: snapping every frame of a turn
// makes it jitter by half a pixel.
void SnapVerticesFrom(ImDrawList* dl, int vtxStart)
{
    if (dl->VtxBuffer.Size <= vtxStart)
    {
        return;
    }

    ImVec2 lo(FLT_MAX, FLT_MAX);
    for (int i = vtxStart; i < dl->VtxBuffer.Size; ++i)
    {
        lo.x = ImMin(lo.x, dl->VtxBuffer[i].pos.x);
        lo.y = ImMin(lo.y, dl->VtxBuffer[i].pos.y);
    }
    const float dx = ImFloor(lo.x + 0.5f) - lo.x;
    const float dy = ImFloor(lo.y + 0.5f) - lo.y;
    if (dx == 0.0f && dy == 0.0f)
    {
        return;
    }
    for (int i = vtxStart; i < dl->VtxBuffer.Size; ++i)
    {
        dl->VtxBuffer[i].pos.x += dx;
        dl->VtxBuffer[i].pos.y += dy;
    }
}

// A chevron that turns instead of being swapped for a different glyph.
// `openT` 0 = closed (as authored), 1 = fully turned by `sweepRad`.
//
// `center` is the middle of the icon's zone, not a pen position: the glyph is
// centered on it by its own metrics, so it does not wander as it turns. The
// same glyph is used at every angle; swapping in chevron-down at the end
// would jump, since that glyph's ink sits elsewhere.
void DrawTurningChevron(ImDrawList* dl, ImFont* font, float size, ImVec2 center, ImU32 col, const char* glyph,
                        float openT, float sweepRad)
{
    const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, glyph);
    const ImVec2 pos(ImFloor(center.x - sz.x * 0.5f), ImFloor(center.y - sz.y * 0.5f));

    const int start = dl->VtxBuffer.Size;
    dl->AddText(font, size, pos, col, glyph);

    const bool atRest = (openT <= 0.0f || openT >= 1.0f);
    RotateVerticesFrom(dl, start, Smooth(openT) * sweepRad);
    if (atRest)
    {
        SnapVerticesFrom(dl, start);
    }
}

// ---- animated section bodies -------------------------------------------
//
// The body is clipped to an animated fraction of its full height, and the
// layout below is pulled up to the clipped height. Not a child window: a
// child has its own id scope and storage, so nested foldouts would read as
// shut during the animation and every id inside would change. Clipping in
// place keeps the same widgets, and ItemAdd treats anything outside the clip
// as culled, so the hidden part is not hoverable either.
struct SectionScope
{
    ImGuiID id = 0;
    bool active = false;      // a body was submitted and needs closing work
    bool clipped = false;     // false = at rest, submitted at full height
    bool treePushed = false;  // we pushed the tree indent/id ourselves
    float startX = 0.0f;
    float startY = 0.0f;
    float visibleH = 0.0f;
    float savedMaxY = 0.0f;
};
ImVector<SectionScope> s_SectionStack;

// Full body height per section, measured every frame the body is drawn, with
// the frame it was last measured so stale entries can be dropped.
struct SectionHeight
{
    float height = 0.0f;
    int frame = 0;
};
std::unordered_map<ImGuiID, SectionHeight> s_SectionHeights;

// A shut section is not re-measured, but its header is still on screen, so
// its height stays wanted: the next open animates from it.
void KeepSectionHeight(ImGuiID id)
{
    auto it = s_SectionHeights.find(id);
    if (it != s_SectionHeights.end())
    {
        it->second.frame = ImGui::GetFrameCount();
    }
}

// The body's animation is keyed apart from the chevron's: same widget, two
// motions, and they do not share a duration.
ImGuiID SectionBodyKey(ImGuiID headerId)
{
    return headerId ^ 0x5EC7B0D1u;
}

// The one definition of a section band's title type, shared by the drawn
// label and the field that edits it.
//
// This band names a thing, so it is set like the object name in the
// Inspector header: display face, heading size, accent. Uppercase
// letter-spaced type is for foldouts, which name sections.
ImFont* BandTitleFont()
{
    return g_FontDisplay ? g_FontDisplay : ImGui::GetFont();
}

// Two sizes for one type, because the two ImGui entry points disagree about
// what a size is: PushFont takes a base size and multiplies it by
// FontScaleDpi at draw time, while ImDrawList::AddText takes the final pixel
// size. Passing one where the other belongs is off by the DPI factor.
float BandTitleFontSizeBase()
{
    return Fonts::CssPx(BandTitleFont(), 16.0f);
}
float BandTitleFontSize()
{
    return BandTitleFontSizeBase() * ImGui::GetStyle().FontScaleDpi;
}

ImVec4 BandTitleColorVec()
{
    return Palette::Accent;
}
ImU32 BandTitleColor()
{
    return ImGui::ColorConvertFloat4ToU32(BandTitleColorVec());
}

// The air between a band and the first row under it. Part of the body, not
// of the band, so it disappears when the section does.
//
// A cursor advance, not an item: an item here would become the window's last
// one, and a caller that places something on the band from the previous
// item's rect (the `EDIT ->` link on a component header) would measure this
// spacer instead. The body's height is measured from the cursor, so the gap
// still counts. SchematicSectionEnd closes the body with a zero-size item, so
// an empty body cannot end a window on a bare cursor write.
void SectionBodyGap()
{
    const float gap = ImGui::GetStyle().ItemSpacing.y;
    if (gap <= 0.0f)
    {
        return;
    }
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y + gap));
}

// Measured heights outlive the frame, like the animations, and a big asset
// tree has many nodes, so heights not measured for a few seconds (a closed
// panel, a node scrolled away) are dropped. Called from AnimateTo.
void PruneSectionHeights()
{
    if (s_SectionHeights.size() <= 512)
    {
        return;
    }
    const int frame = ImGui::GetFrameCount();
    for (auto i = s_SectionHeights.begin(); i != s_SectionHeights.end();)
    {
        i = (frame - i->second.frame > 600) ? s_SectionHeights.erase(i) : ++i;
    }
}
}  // namespace

void FirstUseWindowPlacement(float width, float height)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(vp->GetWorkCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
}

void EmptyStateText(const char* text)
{
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float inset = 12.0f * dpi;
    ImGui::Dummy(ImVec2(0.0f, 6.0f * dpi));
    ImGui::Indent(inset);
    ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - inset);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Unindent(inset);
}

void Tooltip(const char* fmt, ...)
{
    ImGuiContext& g = *GImGui;
    // Either measure says "resting": the item timer for anything ImGui knows as
    // an item, the stationary timer for tooltips hung off hand-drawn regions
    // (canvas pins, toolbar cells) where there is no hovered id to time.
    if (g.HoveredIdTimer < kTooltipDelaySeconds && g.MouseStationaryTimer < kTooltipDelaySeconds)
    {
        return;
    }

    // The theme zeroes WindowPadding for panels, and a tooltip window takes
    // the current padding when it begins, so give it one here.
    const float pad = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad * 0.6f, pad * 0.4f));
    va_list args;
    va_start(args, fmt);
    ImGui::SetTooltipV(Tr(fmt), args);
    va_end(args);
    ImGui::PopStyleVar();
}

bool SchematicSectionBegin(const char* label, ImGuiTreeNodeFlags flags, ImGuiID* outHeaderId)
{
    if (outHeaderId)
    {
        *outHeaderId = 0;
    }

    const bool open = SchematicCollapsingHeader(label, flags);

    // Bail before touching any animation state: with SkipItems set the header
    // submitted no item, so GetItemID would name whatever came before it and
    // this would drive that widget's animation instead.
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems)
    {
        return false;
    }

    const ImGuiID id = ImGui::GetItemID();
    const ImGuiID key = SectionBodyKey(id);
    if (outHeaderId)
    {
        *outHeaderId = id;  // set even when the section is shut
    }

    const float t = AnimateTo(key, open ? 1.0f : 0.0f, 0.16f);
    KeepSectionHeight(id);
    if (t <= 0.0f)
    {
        return false;  // fully shut: nothing to submit, nothing to end
    }

    SectionScope sc;
    sc.id = id;
    sc.active = true;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    sc.startX = cursor.x;
    sc.startY = cursor.y;

    auto measured = s_SectionHeights.find(id);
    if (t >= 1.0f)
    {
        // At rest: submitted unclipped, and measured on the way out. Every
        // later open or close animates against that measurement.
        sc.clipped = false;
        SectionBodyGap();
        s_SectionStack.push_back(sc);
        return true;
    }

    // Opening for the first time, with no height on record: submit the body
    // clipped to nothing. Clipping culls drawing and hit-testing but not
    // layout, so End can still measure the full height. The section is
    // invisible for that one frame and animates from the next.
    sc.clipped = true;
    sc.visibleH = (measured == s_SectionHeights.end()) ? 0.0f : ImMax(0.0f, measured->second.height * Smooth(t));
    sc.savedMaxY = w->DC.CursorMaxPos.y;
    // Clip from the top of the band, not of the body: whatever a caller draws
    // on the band right after the header (the `EDIT ->` link) comes after this
    // point and must not be clipped during the animation. The band is already
    // painted, so including its row costs nothing.
    const float clipTop = ImMin(ImGui::GetItemRectMin().y, sc.startY);
    ImGui::PushClipRect(ImVec2(w->ClipRect.Min.x, clipTop), ImVec2(w->ClipRect.Max.x, sc.startY + sc.visibleH), true);
    SectionBodyGap();
    s_SectionStack.push_back(sc);
    return true;
}

bool SchematicTreeBodyBegin(bool open, ImGuiID nodeId)
{
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems || nodeId == 0)
    {
        s_SectionStack.push_back(SectionScope{});  // End is unconditional here
        return false;
    }

    const ImGuiID key = SectionBodyKey(nodeId);
    const float t = AnimateTo(key, open ? 1.0f : 0.0f, 0.14f);
    KeepSectionHeight(nodeId);

    SectionScope sc;
    sc.id = nodeId;
    if (t <= 0.0f)
    {
        s_SectionStack.push_back(sc);  // inactive marker; End pops it and stops
        return false;
    }
    sc.active = true;

    // Closing: ImGui pushed nothing this frame because to it the node is shut.
    // Push the indent and id it would have pushed, so the children still being
    // drawn stay where they were.
    if (!open)
    {
        ImGui::TreePushOverrideID(nodeId);
        sc.treePushed = true;
    }

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    sc.startX = cursor.x;
    sc.startY = cursor.y;

    auto measured = s_SectionHeights.find(nodeId);
    if (t >= 1.0f)
    {
        s_SectionStack.push_back(sc);
        return true;
    }

    // First expand of a node nobody has ever opened: invisible for one frame
    // while its children are measured, then animated. See SchematicSectionBegin.
    sc.clipped = true;
    sc.visibleH = (measured == s_SectionHeights.end()) ? 0.0f : ImMax(0.0f, measured->second.height * Smooth(t));
    sc.savedMaxY = w->DC.CursorMaxPos.y;
    ImGui::PushClipRect(ImVec2(w->ClipRect.Min.x, sc.startY), ImVec2(w->ClipRect.Max.x, sc.startY + sc.visibleH), true);
    s_SectionStack.push_back(sc);
    return true;
}

void SchematicTreeBodyEnd()
{
    IM_ASSERT(!s_SectionStack.empty() && "SchematicTreeBodyEnd without a Begin");
    if (s_SectionStack.empty())
    {
        return;
    }

    // Nothing was submitted (shut, or a skipped window): drop the marker only.
    if (!s_SectionStack.back().active)
    {
        s_SectionStack.pop_back();
        return;
    }

    const bool pushed = s_SectionStack.back().treePushed;
    SchematicSectionEnd();  // same clip / measure / compress
    if (pushed)
    {
        ImGui::TreePop();
    }
}

void SchematicSectionEnd()
{
    IM_ASSERT(!s_SectionStack.empty() && "SchematicSectionEnd without a Begin that returned true");
    if (s_SectionStack.empty())
    {
        return;
    }

    const SectionScope sc = s_SectionStack.back();
    s_SectionStack.pop_back();

    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w)
    {
        return;
    }

    // The body was submitted whole either way (a clip hides items, it does not
    // shorten the layout), so this is the full height in every state.
    // Recording it every frame keeps the animation right when the content
    // changes size.
    const float h = ImGui::GetCursorScreenPos().y - sc.startY;
    if (h > 0.0f)
    {
        s_SectionHeights[sc.id] = SectionHeight{ h, ImGui::GetFrameCount() };
    }

    if (!sc.clipped)
    {
        // Close on a real (zero-size) item: the body may have submitted nothing
        // at all after SectionBodyGap moved the cursor, and a window that ends
        // on a bare cursor write trips ImGui's boundary check at End().
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopStyleVar();
        return;
    }

    ImGui::PopClipRect();

    // Pull the layout up to the visible height. Closed with a zero-size item,
    // not a bare cursor write, which would trip ImGui's check at End() when
    // the section ends a window.
    ImGui::SetCursorScreenPos(ImVec2(sc.startX, sc.startY + sc.visibleH));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopStyleVar();

    // The body extended the window's content height to its full size, but
    // only the visible part counts; otherwise the scrollbar would jump by the
    // body's height every time a section is toggled.
    w->DC.CursorMaxPos.y = ImMax(sc.savedMaxY, sc.startY + sc.visibleH);
}

namespace Fonts
{
void RegisterTtf(ImFont* font, const char* ttfPath)
{
    if (!font || !ttfPath)
    {
        return;
    }
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0)
    {
        return;
    }
    FT_Face face = nullptr;
    if (FT_New_Face(lib, ttfPath, 0, &face) == 0)
    {
        // ratio = (ascender + |descender|) / unitsPerEm: the factor
        // FT_SIZE_REQUEST_TYPE_REAL_DIM divides the requested pixel_size by.
        // CSS px times this ratio is the pixel_size to ask FreeType for, so
        // the em-square matches CSS.
        const float upm = (float)face->units_per_EM;
        const float ascDesc = (float)(face->ascender - face->descender);
        if (upm > 0.0f)
        {
            s_FontCssRatios[font] = ascDesc / upm;
        }

        FT_Fixed advance = 0;
        const FT_UInt zero = FT_Get_Char_Index(face, '0');
        if (upm > 0.0f && FT_IS_FIXED_WIDTH(face) && zero != 0 &&
            FT_Get_Advance(face, zero, FT_LOAD_NO_SCALE, &advance) == 0 && advance > 0)
        {
            s_FontMonoAdvance[font] = (float)advance / upm;
        }
        FT_Done_Face(face);
    }
    FT_Done_FreeType(lib);
}

float CssPx(ImFont* font, float cssPx)
{
    // A fixed-width font at a size whose advance is not a whole pixel (12.5px
    // JetBrains Mono is 7.5px per character) has that advance rounded by
    // FreeType, which spreads the letters apart. Move to the nearest size
    // with a whole-pixel advance so the glyphs fill their cells.
    auto adv = s_FontMonoAdvance.find(font);
    if (adv != s_FontMonoAdvance.end())
    {
        const float cell = std::round(cssPx * adv->second);
        if (cell >= 1.0f)
        {
            cssPx = cell / adv->second;
        }
    }
    auto it = s_FontCssRatios.find(font);
    return cssPx * (it != s_FontCssRatios.end() ? it->second : 1.0f);
}
}  // namespace Fonts

// Unified right-click context-menu styling shared by every panel. See the header
// for the ordering contract (WindowPadding before Begin*, ItemSpacing after).
void PushContextMenuPopupStyle()
{
    const float menuFont = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(menuFont * 0.75f, menuFont * 0.6f));
}
void PushContextMenuItemStyle()
{
    const float menuFont = ImGui::GetFontSize();
    const float wpx = ImGui::GetStyle().WindowPadding.x;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(wpx * 2.0f, menuFont * 0.7f));
}

bool BeginSchematicModal(const char* title, float cssWidth)
{
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(cssWidth * dpi, 0.0f));  // 0 height = auto-fit
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * dpi, 16.0f * dpi));
    bool open = ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings);
    if (!open)
    {
        ImGui::PopStyleVar();
        return false;
    }
    // Escape closes it, like every other dialog, even a modal with no close
    // flag. It is reported as closed (false) at once, which is where dialogs
    // clear their own open flag, so a dialog that reopens itself while the
    // flag is set does not come straight back. A focused text field (New
    // Project's name) does not keep it open.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        ImGui::PopStyleVar();
        return false;
    }
    return true;
}

void EndSchematicModal()
{
    ImGui::EndPopup();
    ImGui::PopStyleVar();  // WindowPadding
}

int SchematicDialogButtons(std::initializer_list<DialogButtonSpec> buttons)
{
    const int count = static_cast<int>(buttons.size());
    if (count == 0)
    {
        return -1;
    }

    // A little air above the row, then the row across the full width.
    ImGui::Spacing();
    ImGui::Spacing();
    const float gap = 8.0f * ImGui::GetStyle().FontScaleDpi;
    const float width = (ImGui::GetContentRegionAvail().x - gap * static_cast<float>(count - 1)) / count;

    // The theme left-aligns button text for inspector value buttons; dialog
    // buttons are centred.
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    int clicked = -1;
    int i = 0;
    for (const DialogButtonSpec& b : buttons)
    {
        if (i > 0)
        {
            ImGui::SameLine(0.0f, gap);
        }
        int colours = 0;
        if (b.kind != DialogButton::Normal)
        {
            // Pushed colours skip PressMotion's easing, so these buttons ease
            // their own press: held since last frame -> toward the pressed colour.
            const ImGuiID id = ImGui::GetID(TrLabel(b.label));
            const bool held = ImGui::GetActiveID() == id;
            const float t =
                Motion::EaseOut(Motion::To(id ^ 0x9E55EDu, held ? 1.0f : 0.0f, held ? Motion::kFast : Motion::kNormal));
            ImVec4 rest, hovered, pressed;
            if (b.kind == DialogButton::Primary)
            {
                // Accent at rest, brighter on hover, dipping a little when pressed.
                const ImVec4 a = Palette::Accent;
                rest = Palette::AccentDim;
                hovered = a;
                pressed = ImVec4(a.x * 0.82f, a.y * 0.82f, a.z * 0.82f, a.w);
            }
            else
            {
                // Dark theme red at rest, brighter on hover and press.
                const ImVec4 r = Palette::Red;
                rest = ImVec4(r.x * 0.55f, r.y * 0.55f, r.z * 0.55f, 1.0f);
                hovered = ImVec4(r.x * 0.78f, r.y * 0.78f, r.z * 0.78f, 1.0f);
                pressed = r;
            }
            const ImVec4 now = Motion::Mix(hovered, pressed, t);
            ImGui::PushStyleColor(ImGuiCol_Button, rest);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t > 0.0f ? now : hovered);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, now);
            colours = 3;
        }
        ImGui::BeginDisabled(!b.enabled);
        if (ImGui::Button(TrLabel(b.label), ImVec2(width, 0.0f)))
        {
            clicked = i;
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(colours);
        ++i;
    }
    ImGui::PopStyleVar();
    return clicked;
}

// Schematic typography ladder - definitions. Loaded in main.cpp.
ImFont* g_FontUI = nullptr;
ImFont* g_FontUISmall = nullptr;
ImFont* g_FontUIMed = nullptr;
ImFont* g_FontUIBold = nullptr;
ImFont* g_FontMono = nullptr;
ImFont* g_FontMonoSmall = nullptr;
ImFont* g_FontMonoBold = nullptr;
ImFont* g_FontDisplay = nullptr;
ImFont* g_FontDisplaySmall = nullptr;
ImFont* g_FontLogo = nullptr;

void SetupEditorTheme(float dpiScale)
{
    // dpiScale is used only for the style.ScaleAllSizes() call below. Custom
    // widgets read ImGui::GetFontSize() / FontScaleDpi at draw time, as the
    // ImGui 1.92 FAQ advises ("express dimensions as multiples of reference
    // values").
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();

    // Corner rounding. Windows, docked children and tabs stay square: rounding
    // a docked window breaks the shared dock seams, and the tab bar's square
    // active-tab underline (DrawPanelTabUnderline) would poke past rounded
    // corners. Everything the user clicks into (buttons, input fields, drag
    // fields, combos, popups, scrollbars, grabs) gets a small radius. Logical
    // px; ScaleAllSizes() below multiplies it by dpiScale, and custom widgets
    // read the scaled value from g.Style.FrameRounding.
    const float kFrameRounding = 3.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = kFrameRounding;
    style.PopupRounding = kFrameRounding;
    style.ScrollbarRounding = kFrameRounding;
    style.GrabRounding = kFrameRounding;
    style.TabRounding = 0.0f;

    // 1px hairlines on frames and popups; tab borders are off because
    // panels draw their own active-tab underline.
    //
    // Docked panels have no window border (WindowBorderSize = 0). The line
    // between two docked panels is ImGui's dock separator: SplitterBehavior()
    // fills the gap with ImGuiCol_Separator, which the docking code sets to
    // ImGuiCol_Border. A panel border as well would triple the seam (border +
    // separator + border); without it every shared seam is one hairline.
    // DrawDockSeams() adds borders back for floating windows such as dialogs.
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;
    style.SeparatorTextBorderSize = 1.0f;
    // Dock separators: the seam line between docked panels. Resizing still works
    // because the splitter's grab area is widened by WindowBorderHoverPadding,
    // independent of the visual thickness.
    style.DockingSeparatorSize = 2.0f;

    style.WindowPadding = ImVec2(0, 0);  // panels manage their own padding
    // FramePadding.y of 10 makes the dock tab bar ~font(13) + 20 = 33px tall,
    // close to the mockup's 34px; framed widgets (buttons, drag fields) get
    // taller too, which suits the mockup's spacing.
    // FramePadding.x = 14 is the side padding on tabs (left of the label,
    // right of the X); the tab width counts it twice. Other framed widgets
    // gain a few px on each side as well.
    style.FramePadding = ImVec2(14, 10);
    style.ItemSpacing = ImVec2(8, 4);
    style.ItemInnerSpacing = ImVec2(4, 4);
    style.ScrollbarSize = 8.0f;  // uniform thin scrollbars across all panels
    style.GrabMinSize = 8.0f;

    // Left-aligned text in buttons, selectables and tables, so asset pickers,
    // header buttons and list rows read as values (as in the mockup; ImGui's
    // default is centered).
    style.ButtonTextAlign = ImVec2(0.0f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);

    // FontSizeBase is the default size for PushFont(NULL, ...), multiplied by
    // FontScaleMain × FontScaleDpi at draw time. This is a fallback; main.cpp
    // sets the exact value with Fonts::CssPx(g_FontUI, 13) once fonts load.
    style.FontSizeBase = 13.0f;
    style.FontScaleMain = 1.0f;
    // The initial FontScaleDpi. io.ConfigDpiScaleFonts only updates it when
    // the monitor DPI changes, not at startup, so without this fonts render
    // at 1.0× on a 2.0× display. ImGui's own examples do the same.
    style.FontScaleDpi = dpiScale;

    // ScaleAllSizes is still needed in 1.92: ImGui's auto-DPI scales fonts but
    // not sizes and padding. This scales the style fields set above by
    // dpiScale, so padding and spacing match the font scale.
    style.ScaleAllSizes(dpiScale);

    ImVec4* colors = style.Colors;

    const ImVec4& bg = Palette::Bg;
    const ImVec4& bg2 = Palette::Bg2;
    const ImVec4& bg3 = Palette::Bg3;
    const ImVec4& panel = Palette::Panel;
    const ImVec4& line = Palette::Line;
    const ImVec4& line2 = Palette::Line2;
    const ImVec4& fg = Palette::Fg;
    const ImVec4& dim = Palette::Dim;
    const ImVec4& accent = Palette::Accent;

    const ImVec4 accentSubtle = ImVec4(accent.x, accent.y, accent.z, 0.12f);
    const ImVec4 accentFaint = ImVec4(accent.x, accent.y, accent.z, 0.08f);

    // Text
    colors[ImGuiCol_Text] = fg;
    colors[ImGuiCol_TextDisabled] = dim;

    // Surfaces
    colors[ImGuiCol_WindowBg] = bg;
    colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_PopupBg] = panel;
    colors[ImGuiCol_Border] = line2;
    colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

    // Input fields
    colors[ImGuiCol_FrameBg] = bg3;
    colors[ImGuiCol_FrameBgHovered] = bg2;
    colors[ImGuiCol_FrameBgActive] = bg2;

    // Title bars / menus
    colors[ImGuiCol_TitleBg] = panel;
    colors[ImGuiCol_TitleBgActive] = panel;
    colors[ImGuiCol_TitleBgCollapsed] = panel;
    colors[ImGuiCol_MenuBarBg] = panel;

    // Scrollbars
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ScrollbarGrab] = line2;
    colors[ImGuiCol_ScrollbarGrabHovered] = Palette::Line3;
    colors[ImGuiCol_ScrollbarGrabActive] = dim;

    // Controls
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = Palette::AccentDim;
    colors[ImGuiCol_SliderGrabActive] = accent;
    colors[ImGuiCol_Button] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ButtonHovered] = bg2;
    colors[ImGuiCol_ButtonActive] = bg;

    // Headers (selectable rows, tree nodes, collapsing headers)
    colors[ImGuiCol_Header] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_HeaderHovered] = accentFaint;
    colors[ImGuiCol_HeaderActive] = accentSubtle;

    // Separators
    colors[ImGuiCol_Separator] = line;
    colors[ImGuiCol_SeparatorHovered] = accent;
    colors[ImGuiCol_SeparatorActive] = accent;

    // Resize grips
    colors[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ResizeGripHovered] = Palette::AccentDim;
    colors[ImGuiCol_ResizeGripActive] = accent;

    // Tabs (panel headers): the active one uses Bg, inactive ones Panel. The
    // active tab's underline is drawn per panel.
    colors[ImGuiCol_Tab] = panel;
    colors[ImGuiCol_TabHovered] = bg;
    colors[ImGuiCol_TabActive] = bg;
    colors[ImGuiCol_TabUnfocused] = panel;
    colors[ImGuiCol_TabUnfocusedActive] = bg;
    // ImGui draws a 2px overline along the top of the selected dock tab
    // (ImGuiTabBarFlags_DrawSelectedOverline, always on for dock nodes). It
    // covers the top of the close (X) button, and RestyleTabRow already draws
    // an accent underline, so both overline colors are transparent.
    colors[ImGuiCol_TabSelectedOverline] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);

    // Docking
    colors[ImGuiCol_DockingPreview] = Palette::AccentDim;
    colors[ImGuiCol_DockingEmptyBg] = bg;

    // Tables
    colors[ImGuiCol_TableHeaderBg] = panel;
    colors[ImGuiCol_TableBorderStrong] = line2;
    colors[ImGuiCol_TableBorderLight] = line;
    colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.018f);

    // Selection / drag / nav
    colors[ImGuiCol_TextSelectedBg] = accentSubtle;
    colors[ImGuiCol_DragDropTarget] = accent;
    colors[ImGuiCol_NavHighlight] = accent;
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.7f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.5f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
}

// ----------------------------------------------------------------------------
// Schematic chrome helpers
// ----------------------------------------------------------------------------

// Draws `text` with `letterSpacing` extra pixels between glyphs at the given
// font size. Walks UTF-8 codepoints and uses CalcTextSizeA for each advance
// (no kerning, fine for short uppercase labels). Returns the total width.
static float DrawSpacedText(ImDrawList* dl, ImFont* font, float fontSize, ImVec2 pos, ImU32 col, const char* text,
                            float letterSpacing)
{
    if (!font || !text || !*text)
    {
        return 0.0f;
    }
    float x = pos.x;
    const char* s = text;
    while (*s)
    {
        unsigned int c;
        int bytes = ImTextCharFromUtf8(&c, s, nullptr);
        if (bytes <= 0)
        {
            break;
        }
        const char* glyphBegin = s;
        const char* glyphEnd = s + bytes;
        dl->AddText(font, fontSize, ImVec2(x, pos.y), col, glyphBegin, glyphEnd);
        const float advance = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, glyphBegin, glyphEnd).x;
        x += advance + letterSpacing;
        s += bytes;
    }
    return x - pos.x;
}

// Uppercases ASCII text into `out` (size capped). Other bytes pass through
// unchanged, so Tabler icons (in the Private Use Area) are kept.
static void Upper(const char* in, char* out, size_t outCap)
{
    size_t i = 0;
    while (in[i] && i + 1 < outCap)
    {
        char c = in[i];
        if (c >= 'a' && c <= 'z')
        {
            c -= 32;
        }
        out[i] = c;
        ++i;
    }
    out[i] = 0;
}

// Repaints each tab's label area with the tab background, then redraws the
// label in g_FontUIBold, uppercased and letter-spaced. Also draws a 2px
// accent underline on the visible tab and a 1px hairline along the bar's
// bottom, as in the mockup.
//
// `leftEdgeX` / `rightEdgeX` are the visual edges of the dock column. The
// bar's BarRect is inset on both sides by WindowBorderSize + FramePadding.x,
// which would leave gaps before the column separators. For standalone
// (non-dock) bars, pass BarRect.Min.x / Max.x.
static void RestyleTabRow(ImGuiTabBar* bar, ImDrawList* dl, float leftEdgeX, float rightEdgeX)
{
    if (!bar || bar->Tabs.Size == 0 || !g_FontUIBold)
    {
        return;
    }

    const ImU32 colAccent = ImGui::ColorConvertFloat4ToU32(Palette::Accent);
    const ImU32 colLine = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
    const ImU32 colBgAct = ImGui::ColorConvertFloat4ToU32(Palette::Bg);
    const ImU32 colBgInact = ImGui::ColorConvertFloat4ToU32(Palette::Panel);
    const ImU32 colFg = ImGui::ColorConvertFloat4ToU32(Palette::Fg);
    const ImU32 colDim = ImGui::ColorConvertFloat4ToU32(Palette::Dim);

    const float barY0 = bar->BarRect.Min.y;
    const float barY1 = bar->BarRect.Max.y;
    // Tab labels: Inter at the mockup's 11px reads smaller in ImGui than in a
    // browser, so ~12.5px is used instead. Letter spacing stays at the
    // mockup's ~0.08em. Both are ratios of GetFontSize(), so DPI scaling is
    // automatic.
    const float bodyFont = ImGui::GetFontSize();  // = FontSizeBase * FontScaleDpi
    const float fontSize = ImFloor(bodyFont * (12.5f / 13.0f));
    const float letterSpacing = bodyFont * (1.0f / 13.0f);

    // The same tab positions ImGui computes in imgui_widgets.cpp:
    //   central section: BarRect.Min.x + (Offset - ScrollingAnim)
    //   leading/trailing: BarRect.Min.x + Offset
    // Central tabs are clipped to ScrollingRectMinX..ScrollingRectMaxX, so a
    // tab scrolled past the leading section does not draw over it.
    const float scroll = bar->ScrollingAnim;
    // The close button strip on the right of every dock tab:
    // FramePadding.x + ItemInnerSpacing.x + FontSize, as TabItemCalcSize adds
    // when has_close_button_or_unsaved_marker is true. The background repaint
    // stops short of it, so ImGui's close (X) glyph stays visible.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float closeStripW = style.FramePadding.x + style.ItemInnerSpacing.x + ImGui::GetFontSize();
    for (int t = 0; t < bar->Tabs.Size; ++t)
    {
        ImGuiTabItem* tab = &bar->Tabs[t];
        const bool isCentral = (tab->Flags & ImGuiTabItemFlags_SectionMask_) == 0;
        const float tabXOffset = isCentral ? (tab->Offset - scroll) : tab->Offset;
        const float x0 = bar->BarRect.Min.x + tabXOffset;
        const float x1 = x0 + tab->Width;
        const bool active = (tab->ID == bar->VisibleTabId);

        // Clip central tabs, so a scrolled-out tab does not spill onto the
        // leading/trailing buttons or past the bar.
        const bool wantClip = isCentral && (x0 < bar->ScrollingRectMinX || x1 > bar->ScrollingRectMaxX);
        if (wantClip)
        {
            dl->PushClipRect(ImVec2(bar->ScrollingRectMinX, barY0), ImVec2(bar->ScrollingRectMaxX, barY1), true);
        }

        // Cover ImGui's own label, but not the close-button strip on the
        // right: ImGui drew the (X) earlier this frame.
        const float labelAreaRight = ImMax(x0, x1 - closeStripW);
        dl->AddRectFilled(ImVec2(x0, barY0), ImVec2(labelAreaRight, barY1), active ? colBgAct : colBgInact);

        // The panel's display name is the tab name without the ###ID suffix
        // ("Hierarchy###Hierarchy" shows as "HIERARCHY").
        const char* rawName = ImGui::TabBarGetTabName(bar, tab);
        if (!rawName)
        {
            continue;
        }
        char visible[64];
        {
            size_t i = 0;
            while (rawName[i] && i + 1 < sizeof(visible))
            {
                // ImGui's ###ID convention: everything from "###" onward is ID-only.
                if (rawName[i] == '#' && rawName[i + 1] == '#' && rawName[i + 2] == '#')
                {
                    break;
                }
                visible[i] = rawName[i];
                ++i;
            }
            visible[i] = 0;
        }
        char upper[64];
        Upper(visible, upper, sizeof(upper));

        // Label width with this font and letter spacing. Tabs use
        // ImGuiTabBarFlags_FittingPolicyScroll (patched in imgui.cpp), so a tab
        // keeps its natural size and the bar scrolls when full.
        ImFont* font = g_FontUIBold;
        int chars = 0;
        for (const char* s = upper; *s;)
        {
            unsigned int c;
            int bytes = ImTextCharFromUtf8(&c, s, nullptr);
            if (bytes <= 0)
            {
                break;
            }
            s += bytes;
            ++chars;
        }
        float labelW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, upper).x;
        if (chars > 1)
        {
            labelW += letterSpacing * (chars - 1);
        }

        // Ask for the width TabItemCalcSize would compute, but from this
        // label's measured width instead of the default font's. With the
        // label placed at x0 + FramePadding.x (the X's inset, mirrored), the
        // label and X sit symmetrically in the tab. A new tab lags one frame.
        tab->RequestedWidth = labelW + 2.0f * style.FramePadding.x + style.ItemInnerSpacing.x + ImGui::GetFontSize();

        // Center the label + X group in the tab [x0, x1] by mirroring the X's
        // right inset on the left. x0, not tabStartX: the leftmost tab's
        // extension to leftEdgeX only fills the dock-column gap, and every
        // label should sit at the same offset.
        const float labelX = x0 + style.FramePadding.x;
        const float labelY = barY0 + ImFloor((barY1 - barY0 - fontSize) * 0.5f);

        DrawSpacedText(dl, font, fontSize, ImVec2(labelX, labelY), active ? colFg : colDim, upper, letterSpacing);

        // 2px accent underline under the active tab, across its full width.
        if (active)
        {
            dl->AddRectFilled(ImVec2(x0, barY1 - 2.0f), ImVec2(x1, barY1), colAccent);
        }

        if (wantClip)
        {
            dl->PopClipRect();
        }
    }

    // 1px hairline along the bar's bottom, out to the dock node's real edges
    // so it meets the column separators.
    dl->AddLine(ImVec2(leftEdgeX, barY1), ImVec2(rightEdgeX, barY1), colLine, 1.0f);
}

// ImGui draws the seam between two docked panels itself: its dock splitter
// fills the 1px separator gap with ImGuiCol_Border, and with
// WindowBorderSize = 0 the panels draw no border beside it.
//
// WindowBorderSize = 0 also removes the frame around floating windows
// (dialogs, undocked tool windows), so this draws it on each such window's
// own draw list, on top of its content. Docked tabs are skipped (their edges
// are the dock separators); child, popup and tooltip windows have their own
// *BorderSize.
//
// Call once per frame after all panels have rendered, right before
// ImGui::Render() (alongside DrawAllTabTopAccents).
void DrawDockSeams()
{
    ImGuiContext& g = *GImGui;
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Border);
    const ImGuiViewport* mainVp = ImGui::GetMainViewport();

    for (int i = 0; i < g.Windows.Size; ++i)
    {
        ImGuiWindow* w = g.Windows[i];
        if (!w || !w->WasActive || w->Hidden)
        {
            continue;
        }
        if (w != w->RootWindow)
        {
            continue;
        }
        if (w->DockIsActive || w->DockNodeAsHost)
        {
            continue;
        }
        // The window filling the main viewport (main.cpp's DockSpace host,
        // which holds the menu bar and toolbar) has the OS window's edges as
        // its own: a frame there is a stray line along the screen's edges.
        if (w->Viewport == mainVp && w->Pos.x <= mainVp->WorkPos.x && w->Pos.y <= mainVp->WorkPos.y &&
            w->Size.x >= mainVp->WorkSize.x && w->Size.y >= mainVp->WorkSize.y)
        {
            continue;
        }
        if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip))
        {
            continue;
        }
        w->DrawList->AddRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y), col, w->WindowRounding, 0,
                             1.0f);
    }
}

void DrawAllTabTopAccents()
{
    ImGuiContext& g = *GImGui;

    // Draw on each tab bar's own window's draw list, not the foreground list,
    // which is above every window including tooltips and popups. This keeps
    // the accent at the panel's layer, below later windows.

    // 1) Dock-node tab bars (one per docked panel group). ImGui draws the bar
    //    onto node->HostWindow->DrawList in DockNodeUpdateTabBar, so the
    //    accent goes there too.
    ImGuiDockNode* lastNode = nullptr;
    for (int i = 0; i < g.Windows.Size; ++i)
    {
        ImGuiWindow* w = g.Windows[i];
        if (!w || !w->DockNode || w->DockNode == lastNode)
        {
            continue;
        }
        lastNode = w->DockNode;
        // Only a host drawn this frame. A dock node outlives its windows, and
        // ImGui frees an idle host's draw list buffers after about a minute
        // (e.g. on the project manager screen, where no panel is drawn), so
        // drawing onto it would write through a freed pointer.
        ImGuiWindow* host = w->DockNode->HostWindow;
        if (host && w->DockNode->TabBar && host->Active && !host->Hidden && host->LastFrameActive == g.FrameCount)
        {
            // Column edges are the dock node's bounds (Pos.x..Pos.x+Size.x).
            // The bar's BarRect is inset by border + frame padding and would
            // leave gaps before the dock separators.
            const float leftEdgeX = w->DockNode->Pos.x;
            const float rightEdgeX = w->DockNode->Pos.x + w->DockNode->Size.x;
            RestyleTabRow(w->DockNode->TabBar, w->DockNode->HostWindow->DrawList, leftEdgeX, rightEdgeX);
        }
    }

    // 2) Standalone tab bars from BeginTabBar (e.g. the Asset Browser's
    //    Assets/Scripts sub-tabs). They have no pointer to their window, so
    //    find it by hit-testing the bar rect against active windows and
    //    taking the innermost match.
    for (int i = 0; i < g.TabBars.GetMapSize(); ++i)
    {
        ImGuiTabBar* bar = g.TabBars.TryGetMapData(i);
        if (!bar)
        {
            continue;
        }
        if (bar->PrevFrameVisible + 1 < g.FrameCount)
        {
            continue;
        }

        ImGuiWindow* owner = nullptr;
        float ownerArea = FLT_MAX;
        const ImVec2 probe = bar->BarRect.GetCenter();
        for (int wi = 0; wi < g.Windows.Size; ++wi)
        {
            ImGuiWindow* w = g.Windows[wi];
            if (!w || !w->Active || w->Hidden)
            {
                continue;
            }
            // Skip tooltips, popups and menus: the bar belongs to a panel.
            if (w->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup))
            {
                continue;
            }
            ImRect r = w->Rect();
            if (!r.Contains(probe))
            {
                continue;
            }
            float area = r.GetWidth() * r.GetHeight();
            if (area < ownerArea)
            {
                owner = w;
                ownerArea = area;
            }
        }
        if (owner)
        {
            RestyleTabRow(bar, owner->DrawList, bar->BarRect.Min.x, bar->BarRect.Max.x);
        }
    }
}

bool SchematicCheckbox(const char* label, bool* v, const ImVec4& fillColor)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
    {
        return false;
    }

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const float fontSize = g.FontSize;
    const float frameH = ImGui::GetFrameHeight();
    // Outer box: square, sized to read clearly (not a tiny stamp) and centered
    // in the row's frame height so it lines up with the frame-padded label.
    const float box = ImFloor(fontSize * 1.2f);
    const ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);

    const ImVec2 pos = window->DC.CursorPos;
    const ImVec2 boxPos(pos.x, pos.y + ImFloor((frameH - box) * 0.5f));
    const ImRect total(pos, ImVec2(pos.x + box + (labelSize.x > 0.0f ? style.ItemInnerSpacing.x + labelSize.x : 0.0f),
                                   pos.y + ImMax(frameH, box)));

    ImGui::ItemSize(total, style.FramePadding.y);
    if (!ImGui::ItemAdd(total, id))
    {
        return false;
    }

    bool hovered = false, held = false;
    bool pressed = ImGui::ButtonBehavior(total, id, &hovered, &held);
    if (pressed)
    {
        *v = !*v;
        ImGui::MarkItemEdited(id);
    }

    ImDrawList* dl = window->DrawList;
    const ImU32 borderCol = ImGui::ColorConvertFloat4ToU32(hovered ? Palette::Line3 : Palette::Line2);

    // Outer box: 1px border, transparent fill (as in the mockup). Rounded like
    // the themed frames, at most half the box size.
    const ImVec2 bMin = boxPos;
    const ImVec2 bMax(boxPos.x + box, boxPos.y + box);
    const float boxRounding = ImMin(style.FrameRounding, box * 0.5f);
    dl->AddRect(bMin, bMax, borderCol, boxRounding, 0, 1.0f);

    if (*v)
    {
        const ImU32 inner = ImGui::ColorConvertFloat4ToU32(fillColor);
        const float innerInset = ImFloor(box * 0.28f);
        dl->AddRectFilled(ImVec2(bMin.x + innerInset, bMin.y + innerInset),
                          ImVec2(bMax.x - innerInset, bMax.y - innerInset), inner,
                          ImMax(0.0f, boxRounding - innerInset));
    }

    if (labelSize.x > 0.0f)
    {
        ImGui::RenderText(
            ImVec2(bMax.x + style.ItemInnerSpacing.x, pos.y + ImFloor((ImMax(frameH, box) - fontSize) * 0.5f)), label);
    }

    return pressed;
}

bool SchematicCheckbox(const char* label, bool* v)
{
    return SchematicCheckbox(label, v, Palette::Accent);
}

void BeginPropertyContext()
{
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    ImGui::Indent(Metrics::kInspectorIndent * dpi);
    // Row gap and field padding together: separation comes from the height of
    // the fields, not from air between them, which is what keeps a long list
    // reading as a table instead of a stack of loose controls.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, Metrics::kRowSpacing * dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Metrics::kFieldPadding * dpi, Metrics::kFieldPadding * dpi));
}

void EndPropertyContext()
{
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    ImGui::PopStyleVar(2);
    ImGui::Unindent(Metrics::kInspectorIndent * dpi);
}

bool SchematicCollapsingHeader(const char* label, ImGuiTreeNodeFlags flags)
{
    // Styled like a dock tab (uppercase letter-spaced SemiBold label, tab
    // background, bottom hairline), but not movable and with no accent
    // underline. Still a real CollapsingHeader, so clicking folds it.
    ImGuiWindow* w = ImGui::GetCurrentWindow();

    // Split the label into its visible text and any "###ID" tail. The visible
    // text is drawn here (letter-spaced), so CollapsingHeader gets only the
    // id: the same ImGui id, so its state is kept, and no label text.
    const char* hash = ImStrchrRange(label, label + strlen(label), '#');
    const bool hasId = hash && hash[1] == '#' && hash[2] == '#';
    const size_t visLen = hasId ? (size_t)(hash - label) : strlen(label);

    // The id comes from the English text; only the drawn text is translated.
    char visUpper[256];
    char idStr[160];
    {
        char vis[128];
        size_t n = visLen < sizeof(vis) - 1 ? visLen : sizeof(vis) - 1;
        for (size_t i = 0; i < n; ++i)
        {
            vis[i] = label[i];
        }
        vis[n] = 0;
        if (hasId)
        {
            ImFormatString(idStr, sizeof(idStr), "%s", hash);  // "###ID"
        }
        else
        {
            Upper(vis, visUpper, sizeof(visUpper));
            ImFormatString(idStr, sizeof(idStr), "###%s", visUpper);  // id == hash(visUpper)
        }
        Upper(Tr(vis), visUpper, sizeof(visUpper));
    }

    // The header band runs to the panel's real side edges (the dock
    // separators), past the scrollbar on the right.
    //
    // The chevron and label sit one step left of the content, so the rows
    // under the band read as nested inside it. At least a margin from the
    // panel edge, so a header at no indent still has space to its left, and
    // relative, so a band nested deeper still shows its depth.
    const float textBaseX = w ? w->DC.CursorPos.x : 0.0f;
    float labelBaseX = textBaseX;
    float savedWorkMinX = 0.0f, savedWorkMaxX = 0.0f;
    bool pushedBandClip = false;
    if (w)
    {
        savedWorkMinX = w->WorkRect.Min.x;
        savedWorkMaxX = w->WorkRect.Max.x;
        const float bandLeft = w->Pos.x;
        const float bandRight = w->Pos.x + w->Size.x - w->ScrollbarSizes.x;  // stop at the scrollbar
        const float dpiScale = ImGui::GetStyle().FontScaleDpi;
        labelBaseX = ImMax(bandLeft + Metrics::kSectionHeaderMargin * dpiScale,
                           textBaseX - Metrics::kSectionHeaderOutdent * dpiScale);
        w->DC.CursorPos.x = bandLeft;
        w->WorkRect.Min.x = bandLeft;
        w->WorkRect.Max.x = bandRight;
        // Widen the clip so the band paints under the scrollbar, out to the
        // separator.
        ImGui::PushClipRect(ImVec2(bandLeft, w->ClipRect.Min.y), ImVec2(bandRight, w->ClipRect.Max.y), false);
        pushedBandClip = true;
    }

    // Tab background, with a subtle white hover/active (no accent). The text
    // color is transparent, which hides ImGui's filled-triangle arrow; a thin
    // Tabler chevron is drawn below instead.
    ImGui::PushStyleColor(ImGuiCol_Header, Palette::Bg);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.04f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    // Header band a fixed 37 CSS px tall. CollapsingHeader height is
    // fontHeight + FramePadding.y * 2, and the font height is DPI-scaled, so
    // solve the padding from the target (37 px * DPI) and the current font.
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float bandH = 37.0f * dpi;
    const float padY = ImMax(0.0f, (bandH - ImGui::GetFontSize()) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, padY));
    // No 1px frame box around the band (the theme's FrameBorderSize is 1).
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

    // No row gap after the band. The gap belongs to the body (see
    // SchematicSectionBegin, which adds it), so a collapsed section sits flush
    // on the hairline under it, and the gap slides away with the body.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    const bool open = ImGui::CollapsingHeader(idStr, flags);
    ImGui::PopStyleVar();
    // The chevron zone comes from the theme token, not from
    // GetTreeNodeToLabelSpacing(): ImGui's own arrow and label are hidden
    // here, and the token is the column every band and row in every panel
    // aligns to. See Metrics::kSectionLabelIndent.
    const float labelSpacing = Metrics::kSectionLabelIndent * dpi;

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);

    if (pushedBandClip)
    {
        ImGui::PopClipRect();
    }

    // Restore the work rect, and keep CursorPosPrevLine at the content's right
    // edge, so a trailing SameLine (the `EDIT ->` link) lands where expected.
    if (w)
    {
        w->WorkRect.Min.x = savedWorkMinX;
        w->WorkRect.Max.x = savedWorkMaxX;
        w->DC.CursorPosPrevLine.x = savedWorkMaxX;
    }

    if (w && !w->SkipItems)
    {
        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const ImVec2 itemMax = ImGui::GetItemRectMax();
        const float centerY = (itemMin.y + itemMax.y) * 0.5f;
        ImDrawList* dl = w->DrawList;

        // Soft-gray for both chevron and label: rgb(165,166,164).
        const ImU32 gray =
            ImGui::ColorConvertFloat4ToU32(ImVec4(165.0f / 255.0f, 166.0f / 255.0f, 164.0f / 255.0f, 1.0f));

        // A thin Tabler chevron instead of ImGui's filled triangle, centered in
        // the arrow zone before the label. It turns from right to down as the
        // section opens, which shows which section moved when several are open.
        ImFont* ico = g_FontUI ? g_FontUI : ImGui::GetFont();
        const float icoSize = ImGui::GetFontSize();
        if (!(flags & ImGuiTreeNodeFlags_Leaf))  // a leaf band does not fold
        {
            const float openT = AnimateTo(ImGui::GetItemID(), open ? 1.0f : 0.0f, 0.13f);
            DrawTurningChevron(dl, ico, icoSize, ImVec2(labelBaseX + labelSpacing * 0.5f, centerY), gray,
                               ICON_TI_CHEVRON_RIGHT, openT, IM_PI * 0.5f);
        }

        // Tab label: uppercase, letter-spaced, Inter SemiBold, after the arrow.
        const float bodyFont = ImGui::GetFontSize();
        const float fontSize = ImFloor(bodyFont * (12.5f / 13.0f));
        const float letterSpacing = bodyFont * (1.0f / 13.0f);
        ImFont* f = g_FontUIBold ? g_FontUIBold : ImGui::GetFont();
        const float labelY = ImFloor(centerY - fontSize * 0.5f);
        DrawSpacedText(dl, f, fontSize, ImVec2(labelBaseX + labelSpacing, labelY), gray, visUpper, letterSpacing);
    }

    return open;
}

bool SchematicSectionBand(const char* label, float* outLabelX)
{
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems)
    {
        return false;
    }

    // The label is split like SchematicCollapsingHeader's: the visible text is
    // drawn by hand and any "###id" tail only identifies the row, so a band
    // whose text changes keeps one identity.
    //
    // Drawn as written, with no uppercasing or extra tracking: this band shows
    // a thing's name ("myState" is not "MYSTATE"). It also lets the inline
    // rename field (SchematicBandTitleField) look exactly like the title.
    const char* hash = ImStrchrRange(label, label + strlen(label), '#');
    const bool hasId = hash && hash[1] == '#' && hash[2] == '#';
    const size_t visLen = hasId ? (size_t)(hash - label) : strlen(label);

    char vis[128];
    {
        size_t n = visLen < sizeof(vis) - 1 ? visLen : sizeof(vis) - 1;
        for (size_t i = 0; i < n; ++i)
        {
            vis[i] = label[i];
        }
        vis[n] = 0;
    }

    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float bandH = 37.0f * dpi;  // same height as the header
    const ImVec2 pos = w->DC.CursorPos;

    // Out to the panel's side edges, past the scrollbar, like the collapsing
    // header. The clip is widened, not intersected, because the band paints
    // outside the content rect.
    ImDrawList* dl = w->DrawList;
    const float bandLeft = w->Pos.x;
    const float bandRight = w->Pos.x + w->Size.x - w->ScrollbarSizes.x;

    // The label sits one step left of the content, at least a margin from the
    // panel edge: the same offset as the collapsing header, so both kinds of
    // band line up and the rows nest under both.
    const float labelBaseX =
        ImMax(bandLeft + Metrics::kSectionHeaderMargin * dpi, pos.x - Metrics::kSectionHeaderOutdent * dpi);

    // The row is a button, so the caller gets the click on a band that titles
    // something renameable. It spans the whole band (from the panel edge, like
    // the collapsing header's frame), so a click on the outdented label counts.
    ImGui::SetCursorScreenPos(ImVec2(bandLeft, pos.y));
    const float rowW = ImMax(1.0f, w->WorkRect.Max.x - bandLeft);
    // The row covers the whole band, and the first item to claim hover in a
    // frame keeps it. Allowing overlap passes hover and clicks to whatever is
    // drawn on top (the node graph's inline rename field), so a click there
    // moves its caret. The collapsing header does the same with
    // ImGuiTreeNodeFlags_AllowOverlap.
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton(hasId ? hash : label, ImVec2(rowW, bandH));
    const bool hovered = ImGui::IsItemHovered();
    // Nothing may be submitted after the button: it must stay the window's
    // last item, because callers place controls on the band from GetItemRect
    // (the inline rename field). ItemSize already put the cursor's x back at
    // the indent.

    ImGui::PushClipRect(ImVec2(bandLeft, w->ClipRect.Min.y), ImVec2(bandRight, w->ClipRect.Max.y), false);
    dl->AddRectFilled(ImVec2(bandLeft, pos.y), ImVec2(bandRight, pos.y + bandH),
                      ImGui::ColorConvertFloat4ToU32(Palette::Bg));
    if (const float hoverT = Motion::ItemHover(hovered); hoverT > 0.0f)
    {
        dl->AddRectFilled(ImVec2(bandLeft, pos.y), ImVec2(bandRight, pos.y + bandH),
                          Motion::WithAlpha(IM_COL32(255, 255, 255, 10), hoverT));
    }
    if (const float pressT = Motion::ItemPress(); pressT > 0.0f)
    {
        dl->AddRectFilled(ImVec2(bandLeft, pos.y), ImVec2(bandRight, pos.y + bandH), Motion::PressShade(pressT));
    }
    ImGui::PopClipRect();

    // No chevron is drawn, but the label still starts on the shared column,
    // so it reads as the heading of the rows under it. The token is the one
    // the collapsing header and every property row use, so they all line up.
    const float labelTextX = labelBaseX + Metrics::kSectionLabelIndent * dpi;

    const float fontSize = BandTitleFontSize();
    ImFont* f = BandTitleFont();
    const float labelY = ImFloor(pos.y + (bandH - fontSize) * 0.5f);
    dl->AddText(f, fontSize, ImVec2(labelTextX, labelY), BandTitleColor(), vis);

    if (outLabelX)
    {
        *outLabelX = labelTextX;
    }
    return clicked;
}

bool SchematicBandTitleField(const char* id, char* buf, int bufSize, float x, float bandTop, float bandBottom,
                             float width, const char* hint)
{
    if (!buf || bufSize <= 0)
    {
        return false;
    }

    // No frame background, border or padding: the field is text in the band's
    // own type, on the band's line, at the label's x. Starting a rename does
    // not restyle the header; only the caret shows it is editable.
    ImFont* font = BandTitleFont();

    ImGui::PushFont(font, BandTitleFontSizeBase());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    // ImGui outlines whatever holds keyboard focus, and the field is focused
    // as soon as it appears. Hide that outline, which would look like a border.
    ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, BandTitleColorVec());

    // With no padding the frame is exactly the text, so centering the frame on
    // the band centers the glyphs where the label's were.
    const float frameH = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(x, ImFloor((bandTop + bandBottom - frameH) * 0.5f)));
    ImGui::SetNextItemWidth(ImMax(width, 1.0f));
    const bool changed = ImGui::InputTextWithHint(id, hint ? hint : "", buf, (size_t)bufSize);

    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    return changed;
}

void SchematicSubHeader(const char* label)
{
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems)
    {
        return;
    }

    // One step below the section band: the same uppercase letter-spaced type,
    // smaller and dimmer, with a short rule beside it instead of a full-width
    // one. A group is a subdivision of its component; in bold body text it
    // would look like a property with no field.
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float rowH = 24.0f * dpi;
    const ImVec2 pos = w->DC.CursorPos;

    char visUpper[128];
    Upper(label, visUpper, sizeof(visUpper));

    const float bodyFont = ImGui::GetFontSize();
    const float fontSize = ImFloor(bodyFont * (11.0f / 13.0f));
    const float letterSpacing = bodyFont * (1.2f / 13.0f);
    ImFont* f = g_FontUIBold ? g_FontUIBold : ImGui::GetFont();

    ImDrawList* dl = w->DrawList;
    const ImU32 dim = ImGui::ColorConvertFloat4ToU32(Palette::Dim);
    const ImU32 line = ImGui::ColorConvertFloat4ToU32(Palette::Line);
    const float textY = ImFloor(pos.y + (rowH - fontSize) * 0.5f);
    const float textW = DrawSpacedText(dl, f, fontSize, ImVec2(pos.x, textY), dim, visUpper, letterSpacing);

    // Rule from the end of the label to the content edge, on the text's center
    // line: it ties the label to the rows under it without cutting the column.
    const float ruleX0 = pos.x + textW + 10.0f * dpi;
    const float ruleX1 = w->WorkRect.Max.x;
    if (ruleX1 > ruleX0)
    {
        dl->AddLine(ImVec2(ruleX0, ImFloor(pos.y + rowH * 0.5f)), ImVec2(ruleX1, ImFloor(pos.y + rowH * 0.5f)), line,
                    1.0f);
    }

    ImGui::Dummy(ImVec2(ImMax(1.0f, w->WorkRect.Max.x - pos.x), rowH));
}

bool SchematicAddButton(const char* label, float width)
{
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems)
    {
        return false;
    }

    // An outlined, centered box that stays quiet (transparent fill, hairline
    // border, dim label) and turns accent only under the pointer. A solid
    // full-width button would read as one more field to fill in.
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float boxH = 32.0f * dpi;
    const float avail = ImGui::GetContentRegionAvail().x;

    float boxW = width > 0.0f ? width : 240.0f * dpi;
    if (boxW > avail)
    {
        boxW = avail;
    }
    if (boxW < 1.0f)
    {
        boxW = 1.0f;
    }

    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 boxMin(ImFloor(start.x + (avail - boxW) * 0.5f), start.y);
    const ImVec2 boxMax(boxMin.x + boxW, boxMin.y + boxH);

    ImGui::SetCursorScreenPos(boxMin);
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(boxW, boxH));
    const bool hovered = ImGui::IsItemHovered();
    const float pressT = Motion::ItemPress();

    ImDrawList* dl = w->DrawList;
    // Brightens a little further while pressed, easing in and back.
    const ImU32 fill = (hovered || pressT > 0.0f)
                           ? IM_COL32(255, 255, 255, static_cast<int>(8.0f + 6.0f * pressT + 0.5f))
                           : IM_COL32(0, 0, 0, 0);
    const ImU32 border = ImGui::ColorConvertFloat4ToU32(hovered ? Palette::AccentDim : Palette::Line2);
    const ImU32 text = ImGui::ColorConvertFloat4ToU32(hovered ? Palette::Fg : Palette::Dim);
    const float rounding = 2.0f * dpi;

    if (fill)
    {
        dl->AddRectFilled(boxMin, boxMax, fill, rounding);
    }
    dl->AddRect(boxMin, boxMax, border, rounding, 0, 1.0f);

    ImFont* f = g_FontUIMed ? g_FontUIMed : ImGui::GetFont();
    const float fontSize = Fonts::CssPx(f, 12.5f) * dpi;
    const ImVec2 textSz = f->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
    dl->AddText(f, fontSize,
                ImVec2(ImFloor(boxMin.x + (boxW - textSz.x) * 0.5f), ImFloor(boxMin.y + (boxH - textSz.y) * 0.5f)),
                text, label);

    // Put x back for whatever follows; the button itself reserved the row.
    ImGui::SetCursorScreenPos(ImVec2(start.x, ImGui::GetCursorScreenPos().y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopStyleVar();
    return clicked;
}

void DrawTreeNodeChevronLabel(bool open, bool expandable, const char* label, ImU32 labelColor)
{
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems)
    {
        return;
    }

    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    const float centerY = (mn.y + mx.y) * 0.5f;
    const float labelSpacing = ImGui::GetTreeNodeToLabelSpacing();
    ImDrawList* dl = w->DrawList;

    // Chevron, centered in the arrow zone, turning right→down as the node opens
    // (same motion as a section header's, so a tree and an inspector read the
    // same way).
    if (expandable)
    {
        ImFont* ico = g_FontUI ? g_FontUI : ImGui::GetFont();
        const float icoSize = ImGui::GetFontSize();
        const ImU32 gray =
            ImGui::ColorConvertFloat4ToU32(ImVec4(165.0f / 255.0f, 166.0f / 255.0f, 164.0f / 255.0f, 1.0f));
        const float openT = AnimateTo(ImGui::GetItemID(), open ? 1.0f : 0.0f, 0.13f);
        DrawTurningChevron(dl, ico, icoSize, ImVec2(mn.x + labelSpacing * 0.5f, centerY), gray, ICON_TI_CHEVRON_RIGHT,
                           openT, IM_PI * 0.5f);
    }

    // Label - same font/size ImGui would have used, after the arrow zone.
    if (label && *label)
    {
        ImFont* f = ImGui::GetFont();
        const float fs = ImGui::GetFontSize();
        dl->AddText(f, fs, ImVec2(mn.x + labelSpacing, ImFloor(centerY - fs * 0.5f)), labelColor, label);
    }
}

void FullBleedSeparator()
{
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems)
    {
        ImGui::Separator();
        return;
    }

    // Stretch the cursor and work rect to the window's real side edges (the
    // dock seams), and widen the clip so the line paints under the scrollbar
    // to the right seam.
    const float savedWorkMaxX = w->WorkRect.Max.x;
    const float left = w->Pos.x;
    const float right = w->Pos.x + w->Size.x - w->ScrollbarSizes.x;  // stop at the scrollbar
    w->DC.CursorPos.x = left;
    w->WorkRect.Max.x = right;
    ImGui::PushClipRect(ImVec2(left, w->ClipRect.Min.y), ImVec2(right, w->ClipRect.Max.y), false);

    // The line takes exactly its own pixel, with no ItemSpacing above or
    // below. A separator is a boundary; the band or content on either side
    // adds whatever space it needs. Otherwise the gap would grow with each
    // panel's row spacing.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::Separator();
    ImGui::PopStyleVar();

    ImGui::PopClipRect();
    w->WorkRect.Max.x = savedWorkMaxX;  // CursorPos.x is reset to the indent by ItemSize
}

bool PanelToolbar(bool* pinnedExt)
{
    // The toolbar holds the Pin (lock-to-selection) control, which only the
    // Inspector uses. A panel that passes no flag gets no toolbar at all.
    if (!pinnedExt)
    {
        return false;
    }

    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems)
    {
        return *pinnedExt;
    }

    bool* pinned = pinnedExt;

    ImDrawList* dl = w->DrawList;
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float barH = ImGui::GetFontSize() + 10.0f * dpi;  // compact (shorter than the tab)
    const float top = w->DC.CursorPos.y;
    const float left = w->Pos.x;
    const float right = w->Pos.x + w->Size.x - w->ScrollbarSizes.x;  // stop at the scrollbar
    const float cy = top + barH * 0.5f;

    const ImU32 colAccent = ImGui::ColorConvertFloat4ToU32(Palette::Accent);
    const ImU32 colDim = ImGui::ColorConvertFloat4ToU32(Palette::Dim);
    const ImU32 colLine = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
    const ImU32 colHover = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 0.05f));

    ImFont* iconFont = g_FontUI ? g_FontUI : ImGui::GetFont();
    const float iconSize = ImGui::GetFontSize();
    ImFont* lblFont = g_FontUIBold ? g_FontUIBold : ImGui::GetFont();
    const float lblSize = ImFloor(iconSize * (12.5f / 13.0f));
    const float lblSpacing = iconSize * (1.0f / 13.0f);

    const float padX = 10.0f * dpi;      // side padding inside each button
    const float gapIcoLbl = 6.0f * dpi;  // gap between icon and its label

    // Pin button (left): accent and a filled pin glyph when pinned, dim
    // otherwise.
    {
        const char* ico = *pinned ? ICON_TI_PIN_FILLED : ICON_TI_PIN;
        const ImVec2 icoSz = iconFont->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, ico);
        const char* lbl = Tr("PIN");
        const float lblW = lblFont->CalcTextSizeA(lblSize, FLT_MAX, 0.0f, lbl).x + lblSpacing * 3.0f;
        const float btnW = padX * 2.0f + icoSz.x + gapIcoLbl + lblW;

        ImGui::SetCursorScreenPos(ImVec2(left, top));
        if (ImGui::InvisibleButton("##deki_pin_btn", ImVec2(btnW, barH)))
        {
            *pinned = !*pinned;
        }
        if (const float hoverT = Motion::ItemHover(ImGui::IsItemHovered()); hoverT > 0.0f)
        {
            dl->AddRectFilled(ImVec2(left, top), ImVec2(left + btnW, top + barH), Motion::WithAlpha(colHover, hoverT));
        }
        if (const float pressT = Motion::ItemPress(); pressT > 0.0f)
        {
            dl->AddRectFilled(ImVec2(left, top), ImVec2(left + btnW, top + barH), Motion::PressShade(pressT));
        }

        const ImU32 col = *pinned ? colAccent : colDim;
        const float ix = left + padX;
        dl->AddText(iconFont, iconSize, ImVec2(ix, ImFloor(cy - icoSz.y * 0.5f)), col, ico);
        DrawSpacedText(dl, lblFont, lblSize, ImVec2(ix + icoSz.x + gapIcoLbl, ImFloor(cy - lblSize * 0.5f)), col, lbl,
                       lblSpacing);
    }

    // Kebab menu (right).
    {
        const char* ico = ICON_TI_DOTS_VERTICAL;
        const ImVec2 icoSz = iconFont->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, ico);
        const float btnW = padX * 2.0f + icoSz.x;
        const float bx = right - btnW;

        ImGui::SetCursorScreenPos(ImVec2(bx, top));
        if (ImGui::InvisibleButton("##deki_panel_menu_btn", ImVec2(btnW, barH)))
        {
            ImGui::OpenPopup("##deki_panel_menu");
        }
        if (const float hoverT = Motion::ItemHover(ImGui::IsItemHovered()); hoverT > 0.0f)
        {
            dl->AddRectFilled(ImVec2(bx, top), ImVec2(bx + btnW, top + barH), Motion::WithAlpha(colHover, hoverT));
        }
        if (const float pressT = Motion::ItemPress(); pressT > 0.0f)
        {
            dl->AddRectFilled(ImVec2(bx, top), ImVec2(bx + btnW, top + barH), Motion::PressShade(pressT));
        }
        dl->AddText(iconFont, iconSize, ImVec2(bx + padX, ImFloor(cy - icoSz.y * 0.5f)), colDim, ico);
    }

    // Styled like every other context menu (padding, row height).
    PushContextMenuPopupStyle();
    if (ImGui::BeginPopup("##deki_panel_menu"))
    {
        PushContextMenuItemStyle();
        if (ImGui::MenuItem(TrLabel("Pin"), nullptr, *pinned))
        {
            *pinned = !*pinned;
        }
        ImGui::PopStyleVar();  // ItemSpacing
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();  // WindowPadding

    // Bottom hairline, seam to seam.
    dl->AddLine(ImVec2(left, top + barH), ImVec2(right, top + barH), colLine, 1.0f);

    // Content starts below that hairline, not on it: one window is one draw
    // list, so the panel's first row (a log row's background, a hovered row's
    // fill) would paint over the bar's edge. Reserved as an item, not a bare
    // cursor move, so a panel that draws nothing under its bar does not trip
    // ImGui's "submit an item" check.
    ImGui::SetCursorScreenPos(ImVec2(left, top + barH));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::Dummy(ImVec2(0.0f, 1.0f));
    ImGui::PopStyleVar();
    return *pinned;
}

void SearchField(const char* id, char* buf, size_t bufSize, float rightReserve, bool bottomBorder, float* outSlotX,
                 float* outSlotY, float* outSlotW, float* outSlotH, const char* hint)
{
    // A dim magnifier and a borderless transparent input, vertically centered,
    // hinting "Search". Sized by the window's DPI scale, as EditorApp::Dpi()
    // does, so the band matches the rest of the panel.
    const float dpi = ImGui::GetWindowDpiScale();
    const float searchInset = 12.0f * dpi;
    const float inputSize = Fonts::CssPx(g_FontUI, 12.0f);
    const float iconSize = Fonts::CssPx(g_FontUI, 14.0f);
    const float searchPadY = 6.0f * dpi;
    const float slotGap = 8.0f * dpi;  // gap between the input and a reserved slot

    // Tighter frame padding, so the input is shorter than the theme default.
    const ImVec2 themePad = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(themePad.x, 4.0f * dpi));
    const float framePadY = ImGui::GetStyle().FramePadding.y;

    // Measure the rendered (DPI-scaled) sizes with each font pushed.
    ImGui::PushFont(g_FontUI, inputSize);
    const float inputLineH = ImGui::GetTextLineHeight();
    const float inputFrameH = ImGui::GetFrameHeight();
    ImGui::PopFont();

    ImGui::PushFont(g_FontUI, iconSize);
    const float iconLineH = ImGui::GetTextLineHeight();
    const float iconAdvance = ImGui::CalcTextSize(ICON_TI_SEARCH).x;
    ImGui::PopFont();

    const ImVec2 bandStart = ImGui::GetCursorScreenPos();
    const float winLeft = ImGui::GetWindowPos().x;
    const float winRight = winLeft + ImGui::GetWindowSize().x;
    const float iconLeftX = bandStart.x + searchInset;

    // The band is the search input's height plus a little padding, with or
    // without a reserved slot. A reserved control fills the whole band
    // (reported through the out-params), flat against the hairline.
    const float rowTop = bandStart.y + searchPadY;
    const float bandBottom = rowTop + inputFrameH + searchPadY;
    const float inputY = rowTop;

    // Right edge of the input. A reserved slot sits flush against the window's
    // right edge and the input stops a gap short of it; otherwise the input
    // keeps the symmetric searchInset gutter.
    float inputRight = winRight - searchInset;
    if (rightReserve > 0.0f)
    {
        inputRight = (winRight - rightReserve) - slotGap;
    }

    // Magnifier icon, vertically centered on the input text's middle.
    const float iconCursorOffset = framePadY + (inputLineH - iconLineH) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(iconLeftX, inputY + iconCursorOffset));
    ImGui::PushFont(g_FontUI, iconSize);
    ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim2);
    ImGui::TextUnformatted(ICON_TI_SEARCH);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // With text in the field, the input stops short of a clear button.
    // Measured whether or not it is drawn: the input must not change width as
    // you type, or the caret jumps.
    float clearAdvance = 0.0f;
    {
        ImGui::PushFont(g_FontUI, iconSize);
        clearAdvance = ImGui::CalcTextSize(ICON_TI_X).x;
        ImGui::PopFont();
    }
    const float clearWidth = clearAdvance + slotGap;

    // The input starts after the icon and an 8px gap.
    const float inputLeftX = iconLeftX + iconAdvance + 8.0f;
    ImGui::SetCursorScreenPos(ImVec2(inputLeftX, inputY));
    ImGui::PushFont(g_FontUI, inputSize);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, Palette::Dim2);
    ImGui::SetNextItemWidth((inputRight - clearWidth) - inputLeftX);
    ImGui::InputTextWithHint(id, Tr(hint ? hint : TrMark("Search")), buf, bufSize);
    // Escape empties the field as well as dropping focus. In a popup ImGui
    // uses Escape to close, so this is checked while the input is active.
    const bool inputActive = ImGui::IsItemActive();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::PopFont();

    // Read after the input: InputText writes into the buffer, and reading
    // first would leave the button a frame behind.
    const bool hasText = buf && buf[0] != '\0';

    if (hasText && inputActive && ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        buf[0] = '\0';
        ImGui::ClearActiveID();
    }

    // The clear button, drawn only when there is something to clear: a dim x
    // that brightens under the pointer, on the input's baseline.
    if (hasText)
    {
        const float clearX = inputRight - clearAdvance;
        ImGui::SetCursorScreenPos(ImVec2(clearX, inputY + iconCursorOffset));
        ImGui::PushFont(g_FontUI, iconSize);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            ImGui::IsMouseHoveringRect(ImVec2(clearX, inputY), ImVec2(clearX + clearAdvance, inputY + inputFrameH))
                ? Palette::Fg
                : Palette::Dim2);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        // Scoped by the field's id, so two search fields in one window do not
        // share a button.
        ImGui::PushID(id);
        if (ImGui::Button(ICON_TI_X "##clear"))
        {
            buf[0] = '\0';
        }
        ImGui::PopID();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
        ImGui::PopFont();
    }

    ImGui::PopStyleVar();  // FramePadding

    // Report the reserved slot (flush right, full band height), so the caller
    // can fill it with a toolbar-style control flush to the hairline.
    if (outSlotX)
    {
        *outSlotX = winRight - rightReserve;
    }
    if (outSlotY)
    {
        *outSlotY = bandStart.y;
    }
    if (outSlotW)
    {
        *outSlotW = rightReserve;
    }
    if (outSlotH)
    {
        *outSlotH = bandBottom - bandStart.y;
    }

    // Full-width hairline under the row, like the panel toolbar's bottom
    // border, on a whole pixel. It sits searchPadY below the row content, so a
    // slotted control's border does not touch it.
    if (bottomBorder)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float by = ImFloor(bandBottom);
        dl->AddRectFilled(ImVec2(winLeft, by - 1.0f), ImVec2(winRight, by),
                          ImGui::ColorConvertFloat4ToU32(Palette::Line2));
    }

    // Reserve the band for the layout below, closing with a zero-size item:
    // ImGui's "submit an item e.g. Dummy()" check rejects a bare cursor move
    // as a window's last act.
    ImGui::SetCursorScreenPos(ImVec2(bandStart.x, bandBottom));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopStyleVar();
}

// Draws the formatted value left-aligned (mono) inside a rect on a draw list.
// OverlayLeftAlignedValue passes the current item's rect; the schematic combo
// passes the box rect it captured before BeginCombo, since an open popup
// changes the last-item rect.
static void DrawLeftAlignedValue(ImDrawList* dl, const ImVec2& itemMin, const ImVec2& itemMax, const char* formatted,
                                 float xOffset = 0.0f)
{
    ImFont* font = g_FontMono ? g_FontMono : ImGui::GetFont();
    // Mono value font at 12.5 CSS px. CssPx applies the mono em-ratio, and the
    // DPI scale is applied by hand because AddText skips both.
    const float fontSize = Fonts::CssPx(font, 12.5f) * ImGui::GetStyle().FontScaleDpi;
    const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, formatted);
    const float padX = ImGui::GetStyle().FramePadding.x;
    const float y = itemMin.y + ImMax(0.0f, (itemMax.y - itemMin.y - textSize.y) * 0.5f);

    dl->PushClipRect(ImVec2(itemMin.x + 1 + xOffset, itemMin.y + 1), ImVec2(itemMax.x - 1, itemMax.y - 1), true);
    dl->AddText(font, fontSize, ImVec2(itemMin.x + xOffset + padX, y), ImGui::GetColorU32(ImGuiCol_Text), formatted);
    dl->PopClipRect();
}

// Draws the formatted value left-aligned inside the drag widget just drawn,
// in g_FontMono as in the mockup. Skipped while the widget is in keyboard
// edit (TempInput) mode, so ImGui's text editor shows. `xOffset` reserves room
// on the left (e.g. for an axis stamp).
static void OverlayLeftAlignedValue(const char* formatted, float xOffset = 0.0f)
{
    const ImGuiID id = ImGui::GetItemID();
    if (ImGui::TempInputIsActive(id))
    {
        return;
    }

    DrawLeftAlignedValue(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), formatted,
                         xOffset);
}

// Paints a colored axis stamp on the left edge of the drag frame just drawn.
// Returns the stamp width, so the caller can offset the value past it.
static float OverlayAxisStamp(const char* axisLabel, const ImVec4& axisColor)
{
    const ImGuiID id = ImGui::GetItemID();
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    const ImVec2 labelSize = ImGui::CalcTextSize(axisLabel);
    const float padX = 10.0f * ImGui::GetStyle().FontScaleDpi;  // padding around the X/Y/Z letter
    const float stampW = labelSize.x + padX * 2.0f;
    const bool active = ImGui::TempInputIsActive(id);
    if (active)
    {
        return stampW;  // not painted during text edit, but the width is kept
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = IM_COL32(255, 255, 255, 8);
    const ImU32 sep = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
    const ImU32 fg = ImGui::ColorConvertFloat4ToU32(axisColor);

    dl->AddRectFilled(ImVec2(itemMin.x + 1, itemMin.y + 1), ImVec2(itemMin.x + stampW, itemMax.y - 1), bg);
    dl->AddLine(ImVec2(itemMin.x + stampW, itemMin.y + 1), ImVec2(itemMin.x + stampW, itemMax.y - 1), sep, 1.0f);
    dl->AddText(ImVec2(itemMin.x + padX, itemMin.y + (itemMax.y - itemMin.y - labelSize.y) * 0.5f), fg, axisLabel);
    return stampW;
}

bool SchematicDragFloat(const char* id, float* v, float vSpeed, float vMin, float vMax, const char* format)
{
    // An empty format hides ImGui's centered value text but keeps the widget
    // working; edit mode (TempInput) still uses the real format through
    // ImGui's internal fallback.
    bool changed = ImGui::DragFloat(id, v, vSpeed, vMin, vMax, "");
    if (format && format[0])
    {
        char buf[64];
        ImFormatString(buf, IM_ARRAYSIZE(buf), format, *v);
        OverlayLeftAlignedValue(buf);
    }
    return changed;
}

bool SchematicDragInt(const char* id, int* v, float vSpeed, int vMin, int vMax, const char* format)
{
    bool changed = ImGui::DragInt(id, v, vSpeed, vMin, vMax, "");
    if (format && format[0])
    {
        char buf[64];
        ImFormatString(buf, IM_ARRAYSIZE(buf), format, *v);
        OverlayLeftAlignedValue(buf);
    }
    return changed;
}

// Axis labels and colors for multi-component drags (X / Y / Z).
// The red, green and blue come from oklch, to match the mockup's TwoColD
// axis stamps.
static const char* const kAxisLabels[3] = { "X", "Y", "Z" };
static const ImVec4 kAxisColors[3] = {
    ImVec4(0.91f, 0.42f, 0.31f, 1.0f),  // X - red oklch(0.72 0.2 25)
    ImVec4(0.45f, 0.83f, 0.45f, 1.0f),  // Y - green oklch(0.78 0.2 145)
    ImVec4(0.40f, 0.65f, 1.00f, 1.0f),  // Z - blue oklch(0.7 0.18 240)
};

template <int N>
static bool SchematicDragFloatN(const char* id, float v[N], float vSpeed, float vMin, float vMax, const char* format)
{
    // Split the row width across the N components, with spacing between.
    const float total = ImGui::CalcItemWidth();
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float compW = ImMax(1.0f, (total - spacing * (N - 1)) / float(N));

    bool changed = false;
    ImGui::PushID(id);
    ImGui::BeginGroup();
    for (int i = 0; i < N; ++i)
    {
        if (i > 0)
        {
            ImGui::SameLine(0.0f, spacing);
        }
        ImGui::SetNextItemWidth(compW);
        char compId[8];
        ImFormatString(compId, IM_ARRAYSIZE(compId), "##c%d", i);

        if (ImGui::DragFloat(compId, &v[i], vSpeed, vMin, vMax, ""))
        {
            changed = true;
        }

        // Axis stamp on the left, value overlay offset past the stamp.
        const float stampW = OverlayAxisStamp(kAxisLabels[i], kAxisColors[i]);
        if (format && format[0])
        {
            char buf[64];
            ImFormatString(buf, IM_ARRAYSIZE(buf), format, v[i]);
            OverlayLeftAlignedValue(buf, stampW);
        }
    }
    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

bool SchematicDragFloat2(const char* id, float v[2], float vSpeed, float vMin, float vMax, const char* format)
{
    return SchematicDragFloatN<2>(id, v, vSpeed, vMin, vMax, format);
}

bool SchematicDragFloat3(const char* id, float v[3], float vSpeed, float vMin, float vMax, const char* format)
{
    return SchematicDragFloatN<3>(id, v, vSpeed, vMin, vMax, format);
}

bool SchematicDragFloatAxis(const char* id, float* v, int axis, float vSpeed, float vMin, float vMax,
                            const char* format)
{
    axis = ImClamp(axis, 0, 2);
    const bool changed = ImGui::DragFloat(id, v, vSpeed, vMin, vMax, "");
    const float stampW = OverlayAxisStamp(kAxisLabels[axis], kAxisColors[axis]);
    if (format && format[0])
    {
        char buf[64];
        ImFormatString(buf, IM_ARRAYSIZE(buf), format, *v);
        OverlayLeftAlignedValue(buf, stampW);
    }
    return changed;
}

// Shared implementation for the 3- and 4-channel color editors. `N` is 3 or 4.
static bool SchematicColorEditN(const char* id, float* col, int n)
{
    // Layout: N channel cells and a square swatch on the right (frame height).
    // Each channel is a 1px bordered cell with a 3px color stripe on the left
    // and a SchematicDragInt (0-255) in the rest. The swatch opens ImGui's
    // full color picker.
    const float totalW = ImGui::CalcItemWidth();
    const float frameH = ImGui::GetFrameHeight();
    const float swatchW = frameH;  // square swatch matches row height
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    // Tight gaps and cell padding: at the Inspector's default width, four
    // cells with the theme's padding cut "255" to "25".
    const float gap = 3.0f * dpi;
    const float channelsW = totalW - swatchW - gap;
    const float channelW = ImMax(20.0f * dpi, (channelsW - gap * (float)(n - 1)) / (float)n);
    const float stripeW = 3.0f * dpi;

    bool changed = false;

    ImGui::PushID(id);
    ImGui::BeginGroup();

    const ImU32 channelStripeCols[4] = {
        IM_COL32(232, 65, 24, 255),    // R
        IM_COL32(76, 209, 55, 255),    // G
        IM_COL32(64, 153, 255, 255),   // B
        IM_COL32(140, 140, 140, 255),  // A
    };

    for (int ch = 0; ch < n; ++ch)
    {
        if (ch > 0)
        {
            ImGui::SameLine(0.0f, gap);
        }

        const ImVec2 cellPos = ImGui::GetCursorScreenPos();
        const float cellH = ImGui::GetFrameHeight();
        const ImVec2 cellMax(cellPos.x + channelW, cellPos.y + cellH);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        dl->AddRectFilled(cellPos, ImVec2(cellPos.x + stripeW, cellMax.y), channelStripeCols[ch]);

        // The 0-255 value, past the stripe.
        ImGui::PushID(ch);
        ImGui::SetCursorScreenPos(ImVec2(cellPos.x + stripeW, cellPos.y));
        ImGui::SetNextItemWidth(channelW - stripeW);
        int v = (int)ImClamp(col[ch] * 255.0f + 0.5f, 0.0f, 255.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * dpi, ImGui::GetStyle().FramePadding.y));
        if (SchematicDragInt("##v", &v, 1.0f, 0, 255, "%d"))
        {
            col[ch] = (float)v / 255.0f;
            changed = true;
        }
        ImGui::PopStyleVar();
        ImGui::PopID();
    }

    // The color swatch on the right; a click opens the full picker.
    ImGui::SameLine(0.0f, gap);
    const ImVec2 swatchPos = ImGui::GetCursorScreenPos();
    const float swatchH = ImGui::GetFrameHeight();
    const ImVec2 swatchMax(swatchPos.x + swatchW, swatchPos.y + swatchH);

    ImGui::InvisibleButton("##swatch", ImVec2(swatchW, swatchH));
    const bool swatchHovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
    {
        ImGui::OpenPopup("##colorpicker");
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Round the swatch to match the themed frames, clamped to half its size.
    const float swatchRounding = ImMin(ImGui::GetStyle().FrameRounding, swatchW * 0.5f);
    dl->AddRectFilled(swatchPos, swatchMax, ImGui::ColorConvertFloat4ToU32(ImVec4(col[0], col[1], col[2], 1.0f)),
                      swatchRounding);
    dl->AddRect(swatchPos, swatchMax, ImGui::ColorConvertFloat4ToU32(swatchHovered ? Palette::Line3 : Palette::Line2),
                swatchRounding);

    // A fixed picker width, and the popup under the swatch but pulled left so
    // it ends inside the editor window: the Inspector sits at the right edge,
    // and the picker would otherwise run off it.
    const float pickerDpi = ImGui::GetStyle().FontScaleDpi;
    const float pickerW = 240.0f * pickerDpi;
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float popupW = pickerW + ImGui::GetStyle().WindowPadding.x * 2.0f + 24.0f * pickerDpi;
        const float maxX = vp->WorkPos.x + vp->WorkSize.x - popupW - 8.0f * pickerDpi;
        ImGui::SetNextWindowPos(ImVec2(ImMin(swatchPos.x, maxX), swatchMax.y + 4.0f * pickerDpi), ImGuiCond_Appearing);
    }
    if (ImGui::BeginPopup("##colorpicker"))
    {
        ImGui::SetNextItemWidth(pickerW);
        if (n >= 4)
        {
            if (ImGui::ColorPicker4("##pick", col, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview))
            {
                changed = true;
            }
        }
        else
        {
            if (ImGui::ColorPicker3("##pick", col, ImGuiColorEditFlags_NoSidePreview))
            {
                changed = true;
            }
        }
        ImGui::EndPopup();
    }

    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

bool SchematicColorEdit4(const char* id, float col[4])
{
    return SchematicColorEditN(id, col, 4);
}
bool SchematicColorEdit3(const char* id, float col[3])
{
    return SchematicColorEditN(id, col, 3);
}

bool SchematicBeginCombo(const char* id, const char* preview)
{
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems)
    {
        return false;
    }

    const float dpi = ImGui::GetStyle().FontScaleDpi;

    // Take the combo box rect from the cursor before BeginCombo: once the
    // popup opens, ImGui's last-item rect is the popup window. The preview and
    // chevron are anchored to this box and drawn on the parent window's draw
    // list, so they stay put whether the dropdown is open or shut.
    const ImVec2 boxMin = ImGui::GetCursorScreenPos();
    const ImVec2 boxMax(boxMin.x + ImGui::CalcItemWidth(), boxMin.y + ImGui::GetFrameHeight());

    // The theme zeroes WindowPadding (panels manage their own); give the
    // dropdown popup a real inset. It is read at the popup's Begin() inside
    // BeginCombo, so it is popped right after.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * dpi, 6.0f * dpi));

    // The list's own fade in and out is the editor-wide popup motion (the main
    // loop's PopupMotion); here only the chevron turns with it. The id is taken
    // here because it is the one BeginCombo derives from the same string, and
    // once the popup is up the "last item" is no longer this box.
    const ImGuiID comboId = ImGui::GetID(id);
    // The popup's own id, derived the way BeginCombo derives it. Asking about
    // comboId instead always answers "shut".
    const bool popupOpen = ImGui::IsPopupOpen(ImHashStr("##ComboPopup", 0, comboId), 0);
    const float openT = AnimateTo(comboId, popupOpen ? 1.0f : 0.0f, 0.10f);
    // Only when a popup will actually be begun: an unconsumed next-window value
    // would leak onto whatever window or child opens after this one.
    if (popupOpen)
    {
        // Show every row, up to most of the window, instead of ImGui's eight: a
        // list that opens scrolled to its selection hides its first entries.
        // ImGui still widens it to the box.
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                            ImVec2(FLT_MAX, ImGui::GetMainViewport()->WorkSize.y * 0.6f));
    }

    // Empty preview and no native arrow: the preview value (left-aligned
    // mono, like the drag fields) and the chevron are drawn here, to match
    // the rest of the Inspector.
    const bool open = ImGui::BeginCombo(id, "", ImGuiComboFlags_NoArrowButton);
    ImGui::PopStyleVar();  // WindowPadding (popup already baked it)

    if (open)
    {
        // Taller, padded option rows with a full-width hover highlight. A
        // Selectable's box extends by ItemSpacing.x/2 on each side, and ImGui
        // sets the combo popup's WindowPadding.x to FramePadding.x, so
        // ItemSpacing.x of twice that takes the highlight exactly to both
        // window edges while the label keeps its inset (as in the menu
        // dropdowns). Popped in SchematicEndCombo.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f * ImGui::GetStyle().FramePadding.x, 8.0f * dpi));
    }

    if (preview && preview[0])
    {
        const float chevronW = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x;
        DrawLeftAlignedValue(w->DrawList, boxMin, ImVec2(ImMax(boxMin.x, boxMax.x - chevronW), boxMax.y), preview);
    }

    // Right-aligned chevron, like the section header's and turning like it:
    // down when the list is shut, up while it is open.
    {
        ImFont* ico = g_FontUI ? g_FontUI : ImGui::GetFont();
        const float icoSize = ImGui::GetFontSize();
        const ImVec2 cs = ico->CalcTextSizeA(icoSize, FLT_MAX, 0.0f, ICON_TI_CHEVRON_DOWN);
        const float pad = ImGui::GetStyle().FramePadding.x;
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(Palette::Dim);
        DrawTurningChevron(w->DrawList, ico, icoSize,
                           ImVec2(boxMax.x - pad - cs.x * 0.5f, (boxMin.y + boxMax.y) * 0.5f), col,
                           ICON_TI_CHEVRON_DOWN, openT, IM_PI);
    }

    return open;
}

void SchematicEndCombo()
{
    ImGui::PopStyleVar();  // ItemSpacing pushed in SchematicBeginCombo
    ImGui::EndCombo();
}

// A flat, toolbar-styled box: transparent frame, subtle white hover (the same
// alphas as the toolbar cells), no border, square. FramePadding.y makes the
// box exactly `height` tall, like the toolbar strip, with the preview value
// and chevron centered. The styles stay pushed while the list is open and
// are popped by SchematicEndToolbarCombo.
static void PushToolbarComboStyle(float height)
{
    const float fontH = ImGui::GetFontSize();
    const float padY = ImMax(0.0f, (height - fontH) * 0.5f);
    const float padX = ImGui::GetStyle().FramePadding.x;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 6));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(255, 255, 255, 10));
}

static void PopToolbarComboStyle()
{
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

bool SchematicBeginToolbarCombo(const char* id, const char* preview, float height, bool leftDivider)
{
    // Anchor for the optional left-edge divider (the box is drawn at the cursor).
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImVec2 boxMin = ImGui::GetCursorScreenPos();

    PushToolbarComboStyle(height);
    const bool open = SchematicBeginCombo(id, preview);

    // Left-edge divider (matching the toolbar cell separators), drawn on the
    // parent window's draw list so it shows whether or not the popup is open.
    if (leftDivider && w)
    {
        w->DrawList->AddLine(ImVec2(boxMin.x, boxMin.y), ImVec2(boxMin.x, boxMin.y + height),
                             ImGui::ColorConvertFloat4ToU32(Palette::Line2), 1.0f);
    }

    if (!open)
    {
        PopToolbarComboStyle();
    }
    return open;
}

void SchematicEndToolbarCombo()
{
    SchematicEndCombo();
    PopToolbarComboStyle();
}

float SchematicToolbarComboWidth(const char* preview)
{
    // The preview in the mono value font (as DrawLeftAlignedValue draws it),
    // the frame padding each side, and the chevron with a gap before it.
    ImFont* font = g_FontMono ? g_FontMono : ImGui::GetFont();
    const float fontSize = Fonts::CssPx(font, 12.5f) * ImGui::GetStyle().FontScaleDpi;
    const float textW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, preview ? preview : "").x;
    const float padX = ImGui::GetStyle().FramePadding.x;
    return std::ceil(textW + padX * 3.0f + ImGui::GetFontSize());
}

bool SchematicToolbarCombo(const char* id, int* currentIndex, const char* items, float height, bool leftDivider)
{
    if (!currentIndex || !items)
    {
        return false;
    }

    // Parse the "A\0B\0" zero-separated item list.
    const char* item[64];
    int count = 0;
    for (const char* p = items; p && *p && count < 64; p += strlen(p) + 1)
    {
        item[count++] = p;
    }
    if (count == 0)
    {
        return false;
    }

    const char* preview = (*currentIndex >= 0 && *currentIndex < count) ? item[*currentIndex] : "";

    bool changed = false;
    if (SchematicBeginToolbarCombo(id, preview, height, leftDivider))
    {
        for (int i = 0; i < count; ++i)
        {
            const bool selected = (i == *currentIndex);
            if (ImGui::Selectable(item[i], selected, ImGuiSelectableFlags_SpanAvailWidth) && *currentIndex != i)
            {
                *currentIndex = i;
                changed = true;
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        SchematicEndToolbarCombo();
    }
    return changed;
}

bool SchematicCombo(const char* id, int* currentIndex, const char* const items[], int count)
{
    if (!currentIndex || !items || count <= 0)
    {
        return false;
    }

    const char* preview =
        (*currentIndex >= 0 && *currentIndex < count && items[*currentIndex]) ? Tr(items[*currentIndex]) : "";

    bool changed = false;
    if (SchematicBeginCombo(id, preview))
    {
        for (int i = 0; i < count; ++i)
        {
            const bool selected = (i == *currentIndex);
            // SpanAvailWidth, so the hover/selected highlight fills the row
            // (with the ItemSpacing pushed in SchematicBeginCombo, it reaches
            // both popup edges).
            if (ImGui::Selectable(items[i] ? TrLabel(items[i]) : "", selected, ImGuiSelectableFlags_SpanAvailWidth) &&
                *currentIndex != i)
            {
                *currentIndex = i;
                changed = true;
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        SchematicEndCombo();
    }
    return changed;
}

bool SchematicCombo(const char* id, int* currentIndex, const char* itemsSeparatedByZeros)
{
    // Parse a "Item1\0Item2\0\0" zero-separated string into a pointer array.
    const char* items[64];
    int count = 0;
    for (const char* p = itemsSeparatedByZeros; p && *p && count < 64; p += strlen(p) + 1)
    {
        items[count++] = p;
    }
    return SchematicCombo(id, currentIndex, items, count);
}

bool SchematicActionLink(const char* label)
{
    // A right-aligned accent link with a 1px underline, on the right edge of
    // the header band just before it (call it right after the header). Only
    // the text before "##id" is drawn; the full string is the button id. The
    // hit box spans the band height, so the layout below does not move.
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems)
    {
        return false;
    }

    // The preceding item is the header band (nothing else added between).
    const ImVec2 bandMin = ImGui::GetItemRectMin();
    const ImVec2 bandMax = ImGui::GetItemRectMax();
    const float bandH = bandMax.y - bandMin.y;

    // Visible text stops at the "##" id marker.
    const char* visEnd = label;
    while (visEnd[0] != '\0' && !(visEnd[0] == '#' && visEnd[1] == '#'))
    {
        ++visEnd;
    }
    const ImVec2 sz = ImGui::CalcTextSize(label, visEnd);

    // Anchor to the right content edge (before the scrollbar), one frame
    // padding in from the seam.
    const float pad = ImGui::GetStyle().FramePadding.x;
    const float x = w->WorkRect.Max.x - sz.x - pad;

    ImGui::SetCursorScreenPos(ImVec2(x, bandMin.y));
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(sz.x, bandH));
    const bool hovered = ImGui::IsItemHovered();

    const ImU32 col = ImGui::ColorConvertFloat4ToU32(hovered ? Palette::Accent : Palette::AccentDim);
    ImDrawList* dl = w->DrawList;
    const float textY = ImFloor(bandMin.y + (bandH - sz.y) * 0.5f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, textY), col, label, visEnd);
    dl->AddLine(ImVec2(x, textY + sz.y), ImVec2(x + sz.x, textY + sz.y), col, 1.0f);
    return clicked;
}

}  // namespace DekiEditor
