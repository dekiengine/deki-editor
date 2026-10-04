#pragma once

#include <string>
#include <cstdint>
#include <variant>
#include <array>

// Forward declarations - engine types
namespace Deki
{
class Object;
}
namespace Deki
{
class Component;
}
namespace Deki
{
struct PropertyInfo;
}
namespace Deki
{
struct ComponentMeta;
}

namespace DekiEditor
{

// Forward declarations - editor types
class EditorApp;
class AssetPicker;

// For undo: the property being edited and its value when the edit started.
struct PropertyEditState
{
    Deki::Component* component = nullptr;
    size_t propertyOffset = 0;
    std::variant<int32_t, float, bool, std::string, uint32_t, std::array<float, 2>> oldValue;
    bool isEditing = false;
};

}  // namespace DekiEditor
