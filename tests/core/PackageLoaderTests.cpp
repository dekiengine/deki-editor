#include <gtest/gtest.h>
#include "services/PackageLoader.h"
#include "services/SharedLibNaming.h"
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/EditorExtension.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#include <string>

using namespace DekiEditor;
using namespace DekiEditorInternal;

// ============================================================================
// Mock for EditorRegistry tests
// ============================================================================

class MockAssetTypeEditorForLoader : public AssetTypeEditor
{
public:
    MockAssetTypeEditorForLoader(const std::string& typeName = "MockAsset")
        : m_TypeName(typeName) {}

    const char* GetTypeName() const override { return m_TypeName.c_str(); }
    const char* GetDisplayName() const override { return "Mock"; }
    const char* GetExtension() const override { return ".mock"; }
    const char* GetDefaultContent() const override { return "{}"; }
    int GetCompileTarget() const override { return 0; }

    bool Compile(const std::string&, std::vector<uint8_t>& rgba,
                 int& width, int& height) override
    {
        width = 1; height = 1;
        rgba.resize(4, 0);
        return true;
    }

    bool OnInspectorGUI(std::string&, const std::string&, const std::string&) override
    {
        return false;
    }

private:
    std::string m_TypeName;
};

// ============================================================================
// PackageLoader Unit Tests
// ============================================================================

class PackageLoaderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Loader = std::make_unique<PackageLoader>();
    }

    void TearDown() override
    {
        m_Loader.reset();
    }

    std::unique_ptr<PackageLoader> m_Loader;
};

// --- Initial State ---

TEST_F(PackageLoaderTest, InitialState_NoPackagesLoaded)
{
    EXPECT_TRUE(m_Loader->GetLoadedPackages().empty());
}

TEST_F(PackageLoaderTest, InitialState_EmptyPackageDirectory)
{
    EXPECT_TRUE(m_Loader->GetPackageDirectory().empty());
}

// --- LoadPackagesFromDirectory ---

TEST_F(PackageLoaderTest, LoadFromNonExistentDirectory_ReturnsZero)
{
    int count = m_Loader->LoadPackagesFromDirectory("C:/nonexistent_path_xyz_12345", nullptr);
    EXPECT_EQ(count, 0);
    EXPECT_TRUE(m_Loader->GetLoadedPackages().empty());
}

TEST_F(PackageLoaderTest, LoadFromEmptyDirectory_ReturnsZero)
{
    // Create a temp directory with no DLLs
    auto tempDir = std::filesystem::temp_directory_path() / "deki_test_empty_packages";
    std::filesystem::create_directories(tempDir);

    int count = m_Loader->LoadPackagesFromDirectory(tempDir.string(), nullptr);
    EXPECT_EQ(count, 0);
    EXPECT_TRUE(m_Loader->GetLoadedPackages().empty());

    std::filesystem::remove_all(tempDir);
}

TEST_F(PackageLoaderTest, LoadFromDirectory_SetsPackageDirectory)
{
    auto tempDir = std::filesystem::temp_directory_path() / "deki_test_dir_tracking";
    std::filesystem::create_directories(tempDir);

    m_Loader->LoadPackagesFromDirectory(tempDir.string(), nullptr);
    EXPECT_EQ(m_Loader->GetPackageDirectory(), tempDir.string());

    std::filesystem::remove_all(tempDir);
}

TEST_F(PackageLoaderTest, LoadFromDirectory_SkipsNonPackageDLLs)
{
    // Create temp dir with DLLs that should be skipped
    auto tempDir = std::filesystem::temp_directory_path() / "deki_test_skip_dlls";
    std::filesystem::create_directories(tempDir);

    // These should be skipped by IsPackageDLL
    std::ofstream(tempDir / "deki-engine-core.dll").put('\0');
    std::ofstream(tempDir / "deki-editor.dll").put('\0');
    std::ofstream(tempDir / "deki-profiling-hooks.dll").put('\0');

    // Non-deki DLLs should also be skipped
    std::ofstream(tempDir / "SDL3.dll").put('\0');
    std::ofstream(tempDir / "opengl32.dll").put('\0');

    int count = m_Loader->LoadPackagesFromDirectory(tempDir.string(), nullptr);
    EXPECT_EQ(count, 0);
    EXPECT_TRUE(m_Loader->GetLoadedPackages().empty());

    std::filesystem::remove_all(tempDir);
}

TEST_F(PackageLoaderTest, LoadFromDirectory_SkipsNonDLLFiles)
{
    auto tempDir = std::filesystem::temp_directory_path() / "deki_test_skip_nondll";
    std::filesystem::create_directories(tempDir);

    // These match deki- prefix but aren't DLLs
    std::ofstream(tempDir / "deki-2d.lib").put('\0');
    std::ofstream(tempDir / "deki-2d.pdb").put('\0');
    std::ofstream(tempDir / "deki-2d.txt").put('\0');

    int count = m_Loader->LoadPackagesFromDirectory(tempDir.string(), nullptr);
    EXPECT_EQ(count, 0);
    EXPECT_TRUE(m_Loader->GetLoadedPackages().empty());

    std::filesystem::remove_all(tempDir);
}

// --- Unload ---

TEST_F(PackageLoaderTest, UnloadAllPackages_EmptyList_NoCrash)
{
    EXPECT_NO_THROW(m_Loader->UnloadAllPackages());
    EXPECT_TRUE(m_Loader->GetLoadedPackages().empty());
}

TEST_F(PackageLoaderTest, UnloadPackage_UnknownName_NoCrash)
{
    EXPECT_NO_THROW(m_Loader->UnloadPackage("NonExistentPackage"));
}

// --- CheckForUpdates ---

TEST_F(PackageLoaderTest, CheckForUpdates_NoPackages_ReturnsEmpty)
{
    auto changed = m_Loader->CheckForUpdates();
    EXPECT_TRUE(changed.empty());
}

// --- GetFunction ---

TEST_F(PackageLoaderTest, GetFunction_NoPackages_ReturnsNull)
{
    void* func = m_Loader->GetFunction("SomePackage", "SomeFunction");
    EXPECT_EQ(func, nullptr);
}

// --- NotifyPlayModeStart ---

TEST_F(PackageLoaderTest, NotifyPlayModeStart_NoPackages_NoCrash)
{
    EXPECT_NO_THROW(m_Loader->NotifyPlayModeStart(nullptr));
}

// --- ReloadPackage ---

TEST_F(PackageLoaderTest, ReloadPackage_UnknownName_ReturnsFalse)
{
    bool result = m_Loader->ReloadPackage("NonExistentPackage", nullptr);
    EXPECT_FALSE(result);
}

// --- Destructor ---

TEST_F(PackageLoaderTest, Destructor_CleansUpSafely)
{
    // Just verify that creating and destroying a loader doesn't crash
    {
        PackageLoader loader;
    }
    SUCCEED();
}

// ============================================================================
// DLL Filtering Tests
// ============================================================================

// Helper: expose IsPackageDLL for testing via LoadPackagesFromDirectory behavior
class PackageLoaderFilterTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Loader = std::make_unique<PackageLoader>();
        m_TempDir = std::filesystem::temp_directory_path() / "deki_test_filter";
        std::filesystem::create_directories(m_TempDir);
    }

    void TearDown() override
    {
        m_Loader.reset();
        std::filesystem::remove_all(m_TempDir);
    }

    // Create a fake DLL file in the temp directory
    void CreateFakeFile(const std::string& filename)
    {
        std::ofstream(m_TempDir / filename).put('\0');
    }

    std::unique_ptr<PackageLoader> m_Loader;
    std::filesystem::path m_TempDir;
};

// ============================================================================
// Shared library naming
//
// The loader finds packages by name, so the decoration rules have to match what
// the build actually emits. Getting this wrong is silent and total: every
// package fails the "deki-*" test and the editor loads none of them, with no
// error beyond components not existing. It has happened twice — once for the
// ELF "lib" prefix, once for MinGW, which adds the same prefix on Windows where
// the rule had been keyed on the OS rather than the toolchain.
// ============================================================================

TEST(SharedLibNaming, StemRoundTripsFileNameForThisToolchain)
{
    // The invariant the loader depends on, stated without hardcoding a platform.
    EXPECT_EQ(SharedLibStem(SharedLibFileName("deki-2d")), "deki-2d");
    EXPECT_EQ(SharedLibStem(SharedLibFileName("deki-engine-core")), "deki-engine-core");
    EXPECT_TRUE(IsSharedLibFile(SharedLibFileName("deki-2d")));
}

TEST(SharedLibNaming, StripsThePrefixWhenTheToolchainAddsOne)
{
    // GNU toolchains prefix shared libraries everywhere they run, MSVC never
    // does — so this asserts against the prefix the build is actually using
    // rather than against a fixed platform expectation.
    const std::string prefixed = std::string(kSharedLibPrefix) + "deki-2d" + kSharedLibExt;
    EXPECT_EQ(SharedLibStem(prefixed), "deki-2d");
}

TEST(SharedLibNaming, CutsAtTheFirstDot)
{
    // Versioned sonames and the loader's own ".stale-N" shadow suffixes both
    // trail the extension, so a last-dot split would keep ".so" in the stem.
    EXPECT_EQ(SharedLibStem(std::string(kSharedLibPrefix) + "deki-2d.so.1"), "deki-2d");
    EXPECT_EQ(SharedLibStem(SharedLibFileName("deki-2d") + ".stale-3"), "deki-2d");
}

TEST_F(PackageLoaderFilterTest, AcceptsValidPackageDLLNames)
{
    // These should be accepted by IsPackageDLL (will fail to load, but attempted)
    // deki-2d.dll, deki-tween.dll etc. match the pattern
    CreateFakeFile("deki-2d.dll");

    // LoadPackage will be called for matching files but will fail since it's not a real DLL
    // That's fine - we just verify it doesn't count as loaded (count = 0)
    // The important thing is it doesn't crash on invalid DLLs
    int count = m_Loader->LoadPackagesFromDirectory(m_TempDir.string(), nullptr);
    EXPECT_EQ(count, 0); // Fake DLL can't load
}

TEST_F(PackageLoaderFilterTest, SkipsEngineCoreDLL)
{
    CreateFakeFile("deki-engine-core.dll");
    int count = m_Loader->LoadPackagesFromDirectory(m_TempDir.string(), nullptr);
    EXPECT_EQ(count, 0);
}

TEST_F(PackageLoaderFilterTest, SkipsEditorDLL)
{
    CreateFakeFile("deki-editor.dll");
    int count = m_Loader->LoadPackagesFromDirectory(m_TempDir.string(), nullptr);
    EXPECT_EQ(count, 0);
}

TEST_F(PackageLoaderFilterTest, SkipsProfilingHooksDLL)
{
    CreateFakeFile("deki-profiling-hooks.dll");
    int count = m_Loader->LoadPackagesFromDirectory(m_TempDir.string(), nullptr);
    EXPECT_EQ(count, 0);
}

TEST_F(PackageLoaderFilterTest, SkipsNonDekiPrefixDLLs)
{
    CreateFakeFile("SDL3.dll");
    CreateFakeFile("imgui.dll");
    CreateFakeFile("freetype.dll");
    int count = m_Loader->LoadPackagesFromDirectory(m_TempDir.string(), nullptr);
    EXPECT_EQ(count, 0);
}

TEST_F(PackageLoaderFilterTest, SkipsNonDLLExtensions)
{
    CreateFakeFile("deki-2d.lib");
    CreateFakeFile("deki-2d.pdb");
    CreateFakeFile("deki-2d.exp");
    CreateFakeFile("deki-2d.txt");
    int count = m_Loader->LoadPackagesFromDirectory(m_TempDir.string(), nullptr);
    EXPECT_EQ(count, 0);
}

TEST_F(PackageLoaderFilterTest, SkipsDirectories)
{
    std::filesystem::create_directories(m_TempDir / "deki-subdir.dll");
    int count = m_Loader->LoadPackagesFromDirectory(m_TempDir.string(), nullptr);
    EXPECT_EQ(count, 0);
}

TEST_F(PackageLoaderFilterTest, HandlesInvalidDLLGracefully)
{
    // A file that matches the pattern but isn't a valid PE/DLL
    CreateFakeFile("deki-fake-package.dll");
    int count = m_Loader->LoadPackagesFromDirectory(m_TempDir.string(), nullptr);
    EXPECT_EQ(count, 0);
    EXPECT_TRUE(m_Loader->GetLoadedPackages().empty());
}

TEST_F(PackageLoaderFilterTest, CaseInsensitiveFiltering)
{
    // Uppercase variants of skip-list DLLs should also be skipped
    CreateFakeFile("DEKI-ENGINE-CORE.DLL");
    CreateFakeFile("Deki-Editor.dll");
    CreateFakeFile("DEKI-PROFILING-HOOKS.dll");
    int count = m_Loader->LoadPackagesFromDirectory(m_TempDir.string(), nullptr);
    EXPECT_EQ(count, 0);
}

// ============================================================================
// PackageInfo Struct Tests
// ============================================================================

TEST(PackageInfoTest, DefaultValues)
{
    PackageInfo info;
    EXPECT_TRUE(info.name.empty());
    EXPECT_TRUE(info.path.empty());
    EXPECT_TRUE(info.dllFilename.empty());
    EXPECT_EQ(info.componentCount, 0);
    EXPECT_FALSE(info.loaded);
    EXPECT_EQ(info.handle, nullptr);
}

// ============================================================================
// EditorRegistry Shared Singleton Test
// ============================================================================

TEST(EditorRegistrySharedSingletonTest, SingletonIsSharedAcrossCallSites)
{
    // This verifies that EditorRegistry::Instance() returns the same instance
    // regardless of where it's called from (the fix for the DLL boundary bug)
    auto& inst1 = EditorRegistry::Instance();
    auto& inst2 = EditorRegistry::Instance();
    EXPECT_EQ(&inst1, &inst2);

    // Adding a factory from one call site should be visible from another
    size_t countBefore = inst1.GetTotalCount();

    static MockAssetTypeEditorForLoader sharedProto("SharedSingletonTest");
    inst1.AddFactory(&sharedProto, []() -> std::unique_ptr<EditorExtension> {
        return std::make_unique<MockAssetTypeEditorForLoader>("SharedSingletonTest");
    });

    EXPECT_EQ(inst2.GetTotalCount(), countBefore + 1);
}
