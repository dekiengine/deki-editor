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

std::string DescribeNoBuilder(const std::vector<ITargetBuilder*>& builders, const std::string& framework)
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

    return msg;
}

}  // namespace DekiEditor
