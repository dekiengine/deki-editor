#pragma once

#include "AgentCursorIntent.h"

#include "imgui.h"

#include <string>

namespace DekiEditor
{

/// A pointer drawn over the editor that shows what an AI agent is doing
/// through the MCP server: it glides to the row, panel or point a tool call
/// acts on, with a label ("Setting SpriteComponent.color"), the way Claude in
/// Chrome shows its pointer on a page.
///
/// The tool waits for it. McpServer starts the pointer with Show() and holds
/// the call until Arrived(), so what the user sees happen matches what the
/// agent did, in order. Read-only calls are not shown and never wait.
///
/// Frame timing, like EditorCapture and EditorInput: Show() runs between
/// frames (MCP handlers run before ImGui::NewFrame), the panels Report() where
/// the wanted item is while they draw, and Draw() runs last, after every
/// window and before ImGui::Render(). It draws on the foreground list, above
/// popups and tooltips, and not in frames a screenshot is taken from: the
/// agent should see the editor, not its own pointer.
///
/// Setting "mcp.agentPointer", on by default; Editor Settings > MCP.
class AgentCursor
{
public:
    static constexpr const char* kSettingEnabled = "mcp.agentPointer";

    static AgentCursor& Get();

    static bool IsEnabled();
    static void SetEnabled(bool enabled);

    /// Starts showing `intent`, whose label is final (placeholders filled in).
    /// `wait` false shows it without holding the call: input played into a
    /// running game, where a pause before each tap would change what is
    /// being tested. Main thread.
    void Show(const AgentIntent& intent, bool wait = true);

    /// The pointer has reached the target of the last Show(), and stayed long
    /// enough to be seen. True at once for an intent with no target.
    bool Arrived() const;

    /// What the panels look for while drawing; null when nothing is shown.
    const AgentIntent* Wanted() const { return m_Active ? &m_Intent : nullptr; }

    /// While the pointer is on its way: a panel should scroll the wanted item
    /// into view and open what hides it. Not after, so the user can collapse
    /// and scroll as they like while the label lingers.
    bool WantsReveal() const;

    /// How close a reported rect is to what the tool acts on. The closest
    /// reported in a frame is pointed at: a property's own row, else its
    /// component's header (collapsed), else the object's Hierarchy row.
    enum class Precision
    {
        ObjectRow,  // the object's Hierarchy row, standing in for its component or property
        Section,    // the component's Inspector header
        Exact       // the item itself
    };

    /// A panel drew the wanted item, or a stand-in for it, at this screen rect.
    void Report(const ImVec2& min, const ImVec2& max, Precision precision);

    /// Once per frame, after every window, before ImGui::Render().
    void Draw();

private:
    AgentCursor() = default;

    // Seconds since the last Show(), counted in frame time (see Draw).
    float Age() const;

    AgentIntent m_Intent;
    bool m_Active = false;
    float m_Age = 0.0f;  // seconds since Show(), in frame time
    float m_Travel = 0.0f;  // seconds to glide to the target
    float m_Dwell = 0.0f;   // seconds to rest there before the tool runs
    bool m_Wait = true;     // the call is held until Arrived()
    float m_BusyAge = 0.0f; // seconds after Show the injected input was last seen playing

    // Where the tip is, and where it was when the current glide began.
    ImVec2 m_Tip{ -1.0f, -1.0f };
    ImVec2 m_From{ -1.0f, -1.0f };

    // This frame's closest report (cleared by Draw).
    bool m_HaveReport = false;
    Precision m_ReportPrecision = Precision::ObjectRow;
    ImVec2 m_ReportMin{}, m_ReportMax{};
};

}  // namespace DekiEditor
