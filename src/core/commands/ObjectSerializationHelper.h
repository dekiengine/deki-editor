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

/**
 * Helper structures and functions for serializing/deserializing DekiObjects and components
 * Used by DeleteObjectCommand, DuplicateObjectCommand, RemoveComponentCommand
 */
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
    int originalIndex = -1;  // Index in parent's children or scene's objects
    uint32_t parentId = 0;   // Parent object ID (0 = root object)
    uint32_t objectId = 0;   // Object's own ID (for ObjectRef restoration)
};

struct ChildData
{
    ObjectData objectData;
    std::vector<ComponentData> components;
    std::vector<ChildData> children;
};

/**
 * Helper to serialize properties from a single metadata level
 */

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
