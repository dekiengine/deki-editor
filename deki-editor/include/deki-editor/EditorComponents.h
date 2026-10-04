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

/// Creates a component by its qualified class name and attaches it to
/// `object`. Returns the new component, or nullptr when the class is not
/// registered.
///
/// For package editor code that builds objects for the user, such as a custom
/// editor adding the children a component needs. It is the path the editor's
/// own commands and panels use. Scripts use `obj->AddComponent<T>()` instead.
DEKI_EDITOR_API Deki::Component* AddComponentByName(Deki::Object* object, const std::string& typeName);

}  // namespace DekiEditor
