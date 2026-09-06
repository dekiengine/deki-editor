#include <gtest/gtest.h>

#include "services/installers/DekiPackageInstaller.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
    // A package directory holding exactly the manifest text given (or none).
    class PackageDir
    {
    public:
        explicit PackageDir(const char* name)
            : m_Path(fs::temp_directory_path() / (std::string("deki_minengine_") + name))
        {
            std::error_code ec;
            fs::remove_all(m_Path, ec);
            fs::create_directories(m_Path, ec);
        }
        ~PackageDir()
        {
            std::error_code ec;
            fs::remove_all(m_Path, ec);
        }

        void WriteManifest(const std::string& body) const
        {
            std::ofstream out(m_Path / "package.json");
            out << body;
        }

        const fs::path& Path() const { return m_Path; }

    private:
        fs::path m_Path;
    };
}

// The engine this editor embeds. The gate compares against exactly this.
TEST(MinEngineGate, EmbeddedVersionIsDefined)
{
    EXPECT_STRNE(DEKI_ENGINE_VERSION, "");
}

TEST(MinEngineGate, RefusesAPackageNeedingANewerEngine)
{
    PackageDir dir("newer");
    dir.WriteManifest(R"({"id":"deki-x","minEngine":"99.0.0"})");

    DekiPackageInstaller installer;
    std::string reason;
    EXPECT_FALSE(installer.EngineSatisfiesPackage(dir.Path(), reason));
    // The message has to name both versions; that is the whole point of the gate.
    EXPECT_NE(reason.find("99.0.0"), std::string::npos);
    EXPECT_NE(reason.find(DEKI_ENGINE_VERSION), std::string::npos);
}

TEST(MinEngineGate, AcceptsAPackageMatchingOrOlderThanTheEngine)
{
    DekiPackageInstaller installer;
    std::string reason;

    PackageDir same("same");
    same.WriteManifest(std::string(R"({"id":"deki-x","minEngine":")") + DEKI_ENGINE_VERSION + R"("})");
    EXPECT_TRUE(installer.EngineSatisfiesPackage(same.Path(), reason)) << reason;

    PackageDir older("older");
    older.WriteManifest(R"({"id":"deki-x","minEngine":"0.1.0"})");
    EXPECT_TRUE(installer.EngineSatisfiesPackage(older.Path(), reason)) << reason;
}

// Packages published before the field existed must keep installing, and a
// broken manifest is the loader's problem to report, not this gate's.
TEST(MinEngineGate, AcceptsManifestsThatCannotAnswer)
{
    DekiPackageInstaller installer;
    std::string reason;

    PackageDir none("none");  // no package.json at all
    EXPECT_TRUE(installer.EngineSatisfiesPackage(none.Path(), reason)) << reason;

    PackageDir noField("nofield");
    noField.WriteManifest(R"({"id":"deki-x","displayName":"X"})");
    EXPECT_TRUE(installer.EngineSatisfiesPackage(noField.Path(), reason)) << reason;

    PackageDir malformed("malformed");
    malformed.WriteManifest("{ this is not json");
    EXPECT_TRUE(installer.EngineSatisfiesPackage(malformed.Path(), reason)) << reason;

    PackageDir wrongType("wrongtype");
    wrongType.WriteManifest(R"({"id":"deki-x","minEngine":110})");
    EXPECT_TRUE(installer.EngineSatisfiesPackage(wrongType.Path(), reason)) << reason;

    PackageDir empty("empty");
    empty.WriteManifest(R"({"id":"deki-x","minEngine":""})");
    EXPECT_TRUE(installer.EngineSatisfiesPackage(empty.Path(), reason)) << reason;
}
