#include "PackageManifest.h"
#include <deki-editor/SafeNames.h>

#include "PackageManager.h"

#include <deki-editor/FileIO.h>
#include <deki/LogSystem.h>

namespace DekiEditor
{

void ReadPackageSource(const std::string& spelling, bool emptyIsCustom, PackageEntry& pkg)
{
    if (spelling == "github")
    {
        pkg.source = PackageSourceType::GitHub;
    }
    else if (spelling == "custom" || (spelling.empty() && emptyIsCustom))
    {
        pkg.source = PackageSourceType::Custom;
    }
    else if (!spelling.empty())
    {
        pkg.source = PackageSourceType::FrameworkRegistry;
        pkg.installer = spelling;
    }
}

std::string PackageSourceToString(const PackageEntry& pkg)
{
    switch (pkg.source)
    {
        case PackageSourceType::GitHub: return "github";
        case PackageSourceType::FrameworkRegistry:
            if (!pkg.installer.empty())
            {
                return pkg.installer;
            }
            // Nothing sets this source without knowing the installer, so this
            // is a bug upstream. Say so; "custom" keeps the file loadable,
            // where an empty key would read back differently in the manifest
            // and the lock.
            DEKI_LOG_ERROR("PackageManager: '%s' comes from a framework registry but no installer is "
                           "recorded for it; written as \"custom\"",
                           pkg.id.c_str());
            return "custom";
        default: return "custom";
    }
}

nlohmann::json BuildPackageManifest(const std::vector<PackageEntry>& packages,
                                    const std::map<std::string, std::string>& versionOverrides)
{
    using json = nlohmann::json;

    json manifest;
    manifest["schemaVersion"] = 1;
    manifest["packages"] = json::object();

    for (const auto& pkg : packages)
    {
        // An entry with no id would be written keyed on "", which nothing can
        // match or install again. Producers always supply one (see
        // DekiPackageInstaller::GetInstalledPackages); this check keeps a gap
        // anywhere else from corrupting the manifest.
        if (pkg.id.empty())
        {
            continue;
        }
        // And an id that is not a plain name would be written back as a path
        // segment the next restore trusts (SafeNames.h).
        {
            std::string reason;
            if (!SafeNames::IsSafeName(pkg.id, reason))
            {
                continue;
            }
        }

        // Three kinds of entry belong in the manifest, and only these:
        //
        //   declared - the project names it in deki-packages.json. It stays
        //     even when another package also requires it. Must not depend on
        //     resolvedBy, or opening a project deletes the declarations of
        //     every package some other package requires.
        //
        //   adopted - installed on disk with nothing else pulling it in, i.e.
        //     someone cloned it into project/packages/ by hand. Recording it
        //     makes that stick across a fresh checkout.
        //
        //   developing - marked editable, which is intent in its own right
        //     and must survive a reload. A package pulled in by another
        //     (resolvedBy set) is neither declared nor adopted, so without
        //     this its editable flag would be dropped on save and the restore
        //     pass would stash its local edits and reset the checkout.
        //
        // Everything else is left out, above all the registry catalogue: a
        // refresh merges every available package into the list so the panel
        // can show it. Those are offers, not intent; writing them would make
        // the next open restore the whole registry.
        const bool declared = pkg.declaredInManifest;
        const bool adopted = pkg.isInstalled && pkg.resolvedBy.empty();
        const bool developing = pkg.editable;
        if (!declared && !adopted && !developing)
        {
            continue;
        }

        json entry;
        entry["version"] = pkg.installedVersion.empty() ? "*" : pkg.installedVersion;

        // source and url only for non-registry packages
        if (pkg.source == PackageSourceType::GitHub || pkg.source == PackageSourceType::FrameworkRegistry)
        {
            entry["source"] = PackageSourceToString(pkg);
            if (!pkg.url.empty())
            {
                entry["url"] = pkg.url;
            }
        }

        // Written only when true, so a manifest that never used the flag stays
        // byte-identical and opening a project cannot dirty it.
        if (pkg.editable)
        {
            entry["editable"] = true;
        }

        manifest["packages"][pkg.id] = entry;
    }

    if (!versionOverrides.empty())
    {
        json deps;
        for (const auto& [n, v] : versionOverrides)
        {
            deps[n] = v;
        }
        manifest["dependencies"] = deps;
    }

    return manifest;
}

nlohmann::json BuildPackageManifestForIds(const std::vector<std::string>& packageIds)
{
    using json = nlohmann::json;
    json manifest;
    manifest["schemaVersion"] = 1;
    manifest["packages"] = json::object();
    for (const auto& id : packageIds)
    {
        if (id.empty())
        {
            continue;
        }
        manifest["packages"][id] = json{ { "version", "*" } };
    }
    return manifest;
}

bool WritePackageManifest(const std::string& path, const nlohmann::json& manifest, std::string& err)
{
    return AtomicWriteFile(path, manifest.dump(2), err);
}

}  // namespace DekiEditor
