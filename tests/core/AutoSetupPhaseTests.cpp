// When a package's automatic SetupComponent runs.
//
// There are two answers and the difference is user-visible. An EditorLoad setup
// fires as soon as packages finish loading, which is every project open and
// every hot reload. A PlayStart setup waits for the user to press Play.
//
// The distinction exists because deki-gps geolocates by asking a third party
// where the machine is. Registered as EditorLoad, merely opening a project sent
// that request, which nobody had asked for. Registered as PlayStart, it happens
// when the user asks for the game to run.
//
// These tests pin the registry's behaviour: a phase fires only its own entries,
// each entry fires once, and a hot reload (clear then re-register) lets an entry
// fire again.

#include <gtest/gtest.h>

#include <deki/SetupComponent.h>

#include <string>
#include <vector>

using Deki::SetupComponent;
using Phase = Deki::SetupComponent::SetupPhase;

namespace
{
// Every construction and Setup() call, in order, so a test can assert on both
// what ran and how often.
std::vector<std::string>& Ran()
{
    static std::vector<std::string> v;
    return v;
}

template <const char* Name>
class RecordingSetup : public SetupComponent
{
public:
    void Setup(SetupCallback onComplete) override
    {
        Ran().push_back(Name);
        if (onComplete)
        {
            onComplete(true);
        }
    }
    const char* GetSetupName() const override { return Name; }
};

constexpr char kOnLoad[] = "on-load";
constexpr char kOnPlay[] = "on-play";

using LoadSetup = RecordingSetup<kOnLoad>;
using PlaySetup = RecordingSetup<kOnPlay>;

int CountOf(const char* name)
{
    int n = 0;
    for (const auto& s : Ran())
    {
        if (s == name)
        {
            ++n;
        }
    }
    return n;
}

class AutoSetupPhase : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // The registry is process-wide and other suites may have touched it.
        SetupComponent::ClearEditorAutoFactories();
        Ran().clear();
    }
    void TearDown() override { SetupComponent::ClearEditorAutoFactories(); }

    static void RegisterBoth()
    {
        SetupComponent::RegisterEditorAutoFactory([]() -> SetupComponent* { return new LoadSetup(); },
                                                  Phase::EditorLoad);
        SetupComponent::RegisterEditorAutoFactory([]() -> SetupComponent* { return new PlaySetup(); },
                                                  Phase::PlayStart);
    }
};
}  // namespace

TEST_F(AutoSetupPhase, LoadingPackagesRunsOnlyTheEditorLoadSetups)
{
    RegisterBoth();
    SetupComponent::RunEditorAutoSetups(Phase::EditorLoad);

    EXPECT_EQ(CountOf(kOnLoad), 1);
    EXPECT_EQ(CountOf(kOnPlay), 0) << "opening a project must not run a Play setup";
}

TEST_F(AutoSetupPhase, PressingPlayRunsOnlyThePlaySetups)
{
    RegisterBoth();
    SetupComponent::RunEditorAutoSetups(Phase::PlayStart);

    EXPECT_EQ(CountOf(kOnPlay), 1);
    EXPECT_EQ(CountOf(kOnLoad), 0);
}

TEST_F(AutoSetupPhase, TheUsualOrderRunsEachExactlyOnce)
{
    // What actually happens: packages load, then the user presses Play.
    RegisterBoth();
    SetupComponent::RunEditorAutoSetups(Phase::EditorLoad);
    SetupComponent::RunEditorAutoSetups(Phase::PlayStart);

    EXPECT_EQ(CountOf(kOnLoad), 1);
    EXPECT_EQ(CountOf(kOnPlay), 1);
}

TEST_F(AutoSetupPhase, PlayingAgainDoesNotRunItAgain)
{
    // Stop and Play a second time must not repeat the side effect - for
    // deki-gps that would be another request.
    RegisterBoth();
    SetupComponent::RunEditorAutoSetups(Phase::PlayStart);
    SetupComponent::RunEditorAutoSetups(Phase::PlayStart);
    SetupComponent::RunEditorAutoSetups(Phase::PlayStart);

    EXPECT_EQ(CountOf(kOnPlay), 1);
}

TEST_F(AutoSetupPhase, RepeatedPackageLoadsDoNotRerunAnEditorSetup)
{
    // LoadPackagesFromDirectory runs twice per open, once per source directory.
    RegisterBoth();
    SetupComponent::RunEditorAutoSetups(Phase::EditorLoad);
    SetupComponent::RunEditorAutoSetups(Phase::EditorLoad);

    EXPECT_EQ(CountOf(kOnLoad), 1);
}

TEST_F(AutoSetupPhase, AHotReloadLetsASetupRunAgain)
{
    // A package DLL unloading clears the registry and reloading re-registers,
    // which must arm the entry again: its driver went with the old DLL.
    RegisterBoth();
    SetupComponent::RunEditorAutoSetups(Phase::EditorLoad);
    ASSERT_EQ(CountOf(kOnLoad), 1);

    SetupComponent::ClearEditorAutoFactories();
    RegisterBoth();
    SetupComponent::RunEditorAutoSetups(Phase::EditorLoad);

    EXPECT_EQ(CountOf(kOnLoad), 2);
    EXPECT_EQ(CountOf(kOnPlay), 0) << "a reload outside Play must not start a Play setup";
}

TEST_F(AutoSetupPhase, RegisteringWithoutAPhaseMeansEditorLoad)
{
    // The default keeps every package that predates the split working, and
    // matches what DEKI_REGISTER_EDITOR_AUTO_SETUP expands to.
    SetupComponent::RegisterEditorAutoFactory([]() -> SetupComponent* { return new LoadSetup(); });

    SetupComponent::RunEditorAutoSetups(Phase::PlayStart);
    EXPECT_EQ(CountOf(kOnLoad), 0);

    SetupComponent::RunEditorAutoSetups(Phase::EditorLoad);
    EXPECT_EQ(CountOf(kOnLoad), 1);
}

TEST_F(AutoSetupPhase, ANullFactoryIsIgnored)
{
    SetupComponent::RegisterEditorAutoFactory(nullptr, Phase::PlayStart);
    RegisterBoth();
    EXPECT_NO_THROW(SetupComponent::RunEditorAutoSetups(Phase::PlayStart));
    EXPECT_EQ(CountOf(kOnPlay), 1);
}
