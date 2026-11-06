// A package says what it needs FROM a platform; a platform says what it offers.
// A package builds for a target when the target offers everything it needs.
//
// Matching requirements against capabilities, instead of listing platform
// names, lets a platform that no package has heard of work. An allowlist of
// names would refuse it; a denylist would admit it and fail at the compiler.
// Do not turn this back into a list of names.

#include <gtest/gtest.h>

#include <deki-editor/build/CMakeGenUtils.h>
#include <deki-editor/build/PlatformConfig.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace DekiEditor;
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
const std::set<std::string> kDesktop{
    "hosted", "posix-process", "filesystem", "threads", "sdl3", "framework:native"
};
const std::set<std::string> kBoard{ "filesystem", "threads", "spi", "wifi", "framework:espidf" };
const std::set<std::string> kThirdParty{ "filesystem", "threads", "sdl3", "gpu-gles2", "framework:acme-sdk" };

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

// A platform written after the package works with no change to the package.
TEST(PlatformCapability, UnknownPlatformThatOffersWhatIsNeededIsAccepted)
{
    const PackageEntry sdl3 = Pkg("deki-sdl3", "DEKI_PACKAGE_SDL3", { "sdl3" });
    EXPECT_TRUE(PackageSupportsPlatform(sdl3, kThirdParty));

    // One it cannot satisfy is still refused, instead of failing at the
    // compiler.
    const PackageEntry desktopHal = Pkg("deki-desktop-hal", "DEKI_PACKAGE_DESKTOP_HAL", { "hosted" });
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

// A board compiles the engine from a glob of its sources. The entry of a game
// run as a program (src/hosted/: main() and its file system) must stay out
// of it: the board has its own entry, and a second main() would not link.
TEST(PlatformCapability, ABoardsEngineSourcesLeaveOutTheHostedEntry)
{
    std::ostringstream out;
    EmitEngineCoreSourceCollection(out);
    EXPECT_NE(out.str().find(R"(list(FILTER DEKI_ENGINE_SOURCES EXCLUDE REGEX ".*/src/hosted/.*"))"), std::string::npos);
}

// "desktop-os" was renamed "hosted". A platform or package that still says
// the old name is read as the new one, so an old project keeps building, and
// what the editor saves is the new name.
TEST(PlatformCapability, TheOldNameOfHostedIsReadAsHosted)
{
    EXPECT_EQ(CurrentCapabilityName("desktop-os"), "hosted");
    EXPECT_EQ(CurrentCapabilityName("hosted"), "hosted");
    EXPECT_EQ(CurrentCapabilityName("wifi"), "wifi");

    PlatformConfig old;
    ASSERT_TRUE(PlatformFromJson(R"({"id":"old_sim","framework":"native","provides":["desktop-os","sdl3"]})", old));
    EXPECT_EQ(old.provides, (std::vector<std::string>{ "hosted", "sdl3" }));

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "deki-capability-rename";
    fs::remove_all(root);
    fs::create_directories(root / "packages" / "old-desktop");
    std::ofstream(root / "packages" / "old-desktop" / "package.json")
        << R"({"id":"old-desktop","buildDefine":"DEKI_PACKAGE_OLD","platformRequires":["desktop-os"]})";
    const std::vector<PackageEntry> scanned = ScanPackageManifests(root.string());
    fs::remove_all(root);
    ASSERT_EQ(scanned.size(), 1u);
    EXPECT_EQ(scanned[0].platformRequires, (std::vector<std::string>{ "hosted" }));
    EXPECT_TRUE(PackageSupportsPlatform(scanned[0], kDesktop));
}

// The capability defines are the packages' to name: the editor turns what a
// package lists in capabilityDefines into DEKI_CAPABILITY_<NAME> where the
// platform offers it, and names no rendering backend itself.
TEST(PlatformCapability, ACapabilityDefineIsOneAPackageNamesAndThePlatformOffers)
{
    PackageEntry rendering = Pkg("deki-rendering", "DEKI_PACKAGE_RENDERING", {});
    rendering.capabilityDefines = { "opengl" };
    PackageEntry other = Pkg("deki-gpu", "DEKI_PACKAGE_GPU", {});
    other.capabilityDefines = { "gpu-gles2", "opengl" };

    std::set<std::string> desktop = kDesktop;
    desktop.insert("opengl");
    EXPECT_EQ(CapabilityDefinesFor(desktop, { rendering, other }),
              (std::vector<std::string>{ "DEKI_CAPABILITY_OPENGL" }));
    EXPECT_EQ(CapabilityDefinesFor(kThirdParty, { rendering, other }),
              (std::vector<std::string>{ "DEKI_CAPABILITY_GPU_GLES2" }));
    EXPECT_TRUE(CapabilityDefinesFor(kBoard, { rendering, other }).empty()) << "a board's build is unchanged";
    EXPECT_TRUE(CapabilityDefinesFor(desktop, {}).empty()) << "no package names opengl, so nothing defines it";
}

TEST(PlatformCapability, APackageSaysWhichCapabilitiesItDefinesAndFiltersTexturesWith)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "deki-capability-defines";
    fs::remove_all(root);
    fs::create_directories(root / "packages" / "gpu");
    std::ofstream(root / "packages" / "gpu" / "package.json")
        << R"({"id":"gpu","buildDefine":"DEKI_PACKAGE_GPU","capabilityDefines":["opengl"],)"
           R"("filtersTexturesWith":["opengl"]})";
    const std::vector<PackageEntry> scanned = ScanPackageManifests(root.string());
    fs::remove_all(root);
    ASSERT_EQ(scanned.size(), 1u);
    EXPECT_EQ(scanned[0].capabilityDefines, (std::vector<std::string>{ "opengl" }));
    EXPECT_EQ(scanned[0].filtersTexturesWith, (std::vector<std::string>{ "opengl" }));
    EXPECT_EQ(TextureFilteringCapabilities(scanned), (std::set<std::string>{ "opengl" }));
}

namespace
{
void WriteFile(const std::filesystem::path& file, const std::string& text)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}
}  // namespace

// A package from before capabilityDefines (an older deki-rendering, someone's
// own GPU package) still tests the define in its code. The build reads which
// ones from its game code, so it keeps building as it did.
TEST(PlatformCapability, ADefineAPackagesCodeTestsWithoutDeclaringIsFound)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "deki-undeclared-define";
    fs::remove_all(dir);
    WriteFile(dir / "GLThing.cpp", "#ifdef DEKI_CAPABILITY_OPENGL\nint gl;\n#endif\n#if DEKI_CAPABILITY_GPU_GLES2\n#endif\n");
    WriteFile(dir / "editor" / "Tool.cpp", "#ifdef DEKI_CAPABILITY_VULKAN\n#endif\n");
    WriteFile(dir / "tests" / "T.cpp", "#ifdef DEKI_CAPABILITY_METAL\n#endif\n");
    const std::set<std::string> tested = CapabilityDefinesTestedBy(dir);
    fs::remove_all(dir);
    EXPECT_EQ(tested, (std::set<std::string>{ "DEKI_CAPABILITY_GPU_GLES2", "DEKI_CAPABILITY_OPENGL" }))
        << "editor/ and tests/ are not the game's code";

    PackageEntry old = Pkg("old-gl", "DEKI_PACKAGE_OLD_GL", {});
    std::set<std::string> desktop = kDesktop;
    desktop.insert("opengl");
    EXPECT_EQ(UndeclaredCapabilities(desktop, old, tested), (std::vector<std::string>{ "opengl" }));
    EXPECT_EQ(UndeclaredCapabilities(kThirdParty, old, tested), (std::vector<std::string>{ "gpu-gles2" }));
    EXPECT_TRUE(UndeclaredCapabilities(kBoard, old, tested).empty()) << "a board offers neither";
    old.capabilityDefines = { "opengl" };
    EXPECT_TRUE(UndeclaredCapabilities(desktop, old, tested).empty()) << "declared, so nothing to warn of";
    EXPECT_EQ(CapabilityDefineName("gpu-gles2"), "DEKI_CAPABILITY_GPU_GLES2");
}

// A package with a main() of its own (deki-desktop-integration before this
// release, a fork of it) would give a game built as a program two; the build
// names the file instead of the linker's "multiple definition".
TEST(PlatformCapability, APackageThatDefinesMainIsFound)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "deki-package-main";
    fs::remove_all(dir);
    // Renamed by the upgrade, so compiled where the build defines DEKI_HOSTED.
    WriteFile(dir / "DesktopHALPackage.cpp", "#if defined(DEKI_HOSTED)\nint main(int argc, char* argv[])\n{\n}\n#endif\n");
    WriteFile(dir / "Other.cpp", "// int main( is only mentioned here\nint mainLoop();\n");
    WriteFile(dir / "C.cpp", "extern \"C\" int main (void) { return 0; }\n");
    WriteFile(dir / "editor" / "EditorMain.cpp", "int main() {}\n");
    // Only with SIMULATOR, which a build from this release never defines: a
    // package the upgrade did not touch, so its main() is left out.
    WriteFile(dir / "OldGuard.cpp", "#if defined(SIMULATOR)\nint main() {}\n#endif\n");
    WriteFile(dir / "OldIfdef.cpp", "#ifdef SIMULATOR // the old one\n#if 1\n#endif\nint main() {}\n#endif\n");
    // ...but its #else is compiled.
    WriteFile(dir / "OldElse.cpp", "#ifdef SIMULATOR\n#else\nint main() {}\n#endif\n");
    const std::vector<std::string> mains = SourcesDefiningMain(dir);
    fs::remove_all(dir);
    EXPECT_EQ(mains, (std::vector<std::string>{ "C.cpp", "DesktopHALPackage.cpp", "OldElse.cpp" }));
}
