#pragma once

// Keeping the undo history from pointing at freed memory.
//
// Undo steps hold raw Object* / Component* pointers (and, for field edits,
// lambdas that capture them). Scene::DestroyDekiObject announces what it frees
// and EditorApp drops the steps that point there; this header covers the rest:
//
//  - code that frees objects or components some other way (scene instances
//    collapsing and re-expanding) calls Forget* first;
//  - a field edit that applies through callbacks records the object or
//    component it was made on (EditOwnerScope), so it can be found too.

#include <deki-editor/Command.h>

#include <vector>

namespace Deki
{
class Object;
class Component;
}  // namespace Deki

namespace DekiEditor
{
namespace HistoryGuards
{

/// Adds the memory of `obj` and its components to `dead`; with `recursive`,
/// of its whole subtree.
void AddObject(DeadMemory& dead, const Deki::Object* obj, bool recursive);

/// Adds the memory of one component to `dead`: its full instance size when
/// its type is registered.
void AddComponent(DeadMemory& dead, const Deki::Component* comp);

/// Drops the undo steps that point into `obj`'s subtree / `comp`, which the
/// caller is about to free outside Scene::DestroyDekiObject. ForgetComponent
/// also drops the scene loader's record of it (Internal::ForgetComponentData).
void ForgetObjectTree(const Deki::Object* obj);
void ForgetComponent(const Deki::Component* comp);

/// While alive, field edits made through the EditorUI helpers record `owner`
/// (the object or component whose fields are being drawn) so the history can
/// drop them when it dies. Scopes nest; all open owners are recorded.
class EditOwnerScope
{
public:
    explicit EditOwnerScope(const void* owner);
    ~EditOwnerScope();
    EditOwnerScope(const EditOwnerScope&) = delete;
    EditOwnerScope& operator=(const EditOwnerScope&) = delete;

private:
    bool m_Pushed = false;
};

/// The owners of the edits being made now (innermost last).
const std::vector<const void*>& CurrentEditOwners();

}  // namespace HistoryGuards
}  // namespace DekiEditor
