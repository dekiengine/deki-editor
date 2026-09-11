#pragma once

// Which component types a project actually uses.
//
// A scene names every component it carries, so the scenes are the authority
// for authored content. A script can also create a component at runtime
// (`AddComponent<GradientComponent>()`), which no scene records, so the
// project's sources are searched for the known type names as well. The two
// together are what the stripper trusts; anything neither mentions is a
// candidate for leaving out of a device build.

#include <map>
#include <string>
#include <vector>

namespace DekiEditor
{

struct ComponentUsage
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
ComponentUsage ScanComponentUsage(const std::string& projectPath,
                                  const std::vector<std::string>& knownTypes,
                                  const std::string& targetPlatformId = "");

// The name a scene or a descriptor uses for a type: the identifier after the
// last `::`, so "Deki2D::SpriteComponent" and "SpriteComponent" agree.
std::string ShortTypeName(const std::string& qualified);

}  // namespace DekiEditor
