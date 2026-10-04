#include "FileDialogFilter.h"

namespace DekiEditor
{
namespace FileDialogs
{

namespace
{

// "*.scene" -> "scene", "*.*" -> "*", "*.png;*.jpg" -> "png;jpg". Anything
// that is not a "*.ext" glob passes through, so a list of bare extensions
// stays as it is.
std::string ToBareExtensions(const std::string& spec)
{
    std::string out;
    size_t start = 0;
    while (start <= spec.size())
    {
        size_t end = spec.find(';', start);
        if (end == std::string::npos)
        {
            end = spec.size();
        }

        std::string one = spec.substr(start, end - start);

        // Trim the spaces a hand-written filter may carry between globs.
        size_t b = one.find_first_not_of(" \t");
        size_t e = one.find_last_not_of(" \t");
        one = (b == std::string::npos) ? std::string() : one.substr(b, e - b + 1);

        if (one == "*.*" || one == "*")
        {
            one = "*";
        }
        else if (one.size() > 2 && one[0] == '*' && one[1] == '.')
        {
            one = one.substr(2);
        }

        if (!one.empty())
        {
            if (!out.empty())
            {
                out += ';';
            }
            out += one;
        }

        if (end == spec.size())
        {
            break;
        }
        start = end + 1;
    }
    return out;
}

}  // namespace

std::vector<DialogFilter> ParseWin32Filter(const char* filter)
{
    std::vector<DialogFilter> filters;
    if (!filter || filter[0] == '\0')
    {
        return filters;
    }

    // Walk the NUL-separated strings. The list ends with a second NUL right
    // after the one that ended the last string, so an empty string ends it.
    const char* cursor = filter;
    while (*cursor != '\0')
    {
        std::string name(cursor);
        cursor += name.size() + 1;

        // A name with no spec after it is malformed; drop it rather than read
        // past the end looking for one.
        if (*cursor == '\0')
        {
            break;
        }

        std::string spec(cursor);
        cursor += spec.size() + 1;

        std::string pattern = ToBareExtensions(spec);
        if (!pattern.empty())
        {
            filters.push_back({ name, pattern });
        }
    }

    return filters;
}

}  // namespace FileDialogs
}  // namespace DekiEditor
