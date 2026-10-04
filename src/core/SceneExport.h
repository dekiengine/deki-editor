#pragma once

#include <string>

namespace Deki
{
class Object;
}

namespace DekiEditor
{
namespace SceneExport
{

/// Saves `obj` and its children as a standalone .scene file in `folderPath`,
/// named after the object with a number added if the name is taken. Instance
/// markers are cleared for the write and restored afterwards, so the object is
/// saved as real data rather than as a link to its source scene. Returns the
/// path written, or an empty string on failure.
std::string SaveObjectAsScene(Deki::Object* obj, const std::string& folderPath);

}  // namespace SceneExport
}  // namespace DekiEditor
