#pragma once

#include "../PackageManager.h"
#include <filesystem>

namespace fs = std::filesystem;

class ESPIDFInstaller : public IPackageInstaller
{
public:
    ESPIDFInstaller();
    ~ESPIDFInstaller();

    std::string GetPlatformName() const override { return "ESP-IDF"; }

    bool IsApplicable(const std::string& projectPath) const override;
    std::vector<PackageEntry> GetInstalledPackages(const std::string& projectPath) override;
    void Install(const std::string& projectPath, const PackageEntry& pkg,
                 const std::string& version, PackageCallback callback) override;
    void Remove(const std::string& projectPath, const PackageEntry& pkg,
                PackageCallback callback) override;

    // Core dependency management
    std::vector<CoreDependency> GetCoreDependencies() const override;
    void EnsureCoreDependencies(const std::string& projectPath) override;
    bool IsCoreDependency(const std::string& name) const override;

    // Merge dependencies from installed packages into idf_component.yml
    void MergePackageDeps(const std::string& projectPath);

    // Merge a pre-built list of deps into idf_component.yml (add or upgrade)
    void MergeDeps(const std::string& projectPath, const std::vector<DependencyInfo>& deps);

private:
    // Get path to idf_component.yml
    fs::path GetYamlPath(const std::string& projectPath) const;

    // Parse idf_component.yml
    bool ParseYaml(const fs::path& path, std::vector<DependencyInfo>& dependencies);

    // Write idf_component.yml
    bool WriteYaml(const fs::path& path, const std::vector<DependencyInfo>& dependencies);
};
