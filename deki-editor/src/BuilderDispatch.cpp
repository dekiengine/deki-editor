#include <deki-editor/build/BuilderDispatch.h>

#include <deki-editor/build/TargetBuilder.h>

namespace DekiEditor
{

ITargetBuilder* SelectBuilder(const std::vector<ITargetBuilder*>& builders, const std::string& framework)
{
    if (framework.empty())
    {
        return nullptr;
    }

    for (ITargetBuilder* builder : builders)
    {
        if (builder != nullptr && builder->GetFrameworkId() == framework)
        {
            return builder;
        }
    }
    return nullptr;
}

std::string DescribeNoBuilder(const std::vector<ITargetBuilder*>& builders, const std::string& framework,
                              const std::vector<std::string>& packageBuildErrors)
{
    std::string msg =
        "No build backend for framework '" + (framework.empty() ? std::string("<unset>") : framework) + "'. ";

    std::string available;
    for (ITargetBuilder* builder : builders)
    {
        if (builder == nullptr)
        {
            continue;
        }
        if (!available.empty())
        {
            available += ", ";
        }
        available += builder->GetFrameworkId();
    }

    if (available.empty())
    {
        msg += "No build backends are registered at all.";
    }
    else
    {
        msg += "Registered: " + available + ".";
    }

    // A backend is a package's, so the usual reason none is there is that the
    // packages did not build: say that, with what they failed on, rather than
    // leave it looking like a missing install.
    if (!packageBuildErrors.empty())
    {
        msg += " Build backends come from the project's packages, and the last build of them failed, so the "
               "backends they bring are not loaded. It failed on: ";
        const size_t shown = packageBuildErrors.size() < 3 ? packageBuildErrors.size() : 3;
        for (size_t i = 0; i < shown; ++i)
        {
            msg += (i ? " | " : "") + packageBuildErrors[i];
        }
        if (packageBuildErrors.size() > shown)
        {
            msg += " (and " + std::to_string(packageBuildErrors.size() - shown) + " more lines)";
        }
        msg += ". Fix that (the Console lists it all, as does the rebuild tool) and build again.";
    }
    else
    {
        msg += " Build backends come from packages: install the one that builds for '" +
               (framework.empty() ? std::string("<unset>") : framework) + "'.";
    }

    return msg;
}

}  // namespace DekiEditor
