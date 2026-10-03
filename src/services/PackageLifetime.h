#pragma once

#include <functional>

namespace DekiEditor
{

// Things the editor builds from package code for its own use - the Scene
// View's camera, a scene preview's camera - and must drop while that code is
// still loaded. A component's vtable and destructor live in its package, so
// one kept across an unload (a project switch, a hot reload, installing or
// removing a package) is called into after its library is gone.
//
// An owner registers a release callback once; the package loader runs every
// callback before it frees any package library. Owners build again lazily.
namespace PackageLifetime
{
void OnBeforeUnload(const void* owner, std::function<void()> release);
void Forget(const void* owner);
void ReleaseAll();
}  // namespace PackageLifetime

}  // namespace DekiEditor
