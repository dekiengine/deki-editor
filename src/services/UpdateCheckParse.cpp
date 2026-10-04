#include "UpdateCheckParse.h"
#include <deki-editor/VersionCompare.h>

#include <nlohmann/json.hpp>

namespace DekiEditor
{

bool ParseLatestRelease(const std::string& json, LatestRelease& out)
{
    nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_object())
    {
        return false;
    }
    if (j.value("draft", false) || j.value("prerelease", false))
    {
        return false;
    }

    std::string tag = j.value("tag_name", "");
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V'))
    {
        tag.erase(0, 1);
    }
    if (tag.empty())
    {
        return false;
    }

    // The page the Help menu opens with SDL_OpenURL. Anything but our own
    // releases page (file://, a custom scheme, another host) is a reason to
    // distrust the whole response.
    const std::string htmlUrl = j.value("html_url", "");
    if (htmlUrl.rfind("https://github.com/dekiengine/deki-editor/", 0) != 0)
    {
        return false;
    }

    out.tag = tag;
    out.htmlUrl = htmlUrl;
    return true;
}

bool IsNewerRelease(const std::string& releaseTag, const std::string& current)
{
    if (current.rfind("0.0.0", 0) == 0)
    {
        return false;
    }
    std::string tag = releaseTag;
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V'))
    {
        tag.erase(0, 1);
    }
    return CompareVersions(tag, current) > 0;
}

bool UpdateCheckIsDue(int64_t lastCheckEpoch, int64_t nowEpoch, int64_t intervalSec)
{
    if (lastCheckEpoch <= 0)
    {
        return true;
    }
    if (nowEpoch < lastCheckEpoch)
    {
        return true;
    }
    return nowEpoch - lastCheckEpoch >= intervalSec;
}

}  // namespace DekiEditor
