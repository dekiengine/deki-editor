#pragma once

// Which package features a device build compiles, and what it leaves out.
//
// The inputs are the package descriptors (each feature's build define and the
// component types it provides), what the project's scenes and sources
// actually use, and the project's stripping settings. The output is a plan:
// one decision per feature and per package, with the reason, plus the compile
// definitions to emit and the component sources and headers to exclude.
//
// A feature stays in when it is required, pinned, used, or included by a
// kept feature's sources; the last is learned by reading the package's own
// files rather than trusting a descriptor to have listed it. Every package
// is strippable at the level the project chose; the scan reads the platform
// boot scenes as well as the project's, so the setup components a platform
// names there (SDL3DisplaySetup) count as used. The editor's own builds never
// strip anything: it has to open every scene.

#include "BuildStripSettings.h"
#include "ComponentUsageScan.h"
#include "builders/CMakeGenUtils.h"

#include <map>
#include <string>
#include <vector>

namespace DekiEditor
{

enum class FeatureState
{
    Required,   // the descriptor says it can never go
    Pinned,     // the project forced it on
    Used,       // a scene or source names one of its components
    Kept,       // a kept feature's sources include it
    DefaultOn,  // nothing strips it: stripping is off, or it has no components to scan for
    ForcedOff,  // the project forced it off
    Stripped,   // unused, and stripping is on
};

const char* FeatureStateName(FeatureState state);

struct FeatureDecision
{
    std::string packageId;
    std::string packageDir;
    std::string featureId;
    std::string displayName;
    std::string buildDefine;
    std::vector<std::string> componentTypes;
    bool on = true;
    FeatureState state = FeatureState::DefaultOn;
    std::string reason;
    // False when the user gets no switch: the feature is required.
    bool offered = true;
};

struct PackageDecision
{
    std::string packageId;
    std::string packageDir;
    bool on = true;
    std::string reason;
};

struct StripPlan
{
    std::vector<FeatureDecision> features;
    std::vector<PackageDecision> packages;

    // What the build defines: the configured marker, then every kept package's
    // and feature's define, in descriptor order.
    std::vector<std::string> defines;

    // package directory -> component stems (Type) whose Type.cpp / Type.h stay out.
    std::map<std::string, std::vector<std::string>> excludedStems;

    // One line for the build log.
    std::string summary;

    bool AnythingStripped() const;

    // For the reflection codegen's EXCLUDE_REGEX, one per stem:
    // "/<dir>/<Stem>\.(h|cpp|c)$" (the header is not reflected and the source
    // is not scanned for adapter registrations).
    std::vector<std::string> CodegenExcludeRegexes() const;
    // For the source globs, one per stem: "/<dir>/<Stem>\.(cpp|c)$".
    std::vector<std::string> SourceExcludeRegexes() const;

    const FeatureDecision* Find(const std::string& packageId, const std::string& featureId) const;
};

// `packagesDir` is where the package sources are, for the include inspection;
// empty skips that step (the unit tests do this to test the rules alone).
StripPlan ResolveStripPlan(const std::vector<CMakeGen::PackageEntry>& allPackages,
                           const ComponentUsage& usage,
                           const BuildStripSettings& settings,
                           const std::string& packagesDir);

// A component this target's own content uses, from a package this target
// cannot have. The one case that genuinely is a mistake rather than a normal
// exclusion, and the one worth refusing a build over.
struct TargetContentConflict
{
    std::string componentType;            // e.g. "SDL3DisplaySetup"
    std::string where;                    // the scene or source that names it
    std::string packageId;                // the package providing it
    std::vector<std::string> missing;     // what the package needs and the target lacks

    // One line a person can act on, naming all four.
    std::string Describe() const;
};

// Everything this target's content asks for that the target cannot supply.
// Empty means the build is coherent: packages left out are left out because
// nothing here needs them, which is the normal multi-target case and no error.
std::vector<TargetContentConflict> FindTargetContentConflicts(const std::string& projectPath,
                                                              const std::string& targetPlatformId);

// The whole thing for a project: descriptors, scan, settings.
//
// `targetPlatformId` is the platform being built. It scopes which boot scenes
// count as evidence, so a package used only by another target's boot scene is
// not kept alive here. Empty pools every platform, for callers with no target
// in hand.
StripPlan ComputeStripPlan(const std::string& projectPath, const std::string& targetPlatformId = "");

}  // namespace DekiEditor
