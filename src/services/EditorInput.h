#pragma once

#include <deque>
#include <string>

namespace DekiEditor
{

// Mouse and keyboard input for automation (the MCP click, hover, scroll, key
// and type_text tools). It goes through Dear ImGui's own input queue, the same
// path SDL's events take, so it works whether or not the editor's window has
// focus, and it never moves the real cursor.
//
// Requests are queued like EditorCapture's and played one step per frame by
// InjectPending(), which main.cpp calls after ImGui_ImplSDL3_NewFrame() and
// before ImGui::NewFrame(). After, so a step's mouse position comes after the
// backend's report of the real cursor and wins; before, so ImGui sees it this
// frame. A click is three steps (move, press, release) because a widget reacts
// to a press only once the mouse has been over it for a frame.
//
// Positions are in ImGui pixels, the same pixels as an EditorCapture
// screenshot: relative to a panel's top-left corner when a panel is named,
// else to the window's.

/// Modifier keys held down for the whole of a click or drag. Outside the
/// class so it can be a default argument of its members.
struct InputModifiers
{
    bool ctrl = false, shift = false, alt = false;
    bool Any() const { return ctrl || shift || alt; }
};

class EditorInput
{
public:
    static EditorInput& Get();

    enum class Button
    {
        Left,
        Right,
        Middle
    };

    using Modifiers = InputModifiers;

    /// Moves the mouse to (x, y) and clicks there. `count` 2 is a double click.
    /// `holdSeconds` keeps the button down that long before releasing, for
    /// press states and long presses.
    void RequestClick(const std::string& panel, float x, float y, Button button, int count, float holdSeconds = 0.0f,
                      Modifiers mods = {});
    /// Presses at (x, y) in `panel`, moves to (toX, toY) in `toPanel` over
    /// `frames` frames with the button held, waits `holdSeconds` there (a drop
    /// target or dock overlay reacts once the pointer rests on it), then
    /// releases. Both points are resolved when the drag starts, because
    /// dragging a tab moves the panel it was grabbed from.
    void RequestDrag(const std::string& panel, float x, float y, const std::string& toPanel, float toX, float toY,
                     Button button, int frames, float holdSeconds, Modifiers mods = {});
    /// Moves the mouse to (x, y) and holds it there for `holdSeconds`, for
    /// tooltips and hover states. It is held, not just moved, because with the
    /// window focused the backend reports the real cursor every frame and would
    /// move it away. Seconds, not frames: the frame rate varies, and a
    /// tooltip's delay is a time.
    void RequestHover(const std::string& panel, float x, float y, float holdSeconds);
    /// Moves the mouse to (x, y) and turns the wheel; positive `dy` scrolls up.
    void RequestScroll(const std::string& panel, float x, float y, float dx, float dy);
    /// Presses and releases one key, with modifiers held. `key` is ImGui's name
    /// for it ("Enter", "Escape", "Tab", "A", "F2", "LeftArrow", ...). Returns
    /// false, with `error` set, when the name is not a key.
    bool RequestKey(const std::string& key, bool ctrl, bool shift, bool alt, std::string& error);
    /// Types text into whatever has keyboard focus.
    void RequestText(const std::string& text);

    /// Called once per frame; see the comment at the top of this file.
    void InjectPending();

    /// True while steps are still to be played. A screenshot waits for this, so
    /// one asked for after a click shows what the click did. A hover's hold
    /// does not count: a screenshot is meant to be taken during it.
    bool HasPendingWork() const { return !m_Steps.empty(); }

    /// Whether `panel` names an open window, so the tools can reject a typo at once.
    static bool PanelExists(const std::string& panel);

    /// Where automation holds the mouse while its steps play (window pixels),
    /// and whether a button is down; false when it is not driving the mouse.
    /// The agent pointer follows it, so a drag is seen being dragged.
    bool LivePointer(float& x, float& y, bool& pressed) const
    {
        if (m_Steps.empty() && m_HoldSeconds <= 0.0f && !m_Pressed)
        {
            return false;
        }
        x = m_MouseX;
        y = m_MouseY;
        pressed = m_Pressed;
        return true;
    }

private:
    EditorInput() = default;

    struct Step
    {
        enum class Kind
        {
            Move,
            ButtonDown,
            ButtonUp,
            Wheel,
            KeyDown,
            KeyUp,
            Text,
            Hold,
            Wait,      // keep the mouse where it is and play nothing else for `seconds`
            Mods,      // modifier keys `ctrl`/`shift`/`alt` go down (`down`) or up
            DragBegin  // expands into the steps of a drag once both points are known
        } kind = Kind::Move;
        std::string panel;
        float x = 0.0f, y = 0.0f;  // Move: position; Wheel: dx, dy
        int button = 0;
        int key = 0;           // ImGuiKey
        float seconds = 0.0f;  // Hold, Wait
        bool ctrl = false, shift = false, alt = false;
        bool down = false;           // Mods
        std::string panel2;          // DragBegin: the end point's panel
        float x2 = 0.0f, y2 = 0.0f;  // DragBegin: the end point
        int frames = 0;              // DragBegin: frames to move over
        std::string text;
    };

    static Step ModsStep(const Modifiers& mods, bool down);

    void Move(const std::string& panel, float x, float y);

    std::deque<Step> m_Steps;
    // Where the last Move put the mouse, repeated before each later mouse step.
    // With the window focused the backend reports the real cursor every frame,
    // and a press would otherwise land wherever that is.
    float m_MouseX = 0.0f, m_MouseY = 0.0f;
    float m_HoldSeconds = 0.0f;  // time left to keep the mouse at m_MouseX/Y
    bool m_Pressed = false;      // a button is down (between a ButtonDown and its ButtonUp)
};

}  // namespace DekiEditor
