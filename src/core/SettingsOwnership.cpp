#include "SettingsOwnership.h"

#include <map>
#include <mutex>

namespace DekiEditor
{
namespace SettingsOwnership
{
namespace
{
// Packages load on the main thread, but hot reload may move that work off it.
// A mutex is cheap next to a LoadLibrary.
std::mutex& Lock()
{
    static std::mutex s_M;
    return s_M;
}

std::map<std::string, std::string>& Map()
{
    static std::map<std::string, std::string> s_SectionToPackage;
    return s_SectionToPackage;
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
