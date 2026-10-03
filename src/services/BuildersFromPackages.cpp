#include "BuildersFromPackages.h"

#include "PackageLoader.h"
#include "PackageManager.h"

#include <deki-editor/PackageInstallerPlugin.h>
#include <deki-editor/build/BuilderPlugin.h>
#include <deki-editor/build/BuilderRegistry.h>
#include <deki-editor/build/CMakeGenUtils.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/build/TargetBuilder.h>
#include <deki/LogSystem.h>

#include <string>

namespace DekiEditor
{

void RegisterBuildersFromPackages(PackageLoader& loader)
{
    auto& registry = BuilderRegistry::Instance();

    for (const PackageInfo& pkg : loader.GetLoadedPackages())
    {
        // Most packages export nothing of the sort: the normal case, not worth a word.
        auto getAbi =
            (const DekiBuilderAbi* (*)())loader.GetFunction(pkg.name, "DekiBuilder_GetAbi");
        if (getAbi == nullptr)
            continue;

        auto getName = (const char* (*)())loader.GetFunction(pkg.name, "DekiBuilder_GetName");
        auto getCount = (int (*)())loader.GetFunction(pkg.name, "DekiBuilder_GetBuilderCount");
        auto create =
            (ITargetBuilder * (*)(int)) loader.GetFunction(pkg.name, "DekiBuilder_CreateBuilder");
        auto destroy =
            (void (*)(ITargetBuilder*))loader.GetFunction(pkg.name, "DekiBuilder_DestroyBuilder");

        if (getName == nullptr || getCount == nullptr || create == nullptr || destroy == nullptr)
        {
            DEKI_LOG_ERROR("Package '%s' exports DekiBuilder_GetAbi but not the rest of the "
                           "builder ABI; ignoring its backend",
                           pkg.name.c_str());
            continue;
        }

        // Read the version THROUGH the pointer before trusting the struct: a
        // package built against another revision may return a differently
        // sized one, and copying it first is the bug PluginLoader documents.
        const DekiBuilderAbi* abi = getAbi();
        if (abi == nullptr || abi->version != kDekiBuilderAbiVersion ||
            abi->structSize != sizeof(DekiBuilderAbi) ||
            abi->platformConfigSize != sizeof(PlatformConfig) ||
            abi->packageEntrySize != sizeof(CMakeGen::PackageEntry) ||
            abi->stdStringSize != sizeof(std::string) ||
            abi->compilerTag != DekiBuilder_CompilerTag())
        {
            DEKI_LOG_ERROR("Package '%s' provides a build backend built against a different ABI; "
                           "refusing it. Rebuild the package with this editor.",
                           pkg.name.c_str());
            continue;
        }

        const int count = getCount();
        for (int i = 0; i < count; ++i)
        {
            ITargetBuilder* builder = create(i);
            if (builder == nullptr)
                continue;

            // Claimed by an earlier pass (project reopen, hot reload): a
            // duplicate would leave the registry dispatching to a stale one.
            if (registry.GetBuilder(builder->GetFrameworkId()) != nullptr)
            {
                destroy(builder);
                continue;
            }

            DEKI_LOG_EDITOR("Package '%s' provides build backend '%s' (framework '%s')",
                            pkg.name.c_str(), builder->GetName(),
                            builder->GetFrameworkId().c_str());
            registry.RegisterPluginBuilder(builder, destroy, pkg.name);
        }
    }
}

void RegisterInstallersFromPackages(PackageLoader& loader, ::PackageManager& manager)
{
    for (const PackageInfo& pkg : loader.GetLoadedPackages())
    {
        auto getCount = (int (*)())loader.GetFunction(pkg.name, "DekiInstaller_GetCount");
        if (getCount == nullptr)
            continue;

        auto create =
            (IPackageInstaller * (*)(int)) loader.GetFunction(pkg.name, "DekiInstaller_Create");
        auto destroy =
            (void (*)(IPackageInstaller*))loader.GetFunction(pkg.name, "DekiInstaller_Destroy");
        if (create == nullptr || destroy == nullptr)
        {
            DEKI_LOG_ERROR("Package '%s' exports DekiInstaller_GetCount but not Create/Destroy; "
                           "ignoring its installer",
                           pkg.name.c_str());
            continue;
        }

        const int count = getCount();
        for (int i = 0; i < count; ++i)
        {
            IPackageInstaller* installer = create(i);
            if (installer == nullptr)
                continue;

            // A second copy would double every listing the manager produces.
            if (manager.HasInstaller(installer->GetId()))
            {
                destroy(installer);
                continue;
            }

            DEKI_LOG_EDITOR("Package '%s' provides package installer '%s' (id '%s')", pkg.name.c_str(),
                            installer->GetPlatformName().c_str(), installer->GetId().c_str());
            manager.RegisterPluginInstaller(installer, destroy, pkg.name);
        }
    }
}

}  // namespace DekiEditor
