#pragma once

#include <memory>
#include <deki/Scene.h>

namespace DekiEditor
{

// The editor's invariant is that a scene is always available to edit — there is
// no usable "no scene" state. This helper backs that guarantee: if `scene` is
// null it installs a fresh empty scene, and it always returns the (now non-null)
// pointer. EditorApp::GetCurrentScene() routes through it so the rest of the
// editor never has to null-check the current scene.
inline ::Deki::Scene* EnsureScene(std::unique_ptr<::Deki::Scene>& scene)
{
    if (!scene)
        scene = std::make_unique<::Deki::Scene>();
    return scene.get();
}

}  // namespace DekiEditor
