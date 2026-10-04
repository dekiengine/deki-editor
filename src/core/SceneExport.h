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

// Saves `obj` (with its subtree) as a standalone .scene file in `folderPath`,
// picking a unique file name from the object's name. Instance markers are
// stripped for the write and restored afterwards, so the object serialises
// as real data rather than as a link to its source scene. Returns the path
// written, or an empty string on failure.
std::string SaveObjectAsScene(Deki::Object* obj, const std::string& folderPath);

}  // namespace SceneExport
}  // namespace DekiEditor
