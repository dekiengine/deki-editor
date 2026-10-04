#pragma once

#include <deki-editor/EditorExport.h>

#include <string>

#include <nlohmann/json.hpp>

namespace DekiEditor
{

/// Builds a simulated build's boot scene: the board's, plus the simulator's
/// steps. For ITargetBuilder::MakeSimulationBootScene.
///
/// `steps` is an array of {"name", "type", "properties"} (the qualified
/// component type and its property values). Each becomes a child of the
/// object holding the Deki::PlatformSetupComponent, with an id unique in the
/// scene, listed after the board's own setup steps so it runs last: a
/// simulator's display is installed after the board's and is the one the game
/// renders to. Every other object and step is kept as it was.
///
/// `scene` may be null (a platform with no boot scene of its own); it then
/// becomes a boot scene with only a PlatformSetupComponent and the steps,
/// which boots the project's startup scene as the device would.
///
/// False, with `error` set, when `steps` is not an array of such objects or
/// the scene has no objects array.
DEKI_EDITOR_API bool AddSimulationSteps(nlohmann::json& scene, const nlohmann::json& steps, std::string& error);

}  // namespace DekiEditor
