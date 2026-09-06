#pragma once

#include <string>
#include <cstdint>
#include <variant>
#include <array>

// Forward declarations - engine types
namespace Deki { class Object; }
namespace Deki { class Component; }
namespace Deki { struct PropertyInfo; }
namespace Deki { struct ComponentMeta; }

namespace DekiEditor
{

// Forward declarations - editor types
class EditorApp;
class AssetPicker;

// Undo tracking state for property editing
// Stores the value when editing started
struct PropertyEditState
{
    Deki::Component* component = nullptr;
    size_t propertyOffset = 0;
    std::variant<int32_t, float, bool, std::string, uint32_t, std::array<float, 2>> oldValue;
    bool isEditing = false;
};

}  // namespace DekiEditor
