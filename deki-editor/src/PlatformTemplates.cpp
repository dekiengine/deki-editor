#include <deki-editor/build/PlatformTemplates.h>

#include <deki-editor/Paths.h>
#include <deki-editor/SafeNames.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki/LogSystem.h>

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace DekiEditor
{

std::vector<PlatformTemplate> FindPlatformTemplates(const std::string& projectPath)
{
    std::vector<PlatformTemplate> found;

    std::error_code ec;
    const fs::path packagesDir = GetPackagesDirectory(projectPath);
    if (!fs::is_directory(packagesDir, ec))
        return found;

    for (const auto& package : fs::directory_iterator(packagesDir, ec))
    {
        const fs::path platformsDir = package.path() / "platforms";
        if (!fs::is_directory(platformsDir, ec))
            continue;

        for (const auto& entry : fs::directory_iterator(platformsDir, ec))
        {
            if (!entry.is_directory(ec))
                continue;

            // The directory name becomes a directory name in the project, and
            // a package is downloaded content.
            const std::string id = entry.path().filename().string();
            std::string reason;
            if (!SafeNames::IsSafeName(id, reason))
                continue;
            if (!fs::is_regular_file(entry.path() / (id + ".json"), ec))
                continue;

            found.push_back({ id, package.path().filename().string(), entry.path().string() });
        }
    }

    std::sort(found.begin(), found.end(),
              [](const PlatformTemplate& a, const PlatformTemplate& b)
              { return a.platformId != b.platformId ? a.platformId < b.platformId : a.packageId < b.packageId; });
    return found;
}

AdoptPlatformResult AdoptPlatformTemplate(const std::string& projectPath, const std::string& platformId,
                                          std::string& detail)
{
    std::string reason;
    if (!SafeNames::IsSafeName(platformId, reason))
    {
        detail = "'" + platformId + "' is not a usable platform id: " + reason;
        return AdoptPlatformResult::Failed;
    }

    std::error_code ec;
    const fs::path destination = fs::path(GetPlatformsDirectory(projectPath)) / platformId;
    if (fs::is_regular_file(destination / (platformId + ".json"), ec))
        return AdoptPlatformResult::AlreadyDefined;

    std::vector<PlatformTemplate> offers;
    for (const PlatformTemplate& t : FindPlatformTemplates(projectPath))
        if (t.platformId == platformId)
            offers.push_back(t);

    if (offers.empty())
        return AdoptPlatformResult::NoTemplate;

    if (offers.size() > 1)
    {
        for (const PlatformTemplate& t : offers)
            detail += (detail.empty() ? "" : ", ") + t.packageId;
        return AdoptPlatformResult::Ambiguous;
    }

    // Files the project already has under that directory win: a boot scene
    // somebody wrote before the platform JSON existed is theirs.
    fs::create_directories(destination, ec);
    if (!ec)
        fs::copy(offers.front().directory, destination,
                 fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
    if (ec)
    {
        detail = ec.message();
        return AdoptPlatformResult::Failed;
    }

    detail = offers.front().packageId;
    return AdoptPlatformResult::Adopted;
}

bool EnsureProjectPlatform(const std::string& projectPath, const std::string& platformId, std::string& err)
{
    if (platformId.empty())
        return true;

    std::string detail;
    switch (AdoptPlatformTemplate(projectPath, platformId, detail))
    {
        case AdoptPlatformResult::AlreadyDefined:
        case AdoptPlatformResult::NoTemplate:
            return true;

        case AdoptPlatformResult::Adopted:
            DEKI_LOG_INFO("Platform '%s' adopted into the project from package '%s'", platformId.c_str(),
                          detail.c_str());
            PlatformRegistry::Instance().LoadPlatformsFromDirectory(GetPlatformsDirectory(projectPath));
            return true;

        case AdoptPlatformResult::Ambiguous:
            err = "Platform '" + platformId + "' is offered by more than one installed package (" + detail +
                  "); copy the one you want into the project's platforms/ directory.";
            return false;

        case AdoptPlatformResult::Failed:
            err = "Could not adopt platform '" + platformId + "': " + detail;
            return false;
    }
    return true;
}

void RegisterPlatformTemplates(const std::string& projectPath)
{
    std::error_code ec;
    const fs::path packagesDir = GetPackagesDirectory(projectPath);
    if (!fs::is_directory(packagesDir, ec))
        return;

    for (const auto& package : fs::directory_iterator(packagesDir, ec))
    {
        const fs::path platformsDir = package.path() / "platforms";
        if (fs::is_directory(platformsDir, ec))
            PlatformRegistry::Instance().LoadPlatformsFromDirectory(platformsDir.string());
    }
}

}  // namespace DekiEditor
