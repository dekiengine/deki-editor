#include <gtest/gtest.h>

#include "services/PackageManager.h"
#include "services/PackageManifest.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

// Which installer manages a package is decided by an id the installer declares
// and the project records. It used to be decided by comparing an installer's
// DISPLAY name against "ESP-IDF" in six places, so a second framework's
// component manager could be registered and never be handed anything.
//
// Nothing here names a real framework on purpose: the point is that the editor
// routes on whatever id it is given.

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{

class StubInstaller : public IPackageInstaller
{
public:
    explicit StubInstaller(std::string id) : m_Id(std::move(id)) {}

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

// A lock written when one framework's name WAS the package type. The id is in
// the spelling; the component came from git, so "source" says nothing about it.
TEST(InstallerRouting, AnOlderLockStillSaysWhichInstaller)
{
    TempProject project;
    project.Write("deki-packages.json",
                  json{ { "schemaVersion", 1 },
                        { "packages",
                          { { "github:someone/widget",
                              { { "version", "1.0.0" },
                                { "source", "github" },
                                { "url", "https://github.com/someone/widget" } } } } } });
    project.Write("deki-packages-lock.json",
                  json{ { "lockVersion", 1 },
                        { "packages",
                          { { "github:someone/widget",
                              { { "name", "someone/widget" },
                                { "installedVersion", "1.0.0" },
                                { "source", "github" },
                                { "packageType", "alpha_component" } } } } } });

    PackageManager mgr(nullptr);
    mgr.LoadPackages(project.dir.string());

    const PackageEntry* found = nullptr;
    for (const auto& p : mgr.GetPackages())
        if (p.id == "github:someone/widget")
            found = &p;
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->packageType, PackageType::FrameworkComponent);
    EXPECT_EQ(found->installer, "alpha");
    EXPECT_EQ(found->source, PackageSourceType::GitHub);

    // And it is written back in the current form, installer included.
    mgr.SavePackages(project.dir.string());
    const json lock = project.Read("deki-packages-lock.json");
    const json& entry = lock["packages"]["github:someone/widget"];
    EXPECT_EQ(entry.value("packageType", ""), "framework_component");
    EXPECT_EQ(entry.value("installer", ""), "alpha");
    EXPECT_EQ(entry.value("source", ""), "github");
}
