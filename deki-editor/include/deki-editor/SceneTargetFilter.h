#pragma once

#include <deki-editor/EditorExport.h>

// Which objects of a scene exist on a given target.
//
// One project serves several targets, and they all share a scene. An object
// that only makes sense on some (a WiFi setup, a desktop-only debug overlay)
// says what it needs from the platform, in the same words a package uses:
//
//     { "name": "WiFi", "requires": ["wifi"], "components": [ ... ] }
//
// If the target offers that capability, the object is in its build; otherwise
// it is left out.
//
// This happens at build time, like feature stripping: the filter runs over the
// scene JSON before it is compiled, so the runtime never sees targets and older
// engines can still read the scene format. An object with no "requires" is on
// every target.

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace DekiEditor
{

/// What `obj` needs that `capabilities` does not offer. Empty means it belongs
/// on this target. An empty capability set keeps everything, which is what a
/// caller with no target (the editor) wants.
DEKI_EDITOR_API std::vector<std::string> ObjectMissingCapabilities(const nlohmann::json& obj,
                                                                   const std::set<std::string>& capabilities);

/// `scene` with every object the target cannot satisfy removed. Removing a
/// parent removes its children too.
DEKI_EDITOR_API nlohmann::json FilterSceneForTarget(const nlohmann::json& scene,
                                                    const std::set<std::string>& capabilities);

/// True when the filter would change anything, so a caller can skip the copy.
DEKI_EDITOR_API bool SceneHasTargetRequirements(const nlohmann::json& scene);

/// Copies every object's "requires" from `before` (the file as it was) into
/// the file the editor has just rewritten, matching objects by id. Returns
/// true if anything was re-applied.
///
/// The engine's Object does not hold "requires", since it is a build-time key
/// the runtime never sees. Saving a scene rebuilds the JSON from objects, so
/// without this step the key would be lost without warning. Doing it after the
/// save keeps one file and costs no engine ABI. An object deleted meanwhile is
/// not found, so its entry is dropped.
DEKI_EDITOR_API bool PreserveTargetRequirements(const std::string& scenePath, const nlohmann::json& before);

/// The "requires" of every object in `scene`, keyed by object id. Empty when
/// the scene has none, the common case, so a caller can skip the work.
DEKI_EDITOR_API std::map<int64_t, std::vector<std::string>> CollectTargetRequirements(const nlohmann::json& scene);

}  // namespace DekiEditor
