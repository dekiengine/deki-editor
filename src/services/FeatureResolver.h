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
// files rather than trusting a descriptor to have listed it. Only a package
// that says `"strippable": true` in its descriptor has features taken away
// by the scan: a platform package's setup components (SDL3DisplaySetup) are
// created by the runtime, not by a scene, and must never go. The editor's own
// builds never strip anything: it has to open every scene.

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
    // False when the user gets no switch: the feature is required, or its
    // package has not opted into stripping.
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

// The whole thing for a project: descriptors, scan, settings.
StripPlan ComputeStripPlan(const std::string& projectPath);

}  // namespace DekiEditor
