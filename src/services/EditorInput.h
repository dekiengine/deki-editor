#pragma once

#include <deque>
#include <string>

namespace DekiEditor
{

// Mouse and keyboard input for automation (the MCP click, hover, scroll, key
// and type_text tools), fed through Dear ImGui's own input queue - the same
// path SDL's events take - so it works whether or not the editor's window has
// focus, and it never moves the real cursor.
//
// Requests are queued like EditorCapture's and played out one step per frame
// by InjectPending(), which main.cpp calls after ImGui_ImplSDL3_NewFrame() and
// before ImGui::NewFrame(): after, so a step's mouse position lands after the
// backend's own report of where the real cursor is and wins; before, so ImGui
// sees it this frame. A click is three steps (move, press, release) because a
// widget reacts to a press only once the mouse has been over it for a frame.
//
// Positions are in ImGui pixels, the same pixels as an EditorCapture
// screenshot: relative to a panel's top-left corner when a panel is named,
// else to the window's.
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

    // Move the mouse to (x, y) and click there. `count` 2 is a double click.
    void RequestClick(const std::string& panel, float x, float y, Button button, int count);
    // Move the mouse to (x, y) and hold it there for `holdSeconds` (for
    // tooltips and hover states). Held, not just moved: with the window focused
    // the backend reports the real cursor every frame and would move it away.
    // Seconds, not frames: the editor's frame rate varies, and a tooltip's
    // delay is a time.
    void RequestHover(const std::string& panel, float x, float y, float holdSeconds);
    // Move the mouse to (x, y) and turn the wheel; positive `dy` scrolls up.
    void RequestScroll(const std::string& panel, float x, float y, float dx, float dy);
    // Press and release one key, with modifiers held. `key` is ImGui's name for
    // it ("Enter", "Escape", "Tab", "A", "F2", "LeftArrow", ...). False, and
    // `error` says why, when the name is not a key.
    bool RequestKey(const std::string& key, bool ctrl, bool shift, bool alt, std::string& error);
    // Type text into whatever has keyboard focus.
    void RequestText(const std::string& text);

    // Called once per frame; see the class comment.
    void InjectPending();

    // True while steps are still to be played. A screenshot waits for this, so
    // one asked for after a click shows what the click did. A hover's hold is
    // not pending work: a screenshot is meant to be taken during it.
    bool HasPendingWork() const { return !m_Steps.empty(); }

    // Whether `panel` names a live window, for the tools to reject a typo at once.
    static bool PanelExists(const std::string& panel);

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
            Hold
        } kind = Kind::Move;
        std::string panel;
        float x = 0.0f, y = 0.0f;  // Move: position; Wheel: dx, dy
        int button = 0;
        int key = 0;               // ImGuiKey
        float seconds = 0.0f;      // Hold
        bool ctrl = false, shift = false, alt = false;
        std::string text;
    };

    void Move(const std::string& panel, float x, float y);

    std::deque<Step> m_Steps;
    // Where the last Move put the mouse, repeated before each later mouse step:
    // with the window focused the backend reports the real cursor every frame,
    // and without this a press would land wherever that is.
    float m_MouseX = 0.0f, m_MouseY = 0.0f;
    float m_HoldSeconds = 0.0f;  // time left to keep the mouse at m_MouseX/Y
};

}  // namespace DekiEditor
