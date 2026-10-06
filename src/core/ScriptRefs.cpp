#include "ScriptRefs.h"

namespace DekiEditor
{

bool ResolveScriptRefs(nlohmann::json& value, const std::map<std::string, nlohmann::json>& saved, std::string& error)
{
    if (value.is_object() || value.is_array())
    {
        for (auto& item : value)
        {
            if (!ResolveScriptRefs(item, saved, error))
            {
                return false;
            }
        }
        return true;
    }
    if (!value.is_string())
    {
        return true;
    }

    const std::string text = value.get<std::string>();
    std::string out;
    size_t at = 0;
    while (true)
    {
        const size_t start = text.find("${", at);
        const size_t end = start == std::string::npos ? std::string::npos : text.find('}', start);
        if (end == std::string::npos)
        {
            out += text.substr(at);
            break;
        }

        const std::string ref = text.substr(start + 2, end - start - 2);
        const size_t dot = ref.find('.');
        const auto found = saved.find(ref.substr(0, dot));
        if (found == saved.end())
        {
            error = "no earlier step saved a result as '" + ref.substr(0, dot) + "'";
            return false;
        }
        const nlohmann::json* node = &found->second;
        size_t keyStart = dot == std::string::npos ? std::string::npos : dot + 1;
        while (keyStart != std::string::npos)
        {
            const size_t next = ref.find('.', keyStart);
            const std::string key = ref.substr(keyStart, next == std::string::npos ? next : next - keyStart);
            if (!node->is_object() || !node->contains(key))
            {
                error = "'" + ref + "' is not in that step's result";
                return false;
            }
            node = &(*node)[key];
            keyStart = next == std::string::npos ? next : next + 1;
        }

        if (start == 0 && end == text.size() - 1)
        {
            value = *node;
            return true;
        }
        out += text.substr(at, start - at);
        out += node->is_string() ? node->get<std::string>() : node->dump();
        at = end + 1;
    }
    value = out;
    return true;
}

}  // namespace DekiEditor
