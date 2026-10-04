#pragma once

#include <memory>
#include <deki/Scene.h>

namespace DekiEditor
{

/// The editor always has a scene to edit. If `scene` is null this puts a new
/// empty scene in it; it always returns a non-null pointer.
/// EditorApp::GetCurrentScene() goes through this, so the rest of the editor
/// never null-checks the current scene.
inline ::Deki::Scene* EnsureScene(std::unique_ptr<::Deki::Scene>& scene)
{
    if (!scene)
    {
        scene = std::make_unique<::Deki::Scene>();
    }
    return scene.get();
}

}  // namespace DekiEditor
