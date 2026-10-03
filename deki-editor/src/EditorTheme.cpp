// EditorTheme - Schematic / datasheet aesthetic.
// Near-black backgrounds, square 1px hairline borders, steel-blue accent.

#include <deki-editor/EditorTheme.h>
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
// SNAPS to its target: a panel that has just been opened should show its
// state, not animate every foldout in it from shut.
// ---------------------------------------------------------------------------
namespace
{
// Defined below, next to the table it prunes.
void PruneSectionHeights();

float AnimateTo(ImGuiID id, float target, float seconds)
{
    // The section-height table outlives frames too; sweep it on the same
    // rhythm Motion sweeps its own.
    if ((ImGui::GetFrameCount() & 255) == 0)
        PruneSectionHeights();
    return Motion::To(id, target, seconds);
}

// Ease in-out, so the motion starts and lands softly instead of stopping dead.
float Smooth(float t)
{
    return Motion::Smooth(t);
}

// Rotate everything a draw call appended since `vtxStart` around its own
// center. ImGui cannot draw rotated text, so the glyph is emitted normally
// and its vertices are spun afterwards - the icon stays the icon, at any
// angle, instead of being redrawn as hand-built strokes that would not
// match the ones next to it.
void RotateVerticesFrom(ImDrawList* dl, int vtxStart, float rad)
{
    if (rad == 0.0f || dl->VtxBuffer.Size <= vtxStart)
        return;

    ImVec2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
    for (int i = vtxStart; i < dl->VtxBuffer.Size; ++i)
    {
        lo = ImMin(lo, dl->VtxBuffer[i].pos);
        hi = ImMax(hi, dl->VtxBuffer[i].pos);
    }
    const ImVec2 center((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);

    // (No ImVec2 operators in this TU - IMGUI_DEFINE_MATH_OPERATORS is not
    // set - so the rotation is written out by hand.)
    // ImRotate takes COSINE first, then sine. Passing them the other way
    // round rotates by (90° - angle), which turns the icon backwards: fully
    // open it landed exactly where it started.
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

// Nudge everything appended since `vtxStart` so its bounding box starts on
// whole pixels. A glyph landing on a half pixel samples between texels and
// comes out soft and thick - which is the whole difference between a
// chevron that looks drawn and one that looks smeared. Only worth doing
// when the icon is at rest; snapping every frame of the turn would make it
// jitter by half a pixel as it goes.
void SnapVerticesFrom(ImDrawList* dl, int vtxStart)
{
    if (dl->VtxBuffer.Size <= vtxStart)
        return;

    ImVec2 lo(FLT_MAX, FLT_MAX);
    for (int i = vtxStart; i < dl->VtxBuffer.Size; ++i)
    {
        lo.x = ImMin(lo.x, dl->VtxBuffer[i].pos.x);
        lo.y = ImMin(lo.y, dl->VtxBuffer[i].pos.y);
    }
    const float dx = ImFloor(lo.x + 0.5f) - lo.x;
    const float dy = ImFloor(lo.y + 0.5f) - lo.y;
    if (dx == 0.0f && dy == 0.0f)
        return;
    for (int i = vtxStart; i < dl->VtxBuffer.Size; ++i)
    {
        dl->VtxBuffer[i].pos.x += dx;
        dl->VtxBuffer[i].pos.y += dy;
    }
}

// A chevron that turns instead of being swapped for a different glyph.
// `openT` 0 = closed (as authored), 1 = fully turned by `sweepRad`.
//
// `center` is the middle of the zone the icon lives in, not a pen position:
// the glyph is centered on it by its own metrics, so the icon does not
// wander as it turns. The one glyph is used at every angle - swapping in
// chevron-down at the end would land it wherever THAT glyph's ink sits,
// which is not where the turning one arrives.
void DrawTurningChevron(ImDrawList* dl, ImFont* font, float size, ImVec2 center,
                        ImU32 col, const char* glyph, float openT, float sweepRad)
{
    const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, glyph);
    const ImVec2 pos(ImFloor(center.x - sz.x * 0.5f), ImFloor(center.y - sz.y * 0.5f));

    const int start = dl->VtxBuffer.Size;
    dl->AddText(font, size, pos, col, glyph);

    const bool atRest = (openT <= 0.0f || openT >= 1.0f);
    RotateVerticesFrom(dl, start, Smooth(openT) * sweepRad);
    if (atRest)
        SnapVerticesFrom(dl, start);
}

// ---- animated section bodies -------------------------------------------
//
// The body is clipped to an animated fraction of its own full height, and
// the layout below is pulled up to the clipped height. Deliberately NOT a
// child window, which would have been less code: a child is its own ImGui
// id scope and its own storage, so every nested foldout inside a section
// would read as shut for the length of the animation and every id inside
// would change identity mid-interaction. Clipping in place keeps the body
// exactly the widgets it already was - ItemAdd treats anything outside the
// clip as culled, so the hidden part stops being hoverable too.
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
        it->second.frame = ImGui::GetFrameCount();
}

// The body's animation is keyed apart from the chevron's: same widget, two
// motions, and they do not share a duration.
ImGuiID SectionBodyKey(ImGuiID headerId)
{
    return headerId ^ 0x5EC7B0D1u;
}

// One description of a section band's title type, so the drawn label and
// the field that edits it cannot drift apart.
//
// This band names a THING, so it is typeset like the object name in the
// Inspector header: display face, heading size, accent. The uppercase
// letter-spaced treatment belongs to the foldouts, which name sections -
// and could never be matched by a field, since nobody types in caps.
ImFont* BandTitleFont()
{
    return g_FontDisplay ? g_FontDisplay : ImGui::GetFont();
}

// Two sizes for one type, because the two ImGui entry points disagree about
// what a size is: PushFont takes a BASE size and multiplies it by
// FontScaleDpi at draw time, while ImDrawList::AddText takes the final
// pixel size. Feeding one to the other is why the field came out a whole
// DPI factor larger than the label it replaced.
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
// A cursor advance, NOT an item: an item here would become the window's
// last one, and a caller that hangs something off the band by reading the
// previous item's rect - the `EDIT ->` link on a component header does
// exactly that - would measure this spacer instead and drop its control a
// band lower. The body's height is measured from the cursor either way, so
// the gap is still counted. SchematicSectionEnd closes the body with a zero
// item so a section whose body is empty still cannot end a window on a
// bare cursor write.
void SectionBodyGap()
{
    const float gap = ImGui::GetStyle().ItemSpacing.y;
    if (gap <= 0.0f)
        return;
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y + gap));
}

// Measured heights outlive the frame like the animations do, and a big
// project's asset tree has plenty of nodes, so any not measured for a few
// seconds (a closed panel, a node scrolled away) are dropped. Called from
// AnimateTo.
void PruneSectionHeights()
{
    if (s_SectionHeights.size() <= 512)
        return;
    const int frame = ImGui::GetFrameCount();
    for (auto i = s_SectionHeights.begin(); i != s_SectionHeights.end();)
        i = (frame - i->second.frame > 600) ? s_SectionHeights.erase(i) : ++i;
}
}  // namespace

void Tooltip(const char* fmt, ...)
{
    ImGuiContext& g = *GImGui;
    // Either measure says "resting": the item timer for anything ImGui knows as
    // an item, the stationary timer for tooltips hung off hand-drawn regions
    // (canvas pins, toolbar cells) where there is no hovered id to time.
    if (g.HoveredIdTimer < TooltipDelaySeconds &&
        g.MouseStationaryTimer < TooltipDelaySeconds)
        return;

    va_list args;
    va_start(args, fmt);
    ImGui::SetTooltipV(fmt, args);
    va_end(args);
}

bool SchematicSectionBegin(const char* label, ImGuiTreeNodeFlags flags, ImGuiID* outHeaderId)
{
    if (outHeaderId)
        *outHeaderId = 0;

    const bool open = SchematicCollapsingHeader(label, flags);

    // Bail before touching any animation state: with SkipItems set the header
    // submitted no item, so GetItemID would name whatever came before it and
    // this would drive that widget's animation instead.
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems)
        return false;

    const ImGuiID id = ImGui::GetItemID();
    const ImGuiID key = SectionBodyKey(id);
    if (outHeaderId)
        *outHeaderId = id;  // set even when the section is shut

    const float t = AnimateTo(key, open ? 1.0f : 0.0f, 0.16f);
    KeepSectionHeight(id);
    if (t <= 0.0f)
        return false;  // fully shut: nothing to submit, nothing to end

    SectionScope sc;
    sc.id = id;
    sc.active = true;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    sc.startX = cursor.x;
    sc.startY = cursor.y;

    auto measured = s_SectionHeights.find(id);
    if (t >= 1.0f)
    {
        // At rest: submitted exactly as it always was, and measured on the way
        // out. That measurement is what every later open or close animates
        // against.
        sc.clipped = false;
        SectionBodyGap();
        s_SectionStack.push_back(sc);
        return true;
    }

    // Opening for the very first time, with no height on record yet: submit the
    // body clipped to nothing. Clipping culls drawing and hit-testing but does
    // NOT change layout, so the cursor still travels the body's full height and
    // End can measure it - the section is invisible for the one frame it takes
    // to learn its own size, and animates properly from the next. The
    // alternative was to show it at full height and then animate it from a
    // fifth of that, which is why the first click used to just snap open.
    sc.clipped = true;
    sc.visibleH = (measured == s_SectionHeights.end())
                      ? 0.0f
                      : ImMax(0.0f, measured->second.height * Smooth(t));
    sc.savedMaxY = w->DC.CursorMaxPos.y;
    // Clip from the top of the BAND, not the top of the body: whatever a caller
    // draws on the band right after the header (the `EDIT ->` link) is drawn
    // after this point and would otherwise be clipped away for the length of
    // the animation. The band itself is already painted, so including its row
    // costs nothing.
    const float clipTop = ImMin(ImGui::GetItemRectMin().y, sc.startY);
    ImGui::PushClipRect(ImVec2(w->ClipRect.Min.x, clipTop),
                        ImVec2(w->ClipRect.Max.x, sc.startY + sc.visibleH), true);
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

    // Closing: ImGui pushed nothing this frame because as far as it is
    // concerned the node is shut. Push the same indent and the same id it would
    // have, so the children that are still being drawn sit where they sat.
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
    sc.visibleH = (measured == s_SectionHeights.end())
                      ? 0.0f
                      : ImMax(0.0f, measured->second.height * Smooth(t));
    sc.savedMaxY = w->DC.CursorMaxPos.y;
    ImGui::PushClipRect(ImVec2(w->ClipRect.Min.x, sc.startY),
                        ImVec2(w->ClipRect.Max.x, sc.startY + sc.visibleH), true);
    s_SectionStack.push_back(sc);
    return true;
}

void SchematicTreeBodyEnd()
{
    IM_ASSERT(!s_SectionStack.empty() && "SchematicTreeBodyEnd without a Begin");
    if (s_SectionStack.empty())
        return;

    // Nothing was submitted (shut, or a skipped window): drop the marker only.
    if (!s_SectionStack.back().active)
    {
        s_SectionStack.pop_back();
        return;
    }

    const bool pushed = s_SectionStack.back().treePushed;
    SchematicSectionEnd();  // same clip / measure / compress
    if (pushed)
        ImGui::TreePop();
}

void SchematicSectionEnd()
{
    IM_ASSERT(!s_SectionStack.empty() && "SchematicSectionEnd without a Begin that returned true");
    if (s_SectionStack.empty())
        return;

    const SectionScope sc = s_SectionStack.back();
    s_SectionStack.pop_back();

    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w)
        return;

    // The body was submitted whole either way - a clip hides items, it does not
    // shorten the layout - so this is the full height whatever the state, and
    // recording it every frame keeps the animation honest when the content
    // itself changes size.
    const float h = ImGui::GetCursorScreenPos().y - sc.startY;
    if (h > 0.0f)
        s_SectionHeights[sc.id] = SectionHeight{ h, ImGui::GetFrameCount() };

    if (!sc.clipped)
    {
        // Close on a real (zero-size) item: the body may have submitted nothing
        // at all after SectionBodyGap moved the cursor, and a window that ends
        // on a bare cursor write trips ImGui's boundary check at End().
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopStyleVar();
        return;
    }

    ImGui::PopClipRect();

    // Pull the layout up to the visible height. Closing with a zero item rather
    // than a bare cursor write: a section that ends a window would otherwise
    // leave the cursor "set" and trip ImGui's check at End().
    ImGui::SetCursorScreenPos(ImVec2(sc.startX, sc.startY + sc.visibleH));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopStyleVar();

    // The body extended the window's content height to its FULL size on its way
    // past; the window only contains the visible part right now, and a
    // scrollbar that grows and shrinks by the body's height every time one
    // section is toggled is worse than no animation at all.
    w->DC.CursorMaxPos.y = ImMax(sc.savedMaxY, sc.startY + sc.visibleH);
}

namespace Fonts
{
void RegisterTtf(ImFont* font, const char* ttfPath)
{
    if (!font || !ttfPath) return;
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) return;
    FT_Face face = nullptr;
    if (FT_New_Face(lib, ttfPath, 0, &face) == 0)
    {
        // ratio = (ascender + |descender|) / unitsPerEm
        // This is exactly the factor FT_SIZE_REQUEST_TYPE_REAL_DIM divides
        // by when interpreting our requested pixel_size. Multiplying CSS
        // px by this ratio gives the pixel_size we need to ask FreeType
        // for so the rendered em-square matches CSS semantics.
        const float upm = (float)face->units_per_EM;
        const float ascDesc = (float)(face->ascender - face->descender);
        if (upm > 0.0f)
            s_FontCssRatios[font] = ascDesc / upm;

        FT_Fixed advance = 0;
        const FT_UInt zero = FT_Get_Char_Index(face, '0');
        if (upm > 0.0f && FT_IS_FIXED_WIDTH(face) && zero != 0 &&
            FT_Get_Advance(face, zero, FT_LOAD_NO_SCALE, &advance) == 0 && advance > 0)
            s_FontMonoAdvance[font] = (float)advance / upm;
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
            cssPx = cell / adv->second;
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
        ImGui::PopStyleVar();
    return open;
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
        return -1;

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
            ImGui::SameLine(0.0f, gap);
        int colours = 0;
        if (b.kind != DialogButton::Normal)
        {
            // Pushed colours skip PressMotion's easing, so these buttons ease
            // their own press: held since last frame -> toward the pressed colour.
            const ImGuiID id = ImGui::GetID(b.label);
            const bool held = ImGui::GetActiveID() == id;
            const float t = Motion::EaseOut(
                Motion::To(id ^ 0x9E55EDu, held ? 1.0f : 0.0f, held ? Motion::Fast : Motion::Normal));
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
        if (ImGui::Button(b.label, ImVec2(width, 0.0f)))
            clicked = i;
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
    // dpiScale is only used for the one-shot style.ScaleAllSizes() call below.
    // All draw-time custom-widget code reads ImGui::GetFontSize() / FontScaleDpi
    // directly per the ImGui 1.92 FAQ "express dimensions as multiples of
    // reference values" guidance.
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();

    // Corner rounding. Windows, docked children and tabs stay square: rounding a
    // docked window breaks the shared dock seams, and the tab bar draws its own
    // square active-tab underline (DrawPanelTabUnderline) that would poke past
    // rounded tab corners. Everything else the user actually clicks into (frames
    // = buttons / input fields / drag fields / combos, popups, scrollbars and
    // grabs) gets a small radius so the widgets read a touch softer without
    // losing the schematic feel. Value is in logical px; ScaleAllSizes() below
    // multiplies it by dpiScale. Draw-time custom widgets read it back off
    // g.Style.FrameRounding (already DPI-scaled) so they stay in sync.
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
    // Docked panels deliberately have NO window border (WindowBorderSize = 0).
    // The single line you see between two docked panels is ImGui's own dock
    // separator: SplitterBehavior() fills the DockingSeparatorSize-wide gap with
    // ImGuiCol_Separator, which the docking code sets to ImGuiCol_Border. If we
    // also let each panel stroke its own 1px outer border, the seam doubled up
    // (border + separator + border). With the border off and the separator
    // pinned to 1px below, every shared seam is a single crisp hairline.
    // (DrawDockSeams() then only re-adds borders for standalone floating
    // windows, e.g. dialogs, which would otherwise lose theirs.)
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
    // FramePadding.y of 10 puts dock tab bar height at ~font(13) + 20 = 33px,
    // close to the mockup's 34px target. Side effect: every framed widget
    // (buttons, drag fields) also gets taller, which suits the mockup's
    // spacious feel.
    // FramePadding.x = 14: side padding on tabs (left of label, right of X).
    // Tab RequestedWidth formula uses this 2x, so each tab gets ~12 more
    // logical px versus FramePadding.x=8. Other framed widgets (buttons,
    // drag fields) also gain a few px on each side.
    style.FramePadding = ImVec2(14, 10);
    style.ItemSpacing = ImVec2(8, 4);
    style.ItemInnerSpacing = ImVec2(4, 4);
    style.ScrollbarSize = 8.0f;  // uniform thin scrollbars across all panels
    style.GrabMinSize = 8.0f;

    // Left-align text inside buttons / selectables / tables so asset
    // pickers, header buttons, and list rows read as left-aligned values
    // (matches the schematic mockup; ImGui's default is centered).
    style.ButtonTextAlign = ImVec2(0.0f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);

    // FontSizeBase is the default size used by PushFont(NULL, ...) and is
    // multiplied by FontScaleMain × FontScaleDpi at draw time. A sensible
    // fallback (CSS 13 × Inter's typical ratio) - main.cpp overrides this
    // to the precise value via Fonts::CssPx(g_FontUI, 13) once fonts load.
    style.FontSizeBase = 13.0f;
    style.FontScaleMain = 1.0f;
    // Initial FontScaleDpi - io.ConfigDpiScaleFonts only AUTO-OVERWRITES this
    // on monitor-DPI-change events, not at startup. Without this line, fonts
    // would render at 1.0× even on a 2.0× display until the user dragged the
    // window between monitors. All ImGui's official examples do the same.
    style.FontScaleDpi = dpiScale;

    // ScaleAllSizes is still required in 1.92 - ImGui's auto-DPI handles fonts
    // but explicitly NOT sizes/padding. Calling here scales the style fields
    // we just set above by dpiScale, so frame padding / item spacing / etc.
    // match the font scale at startup.
    style.ScaleAllSizes(dpiScale);

    ImVec4* colors = style.Colors;

    const ImVec4& Bg = Palette::Bg;
    const ImVec4& Bg2 = Palette::Bg2;
    const ImVec4& Bg3 = Palette::Bg3;
    const ImVec4& Panel = Palette::Panel;
    const ImVec4& Line = Palette::Line;
    const ImVec4& Line2 = Palette::Line2;
    const ImVec4& Fg = Palette::Fg;
    const ImVec4& Dim = Palette::Dim;
    const ImVec4& Accent = Palette::Accent;

    const ImVec4 AccentSubtle = ImVec4(Accent.x, Accent.y, Accent.z, 0.12f);
    const ImVec4 AccentFaint = ImVec4(Accent.x, Accent.y, Accent.z, 0.08f);

    // Text
    colors[ImGuiCol_Text] = Fg;
    colors[ImGuiCol_TextDisabled] = Dim;

    // Surfaces
    colors[ImGuiCol_WindowBg] = Bg;
    colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_PopupBg] = Panel;
    colors[ImGuiCol_Border] = Line2;
    colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

    // Input fields
    colors[ImGuiCol_FrameBg] = Bg3;
    colors[ImGuiCol_FrameBgHovered] = Bg2;
    colors[ImGuiCol_FrameBgActive] = Bg2;

    // Title bars / menus
    colors[ImGuiCol_TitleBg] = Panel;
    colors[ImGuiCol_TitleBgActive] = Panel;
    colors[ImGuiCol_TitleBgCollapsed] = Panel;
    colors[ImGuiCol_MenuBarBg] = Panel;

    // Scrollbars
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ScrollbarGrab] = Line2;
    colors[ImGuiCol_ScrollbarGrabHovered] = Palette::Line3;
    colors[ImGuiCol_ScrollbarGrabActive] = Dim;

    // Controls
    colors[ImGuiCol_CheckMark] = Accent;
    colors[ImGuiCol_SliderGrab] = Palette::AccentDim;
    colors[ImGuiCol_SliderGrabActive] = Accent;
    colors[ImGuiCol_Button] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ButtonHovered] = Bg2;
    colors[ImGuiCol_ButtonActive] = Bg;

    // Headers (selectable rows, tree nodes, collapsing headers)
    colors[ImGuiCol_Header] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_HeaderHovered] = AccentFaint;
    colors[ImGuiCol_HeaderActive] = AccentSubtle;

    // Separators
    colors[ImGuiCol_Separator] = Line;
    colors[ImGuiCol_SeparatorHovered] = Accent;
    colors[ImGuiCol_SeparatorActive] = Accent;

    // Resize grips
    colors[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ResizeGripHovered] = Palette::AccentDim;
    colors[ImGuiCol_ResizeGripActive] = Accent;

    // Tabs (panel headers) - active draws with Bg, inactive with Panel.
    // Active tab gets a visual underline drawn per-panel.
    colors[ImGuiCol_Tab] = Panel;
    colors[ImGuiCol_TabHovered] = Bg;
    colors[ImGuiCol_TabActive] = Bg;
    colors[ImGuiCol_TabUnfocused] = Panel;
    colors[ImGuiCol_TabUnfocusedActive] = Bg;
    // ImGui draws a 2px "overline" along the TOP of the selected dock tab
    // (ImGuiTabBarFlags_DrawSelectedOverline, hardcoded on for dock nodes).
    // We already paint our own accent underline at the bottom in RestyleTabRow,
    // and the native overline overdraws the top of the close-(X) button. Make
    // both overline colors fully transparent so the native code draws nothing.
    colors[ImGuiCol_TabSelectedOverline] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);

    // Docking
    colors[ImGuiCol_DockingPreview] = Palette::AccentDim;
    colors[ImGuiCol_DockingEmptyBg] = Bg;

    // Tables
    colors[ImGuiCol_TableHeaderBg] = Panel;
    colors[ImGuiCol_TableBorderStrong] = Line2;
    colors[ImGuiCol_TableBorderLight] = Line;
    colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.018f);

    // Selection / drag / nav
    colors[ImGuiCol_TextSelectedBg] = AccentSubtle;
    colors[ImGuiCol_DragDropTarget] = Accent;
    colors[ImGuiCol_NavHighlight] = Accent;
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.7f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.5f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
}

// ----------------------------------------------------------------------------
// Schematic chrome helpers
// ----------------------------------------------------------------------------

// Draw `text` with `letterSpacing` extra pixels between glyphs at the given
// font size. Walks UTF-8 codepoints; uses CalcTextSizeA for per-char advance
// (kerning ignored - fine for short uppercase labels). Returns total width.
static float DrawSpacedText(ImDrawList* dl, ImFont* font, float fontSize,
                            ImVec2 pos, ImU32 col, const char* text,
                            float letterSpacing)
{
    if (!font || !text || !*text)
        return 0.0f;
    float x = pos.x;
    const char* s = text;
    while (*s)
    {
        unsigned int c;
        int bytes = ImTextCharFromUtf8(&c, s, nullptr);
        if (bytes <= 0) break;
        const char* glyphBegin = s;
        const char* glyphEnd = s + bytes;
        dl->AddText(font, fontSize, ImVec2(x, pos.y), col, glyphBegin, glyphEnd);
        const float advance = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, glyphBegin, glyphEnd).x;
        x += advance + letterSpacing;
        s += bytes;
    }
    return x - pos.x;
}

// Convert ASCII text to uppercase in `out` (size cap inside). Non-ASCII bytes
// pass through unchanged (Tabler icons sit in PUA range so they're unaffected).
static void Upper(const char* in, char* out, size_t outCap)
{
    size_t i = 0;
    while (in[i] && i + 1 < outCap)
    {
        char c = in[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i] = c;
        ++i;
    }
    out[i] = 0;
}

// Repaint one tab's label area with the tab background, then redraw the label
// in g_FontUIBold uppercased + letter-spaced. Used for every tab; also draws a
// 2px accent underline on the visible tab and a 1px hairline along the bar
// bottom (matching the mockup's tab row chrome).
//
// `leftEdgeX` / `rightEdgeX` are the visual edges of the dock column - the
// bar's own BarRect is inset on both sides by WindowBorderSize +
// FramePadding.x, so the accent underline and hairline would otherwise leave
// gaps before the column separators. For standalone (non-dock) bars, pass
// BarRect.Min.x / Max.x.
static void RestyleTabRow(ImGuiTabBar* bar, ImDrawList* dl, float leftEdgeX, float rightEdgeX)
{
    if (!bar || bar->Tabs.Size == 0 || !g_FontUIBold)
        return;

    const ImU32 colAccent = ImGui::ColorConvertFloat4ToU32(Palette::Accent);
    const ImU32 colLine = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
    const ImU32 colBgAct = ImGui::ColorConvertFloat4ToU32(Palette::Bg);
    const ImU32 colBgInact = ImGui::ColorConvertFloat4ToU32(Palette::Panel);
    const ImU32 colFg = ImGui::ColorConvertFloat4ToU32(Palette::Fg);
    const ImU32 colDim = ImGui::ColorConvertFloat4ToU32(Palette::Dim);

    const float barY0 = bar->BarRect.Min.y;
    const float barY1 = bar->BarRect.Max.y;
    // Tab labels: ImGui's text-rendering metrics make Inter at the mockup's
    // raw 11px read smaller than the browser equivalent - bump to ~12.5px for
    // visual parity. Letter-spacing stays at the mockup's ~0.08em.
    // Expressed as ratios of GetFontSize() so DPI scaling is automatic.
    const float bodyFont = ImGui::GetFontSize();  // = FontSizeBase * FontScaleDpi
    const float fontSize = ImFloor(bodyFont * (12.5f / 13.0f));
    const float letterSpacing = bodyFont * (1.0f / 13.0f);

    // Mirror ImGui's own tab-position math (imgui_widgets.cpp:10761):
    //   central section: BarRect.Min.x + (Offset - ScrollingAnim)
    //   leading/trailing: BarRect.Min.x + Offset
    // and clip central tabs to ScrollingRectMinX..ScrollingRectMaxX so that
    // tabs scrolled past the leading section don't overdraw it.
    const float scroll = bar->ScrollingAnim;
    // Close button strip reserved on the right edge of every dock tab -
    // FramePadding.x + ItemInnerSpacing.x + FontSize, matching what
    // TabItemCalcSize adds when has_close_button_or_unsaved_marker is true.
    // We trim our background overdraw by this much so the native close (X)
    // glyph ImGui drew underneath stays visible.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float closeStripW = style.FramePadding.x + style.ItemInnerSpacing.x + ImGui::GetFontSize();
    for (int t = 0; t < bar->Tabs.Size; ++t)
    {
        ImGuiTabItem* tab = &bar->Tabs[t];
        const bool is_central = (tab->Flags & ImGuiTabItemFlags_SectionMask_) == 0;
        const float tab_x_offset = is_central ? (tab->Offset - scroll) : tab->Offset;
        const float x0 = bar->BarRect.Min.x + tab_x_offset;
        const float x1 = x0 + tab->Width;
        const bool active = (tab->ID == bar->VisibleTabId);

        // Push central-section clip rect so scrolled-out tabs don't bleed
        // into the leading/trailing buttons or beyond the bar.
        const bool want_clip = is_central &&
                               (x0 < bar->ScrollingRectMinX || x1 > bar->ScrollingRectMaxX);
        if (want_clip)
            dl->PushClipRect(ImVec2(bar->ScrollingRectMinX, barY0),
                             ImVec2(bar->ScrollingRectMaxX, barY1), true);

        // Wipe the native label area so the default-font text is hidden.
        // Reserve the close-button strip on the right so the native (X) glyph
        // stays visible - ImGui drew it earlier in this frame and our fill
        // would otherwise paint over it.
        const float labelAreaRight = ImMax(x0, x1 - closeStripW);
        dl->AddRectFilled(ImVec2(x0, barY0), ImVec2(labelAreaRight, barY1),
                          active ? colBgAct : colBgInact);

        // Resolve the panel display name from the tab name (strip the ###ID
        // suffix so e.g. "Hierarchy###Hierarchy" renders just "HIERARCHY").
        const char* rawName = ImGui::TabBarGetTabName(bar, tab);
        if (!rawName) continue;
        char visible[64];
        {
            size_t i = 0;
            while (rawName[i] && i + 1 < sizeof(visible))
            {
                // ImGui's ###ID convention: everything from "###" onward is ID-only.
                if (rawName[i] == '#' && rawName[i + 1] == '#' && rawName[i + 2] == '#')
                    break;
                visible[i] = rawName[i];
                ++i;
            }
            visible[i] = 0;
        }
        char upper[64];
        Upper(visible, upper, sizeof(upper));

        // Compute label width with our font + letter-spacing.
        // Tabs use ImGuiTabBarFlags_FittingPolicyScroll (patched in imgui.cpp),
        // so the tab keeps its natural size and the bar scrolls when full -
        // no need to shrink labels or clip to fit.
        ImFont* font = g_FontUIBold;
        int chars = 0;
        for (const char* s = upper; *s;)
        {
            unsigned int c;
            int bytes = ImTextCharFromUtf8(&c, s, nullptr);
            if (bytes <= 0) break;
            s += bytes;
            ++chars;
        }
        float labelW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, upper).x;
        if (chars > 1) labelW += letterSpacing * (chars - 1);

        // Request a tab width that fits our drawn label exactly the way
        // TabItemCalcSize would - but using OUR measured label width instead
        // of ImGui's (default-font) measurement. With this width plus the
        // label positioned at tabStartX + FramePadding.x (mirroring the X's
        // right inset), the LABEL + X group lands symmetrically in the tab.
        // (1-frame visual lag the first time a tab is shown.)
        tab->RequestedWidth = labelW + 2.0f * style.FramePadding.x + style.ItemInnerSpacing.x + ImGui::GetFontSize();

        // Position the label so the (LABEL + X) group is centered in the
        // natural tab [x0, x1]. Mirror the X's right inset on the left -
        // labelX = x0 + FramePadding.x. We use x0 (not tabStartX) on
        // purpose: the leftmost-tab extension to leftEdgeX is a *visual*
        // fix for the dock-column gap, but the label should sit at the
        // same offset across every tab so they all look identical.
        const float labelX = x0 + style.FramePadding.x;
        const float labelY = barY0 + ImFloor((barY1 - barY0 - fontSize) * 0.5f);

        DrawSpacedText(dl, font, fontSize, ImVec2(labelX, labelY),
                       active ? colFg : colDim, upper, letterSpacing);

        // 2px accent bottom underline for the active tab - spans the full
        // tab width (label + X + surrounding padding).
        if (active)
        {
            dl->AddRectFilled(ImVec2(x0, barY1 - 2.0f),
                              ImVec2(x1, barY1), colAccent);
        }

        if (want_clip)
            dl->PopClipRect();
    }

    // 1px hairline along the bar's bottom edge - extend to the dock node's
    // true edges on both sides so it meets the column separators with no gap.
    dl->AddLine(ImVec2(leftEdgeX, barY1),
                ImVec2(rightEdgeX, barY1), colLine, 1.0f);
}

// The seam line between two docked panels is drawn by ImGui itself: its dock
// splitter fills the (now 1px) separator gap with ImGuiCol_Border. With
// WindowBorderSize = 0 the panels no longer stroke their own borders alongside
// it, so each seam is a single hairline - nothing to draw here for docked
// panels.
//
// What IS still needed: WindowBorderSize = 0 also dropped the frame around
// standalone *floating* windows (dialogs, undocked tool windows). Re-draw it on
// each such window's own draw list so the border sits on top of its content,
// exactly as ImGui used to. Docked tabs are skipped (their edges are the dock
// separators); child, popup and tooltip windows keep their own *BorderSize and
// are untouched.
//
// Call once per frame after all panels have rendered, right before
// ImGui::Render() (alongside DrawAllTabTopAccents).
void DrawDockSeams()
{
    ImGuiContext& g = *GImGui;
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Border);

    for (int i = 0; i < g.Windows.Size; ++i)
    {
        ImGuiWindow* w = g.Windows[i];
        if (!w || !w->WasActive || w->Hidden)
            continue;
        if (w != w->RootWindow)
            continue;
        if (w->DockIsActive || w->DockNodeAsHost)
            continue;
        if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Popup |
                        ImGuiWindowFlags_Tooltip))
            continue;
        w->DrawList->AddRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y),
                             col, w->WindowRounding, 0, 1.0f);
    }
}

void DrawAllTabTopAccents()
{
    ImGuiContext& g = *GImGui;

    // Draw onto each tab bar's owning window's draw list - NOT the foreground
    // draw list. The foreground layer renders above all windows including
    // tooltips and popups, which makes our accent stripes paint over them.
    // Using the host/owner window's draw list places the accent at the same
    // z-layer as the panel chrome, so later windows (tooltips, popups, modal
    // dialogs) render correctly above.

    // 1) Dock-node tab bars (one per docked panel group). The bar is drawn by
    //    ImGui onto node->HostWindow->DrawList during DockNodeUpdateTabBar, so
    //    that's the right surface to stamp our accent on.
    ImGuiDockNode* lastNode = nullptr;
    for (int i = 0; i < g.Windows.Size; ++i)
    {
        ImGuiWindow* w = g.Windows[i];
        if (!w || !w->DockNode || w->DockNode == lastNode)
            continue;
        lastNode = w->DockNode;
        // Only a host drawn this frame. A dock node outlives its windows: on
        // the project manager screen no panel is drawn, and after a minute
        // ImGui frees the idle host's draw list buffers, so stamping onto it
        // wrote through a freed pointer (crash after ~60 s on that screen).
        ImGuiWindow* host = w->DockNode->HostWindow;
        if (host && w->DockNode->TabBar && host->Active && !host->Hidden && host->LastFrameActive == g.FrameCount)
        {
            // Column edges = the dock node's true bounds (Pos.x..Pos.x+Size.x).
            // The bar's own BarRect is inset on both sides by border +
            // framepadding and would leave visible gaps before the dock
            // separators on either side.
            const float leftEdgeX = w->DockNode->Pos.x;
            const float rightEdgeX = w->DockNode->Pos.x + w->DockNode->Size.x;
            RestyleTabRow(w->DockNode->TabBar, w->DockNode->HostWindow->DrawList,
                          leftEdgeX, rightEdgeX);
        }
    }

    // 2) Standalone tab bars created via BeginTabBar (e.g. Asset Browser's
    //    Assets/Scripts sub-tabs). These live in the global pool with no back
    //    pointer to the owning window - find it by hit-testing the bar rect
    //    against active windows and picking the innermost match.
    for (int i = 0; i < g.TabBars.GetMapSize(); ++i)
    {
        ImGuiTabBar* bar = g.TabBars.TryGetMapData(i);
        if (!bar)
            continue;
        if (bar->PrevFrameVisible + 1 < g.FrameCount)
            continue;

        ImGuiWindow* owner = nullptr;
        float ownerArea = FLT_MAX;
        const ImVec2 probe = bar->BarRect.GetCenter();
        for (int wi = 0; wi < g.Windows.Size; ++wi)
        {
            ImGuiWindow* w = g.Windows[wi];
            if (!w || !w->Active || w->Hidden)
                continue;
            // Skip tooltip / popup / menu layers - we want the underlying panel.
            if (w->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup))
                continue;
            ImRect r = w->Rect();
            if (!r.Contains(probe))
                continue;
            float area = r.GetWidth() * r.GetHeight();
            if (area < ownerArea)
            {
                owner = w;
                ownerArea = area;
            }
        }
        if (owner)
            RestyleTabRow(bar, owner->DrawList, bar->BarRect.Min.x, bar->BarRect.Max.x);
    }
}

bool SchematicCheckbox(const char* label, bool* v, const ImVec4& fillColor)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

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
    const ImRect total(pos,
                       ImVec2(pos.x + box + (labelSize.x > 0.0f ? style.ItemInnerSpacing.x + labelSize.x : 0.0f),
                              pos.y + ImMax(frameH, box)));

    ImGui::ItemSize(total, style.FramePadding.y);
    if (!ImGui::ItemAdd(total, id))
        return false;

    bool hovered = false, held = false;
    bool pressed = ImGui::ButtonBehavior(total, id, &hovered, &held);
    if (pressed)
    {
        *v = !*v;
        ImGui::MarkItemEdited(id);
    }

    ImDrawList* dl = window->DrawList;
    const ImU32 borderCol = ImGui::ColorConvertFloat4ToU32(
        hovered ? Palette::Line3 : Palette::Line2);

    // Outer box (1px border, fully transparent fill - matches mockup where the
    // box sits flush on the panel background). Rounded to the same radius as the
    // themed frames, clamped so it never exceeds half the box size.
    const ImVec2 bMin = boxPos;
    const ImVec2 bMax(boxPos.x + box, boxPos.y + box);
    const float boxRounding = ImMin(style.FrameRounding, box * 0.5f);
    dl->AddRect(bMin, bMax, borderCol, boxRounding, 0, 1.0f);

    if (*v)
    {
        const ImU32 inner = ImGui::ColorConvertFloat4ToU32(fillColor);
        const float innerInset = ImFloor(box * 0.28f);
        dl->AddRectFilled(ImVec2(bMin.x + innerInset, bMin.y + innerInset),
                          ImVec2(bMax.x - innerInset, bMax.y - innerInset),
                          inner, ImMax(0.0f, boxRounding - innerInset));
    }

    if (labelSize.x > 0.0f)
    {
        ImGui::RenderText(ImVec2(bMax.x + style.ItemInnerSpacing.x,
                                 pos.y + ImFloor((ImMax(frameH, box) - fontSize) * 0.5f)),
                          label);
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
    ImGui::Indent(Metrics::InspectorIndent * dpi);
    // Row gap and field padding together: separation comes from the height of
    // the fields, not from air between them, which is what keeps a long list
    // reading as a table instead of a stack of loose controls.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, Metrics::RowSpacing * dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(Metrics::FieldPadding * dpi, Metrics::FieldPadding * dpi));
}

void EndPropertyContext()
{
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    ImGui::PopStyleVar(2);
    ImGui::Unindent(Metrics::InspectorIndent * dpi);
}

bool SchematicCollapsingHeader(const char* label, ImGuiTreeNodeFlags flags)
{
    // Styled like a dock tab (uppercase letter-spaced SemiBold label, tab-
    // colored background, bottom hairline) but NOT movable and WITHOUT the
    // accent underline. Still a real CollapsingHeader so the arrow + click-to-
    // collapse behavior is preserved.
    ImGuiWindow* w = ImGui::GetCurrentWindow();

    // Split the label into its visible text and any "###ID" tail. We draw the
    // visible text ourselves (letter-spaced), so CollapsingHeader gets an
    // id-only string: same ImGui id (state preserved), no built-in label text.
    const char* hash = ImStrchrRange(label, label + strlen(label), '#');
    const bool hasId = hash && hash[1] == '#' && hash[2] == '#';
    const size_t visLen = hasId ? (size_t)(hash - label) : strlen(label);

    char visUpper[128];
    {
        char vis[128];
        size_t n = visLen < sizeof(vis) - 1 ? visLen : sizeof(vis) - 1;
        for (size_t i = 0; i < n; ++i)
            vis[i] = label[i];
        vis[n] = 0;
        Upper(vis, visUpper, sizeof(visUpper));
    }

    char idStr[160];
    if (hasId)
        ImFormatString(idStr, sizeof(idStr), "%s", hash);  // "###ID"
    else
        ImFormatString(idStr, sizeof(idStr), "###%s", visUpper);  // id == hash(visUpper)

    // Full-bleed the header band to the panel's real side edges (the dock
    // separators), reaching past the scrollbar on the right.
    //
    // The chevron + label sit one step OUTSIDE the content-left, so the rows
    // under the band read as nested inside it rather than as a stack of
    // unrelated lines starting on the same edge. Clamped to a margin from the
    // panel edge, so a header drawn at no indent at all still has air to its
    // left, and relative (not absolute) so a band nested deeper in the layout
    // keeps showing its depth.
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
        labelBaseX = ImMax(bandLeft + Metrics::SectionHeaderMargin * dpiScale,
                           textBaseX - Metrics::SectionHeaderOutdent * dpiScale);
        w->DC.CursorPos.x = bandLeft;
        w->WorkRect.Min.x = bandLeft;
        w->WorkRect.Max.x = bandRight;
        // Widen the clip so the band paints under the scrollbar all the way to
        // the separator instead of being cut off at the content edge.
        ImGui::PushClipRect(ImVec2(bandLeft, w->ClipRect.Min.y),
                            ImVec2(bandRight, w->ClipRect.Max.y), false);
        pushedBandClip = true;
    }

    // Tab background, subtle white-fade hover/active (no accent). Text is made
    // transparent so ImGui's built-in filled-triangle arrow is hidden: we draw
    // a thin Tabler chevron ourselves below.
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

    // No row gap charged after the band. The gap belongs to the BODY (see
    // SchematicSectionBegin, which emits it), so a collapsed section sits flush
    // on the hairline under it instead of leaving a row of air with nothing in
    // it - and so that gap slides away with the body rather than outliving it.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    const bool open = ImGui::CollapsingHeader(idStr, flags);
    ImGui::PopStyleVar();
    // Chevron zone from the theme token, NOT from GetTreeNodeToLabelSpacing():
    // ImGui's own arrow and label are hidden here (transparent text), so nothing
    // depends on its geometry, and the token is the one column every band and
    // every row in every panel aligns to. See Metrics::SectionLabelIndent.
    const float labelSpacing = Metrics::SectionLabelIndent * dpi;

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);

    if (pushedBandClip)
        ImGui::PopClipRect();

    // Restore the work rect; keep CursorPosPrevLine at the un-bled right edge so
    // a trailing SameLine (the `EDIT ->` link) still positions as before.
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
        const ImU32 gray = ImGui::ColorConvertFloat4ToU32(
            ImVec4(165.0f / 255.0f, 166.0f / 255.0f, 164.0f / 255.0f, 1.0f));

        // Thin Tabler chevron in place of ImGui's filled triangle, centered in
        // the arrow zone before the label. It TURNS from right to down over the
        // open/close instead of the glyph being swapped: the quarter turn is
        // what tells you the section you just clicked is the one that moved,
        // which matters most when several are open at once.
        ImFont* ico = g_FontUI ? g_FontUI : ImGui::GetFont();
        const float icoSize = ImGui::GetFontSize();
        const float openT = AnimateTo(ImGui::GetItemID(), open ? 1.0f : 0.0f, 0.13f);
        DrawTurningChevron(dl, ico, icoSize,
                           ImVec2(labelBaseX + labelSpacing * 0.5f, centerY),
                           gray, ICON_TI_CHEVRON_RIGHT, openT, IM_PI * 0.5f);

        // Tab label: uppercase, letter-spaced, Inter SemiBold, after the arrow.
        const float bodyFont = ImGui::GetFontSize();
        const float fontSize = ImFloor(bodyFont * (12.5f / 13.0f));
        const float letterSpacing = bodyFont * (1.0f / 13.0f);
        ImFont* f = g_FontUIBold ? g_FontUIBold : ImGui::GetFont();
        const float labelY = ImFloor(centerY - fontSize * 0.5f);
        DrawSpacedText(dl, f, fontSize, ImVec2(labelBaseX + labelSpacing, labelY),
                       gray, visUpper, letterSpacing);
    }

    return open;
}

bool SchematicSectionBand(const char* label, float* outLabelX)
{
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems)
        return false;

    // Label split like SchematicCollapsingHeader's: the visible text is drawn by
    // hand and any "###id" tail only identifies the row, so a band whose text
    // changes keeps one identity.
    //
    // Drawn VERBATIM though - no uppercasing, no extra tracking. This band
    // titles a thing by its name rather than naming a section, and a name is
    // not a label to shout: "myState" is not "MYSTATE". It also lets the inline
    // rename field (SchematicBandTitleField) look exactly like the title it
    // replaces, so starting a rename does not restyle the header.
    const char* hash = ImStrchrRange(label, label + strlen(label), '#');
    const bool hasId = hash && hash[1] == '#' && hash[2] == '#';
    const size_t visLen = hasId ? (size_t)(hash - label) : strlen(label);

    char vis[128];
    {
        size_t n = visLen < sizeof(vis) - 1 ? visLen : sizeof(vis) - 1;
        for (size_t i = 0; i < n; ++i)
            vis[i] = label[i];
        vis[n] = 0;
    }

    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float bandH = 37.0f * dpi;  // same height as the header
    const ImVec2 pos = w->DC.CursorPos;

    // Full-bleed to the panel's side edges, past the scrollbar, exactly like
    // the collapsing header. The clip is widened (not intersected) for the same
    // reason it is there: the band paints outside the content rect.
    ImDrawList* dl = w->DrawList;
    const float bandLeft = w->Pos.x;
    const float bandRight = w->Pos.x + w->Size.x - w->ScrollbarSizes.x;

    // Label one step outside the content-left, floored at a margin from the
    // panel edge - the same offset the collapsing header uses, so the two kinds
    // of band line up with each other and the rows nest under both.
    const float labelBaseX = ImMax(bandLeft + Metrics::SectionHeaderMargin * dpi,
                                   pos.x - Metrics::SectionHeaderOutdent * dpi);

    // The row is a button, not a Dummy: a band that titles something renameable
    // is clicked, and this way the caller gets the click without reaching for
    // the item rect afterwards. It spans the whole band (from the panel edge,
    // like the collapsing header's frame does) so clicking the outdented label
    // counts; the cursor's x is put back underneath for the content that
    // follows.
    ImGui::SetCursorScreenPos(ImVec2(bandLeft, pos.y));
    const float rowW = ImMax(1.0f, w->WorkRect.Max.x - bandLeft);
    // The row claims the whole band, so anything a caller then places ON the
    // band - the node graph's inline rename field - would be submitted second
    // and never get the hover, since the first item to claim it in a frame
    // keeps it. Allowing overlap hands hover and clicks to whatever is drawn on
    // top, which is how a click inside that field can move its caret instead of
    // being swallowed by the row underneath. The collapsing header does the
    // same thing through ImGuiTreeNodeFlags_AllowOverlap.
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton(hasId ? hash : label, ImVec2(rowW, bandH));
    const bool hovered = ImGui::IsItemHovered();
    // Nothing is submitted after the button on purpose. ItemSize already put the
    // cursor's x back at the indent for the content that follows, so no restore
    // is needed, and closing with a zero-size Dummy (as this used to) made THAT
    // the window's last item: a caller reading GetItemRect to place a control on
    // the band - the inline rename field does exactly this - measured the dummy
    // sitting on the band's bottom edge and dropped its field half a band lower,
    // over the row underneath. The button IS the band's item; leave it that way.

    ImGui::PushClipRect(ImVec2(bandLeft, w->ClipRect.Min.y),
                        ImVec2(bandRight, w->ClipRect.Max.y), false);
    dl->AddRectFilled(ImVec2(bandLeft, pos.y), ImVec2(bandRight, pos.y + bandH),
                      ImGui::ColorConvertFloat4ToU32(Palette::Bg));
    if (const float hoverT = Motion::ItemHover(hovered); hoverT > 0.0f)
        dl->AddRectFilled(ImVec2(bandLeft, pos.y), ImVec2(bandRight, pos.y + bandH),
                          Motion::WithAlpha(IM_COL32(255, 255, 255, 10), hoverT));
    if (const float pressT = Motion::ItemPress(); pressT > 0.0f)
        dl->AddRectFilled(ImVec2(bandLeft, pos.y), ImVec2(bandRight, pos.y + bandH), Motion::PressShade(pressT));
    ImGui::PopClipRect();

    // No chevron is drawn, but the label still starts on the shared column:
    // a title that sits further left than the rows it names reads as a separate
    // line above them rather than as their heading. The token is the same one
    // the collapsing header and every property row use, so a chevron-less band,
    // a foldout and their content all line up.
    const float labelTextX = labelBaseX + Metrics::SectionLabelIndent * dpi;

    const float fontSize = BandTitleFontSize();
    ImFont* f = BandTitleFont();
    const float labelY = ImFloor(pos.y + (bandH - fontSize) * 0.5f);
    dl->AddText(f, fontSize, ImVec2(labelTextX, labelY), BandTitleColor(), vis);

    if (outLabelX)
        *outLabelX = labelTextX;
    return clicked;
}

bool SchematicBandTitleField(const char* id, char* buf, int bufSize,
                             float x, float bandTop, float bandBottom, float width,
                             const char* hint)
{
    if (!buf || bufSize <= 0)
        return false;

    // Everything that makes an ImGui field LOOK like a field is turned off:
    // no frame background, no border, no padding. What is left is text in the
    // band's own type, on the band's own line, at the same x the label sits at
    // - so entering a rename does not restyle the header, and the caret is the
    // only thing that says it is editable now.
    ImFont* font = BandTitleFont();

    ImGui::PushFont(font, BandTitleFontSizeBase());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    // The last box on the field: ImGui outlines whatever holds keyboard focus,
    // and the field is focused the moment it appears (that is the point of the
    // rename). In accent, 2px, around a control that is otherwise invisible, it
    // WAS the field's border as far as the eye is concerned.
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
        return;

    // One rung below the section band in the same ladder: same uppercase
    // letter-spaced treatment, smaller and dimmer, with a short rule beside it
    // instead of a full-width one. A group inside a component is a subdivision
    // of that component, and reading as bold body text made it look like a
    // property that had lost its field.
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
    const float textW = DrawSpacedText(dl, f, fontSize, ImVec2(pos.x, textY), dim,
                                       visUpper, letterSpacing);

    // Rule from the end of the label to the content edge, on the text's center
    // line: it ties the label to the rows under it without cutting the column.
    const float ruleX0 = pos.x + textW + 10.0f * dpi;
    const float ruleX1 = w->WorkRect.Max.x;
    if (ruleX1 > ruleX0)
        dl->AddLine(ImVec2(ruleX0, ImFloor(pos.y + rowH * 0.5f)),
                    ImVec2(ruleX1, ImFloor(pos.y + rowH * 0.5f)), line, 1.0f);

    ImGui::Dummy(ImVec2(ImMax(1.0f, w->WorkRect.Max.x - pos.x), rowH));
}

bool SchematicAddButton(const char* label, float width)
{
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems)
        return false;

    // An "add" is an invitation, not a command: an outlined, centered box that
    // sits quietly (transparent fill, hairline border, dim label) and only
    // commits to the accent when the pointer is on it. A solid full-width
    // button in the same weight as the fields above it reads as one more
    // control to fill in, and at the bottom of a list that is exactly wrong.
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float boxH = 32.0f * dpi;
    const float avail = ImGui::GetContentRegionAvail().x;

    float boxW = width > 0.0f ? width : 240.0f * dpi;
    if (boxW > avail) boxW = avail;
    if (boxW < 1.0f) boxW = 1.0f;

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
        dl->AddRectFilled(boxMin, boxMax, fill, rounding);
    dl->AddRect(boxMin, boxMax, border, rounding, 0, 1.0f);

    ImFont* f = g_FontUIMed ? g_FontUIMed : ImGui::GetFont();
    const float fontSize = Fonts::CssPx(f, 12.5f) * dpi;
    const ImVec2 textSz = f->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
    dl->AddText(f, fontSize,
                ImVec2(ImFloor(boxMin.x + (boxW - textSz.x) * 0.5f),
                       ImFloor(boxMin.y + (boxH - textSz.y) * 0.5f)),
                text, label);

    // Put x back for whatever follows; the button itself reserved the row.
    ImGui::SetCursorScreenPos(ImVec2(start.x, ImGui::GetCursorScreenPos().y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopStyleVar();
    return clicked;
}

void DrawTreeNodeChevronLabel(bool open, bool expandable, const char* label, ImU32 labelColor)
{
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems)
        return;

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
        const ImU32 gray = ImGui::ColorConvertFloat4ToU32(
            ImVec4(165.0f / 255.0f, 166.0f / 255.0f, 164.0f / 255.0f, 1.0f));
        const float openT = AnimateTo(ImGui::GetItemID(), open ? 1.0f : 0.0f, 0.13f);
        DrawTurningChevron(dl, ico, icoSize,
                           ImVec2(mn.x + labelSpacing * 0.5f, centerY),
                           gray, ICON_TI_CHEVRON_RIGHT, openT, IM_PI * 0.5f);
    }

    // Label - same font/size ImGui would have used, after the arrow zone.
    if (label && *label)
    {
        ImFont* f = ImGui::GetFont();
        const float fs = ImGui::GetFontSize();
        dl->AddText(f, fs, ImVec2(mn.x + labelSpacing, ImFloor(centerY - fs * 0.5f)),
                    labelColor, label);
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

    // Stretch the cursor + work rect to the window's real side edges (the dock
    // seams) so the line spans full width, then widen the clip so it paints
    // under the scrollbar all the way to the right seam.
    const float savedWorkMaxX = w->WorkRect.Max.x;
    const float left = w->Pos.x;
    const float right = w->Pos.x + w->Size.x - w->ScrollbarSizes.x;  // stop at the scrollbar
    w->DC.CursorPos.x = left;
    w->WorkRect.Max.x = right;
    ImGui::PushClipRect(ImVec2(left, w->ClipRect.Min.y),
                        ImVec2(right, w->ClipRect.Max.y), false);

    // The line costs exactly its own pixel: no ItemSpacing charged above or
    // below it. A separator is a BOUNDARY, and the band or content on either
    // side owns whatever air it needs - charged spacing put a gap between the
    // hairline and the section header hanging off it, and the gap grew with
    // whatever row spacing the panel happened to be using (the property context
    // uses 12, so the boundary opened to 24). Tight here, deliberate there.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::Separator();
    ImGui::PopStyleVar();

    ImGui::PopClipRect();
    w->WorkRect.Max.x = savedWorkMaxX;  // CursorPos.x is reset to the indent by ItemSize
}

bool PanelToolbar(bool* pinnedExt)
{
    // The toolbar exists solely to host the Pin (lock-to-selection) control,
    // which only the Inspector wires up. Panels that don't pin pass no flag and
    // get no toolbar - no Pin button, no kebab, no reserved strip.
    if (!pinnedExt)
        return false;

    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems)
        return *pinnedExt;

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

    // PIN button (left). Accent when pinned, dim otherwise; filled pin glyph
    // signals the active state.
    {
        const char* ico = *pinned ? ICON_TI_PIN_FILLED : ICON_TI_PIN;
        const ImVec2 icoSz = iconFont->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, ico);
        const char* lbl = "PIN";
        const float lblW = lblFont->CalcTextSizeA(lblSize, FLT_MAX, 0.0f, lbl).x + lblSpacing * 3.0f;
        const float btnW = padX * 2.0f + icoSz.x + gapIcoLbl + lblW;

        ImGui::SetCursorScreenPos(ImVec2(left, top));
        if (ImGui::InvisibleButton("##deki_pin_btn", ImVec2(btnW, barH)))
            *pinned = !*pinned;
        if (const float hoverT = Motion::ItemHover(ImGui::IsItemHovered()); hoverT > 0.0f)
            dl->AddRectFilled(ImVec2(left, top), ImVec2(left + btnW, top + barH), Motion::WithAlpha(colHover, hoverT));
        if (const float pressT = Motion::ItemPress(); pressT > 0.0f)
            dl->AddRectFilled(ImVec2(left, top), ImVec2(left + btnW, top + barH), Motion::PressShade(pressT));

        const ImU32 col = *pinned ? colAccent : colDim;
        const float ix = left + padX;
        dl->AddText(iconFont, iconSize, ImVec2(ix, ImFloor(cy - icoSz.y * 0.5f)), col, ico);
        DrawSpacedText(dl, lblFont, lblSize,
                       ImVec2(ix + icoSz.x + gapIcoLbl, ImFloor(cy - lblSize * 0.5f)),
                       col, lbl, lblSpacing);
    }

    // Kebab menu (right).
    {
        const char* ico = ICON_TI_DOTS_VERTICAL;
        const ImVec2 icoSz = iconFont->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, ico);
        const float btnW = padX * 2.0f + icoSz.x;
        const float bx = right - btnW;

        ImGui::SetCursorScreenPos(ImVec2(bx, top));
        if (ImGui::InvisibleButton("##deki_panel_menu_btn", ImVec2(btnW, barH)))
            ImGui::OpenPopup("##deki_panel_menu");
        if (const float hoverT = Motion::ItemHover(ImGui::IsItemHovered()); hoverT > 0.0f)
            dl->AddRectFilled(ImVec2(bx, top), ImVec2(bx + btnW, top + barH), Motion::WithAlpha(colHover, hoverT));
        if (const float pressT = Motion::ItemPress(); pressT > 0.0f)
            dl->AddRectFilled(ImVec2(bx, top), ImVec2(bx + btnW, top + barH), Motion::PressShade(pressT));
        dl->AddText(iconFont, iconSize, ImVec2(bx + padX, ImFloor(cy - icoSz.y * 0.5f)), colDim, ico);
    }

    if (ImGui::BeginPopup("##deki_panel_menu"))
    {
        if (ImGui::MenuItem("Pin", nullptr, *pinned))
            *pinned = !*pinned;
        ImGui::EndPopup();
    }

    // Bottom hairline, seam to seam.
    dl->AddLine(ImVec2(left, top + barH), ImVec2(right, top + barH), colLine, 1.0f);

    // Content flows BELOW that hairline, not onto it: one window is one draw
    // list, so the first thing the panel draws would otherwise paint over the
    // bar's edge (a log row's background, a hovered row's fill) and the
    // boundary would vanish exactly when there is content near the top.
    // Reserved as an item rather than a bare cursor move, so a panel that draws
    // nothing under its bar does not trip ImGui's "submit an item" check.
    ImGui::SetCursorScreenPos(ImVec2(left, top + barH));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::Dummy(ImVec2(0.0f, 1.0f));
    ImGui::PopStyleVar();
    return *pinned;
}

void SearchField(const char* id, char* buf, size_t bufSize,
                 float rightReserve, bool bottomBorder,
                 float* outSlotX, float* outSlotY, float* outSlotW, float* outSlotH,
                 const char* hint)
{
    // A dim magnifier + a borderless transparent input (no box), vertically
    // centered, hinting "Search". Sizing mirrors EditorApp::Dpi() via the
    // window's DPI scale so the band matches the rest of the panel chrome.
    const float dpi = ImGui::GetWindowDpiScale();
    const float searchInset = 12.0f * dpi;
    const float inputSize = Fonts::CssPx(g_FontUI, 12.0f);
    const float iconSize = Fonts::CssPx(g_FontUI, 14.0f);
    const float searchPadY = 6.0f * dpi;
    const float slotGap = 8.0f * dpi;  // gap between the input and a reserved slot

    // Tighter frame padding so the input isn't as tall as the theme default.
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

    // The band is just the search input's height plus a little top/bottom pad,
    // unchanged whether or not a slot is reserved. A reserved control fills the
    // whole band (reported via the out-params), flat against the hairline.
    const float rowTop = bandStart.y + searchPadY;
    const float bandBottom = rowTop + inputFrameH + searchPadY;
    const float inputY = rowTop;

    // Right edge of the input. With a reserved slot, the slot sits flush against
    // the window's right edge (no right gutter) and the input stops a gap short of
    // it; otherwise the input keeps the symmetric searchInset gutter.
    float inputRight = winRight - searchInset;
    if (rightReserve > 0.0f)
        inputRight = (winRight - rightReserve) - slotGap;

    // Magnifier icon, vertically centered on the input text's middle.
    const float iconCursorOffset = framePadY + (inputLineH - iconLineH) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(iconLeftX, inputY + iconCursorOffset));
    ImGui::PushFont(g_FontUI, iconSize);
    ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim2);
    ImGui::TextUnformatted(ICON_TI_SEARCH);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // With text in the field, the input stops short of a clear button sitting
    // where the text ends. Measured whether or not it is drawn: the input must
    // not change width the moment you type, or the caret jumps under the cursor.
    float clearAdvance = 0.0f;
    {
        ImGui::PushFont(g_FontUI, iconSize);
        clearAdvance = ImGui::CalcTextSize(ICON_TI_X).x;
        ImGui::PopFont();
    }
    const float clearWidth = clearAdvance + slotGap;

    // Borderless transparent input beginning after the icon + 8px gap.
    const float inputLeftX = iconLeftX + iconAdvance + 8.0f;
    ImGui::SetCursorScreenPos(ImVec2(inputLeftX, inputY));
    ImGui::PushFont(g_FontUI, inputSize);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, Palette::Dim2);
    ImGui::SetNextItemWidth((inputRight - clearWidth) - inputLeftX);
    ImGui::InputTextWithHint(id, hint ? hint : "Search", buf, bufSize);
    // Escape empties the field rather than only dropping focus. In a popup ImGui
    // takes Escape to close, so this is checked while the input is active - and
    // an emptied field is the same state as a closed popup would leave anyway.
    const bool inputActive = ImGui::IsItemActive();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::PopFont();

    // Read AFTER the input: InputText writes into the buffer, and asking first
    // would hold the button back a frame behind the character you just typed.
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
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::IsMouseHoveringRect(ImVec2(clearX, inputY),
                                                         ImVec2(clearX + clearAdvance,
                                                                inputY + inputFrameH))
                                  ? Palette::Fg
                                  : Palette::Dim2);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        // Scoped by the field's own id, so two search fields in one window do not
        // share a button.
        ImGui::PushID(id);
        if (ImGui::Button(ICON_TI_X "##clear"))
            buf[0] = '\0';
        ImGui::PopID();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
        ImGui::PopFont();
    }

    ImGui::PopStyleVar();  // FramePadding

    // Report the reserved slot (flush right, spanning the full band height) so the
    // caller can fill it with a toolbar-style control sitting flush to the hairline.
    if (outSlotX) *outSlotX = winRight - rightReserve;
    if (outSlotY) *outSlotY = bandStart.y;
    if (outSlotW) *outSlotW = rightReserve;
    if (outSlotH) *outSlotH = bandBottom - bandStart.y;

    // Full-width hairline beneath the row (matches the panel toolbar's bottom
    // border above it), floored to a crisp 1px row. It sits a clear searchPadY
    // below the row content so a slotted control's own border doesn't touch it.
    if (bottomBorder)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float by = ImFloor(bandBottom);
        dl->AddRectFilled(ImVec2(winLeft, by - 1.0f), ImVec2(winRight, by),
                          ImGui::ColorConvertFloat4ToU32(Palette::Line2));
    }

    // Reserve the band so the content below lays out correctly, closing with a
    // zero item: a bare cursor move as a window's last act is what ImGui's
    // "submit an item e.g. Dummy()" check complains about.
    ImGui::SetCursorScreenPos(ImVec2(bandStart.x, bandBottom));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopStyleVar();
}

// Draw the formatted value left-aligned (mono) inside an explicit rect on a
// given draw list. Shared core: OverlayLeftAlignedValue uses the current item's
// rect; the schematic combo uses the box rect it captured before BeginCombo
// (the open popup changes the last-item rect, so GetItemRect* can't be trusted).
static void DrawLeftAlignedValue(ImDrawList* dl, const ImVec2& itemMin, const ImVec2& itemMax,
                                 const char* formatted, float xOffset = 0.0f)
{
    ImFont* font = g_FontMono ? g_FontMono : ImGui::GetFont();
    // Mono value font at 12.5 CSS px. CssPx applies the mono em-ratio and we
    // multiply by the DPI scale, since this hand-rolled AddText bypasses both.
    const float fontSize = Fonts::CssPx(font, 12.5f) * ImGui::GetStyle().FontScaleDpi;
    const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, formatted);
    const float padX = ImGui::GetStyle().FramePadding.x;
    const float y = itemMin.y + ImMax(0.0f, (itemMax.y - itemMin.y - textSize.y) * 0.5f);

    dl->PushClipRect(ImVec2(itemMin.x + 1 + xOffset, itemMin.y + 1),
                     ImVec2(itemMax.x - 1, itemMax.y - 1), true);
    dl->AddText(font, fontSize, ImVec2(itemMin.x + xOffset + padX, y),
                ImGui::GetColorU32(ImGuiCol_Text), formatted);
    dl->PopClipRect();
}

// Render the formatted value as left-aligned text inside the just-drawn drag
// widget's frame. Skipped while the widget is in keyboard-edit (TempInput)
// mode so ImGui's native text editor remains usable. Uses g_FontMono so
// numeric values match the mockup's mono value-cell typography.
// `xOffset` lets callers reserve room on the left (e.g. for an axis stamp).
static void OverlayLeftAlignedValue(const char* formatted, float xOffset = 0.0f)
{
    const ImGuiID id = ImGui::GetItemID();
    if (ImGui::TempInputIsActive(id))
        return;

    DrawLeftAlignedValue(ImGui::GetWindowDrawList(),
                         ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), formatted, xOffset);
}

// Paint a colored axis stamp on the left edge of the just-drawn drag frame.
// Returns the stamp width so the caller can offset the value overlay past it.
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
        return stampW;  // skip overlay paint during text edit; reserve width anyway

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = IM_COL32(255, 255, 255, 8);
    const ImU32 sep = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
    const ImU32 fg = ImGui::ColorConvertFloat4ToU32(axisColor);

    dl->AddRectFilled(ImVec2(itemMin.x + 1, itemMin.y + 1),
                      ImVec2(itemMin.x + stampW, itemMax.y - 1), bg);
    dl->AddLine(ImVec2(itemMin.x + stampW, itemMin.y + 1),
                ImVec2(itemMin.x + stampW, itemMax.y - 1), sep, 1.0f);
    dl->AddText(ImVec2(itemMin.x + padX,
                       itemMin.y + (itemMax.y - itemMin.y - labelSize.y) * 0.5f),
                fg, axisLabel);
    return stampW;
}

bool SchematicDragFloat(const char* id, float* v, float v_speed,
                        float v_min, float v_max, const char* format)
{
    // Empty format suppresses ImGui's centered value text without disabling
    // the widget; edit mode (TempInput) still uses the real format via
    // ImGui's internal fallback.
    bool changed = ImGui::DragFloat(id, v, v_speed, v_min, v_max, "");
    if (format && format[0])
    {
        char buf[64];
        ImFormatString(buf, IM_ARRAYSIZE(buf), format, *v);
        OverlayLeftAlignedValue(buf);
    }
    return changed;
}

bool SchematicDragInt(const char* id, int* v, float v_speed,
                      int v_min, int v_max, const char* format)
{
    bool changed = ImGui::DragInt(id, v, v_speed, v_min, v_max, "");
    if (format && format[0])
    {
        char buf[64];
        ImFormatString(buf, IM_ARRAYSIZE(buf), format, *v);
        OverlayLeftAlignedValue(buf);
    }
    return changed;
}

// Axis labels and colors for multi-component drags (X / Y / Z).
// Reds / greens / blues are oklch-derived to match the mockup's TwoColD axis stamps.
static const char* const kAxisLabels[3] = { "X", "Y", "Z" };
static const ImVec4 kAxisColors[3] = {
    ImVec4(0.91f, 0.42f, 0.31f, 1.0f),  // X - red oklch(0.72 0.2 25)
    ImVec4(0.45f, 0.83f, 0.45f, 1.0f),  // Y - green oklch(0.78 0.2 145)
    ImVec4(0.40f, 0.65f, 1.00f, 1.0f),  // Z - blue oklch(0.7 0.18 240)
};

template <int N>
static bool SchematicDragFloatN(const char* id, float v[N], float v_speed,
                                float v_min, float v_max, const char* format)
{
    // Split available row width across N components with inner spacing.
    const float total = ImGui::CalcItemWidth();
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float compW = ImMax(1.0f, (total - spacing * (N - 1)) / float(N));

    bool changed = false;
    ImGui::PushID(id);
    ImGui::BeginGroup();
    for (int i = 0; i < N; ++i)
    {
        if (i > 0) ImGui::SameLine(0.0f, spacing);
        ImGui::SetNextItemWidth(compW);
        char compId[8];
        ImFormatString(compId, IM_ARRAYSIZE(compId), "##c%d", i);

        // Draw the drag with no value text (empty format).
        if (ImGui::DragFloat(compId, &v[i], v_speed, v_min, v_max, ""))
            changed = true;

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

bool SchematicDragFloat2(const char* id, float v[2], float v_speed,
                         float v_min, float v_max, const char* format)
{
    return SchematicDragFloatN<2>(id, v, v_speed, v_min, v_max, format);
}

bool SchematicDragFloat3(const char* id, float v[3], float v_speed,
                         float v_min, float v_max, const char* format)
{
    return SchematicDragFloatN<3>(id, v, v_speed, v_min, v_max, format);
}

// Shared implementation for the 3- and 4-channel color editors. `N` is 3 or 4.
static bool SchematicColorEditN(const char* id, float* col, int N)
{
    // Layout: N channel cells + a swatch on the right (square = frame
    // height). Each channel is a 1px bordered cell with a 3px vertical
    // color stripe on the left and a SchematicDragInt (0-255) filling the
    // rest. Click the swatch to open the full ImGui color picker.
    const float totalW = ImGui::CalcItemWidth();
    const float frameH = ImGui::GetFrameHeight();
    const float swatchW = frameH;  // square swatch matches row height
    const float gap = 6.0f;
    const float channelsW = totalW - swatchW - gap;
    const float channelW = ImMax(20.0f, (channelsW - gap * (float)(N - 1)) / (float)N);
    const float stripeW = 3.0f;

    bool changed = false;

    ImGui::PushID(id);
    ImGui::BeginGroup();

    const ImU32 channelStripeCols[4] = {
        IM_COL32(232, 65, 24, 255),    // R
        IM_COL32(76, 209, 55, 255),    // G
        IM_COL32(64, 153, 255, 255),   // B
        IM_COL32(140, 140, 140, 255),  // A
    };

    for (int ch = 0; ch < N; ++ch)
    {
        if (ch > 0) ImGui::SameLine(0.0f, gap);

        const ImVec2 cellPos = ImGui::GetCursorScreenPos();
        const float cellH = ImGui::GetFrameHeight();
        const ImVec2 cellMax(cellPos.x + channelW, cellPos.y + cellH);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // 3px colored stripe on the left
        dl->AddRectFilled(cellPos, ImVec2(cellPos.x + stripeW, cellMax.y),
                          channelStripeCols[ch]);

        // Drag widget for the 0-255 value, offset past the stripe
        ImGui::PushID(ch);
        ImGui::SetCursorScreenPos(ImVec2(cellPos.x + stripeW, cellPos.y));
        ImGui::SetNextItemWidth(channelW - stripeW);
        int v = (int)ImClamp(col[ch] * 255.0f + 0.5f, 0.0f, 255.0f);
        if (SchematicDragInt("##v", &v, 1.0f, 0, 255, "%d"))
        {
            col[ch] = (float)v / 255.0f;
            changed = true;
        }
        ImGui::PopID();
    }

    // Combined color swatch on the right, click to open the full picker
    ImGui::SameLine(0.0f, gap);
    const ImVec2 swatchPos = ImGui::GetCursorScreenPos();
    const float swatchH = ImGui::GetFrameHeight();
    const ImVec2 swatchMax(swatchPos.x + swatchW, swatchPos.y + swatchH);

    ImGui::InvisibleButton("##swatch", ImVec2(swatchW, swatchH));
    const bool swatchHovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
        ImGui::OpenPopup("##colorpicker");

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Round the swatch to match the themed frames, clamped to half its size.
    const float swatchRounding = ImMin(ImGui::GetStyle().FrameRounding, swatchW * 0.5f);
    dl->AddRectFilled(swatchPos, swatchMax,
                      ImGui::ColorConvertFloat4ToU32(ImVec4(col[0], col[1], col[2], 1.0f)), swatchRounding);
    dl->AddRect(swatchPos, swatchMax,
                ImGui::ColorConvertFloat4ToU32(swatchHovered ? Palette::Line3 : Palette::Line2), swatchRounding);

    if (ImGui::BeginPopup("##colorpicker"))
    {
        if (N >= 4)
        {
            if (ImGui::ColorPicker4("##pick", col,
                                    ImGuiColorEditFlags_AlphaBar |
                                        ImGuiColorEditFlags_NoSidePreview))
                changed = true;
        }
        else
        {
            if (ImGui::ColorPicker3("##pick", col, ImGuiColorEditFlags_NoSidePreview))
                changed = true;
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
        return false;

    const float dpi = ImGui::GetStyle().FontScaleDpi;

    // Capture the combo box rect from the cursor BEFORE BeginCombo. Once the
    // popup opens, ImGui's last-item rect points at the popup window (below the
    // box), so the preview/chevron would jump down if anchored via GetItemRect*.
    // We anchor them to this captured box and draw on the parent window's draw
    // list (w->DrawList), so they stay put whether the dropdown is open or shut.
    const ImVec2 boxMin = ImGui::GetCursorScreenPos();
    const ImVec2 boxMax(boxMin.x + ImGui::CalcItemWidth(), boxMin.y + ImGui::GetFrameHeight());

    // The global theme zeroes WindowPadding (panels manage their own), which left
    // the dropdown popup's options flush against the edges and stacked tightly,
    // reading as cramped/small. Give the popup a real inset. It's read at the
    // popup's Begin() inside BeginCombo, so we pop immediately after.
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
        // Every row, up to most of the window, rather than ImGui's eight: a
        // list that opens scrolled to its selection hides its first entries.
        // ImGui still widens it to the box.
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                            ImVec2(FLT_MAX, ImGui::GetMainViewport()->WorkSize.y * 0.6f));
    }

    // Empty preview + no native arrow: we draw the preview value (left-aligned
    // mono, like the drag fields) and a chevron ourselves so the dropdown reads
    // the same as the rest of the inspector.
    const bool open = ImGui::BeginCombo(id, "", ImGuiComboFlags_NoArrowButton);
    ImGui::PopStyleVar();  // WindowPadding (popup already baked it)

    if (open)
    {
        // Taller, padded option rows with a full-width hover highlight. A
        // Selectable's box extends by ItemSpacing.x/2 each side; ImGui forces the
        // combo popup's WindowPadding.x to FramePadding.x, so ItemSpacing.x set to
        // twice that makes the highlight bleed exactly to both window edges while
        // the label keeps its inset (same trick as the menu dropdowns). Popped in
        // SchematicEndCombo.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(2.0f * ImGui::GetStyle().FramePadding.x, 8.0f * dpi));
    }

    if (preview && preview[0])
    {
        const float chevronW = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x;
        DrawLeftAlignedValue(w->DrawList, boxMin, ImVec2(ImMax(boxMin.x, boxMax.x - chevronW), boxMax.y), preview);
    }

    // Right-aligned chevron, matching the section-header chevron - and turning
    // like it: down when the list is shut, flipped up while it is open, so the
    // box itself says whether the popup below belongs to it.
    {
        ImFont* ico = g_FontUI ? g_FontUI : ImGui::GetFont();
        const float icoSize = ImGui::GetFontSize();
        const ImVec2 cs = ico->CalcTextSizeA(icoSize, FLT_MAX, 0.0f, ICON_TI_CHEVRON_DOWN);
        const float pad = ImGui::GetStyle().FramePadding.x;
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(Palette::Dim);
        DrawTurningChevron(w->DrawList, ico, icoSize,
                           ImVec2(boxMax.x - pad - cs.x * 0.5f, (boxMin.y + boxMax.y) * 0.5f),
                           col, ICON_TI_CHEVRON_DOWN, openT, IM_PI);
    }

    return open;
}

void SchematicEndCombo()
{
    ImGui::PopStyleVar();  // ItemSpacing pushed in SchematicBeginCombo
    ImGui::EndCombo();
}

// Flat, toolbar-styled box: a transparent frame with a subtle white hover (the
// same alphas the toolbar cells use), no border, squared. FramePadding.y is set
// so the box is exactly `height` tall - matching the toolbar strip - while the
// schematic preview value + chevron center within it. The styles stay pushed
// while the list is open and are popped by SchematicEndToolbarCombo.
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
        w->DrawList->AddLine(ImVec2(boxMin.x, boxMin.y), ImVec2(boxMin.x, boxMin.y + height),
                             ImGui::ColorConvertFloat4ToU32(Palette::Line2), 1.0f);

    if (!open)
        PopToolbarComboStyle();
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

bool SchematicToolbarCombo(const char* id, int* currentIndex, const char* items, float height,
                           bool leftDivider)
{
    if (!currentIndex || !items)
        return false;

    // Parse the "A\0B\0" zero-separated item list.
    const char* item[64];
    int count = 0;
    for (const char* p = items; p && *p && count < 64; p += strlen(p) + 1)
        item[count++] = p;
    if (count == 0)
        return false;

    const char* preview = (*currentIndex >= 0 && *currentIndex < count) ? item[*currentIndex] : "";

    bool changed = false;
    if (SchematicBeginToolbarCombo(id, preview, height, leftDivider))
    {
        for (int i = 0; i < count; ++i)
        {
            const bool selected = (i == *currentIndex);
            if (ImGui::Selectable(item[i], selected, ImGuiSelectableFlags_SpanAvailWidth) &&
                *currentIndex != i)
            {
                *currentIndex = i;
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        SchematicEndToolbarCombo();
    }
    return changed;
}

bool SchematicCombo(const char* id, int* currentIndex, const char* const items[], int count)
{
    if (!currentIndex || !items || count <= 0)
        return false;

    const char* preview = (*currentIndex >= 0 && *currentIndex < count && items[*currentIndex])
                              ? items[*currentIndex]
                              : "";

    bool changed = false;
    if (SchematicBeginCombo(id, preview))
    {
        for (int i = 0; i < count; ++i)
        {
            const bool selected = (i == *currentIndex);
            // SpanAvailWidth so the option's hover/selected highlight fills the
            // whole row width (combined with the full-bleed ItemSpacing pushed in
            // SchematicBeginCombo, it bleeds to both popup edges).
            if (ImGui::Selectable(items[i] ? items[i] : "", selected,
                                  ImGuiSelectableFlags_SpanAvailWidth) &&
                *currentIndex != i)
            {
                *currentIndex = i;
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
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
        items[count++] = p;
    return SchematicCombo(id, currentIndex, items, count);
}

bool SchematicActionLink(const char* label)
{
    // Right-aligned accent link with a 1px underline, anchored to the right
    // edge of the header band it follows (call immediately after the header).
    // Only the text before the "##id" marker is drawn; the full string is the
    // button id. The hit box spans the band height so vertical layout below it
    // advances exactly as it would without the link.
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems)
        return false;

    // The preceding item is the header band (nothing else added between).
    const ImVec2 bandMin = ImGui::GetItemRectMin();
    const ImVec2 bandMax = ImGui::GetItemRectMax();
    const float bandH = bandMax.y - bandMin.y;

    // Visible text stops at the "##" id marker.
    const char* visEnd = label;
    while (visEnd[0] != '\0' && !(visEnd[0] == '#' && visEnd[1] == '#'))
        ++visEnd;
    const ImVec2 sz = ImGui::CalcTextSize(label, visEnd);

    // Anchor to the right content edge (before the scrollbar), one frame
    // padding in from the seam.
    const float pad = ImGui::GetStyle().FramePadding.x;
    const float x = w->WorkRect.Max.x - sz.x - pad;

    ImGui::SetCursorScreenPos(ImVec2(x, bandMin.y));
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(sz.x, bandH));
    const bool hovered = ImGui::IsItemHovered();

    const ImU32 col = ImGui::ColorConvertFloat4ToU32(
        hovered ? Palette::Accent : Palette::AccentDim);
    ImDrawList* dl = w->DrawList;
    const float textY = ImFloor(bandMin.y + (bandH - sz.y) * 0.5f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, textY), col, label, visEnd);
    dl->AddLine(ImVec2(x, textY + sz.y), ImVec2(x + sz.x, textY + sz.y), col, 1.0f);
    return clicked;
}

}  // namespace DekiEditor
