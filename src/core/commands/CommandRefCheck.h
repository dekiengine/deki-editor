#pragma once

// Helpers for Command::ReferencesAny.
//
// Commands address their targets with raw Deki::Object* / Deki::Component* /
// Scene* pointers. When the scene frees objects, CommandHistory asks every
// command whether it points at anything in the dying set and drops the ones
// that do — otherwise a later Undo()/Redo() writes through freed memory.
//
// Each command reports the pointers it stores. Anything a command owns while
// detached (an undone create, an executed delete) is deliberately still
// reported: it is not reachable from the scene, so it will never appear in a
// dying set, and reporting it costs nothing.

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
