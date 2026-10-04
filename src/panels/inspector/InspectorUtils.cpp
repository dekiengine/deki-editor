#include "InspectorUtils.h"
#include <deki/reflection/Property.h>
#include <deki/Component.h>
#include <deki/reflection/ComponentRegistry.h>
#include "../../core/commands/PropertyTypeHandlers.h"
#include <deki/Object.h>
#include <deki/Scene.h>

#include <unordered_map>
#include <cctype>
#include <cstring>

namespace DekiEditor
{

namespace InspectorUtils
{

bool ComponentIsOrDerivesFrom(Deki::Component* comp, const char* targetTypeName)
{
    if (!comp || !targetTypeName)
    {
        return false;
    }

    auto& registry = Deki::ComponentRegistry::Instance();
    uint32_t typeId = comp->GetType();

    // Walk up the inheritance chain
    while (typeId != 0)
    {
        const Deki::ComponentMeta* meta = registry.GetMeta(typeId);
        if (!meta)
        {
            break;
        }

        if (strcmp(meta->name, targetTypeName) == 0)
        {
            return true;
        }

        typeId = meta->baseTypeId;
    }

    return false;
}

bool EvaluateVisibility(Deki::Component* comp, const Deki::PropertyInfo& prop, const Deki::ComponentMeta* meta)
{
    // If no visibility condition, always visible
    if (prop.visibilityProperty == nullptr)
    {
        return true;
    }

    if (!meta || !meta->properties || meta->propertyCount == 0)
    {
        return true;
    }

    // Find the controlling property
    for (int j = 0; j < meta->propertyCount; ++j)
    {
        const Deki::PropertyInfo& ctrlProp = meta->properties[j];
        if (ctrlProp.name && strcmp(ctrlProp.name, prop.visibilityProperty) == 0)
        {
            // Read the controlling property value based on its size
            int ctrlValue = 0;
            void* ctrlPtr = reinterpret_cast<char*>(comp) + ctrlProp.offset;

            if (ctrlProp.type == Deki::PropertyType::Enum)
            {
                uint8_t enumSize = ctrlProp.enumSize > 0 ? ctrlProp.enumSize : 1;
                switch (enumSize)
                {
                    case 1: ctrlValue = static_cast<int>(*reinterpret_cast<uint8_t*>(ctrlPtr)); break;
                    case 2: ctrlValue = static_cast<int>(*reinterpret_cast<uint16_t*>(ctrlPtr)); break;
                    default: ctrlValue = *reinterpret_cast<int32_t*>(ctrlPtr); break;
                }
            }
            else if (ctrlProp.type == Deki::PropertyType::Int32)
            {
                ctrlValue = *reinterpret_cast<int32_t*>(ctrlPtr);
            }
            else if (ctrlProp.type == Deki::PropertyType::Bool)
            {
                ctrlValue = *reinterpret_cast<bool*>(ctrlPtr) ? 1 : 0;
            }

            // Check visibility condition
            if (prop.visibilityMask != 0)
            {
                // Bitmask mode: visible when bit is set for the current value
                if (ctrlValue >= 0 && ctrlValue < 32)
                {
                    return (prop.visibilityMask & (1u << ctrlValue)) != 0;
                }
                return false;
            }
            // Threshold mode: visible when value >= minValue
            return (ctrlValue >= prop.visibilityMinValue);
        }
    }

    // Controlling property not found, default to visible
    return true;
}

namespace
{
// (objectId, component index on that object, property offset) -> value on disk.
struct SavedKey
{
    uint32_t objectId;
    uint32_t componentIndex;
    size_t offset;
    bool operator==(const SavedKey& o) const
    {
        return objectId == o.objectId && componentIndex == o.componentIndex && offset == o.offset;
    }
};
struct SavedKeyHash
{
    size_t operator()(const SavedKey& k) const
    {
        size_t h = std::hash<uint32_t>()(k.objectId);
        h = h * 31 + std::hash<uint32_t>()(k.componentIndex);
        h = h * 31 + std::hash<size_t>()(k.offset);
        return h;
    }
};
std::unordered_map<SavedKey, nlohmann::json, SavedKeyHash> g_SavedState;

// Same capture the undo system uses, so "unsaved" means exactly what "this
// edit changed something" means, arrays and sized enums included.
nlohmann::json CaptureValue(const Deki::Component* comp, const Deki::PropertyInfo& prop)
{
    if (prop.type == Deki::PropertyType::Array)
    {
        return CaptureArray(comp, prop);
    }
    if (prop.type == Deki::PropertyType::Enum)
    {
        return CaptureEnumSized(comp, prop.offset, prop.enumSize);
    }
    return GetPropertyTypeHandler(prop.type).capture(comp, prop.offset);
}

// The component's position among its object's components - the half of the key
// that a pointer would otherwise have to provide.
bool FindComponentIndex(const Deki::Component* comp, uint32_t* outObjectId, uint32_t* outIndex)
{
    const Deki::Object* owner = comp ? comp->GetOwner() : nullptr;
    if (!owner)
    {
        return false;
    }
    const auto& comps = owner->GetComponents();
    for (size_t i = 0; i < comps.size(); ++i)
    {
        if (comps[i] == comp)
        {
            *outObjectId = owner->GetId();
            *outIndex = static_cast<uint32_t>(i);
            return true;
        }
    }
    return false;
}
}  // namespace

namespace
{
uint64_t g_SavedStateEpoch = 1;
}

uint64_t SavedStateEpoch()
{
    return g_SavedStateEpoch;
}

void ClearSavedState()
{
    ++g_SavedStateEpoch;
    g_SavedState.clear();
}

void CaptureSavedState(::Deki::Scene* scene)
{
    ++g_SavedStateEpoch;
    g_SavedState.clear();
    if (!scene)
    {
        return;
    }

    auto& registry = Deki::ComponentRegistry::Instance();
    for (Deki::Object* obj : scene->GetObjects())
    {
        if (!obj)
        {
            continue;
        }
        const auto& comps = obj->GetComponents();
        for (size_t ci = 0; ci < comps.size(); ++ci)
        {
            Deki::Component* comp = comps[ci];
            if (!comp)
            {
                continue;
            }
            // Own properties and every inherited one, matching what the
            // inspector draws.
            const Deki::ComponentMeta* walk = registry.GetMeta(comp->GetType());
            while (walk)
            {
                for (int i = 0; i < walk->propertyCount; ++i)
                {
                    const Deki::PropertyInfo& p = walk->properties[i];
                    g_SavedState[SavedKey{ obj->GetId(), static_cast<uint32_t>(ci), p.offset }] = CaptureValue(comp, p);
                }
                walk = walk->baseTypeId != 0 ? registry.GetMeta(walk->baseTypeId) : nullptr;
            }
        }
    }
}

const nlohmann::json* SavedValueFor(const Deki::Component* comp, const Deki::PropertyInfo& prop)
{
    uint32_t objectId = 0, index = 0;
    if (!FindComponentIndex(comp, &objectId, &index))
    {
        return nullptr;  // a component with no owner was never saved
    }
    auto it = g_SavedState.find(SavedKey{ objectId, index, prop.offset });
    return it == g_SavedState.end() ? nullptr : &it->second;
}

bool ComponentNameMatchesFilter(const Deki::ComponentMeta* meta)
{
    if (!PropertyFilterActive())
    {
        return false;
    }
    if (!meta)
    {
        return false;
    }
    // Both spellings, as the Add Component picker matches them: the label you
    // read on the header, and the type name you would have written in code.
    return TextMatchesFilter(meta->GetDisplayName()) || TextMatchesFilter(meta->name);
}

bool ComponentHasFilterMatch(const Deki::ComponentMeta* meta)
{
    if (!PropertyFilterActive())
    {
        return true;
    }
    if (!meta)
    {
        return false;
    }
    if (ComponentNameMatchesFilter(meta))
    {
        return true;
    }

    // Own properties, then inherited ones: a filter that hides a component
    // because the match lives on its base class would be worse than no filter.
    auto& registry = Deki::ComponentRegistry::Instance();
    const Deki::ComponentMeta* walk = meta;
    while (walk)
    {
        for (int i = 0; i < walk->propertyCount; ++i)
        {
            if (PropertyMatchesFilter(GetDisplayName(walk->properties[i]).c_str()))
            {
                return true;
            }
        }
        walk = walk->baseTypeId != 0 ? registry.GetMeta(walk->baseTypeId) : nullptr;
    }
    return false;
}

}  // namespace InspectorUtils

}  // namespace DekiEditor

namespace DekiEditor
{
namespace InspectorUtils
{

std::string GetMatchingComponentName(Deki::Object* obj, const char* targetTypeName)
{
    if (!obj || !targetTypeName)
    {
        return "";
    }
    auto& registry = Deki::ComponentRegistry::Instance();
    std::string result;
    for (auto* comp : obj->GetComponents())
    {
        if (ComponentIsOrDerivesFrom(comp, targetTypeName))
        {
            const auto* meta = registry.GetMeta(comp->GetType());
            if (meta && meta->name)
            {
                if (!result.empty())
                {
                    result += ", ";
                }
                result += meta->name;
            }
        }
    }
    return result;
}

}  // namespace InspectorUtils
}  // namespace DekiEditor
