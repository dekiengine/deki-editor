// A package says what it needs FROM a platform; a platform says what it offers.
// A package builds for a target when the target offers everything it needs.
//
// The point of matching requirements against capabilities, rather than listing
// platform names a package approves of, is that the engine is modular enough
// that platforms appear which no existing package has heard of. An allowlist
// refuses those for no better reason than that their name is not on a list
// written earlier; a denylist admits them and fails at the compiler. These
// tests pin the open-world behaviour, because it is the property most easily
// lost by someone "simplifying" this back into a list of names.

#include <gtest/gtest.h>

#include "services/builders/CMakeGenUtils.h"

#include <set>
#include <string>
#include <vector>

using namespace DekiEditor::CMakeGen;

namespace
{

PackageEntry Pkg(const char* id, const char* buildDefine, std::vector<std::string> needs,
                 std::vector<std::string> deps = {})
{
    PackageEntry p;
    p.id = id;
    p.buildDefine = buildDefine;
    p.platformRequires = std::move(needs);
    p.dekiDeps = std::move(deps);
    return p;
}

// A desktop, an ESP32 board, and a board from a vendor this code has never
// heard of which nevertheless ships SDL3.
const std::set<std::string> kDesktop{ "desktop-os", "posix-process", "filesystem",
                                      "threads",    "sdl3",          "framework:native" };
const std::set<std::string> kBoard{ "filesystem", "threads", "spi", "wifi", "framework:espidf" };
const std::set<std::string> kThirdParty{ "filesystem", "threads", "sdl3", "gpu-gles2",
                                         "framework:acme-sdk" };

}  // namespace

TEST(PlatformCapability, NoRequirementsBuildsAnywhere)
{
    const PackageEntry anywhere = Pkg("deki-2d", "DEKI_PACKAGE_2D", {});
    EXPECT_TRUE(PackageSupportsPlatform(anywhere, kDesktop));
    EXPECT_TRUE(PackageSupportsPlatform(anywhere, kBoard));
    EXPECT_TRUE(PackageSupportsPlatform(anywhere, kThirdParty));
}

TEST(PlatformCapability, MissingCapabilityExcludesAndSaysWhich)
{
    const PackageEntry sdl3 = Pkg("deki-sdl3", "DEKI_PACKAGE_SDL3", { "sdl3" });
    EXPECT_TRUE(PackageSupportsPlatform(sdl3, kDesktop));
    EXPECT_FALSE(PackageSupportsPlatform(sdl3, kBoard));

    const std::vector<std::string> missing = MissingCapabilities(sdl3, kBoard);
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_EQ(missing[0], "sdl3");
}

// The reason this design exists: a platform written after the package still
// works, with no change to the package.
TEST(PlatformCapability, UnknownPlatformThatOffersWhatIsNeededIsAccepted)
{
    const PackageEntry sdl3 = Pkg("deki-sdl3", "DEKI_PACKAGE_SDL3", { "sdl3" });
    EXPECT_TRUE(PackageSupportsPlatform(sdl3, kThirdParty));

    // And one that genuinely cannot be satisfied is still refused, rather than
    // being admitted and failing at the compiler.
    const PackageEntry desktopHal = Pkg("deki-desktop-hal", "DEKI_PACKAGE_DESKTOP_HAL", { "desktop-os" });
    EXPECT_FALSE(PackageSupportsPlatform(desktopHal, kThirdParty));
}

TEST(PlatformCapability, NoCapabilitySetJudgesNothing)
{
    // The editor's own build has no target in hand and must load everything,
    // or a device component cannot be shown or edited on a desktop.
    const PackageEntry sdl3 = Pkg("deki-sdl3", "DEKI_PACKAGE_SDL3", { "sdl3" });
    EXPECT_TRUE(PackageSupportsPlatform(sdl3, {}));
}

TEST(PlatformCapability, ActiveSetDropsWhatTheTargetCannotSatisfy)
{
    const std::vector<PackageEntry> all{
        Pkg("deki-2d", "DEKI_PACKAGE_2D", {}),
        Pkg("deki-sdl3", "DEKI_PACKAGE_SDL3", { "sdl3" }),
    };
    const std::vector<std::string> defines{ "DEKI_PACKAGE_2D", "DEKI_PACKAGE_SDL3" };

    const std::set<std::string> onDesktop = ResolveActivePackages(all, defines, kDesktop);
    EXPECT_EQ(onDesktop.count("deki-sdl3"), 1u);

    const std::set<std::string> onBoard = ResolveActivePackages(all, defines, kBoard);
    EXPECT_EQ(onBoard.count("deki-2d"), 1u);
    EXPECT_EQ(onBoard.count("deki-sdl3"), 0u);
}

// dekiDeps absorbs names from every "dependencies" key regardless of which
// platform they were listed under, so a package that is itself buildable here
// must not drag in one that is not.
TEST(PlatformCapability, DependencyEdgesAreFilteredToo)
{
    const std::vector<PackageEntry> all{
        Pkg("deki-app", "DEKI_PACKAGE_APP", {}, { "deki-sdl3" }),
        Pkg("deki-sdl3", "DEKI_PACKAGE_SDL3", { "sdl3" }),
    };
    const std::vector<std::string> defines{ "DEKI_PACKAGE_APP" };

    EXPECT_EQ(ResolveActivePackages(all, defines, kDesktop).count("deki-sdl3"), 1u);
    EXPECT_EQ(ResolveActivePackages(all, defines, kBoard).count("deki-sdl3"), 0u);
}
