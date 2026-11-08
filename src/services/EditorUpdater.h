#pragma once

// Updates the editor from the project launcher: downloads the release for this
// computer, checks it against its published SHA-256, unpacks it beside the
// install and, when the user says so, swaps the folders and restarts.
//
// Only a downloaded editor can update itself: a build from source has no
// install to replace, and keeps Help > Open release page.
//
// The download runs on its own thread. Projects are not touched: opening one
// with a newer editor offers the upgrade as usual.

#include <string>

namespace DekiEditor
{

class EditorUpdater
{
public:
    enum class State
    {
        Idle,
        Working,
        Ready,  // unpacked; restarting applies it
        Failed,
    };

    /// True when this is a downloaded editor whose folder it can write beside.
    static bool CanUpdateInPlace();

    /// Starts downloading `version`, unless a download is already running.
    static void Start(const std::string& version);

    static State GetState();

    /// What it is doing, or why it failed.
    static std::string GetStatus();

    /// Starts the swap, which waits for this process to exit. False when it
    /// could not be started. The caller then quits the editor.
    static bool ApplyAndRestart();
};

}  // namespace DekiEditor
