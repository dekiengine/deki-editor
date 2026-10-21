#include <deki-editor/EditorNaming.h>

#include <deki/reflection/Property.h>

#include <cctype>
#include <unordered_map>

namespace DekiEditor
{

namespace EditorNaming
{

std::string NicifyName(const char* name)
{
    if (!name || !name[0])
    {
        return "";
    }

    std::string result;
    bool capitalizeNext = true;

    for (size_t i = 0; name[i]; ++i)
    {
        char c = name[i];

        // Separators (underscores, and the dashes and spaces of file names)
        // become one space, and the next letter is capitalized.
        if (c == '_' || c == '-' || c == ' ')
        {
            if (!result.empty() && result.back() != ' ')
            {
                result += ' ';
            }
            capitalizeNext = true;
            continue;
        }

        // A space before an uppercase letter that follows a lowercase one, or
        // that starts a word after a digit ("stop1Position" -> "Stop 1
        // Position", but "texture2D" -> "Texture 2D").
        if (std::isupper(c) && i > 0 &&
            (std::islower(name[i - 1]) || (std::isdigit(name[i - 1]) && std::islower(name[i + 1]))))
        {
            result += ' ';
        }
        // A space before a digit that follows a letter.
        else if (std::isdigit(c) && i > 0 && std::isalpha(name[i - 1]))
        {
            result += ' ';
        }

        if (capitalizeNext)
        {
            result += static_cast<char>(std::toupper(c));
            capitalizeNext = false;
        }
        else
        {
            result += c;
        }
    }

    return result;
}

namespace
{
struct CachedName
{
    const char* name;         // the table entry's name pointer at caching time
    const char* displayName;  // and its displayName pointer, to detect a reused address
    std::string text;
};
std::unordered_map<const Deki::PropertyInfo*, CachedName> s_DisplayNames;
}  // namespace

const std::string& GetDisplayName(const Deki::PropertyInfo& prop)
{
    auto it = s_DisplayNames.find(&prop);
    if (it != s_DisplayNames.end() && it->second.name == prop.name && it->second.displayName == prop.displayName)
    {
        return it->second.text;
    }

    CachedName entry{ prop.name, prop.displayName,
                      prop.displayName ? std::string(prop.displayName) : NicifyName(prop.name) };
    return (s_DisplayNames[&prop] = std::move(entry)).text;
}

void ClearDisplayNameCache()
{
    s_DisplayNames.clear();
}

}  // namespace EditorNaming

}  // namespace DekiEditor
