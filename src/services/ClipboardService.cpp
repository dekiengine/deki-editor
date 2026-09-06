#include "ClipboardService.h"
#include <deki/Component.h>
#include <deki/Object.h>
#include "imgui.h"

namespace DekiEditor
{

ClipboardService& ClipboardService::Instance()
{
    static ClipboardService instance;
    return instance;
}

void ClipboardService::WriteToClipboard(const nlohmann::json& data)
{
    ImGui::SetClipboardText(data.dump().c_str());
}

std::optional<nlohmann::json> ClipboardService::ReadFromClipboard() const
{
    const char* text = ImGui::GetClipboardText();
    if (!text || text[0] == '\0')
        return std::nullopt;

    // Clipboard usually holds arbitrary non-JSON text; parse without exceptions so a
    // failed parse returns a discarded value instead of throwing (which halts the debugger).
    nlohmann::json parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded())
        return std::nullopt;

    return parsed;
}

std::optional<ClipboardService::ParsedClipboard> ClipboardService::ParseClipboard() const
{
    auto jsonOpt = ReadFromClipboard();
    if (!jsonOpt.has_value())
        return std::nullopt;

    const auto& json = jsonOpt.value();

    if (!json.contains("deki_clipboard_type") || !json.contains("version"))
        return std::nullopt;

    if (json["version"].get<int>() != CLIPBOARD_VERSION)
        return std::nullopt;

    ParsedClipboard data;
    std::string typeStr = json["deki_clipboard_type"].get<std::string>();

    if (typeStr == TYPE_COMPONENT)
    {
        data.type = ClipboardContentType::Component;
        data.componentType = json.value("component_type", "");
        data.properties = json.value("properties", nlohmann::json::object());
    }
    else if (typeStr == TYPE_TRANSFORM)
    {
        data.type = ClipboardContentType::Transform;
        if (json.contains("transform"))
        {
            const auto& t = json["transform"];
            data.transformData.name = "";
            data.transformData.x = t.value("x", 0.0f);
            data.transformData.y = t.value("y", 0.0f);
            data.transformData.rotation = t.value("rotation", 0.0f);
            data.transformData.scaleX = t.value("scaleX", 1.0f);
            data.transformData.scaleY = t.value("scaleY", 1.0f);
            data.transformData.active = t.value("active", true);
        }
    }
    else if (typeStr == TYPE_OBJECTS)
    {
        data.type = ClipboardContentType::Objects;
        if (json.contains("objects") && json["objects"].is_array())
        {
            for (const auto& objJson : json["objects"])
            {
                data.objectsData.push_back(JsonToChildData(objJson));
            }
        }
    }
    else
    {
        return std::nullopt;
    }

    return data;
}

nlohmann::json ClipboardService::ChildDataToJson(const ChildData& data)
{
    nlohmann::json obj;
    obj["name"] = data.objectData.name;
    obj["x"] = data.objectData.x;
    obj["y"] = data.objectData.y;
    obj["rotation"] = data.objectData.rotation;
    obj["scaleX"] = data.objectData.scaleX;
    obj["scaleY"] = data.objectData.scaleY;
    obj["active"] = data.objectData.active;

    nlohmann::json compsJson = nlohmann::json::array();
    for (const auto& comp : data.components)
    {
        nlohmann::json cj;
        cj["typeName"] = comp.typeName;
        cj["properties"] = comp.properties;
        compsJson.push_back(cj);
    }
    obj["components"] = compsJson;

    nlohmann::json childrenJson = nlohmann::json::array();
    for (const auto& child : data.children)
    {
        childrenJson.push_back(ChildDataToJson(child));
    }
    obj["children"] = childrenJson;

    return obj;
}

ChildData ClipboardService::JsonToChildData(const nlohmann::json& json)
{
    ChildData data;
    data.objectData.name = json.value("name", "Object");
    data.objectData.x = json.value("x", 0.0f);
    data.objectData.y = json.value("y", 0.0f);
    data.objectData.rotation = json.value("rotation", 0.0f);
    data.objectData.scaleX = json.value("scaleX", 1.0f);
    data.objectData.scaleY = json.value("scaleY", 1.0f);
    data.objectData.active = json.value("active", true);

    if (json.contains("components") && json["components"].is_array())
    {
        for (const auto& cj : json["components"])
        {
            ComponentData cd;
            cd.typeName = cj.value("typeName", "");
            cd.properties = cj.value("properties", nlohmann::json::object());
            data.components.push_back(cd);
        }
    }

    if (json.contains("children") && json["children"].is_array())
    {
        for (const auto& childJson : json["children"])
        {
            data.children.push_back(JsonToChildData(childJson));
        }
    }

    return data;
}

void ClipboardService::CopyObjects(const std::vector<Deki::Object*>& objects)
{
    nlohmann::json json;
    json["deki_clipboard_type"] = TYPE_OBJECTS;
    json["version"] = CLIPBOARD_VERSION;

    nlohmann::json objectsArray = nlohmann::json::array();
    for (const auto* obj : objects)
    {
        ChildData childData = ObjectSerializationHelper::SerializeChildRecursive(obj);
        objectsArray.push_back(ChildDataToJson(childData));
    }
    json["objects"] = objectsArray;

    WriteToClipboard(json);
}

void ClipboardService::CopyComponent(const Deki::Component* comp)
{
    ComponentData cd = ObjectSerializationHelper::SerializeComponent(comp);

    nlohmann::json json;
    json["deki_clipboard_type"] = TYPE_COMPONENT;
    json["version"] = CLIPBOARD_VERSION;
    json["component_type"] = cd.typeName;
    json["properties"] = cd.properties;

    WriteToClipboard(json);
}

void ClipboardService::CopyTransform(const Deki::Object* obj)
{
    ObjectData od = ObjectSerializationHelper::SerializeObjectData(obj);

    nlohmann::json json;
    json["deki_clipboard_type"] = TYPE_TRANSFORM;
    json["version"] = CLIPBOARD_VERSION;
    json["transform"]["x"] = od.x;
    json["transform"]["y"] = od.y;
    json["transform"]["rotation"] = od.rotation;
    json["transform"]["scaleX"] = od.scaleX;
    json["transform"]["scaleY"] = od.scaleY;
    json["transform"]["active"] = od.active;

    WriteToClipboard(json);
}

bool ClipboardService::HasComponent() const
{
    auto data = ParseClipboard();
    return data.has_value() && data->type == ClipboardContentType::Component;
}

bool ClipboardService::HasComponentOfType(const std::string& typeName) const
{
    auto data = ParseClipboard();
    if (!data.has_value())
        return false;
    if (data->type != ClipboardContentType::Component)
        return false;
    return data->componentType == typeName;
}

bool ClipboardService::HasTransform() const
{
    auto data = ParseClipboard();
    return data.has_value() && data->type == ClipboardContentType::Transform;
}

std::string ClipboardService::GetClipboardComponentType() const
{
    auto data = ParseClipboard();
    if (!data.has_value())
        return "";
    return data->componentType;
}

std::optional<ComponentData> ClipboardService::GetComponentData() const
{
    auto data = ParseClipboard();
    if (!data.has_value())
        return std::nullopt;
    if (data->type != ClipboardContentType::Component)
        return std::nullopt;

    ComponentData cd;
    cd.typeName = data->componentType;
    cd.properties = data->properties;
    return cd;
}

std::optional<ObjectData> ClipboardService::GetTransformData() const
{
    auto data = ParseClipboard();
    if (!data.has_value() || data->type != ClipboardContentType::Transform)
        return std::nullopt;
    return data->transformData;
}

bool ClipboardService::HasObjects() const
{
    auto data = ParseClipboard();
    return data.has_value() && data->type == ClipboardContentType::Objects && !data->objectsData.empty();
}

std::optional<std::vector<ChildData>> ClipboardService::GetObjectsData() const
{
    auto data = ParseClipboard();
    if (!data.has_value() || data->type != ClipboardContentType::Objects)
        return std::nullopt;
    return data->objectsData;
}

}  // namespace DekiEditor
