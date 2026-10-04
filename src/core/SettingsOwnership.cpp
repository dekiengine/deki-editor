#include "SettingsOwnership.h"

#include <map>
#include <mutex>

namespace DekiEditor
{
namespace SettingsOwnership
{
namespace
{
// Package load happens on the main thread today, but hot reload has moved work off
// it before; a mutex here is cheap next to a LoadLibrary.
std::mutex& Lock()
{
    static std::mutex m;
    return m;
}

std::map<std::string, std::string>& Map()
{
    static std::map<std::string, std::string> sectionToPackage;
    return sectionToPackage;
}
}  // namespace

void SetOwner(const std::string& sectionName, const std::string& packageId)
{
    if (sectionName.empty() || packageId.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> guard(Lock());
    Map()[sectionName] = packageId;
}

std::string OwnerOf(const std::string& sectionName)
{
    std::lock_guard<std::mutex> guard(Lock());
    auto it = Map().find(sectionName);
    return it == Map().end() ? std::string() : it->second;
}

void ClearPackage(const std::string& packageId)
{
    std::lock_guard<std::mutex> guard(Lock());
    for (auto it = Map().begin(); it != Map().end();)
    {
        it = (it->second == packageId) ? Map().erase(it) : std::next(it);
    }
}

void Clear()
{
    std::lock_guard<std::mutex> guard(Lock());
    Map().clear();
}

}  // namespace SettingsOwnership
}  // namespace DekiEditor
