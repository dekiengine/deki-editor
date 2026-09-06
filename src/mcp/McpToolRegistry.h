#pragma once

#include <string>
#include <vector>
#include <functional>
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

    const std::vector<McpToolDescriptor>& GetTools() const { return m_Tools; }

    const McpToolDescriptor* FindTool(const std::string& name) const
    {
        for (auto& tool : m_Tools)
        {
            if (tool.name == name)
                return &tool;
        }
        return nullptr;
    }

private:
    std::vector<McpToolDescriptor> m_Tools;
};

}  // namespace DekiEditor
