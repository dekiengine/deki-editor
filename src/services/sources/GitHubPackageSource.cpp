#include "GitHubPackageSource.h"
#include <deki-editor/EditorHttpUtils.h>
#include <algorithm>
#include <regex>
#include <sstream>

GitHubPackageSource::GitHubPackageSource() = default;
GitHubPackageSource::~GitHubPackageSource() = default;

bool GitHubPackageSource::CanHandle(const std::string& url) const
{
    return url.find("github.com") != std::string::npos;
}

bool GitHubPackageSource::ParseGitHubUrl(const std::string& url, std::string& owner, std::string& repo) const
{
    // Matches:
    // https://github.com/owner/repo
    // https://github.com/owner/repo.git
    // github.com/owner/repo
    std::regex pattern("(?:https?://)?github\\.com/([^/]+)/([^/\\.]+)(?:\\.git)?/?$");
    std::smatch match;

    if (std::regex_search(url, match, pattern) && match.size() >= 3)
    {
        owner = match[1].str();
        repo = match[2].str();
        return true;
    }
    return false;
}

bool GitHubPackageSource::ParseUrl(const std::string& url, PackageEntry& outInfo)
{
    std::string owner, repo;
    if (!ParseGitHubUrl(url, owner, repo))
    {
        return false;
    }

    outInfo.id = "github:" + owner + "/" + repo;
    outInfo.name = owner + "/" + repo;
    outInfo.displayName = repo;  // replaced by the name from the API later
    outInfo.url = "https://github.com/" + owner + "/" + repo;
    outInfo.source = PackageSourceType::GitHub;
    outInfo.isInstalled = false;

    return true;
}

void GitHubPackageSource::FetchVersions(const PackageEntry& pkg, VersionsCallback callback)
{
    std::thread(
        [this, pkg, callback]()
        {
            std::string owner, repo;
            if (!ParseGitHubUrl(pkg.url, owner, repo))
            {
                callback(false, {});
                return;
            }

            std::vector<std::string> versions;

            // Releases first, then tags when there are none
            std::string releasesUrl = "https://api.github.com/repos/" + owner + "/" + repo + "/releases";
            std::string releasesJson = DekiEditor::EditorHttpUtils::FetchUrl(releasesUrl);

            if (!releasesJson.empty())
            {
                versions = ParseReleasesJson(releasesJson);
            }

            if (versions.empty())
            {
                std::string tagsUrl = "https://api.github.com/repos/" + owner + "/" + repo + "/tags";
                std::string tagsJson = DekiEditor::EditorHttpUtils::FetchUrl(tagsUrl);

                if (!tagsJson.empty())
                {
                    versions = ParseTagsJson(tagsJson);
                }
            }

            // Branches are always added, prefixed with "branch:"
            std::string branchesUrl = "https://api.github.com/repos/" + owner + "/" + repo + "/branches";
            std::string branchesJson = DekiEditor::EditorHttpUtils::FetchUrl(branchesUrl);

            if (!branchesJson.empty())
            {
                auto branches = ParseBranchesJson(branchesJson);
                for (const auto& branch : branches)
                {
                    versions.push_back("branch:" + branch);
                }
            }

            callback(!versions.empty(), versions);
        })
        .detach();
}

std::vector<std::string> GitHubPackageSource::ParseReleasesJson(const std::string& json)
{
    std::vector<std::string> versions;

    // Every "tag_name" value, by regex: [...{"tag_name": "v7.0.0", ...}, ...]
    std::regex pattern("\"tag_name\"\\s*:\\s*\"([^\"]+)\"");
    std::sregex_iterator iter(json.begin(), json.end(), pattern);
    std::sregex_iterator end;

    while (iter != end)
    {
        std::string version = (*iter)[1].str();
        if (!version.empty() && version[0] == 'v')
        {
            version = version.substr(1);
        }
        versions.push_back(version);
        ++iter;
    }

    return versions;
}

std::vector<std::string> GitHubPackageSource::ParseTagsJson(const std::string& json)
{
    std::vector<std::string> versions;

    // Every "name" value, by regex: [...{"name": "v7.0.0", ...}, ...]
    std::regex pattern("\"name\"\\s*:\\s*\"([^\"]+)\"");
    std::sregex_iterator iter(json.begin(), json.end(), pattern);
    std::sregex_iterator end;

    while (iter != end)
    {
        std::string version = (*iter)[1].str();
        if (!version.empty() && version[0] == 'v')
        {
            version = version.substr(1);
        }
        versions.push_back(version);
        ++iter;
    }

    return versions;
}

std::vector<std::string> GitHubPackageSource::ParseBranchesJson(const std::string& json)
{
    std::vector<std::string> branches;

    // Every "name" value, by regex: [...{"name": "main", ...}, ...]
    std::regex pattern("\"name\"\\s*:\\s*\"([^\"]+)\"");
    std::sregex_iterator iter(json.begin(), json.end(), pattern);
    std::sregex_iterator end;

    while (iter != end)
    {
        branches.push_back((*iter)[1].str());
        ++iter;
    }

    return branches;
}

void GitHubPackageSource::ParseRepoMetadata(const std::string& json, PackageEntry& pkg)
{
    std::regex descPattern("\"description\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch descMatch;
    if (std::regex_search(json, descMatch, descPattern))
    {
        pkg.description = descMatch[1].str();
    }

    std::regex namePattern("\"name\"\\s*:\\s*\"([^\"]+)\"");
    std::smatch nameMatch;
    if (std::regex_search(json, nameMatch, namePattern))
    {
        pkg.displayName = nameMatch[1].str();
    }

    std::regex licensePattern("\"license\"\\s*:\\s*\\{[^}]*\"name\"\\s*:\\s*\"([^\"]+)\"");
    std::smatch licenseMatch;
    if (std::regex_search(json, licenseMatch, licensePattern))
    {
        pkg.license = licenseMatch[1].str();
    }

    // The owner's login is the author
    std::regex ownerPattern("\"owner\"\\s*:\\s*\\{[^}]*\"login\"\\s*:\\s*\"([^\"]+)\"");
    std::smatch ownerMatch;
    if (std::regex_search(json, ownerMatch, ownerPattern))
    {
        pkg.author = ownerMatch[1].str();
    }
}
