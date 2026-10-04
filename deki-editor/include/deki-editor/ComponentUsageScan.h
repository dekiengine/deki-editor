#pragma once

#include <deki-editor/EditorExport.h>

// Which component types a project actually uses.
//
// A scene names every component it carries, so the scenes are the authority
// for authored content. A script can also create a component at runtime
// (`AddComponent<GradientComponent>()`), which no scene records, so the
// project's sources are searched for the known type names as well. The two
// together are what the stripper trusts; anything neither mentions is a
// candidate for leaving out of a device build.

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

// `knownTypes` bounds the source search to real component names, so a word
// that happens to appear in a comment cannot keep a feature alive unless it
// is a component the project could actually create.
//
// `targetPlatformId` scopes the platform boot scenes to the target being
// built: one platform's boot scene is no evidence about another's, and
// pooling them is what let a simulator's SDL3 setup keep SDL3 alive in a
// firmware build. Empty means every platform, the pooled view the editor
// panels use when no target is selected.
// `targetCapabilities` is what the target platform offers. Objects a scene
// marks as needing something it does not offer are not part of that target's
// build (services/SceneTargetFilter), so their components must not count as
// used here either - otherwise the stripper keeps a package alive for an object
// the build will not contain.
// `scanPackageIds` names the packages whose own sources count as usage. A
// package may use another package's component - deki-fsm calls
// Deki2D::AnimationComponent::Play() - and that reference is as real as one in
// the project's own code; scanning only the project stripped deki-2d's
// animation feature out of a build whose deki-fsm still called it, and the
// link failed with an undefined reference pointing nowhere near stripping.
//
// It must be the packages ELIGIBLE for the target, not every installed one:
// deki-sdl3-integration's sources are not this target's content when the
// target cannot provide sdl3, and counting them reports a conflict against a
// package that was never going to be built.
//
// `extraSceneDirs` are more directories whose scenes count, for a build whose
// boot scene is not under the project: a simulated build compiles a copy of
// the board's boot scene with the simulator's steps added, kept in its own
// build directory.
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
