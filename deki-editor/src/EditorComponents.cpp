#include <deki-editor/EditorComponents.h>

#include "ObjectInternal.h"

namespace DekiEditor
{

Deki::Component* AddComponentByName(Deki::Object* object, const std::string& typeName)
{
    if (!object) return nullptr;
    return Deki::detail::AddComponentByName(*object, typeName);
}

}  // namespace DekiEditor
