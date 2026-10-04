#pragma once

// Toast - transient, self-dismissing notification cards.
//
// The editor had two notification surfaces and neither fits a passing
// message. The Console's pinned entries (ConsolePanel::LogBuildError) are the
// COMPILER's channel: they survive Clear(), are keyed per build target and
// feed the "Build failed - N errors" pill in the status bar, so anything else
// posted there reads as a broken build. A plain LogWarning is durable but
// silent - LogLevel::Editor is hidden by default (ConsolePanel.h) and even a
// warning is easy to miss when the Console is docked behind another panel.
//
// A toast covers the gap: it is impossible to miss on the frame it appears,
// costs nothing once it fades, and leaves the durable record to the Console.
// Pair the two - toast what the user must see now, log what they may want to
// scroll back to later.
//
// Modelled on ProgressModal::DrawInFrame: a non-modal card drawn inside the
// editor's own frame, anchored to the main viewport and DPI-scaled, with no
// instance to thread through call sites.

#include <cstddef>
#include <string>

namespace DekiEditor
{

class Toast
{
public:
    enum class Level
    {
        Info,     // neutral, accent-coloured
        Success,  // green
        Warning,  // amber
        Error,    // red - a real failure, NOT a compile error (that is the Console's)
    };

    /// Queue a toast. Safe to call from any thread: builds and package work run
    /// on a ProcessRunner thread, and the queue is drained on the main thread.
    /// @param seconds visible lifetime before the fade-out begins; hovering pauses it.
    static void Push(Level level, const char* title, const std::string& message, float seconds = 8.0f);

    /// Draw and age the stack. Call once per frame from EditorApp::Update,
    /// INSIDE the ImGui frame, after the build progress card so a toast never
    /// lands underneath it.
    static void DrawInFrame();

    /// Drop every queued toast (project close, entering play mode).
    static void DismissAll();

    /// Live toast count. For tests and diagnostics.
    static std::size_t Count();
};

}  // namespace DekiEditor
