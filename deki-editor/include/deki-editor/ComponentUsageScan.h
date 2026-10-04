#pragma once

#include <deki-editor/EditorExport.h>

// Which component types a project uses.
//
// A scene names every component it carries. A script can also create one at
// runtime (`AddComponent<GradientComponent>()`), which no scene records, so
// the project's sources are searched for the known type names too. The
// stripper trusts the two together; anything neither mentions may be left
// out of a device build.

#include <map>
#include <set>
#include <string>
#include <vector>

namespace DekiEditor
{

struct DEKI_EDITOR_API ComponentUsage
{
    // Short component type name (namespace stripped) -> where it was seen,
    // as project-relative paths. A scene path means an object carries it; a
    // source path means the code names it.
    std::map<std::string, std::vector<std::string>> byType;

    bool Uses(const std::string& type) const { return byType.count(type) != 0; }
};

// `knownTypes` limits the source search to real component names, so a word
// in a comment keeps a feature alive only if it names a component the
// project could create.
//
// `targetPlatformId` limits the platform boot scenes to the target being
// built: one platform's boot scene says nothing about another's (pooled, a
// simulator's SDL3 setup kept SDL3 in a firmware build). Empty means every
// platform, the pooled view the editor panels use when no target is selected.
// `targetCapabilities` is what the target platform offers. Objects a scene
// marks as needing something it does not offer are left out of that target's
// build (services/SceneTargetFilter), so their components do not count as
// used here either; otherwise a package is kept for an object the build will
// not contain.
// `scanPackageIds` names the packages whose own sources count as usage. A
// package may use another package's component (deki-fsm calls
// Deki2D::AnimationComponent::Play()), and that is as real as a use in the
// project's code: leaving it out strips a feature the build still calls, and
// the link fails with an undefined reference.
//
// It must be the packages ELIGIBLE for the target, not every installed one:
// deki-sdl3-integration's sources are not this target's content when the
// target cannot provide sdl3, and counting them reports a conflict against a
// package that will not be built.
//
// `extraSceneDirs` are more folders whose scenes count, for a build whose
// boot scene is not under the project: a simulated build compiles a copy of
// the board's boot scene, with the simulator's steps added, in its own build
// folder.
DEKI_EDITOR_API ComponentUsage ScanComponentUsage(const std::string& projectPath,
                                                  const std::vector<std::string>& knownTypes,
                                                  const std::vector<std::string>& scanPackageIds,
                                                  const std::string& targetPlatformId = "",
                                                  const std::set<std::string>& targetCapabilities = {},
                                                  const std::vector<std::string>& extraSceneDirs = {});

// The name a scene or a descriptor uses for a type: the identifier after the
// last `::`, so "Deki2D::SpriteComponent" and "SpriteComponent" agree.
DEKI_EDITOR_API std::string ShortTypeName(const std::string& qualified);

}  // namespace DekiEditor
