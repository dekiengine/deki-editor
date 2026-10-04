#pragma once

#include <string>
#include <optional>
#include <nlohmann/json.hpp>
#include "../core/commands/ObjectSerializationHelper.h"

namespace Deki
{
class Component;
}
namespace Deki
{
class Object;
}

namespace DekiEditor
{

// Named imports rather than `using namespace`: a wholesale import in a header
// is open-ended, so anything later added to ObjectSerializationHelper would
// silently appear in every DekiEditor TU that includes this.
using ObjectSerializationHelper::ChildData;
using ObjectSerializationHelper::ComponentData;
using ObjectSerializationHelper::ObjectData;

enum class ClipboardContentType
{
    None,
    Component,
    Transform,
    Objects
};

class ClipboardService
{
public:
    static ClipboardService& Instance();

    // Copy operations
    void CopyComponent(const Deki::Component* comp);
    void CopyTransform(const Deki::Object* obj);
    void CopyObjects(const std::vector<Deki::Object*>& objects);

    // Query operations
    bool HasComponent() const;
    bool HasComponentOfType(const std::string& typeName) const;
    bool HasTransform() const;
    bool HasObjects() const;
    std::string GetClipboardComponentType() const;

    // Retrieve operations
    std::optional<ComponentData> GetComponentData() const;
    std::optional<ObjectData> GetTransformData() const;
    std::optional<std::vector<ChildData>> GetObjectsData() const;

private:
    ClipboardService() = default;

    void WriteToClipboard(const nlohmann::json& data);
    std::optional<nlohmann::json> ReadFromClipboard() const;

    struct ParsedClipboard
    {
        ClipboardContentType type = ClipboardContentType::None;
        std::string componentType;
        nlohmann::json properties;
        ObjectData transformData;
        std::vector<ChildData> objectsData;
    };

    static nlohmann::json ChildDataToJson(const ChildData& data);
    static ChildData JsonToChildData(const nlohmann::json& json);

    std::optional<ParsedClipboard> ParseClipboard() const;

    static constexpr int kClipboardVersion = 1;
    static constexpr const char* kTypeComponent = "component";
    static constexpr const char* kTypeTransform = "transform";
    static constexpr const char* kTypeObjects = "objects";
};

}  // namespace DekiEditor
