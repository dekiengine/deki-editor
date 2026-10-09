#include "AgentCursor.h"

#include "EditorCapture.h"
#include "EditorInput.h"

#include <deki-editor/EditorSettings.h>
#include <deki-editor/EditorTheme.h>
#include <deki-editor/Motion.h>

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>

namespace DekiEditor
{

namespace
{

// Seconds. Long enough to follow with the eye, short enough that an agent
// making many edits is not held up much: a call waits kTravel + kDwell, 0.2 s.
constexpr float kTravel = 0.15f;
constexpr float kDwell = 0.05f;
// Reduce Motion: no glide, but the same short rest on the target, so the user
// still sees where the edit lands before it does.
constexpr float kReducedDwell = 0.20f;
// Input into a running game is not held, so the pointer catches up with the
// injected mouse quickly instead of gliding ahead of it.
constexpr float kCatchUp = 0.10f;
// After the last call the pointer stays this long, then fades out.
constexpr float kLinger = 2.5f;
constexpr float kFade = 0.4f;
// The ring drawn where the pointer lands.
constexpr float kRipple = 0.35f;

ImU32 Col(ImVec4 c, float alpha)
{
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

float EaseOutCubic(float t)
{
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

ImGuiWindow* DrawnWindow(const std::string& name)
{
    if (name.empty())
    {
        return nullptr;
    }
    ImGuiWindow* win = ImGui::FindWindowByName(name.c_str());
    return (win && win->WasActive && !win->Hidden) ? win : nullptr;
}

// Where the tip goes on a rect: a row is pointed at near its left end, where
// its name is; a panel at its middle.
ImVec2 AnchorIn(const ImVec2& min, const ImVec2& max, bool row)
{
    const float w = max.x - min.x, h = max.y - min.y;
    if (row)
    {
        return ImVec2(min.x + std::min(w * 0.3f, 56.0f * ImGui::GetStyle().FontScaleDpi), min.y + h * 0.5f);
    }
    return ImVec2(min.x + w * 0.5f, min.y + std::min(h * 0.5f, 120.0f * ImGui::GetStyle().FontScaleDpi));
}

void DrawPointer(ImDrawList* dl, ImVec2 tip, float s, float alpha)
{
    // The classic arrow, tip at (0, 0), pointing up and to the left.
    static const ImVec2 kShape[] = { { 0.0f, 0.0f },  { 0.0f, 17.0f }, { 4.4f, 13.2f }, { 7.4f, 19.6f },
                                     { 10.2f, 18.4f }, { 7.3f, 12.2f }, { 12.6f, 12.2f } };
    constexpr int kCount = static_cast<int>(sizeof(kShape) / sizeof(kShape[0]));
    ImVec2 pts[kCount], shadow[kCount];
    for (int i = 0; i < kCount; ++i)
    {
        pts[i] = ImVec2(tip.x + kShape[i].x * s, tip.y + kShape[i].y * s);
        shadow[i] = ImVec2(pts[i].x + 1.5f * s, pts[i].y + 2.0f * s);
    }
    dl->AddConcavePolyFilled(shadow, kCount, Col(ImVec4(0, 0, 0, 1), 0.35f * alpha));
    dl->AddConcavePolyFilled(pts, kCount, Col(Palette::Agent, alpha));
    dl->AddPolyline(pts, kCount, Col(ImVec4(1, 1, 1, 1), 0.95f * alpha), ImDrawFlags_Closed, 1.4f * s);
}

void DrawLabel(ImDrawList* dl, ImVec2 tip, const std::string& text, float s, float alpha)
{
    const ImVec2 pad(8.0f * s, 4.0f * s);
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 box(size.x + pad.x * 2.0f, size.y + pad.y * 2.0f);
    // Below and to the right of the arrow, flipped to stay on screen.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 pos(tip.x + 14.0f * s, tip.y + 20.0f * s);
    if (pos.x + box.x > vp->Pos.x + vp->Size.x - 4.0f * s)
    {
        pos.x = tip.x - box.x - 4.0f * s;
    }
    if (pos.y + box.y > vp->Pos.y + vp->Size.y - 4.0f * s)
    {
        pos.y = tip.y - box.y - 6.0f * s;
    }
    pos.x = std::max(pos.x, vp->Pos.x + 4.0f * s);
    pos.y = std::max(pos.y, vp->Pos.y + 4.0f * s);
    const ImVec2 end(pos.x + box.x, pos.y + box.y);
    const float r = box.y * 0.5f;
    dl->AddRectFilled(ImVec2(pos.x + 1.0f * s, pos.y + 2.0f * s), ImVec2(end.x + 1.0f * s, end.y + 2.0f * s),
                      Col(ImVec4(0, 0, 0, 1), 0.30f * alpha), r);
    dl->AddRectFilled(pos, end, Col(Palette::Panel, 0.96f * alpha), r);
    dl->AddRect(pos, end, Col(Palette::Agent, 0.90f * alpha), r, 0, 1.0f * s);
    dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), Col(Palette::Fg, alpha), text.c_str());
}

}  // namespace

AgentCursor& AgentCursor::Get()
{
    static AgentCursor s_Instance;
    return s_Instance;
}

bool AgentCursor::IsEnabled()
{
    return EditorSettings::GetBool(kSettingEnabled, true);
}

void AgentCursor::SetEnabled(bool enabled)
{
    EditorSettings::SetBool(kSettingEnabled, enabled);
    if (!enabled)
    {
        Get().m_Active = false;
    }
}

float AgentCursor::SecondsSince(Clock::time_point t) const
{
    return std::chrono::duration<float>(Clock::now() - t).count();
}

void AgentCursor::Show(const AgentIntent& intent, bool wait)
{
    m_Intent = intent;
    m_Active = true;
    m_Wait = wait;
    m_ShownAt = Clock::now();
    m_BusyAge = 0.0f;
    const bool reduce = Motion::ReduceMotion();
    // Not waiting, the input is already playing: a quick catch-up, not a glide.
    m_Travel = (intent.Travels() && !reduce) ? (wait ? kTravel : kCatchUp) : 0.0f;
    m_Dwell = (intent.Travels() && wait) ? (reduce ? kReducedDwell : kDwell) : 0.0f;
    m_From = m_Tip;
}

bool AgentCursor::Arrived() const
{
    return !m_Active || !m_Wait || !m_Intent.Travels() || SecondsSince(m_ShownAt) >= m_Travel + m_Dwell;
}

bool AgentCursor::WantsReveal() const
{
    return m_Active && m_Intent.Travels() && SecondsSince(m_ShownAt) < m_Travel + m_Dwell + 0.25f;
}

void AgentCursor::Report(const ImVec2& min, const ImVec2& max, Precision precision)
{
    if (!m_HaveReport || precision > m_ReportPrecision)
    {
        m_HaveReport = true;
        m_ReportPrecision = precision;
        m_ReportMin = min;
        m_ReportMax = max;
    }
}

void AgentCursor::Draw()
{
    // Reports are per frame, whatever happens below.
    const bool haveReport = m_HaveReport;
    const ImVec2 reportMin = m_ReportMin, reportMax = m_ReportMax;
    m_HaveReport = false;

    if (!m_Active)
    {
        return;
    }
    const float age = SecondsSince(m_ShownAt);
    // The linger counts from the end of the glide, or from the last frame the
    // input was still playing: a long drag or hover keeps its pointer.
    float lx = 0.0f, ly = 0.0f;
    bool pressed = false;
    const bool live = m_Intent.target == AgentIntent::Target::Point && EditorInput::Get().LivePointer(lx, ly, pressed);
    if (live)
    {
        m_BusyAge = age;
    }
    const float settled = std::max(m_Travel + m_Dwell, m_BusyAge);
    if (age > settled + kLinger + kFade)
    {
        m_Active = false;
        return;
    }
    const float alpha = std::clamp(1.0f - (age - settled - kLinger) / kFade, 0.0f, 1.0f);
    const float s = ImGui::GetStyle().FontScaleDpi;
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    // Where it first appears: by the status bar's MCP button, where the agent
    // comes in.
    if (m_Tip.x < 0.0f)
    {
        m_Tip = m_From = ImVec2(vp->Pos.x + vp->Size.x - 120.0f * s, vp->Pos.y + vp->Size.y - 40.0f * s);
    }

    // The target this frame: the item, a stand-in for it, or its panel.
    using T = AgentIntent::Target;
    bool haveRect = false, isRow = false;
    ImVec2 rmin, rmax, aim = m_Tip;
    if (m_Intent.target == T::Point)
    {
        // Once the input plays, the pointer is wherever it holds the mouse,
        // so a drag is seen being dragged and a hold being held.
        if (live)
        {
            aim = ImVec2(lx, ly);
        }
        else
        {
            float ox = 0.0f, oy = 0.0f;
            bool known = m_Intent.panel.empty();
            if (ImGuiWindow* win = DrawnWindow(m_Intent.panel))
            {
                ox = win->Pos.x;
                oy = win->Pos.y;
                known = true;
            }
            if (known)
            {
                aim = ImVec2(ox + m_Intent.x, oy + m_Intent.y);
            }
        }
    }
    else if (m_Intent.Travels())
    {
        if (haveReport)
        {
            rmin = reportMin, rmax = reportMax, haveRect = isRow = true;
        }
        else if (ImGuiWindow* win = DrawnWindow(m_Intent.panel))
        {
            rmin = win->Pos;
            rmax = ImVec2(win->Pos.x + win->Size.x, win->Pos.y + win->Size.y);
            haveRect = true;
        }
        if (haveRect)
        {
            aim = AnchorIn(rmin, rmax, isRow);
        }
    }

    // Glide from where the last call left it. The target is looked up every
    // frame, so the pointer follows a row that scrolls into view on the way.
    const float t = m_Travel > 0.0f ? std::clamp(age / m_Travel, 0.0f, 1.0f) : 1.0f;
    const float e = EaseOutCubic(t);
    m_Tip = ImVec2(m_From.x + (aim.x - m_From.x) * e, m_From.y + (aim.y - m_From.y) * e);

    // Never in a frame a screenshot reads (see the class comment).
    if (EditorCapture::Get().HasPendingWork())
    {
        return;
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (haveRect && m_Intent.Travels())
    {
        // The item gets an outline that brightens as the pointer lands.
        const float glow = 0.35f + 0.65f * e;
        const float pad = (isRow ? 1.0f : 0.0f) * s;
        dl->AddRect(ImVec2(rmin.x - pad, rmin.y - pad), ImVec2(rmax.x + pad, rmax.y + pad),
                    Col(Palette::Agent, glow * alpha * (isRow ? 0.9f : 0.6f)), 4.0f * s, 0,
                    (isRow ? 1.5f : 1.0f) * s);
        if (isRow)
        {
            dl->AddRectFilled(rmin, rmax, Col(Palette::Agent, 0.10f * glow * alpha), 4.0f * s);
        }
    }
    if (m_Intent.Travels() && !Motion::ReduceMotion() && age >= m_Travel && age < m_Travel + kRipple)
    {
        const float u = (age - m_Travel) / kRipple;
        dl->AddCircle(m_Tip, (4.0f + 16.0f * u) * s, Col(Palette::Agent, (1.0f - u) * 0.8f * alpha), 0, 2.0f * s);
    }
    // Button held: a fingertip under the arrow, as a touch would look.
    if (pressed)
    {
        dl->AddCircleFilled(m_Tip, 9.0f * s, Col(Palette::Agent, 0.35f * alpha));
        dl->AddCircle(m_Tip, 9.0f * s, Col(Palette::Agent, 0.9f * alpha), 0, 1.5f * s);
    }
    DrawPointer(dl, m_Tip, s, alpha);
    if (!m_Intent.label.empty())
    {
        DrawLabel(dl, m_Tip, m_Intent.label, s, alpha);
    }
}

}  // namespace DekiEditor
