// Only libraries this editor built here load: a project that arrives with
// generated/runtime/ filled has those libraries removed on open, and the
// build makes them again.

#include <gtest/gtest.h>
#include "services/BuiltLibraries.h"
#include "services/SharedLibNaming.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
namespace BL = DekiEditor::BuiltLibraries;

namespace
{
struct Runtime
{
    fs::path root = fs::temp_directory_path() / "deki-built-libraries-test";
    fs::path dir = root / "runtime";
    Runtime()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(dir / ".build_temp");
        BL::SetStorePathForTesting((root / "built-libraries.json").string());
    }
    ~Runtime()
    {
        BL::SetStorePathForTesting("");
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    fs::path Lib(const std::string& stem, const char* content = "code") const
    {
        const fs::path p = dir / (std::string(DekiEditor::kSharedLibPrefix) + stem + DekiEditor::kSharedLibExt);
        std::ofstream(p, std::ios::binary) << content;
        return p;
    }
};

bool Has(const std::vector<std::string>& v, const fs::path& p)
{
    return std::find(v.begin(), v.end(), p.filename().string()) != v.end();
}
}  // namespace

TEST(BuiltLibraries, WhatABuildWroteStaysAndAnythingElseGoes)
{
    Runtime r;
    const fs::path shipped = r.Lib("deki-shipped");  // came with the project
    fs::last_write_time(shipped, fs::file_time_type::clock::now() - std::chrono::hours(1));

    const auto buildStart = fs::file_time_type::clock::now() - std::chrono::seconds(1);
    const fs::path built = r.Lib("deki-built");
    BL::RecordBuiltSince(r.dir.string(), buildStart);

    const auto removed = BL::RemoveUnknown(r.dir.string());
    EXPECT_TRUE(Has(removed, shipped));
    EXPECT_FALSE(fs::exists(shipped));
    EXPECT_FALSE(Has(removed, built));
    EXPECT_TRUE(fs::exists(built));
}

TEST(BuiltLibraries, ALibraryReplacedAfterItsBuildGoes)
{
    Runtime r;
    const auto buildStart = fs::file_time_type::clock::now() - std::chrono::seconds(1);
    const fs::path lib = r.Lib("deki-x");
    BL::RecordBuiltSince(r.dir.string(), buildStart);

    r.Lib("deki-x", "something else, longer");  // swapped for another file
    EXPECT_TRUE(Has(BL::RemoveUnknown(r.dir.string()), lib));
    EXPECT_FALSE(fs::exists(lib));
}

TEST(BuiltLibraries, AStagedPluginFromBeforeThisSessionGoes)
{
    Runtime r;
    const fs::path staged = r.dir / ".build_temp" /
                            (std::string(DekiEditor::kSharedLibPrefix) + "game-components" + DekiEditor::kSharedLibExt);
    std::ofstream(staged, std::ios::binary) << "code";
    BL::Record({ staged });  // even a record does not keep the staging folder
    EXPECT_TRUE(Has(BL::RemoveUnknown(r.dir.string()), staged));
    EXPECT_FALSE(fs::exists(staged));
}

TEST(BuiltLibraries, APromotedPluginIsKept)
{
    Runtime r;
    const fs::path plugin = r.Lib("game-components");
    BL::Record({ plugin });
    EXPECT_TRUE(BL::RemoveUnknown(r.dir.string()).empty());
    EXPECT_TRUE(fs::exists(plugin));
}
