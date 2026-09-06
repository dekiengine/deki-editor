#pragma once

#include "../PackageManager.h"
#include <thread>
#include <atomic>

class GitHubPackageSource : public IPackageSource
{
public:
    GitHubPackageSource();
    ~GitHubPackageSource();

    PackageSourceType GetType() const override { return PackageSourceType::GitHub; }
    std::string GetName() const override { return "GitHub"; }

    bool CanHandle(const std::string& url) const override;
    bool ParseUrl(const std::string& url, PackageEntry& outInfo) override;
    void FetchVersions(const PackageEntry& pkg, VersionsCallback callback) override;

private:
    // Parse GitHub URL to extract owner and repo
    bool ParseGitHubUrl(const std::string& url, std::string& owner, std::string& repo) const;

    // Parse GitHub API responses
    std::vector<std::string> ParseReleasesJson(const std::string& json);
    std::vector<std::string> ParseTagsJson(const std::string& json);
    std::vector<std::string> ParseBranchesJson(const std::string& json);
    void ParseRepoMetadata(const std::string& json, PackageEntry& pkg);

    std::atomic<bool> m_CancelRequested{ false };
};
