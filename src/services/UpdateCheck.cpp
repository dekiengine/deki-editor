#include "UpdateCheck.h"
#include "UpdateCheckParse.h"
#include <deki-editor/EditorSettings.h>
#include "../EditorVersion.h"
#include "../widgets/Toast.h"

#include <deki-editor/EditorHttpUtils.h>
#include <deki/LogSystem.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace DekiEditor
{

namespace
{
constexpr int64_t kIntervalSec = 24 * 60 * 60;

std::mutex g_Mutex;
bool g_Started = false;   // Start() ran once
bool g_InFlight = false;  // a request thread is running
bool g_Manual = false;    // the in-flight request came from CheckNow()
bool g_Ready = false;     // a result waits for Poll()
bool g_Newer = false;
LatestRelease g_Result;
std::string g_Error;             // empty when the request parsed
std::string g_AvailableVersion;  // remembered for the Help menu
std::string g_AvailableUrl;

int64_t NowEpoch()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The worker: fetch, parse, compare, hand over. Everything it touches is
// under g_Mutex or thread-local; the toast is left to Poll() on the main
// thread because ImGui state is not for a worker to touch.
void Run(bool manual)
{
    const std::string body = EditorHttpUtils::FetchUrl(UpdateCheck::kLatestReleaseUrl);

    LatestRelease release;
    std::string error;
    bool newer = false;
    if (body.empty())
    {
        error = "no response (offline, or the repository is not public)";
    }
    else if (!ParseLatestRelease(body, release))
    {
        error = "no published release in the response";
    }
    else
    {
        newer = IsNewerRelease(release.tag, EditorVersionString());
    }

    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Result = release;
    g_Error = error;
    g_Newer = newer;
    g_Manual = manual;
    g_Ready = true;
    g_InFlight = false;
}

void Launch(bool manual)
{
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (g_InFlight)
        {
            return;
        }
        g_InFlight = true;
        g_Ready = false;
    }
    std::thread(Run, manual).detach();
}
}  // namespace

bool UpdateCheck::IsEnabled()
{
    return EditorSettings::GetBool(kSettingEnabled, true);
}

void UpdateCheck::SetEnabled(bool enabled)
{
    EditorSettings::SetBool(kSettingEnabled, enabled);
}

void UpdateCheck::Start()
{
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (g_Started)
        {
            return;
        }
        g_Started = true;
    }
    if (!IsEnabled())
    {
        return;
    }

    const int64_t last = std::strtoll(EditorSettings::GetString(kSettingLastCheck, "0").c_str(), nullptr, 10);
    if (!UpdateCheckIsDue(last, NowEpoch(), kIntervalSec))
    {
        return;
    }

    // Recorded before the answer arrives: a machine that is offline every
    // morning must not retry on every launch.
    EditorSettings::SetString(kSettingLastCheck, std::to_string(NowEpoch()));
    Launch(false);
}

void UpdateCheck::CheckNow()
{
    EditorSettings::SetString(kSettingLastCheck, std::to_string(NowEpoch()));
    Launch(true);
}

void UpdateCheck::Poll()
{
    LatestRelease release;
    std::string error;
    bool newer = false;
    bool manual = false;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (!g_Ready)
        {
            return;
        }
        g_Ready = false;
        release = g_Result;
        error = g_Error;
        newer = g_Newer;
        manual = g_Manual;
        if (newer)
        {
            g_AvailableVersion = release.tag;
            g_AvailableUrl = release.htmlUrl;
        }
    }

    if (!error.empty())
    {
        DEKI_LOG_DEBUG("UpdateCheck: %s", error.c_str());
        if (manual)
        {
            Toast::Push(Toast::Level::Warning, "Update check", "Could not reach GitHub releases: " + error);
        }
        return;
    }

    if (newer)
    {
        DEKI_LOG_INFO("UpdateCheck: Deki Editor %s is available (running %s): %s", release.tag.c_str(),
                      EditorVersionString(), release.htmlUrl.c_str());
        Toast::Push(Toast::Level::Info, ("Deki Editor " + release.tag + " is available").c_str(),
                    "You are running " + std::string(EditorVersionString()) +
                        ". Help > Open release page has the download.",
                    12.0f);
    }
    else
    {
        DEKI_LOG_DEBUG("UpdateCheck: up to date (%s, latest %s)", EditorVersionString(), release.tag.c_str());
        if (manual)
        {
            Toast::Push(Toast::Level::Success, "Up to date",
                        "Deki Editor " + std::string(EditorVersionString()) + " is the latest release.");
        }
    }
}

std::string UpdateCheck::AvailableVersion()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    return g_AvailableVersion;
}

std::string UpdateCheck::AvailableUrl()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    return g_AvailableUrl;
}

}  // namespace DekiEditor
