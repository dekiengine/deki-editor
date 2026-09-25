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

void EditorInput::RequestClick(const std::string& panel, float x, float y, Button button, int count)
{
    Move(panel, x, y);
    for (int i = 0; i < count; ++i)
    {
        Step down;
        down.kind = Step::Kind::ButtonDown;
        down.button = static_cast<int>(button);
        m_Steps.push_back(down);
        Step up = down;
        up.kind = Step::Kind::ButtonUp;
        m_Steps.push_back(up);
    }
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
    }
}

}  // namespace DekiEditor
