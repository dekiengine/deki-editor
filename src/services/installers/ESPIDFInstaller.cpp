#include "ESPIDFInstaller.h"
#include <deki-editor/Paths.h>
#include <deki-editor/build/ProjectPaths.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <deki-editor/VersionCompare.h>

ESPIDFInstaller::ESPIDFInstaller() = default;
ESPIDFInstaller::~ESPIDFInstaller() = default;

fs::path ESPIDFInstaller::GetYamlPath(const std::string& projectPath) const
{
    // Check current project structure: build/esp-idf/main/idf_component.yml
    fs::path buildPath = DekiEditor::ProjectPaths::Build(projectPath) / "esp-idf" / "main" / "idf_component.yml";
    if (fs::exists(buildPath))
    {
        return buildPath;
    }

    // Return new path as default (will be created if needed)
    return buildPath;
}

bool ESPIDFInstaller::IsApplicable(const std::string& projectPath) const
{
    fs::path yamlPath = GetYamlPath(projectPath);
    return fs::exists(yamlPath) || fs::exists(DekiEditor::ProjectPaths::Build(projectPath) / "esp-idf");
}

bool ESPIDFInstaller::ParseYaml(const fs::path& path, std::vector<DependencyInfo>& dependencies)
{
    if (!fs::exists(path))
    {
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open())
    {
        return false;
    }

    std::string line;
    bool inDependencies = false;
    DependencyInfo currentDep;
    bool hasCurrentDep = false;

    // Simple YAML parser for our specific format:
    // dependencies:
    //   namespace/name:           # Registry package
    //     version: "x.x.x"
    //   PackageName:              # GitHub package
    //     git: https://github.com/owner/repo.git
    //     version: "x.x.x"

    while (std::getline(file, line))
    {
        // Check if we're entering dependencies section
        if (line.find("dependencies:") != std::string::npos)
        {
            inDependencies = true;
            continue;
        }

        if (!inDependencies) continue;

        // Check if we're leaving dependencies section (new top-level key)
        if (!line.empty() && line[0] != ' ' && line[0] != '\t')
        {
            break;
        }

        // Check for dependency name (2-space indent, ends with :)
        if (line.size() > 2 && line[0] == ' ' && line[1] == ' ' && line[2] != ' ')
        {
            // Save previous dependency if complete
            if (hasCurrentDep && !currentDep.name.empty() && !currentDep.version.empty())
            {
                dependencies.push_back(currentDep);
            }

            // Start new dependency
            currentDep = DependencyInfo{};
            hasCurrentDep = true;

            // Extract dependency name
            size_t colonPos = line.find(':');
            if (colonPos != std::string::npos)
            {
                currentDep.name = line.substr(2, colonPos - 2);
                // Trim whitespace
                currentDep.name.erase(0, currentDep.name.find_first_not_of(" \t"));
                currentDep.name.erase(currentDep.name.find_last_not_of(" \t") + 1);
            }
        }

        // Check for git line (4-space indent)
        if (line.size() > 4 && line.substr(0, 4) == "    " && line.find("git:") != std::string::npos)
        {
            size_t colonPos = line.find(':');
            if (colonPos != std::string::npos && hasCurrentDep)
            {
                std::string gitUrl = line.substr(colonPos + 1);
                // Remove whitespace
                gitUrl.erase(0, gitUrl.find_first_not_of(" \t"));
                gitUrl.erase(gitUrl.find_last_not_of(" \t") + 1);
                currentDep.gitUrl = gitUrl;
                currentDep.source = PackageSourceType::GitHub;
            }
        }

        // Check for version line (4-space indent)
        if (line.size() > 4 && line.substr(0, 4) == "    " && line.find("version:") != std::string::npos)
        {
            // Extract version
            size_t colonPos = line.find(':');
            if (colonPos != std::string::npos && hasCurrentDep)
            {
                std::string version = line.substr(colonPos + 1);
                // Remove quotes and whitespace
                version.erase(0, version.find_first_not_of(" \t\"'"));
                version.erase(version.find_last_not_of(" \t\"'") + 1);
                currentDep.version = version;
            }
        }
    }

    // Don't forget the last dependency
    if (hasCurrentDep && !currentDep.name.empty() && !currentDep.version.empty())
    {
        dependencies.push_back(currentDep);
    }

    return true;
}

bool ESPIDFInstaller::WriteYaml(const fs::path& path, const std::vector<DependencyInfo>& dependencies)
{
    // Create parent directories if needed
    fs::create_directories(path.parent_path());

    std::ofstream file(path);
    if (!file.is_open())
    {
        return false;
    }

    file << "# Deki Game - Component Dependencies\n";
    file << "dependencies:\n";

    for (const auto& dep : dependencies)
    {
        file << "  " << dep.name << ":\n";
        if (dep.source == PackageSourceType::GitHub && !dep.gitUrl.empty())
        {
            file << "    git: " << dep.gitUrl << "\n";
        }
        file << "    version: \"" << dep.version << "\"\n";
    }

    return file.good();
}

std::vector<PackageEntry> ESPIDFInstaller::GetInstalledPackages(const std::string& projectPath)
{
    std::vector<PackageEntry> packages;

    fs::path yamlPath = GetYamlPath(projectPath);
    std::vector<DependencyInfo> dependencies;

    if (!ParseYaml(yamlPath, dependencies))
    {
        return packages;
    }

    for (const auto& dep : dependencies)
    {
        PackageEntry pkg;
        pkg.name = dep.name;
        pkg.displayName = dep.name;
        pkg.source = dep.source;

        if (dep.source == PackageSourceType::GitHub)
        {
            // GitHub package - extract owner/repo from git URL for the ID
            // e.g., "https://github.com/lovyan03/LovyanGFX.git" -> "github:lovyan03/LovyanGFX"
            std::string ownerRepo = dep.gitUrl;
            // Remove .git suffix if present
            if (ownerRepo.size() > 4 && ownerRepo.substr(ownerRepo.size() - 4) == ".git")
            {
                ownerRepo = ownerRepo.substr(0, ownerRepo.size() - 4);
            }
            // Extract owner/repo from URL
            size_t githubPos = ownerRepo.find("github.com/");
            if (githubPos != std::string::npos)
            {
                ownerRepo = ownerRepo.substr(githubPos + 11);  // Skip "github.com/"
            }
            pkg.id = "github:" + ownerRepo;
            pkg.url = dep.gitUrl;
            // Remove .git suffix from URL for display
            if (pkg.url.size() > 4 && pkg.url.substr(pkg.url.size() - 4) == ".git")
            {
                pkg.url = pkg.url.substr(0, pkg.url.size() - 4);
            }
        }
        else
        {
            // Registry package
            pkg.id = "espidf:" + dep.name;
            // Extract display name from package name (e.g., "owner/PackageName" -> "PackageName")
            size_t slashPos = dep.name.find('/');
            if (slashPos != std::string::npos)
            {
                pkg.displayName = dep.name.substr(slashPos + 1);
            }
        }

        pkg.installedVersion = dep.version;
        pkg.isInstalled = true;

        packages.push_back(pkg);
    }

    return packages;
}

void ESPIDFInstaller::Install(const std::string& projectPath, const PackageEntry& pkg,
                              const std::string& version, PackageCallback callback)
{
    fs::path yamlPath = GetYamlPath(projectPath);

    // Read existing dependencies
    std::vector<DependencyInfo> dependencies;
    ParseYaml(yamlPath, dependencies);

    // Create DependencyInfo for the new package
    DependencyInfo newDep;
    // Handle "branch:" prefix - strip it for the actual version
    if (version.rfind("branch:", 0) == 0)
    {
        newDep.version = version.substr(7);  // Remove "branch:" prefix
    }
    else
    {
        newDep.version = version;
    }
    newDep.source = pkg.source;

    if (pkg.source == PackageSourceType::GitHub)
    {
        // For GitHub packages, extract repo name from pkg.name (e.g., "lovyan03/LovyanGFX" -> "LovyanGFX")
        std::string repoName = pkg.name;
        size_t slashPos = repoName.find('/');
        if (slashPos != std::string::npos)
        {
            repoName = repoName.substr(slashPos + 1);
        }
        newDep.name = repoName;

        // Construct git URL from pkg.url
        newDep.gitUrl = pkg.url;
        if (!newDep.gitUrl.empty() && newDep.gitUrl.substr(newDep.gitUrl.size() - 4) != ".git")
        {
            newDep.gitUrl += ".git";
        }
    }
    else
    {
        // Registry package - use full name (e.g., "espressif/esp_tinyusb")
        newDep.name = pkg.name;
    }

    // Check if already exists (by name for registry, by git URL for GitHub)
    bool found = false;
    for (auto& dep : dependencies)
    {
        bool match = false;
        if (pkg.source == PackageSourceType::GitHub && dep.source == PackageSourceType::GitHub)
        {
            // Match by git URL or name
            match = (dep.gitUrl == newDep.gitUrl) || (dep.name == newDep.name);
        }
        else
        {
            match = (dep.name == newDep.name);
        }

        if (match)
        {
            dep = newDep;  // Update existing
            found = true;
            break;
        }
    }

    // Add new dependency
    if (!found)
    {
        dependencies.push_back(newDep);
    }

    // Write back
    if (WriteYaml(yamlPath, dependencies))
    {
        callback(true, "Package installed successfully");
    }
    else
    {
        callback(false, "Failed to write idf_component.yml");
    }
}

void ESPIDFInstaller::Remove(const std::string& projectPath, const PackageEntry& pkg,
                             PackageCallback callback)
{
    fs::path yamlPath = GetYamlPath(projectPath);

    // Read existing dependencies
    std::vector<DependencyInfo> dependencies;
    if (!ParseYaml(yamlPath, dependencies))
    {
        callback(false, "Failed to read idf_component.yml");
        return;
    }

    // For GitHub packages, extract repo name for matching
    std::string matchName = pkg.name;
    if (pkg.source == PackageSourceType::GitHub)
    {
        size_t slashPos = matchName.find('/');
        if (slashPos != std::string::npos)
        {
            matchName = matchName.substr(slashPos + 1);
        }
    }

    // Remove the dependency
    auto it = std::remove_if(dependencies.begin(), dependencies.end(),
                             [&pkg, &matchName](const DependencyInfo& dep)
                             {
                                 if (pkg.source == PackageSourceType::GitHub && dep.source == PackageSourceType::GitHub)
                                 {
                                     // Match by name or git URL
                                     return dep.name == matchName ||
                                            (!pkg.url.empty() && dep.gitUrl.find(pkg.url) != std::string::npos);
                                 }
                                 return dep.name == pkg.name;
                             });

    if (it == dependencies.end())
    {
        callback(false, "Package not found");
        return;
    }

    dependencies.erase(it, dependencies.end());

    // Write back
    if (WriteYaml(yamlPath, dependencies))
    {
        callback(true, "Package removed successfully");
    }
    else
    {
        callback(false, "Failed to write idf_component.yml");
    }
}

// ============================================================================
// Core Dependencies (managed by editor, hidden from users)
// ============================================================================

// Core dependencies for Espressif platforms (ESP32, ESP32-S3, etc.)
// These are now bundled in toolchains/esp-idf-components/ and copied to project's
// builders/esp-idf/components/ folder during project creation.
// Empty list since we no longer manage dependencies via idf_component.yml.
static const std::vector<CoreDependency> s_EspressifCoreDeps = {
    // Components are bundled - no managed dependencies needed
};

std::vector<CoreDependency> ESPIDFInstaller::GetCoreDependencies() const
{
    return s_EspressifCoreDeps;
}

bool ESPIDFInstaller::IsCoreDependency(const std::string& name) const
{
    for (const auto& coreDep : s_EspressifCoreDeps)
    {
        // Match exact name
        if (coreDep.name == name)
        {
            return true;
        }

        // For registry packages like "espressif/esp_tinyusb", also match just "esp_tinyusb"
        size_t slashPos = coreDep.name.find('/');
        if (slashPos != std::string::npos)
        {
            std::string shortName = coreDep.name.substr(slashPos + 1);
            if (shortName == name)
            {
                return true;
            }
        }

        // For GitHub packages, match the repo name or owner/repo format
        if (coreDep.isGitHub && !coreDep.gitUrl.empty())
        {
            // Extract owner/repo from URL (e.g., "lovyan03/LovyanGFX")
            std::string url = coreDep.gitUrl;
            if (url.size() > 4 && url.substr(url.size() - 4) == ".git")
            {
                url = url.substr(0, url.size() - 4);
            }
            size_t githubPos = url.find("github.com/");
            if (githubPos != std::string::npos)
            {
                std::string ownerRepo = url.substr(githubPos + 11);  // "lovyan03/LovyanGFX"
                if (ownerRepo == name)
                {
                    return true;
                }
                // Also match just the repo name
                size_t lastSlash = ownerRepo.find_last_of('/');
                if (lastSlash != std::string::npos)
                {
                    std::string repoName = ownerRepo.substr(lastSlash + 1);
                    if (repoName == name)
                    {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void ESPIDFInstaller::EnsureCoreDependencies(const std::string& projectPath)
{
    fs::path yamlPath = GetYamlPath(projectPath);

    // Read existing dependencies
    std::vector<DependencyInfo> dependencies;
    ParseYaml(yamlPath, dependencies);

    bool modified = false;

    // Check each core dependency
    for (const auto& coreDep : s_EspressifCoreDeps)
    {
        // Determine the name to match (repo name for GitHub, full name for registry)
        std::string matchName = coreDep.name;
        if (coreDep.isGitHub && !coreDep.gitUrl.empty())
        {
            // For GitHub, the yaml uses repo name (e.g., "LovyanGFX")
            // which is already the case for our core deps
        }

        // Find if this dependency exists
        bool found = false;
        for (auto& dep : dependencies)
        {
            bool isMatch = (dep.name == coreDep.name);

            // Also check short name for registry packages
            if (!isMatch)
            {
                size_t slashPos = coreDep.name.find('/');
                if (slashPos != std::string::npos)
                {
                    std::string shortName = coreDep.name.substr(slashPos + 1);
                    isMatch = (dep.name == shortName);
                }
            }

            // For GitHub packages, also match by repo name
            if (!isMatch && coreDep.isGitHub)
            {
                isMatch = (dep.name == coreDep.name);
            }

            if (isMatch)
            {
                found = true;
                // Update version if different
                if (dep.version != coreDep.version)
                {
                    dep.version = coreDep.version;
                    modified = true;
                }
                // Update git URL if needed
                if (coreDep.isGitHub && dep.gitUrl != coreDep.gitUrl)
                {
                    dep.gitUrl = coreDep.gitUrl;
                    dep.source = PackageSourceType::GitHub;
                    modified = true;
                }
                break;
            }
        }

        // Add if not found
        if (!found)
        {
            DependencyInfo newDep;
            newDep.name = coreDep.name;
            newDep.version = coreDep.version;
            newDep.gitUrl = coreDep.gitUrl;
            newDep.source = coreDep.isGitHub ? PackageSourceType::GitHub : PackageSourceType::ESPIDFRegistry;
            dependencies.push_back(newDep);
            modified = true;
        }
    }

    // Write back if modified
    if (modified)
    {
        WriteYaml(yamlPath, dependencies);
    }
}

void ESPIDFInstaller::MergePackageDeps(const std::string& projectPath)
{
    // Scan project/packages/*/ for .deki-package.json files containing dependencies.espidf
    fs::path packagesDir = fs::path(DekiEditor::GetPackagesDirectory(projectPath));
    if (!fs::exists(packagesDir)) return;

    // Collect all espidf deps from installed packages (highest version wins)
    std::vector<DependencyInfo> packageDeps;

    try
    {
        for (const auto& entry : fs::directory_iterator(packagesDir))
        {
            if (!entry.is_directory()) continue;

            fs::path metaPath = entry.path() / ".deki-package.json";
            if (!fs::exists(metaPath)) continue;

            std::ifstream metaFile(metaPath);
            std::string content((std::istreambuf_iterator<char>(metaFile)),
                                std::istreambuf_iterator<char>());

            // Find "dependencies" section
            size_t pdPos = content.find("\"dependencies\"");
            if (pdPos == std::string::npos) continue;

            // Find "espidf" key within dependencies
            size_t espidfPos = content.find("\"espidf\"", pdPos);
            if (espidfPos == std::string::npos) continue;

            // Find the array start
            size_t arrayStart = content.find('[', espidfPos);
            if (arrayStart == std::string::npos) continue;
            size_t arrayEnd = content.find(']', arrayStart);
            if (arrayEnd == std::string::npos) continue;

            std::string arrayContent = content.substr(arrayStart, arrayEnd - arrayStart + 1);

            // Parse each dep object within the array
            size_t searchPos = 0;
            while (true)
            {
                size_t objStart = arrayContent.find('{', searchPos);
                if (objStart == std::string::npos) break;
                size_t objEnd = arrayContent.find('}', objStart);
                if (objEnd == std::string::npos) break;

                std::string obj = arrayContent.substr(objStart, objEnd - objStart + 1);
                searchPos = objEnd + 1;

                // Extract fields from the dep object
                auto extractField = [&obj](const std::string& key) -> std::string
                {
                    std::string needle = "\"" + key + "\"";
                    size_t pos = obj.find(needle);
                    if (pos == std::string::npos) return "";
                    pos = obj.find(':', pos + needle.size());
                    if (pos == std::string::npos) return "";
                    pos = obj.find('"', pos + 1);
                    if (pos == std::string::npos) return "";
                    pos++;
                    size_t end = obj.find('"', pos);
                    if (end == std::string::npos) return "";
                    return obj.substr(pos, end - pos);
                };

                DependencyInfo dep;
                dep.name = extractField("name");
                dep.version = extractField("version");
                dep.gitUrl = extractField("git");
                dep.source = dep.gitUrl.empty() ? PackageSourceType::ESPIDFRegistry : PackageSourceType::GitHub;

                if (!dep.name.empty() && !dep.version.empty())
                {
                    // Deduplicate: if same dep from another package, keep higher version
                    bool replaced = false;
                    for (auto& existing : packageDeps)
                    {
                        if (existing.name == dep.name)
                        {
                            if (DekiEditor::CompareVersions(dep.version, existing.version) > 0)
                            {
                                existing.version = dep.version;
                                existing.gitUrl = dep.gitUrl;
                                existing.source = dep.source;
                            }
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced)
                    {
                        packageDeps.push_back(dep);
                    }
                }
            }
        }
    }
    catch (const fs::filesystem_error&)
    {
        return;
    }

    if (packageDeps.empty()) return;

    MergeDeps(projectPath, packageDeps);
}

void ESPIDFInstaller::MergeDeps(const std::string& projectPath, const std::vector<DependencyInfo>& deps)
{
    if (deps.empty()) return;

    // Read current idf_component.yml
    fs::path yamlPath = GetYamlPath(projectPath);
    std::vector<DependencyInfo> existingDeps;
    ParseYaml(yamlPath, existingDeps);

    // Merge: add new deps or upgrade existing ones to higher versions
    bool modified = false;
    for (const auto& dep : deps)
    {
        DependencyInfo* match = nullptr;
        for (auto& existing : existingDeps)
        {
            if (existing.name == dep.name)
            {
                match = &existing;
                break;
            }
            // Also check short name (e.g., "esp_tinyusb" matches "espressif/esp_tinyusb")
            size_t slashPos = dep.name.find('/');
            if (slashPos != std::string::npos)
            {
                if (existing.name == dep.name.substr(slashPos + 1))
                {
                    match = &existing;
                    break;
                }
            }
        }

        if (!match)
        {
            existingDeps.push_back(dep);
            modified = true;
        }
        else if (DekiEditor::CompareVersions(dep.version, match->version) > 0)
        {
            // Needs a higher version — upgrade
            match->version = dep.version;
            match->gitUrl = dep.gitUrl;
            match->source = dep.source;
            modified = true;
        }
    }

    if (modified)
    {
        WriteYaml(yamlPath, existingDeps);
    }
}
