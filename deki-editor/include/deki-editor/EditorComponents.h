#pragma once

#include <deki-editor/EditorExport.h>

#include <string>

namespace Deki
{
class Object;
class Component;
}  // namespace Deki

namespace DekiEditor
{

/**
 * @brief Create and attach a component by its qualified class name, for
 *        package editor code that builds objects on the user's behalf (a
 *        custom editor adding the children a component needs).
 *
 * Scripts never need this: a script adds a component with
 * `obj->AddComponent<T>()`. This is the editor's own path, the one its
 * commands and panels use, exported so package editor code can reach it.
 * @return The new component, or nullptr when the class is not registered.
 */
DEKI_EDITOR_API Deki::Component* AddComponentByName(Deki::Object* object, const std::string& typeName);

}  // namespace DekiEditor
