// Platform templates: a package offers a platform (its config and boot scene),
// and a project adopts it by copying it in.
//
// The editor knows no target by name. None of the tests below names a real
// target except the last, which checks the templates the packages ship.

#include <gtest/gtest.h>

#include <deki-editor/Paths.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/build/PlatformTemplates.h>
#include <deki/TypeId.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace DekiEditor;
using nlohmann::json;
namespace fs = std::filesystem;

namespace
{

struct TempProject
{
    fs::path dir;
    TempProject()
    {
        dir = fs::temp_directory_path() / "deki_platform_template_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
    }
    ~TempProject()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string Path() const { return dir.string(); }

    void Write(const fs::path& absolute, const std::string& text) const
    {
        fs::create_directories(absolute.parent_path());
        std::ofstream(absolute, std::ios::binary) << text;
    }
    std::string Read(const fs::path& absolute) const
    {
        std::ifstream f(absolute, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    void OfferTemplate(const std::string& package, const std::string& platformId, const std::string& marker) const
    {
        const fs::path t = fs::path(GetPackagesDirectory(Path())) / package / "platforms" / platformId;
        Write(t / (platformId + ".json"), "{\"id\":\"" + platformId + "\",\"from\":\"" + marker + "\"}");
        Write(t / "boot.scene", "scene from " + marker);
    }
    fs::path ProjectPlatform(const std::string& platformId) const
    {
        return fs::path(GetPlatformsDirectory(Path())) / platformId;
    }
};

}  // namespace

TEST(PlatformTemplates, APackageOffersAndTheProjectAdopts)
{
    TempProject p;
    p.OfferTemplate("pkg-a", "widget_devkit", "a");

    const auto offers = FindPlatformTemplates(p.Path());
    ASSERT_EQ(offers.size(), 1u);
    EXPECT_EQ(offers[0].platformId, "widget_devkit");
    EXPECT_EQ(offers[0].packageId, "pkg-a");

    std::string detail;
    EXPECT_EQ(AdoptPlatformTemplate(p.Path(), "widget_devkit", detail), AdoptPlatformResult::Adopted);
    EXPECT_EQ(detail, "pkg-a");
    EXPECT_TRUE(fs::exists(p.ProjectPlatform("widget_devkit") / "widget_devkit.json"));
    EXPECT_EQ(p.Read(p.ProjectPlatform("widget_devkit") / "boot.scene"), "scene from a");
}

TEST(PlatformTemplates, AProjectsOwnPlatformIsNeverOverwritten)
{
    TempProject p;
    p.OfferTemplate("pkg-a", "widget_devkit", "a");
    p.Write(p.ProjectPlatform("widget_devkit") / "widget_devkit.json", "{\"id\":\"widget_devkit\",\"mine\":true}");

    std::string detail;
    EXPECT_EQ(AdoptPlatformTemplate(p.Path(), "widget_devkit", detail), AdoptPlatformResult::AlreadyDefined);
    EXPECT_NE(p.Read(p.ProjectPlatform("widget_devkit") / "widget_devkit.json").find("mine"), std::string::npos);
    // Not even the files it lacks: a project that defines the platform owns
    // the whole directory.
    EXPECT_FALSE(fs::exists(p.ProjectPlatform("widget_devkit") / "boot.scene"));
}

TEST(PlatformTemplates, FilesAlreadyThereSurviveAnAdoption)
{
    TempProject p;
    p.OfferTemplate("pkg-a", "widget_devkit", "a");
    p.Write(p.ProjectPlatform("widget_devkit") / "boot.scene", "my own boot scene");

    std::string detail;
    ASSERT_EQ(AdoptPlatformTemplate(p.Path(), "widget_devkit", detail), AdoptPlatformResult::Adopted);
    EXPECT_EQ(p.Read(p.ProjectPlatform("widget_devkit") / "boot.scene"), "my own boot scene");
}

TEST(PlatformTemplates, NothingIsInventedWhenNobodyOffersIt)
{
    TempProject p;
    p.OfferTemplate("pkg-a", "widget_devkit", "a");

    std::string detail;
    EXPECT_EQ(AdoptPlatformTemplate(p.Path(), "some_other_target", detail), AdoptPlatformResult::NoTemplate);
    EXPECT_FALSE(fs::exists(p.ProjectPlatform("some_other_target")));
}

TEST(PlatformTemplates, TwoOffersAreAConflictNotACoinToss)
{
    TempProject p;
    p.OfferTemplate("pkg-a", "widget_devkit", "a");
    p.OfferTemplate("pkg-b", "widget_devkit", "b");

    std::string detail;
    EXPECT_EQ(AdoptPlatformTemplate(p.Path(), "widget_devkit", detail), AdoptPlatformResult::Ambiguous);
    EXPECT_NE(detail.find("pkg-a"), std::string::npos);
    EXPECT_NE(detail.find("pkg-b"), std::string::npos);
    EXPECT_FALSE(fs::exists(p.ProjectPlatform("widget_devkit")));
}

TEST(PlatformTemplates, APlatformIdIsNotAPath)
{
    TempProject p;
    std::string detail;
    EXPECT_EQ(AdoptPlatformTemplate(p.Path(), "../escape", detail), AdoptPlatformResult::Failed);
}

TEST(PlatformTemplates, AReloadFollowsThePackagesInstalled)
{
    TempProject p;
    auto& registry = PlatformRegistry::Instance();

    // Installed under an open project: its board is on offer at once.
    p.OfferTemplate("pkg-a", "widget_devkit", "a");
    ReloadProjectPlatforms(p.Path());
    EXPECT_NE(registry.GetPlatform("widget_devkit"), nullptr);

    // Removed again, never adopted: the offer goes with it.
    std::error_code ec;
    fs::remove_all(fs::path(GetPackagesDirectory(p.Path())) / "pkg-a", ec);
    ReloadProjectPlatforms(p.Path());
    EXPECT_EQ(registry.GetPlatform("widget_devkit"), nullptr);

    // Adopted, then removed: the project's copy is the project's.
    p.OfferTemplate("pkg-a", "widget_devkit", "a");
    std::string detail;
    ASSERT_EQ(AdoptPlatformTemplate(p.Path(), "widget_devkit", detail), AdoptPlatformResult::Adopted);
    fs::remove_all(fs::path(GetPackagesDirectory(p.Path())) / "pkg-a", ec);
    ReloadProjectPlatforms(p.Path());
    EXPECT_NE(registry.GetPlatform("widget_devkit"), nullptr);

    registry.Clear();
}

// ---------------------------------------------------------------------------
// An adopted platform remembers where it came from: it can be edited, take the
// package's later changes, and go back to the package's version.
// ---------------------------------------------------------------------------

namespace
{

struct BoardProject : TempProject
{
    fs::path TemplateDir() const
    {
        return fs::path(GetPackagesDirectory(Path())) / "pkg-board" / "platforms" / "devkit";
    }
    fs::path Mine() const { return ProjectPlatform("devkit"); }

    // What the package ships. Older spelling on purpose: "pins" sits at the
    // top level, and a save moves it into frameworkOptions.
    void Ship(int width, const std::string& pins, const std::string& bootScene) const
    {
        json j = { { "id", "devkit" },       { "displayName", "Dev Kit" }, { "framework", "demo" },
                   { "screenWidth", width }, { "screenHeight", 240 },      { "pins", pins } };
        Write(TemplateDir() / "devkit.json", j.dump(2));
        Write(TemplateDir() / "boot.scene", bootScene);
    }

    void Adopt() const
    {
        std::string detail;
        ASSERT_EQ(AdoptPlatformTemplate(Path(), "devkit", detail), AdoptPlatformResult::Adopted);
    }

    PlatformConfig LoadMine() const
    {
        PlatformConfig config;
        EXPECT_TRUE(PlatformFromJson(Read(Mine() / "devkit.json"), config));
        return config;
    }
    void SaveMine(const PlatformConfig& config) const
    {
        ASSERT_TRUE(PlatformRegistry::Instance().SavePlatformToFile(config, GetPlatformsDirectory(Path())));
    }

    ~BoardProject() { PlatformRegistry::Instance().Clear(); }
};

bool Names(const std::vector<std::string>& list, const std::string& name)
{
    return std::find(list.begin(), list.end(), name) != list.end();
}

}  // namespace

TEST(PlatformOrigin, AnAdoptedPlatformKnowsItsPackage)
{
    BoardProject p;
    p.Ship(320, "a", "scene 1");
    p.Adopt();

    const PlatformOriginStatus s = GetPlatformOrigin(p.Path(), "devkit");
    EXPECT_TRUE(s.fromPackage);
    EXPECT_TRUE(s.recorded);
    EXPECT_TRUE(s.packageInstalled);
    EXPECT_EQ(s.packageId, "pkg-board");
    EXPECT_FALSE(s.modified);
    EXPECT_FALSE(s.updateAvailable);
    EXPECT_FALSE(s.CanUpdate());
    EXPECT_FALSE(s.CanRestore());
}

TEST(PlatformOrigin, AProjectsOwnPlatformHasNoOrigin)
{
    BoardProject p;
    p.Write(p.ProjectPlatform("handmade") / "handmade.json", "{\"id\":\"handmade\"}");
    EXPECT_FALSE(GetPlatformOrigin(p.Path(), "handmade").fromPackage);
}

TEST(PlatformOrigin, ASaveThatChangesNothingIsNotAnEdit)
{
    BoardProject p;
    p.Ship(320, "a", "scene 1");
    p.Adopt();

    // The editor rewrites the file in its own form: "pins" moves into
    // frameworkOptions, defaults appear. None of that is the user's doing.
    p.SaveMine(p.LoadMine());
    EXPECT_NE(p.Read(p.Mine() / "devkit.json"), p.Read(p.TemplateDir() / "devkit.json"));
    EXPECT_FALSE(GetPlatformOrigin(p.Path(), "devkit").modified);
}

TEST(PlatformOrigin, LineEndingsAreNotAnEdit)
{
    BoardProject p;
    p.Ship(320, "a", "line one\nline two\n");
    p.Adopt();
    p.Write(p.Mine() / "boot.scene", "line one\r\nline two\r\n");
    EXPECT_FALSE(GetPlatformOrigin(p.Path(), "devkit").modified);
}

TEST(PlatformOrigin, EditsAreNamed)
{
    BoardProject p;
    p.Ship(320, "a", "scene 1");
    p.Adopt();

    PlatformConfig mine = p.LoadMine();
    mine.screenWidth = 480;
    mine.SetOption("pins", "mine");
    p.SaveMine(mine);
    p.Write(p.Mine() / "boot.scene", "my scene");

    const PlatformOriginStatus s = GetPlatformOrigin(p.Path(), "devkit");
    EXPECT_TRUE(s.modified);
    EXPECT_TRUE(Names(s.modifiedItems, "screenWidth"));
    EXPECT_TRUE(Names(s.modifiedItems, "pins"));
    EXPECT_TRUE(Names(s.modifiedItems, "boot.scene"));
    EXPECT_FALSE(s.updateAvailable);
    EXPECT_TRUE(s.CanRestore());
}

TEST(PlatformOrigin, AnUpdateTakesThePackagesChangesAndKeepsTheProjects)
{
    BoardProject p;
    p.Ship(320, "a", "scene 1");
    p.Adopt();

    // The project changes the width; the package fixes the pins and the scene.
    PlatformConfig mine = p.LoadMine();
    mine.screenWidth = 480;
    p.SaveMine(mine);
    p.Ship(320, "fixed", "scene 2");

    PlatformOriginStatus s = GetPlatformOrigin(p.Path(), "devkit");
    EXPECT_TRUE(s.modified);
    ASSERT_TRUE(s.CanUpdate());
    EXPECT_TRUE(Names(s.updatedItems, "pins"));
    EXPECT_TRUE(Names(s.updatedItems, "boot.scene"));

    const PlatformSyncResult r = UpdatePlatformFromPackage(p.Path(), "devkit");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(Names(r.taken, "pins"));
    EXPECT_TRUE(Names(r.taken, "boot.scene"));
    EXPECT_TRUE(r.kept.empty());

    const PlatformConfig after = p.LoadMine();
    EXPECT_EQ(after.screenWidth, 480);
    EXPECT_EQ(after.Option("pins"), "fixed");
    EXPECT_EQ(p.Read(p.Mine() / "boot.scene"), "scene 2");

    // The registry has it too: a build started now sees the update.
    const PlatformConfig* registered = PlatformRegistry::Instance().GetPlatform("devkit");
    ASSERT_NE(registered, nullptr);
    EXPECT_EQ(registered->Option("pins"), "fixed");

    s = GetPlatformOrigin(p.Path(), "devkit");
    EXPECT_FALSE(s.updateAvailable);
    EXPECT_TRUE(s.modified) << "the width is still the project's own";
    EXPECT_EQ(s.modifiedItems, std::vector<std::string>{ "screenWidth" });
}

TEST(PlatformOrigin, WhereBothChangedTheProjectsIsKeptAndNamed)
{
    BoardProject p;
    p.Ship(320, "a", "scene 1");
    p.Adopt();

    PlatformConfig mine = p.LoadMine();
    mine.SetOption("pins", "mine");
    p.SaveMine(mine);
    p.Write(p.Mine() / "boot.scene", "my scene");
    p.Ship(320, "theirs", "scene 2");

    const PlatformSyncResult r = UpdatePlatformFromPackage(p.Path(), "devkit");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(Names(r.kept, "pins"));
    EXPECT_TRUE(Names(r.kept, "boot.scene"));
    EXPECT_EQ(p.LoadMine().Option("pins"), "mine");
    EXPECT_EQ(p.Read(p.Mine() / "boot.scene"), "my scene");

    // Asked once, not on every look: the package's version is now the base.
    EXPECT_FALSE(GetPlatformOrigin(p.Path(), "devkit").updateAvailable);
}

TEST(PlatformOrigin, AnUpdateBringsAFileThePackageAdded)
{
    BoardProject p;
    p.Ship(320, "a", "scene 1");
    p.Adopt();
    p.Write(p.TemplateDir() / "extra" / "pins.md", "notes");

    ASSERT_TRUE(GetPlatformOrigin(p.Path(), "devkit").CanUpdate());
    ASSERT_TRUE(UpdatePlatformFromPackage(p.Path(), "devkit").ok);
    EXPECT_EQ(p.Read(p.Mine() / "extra" / "pins.md"), "notes");
}

TEST(PlatformOrigin, RestoreIsThePackagesVersionAndLeavesTheProjectsOwnFiles)
{
    BoardProject p;
    p.Ship(320, "a", "scene 1");
    p.Adopt();

    PlatformConfig mine = p.LoadMine();
    mine.screenWidth = 480;
    mine.SetOption("pins", "mine");
    p.SaveMine(mine);
    p.Write(p.Mine() / "boot.scene", "my scene");
    p.Write(p.Mine() / "boot.scene.data", "sidecar");

    const PlatformSyncResult r = RestorePlatformFromPackage(p.Path(), "devkit");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(Names(r.taken, "screenWidth"));

    const PlatformConfig after = p.LoadMine();
    EXPECT_EQ(after.screenWidth, 320);
    EXPECT_EQ(after.Option("pins"), "a");
    EXPECT_EQ(p.Read(p.Mine() / "boot.scene"), "scene 1");
    EXPECT_EQ(p.Read(p.Mine() / "boot.scene.data"), "sidecar");

    const PlatformOriginStatus s = GetPlatformOrigin(p.Path(), "devkit");
    EXPECT_FALSE(s.modified);
    EXPECT_FALSE(s.CanRestore());
}

TEST(PlatformOrigin, WithThePackageGoneThePlatformStaysAndSaysWhy)
{
    BoardProject p;
    p.Ship(320, "a", "scene 1");
    p.Adopt();
    std::error_code ec;
    fs::remove_all(fs::path(GetPackagesDirectory(p.Path())) / "pkg-board", ec);

    const PlatformOriginStatus s = GetPlatformOrigin(p.Path(), "devkit");
    EXPECT_TRUE(s.fromPackage);
    EXPECT_EQ(s.packageId, "pkg-board");
    EXPECT_FALSE(s.packageInstalled);
    EXPECT_FALSE(s.CanUpdate());
    EXPECT_FALSE(s.CanRestore());

    const PlatformSyncResult r = RestorePlatformFromPackage(p.Path(), "devkit");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("pkg-board"), std::string::npos);
}

// A copy without an origin record that differs from the package.
TEST(PlatformOrigin, AnUnrecordedCopyThatDiffersCanOnlyBeRestored)
{
    BoardProject p;
    p.Ship(320, "a", "scene 1");
    p.Adopt();
    fs::remove(p.Mine() / "origin.json");
    PlatformConfig mine = p.LoadMine();
    mine.screenWidth = 480;
    p.SaveMine(mine);

    std::string err;
    ASSERT_TRUE(EnsureProjectPlatform(p.Path(), "devkit", err));

    // Nothing says whether 480 was the project's edit or the package's old
    // value, so no record is invented and no update is offered.
    const PlatformOriginStatus s = GetPlatformOrigin(p.Path(), "devkit");
    EXPECT_TRUE(s.fromPackage);
    EXPECT_FALSE(s.recorded);
    EXPECT_TRUE(s.modified);
    EXPECT_FALSE(s.CanUpdate());
    EXPECT_TRUE(s.CanRestore());
    EXPECT_FALSE(UpdatePlatformFromPackage(p.Path(), "devkit").ok);

    ASSERT_TRUE(RestorePlatformFromPackage(p.Path(), "devkit").ok);
    EXPECT_TRUE(GetPlatformOrigin(p.Path(), "devkit").recorded);
    EXPECT_EQ(p.LoadMine().screenWidth, 320);
}

// ---------------------------------------------------------------------------
// The templates the packages actually ship.
//
// A boot scene's PlatformSetupComponent refers to its setup components by
// {id, typeId}, where typeId is the hash of the component's qualified class
// name. It is a literal in a data file, so no rename updates it. An ObjectRef
// whose typeId matches no live component resolves to null: the setup step
// silently never runs and the target comes up with no display. This checks
// every package's boot scenes.
// ---------------------------------------------------------------------------

#ifdef DEKI_TEST_PACKAGES_DIR
namespace
{
std::string SoleComponentType(const json& object)
{
    const auto& components = object.at("components");
    EXPECT_EQ(components.size(), 1u) << "a boot scene's setup objects hold one component each";
    return components.at(0).value("type", std::string());
}

const json* ChildWithId(const json& parent, uint32_t id)
{
    for (const auto& child : parent.at("children"))
    {
        if (child.value("id", 0u) == id)
        {
            return &child;
        }
    }
    return nullptr;
}
}  // namespace

TEST(ShippedPlatformTemplates, EveryBootSceneTypeIdIsTheHashOfTheNameItPointsAt)
{
    int checked = 0;
    std::error_code ec;
    for (const auto& package : fs::directory_iterator(DEKI_TEST_PACKAGES_DIR, ec))
    {
        const fs::path platforms = package.path() / "platforms";
        if (!fs::is_directory(platforms, ec))
        {
            continue;
        }
        for (const auto& platform : fs::directory_iterator(platforms, ec))
        {
            const fs::path bootScene = platform.path() / "boot.scene";
            if (!fs::is_regular_file(bootScene, ec))
            {
                continue;
            }
            SCOPED_TRACE(bootScene.string());

            std::ifstream f(bootScene);
            json scene;
            ASSERT_NO_THROW(scene = json::parse(f));
            const json& setup = scene.at("objects").at(0);
            EXPECT_NE(setup.at("components").at(0).value("type", std::string()).find("::"), std::string::npos)
                << "scene files store the qualified class name";

            const json& refs = setup.at("components").at(0).at("properties").at("setupComponents");
            ASSERT_FALSE(refs.empty()) << "this platform boots nothing at all";
            for (const auto& ref : refs)
            {
                const uint32_t id = ref.at("id").get<uint32_t>();
                const json* child = ChildWithId(setup, id);
                ASSERT_NE(child, nullptr) << "no child object with id " << id;
                const std::string type = SoleComponentType(*child);
                ASSERT_FALSE(type.empty());
                EXPECT_NE(type.find("::"), std::string::npos) << "'" << type << "' is a bare class name";
                EXPECT_EQ(ref.at("typeId").get<uint32_t>(), Deki::HashString(type.c_str()))
                    << "setupComponents entry for object " << id << " does not carry the hash of '" << type << "' ("
                    << Deki::HashString(type.c_str()) << "); this setup step would silently never run";
                ++checked;
            }
        }
    }
    EXPECT_GT(checked, 0) << "no package under " << DEKI_TEST_PACKAGES_DIR << " ships a boot scene";
}
#endif
