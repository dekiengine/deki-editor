#pragma once

// Helpers for Command::ReferencesAny.
//
// Commands hold raw Deki::Object* / Deki::Component* / Scene* pointers. When
// the scene frees objects, CommandHistory asks every command whether it
// points into the dying memory and drops the ones that do, so a later
// Undo()/Redo() never writes through freed memory.
//
// Each command reports every pointer it stores, including ones it owns while
// detached (an undone create, an executed delete). Those are not reachable
// from the scene, so they never show up as dying, and reporting them is free.

// Everything here works on void*, so no engine types need to be visible.
#include <deki-editor/Command.h>

namespace DekiEditor
{
namespace CommandRefs
{

inline bool Has(const DeadMemory& dead, const void* ptr)
{
    return dead.Contains(ptr);
}

/// True if any of the given pointers falls in dying memory.
template <typename... Ptrs>
inline bool HasAny(const DeadMemory& dead, Ptrs... ptrs)
{
    return (dead.Contains(static_cast<const void*>(ptrs)) || ...);
}

/// True if any element of the container falls in dying memory.
template <typename Container>
inline bool HasAnyIn(const DeadMemory& dead, const Container& items)
{
    return dead.ContainsAnyIn(items);
}

}  // namespace CommandRefs
}  // namespace DekiEditor
