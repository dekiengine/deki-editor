#pragma once

// Update check: tells the user when a newer editor release exists.
//
// Once a day (setting "updates.check", on by default) the editor asks GitHub
// for the latest release of dekiengine/deki-editor and, when its tag is newer
// than DEKI_EDITOR_VERSION, shows a toast naming it. Help > "Open release
// page" opens it in the browser; the editor downloads nothing itself.
//
// The request runs on its own thread and reports back through Poll(), which
// the frame loop calls. A failure (offline, a private repository, rate limit,
// bad JSON) only logs a debug line. Headless runs never check.

#include <string>

namespace DekiEditor
{

class UpdateCheck
{
public:
    static constexpr const char* kSettingEnabled = "updates.check";
    static constexpr const char* kSettingLastCheck = "updates.lastCheck";  // unix seconds
    static constexpr const char* kLatestReleaseUrl =
        "https://api.github.com/repos/dekiengine/deki-editor/releases/latest";

    /// Starts the daily check if it is enabled and due. Runs at most once per
    /// process.
    static void Start();

    /// Checks now, whatever the interval (Help > Check for updates now). Unlike
    /// the daily check, it also shows "up to date" and errors as toasts.
    static void CheckNow();

    /// Main thread, once per frame: shows a finished check as a toast.
    static void Poll();

    static bool IsEnabled();
    static void SetEnabled(bool enabled);

    /// Tag and page of a newer release seen this session, or empty. The Help
    /// menu offers to open it.
    static std::string AvailableVersion();
    static std::string AvailableUrl();
};

}  // namespace DekiEditor
