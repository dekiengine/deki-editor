#include <deki-editor/FeatureResolver.h>
#include <deki-editor/build/PlatformConfig.h>

#include <deki-editor/Paths.h>
#include <deki/LogSystem.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace DekiEditor
{

const char* FeatureStateName(FeatureState state)
{
    switch (state)
    {
        case FeatureState::Required: return "required";
        case FeatureState::Pinned: return "pinned on";
        case FeatureState::Used: return "used";
        case FeatureState::Kept: return "kept";
        case FeatureState::DefaultOn: return "on";
        case FeatureState::ForcedOff: return "forced off";
        case FeatureState::Stripped: return "stripped";
    }
    return "?";
}

bool StripPlan::AnythingStripped() const
{
    for (const auto& f : features)
        if (!f.on) return true;
    for (const auto& p : packages)
        if (!p.on) return true;
    return false;
}

std::vector<std::string> StripPlan::CodegenExcludeRegexes() const
{
    std::vector<std::string> out;
    for (const auto& pkg : excludedStems)
        for (const auto& stem : pkg.second)
            out.push_back("/" + pkg.first + "/" + stem + "\\.(h|cpp|c)$");
    return out;
}

std::vector<std::string> StripPlan::SourceExcludeRegexes() const
{
    std::vector<std::string> out;
    for (const auto& pkg : excludedStems)
        for (const auto& stem : pkg.second)
            out.push_back("/" + pkg.first + "/" + stem + "\\.(cpp|c)$");
    return out;
}

const FeatureDecision* StripPlan::Find(const std::string& packageId, const std::string& featureId) const
{
    for (const auto& f : features)
        if (f.packageId == packageId && f.featureId == featureId)
            return &f;
    return nullptr;
}

namespace
{

std::string Join(const std::vector<std::string>& v, size_t limit = 3)
{
    std::string s;
    for (size_t i = 0; i < v.size() && i < limit; ++i)
        s += (i ? ", " : "") + v[i];
    if (v.size() > limit)
        s += ", ...";
    return s;
}

std::string ReadWhole(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The #include lines of `text` that a device build with the feature off
// would still compile: not inside `#ifdef DEKI_EDITOR` (the editor has every
// feature) and not inside `#ifdef DEKI_FEATURE_*` (gone with the feature).
// A small preprocessor walk: each conditional pushes whether its branch is
// one of those, `#else` flips it, `#endif` pops. `#ifndef DEKI_EDITOR` is the
// device branch and counts.
std::vector<std::string> DeviceIncludeLines(const std::string& text)
{
    std::vector<std::string> out;
    std::vector<bool> guarded;  // one per open conditional: is this branch editor- or feature-only?
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos || line[i] != '#')
            continue;
        std::string directive = line.substr(i + 1);
        directive.erase(0, directive.find_first_not_of(" \t"));
        const bool ifdef = directive.rfind("ifdef", 0) == 0;
        const bool ifndef = directive.rfind("ifndef", 0) == 0;
        const bool plainIf = !ifdef && !ifndef && directive.rfind("if", 0) == 0;
        if (ifdef || ifndef || plainIf)
        {
            const bool namesEditor = directive.find("DEKI_EDITOR") != std::string::npos;
            const bool namesFeature = directive.find("DEKI_FEATURE_") != std::string::npos;
            const bool negated = ifndef || directive.find('!') != std::string::npos;
            guarded.push_back((namesEditor || namesFeature) && !negated);
        }
        else if (directive.rfind("else", 0) == 0 && !guarded.empty())
        {
            guarded.back() = !guarded.back();
        }
        else if (directive.rfind("endif", 0) == 0 && !guarded.empty())
        {
            guarded.pop_back();
        }
        else if (directive.rfind("include", 0) == 0)
        {
            bool inGuarded = false;
            for (bool g : guarded)
                inGuarded = inGuarded || g;
            if (!inGuarded)
                out.push_back(directive);
        }
    }
    return out;
}

// Does any source of `pkgDir` that is NOT itself excluded include
// "<Stem>.h" for one of `offStems`, outside an editor-only or feature-guarded
// block? Returns the stems that are included and by whom. This is what keeps
// roller's dependency on clip alive without the descriptor having to say so.
std::map<std::string, std::string> IncludedByKeptSources(const fs::path& pkgDir,
                                                         const std::set<std::string>& offStems)
{
    std::map<std::string, std::string> included;
    if (offStems.empty())
        return included;
    std::error_code ec;
    if (!fs::exists(pkgDir, ec))
        return included;

    for (const auto& entry : fs::recursive_directory_iterator(pkgDir, ec))
    {
        if (!entry.is_regular_file())
            continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".c")
            continue;
        const std::string generic = entry.path().generic_string();
        if (generic.find("/editor/") != std::string::npos || generic.find("/tests/") != std::string::npos ||
            generic.find("/generated/") != std::string::npos)
            continue;
        const std::string stem = entry.path().stem().string();
        if (offStems.count(stem))
            continue;  // an excluded file may include what it likes
        const std::vector<std::string> includes = DeviceIncludeLines(ReadWhole(entry.path()));
        for (const auto& off : offStems)
        {
            if (included.count(off))
                continue;
            for (const auto& inc : includes)
            {
                if (inc.find("\"" + off + ".h\"") != std::string::npos ||
                    inc.find("/" + off + ".h\"") != std::string::npos ||
                    inc.find("/" + off + ".h>") != std::string::npos)
                {
                    included[off] = entry.path().filename().string();
                    break;
                }
            }
        }
    }
    return included;
}

}  // namespace

StripPlan ResolveStripPlan(const std::vector<CMakeGen::PackageEntry>& allPackages,
                           const ComponentUsage& usage,
                           const BuildStripSettings& settings,
                           const std::string& packagesDir)
{
    StripPlan plan;

    // ---- features: the rules, before anyone's includes are considered ----
    for (const auto& pkg : allPackages)
    {
        for (const auto& feat : pkg.features)
        {
            FeatureDecision d;
            d.packageId = pkg.id;
            d.packageDir = pkg.dirName;
            d.featureId = feat.id;
            d.displayName = feat.displayName.empty() ? feat.id : feat.displayName;
            d.buildDefine = feat.buildDefine;
            d.componentTypes = feat.componentTypes;

            const std::string ov = settings.Override(pkg.id, feat.id);
            if (feat.required)
            {
                d.on = true;
                d.state = FeatureState::Required;
                d.reason = "required by the package";
                d.offered = false;
            }
            else if (ov == "on")
            {
                d.on = true;
                d.state = FeatureState::Pinned;
                d.reason = "pinned on in settings/build.json";
            }
            else if (ov == "off")
            {
                d.on = false;
                d.state = FeatureState::ForcedOff;
                d.reason = "forced off in settings/build.json";
            }
            else if (feat.componentTypes.empty())
            {
                d.on = true;
                d.state = FeatureState::DefaultOn;
                d.reason = "no components to scan for";
            }
            else
            {
                // A package naming its OWN component is not evidence that the
                // game needs it - AnimationComponent.cpp obviously mentions
                // AnimationComponent, and counting that would keep every
                // feature alive forever. What counts is a scene, the project's
                // own code, or ANOTHER package: deki-fsm calling
                // Deki2D::AnimationComponent::Play() genuinely needs deki-2d's
                // animation feature, and stripping it broke the link.
                // Intra-package dependencies are handled separately, by the
                // include inspection that marks a feature Kept.
                const std::string ownPrefix =
                    GetPackagesDirectoryRelative() + "/" + pkg.id + "/";

                std::vector<std::string> where;
                for (const auto& type : feat.componentTypes)
                {
                    auto it = usage.byType.find(type);
                    if (it == usage.byType.end())
                        continue;
                    for (const auto& w : it->second)
                    {
                        if (w.rfind(ownPrefix, 0) == 0)
                            continue;
                        where.push_back(type + " in " + w);
                    }
                }
                if (!where.empty())
                {
                    d.on = true;
                    d.state = FeatureState::Used;
                    d.reason = Join(where);
                }
                else if (settings.stripUnusedComponents)
                {
                    d.on = false;
                    d.state = FeatureState::Stripped;
                    d.reason = "no scene or source names " + Join(feat.componentTypes);
                }
                else
                {
                    d.on = true;
                    d.state = FeatureState::DefaultOn;
                    d.reason = "unused, but stripping is off";
                }
            }
            plan.features.push_back(d);
        }
    }

    // ---- includes: a kept source that includes an excluded header keeps it ----
    if (!packagesDir.empty())
    {
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (const auto& pkg : allPackages)
            {
                std::set<std::string> offStems;
                std::map<std::string, FeatureDecision*> byStem;
                for (auto& d : plan.features)
                {
                    if (d.packageDir != pkg.dirName || d.on)
                        continue;
                    for (const auto& stem : d.componentTypes)
                    {
                        offStems.insert(stem);
                        byStem[stem] = &d;
                    }
                }
                if (offStems.empty())
                    continue;
                const auto included = IncludedByKeptSources(fs::path(packagesDir) / pkg.dirName, offStems);
                for (const auto& hit : included)
                {
                    FeatureDecision* d = byStem[hit.first];
                    if (!d || d->on)
                        continue;
                    d->on = true;
                    d->state = FeatureState::Kept;
                    d->reason = hit.first + ".h is included by " + hit.second;
                    changed = true;
                }
            }
        }
    }

    // ---- packages ----
    // A package goes only when asked, only when it had component features to
    // judge by and nothing outside it names one (a feature kept by the
    // package's own sources is no reason for the whole package to stay), and
    // only when no kept package requires it.
    for (const auto& pkg : allPackages)
    {
        bool hasComponentFeatures = false;
        bool anyWanted = false;
        for (const auto& d : plan.features)
        {
            if (d.packageDir != pkg.dirName)
                continue;
            if (!d.componentTypes.empty())
                hasComponentFeatures = true;
            if (d.state == FeatureState::Used || d.state == FeatureState::Pinned || d.state == FeatureState::Required)
                anyWanted = true;
        }
        PackageDecision p;
        p.packageId = pkg.id;
        p.packageDir = pkg.dirName;
        p.on = true;
        if (settings.stripUnusedPackages && hasComponentFeatures && !anyWanted)
        {
            p.on = false;
            p.reason = "no scene or source names any of its components";
        }
        else
        {
            p.reason = hasComponentFeatures ? "has components in use" : "no component features to judge by";
        }
        plan.packages.push_back(p);
    }
    // Dependencies of kept packages stay, however their own features fared.
    bool depChanged = true;
    while (depChanged)
    {
        depChanged = false;
        for (const auto& pkg : allPackages)
        {
            auto self = std::find_if(plan.packages.begin(), plan.packages.end(),
                                     [&](const PackageDecision& p) { return p.packageId == pkg.id; });
            if (self == plan.packages.end() || !self->on)
                continue;
            for (const auto& dep : pkg.dekiDeps)
            {
                for (auto& other : plan.packages)
                {
                    if (other.on)
                        continue;
                    if (other.packageId == dep || other.packageDir == dep)
                    {
                        other.on = true;
                        other.reason = "required by " + pkg.id;
                        depChanged = true;
                    }
                }
            }
        }
    }

    // A stripped package takes its features with it, so the plan reads as one.
    for (auto& d : plan.features)
    {
        auto self = std::find_if(plan.packages.begin(), plan.packages.end(),
                                 [&](const PackageDecision& p) { return p.packageDir == d.packageDir; });
        if (self != plan.packages.end() && !self->on && d.on)
        {
            d.on = false;
            d.state = FeatureState::Stripped;
            d.reason = "its package is stripped: " + self->reason;
        }
    }

    // ---- outputs ----
    plan.defines.push_back("DEKI_PACKAGE_FEATURES_CONFIGURED");
    for (const auto& pkg : allPackages)
    {
        auto self = std::find_if(plan.packages.begin(), plan.packages.end(),
                                 [&](const PackageDecision& p) { return p.packageId == pkg.id; });
        const bool pkgOn = self != plan.packages.end() && self->on;
        if (!pkgOn)
            continue;
        if (!pkg.buildDefine.empty())
            plan.defines.push_back(pkg.buildDefine);
        for (const auto& d : plan.features)
        {
            if (d.packageDir != pkg.dirName)
                continue;
            if (d.on)
            {
                if (!d.buildDefine.empty())
                    plan.defines.push_back(d.buildDefine);
            }
            else
            {
                for (const auto& stem : d.componentTypes)
                    plan.excludedStems[pkg.dirName].push_back(stem);
            }
        }
    }

    // ---- summary ----
    {
        std::map<std::string, std::vector<std::string>> offByPkg;
        size_t offFeatures = 0;
        for (const auto& d : plan.features)
        {
            if (d.on)
                continue;
            offByPkg[d.packageId].push_back(d.featureId);
            ++offFeatures;
        }
        std::vector<std::string> offPkgs;
        for (const auto& p : plan.packages)
            if (!p.on)
                offPkgs.push_back(p.packageId);

        if (offFeatures == 0 && offPkgs.empty())
        {
            plan.summary = "nothing stripped";
        }
        else
        {
            std::ostringstream s;
            if (offFeatures)
            {
                s << "stripped " << offFeatures << " feature" << (offFeatures == 1 ? "" : "s");
                bool first = true;
                for (const auto& kv : offByPkg)
                {
                    s << (first ? " (" : "; ") << kv.first << ": " << Join(kv.second, 12);
                    first = false;
                }
                s << ")";
            }
            if (!offPkgs.empty())
            {
                if (offFeatures) s << "; ";
                s << "stripped " << offPkgs.size() << " package" << (offPkgs.size() == 1 ? "" : "s")
                  << " (" << Join(offPkgs, 12) << ")";
            }
            plan.summary = s.str();
        }
    }

    return plan;
}

std::string TargetContentConflict::Describe() const
{
    std::string s = where + " uses " + componentType + " from " + packageId + ", which needs ";
    for (size_t i = 0; i < missing.size(); ++i)
        s += (i ? ", " : "") + missing[i];
    s += missing.size() == 1 ? "; this target does not provide it."
                             : "; this target provides none of those.";
    return s;
}

std::vector<TargetContentConflict> FindTargetContentConflicts(const std::string& projectPath,
                                                              const std::string& targetPlatformId)
{
    std::vector<TargetContentConflict> conflicts;
    if (targetPlatformId.empty())
        return conflicts;  // no target, nothing to be incompatible with

    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(targetPlatformId);
    if (!platform)
        return conflicts;
    const std::set<std::string> caps = platform->Capabilities();

    const auto all = CMakeGen::ScanPackageManifests(projectPath);

    // component type -> the package that provides it.
    std::map<std::string, const CMakeGen::PackageEntry*> owner;
    std::vector<std::string> knownTypes;
    for (const auto& pkg : all)
        for (const auto& feat : pkg.features)
            for (const auto& type : feat.componentTypes)
            {
                owner[type] = &pkg;
                knownTypes.push_back(type);
            }

    // Scoped to this target, and with the objects it will not carry already
    // dropped: an object excluded by its own "requires" is not this target's
    // content and must not be reported against it. Likewise only the packages
    // this target can actually build are scanned - an ineligible package's
    // sources are not its content either.
    std::vector<std::string> eligibleIds;
    for (const auto& pkg : all)
        if (CMakeGen::MissingCapabilities(pkg, caps).empty())
            eligibleIds.push_back(pkg.id);

    const ComponentUsage usage =
        ScanComponentUsage(projectPath, knownTypes, eligibleIds, targetPlatformId, caps);

    for (const auto& [type, places] : usage.byType)
    {
        auto it = owner.find(type);
        if (it == owner.end())
            continue;
        const std::vector<std::string> missing = CMakeGen::MissingCapabilities(*it->second, caps);
        if (missing.empty())
            continue;

        TargetContentConflict c;
        c.componentType = type;
        c.where = places.empty() ? "(unknown)" : places.front();
        c.packageId = it->second->id;
        c.missing = missing;
        conflicts.push_back(std::move(c));
    }

    return conflicts;
}

StripPlan ComputeStripPlan(const std::string& projectPath, const std::string& targetPlatformId)
{
    const auto all = CMakeGen::ScanPackageManifests(projectPath);
    std::vector<std::string> knownTypes;
    for (const auto& pkg : all)
        for (const auto& feat : pkg.features)
            for (const auto& type : feat.componentTypes)
                knownTypes.push_back(type);
    // What the target offers, so objects a scene marks as needing something it
    // has not got stop keeping their components alive. Resolved from whatever
    // platforms are registered; an unknown target simply offers nothing, which
    // keeps every object, the same as having no target at all.
    std::set<std::string> targetCapabilities;
    if (!targetPlatformId.empty())
    {
        if (const PlatformConfig* platform =
                PlatformRegistry::Instance().GetPlatform(targetPlatformId))
            targetCapabilities = platform->Capabilities();
    }

    // Only packages this target can build count as usage; see the note in
    // ComponentUsageScan.h.
    std::vector<std::string> eligibleIds;
    for (const auto& pkg : all)
        if (CMakeGen::MissingCapabilities(pkg, targetCapabilities).empty())
            eligibleIds.push_back(pkg.id);

    const ComponentUsage usage = ScanComponentUsage(projectPath, knownTypes, eligibleIds,
                                                    targetPlatformId, targetCapabilities);
    const BuildStripSettings settings = BuildStripSettings::Load(projectPath);
    return ResolveStripPlan(all, usage, settings, GetPackagesDirectory(projectPath));
}

}  // namespace DekiEditor
