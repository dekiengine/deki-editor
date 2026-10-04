// Choosing a build backend. The key is the platform's framework string. A
// platform no builder claims is an error; it never falls back to the first
// registered builder.

#include <gtest/gtest.h>

#include <deki-editor/build/BuilderDispatch.h>
#include <deki-editor/build/TargetBuilder.h>
#include <deki-editor/build/PlatformConfig.h>

using namespace DekiEditor;

namespace
{
// Only the name and framework matter here; the dispatcher must not call
// anything else.
class StubBuilder : public ITargetBuilder
{
public:
    StubBuilder(const char* name, std::string framework)
        : m_Name(name),
          m_Framework(std::move(framework))
    {
    }

    const char* GetName() const override { return m_Name; }
    std::string GetFrameworkId() const override { return m_Framework; }

    void Build(const std::string&, BuildOutputCallback, BuildProgressCallback) override {}
    bool SupportsDeploy() const override { return false; }
    void Clean(const std::string&, BuildOutputCallback, BuildProgressCallback) override {}
    void Cancel() override {}
    bool IsBuilding() const override { return false; }
    BuildState GetState() const override { return BuildState::Idle; }
    BuildProgress GetProgress() const override { return {}; }
    void SetBuildOptions(const BuildOptions&) override {}
    const BuildOptions& GetBuildOptions() const override { return m_Options; }
    void SetPlatformConfig(const PlatformConfig&) override {}
    void ClearPlatformConfig() override {}
    void SetPackageDefines(const std::vector<std::string>&) override {}
    bool IsToolchainInstalled() const override { return true; }
    std::string GetToolchainStatus() const override { return "stub"; }
    void InstallToolchainComponent(const std::string&, BuildProgressCallback) override {}
    std::string GetEnginePath(const std::string&) const override { return ""; }
    bool GenerateBuildFiles(const std::string&, const PlatformConfig&, const std::vector<std::string>&) override
    {
        return true;
    }
    std::string GetBuildDirectory(const std::string&) const override { return ""; }
    std::string GetBootPayloadDirectory(const std::string&) const override { return ""; }
    std::string GetPlatformKey() const override { return m_Framework; }

private:
    const char* m_Name;
    std::string m_Framework;
    BuildOptions m_Options;
};
}  // namespace

TEST(BuilderDispatch, PicksTheBuilderClaimingTheFramework)
{
    StubBuilder native("Native", "native");
    StubBuilder espidf("ESP-IDF", "espidf");
    const std::vector<ITargetBuilder*> all{ &native, &espidf };

    EXPECT_EQ(SelectBuilder(all, "espidf"), &espidf);
    EXPECT_EQ(SelectBuilder(all, "native"), &native);
}

TEST(BuilderDispatch, TwoBuildersForTheSameSiliconAreToldApart)
{
    // A Tactility board is an ESP32-S3, so its platform and an ESP-IDF
    // platform share family "espressif". Keyed on family, the first
    // registered builder would win and a Tactility app would build as
    // firmware.
    StubBuilder espidf("ESP-IDF", "espidf");
    StubBuilder tactility("Tactility", "tactility");
    const std::vector<ITargetBuilder*> all{ &espidf, &tactility };

    EXPECT_EQ(SelectBuilder(all, "tactility"), &tactility);
    EXPECT_EQ(SelectBuilder(all, "espidf"), &espidf);
}

TEST(BuilderDispatch, AnUnclaimedFrameworkIsNullNotAGuess)
{
    StubBuilder espidf("ESP-IDF", "espidf");
    const std::vector<ITargetBuilder*> all{ &espidf };

    EXPECT_EQ(SelectBuilder(all, "tactility"), nullptr);
}

TEST(BuilderDispatch, AnUnsetFrameworkMatchesNothing)
{
    // Even with only one builder registered: a misconfigured platform must
    // not build as something else.
    StubBuilder espidf("ESP-IDF", "espidf");
    EXPECT_EQ(SelectBuilder({ &espidf }, ""), nullptr);
}

TEST(BuilderDispatch, ANullBuilderInTheListIsSkipped)
{
    StubBuilder espidf("ESP-IDF", "espidf");
    const std::vector<ITargetBuilder*> all{ nullptr, &espidf };
    EXPECT_EQ(SelectBuilder(all, "espidf"), &espidf);
}

TEST(BuilderDispatch, TheErrorNamesWhatWasAskedForAndWhatExists)
{
    StubBuilder native("Native", "native");
    StubBuilder espidf("ESP-IDF", "espidf");
    const std::string msg = DescribeNoBuilder({ &native, &espidf }, "tactility");

    EXPECT_NE(msg.find("tactility"), std::string::npos);
    EXPECT_NE(msg.find("native"), std::string::npos);
    EXPECT_NE(msg.find("espidf"), std::string::npos);
}

TEST(BuilderDispatch, TheErrorSaysSoWhenNothingIsRegisteredAtAll)
{
    const std::string msg = DescribeNoBuilder({}, "espidf");
    EXPECT_NE(msg.find("No build backends are registered"), std::string::npos);
}
