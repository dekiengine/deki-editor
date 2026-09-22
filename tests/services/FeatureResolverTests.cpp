// The stripper's rules, with the package descriptors and the usage scan
// stated directly. The include inspection gets a real directory, because
// that is the one rule that reads files.

#include <gtest/gtest.h>

#include <deki-editor/FeatureResolver.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace DekiEditor;
namespace fs = std::filesystem;

namespace
{
CMakeGen::FeatureEntry Feat(const char* id, const char* define, std::vector<std::string> types, bool required = false)
{
    CMakeGen::FeatureEntry f;
    f.id = id;
    f.displayName = id;
    f.buildDefine = define;
    f.componentTypes = std::move(types);
    f.required = required;
    return f;
}

CMakeGen::PackageEntry Pkg(const char* id, const char* define, std::vector<CMakeGen::FeatureEntry> feats,
                           std::vector<std::string> deps = {})
{
    CMakeGen::PackageEntry p;
    p.id = id;
    p.dirName = id;
    p.buildDefine = define;
    p.features = std::move(feats);
    p.dekiDeps = std::move(deps);
    return p;
}

ComponentUsage Using(std::vector<std::string> types)
{
    ComponentUsage u;
    for (const auto& t : types)
        u.byType[t].push_back("assets/scenes/a.scene");
    return u;
}

bool Has(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

const std::vector<CMakeGen::PackageEntry> kTwoD = {
    Pkg("deki-2d", "DEKI_PACKAGE_2D",
        { Feat("sprite", "DEKI_FEATURE_SPRITE", { "SpriteComponent" }),
          Feat("gradient", "DEKI_FEATURE_GRADIENT", { "GradientComponent" }),
          Feat("transform", "DEKI_TRANSFORM_2D", {}, /*required=*/true) }),
};
}  // namespace

TEST(FeatureResolver, UsedStaysAndUnusedGoes)
{
    BuildStripSettings s;
    const StripPlan plan = ResolveStripPlan(kTwoD, Using({ "SpriteComponent" }), s, "");

    const FeatureDecision* sprite = plan.Find("deki-2d", "sprite");
    const FeatureDecision* gradient = plan.Find("deki-2d", "gradient");
    ASSERT_TRUE(sprite && gradient);
    EXPECT_TRUE(sprite->on);
    EXPECT_EQ(sprite->state, FeatureState::Used);
    EXPECT_FALSE(gradient->on);
    EXPECT_EQ(gradient->state, FeatureState::Stripped);

    EXPECT_TRUE(Has(plan.defines, "DEKI_PACKAGE_FEATURES_CONFIGURED"));
    EXPECT_TRUE(Has(plan.defines, "DEKI_PACKAGE_2D"));
    EXPECT_TRUE(Has(plan.defines, "DEKI_FEATURE_SPRITE"));
    EXPECT_FALSE(Has(plan.defines, "DEKI_FEATURE_GRADIENT"));
    ASSERT_EQ(plan.excludedStems.at("deki-2d"), std::vector<std::string>{ "GradientComponent" });
    EXPECT_TRUE(plan.AnythingStripped());
    EXPECT_NE(plan.summary.find("gradient"), std::string::npos) << plan.summary;
}

TEST(FeatureResolver, RequiredCanNeverBeStripped)
{
    BuildStripSettings s;
    s.SetOverride("deki-2d", "transform", "off");  // a stale or hostile setting
    const StripPlan plan = ResolveStripPlan(kTwoD, Using({}), s, "");
    const FeatureDecision* t = plan.Find("deki-2d", "transform");
    ASSERT_TRUE(t);
    EXPECT_TRUE(t->on);
    EXPECT_FALSE(t->offered);
    EXPECT_EQ(t->state, FeatureState::Required);
    EXPECT_TRUE(Has(plan.defines, "DEKI_TRANSFORM_2D"));
}

TEST(FeatureResolver, StrippingOffKeepsEverything)
{
    BuildStripSettings s;
    s.stripUnusedComponents = false;
    const StripPlan plan = ResolveStripPlan(kTwoD, Using({}), s, "");
    EXPECT_TRUE(plan.Find("deki-2d", "gradient")->on);
    EXPECT_EQ(plan.Find("deki-2d", "gradient")->state, FeatureState::DefaultOn);
    EXPECT_FALSE(plan.AnythingStripped());
    EXPECT_EQ(plan.summary, "nothing stripped");
}

TEST(FeatureResolver, PinsAndForcesWin)
{
    BuildStripSettings s;
    s.SetOverride("deki-2d", "gradient", "on");
    s.SetOverride("deki-2d", "sprite", "off");
    const StripPlan plan = ResolveStripPlan(kTwoD, Using({ "SpriteComponent" }), s, "");
    EXPECT_TRUE(plan.Find("deki-2d", "gradient")->on);
    EXPECT_EQ(plan.Find("deki-2d", "gradient")->state, FeatureState::Pinned);
    EXPECT_FALSE(plan.Find("deki-2d", "sprite")->on);
    EXPECT_EQ(plan.Find("deki-2d", "sprite")->state, FeatureState::ForcedOff);
}

TEST(FeatureResolver, AFeatureWithNoComponentsIsNotAutoStripped)
{
    const std::vector<CMakeGen::PackageEntry> pkgs = {
        Pkg("deki-wifi", "DEKI_PACKAGE_WIFI", { Feat("wifi", "DEKI_FEATURE_WIFI", {}) }),
    };
    BuildStripSettings s;
    s.stripUnusedPackages = true;
    const StripPlan plan = ResolveStripPlan(pkgs, Using({}), s, "");
    EXPECT_TRUE(plan.Find("deki-wifi", "wifi")->on);
    EXPECT_TRUE(plan.packages[0].on) << plan.packages[0].reason;
    EXPECT_TRUE(Has(plan.defines, "DEKI_PACKAGE_WIFI"));
}

TEST(FeatureResolver, AKeptSourceKeepsWhatItIncludes)
{
    // roller is used and its source includes Clip.h; clip is unused and would
    // be stripped, which would break the build. The inspection keeps it.
    const fs::path root = fs::temp_directory_path() / "deki-feature-resolver-test";
    fs::remove_all(root);
    fs::create_directories(root / "deki-2d");
    std::ofstream(root / "deki-2d" / "RollerComponent.cpp") << "#include \"ClipComponent.h\"\n";
    std::ofstream(root / "deki-2d" / "ClipComponent.cpp") << "// clip\n";
    std::ofstream(root / "deki-2d" / "ClipComponent.h") << "// clip\n";

    const std::vector<CMakeGen::PackageEntry> pkgs = {
        Pkg("deki-2d", "DEKI_PACKAGE_2D",
            { Feat("roller", "DEKI_FEATURE_ROLLER", { "RollerComponent" }),
              Feat("clip", "DEKI_FEATURE_CLIP", { "ClipComponent" }) }),
    };
    BuildStripSettings s;
    const StripPlan plan = ResolveStripPlan(pkgs, Using({ "RollerComponent" }), s, root.string());
    const FeatureDecision* clip = plan.Find("deki-2d", "clip");
    ASSERT_TRUE(clip);
    EXPECT_TRUE(clip->on) << clip->reason;
    EXPECT_EQ(clip->state, FeatureState::Kept);
    EXPECT_NE(clip->reason.find("RollerComponent.cpp"), std::string::npos) << clip->reason;
    EXPECT_EQ(plan.excludedStems.count("deki-2d"), 0u);

    fs::remove_all(root);
}

TEST(FeatureResolver, AnEditorOnlyOrFeatureGuardedIncludeKeepsNothing)
{
    // The package entry point includes every component for the editor's
    // sake, and a feature-guarded include goes away with its feature; neither
    // keeps clip. A plain include in the same file does.
    const fs::path root = fs::temp_directory_path() / "deki-feature-resolver-test3";
    fs::remove_all(root);
    fs::create_directories(root / "deki-2d");
    std::ofstream(root / "deki-2d" / "Deki2DPackage.cpp")
        << "#include \"Deki2DPackage.h\"\n"
           "#ifdef DEKI_EDITOR\n"
           "#include \"ClipComponent.h\"\n"
           "#else\n"
           "#include \"SortingGroupComponent.h\"\n"
           "#endif\n"
           "#ifdef DEKI_FEATURE_ROLLER\n"
           "#include \"RollerComponent.h\"\n"
           "#endif\n"
           "#ifndef DEKI_EDITOR\n"
           "#include \"GradientComponent.h\"\n"
           "#endif\n";

    const std::vector<CMakeGen::PackageEntry> pkgs = {
        Pkg("deki-2d", "DEKI_PACKAGE_2D",
            { Feat("clip", "DEKI_FEATURE_CLIP", { "ClipComponent" }),
              Feat("sorting_group", "DEKI_FEATURE_SORTING_GROUP", { "SortingGroupComponent" }),
              Feat("roller", "DEKI_FEATURE_ROLLER", { "RollerComponent" }),
              Feat("gradient", "DEKI_FEATURE_GRADIENT", { "GradientComponent" }) }),
    };
    BuildStripSettings s;
    s.stripUnusedPackages = false;  // the features are the question here
    const StripPlan plan = ResolveStripPlan(pkgs, Using({}), s, root.string());
    EXPECT_FALSE(plan.Find("deki-2d", "clip")->on) << plan.Find("deki-2d", "clip")->reason;
    EXPECT_TRUE(plan.Find("deki-2d", "sorting_group")->on) << plan.Find("deki-2d", "sorting_group")->reason;
    EXPECT_FALSE(plan.Find("deki-2d", "roller")->on) << plan.Find("deki-2d", "roller")->reason;
    EXPECT_TRUE(plan.Find("deki-2d", "gradient")->on) << plan.Find("deki-2d", "gradient")->reason;

    fs::remove_all(root);
}

TEST(FeatureResolver, AnExcludedSourceMayIncludeWhatItLikes)
{
    // Both unused: roller includes clip, but roller itself is going, so its
    // include does not keep clip.
    const fs::path root = fs::temp_directory_path() / "deki-feature-resolver-test2";
    fs::remove_all(root);
    fs::create_directories(root / "deki-2d");
    std::ofstream(root / "deki-2d" / "RollerComponent.cpp") << "#include \"ClipComponent.h\"\n";

    const std::vector<CMakeGen::PackageEntry> pkgs = {
        Pkg("deki-2d", "DEKI_PACKAGE_2D",
            { Feat("roller", "DEKI_FEATURE_ROLLER", { "RollerComponent" }),
              Feat("clip", "DEKI_FEATURE_CLIP", { "ClipComponent" }) }),
    };
    BuildStripSettings s;
    const StripPlan plan = ResolveStripPlan(pkgs, Using({}), s, root.string());
    EXPECT_FALSE(plan.Find("deki-2d", "roller")->on);
    EXPECT_FALSE(plan.Find("deki-2d", "clip")->on);

    fs::remove_all(root);
}

TEST(FeatureResolver, PackagesGoOnlyWhenAskedAndOnlyWhenNothingNeedsThem)
{
    const std::vector<CMakeGen::PackageEntry> pkgs = {
        Pkg("deki-2d", "DEKI_PACKAGE_2D", { Feat("sprite", "DEKI_FEATURE_SPRITE", { "SpriteComponent" }) }),
        Pkg("deki-particles", "DEKI_PACKAGE_PARTICLES",
            { Feat("emitter", "DEKI_FEATURE_EMITTER", { "ParticleEmitter" }) }, { "deki-2d" }),
    };

    // Not asked: both stay, whatever their features did.
    {
        BuildStripSettings s;
        s.stripUnusedPackages = false;
        const StripPlan plan = ResolveStripPlan(pkgs, Using({}), s, "");
        EXPECT_TRUE(plan.packages[0].on);
        EXPECT_TRUE(plan.packages[1].on);
        EXPECT_TRUE(Has(plan.defines, "DEKI_PACKAGE_PARTICLES"));
    }
    // Asked, nothing used: both go.
    {
        BuildStripSettings s;
        s.stripUnusedPackages = true;
        const StripPlan plan = ResolveStripPlan(pkgs, Using({}), s, "");
        EXPECT_FALSE(plan.packages[0].on);
        EXPECT_FALSE(plan.packages[1].on);
        EXPECT_FALSE(Has(plan.defines, "DEKI_PACKAGE_2D"));
        EXPECT_FALSE(Has(plan.defines, "DEKI_PACKAGE_PARTICLES"));
        EXPECT_NE(plan.summary.find("2 packages"), std::string::npos) << plan.summary;
    }
    // Asked, particles used: particles stays, and keeps 2d because it needs it.
    {
        BuildStripSettings s;
        s.stripUnusedPackages = true;
        const StripPlan plan = ResolveStripPlan(pkgs, Using({ "ParticleEmitter" }), s, "");
        EXPECT_TRUE(plan.packages[1].on);
        EXPECT_TRUE(plan.packages[0].on) << plan.packages[0].reason;
        EXPECT_NE(plan.packages[0].reason.find("deki-particles"), std::string::npos);
        EXPECT_TRUE(Has(plan.defines, "DEKI_PACKAGE_2D"));
    }
}

TEST(FeatureResolver, AFeatureKeptByItsOwnPackageDoesNotKeepThePackage)
{
    // Every single-component package includes its component from its system
    // code, so the feature is always kept while the package stays. With the
    // package level on, that is no reason to stay: nothing outside names it.
    const fs::path root = fs::temp_directory_path() / "deki-feature-resolver-test4";
    fs::remove_all(root);
    fs::create_directories(root / "deki-tween");
    std::ofstream(root / "deki-tween" / "TweenManager.cpp") << "#include \"TweenComponent.h\"\n";

    const std::vector<CMakeGen::PackageEntry> pkgs = {
        Pkg("deki-tween", "DEKI_PACKAGE_TWEEN", { Feat("tween", "DEKI_FEATURE_TWEEN", { "TweenComponent" }) }),
    };
    {
        BuildStripSettings s;  // components only: the feature is kept, the package stays
        s.stripUnusedPackages = false;
        const StripPlan plan = ResolveStripPlan(pkgs, Using({}), s, root.string());
        EXPECT_EQ(plan.Find("deki-tween", "tween")->state, FeatureState::Kept);
        EXPECT_TRUE(plan.packages[0].on);
        EXPECT_TRUE(Has(plan.defines, "DEKI_PACKAGE_TWEEN"));
    }
    {
        BuildStripSettings s;
        s.stripUnusedPackages = true;
        const StripPlan plan = ResolveStripPlan(pkgs, Using({}), s, root.string());
        EXPECT_FALSE(plan.packages[0].on) << plan.packages[0].reason;
        EXPECT_FALSE(plan.Find("deki-tween", "tween")->on);
        EXPECT_NE(plan.Find("deki-tween", "tween")->reason.find("package is stripped"), std::string::npos);
        EXPECT_FALSE(Has(plan.defines, "DEKI_PACKAGE_TWEEN"));
        EXPECT_FALSE(Has(plan.defines, "DEKI_FEATURE_TWEEN"));
    }
    {
        BuildStripSettings s;  // used from a scene: stays at either level
        s.stripUnusedPackages = true;
        const StripPlan plan = ResolveStripPlan(pkgs, Using({ "TweenComponent" }), s, root.string());
        EXPECT_TRUE(plan.packages[0].on);
    }
    fs::remove_all(root);
}

TEST(FeatureResolver, RegexesNameTheExcludedFiles)
{
    BuildStripSettings s;
    const StripPlan plan = ResolveStripPlan(kTwoD, Using({ "SpriteComponent" }), s, "");
    ASSERT_EQ(plan.CodegenExcludeRegexes(), std::vector<std::string>{ "/deki-2d/GradientComponent\\.(h|cpp|c)$" });
    ASSERT_EQ(plan.SourceExcludeRegexes(), std::vector<std::string>{ "/deki-2d/GradientComponent\\.(cpp|c)$" });
}

// ---------------------------------------------------------------------------
// A feature's own requires: packages one part of a package needs and the rest
// does not. They count only while that feature is in the build.
// ---------------------------------------------------------------------------

namespace
{
CMakeGen::FeatureEntry FeatNeeding(const char* id, const char* define, const char* type, const char* needs)
{
    CMakeGen::FeatureEntry f = Feat(id, define, { type });
    f.requiredPackages = { needs };
    return f;
}

// An input package whose keyboard needs a bus package; the bus has components
// of its own, so the stripper can judge it.
std::vector<CMakeGen::PackageEntry> InputWithBus(bool busInstalled)
{
    std::vector<CMakeGen::PackageEntry> all = {
        Pkg("deki-input", "DEKI_PACKAGE_INPUT",
            { Feat("colliders", "DEKI_FEATURE_INPUT", { "InputCollider" }),
              FeatNeeding("keyboard", "DEKI_FEATURE_KEYBOARD", "KeyboardComponent", "deki-bus") }),
    };
    if (busInstalled)
        all.push_back(Pkg("deki-bus", "DEKI_PACKAGE_BUS", { Feat("bus", "DEKI_FEATURE_BUS", { "BusComponent" }) }));
    return all;
}
}  // namespace

TEST(FeatureRequires, APackageIsPulledInOnlyWhileTheFeatureNeedingItIsOn)
{
    const auto all = InputWithBus(true);
    const std::vector<std::string> keyboardOff = { "DEKI_PACKAGE_FEATURES_CONFIGURED", "DEKI_PACKAGE_INPUT",
                                                   "DEKI_FEATURE_INPUT" };
    EXPECT_EQ(CMakeGen::ResolveActivePackages(all, keyboardOff).count("deki-bus"), 0u);

    std::vector<std::string> keyboardOn = keyboardOff;
    keyboardOn.push_back("DEKI_FEATURE_KEYBOARD");
    EXPECT_EQ(CMakeGen::ResolveActivePackages(all, keyboardOn).count("deki-bus"), 1u);
}

TEST(FeatureRequires, WithoutAFeaturePlanEveryFeatureCounts)
{
    // The editor's own build compiles every feature in, so it needs them all.
    const auto all = InputWithBus(true);
    EXPECT_EQ(CMakeGen::ResolveActivePackages(all, { "DEKI_PACKAGE_INPUT" }).count("deki-bus"), 1u);
}

TEST(FeatureRequires, StrippingKeepsTheBusForAKeyboardInUseAndDropsItOtherwise)
{
    BuildStripSettings s;
    s.stripUnusedPackages = true;

    const StripPlan used = ResolveStripPlan(InputWithBus(true), Using({ "KeyboardComponent" }), s, "");
    const PackageDecision* bus = nullptr;
    for (const auto& p : used.packages)
        if (p.packageId == "deki-bus") bus = &p;
    ASSERT_NE(bus, nullptr);
    EXPECT_TRUE(bus->on) << bus->reason;

    const StripPlan unused = ResolveStripPlan(InputWithBus(true), Using({ "InputCollider" }), s, "");
    for (const auto& p : unused.packages)
        if (p.packageId == "deki-bus") EXPECT_FALSE(p.on) << "no keyboard in use, so nothing needs the bus";
}

TEST(FeatureRequires, AFeatureInUseWithoutItsPackageIsReported)
{
    BuildStripSettings s;
    const StripPlan plan = ResolveStripPlan(InputWithBus(false), Using({ "KeyboardComponent" }), s, "");
    ASSERT_EQ(plan.warnings.size(), 1u);
    EXPECT_NE(plan.warnings[0].find("deki-bus"), std::string::npos);
    EXPECT_NE(plan.warnings[0].find("keyboard"), std::string::npos);

    const StripPlan notUsed = ResolveStripPlan(InputWithBus(false), Using({ "InputCollider" }), s, "");
    EXPECT_TRUE(notUsed.warnings.empty()) << "a stripped feature needs nothing";
}
