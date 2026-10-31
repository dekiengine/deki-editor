#pragma once

#include <string>
#include <vector>

namespace DekiEditor
{

/// The scenes a .scene file instances: the GUID in each object's "instance"
/// key, sorted, each once. A GUID anywhere else in the file (a SceneLink's
/// target, a sprite, a font) only refers to that asset, so it is left out.
std::vector<std::string> InstancedSceneGuids(const std::string& sceneText);

}  // namespace DekiEditor
