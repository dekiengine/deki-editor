#pragma once

// Update check: a notice, not an updater.
//
// Once a day (setting "updates.check", on by default) the editor asks GitHub
// for the latest release of dekiengine/deki-editor and, when its tag is newer
// than DEKI_EDITOR_VERSION, shows a toast naming it. Help > "Open release
// page" then opens it in the browser; nothing is downloaded or installed.
//
// The request runs on its own thread and reports back through Poll(), which
// the frame loop calls; every failure (offline, a private repository, rate
// limit, bad JSON) is a debug log line and nothing else. Headless runs never
// check.

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

    /// Start the daily check if it is enabled and due. Idempotent per process.
    static void Start();

    /// Check regardless of the interval (Help > Check for updates now). Also
    /// reports "up to date" as a toast, which the daily check does not.
    static void CheckNow();

    /// Main thread, once per frame: deliver a finished check as a toast.
    static void Poll();

    static bool IsEnabled();
    static void SetEnabled(bool enabled);

    /// Tag and page of a newer release once one was seen this session;
    /// empty otherwise. What the Help menu offers to open.
    static std::string AvailableVersion();
    static std::string AvailableUrl();
};

}  // namespace DekiEditor
