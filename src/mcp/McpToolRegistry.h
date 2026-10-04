#pragma once

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <nlohmann/json.hpp>

namespace DekiEditor
{

struct McpToolDescriptor
{
    std::string name;
    std::string description;
    nlohmann::json inputSchema;  // JSON Schema object
    std::function<nlohmann::json(const nlohmann::json& arguments)> handler;
};

/// The arguments a tool's schema requires that `args` does not have, joined
/// with ", "; empty when none are missing. Checked before every call: handlers
/// read their arguments with operator[], which on a missing key asserts in a
/// debug build - behind a message box that froze the editor's main thread
/// while its window still answered.
inline std::string MissingRequiredArguments(const McpToolDescriptor& tool, const nlohmann::json& args)
{
    std::string missing;
    const auto required = tool.inputSchema.find("required");
    if (required == tool.inputSchema.end() || !required->is_array())
    {
        return missing;
    }
    for (const auto& name : *required)
    {
        if (!name.is_string())
        {
            continue;
        }
        if (!args.is_object() || !args.contains(name.get<std::string>()))
        {
            missing += (missing.empty() ? "" : ", ") + name.get<std::string>();
        }
    }
    return missing;
}

class McpToolRegistry
{
public:
    void Register(McpToolDescriptor tool) { m_Tools.push_back(std::move(tool)); }

    /// Tools that are not known when the registry is built: the ones packages
    /// add (CliTool), which come and go as packages load and unload. Asked
    /// afresh on every listing and lookup.
    void SetDynamicSource(std::function<std::vector<McpToolDescriptor>()> source) { m_Dynamic = std::move(source); }

    std::vector<McpToolDescriptor> GetTools() const
    {
        std::vector<McpToolDescriptor> tools = m_Tools;
        if (m_Dynamic)
        {
            for (auto& tool : m_Dynamic())
            {
                tools.push_back(std::move(tool));
            }
        }
        return tools;
    }

    /// A copy, not a pointer: a package's tool is built on the fly.
    std::optional<McpToolDescriptor> FindTool(const std::string& name) const
    {
        for (const auto& tool : m_Tools)
        {
            if (tool.name == name)
            {
                return tool;
            }
        }
        if (m_Dynamic)
        {
            for (auto& tool : m_Dynamic())
            {
                if (tool.name == name)
                {
                    return tool;
                }
            }
        }
        return std::nullopt;
    }

private:
    std::vector<McpToolDescriptor> m_Tools;
    std::function<std::vector<McpToolDescriptor>()> m_Dynamic;
};

}  // namespace DekiEditor
