#include "ProjectUpgrade.h"

#include "HostedRename.h"
#include "installers/DekiPackageInstaller.h"

#include <deki-editor/Paths.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace DekiEditor
{

bool ProjectUpgrade::Needed() const
{
    if (!files.empty())
    {
        return true;
    }
    for (const Package& p : packages)
    {
        if (!p.editable)
        {
            return true;
        }
    }
    return false;
}

std::string ProjectUpgrade::Describe() const
{
    std::string text = "This project was made with an older Deki.";
    if (!files.empty())
    {
        text += " Its files use names this release changed (desktop-os is \"hosted\", SIMULATOR is DEKI_HOSTED); "
                "the upgrade changes them in " +
                std::to_string(files.size()) + (files.size() == 1 ? " file: " : " files: ");
        for (size_t i = 0; i < files.size(); ++i)
        {
            text += (i ? ", " : "") + files[i];
        }
        text += ".";
    }
    std::string update;
    std::string own;
    for (const Package& p : packages)
    {
        std::string& into = p.editable ? own : update;
        into += (into.empty() ? "" : ", ") + p.id + " " + p.version + " (made for " + p.minEngine + ")";
    }
    if (!update.empty())
    {
        text += " These packages are from an older release, and the upgrade moves them to this editor's: " + update +
                ".";
    }
    if (!own.empty())
    {
        text += " These are editable and from an older release, so they are yours to update: " + own + ".";
    }
    text += " Scenes, assets and settings stay as they are, and version control shows every change.";
    return text;
}

ProjectUpgrade PlanProjectUpgrade(const std::string& projectPath)
{
    ProjectUpgrade plan;
    plan.files = HostedRename::MigrateProject(projectPath, /*apply*/ false);

    std::ifstream in(GetPackageManifestFile(projectPath));
    const nlohmann::json manifest = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
    if (!manifest.is_object() || !manifest.contains("packages") || !manifest["packages"].is_object())
    {
        return plan;
    }
    for (const auto& [id, entry] : manifest["packages"].items())
    {
        if (!entry.is_object())
        {
            continue;
        }
        const fs::path dir = fs::path(GetPackagesDirectory(projectPath)) / id;
        std::string minEngine;
        if (DekiPackageInstaller::IsFromOlderRelease(dir, minEngine))
        {
            plan.packages.push_back({ id, entry.value("version", std::string()), minEngine, entry.value("editable", false) });
        }
    }
    return plan;
}

}  // namespace DekiEditor
