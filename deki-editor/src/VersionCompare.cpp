#include <deki-editor/VersionCompare.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace DekiEditor
{

namespace
{
    std::vector<int> ParseSegments(const std::string& v)
    {
        std::vector<int> segments;
        std::string current;
        for (char c : v)
        {
            if (c == '.')
            {
                segments.push_back(current.empty() ? 0 : std::atoi(current.c_str()));
                current.clear();
            }
            else
            {
                current += c;
            }
        }
        segments.push_back(current.empty() ? 0 : std::atoi(current.c_str()));
        return segments;
    }
}

int CompareVersions(const std::string& a, const std::string& b)
{
    const std::vector<int> segsA = ParseSegments(a);
    const std::vector<int> segsB = ParseSegments(b);
    const size_t maxLen = std::max(segsA.size(), segsB.size());
    for (size_t i = 0; i < maxLen; i++)
    {
        const int va = (i < segsA.size()) ? segsA[i] : 0;
        const int vb = (i < segsB.size()) ? segsB[i] : 0;
        if (va < vb) return -1;
        if (va > vb) return 1;
    }
    return 0;
}

std::string HighestVersion(const std::vector<std::string>& versions)
{
    std::string best;
    for (const std::string& v : versions)
    {
        if (v.empty())
            continue;
        if (best.empty() || CompareVersions(v, best) > 0)
            best = v;
    }
    return best;
}

}  // namespace DekiEditor
