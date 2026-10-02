#include "EditorInput.h"

#include <deki/LogSystem.h>

#include "imgui.h"
#include "imgui_internal.h"

namespace DekiEditor
{

EditorInput& EditorInput::Get()
{
    static EditorInput s_Instance;
    return s_Instance;
}

bool EditorInput::PanelExists(const std::string& panel)
{
    if (panel.empty())
        return true;
    ImGuiWindow* win = ImGui::FindWindowByName(panel.c_str());
    return win && win->WasActive;
}

void EditorInput::Move(const std::string& panel, float x, float y)
{
    Step s;
    s.kind = Step::Kind::Move;
    s.panel = panel;
    s.x = x;
    s.y = y;
    m_Steps.push_back(s);
}

EditorInput::Step EditorInput::ModsStep(const Modifiers& mods, bool down)
{
    Step s;
    s.kind = Step::Kind::Mods;
    s.ctrl = mods.ctrl;
    s.shift = mods.shift;
    s.alt = mods.alt;
    s.down = down;
    return s;
}

void EditorInput::RequestDrag(const std::string& panel, float x, float y, const std::string& toPanel, float toX,
                              float toY, Button button, int frames, float holdSeconds, Modifiers mods)
{
    Step s;
    s.kind = Step::Kind::DragBegin;
    s.panel = panel;
    s.x = x;
    s.y = y;
    s.panel2 = toPanel;
    s.x2 = toX;
    s.y2 = toY;
    s.button = static_cast<int>(button);
    s.frames = frames < 2 ? 2 : frames;
    s.seconds = holdSeconds;
    s.ctrl = mods.ctrl;
    s.shift = mods.shift;
    s.alt = mods.alt;
    m_Steps.push_back(s);
}

void EditorInput::RequestClick(const std::string& panel, float x, float y, Button button, int count,
                               float holdSeconds, Modifiers mods)
{
    Move(panel, x, y);
    if (mods.Any())
        m_Steps.push_back(ModsStep(mods, true));
    for (int i = 0; i < count; ++i)
    {
        Step down;
        down.kind = Step::Kind::ButtonDown;
        down.button = static_cast<int>(button);
        m_Steps.push_back(down);
        if (holdSeconds > 0.0f)
        {
            Step wait;
            wait.kind = Step::Kind::Wait;
            wait.seconds = holdSeconds;
            m_Steps.push_back(wait);
        }
        Step up = down;
        up.kind = Step::Kind::ButtonUp;
        m_Steps.push_back(up);
    }
    if (mods.Any())
        m_Steps.push_back(ModsStep(mods, false));
}

void EditorInput::RequestHover(const std::string& panel, float x, float y, float holdSeconds)
{
    Move(panel, x, y);
    Step s;
    s.kind = Step::Kind::Hold;
    s.seconds = holdSeconds;
    m_Steps.push_back(s);
}

void EditorInput::RequestScroll(const std::string& panel, float x, float y, float dx, float dy)
{
    Move(panel, x, y);
    Step s;
    s.kind = Step::Kind::Wheel;
    s.x = dx;
    s.y = dy;
    m_Steps.push_back(s);
}

bool EditorInput::RequestKey(const std::string& key, bool ctrl, bool shift, bool alt, std::string& error)
{
    int found = ImGuiKey_None;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
    {
        const char* name = ImGui::GetKeyName(static_cast<ImGuiKey>(k));
        if (name && key == name)
        {
            found = k;
            break;
        }
    }
    if (found == ImGuiKey_None)
    {
        error = "'" + key + "' is not a key; use ImGui's names: Enter, Escape, Tab, Space, Backspace, "
                "Delete, LeftArrow, A, 0, F2, ...";
        return false;
    }
    Step down;
    down.kind = Step::Kind::KeyDown;
    down.key = found;
    down.ctrl = ctrl;
    down.shift = shift;
    down.alt = alt;
    m_Steps.push_back(down);
    Step up = down;
    up.kind = Step::Kind::KeyUp;
    m_Steps.push_back(up);
    return true;
}

void EditorInput::RequestText(const std::string& text)
{
    Step s;
    s.kind = Step::Kind::Text;
    s.text = text;
    m_Steps.push_back(s);
}

void EditorInput::InjectPending()
{
    ImGuiIO& io = ImGui::GetIO();
    if (m_Steps.empty())
    {
        if (m_HoldSeconds > 0.0f)
        {
            m_HoldSeconds -= io.DeltaTime;  // this frame's, set by the backend already
            io.AddMousePosEvent(m_MouseX, m_MouseY);
        }
        return;
    }
    m_HoldSeconds = 0.0f;  // new input ends a hover

    // A wait blocks the queue until its time has passed, keeping the mouse put.
    if (m_Steps.front().kind == Step::Kind::Wait)
    {
        io.AddMousePosEvent(m_MouseX, m_MouseY);
        m_Steps.front().seconds -= io.DeltaTime;
        if (m_Steps.front().seconds <= 0.0f)
            m_Steps.pop_front();
        return;
    }

    const Step step = m_Steps.front();
    m_Steps.pop_front();

    switch (step.kind)
    {
        case Step::Kind::Move:
        {
            float ox = 0.0f, oy = 0.0f;
            if (!step.panel.empty())
            {
                // Resolved now, not when asked: the panel may have moved since.
                ImGuiWindow* win = ImGui::FindWindowByName(step.panel.c_str());
                if (!win || !win->WasActive)
                {
                    DEKI_LOG_ERROR("EditorInput: panel '%s' is not drawn; dropping the rest of this input",
                                   step.panel.c_str());
                    m_Steps.clear();
                    return;
                }
                ox = win->Pos.x;
                oy = win->Pos.y;
            }
            m_MouseX = ox + step.x;
            m_MouseY = oy + step.y;
            io.AddMousePosEvent(m_MouseX, m_MouseY);
            break;
        }
        case Step::Kind::ButtonDown:
        case Step::Kind::ButtonUp:
            io.AddMousePosEvent(m_MouseX, m_MouseY);
            io.AddMouseButtonEvent(step.button, step.kind == Step::Kind::ButtonDown);
            break;
        case Step::Kind::Wheel:
            io.AddMousePosEvent(m_MouseX, m_MouseY);
            io.AddMouseWheelEvent(step.x, step.y);
            break;
        case Step::Kind::KeyDown:
        case Step::Kind::KeyUp:
        {
            const bool down = step.kind == Step::Kind::KeyDown;
            // Modifiers go down before the key and come up with it.
            if (step.ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, down);
            if (step.shift) io.AddKeyEvent(ImGuiMod_Shift, down);
            if (step.alt) io.AddKeyEvent(ImGuiMod_Alt, down);
            io.AddKeyEvent(static_cast<ImGuiKey>(step.key), down);
            break;
        }
        case Step::Kind::Text:
            io.AddInputCharactersUTF8(step.text.c_str());
            break;
        case Step::Kind::Hold:
            io.AddMousePosEvent(m_MouseX, m_MouseY);
            m_HoldSeconds = step.seconds;
            break;
        case Step::Kind::Wait:
            break;  // handled above, before the step is taken off the queue
        case Step::Kind::Mods:
            if (step.ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, step.down);
            if (step.shift) io.AddKeyEvent(ImGuiMod_Shift, step.down);
            if (step.alt) io.AddKeyEvent(ImGuiMod_Alt, step.down);
            break;
        case Step::Kind::DragBegin:
        {
            // Both ends to window pixels, now.
            auto resolve = [](const std::string& panel, float x, float y, float& outX, float& outY) -> bool
            {
                float ox = 0.0f, oy = 0.0f;
                if (!panel.empty())
                {
                    ImGuiWindow* win = ImGui::FindWindowByName(panel.c_str());
                    if (!win || !win->WasActive)
                        return false;
                    ox = win->Pos.x;
                    oy = win->Pos.y;
                }
                outX = ox + x;
                outY = oy + y;
                return true;
            };
            float ax, ay, bx, by;
            if (!resolve(step.panel, step.x, step.y, ax, ay) || !resolve(step.panel2, step.x2, step.y2, bx, by))
            {
                DEKI_LOG_ERROR("EditorInput: a drag's panel is not drawn; dropping the rest of this input");
                m_Steps.clear();
                return;
            }
            // This frame puts the pointer on the start point; a widget reacts
            // to a press only once the pointer has been over it for a frame.
            m_MouseX = ax;
            m_MouseY = ay;
            io.AddMousePosEvent(m_MouseX, m_MouseY);

            Modifiers mods;
            mods.ctrl = step.ctrl;
            mods.shift = step.shift;
            mods.alt = step.alt;
            std::deque<Step> drag;
            if (mods.Any())
                drag.push_back(ModsStep(mods, true));
            Step down;
            down.kind = Step::Kind::ButtonDown;
            down.button = step.button;
            drag.push_back(down);
            for (int i = 1; i <= step.frames; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(step.frames);
                Step move;
                move.kind = Step::Kind::Move;  // no panel: window pixels
                move.x = ax + (bx - ax) * t;
                move.y = ay + (by - ay) * t;
                drag.push_back(move);
            }
            if (step.seconds > 0.0f)
            {
                Step wait;
                wait.kind = Step::Kind::Wait;
                wait.seconds = step.seconds;
                drag.push_back(wait);
            }
            Step up = down;
            up.kind = Step::Kind::ButtonUp;
            drag.push_back(up);
            if (mods.Any())
                drag.push_back(ModsStep(mods, false));
            m_Steps.insert(m_Steps.begin(), drag.begin(), drag.end());
            break;
        }
    }
}

}  // namespace DekiEditor
