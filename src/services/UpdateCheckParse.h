#pragma once

// The pure half of the update check (services/UpdateCheck.h): what to make of
// a GitHub "latest release" response and whether it beats the running build.
// No settings, no network, no UI, so UpdateCheckTests can drive it directly.

#include <cstdint>
#include <string>

namespace DekiEditor
{

struct LatestRelease
{
    std::string tag;      // "0.11.0"; a leading "v" is stripped
    std::string htmlUrl;  // the release page, what a person opens
};

/// Parses the JSON of GET /repos/<owner>/<repo>/releases/latest. False when it
/// is not a published release (malformed, a draft, a prerelease, no tag).
bool ParseLatestRelease(const std::string& json, LatestRelease& out);

/// True when `releaseTag` is a higher version than `current`. A dev build
/// (current "0.0.0-dev", the fallback in EditorVersion.h) never counts as
/// outdated: it is not a release, and every tag would beat it.
bool IsNewerRelease(const std::string& releaseTag, const std::string& current);

/// True when at least `intervalSec` passed since `lastCheckEpoch` (0 = never
/// checked). Clock skew backwards also counts as due, so a wrong clock cannot
/// silence the check for years.
bool UpdateCheckIsDue(int64_t lastCheckEpoch, int64_t nowEpoch, int64_t intervalSec);

}  // namespace DekiEditor
