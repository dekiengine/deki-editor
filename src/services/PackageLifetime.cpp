#include "PackageLifetime.h"

#include <utility>
#include <vector>

namespace DekiEditor
{
namespace PackageLifetime
{
namespace
{
std::vector<std::pair<const void*, std::function<void()>>>& Releases()
{
    static std::vector<std::pair<const void*, std::function<void()>>> s_Releases;
    return s_Releases;
}
}  // namespace

void OnBeforeUnload(const void* owner, std::function<void()> release)
{
    Forget(owner);
    Releases().emplace_back(owner, std::move(release));
}

void Forget(const void* owner)
{
    auto& list = Releases();
    for (auto it = list.begin(); it != list.end();)
    {
        it = (it->first == owner) ? list.erase(it) : it + 1;
    }
}

void ReleaseAll()
{
    // A copy: a release may register or forget.
    const auto list = Releases();
    for (const auto& [owner, release] : list)
    {
        if (release)
        {
            release();
        }
    }
}
}  // namespace PackageLifetime
}  // namespace DekiEditor
