#pragma once

#include <deki/Object.h>
#include <deki/Component.h>
#include <deki/Scene.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/Property.h>
#include <deki/assets/AssetRef.h>
#include "PropertyTypeHandlers.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>

namespace DekiEditor
{

/// Snapshots of objects and components as plain data, and the functions that
/// take and restore them. Used by the paste, instance and component commands.
namespace ObjectSerializationHelper
{

struct ComponentData
{
    std::string typeName;
    nlohmann::json properties;
};

struct ObjectData
{
    std::string name;
    float x, y;
    float rotation;
    float scaleX, scaleY;
    bool active;
    int originalIndex = -1;  // index among the parent's children, or the scene's objects
    uint32_t parentId = 0;   // 0 = root object
    uint32_t objectId = 0;   // kept so ObjectRefs to it still resolve
};

struct ChildData
{
    ObjectData objectData;
    std::vector<ComponentData> components;
    std::vector<ChildData> children;
};

/// Writes the properties declared at one level of the class hierarchy into
/// `properties`.
void SerializePropertiesFromMeta(const Deki::Component* comp, const Deki::ComponentMeta* meta,
                                 nlohmann::json& properties);

ComponentData SerializeComponent(const Deki::Component* comp);

void RestorePropertiesFromMeta(Deki::Component* comp, const Deki::ComponentMeta* meta,
                               const nlohmann::json& properties);

void RestoreComponentProperties(Deki::Component* comp, const ComponentData& data);

ObjectData SerializeObjectData(const Deki::Object* obj);

void ApplyObjectData(Deki::Object* obj, const ObjectData& data);

std::vector<ComponentData> SerializeComponents(const Deki::Object* obj);

void RestoreComponents(Deki::Object* obj, const std::vector<ComponentData>& components);

ChildData SerializeChildRecursive(const Deki::Object* obj);

std::vector<ChildData> SerializeChildren(const Deki::Object* obj);

void CreateChildrenRecursive(Deki::Object* parent, const std::vector<ChildData>& children);

}  // namespace ObjectSerializationHelper

}  // namespace DekiEditor
