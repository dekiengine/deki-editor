// Package libraries load after the libraries they link to, so Linux maps
// each one once (see DependencyOrder).

#include <gtest/gtest.h>

#include "services/LibraryDependencies.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using DekiEditor::DependencyOrder;
using DekiEditor::NeededLibraries;

namespace
{

std::vector<std::string> Names(const std::vector<fs::path>& paths)
{
    std::vector<std::string> names;
    for (const fs::path& p : paths)
    {
        names.push_back(p.filename().string());
    }
    return names;
}

size_t IndexOf(const std::vector<std::string>& names, const std::string& name)
{
    return static_cast<size_t>(std::find(names.begin(), names.end(), name) - names.begin());
}

}  // namespace

// The case that split deki-rendering in two: deki-2d sorts first by name but
// links to deki-rendering and deki-input.
TEST(LibraryDependencies, ALibraryComesAfterTheOnesItNeeds)
{
    const std::map<std::string, std::vector<std::string>> needed = {
        { "libdeki-2d.so", { "libdeki-rendering.so", "libdeki-input.so", "libstdc++.so.6" } },
        { "libdeki-input.so", {} },
        { "libdeki-rendering.so", { "libdeki-editor.so" } },
        { "libdeki-esp32-hal.so", { "libdeki-i2c.so", "libdeki-gpio.so" } },
        { "libdeki-gpio.so", {} },
        { "libdeki-i2c.so", {} },
    };
    std::vector<fs::path> paths;
    for (const auto& [name, deps] : needed)
    {
        paths.push_back(fs::path("shadow") / name);
    }

    const std::vector<std::string> order =
        Names(DependencyOrder(paths, [&needed](const fs::path& p) { return needed.at(p.filename().string()); }));

    ASSERT_EQ(order.size(), needed.size());
    EXPECT_LT(IndexOf(order, "libdeki-rendering.so"), IndexOf(order, "libdeki-2d.so"));
    EXPECT_LT(IndexOf(order, "libdeki-input.so"), IndexOf(order, "libdeki-2d.so"));
    EXPECT_LT(IndexOf(order, "libdeki-i2c.so"), IndexOf(order, "libdeki-esp32-hal.so"));
    EXPECT_LT(IndexOf(order, "libdeki-gpio.so"), IndexOf(order, "libdeki-esp32-hal.so"));
}

TEST(LibraryDependencies, ACycleStillListsEveryLibraryOnce)
{
    const std::vector<fs::path> paths = { "liba.so", "libb.so" };
    const std::vector<std::string> order = Names(
        DependencyOrder(paths, [](const fs::path& p)
                        { return std::vector<std::string>{ p.filename() == "liba.so" ? "libb.so" : "liba.so" }; }));
    EXPECT_EQ(order.size(), 2u);
}

TEST(LibraryDependencies, ANonElfFileNeedsNothing)
{
    const fs::path file = fs::temp_directory_path() / "deki_not_a_library.txt";
    {
        std::ofstream(file) << "not a library";
    }
    EXPECT_TRUE(NeededLibraries(file).empty());
    EXPECT_TRUE(NeededLibraries(fs::temp_directory_path() / "deki_no_such_library.so").empty());
    std::error_code ec;
    fs::remove(file, ec);
}

#ifdef __linux__
// This test binary is an ELF file linked to the editor library.
TEST(LibraryDependencies, ReadsTheLibrariesAnElfFileNeeds)
{
    const std::vector<std::string> needed = NeededLibraries("/proc/self/exe");
    EXPECT_NE(std::find(needed.begin(), needed.end(), "libdeki-engine-core.so"), needed.end());
}
#endif
