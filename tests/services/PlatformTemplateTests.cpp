// Platform templates: a package offers a platform (its config and boot scene),
// and a project adopts it by copying it in.
//
// The editor used to hold one of these in its own source - the desktop
// simulator's config and a boot scene naming SDL3 components - which made that
// the one target it knew by name. None of the tests below names a real target
// except the last, which checks the templates the packages actually ship.

#include <gtest/gtest.h>

#include <deki-editor/Paths.h>
#include <deki-editor/build/PlatformTemplates.h>
#include <deki/TypeId.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

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

// ---------------------------------------------------------------------------
// The templates the packages actually ship.
//
// A boot scene's PlatformSetupComponent refers to its setup components by
// {id, typeId}, where typeId is the hash of the component's qualified class
// name. It is a literal in a data file, so no rename updates it, and an
// ObjectRef whose typeId matches no live component resolves to null: the setup
// step silently never runs and the target comes up with no display. This
// check used to cover the one boot scene the editor carried; it covers every
// package's now.
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
        if (child.value("id", 0u) == id)
            return &child;
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
            continue;
        for (const auto& platform : fs::directory_iterator(platforms, ec))
        {
            const fs::path bootScene = platform.path() / "boot.scene";
            if (!fs::is_regular_file(bootScene, ec))
                continue;
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
                    << "setupComponents entry for object " << id << " does not carry the hash of '" << type
                    << "' (" << Deki::HashString(type.c_str()) << "); this setup step would silently never run";
                ++checked;
            }
        }
    }
    EXPECT_GT(checked, 0) << "no package under " << DEKI_TEST_PACKAGES_DIR << " ships a boot scene";
}
#endif
