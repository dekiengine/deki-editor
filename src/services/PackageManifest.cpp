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
        case PackageSourceType::GitHub:
            return "github";
        case PackageSourceType::FrameworkRegistry:
            if (!pkg.installer.empty())
                return pkg.installer;
            // Nothing sets this source without also knowing the installer, so
            // this is a bug upstream. Say so; "custom" at least keeps the file
            // loadable, where an empty key would read back differently in the
            // manifest and the lock.
            DEKI_LOG_ERROR("PackageManager: '%s' comes from a framework registry but no installer is "
                           "recorded for it; written as \"custom\"",
                           pkg.id.c_str());
            return "custom";
        default:
            return "custom";
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
        // ever match or install again. Producers are expected to always supply
        // one (see DekiPackageInstaller::GetInstalledPackages); this is the
        // backstop so a gap anywhere else cannot corrupt the manifest.
        if (pkg.id.empty()) continue;
        // And an id that is not a plain name would be written back as a path
        // segment the next restore trusts (SafeNames.h).
        {
            std::string reason;
            if (!SafeNames::IsSafeName(pkg.id, reason)) continue;
        }

        // Two things belong in the manifest, and only these two:
        //
        //   declared — the project names it in deki-packages.json. It stays,
        //     full stop, even when another package also requires it. Keying
        //     this off resolvedBy instead used to delete the declaration: a
        //     project declaring 25 packages was rewritten down to 13 the first
        //     time it opened, losing exactly the ones some other package
        //     happened to require.
        //
        //   adopted — installed on disk with nothing else pulling it in, i.e.
        //     someone cloned it into project/packages/ by hand. Recording it
        //     is what makes that stick across a fresh checkout.
        //
        // Everything else is excluded, and the important exclusion is the
        // registry catalogue: a refresh merges every available package into
        // the list so the panel can show it. Those are offers, not intent —
        // writing them would make the next open restore the whole registry.
        // They are neither declared nor installed, so they fall out here.
        //   developing — marked editable. That is a statement of intent in its
        //     own right, and it has to survive a reload or it means nothing.
        //     A package pulled in by another (resolvedBy set) is neither
        //     declared nor adopted, so without this its editable flag was
        //     dropped on every save while the caller was told it had been set
        //     — and the package stayed exposed to the restore pass that stashes
        //     local edits and resets the checkout.
        //
        //     This only ever ADDS entries, so it cannot bring back the bug
        //     described above, where inclusion keyed off resolvedBy and deleted
        //     declarations.
        const bool declared = pkg.declaredInManifest;
        const bool adopted = pkg.isInstalled && pkg.resolvedBy.empty();
        const bool developing = pkg.editable;
        if (!declared && !adopted && !developing) continue;

        json entry;
        entry["version"] = pkg.installedVersion.empty() ? "*" : pkg.installedVersion;

        // Only include source/url for non-registry packages
        if (pkg.source == PackageSourceType::GitHub || pkg.source == PackageSourceType::FrameworkRegistry)
        {
            entry["source"] = PackageSourceToString(pkg);
            if (!pkg.url.empty()) entry["url"] = pkg.url;
        }

        // Written only when true: a manifest that never used the flag stays
        // byte-identical, so opening a project cannot dirty it.
        if (pkg.editable)
            entry["editable"] = true;

        manifest["packages"][pkg.id] = entry;
    }

    if (!versionOverrides.empty())
    {
        json deps;
        for (const auto& [n, v] : versionOverrides)
            deps[n] = v;
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
        if (id.empty()) continue;
        manifest["packages"][id] = json{ { "version", "*" } };
    }
    return manifest;
}

bool WritePackageManifest(const std::string& path, const nlohmann::json& manifest, std::string& err)
{
    return AtomicWriteFile(path, manifest.dump(2), err);
}

}  // namespace DekiEditor
