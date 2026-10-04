#include <deki-editor/EditorComponents.h>

#include "ObjectInternal.h"

namespace DekiEditor
{

Deki::Component* AddComponentByName(Deki::Object* object, const std::string& typeName)
{
    if (!object)
    {
        return nullptr;
    }
    return Deki::Internal::AddComponentByName(*object, typeName);
}

}  // namespace DekiEditor
