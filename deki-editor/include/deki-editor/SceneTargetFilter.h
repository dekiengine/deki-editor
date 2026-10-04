#pragma once

#include <deki-editor/EditorExport.h>

// Which objects of a scene exist on a given target.
//
// One project serves several targets, and a scene is shared by all of them. An
// object that only makes sense on one - a WiFi setup, a desktop-only debug
// overlay - says what it needs from the platform in the same vocabulary a
// package uses:
//
//     { "name": "WiFi", "requires": ["wifi"], "components": [ ... ] }
//
// The target either offers that capability or the object is not part of that
// target's build. Without this the only outcomes were a build that carries
// something it cannot run, or one that refuses to build at all, and neither is
// what "desktop and embedded in one project" needs.
//
// This is a BUILD-time concept, exactly like feature stripping: the filter runs
// over the scene JSON before it is compiled, so the runtime never learns that
// targets exist and the scene format stays something older engines can read.
// An object with no "requires" is on every target, so every scene ever authored
// keeps working.

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace DekiEditor
{

// What `obj` needs that `capabilities` does not offer. Empty means it belongs
// on this target. An empty capability set judges nothing and keeps everything,
// which is what a caller with no target in hand (the editor) wants.
DEKI_EDITOR_API std::vector<std::string> ObjectMissingCapabilities(const nlohmann::json& obj,
                                                                   const std::set<std::string>& capabilities);

// `scene` with every object the target cannot satisfy removed, children
// included: dropping a parent drops what it carries, because a child's
// existence depends on it.
DEKI_EDITOR_API nlohmann::json FilterSceneForTarget(const nlohmann::json& scene,
                                                    const std::set<std::string>& capabilities);

// True when the filter would change anything, so a caller can skip the copy.
DEKI_EDITOR_API bool SceneHasTargetRequirements(const nlohmann::json& scene);

// Carry every object's "requires" from the file as it was into the file as the
// editor has just rewritten it, matching objects by id.
//
// The engine's Object does not model "requires" - it is a build-time key and
// deliberately invisible to the runtime - so saving a scene rebuilds the JSON
// from objects that never knew about it and the authoring is lost. Silently,
// which is the worst way to lose it. Re-applying afterwards keeps one file and
// costs no engine ABI. An object deleted meanwhile is simply not found, so its
// entry goes with it.
//
// Returns true if anything was re-applied.
DEKI_EDITOR_API bool PreserveTargetRequirements(const std::string& scenePath, const nlohmann::json& before);

// The "requires" of every object in `scene`, keyed by object id. Empty when the
// scene has none, which is the common case and lets a caller skip the work.
DEKI_EDITOR_API std::map<int64_t, std::vector<std::string>> CollectTargetRequirements(const nlohmann::json& scene);

}  // namespace DekiEditor
