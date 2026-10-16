#include "PackagePins.h"

#include "GitProcess.h"
#include "PackageIntegrity.h"
#include "installers/DekiPackageInstaller.h"
#include <deki/LogSystem.h>
#include <deki-editor/Paths.h>
#include <deki-editor/VersionCompare.h>

#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace DekiEditor
{

// Versions pinned in deki-packages.json, keyed by package id. "*" means
// "any" and is returned as is, for the caller to treat as unpinned.
std::map<std::string, std::string> ReadManifestVersions(const std::string& projectPath)
{
    std::map<std::string, std::string> out;
    const fs::path manifest = fs::path(projectPath) / GetPackageManifestFileName();
    if (!fs::exists(manifest))
    {
        return out;
    }

    try
    {
        std::ifstream f(manifest);
        nlohmann::json j;
        f >> j;
        if (j.contains("packages") && j["packages"].is_object())
        {
            for (auto& [id, entry] : j["packages"].items())
            {
                if (entry.is_object() && entry.contains("version") && entry["version"].is_string())
                {
                    out[id] = entry["version"].get<std::string>();
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        DEKI_LOG_WARNING("PackagePins: deki-packages.json unreadable (%s); reading no pinned versions", e.what());
    }

    return out;
}

namespace
{

// The commit the installer checked out, from its stamp (.deki-package.json).
// Empty when the stamp has none: an install from before it was recorded, or
// a clone made by hand.
std::string InstalledCommit(const fs::path& packageDir)
{
    std::ifstream in(packageDir / ".deki-package.json");
    if (!in)
    {
        return "";
    }
    const nlohmann::json stamp = nlohmann::json::parse(in, nullptr, false);
    return stamp.is_object() ? stamp.value("commit", "") : "";
}

}  // namespace

std::vector<PinMismatch> FindPinMismatches(const std::string& projectPath, PackageManager& mgr)
{
    // "v0.17.0" and "0.17.0" name one tag.
    auto bare = [](const std::string& v) { return (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? v.substr(1) : v; };
    auto numeric = [](const std::string& v)
    { return !v.empty() && v.find_first_not_of("0123456789.") == std::string::npos; };

    std::vector<PinMismatch> out;
    const std::map<std::string, std::string> pinned = ReadManifestVersions(projectPath);
    for (const PackageEntry& pkg : mgr.GetPackages())
    {
        auto it = pinned.find(pkg.id);
        if (it == pinned.end() || !pkg.isInstalled || pkg.installedVersion.empty())
        {
            continue;
        }
        const std::string pin = bare(it->second);
        const std::string have = bare(pkg.installedVersion);
        if (!numeric(pin) || !numeric(have))
        {
            continue;  // "*", a branch, or a version this cannot read
        }
        if (CompareVersions(pin, have) != 0)
        {
            out.push_back({ pkg.id, it->second, pkg.installedVersion });
        }
    }
    return out;
}

std::string DescribePinMismatch(const PinMismatch& m)
{
    return "'" + m.id + "' is pinned to " + m.pinned + " in deki-packages.json, but " + m.installed +
           " is installed. Run --packages-add " + m.id + " --version " + m.pinned +
           " to install the pinned version, or change the pin.";
}

PinApplyResult ApplyPinnedVersions(const std::string& projectPath, PackageManager& mgr)
{
    PinApplyResult result;
    const std::vector<PinMismatch> mismatches = FindPinMismatches(projectPath, mgr);
    if (mismatches.empty())
    {
        return result;
    }

    const fs::path packagesDir = fs::path(GetPackagesDirectory(projectPath));
    for (const PinMismatch& m : mismatches)
    {
        auto it = std::find_if(mgr.GetPackages().begin(), mgr.GetPackages().end(),
                               [&](const PackageEntry& e) { return e.id == m.id; });
        if (it == mgr.GetPackages().end())
        {
            continue;
        }
        // A copy: installing can append to (and reallocate) the package vector.
        PackageEntry pkg = *it;
        const std::string dirName = DekiPackageInstaller::GetRepoName(pkg);
        const fs::path dir = packagesDir / dirName;
        const std::string why = DescribePinMismatch(m);

        // A junction is the developer's own repo; the installer would skip it anyway.
        if (IsReparsePoint(dir))
        {
            result.kept.push_back(why + " (left alone: it is a link to another folder)");
            continue;
        }

        // An editable package on a branch is being developed. One the
        // installer put on a tag has a detached HEAD, and moves like any other.
        std::string out;
        if (pkg.editable && RunGit({ "-C", dir.string(), "symbolic-ref", "-q", "HEAD" }, out) == 0)
        {
            result.kept.push_back(why + " (left alone: it is marked editable and on a branch)");
            continue;
        }

        // The installer records the commit it checked out. HEAD anywhere else
        // means someone moved it (a commit, a checkout, a pull), so it is theirs.
        // An install from before the commit was recorded relies on the checks
        // above and the installer's own.
        const std::string installed = InstalledCommit(dir);
        if (!installed.empty())
        {
            std::string head;
            RunGit({ "-C", dir.string(), "rev-parse", "HEAD" }, head);
            while (!head.empty() && (head.back() == '\n' || head.back() == '\r'))
            {
                head.pop_back();
            }
            if (head != installed)
            {
                result.kept.push_back(why + " (left alone: it is not on the commit the editor installed)");
                continue;
            }
        }

        std::string whyNot;
        IPackageInstaller* installer = mgr.GetInstallerFor(pkg, projectPath, whyNot);
        if (!installer)
        {
            result.kept.push_back(why + " (" + whyNot + ")");
            continue;
        }

        bool ok = false;
        std::string message;
        mgr.InstallWithDependencies(projectPath, pkg, m.pinned, installer,
                                    [&](bool success, const std::string& msg)
                                    {
                                        ok = success;
                                        message = msg;
                                    });
        if (ok)
        {
            result.updated.push_back(m.id + " " + m.installed + " -> " + m.pinned);
        }
        else
        {
            result.kept.push_back(why + " (could not switch: " + (message.empty() ? "no reason given" : message) +
                                  ")");
        }
    }

    mgr.RefreshInstalledStatus(projectPath);
    if (result.DidWork())
    {
        mgr.SavePackages(projectPath);
        // The next build regenerates the build files for the new sources.
        std::error_code ec;
        fs::remove(fs::path(projectPath) / "CMakeLists.txt", ec);
    }
    return result;
}

}  // namespace DekiEditor
