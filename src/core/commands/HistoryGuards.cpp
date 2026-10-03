#include "HistoryGuards.h"

#include <deki-editor/CommandHistory.h>
#include <deki/Component.h>
#include <deki/Object.h>
#include <deki/reflection/ComponentRegistry.h>

namespace DekiEditor
{
namespace HistoryGuards
{

namespace
{
std::vector<const void*>& OwnerStack()
{
    static std::vector<const void*> s_Owners;
    return s_Owners;
}
}  // namespace

void AddComponent(DeadMemory& dead, const Deki::Component* comp)
{
    if (!comp)
        return;
    // instanceSize comes from the reflection metadata; when a component's type
    // isn't registered only the base subobject is covered, which still catches
    // a step holding the component pointer itself.
    const Deki::ComponentMeta* meta = Deki::ComponentRegistry::Instance().GetMeta(comp->GetType());
    const size_t size = (meta && meta->instanceSize > 0) ? meta->instanceSize : sizeof(Deki::Component);
    dead.Add(comp, size);
}

void AddObject(DeadMemory& dead, const Deki::Object* obj, bool recursive)
{
    if (!obj)
        return;
    dead.Add(obj, sizeof(Deki::Object));
    for (const Deki::Component* comp : obj->GetComponents())
        AddComponent(dead, comp);
    if (recursive)
        for (const Deki::Object* child : obj->GetChildren())
            AddObject(dead, child, true);
}

void ForgetObjectTree(const Deki::Object* obj)
{
    DeadMemory dead;
    AddObject(dead, obj, true);
    CommandHistory::Instance().DiscardCommandsReferencing(dead);
}

void ForgetComponent(const Deki::Component* comp)
{
    DeadMemory dead;
    AddComponent(dead, comp);
    CommandHistory::Instance().DiscardCommandsReferencing(dead);
    // And what the scene loader recorded for it, keyed by its address.
    Deki::Internal::ForgetComponentData(comp);
}

EditOwnerScope::EditOwnerScope(const void* owner)
{
    if (owner)
    {
        OwnerStack().push_back(owner);
        m_Pushed = true;
    }
}

EditOwnerScope::~EditOwnerScope()
{
    if (m_Pushed && !OwnerStack().empty())
        OwnerStack().pop_back();
}

const std::vector<const void*>& CurrentEditOwners()
{
    return OwnerStack();
}

}  // namespace HistoryGuards
}  // namespace DekiEditor
