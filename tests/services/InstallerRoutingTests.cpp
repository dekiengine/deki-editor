#include <gtest/gtest.h>

#include "services/PackageManager.h"
#include "services/PackageManifest.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

// Which installer manages a package is decided by an id the installer declares
// and the project records, never by an installer's display name.
//
// Nothing here names a real framework on purpose: the editor routes on
// whatever id it is given.

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{

class StubInstaller : public IPackageInstaller
{
public:
    explicit StubInstaller(std::string id)
        : m_Id(std::move(id))
    {
    }

    std::string GetId() const override { return m_Id; }
    std::string GetPlatformName() const override { return "Stub (" + m_Id + ")"; }
    bool IsApplicable(const std::string&) const override { return true; }
    std::vector<PackageEntry> GetInstalledPackages(const std::string&) override { return {}; }
    void Install(const std::string&, const PackageEntry&, const std::string&, PackageCallback) override {}
    void Remove(const std::string&, const PackageEntry&, PackageCallback) override {}
    std::vector<CoreDependency> GetCoreDependencies() const override { return {}; }
    void EnsureCoreDependencies(const std::string&) override {}
    bool IsCoreDependency(const std::string&) const override { return false; }

private:
    std::string m_Id;
};

PackageEntry FrameworkComponent(const std::string& id, const std::string& installer)
{
    PackageEntry pkg;
    pkg.id = id;
    pkg.name = id;
    pkg.packageType = PackageType::FrameworkComponent;
    pkg.installer = installer;
    return pkg;
}

struct TempProject
{
    fs::path dir;
    TempProject()
    {
        dir = fs::temp_directory_path() / "deki_installer_routing_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
    }
    ~TempProject()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void Write(const char* name, const json& j) const
    {
        std::ofstream f(dir / name, std::ios::binary);
        f << j.dump(2);
    }
    json Read(const char* name) const
    {
        std::ifstream f(dir / name);
        return json::parse(f);
    }
};

}  // namespace

TEST(InstallerRouting, AFrameworkComponentGoesToTheInstallerItNames)
{
    PackageManager mgr(nullptr);
    mgr.RegisterInstaller(std::make_unique<StubInstaller>("alpha"));
    mgr.RegisterInstaller(std::make_unique<StubInstaller>("beta"));

    std::string whyNot;
    IPackageInstaller* a = mgr.GetInstallerFor(FrameworkComponent("x", "alpha"), "", whyNot);
    IPackageInstaller* b = mgr.GetInstallerFor(FrameworkComponent("y", "beta"), "", whyNot);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->GetId(), "alpha");
    EXPECT_EQ(b->GetId(), "beta");
}

TEST(InstallerRouting, AMissingInstallerIsRefusedByNameNotGuessed)
{
    PackageManager mgr(nullptr);
    mgr.RegisterInstaller(std::make_unique<StubInstaller>("alpha"));

    // The only other installer present must not be picked because it is there.
    std::string whyNot;
    EXPECT_EQ(mgr.GetInstallerFor(FrameworkComponent("x", "gamma"), "", whyNot), nullptr);
    EXPECT_NE(whyNot.find("gamma"), std::string::npos) << whyNot;

    whyNot.clear();
    EXPECT_EQ(mgr.GetInstallerFor(FrameworkComponent("x", ""), "", whyNot), nullptr);
    EXPECT_FALSE(whyNot.empty());
}

TEST(InstallerRouting, ADekiPackageGoesToTheEditorsOwnInstaller)
{
    PackageManager mgr(nullptr);
    mgr.RegisterInstaller(std::make_unique<StubInstaller>("alpha"));

    PackageEntry pkg;
    pkg.id = "deki-x";
    pkg.packageType = PackageType::DekiPackage;

    std::string whyNot;
    IPackageInstaller* installer = mgr.GetInstallerFor(pkg, "", whyNot);
    ASSERT_NE(installer, nullptr) << whyNot;
    EXPECT_EQ(installer->GetId(), PackageManager::kDekiInstallerId);
}

TEST(PackageSourceSpelling, TheEditorsOwnSourcesAndAnInstallersId)
{
    PackageEntry pkg;
    DekiEditor::ReadPackageSource("github", true, pkg);
    EXPECT_EQ(pkg.source, PackageSourceType::GitHub);
    EXPECT_EQ(DekiEditor::PackageSourceToString(pkg), "github");

    pkg = {};
    DekiEditor::ReadPackageSource("", true, pkg);
    EXPECT_EQ(pkg.source, PackageSourceType::Custom);

    // Anything else names the installer whose registry the package is from,
    // and is written back exactly as it was read.
    pkg = {};
    DekiEditor::ReadPackageSource("alpha", true, pkg);
    EXPECT_EQ(pkg.source, PackageSourceType::FrameworkRegistry);
    EXPECT_EQ(pkg.installer, "alpha");
    EXPECT_EQ(DekiEditor::PackageSourceToString(pkg), "alpha");
}

// The registry catalogue a refresh merges in is offered, not used: the lock
// records the project's packages only.
TEST(PackageLock, RegistryOffersAreNotWrittenToTheLock)
{
    TempProject project;
    project.Write("deki-packages.json",
                  json{ { "schemaVersion", 1 }, { "packages", { { "deki-used", { { "version", "1.0.0" } } } } } });

    PackageManager mgr(nullptr);
    mgr.LoadPackages(project.dir.string());
    PackageEntry offer;
    offer.id = "deki-offer";
    offer.name = "deki-offer";
    offer.packageType = PackageType::DekiPackage;
    mgr.GetPackages().push_back(offer);

    mgr.SavePackages(project.dir.string());
    const json lock = project.Read("deki-packages-lock.json");
    EXPECT_TRUE(lock["packages"].contains("deki-used"));
    EXPECT_FALSE(lock["packages"].contains("deki-offer"));
}

// Updating or restoring a package that is another's dependency keeps it a
// dependency; only a package asked for directly is declared.
TEST(PackageDependencies, UpdatingADependencyDoesNotDeclareIt)
{
    class SucceedingInstaller : public StubInstaller
    {
    public:
        SucceedingInstaller()
            : StubInstaller("stub")
        {
        }
        void Install(const std::string&, const PackageEntry&, const std::string&, PackageCallback callback) override
        {
            callback(true, "");
        }
    };

    TempProject project;
    PackageManager mgr(nullptr);
    PackageEntry dependency;
    dependency.id = "deki-dep";
    dependency.name = "deki-dep";
    dependency.resolvedBy = "deki-board";
    dependency.isInstalled = true;
    mgr.GetPackages().push_back(dependency);
    PackageEntry asked;
    asked.id = "deki-asked";
    asked.name = "deki-asked";
    mgr.GetPackages().push_back(asked);

    SucceedingInstaller installer;
    mgr.InstallWithDependencies(project.dir.string(), dependency, "2.0.0", &installer, [](bool, const std::string&) {});
    mgr.InstallWithDependencies(project.dir.string(), asked, "1.0.0", &installer, [](bool, const std::string&) {});

    for (const PackageEntry& entry : mgr.GetPackages())
    {
        if (entry.id == "deki-dep")
        {
            EXPECT_FALSE(entry.declaredInManifest);
        }
        if (entry.id == "deki-asked")
        {
            EXPECT_TRUE(entry.declaredInManifest);
        }
    }
}

// An installed package is marked installed at its new version. A later
// install that requires it reads that mark; unmarked, it installed the
// package again at the version the lock remembered, undoing an update.
TEST(PackageDependencies, AnInstalledPackageIsMarkedAtItsVersion)
{
    class SucceedingInstaller : public StubInstaller
    {
    public:
        SucceedingInstaller()
            : StubInstaller("stub")
        {
        }
        void Install(const std::string&, const PackageEntry&, const std::string&, PackageCallback callback) override
        {
            callback(true, "");
        }
    };

    TempProject project;
    PackageManager mgr(nullptr);
    PackageEntry package;
    package.id = "deki-x";
    package.name = "deki-x";
    package.lockedVersion = "1.0.0";
    mgr.GetPackages().push_back(package);

    SucceedingInstaller installer;
    mgr.InstallWithDependencies(project.dir.string(), package, "2.0.0", &installer, [](bool, const std::string&) {});

    ASSERT_EQ(mgr.GetPackages().size(), 1u);
    EXPECT_TRUE(mgr.GetPackages()[0].isInstalled);
    EXPECT_EQ(mgr.GetPackages()[0].installedVersion, "2.0.0");
}

// Every requirement of a package is resolved, even when resolving one adds
// entries to the list the package itself lives in. deki-lilygo-boards lost
// deki-lovyangfx-integration this way: the list moved under the loop.
TEST(PackageDependencies, EveryRequirementIsResolved)
{
    PackageManager mgr(nullptr);
    PackageEntry board;
    board.id = "deki-board";
    board.name = "deki-board";
    board.requiredPackages = { "deki-a", "deki-b", "deki-c", "deki-d" };
    mgr.GetPackages().clear();
    mgr.GetPackages().shrink_to_fit();
    mgr.GetPackages().push_back(board);

    mgr.ResolveDependencies(mgr.GetPackages()[0]);

    for (const char* id : { "deki-a", "deki-b", "deki-c", "deki-d" })
    {
        bool found = false;
        for (const PackageEntry& entry : mgr.GetPackages())
        {
            found |= entry.id == id && entry.resolvedBy == "deki-board";
        }
        EXPECT_TRUE(found) << id;
    }
}

// The lock records the commit each package's version is (from the installer's
// .deki-package.json), so a restore can tell when a tag was moved. A package
// not on disk yet keeps the commit the lock already had.
TEST(PackageLock, RecordsTheCommitOfEachInstalledVersion)
{
    TempProject project;
    project.Write("deki-packages.json", json{ { "schemaVersion", 1 },
                                              { "packages",
                                                { { "deki-here", { { "version", "1.0.0" } } },
                                                  { "deki-away", { { "version", "2.0.0" } } } } } });
    project.Write("deki-packages-lock.json",
                  json{ { "lockVersion", 1 },
                        { "packages",
                          { { "deki-away",
                              { { "name", "deki-away" },
                                { "installedVersion", "2.0.0" },
                                { "commit", "feedfacefeedfacefeedfacefeedfacefeedface" } } } } } });
    const fs::path pkg = project.dir / "packages" / "deki-here";
    fs::create_directories(pkg);
    std::ofstream(pkg / "package.json") << R"({"name":"deki-here"})";
    std::ofstream(pkg / ".deki-package.json")
        << R"({"id":"deki-here","version":"1.0.0","commit":"0123456789abcdef0123456789abcdef01234567","name":"deki-here"})";

    PackageManager mgr(nullptr);
    mgr.LoadPackages(project.dir.string());
    // As RefreshInstalledStatus finds it (no installer is registered here).
    for (PackageEntry& p : mgr.GetPackages())
    {
        if (p.id == "deki-here")
        {
            p.isInstalled = true;
            p.installedVersion = "1.0.0";
        }
    }
    mgr.SavePackages(project.dir.string());
    const json lock = project.Read("deki-packages-lock.json");
    EXPECT_EQ(lock["packages"]["deki-here"].value("commit", ""), "0123456789abcdef0123456789abcdef01234567");
    EXPECT_EQ(lock["packages"]["deki-away"].value("commit", ""), "feedfacefeedfacefeedfacefeedfacefeedface");
}
