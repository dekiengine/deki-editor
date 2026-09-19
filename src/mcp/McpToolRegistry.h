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

class McpToolRegistry
{
public:
    void Register(McpToolDescriptor tool)
    {
        m_Tools.push_back(std::move(tool));
    }

    /// Tools that are not known when the registry is built: the ones packages
    /// add (CliTool), which come and go as packages load and unload. Asked
    /// afresh on every listing and lookup.
    void SetDynamicSource(std::function<std::vector<McpToolDescriptor>()> source)
    {
        m_Dynamic = std::move(source);
    }

    std::vector<McpToolDescriptor> GetTools() const
    {
        std::vector<McpToolDescriptor> tools = m_Tools;
        if (m_Dynamic)
            for (auto& tool : m_Dynamic())
                tools.push_back(std::move(tool));
        return tools;
    }

    /// A copy, not a pointer: a package's tool is built on the fly.
    std::optional<McpToolDescriptor> FindTool(const std::string& name) const
    {
        for (const auto& tool : m_Tools)
        {
            if (tool.name == name)
                return tool;
        }
        if (m_Dynamic)
            for (auto& tool : m_Dynamic())
                if (tool.name == name)
                    return tool;
        return std::nullopt;
    }

private:
    std::vector<McpToolDescriptor> m_Tools;
    std::function<std::vector<McpToolDescriptor>()> m_Dynamic;
};

}  // namespace DekiEditor
