#pragma once

// How the MCP tools report and change an object's transform.
//
// Depth (z, the tilt about X and Y, the Z scale) belongs to a project that
// declares transform_3d, as deki-3d does. The editor always has the fields,
// but a 2D project's game does not, so there the tools neither show them nor
// take them: a depth set there would be lost on export.

#include "../EditorApp.h"
#include <deki/Math.h>
#include <deki/Object.h>
#include <nlohmann/json.hpp>

#include <initializer_list>
#include <stdexcept>
#include <string>

namespace DekiEditor
{

/// x, y, rotation (degrees), scaleX and scaleY into `j`; and z, rotationX,
/// rotationY (degrees) and scaleZ when `depth`.
inline void WriteTransform(nlohmann::json& j, const Deki::Object* obj, bool depth)
{
    j["x"] = obj->GetX();
    j["y"] = obj->GetY();
    // MCP reports rotation in degrees; it is stored in radians.
    j["rotation"] = obj->GetLocalRotation() * Deki::Math::kRadToDeg;
    j["scaleX"] = obj->GetScaleX();
    j["scaleY"] = obj->GetScaleY();
    if (depth)
    {
        j["z"] = obj->GetZ();
        j["rotationX"] = obj->GetRotationX() * Deki::Math::kRadToDeg;
        j["rotationY"] = obj->GetRotationY() * Deki::Math::kRadToDeg;
        j["scaleZ"] = obj->GetScaleZ();
    }
}

/// Throws, saying why, when `args` gives any of `depthKeys` and the open
/// project has no depth.
inline void RequireDepthFor(const EditorApp* editor, const nlohmann::json& args,
                            std::initializer_list<const char*> depthKeys)
{
    if (editor->ProjectDeclaresDepth())
    {
        return;
    }
    for (const char* key : depthKeys)
    {
        if (args.contains(key))
        {
            throw std::runtime_error(std::string("'") + key +
                                     "' is depth, and this project has a 2D transform: no package it uses declares "
                                     "transform_3d. Add deki-3d (or another package that does) to give its objects "
                                     "depth.");
        }
    }
}

}  // namespace DekiEditor
