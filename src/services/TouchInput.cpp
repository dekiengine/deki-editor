#include "TouchInput.h"

#include <SDL3/SDL.h>

#include <cfloat>

#include "imgui_internal.h"

namespace DekiEditor
{

TouchInput& TouchInput::Get()
{
    static TouchInput s_Instance;
    return s_Instance;
}

void TouchInput::DisableSdlTouchMouse()
{
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
}

void TouchInput::RestoreSdlTouchMouse()
{
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
}

static double NowSeconds()
{
    return static_cast<double>(SDL_GetTicksNS()) / 1e9;
}

bool TouchInput::ToWindow(const SDL_Event& event, float& x, float& y) const
{
    if (!m_Window)
        return false;
    // A trackpad reports touches too, but it also moves the mouse itself;
    // only a screen's touches are the user touching the editor.
    if (SDL_GetTouchDeviceType(event.tfinger.touchID) != SDL_TOUCH_DEVICE_DIRECT)
        return false;
    if (event.tfinger.windowID != 0 && event.tfinger.windowID != SDL_GetWindowID(m_Window))
        return false;
    int w = 0, h = 0;
    if (!SDL_GetWindowSize(m_Window, &w, &h) || w <= 0 || h <= 0)
        return false;
    // Normalized to the window; ImGui pixels are SDL window coordinates.
    x = event.tfinger.x * static_cast<float>(w);
    y = event.tfinger.y * static_cast<float>(h);
    return true;
}

bool TouchInput::InDirectRegion(float x, float y) const
{
    for (const Rect& r : m_Direct)
        if (r.Contains(x, y))
            return true;
    return false;
}

void TouchInput::ProcessEvent(const SDL_Event& event)
{
    float x = 0.0f, y = 0.0f;
    switch (event.type)
    {
    case SDL_EVENT_FINGER_DOWN:
        if (ToWindow(event, x, y))
            m_Gestures.FingerDown(event.tfinger.fingerID, x, y, NowSeconds(), InDirectRegion(x, y));
        break;
    case SDL_EVENT_FINGER_MOTION:
        if (ToWindow(event, x, y))
            m_Gestures.FingerMove(event.tfinger.fingerID, x, y);
        break;
    case SDL_EVENT_FINGER_UP:
        if (ToWindow(event, x, y))
            m_Gestures.FingerUp(event.tfinger.fingerID, x, y);
        break;
    case SDL_EVENT_FINGER_CANCELED:
        m_Gestures.CancelAll();
        break;
    default:
        break;
    }
}

void TouchInput::ScrollUnclaimed()
{
    if (!m_Frame.active || m_FrameClaimed || (m_Frame.panX == 0.0f && m_Frame.panY == 0.0f))
        return;

    // The window a wheel would scroll: the one under the fingers, or the
    // nearest parent that can scroll, skipping any that opt out of wheel
    // scrolling (a canvas that pans itself). Scrolled directly rather than
    // through a wheel event so the content follows the fingers pixel for
    // pixel: a wheel notch is five lines, whatever the fingers did.
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (ImGuiWindow* w = g.HoveredWindow; w; w = w->ParentWindow)
    {
        const bool optedOut = (w->Flags & (ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMouseInputs)) != 0;
        const bool canX = w->ScrollMax.x > 0.0f && m_Frame.panX != 0.0f;
        const bool canY = w->ScrollMax.y > 0.0f && m_Frame.panY != 0.0f;
        if (!optedOut && (canX || canY))
        {
            // Fingers moving down pull the content down, toward the top.
            if (canX)
                ImGui::SetScrollX(w, w->Scroll.x - m_Frame.panX);
            if (canY)
                ImGui::SetScrollY(w, w->Scroll.y - m_Frame.panY);
            return;
        }
        if (!(w->Flags & ImGuiWindowFlags_ChildWindow))
            return;
    }
}

void TouchInput::InjectPending()
{
    // Last frame's gesture, if no panel claimed it, scrolls. It is known to
    // be unclaimed only now, after every panel has drawn.
    ScrollUnclaimed();

    // A finger that lands before the next frame's panels draw is tested
    // against where they were this frame.
    m_Direct.swap(m_DirectNext);
    m_DirectNext.clear();

    // Slop in the same pixels as the positions, so it is a finger's width of
    // wobble at any display scale.
    TouchGestures::Config config = m_Gestures.GetConfig();
    config.tapSlop = 10.0f * ImMax(ImGui::GetStyle().FontScaleDpi, 1.0f);
    m_Gestures.SetConfig(config);

    TouchGestures::Output out = m_Gestures.Update(NowSeconds());
    m_Frame = out.gesture;
    m_FrameClaimed = false;

    if (out.mouse.empty() && !m_Gestures.OwnsPointer())
        return;

    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    if (out.mouse.empty())
    {
        // Nothing new, but a touch still owns the pointer: hold it there
        // against the backend's report of the real cursor.
        io.AddMousePosEvent(m_Gestures.PointerX(), m_Gestures.PointerY());
        return;
    }
    for (const TouchGestures::MouseEvent& e : out.mouse)
    {
        switch (e.kind)
        {
        case TouchGestures::MouseEvent::Kind::Move:
            io.AddMousePosEvent(e.x, e.y);
            break;
        case TouchGestures::MouseEvent::Kind::Down:
            io.AddMouseButtonEvent(e.button, true);
            break;
        case TouchGestures::MouseEvent::Kind::Up:
            io.AddMouseButtonEvent(e.button, false);
            break;
        case TouchGestures::MouseEvent::Kind::Leave:
            // Nothing hovers once the finger is gone. ImGui's input queue
            // spreads this over frames after the release, so the release
            // still lands on what was under the finger.
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            break;
        }
    }
}

bool TouchInput::ClaimGesture(const ImVec2& min, const ImVec2& max, Gesture& out)
{
    if (!m_Frame.active || m_FrameClaimed)
        return false;
    const Rect r{ min, max };
    if (!r.Contains(m_Frame.centerX, m_Frame.centerY))
        return false;
    m_FrameClaimed = true;
    out.center = ImVec2(m_Frame.centerX, m_Frame.centerY);
    out.pan = ImVec2(m_Frame.panX, m_Frame.panY);
    out.scale = m_Frame.scale;
    return true;
}

void TouchInput::AddDirectRegion(const ImVec2& min, const ImVec2& max)
{
    m_DirectNext.push_back({ min, max });
}

}  // namespace DekiEditor
