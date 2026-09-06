#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "services/PluginToolchain.h"
#include "services/Prerequisites.h"

using namespace DekiEditor;

// The editor built with GCC/Clang (Itanium ABI) builds packages with that same
// family; only an MSVC-ABI editor needs clang-cl. The probe used to demand
// clang-cl on every Windows machine, which skipped the first build on the
// supported MinGW setup and left every component unresolved.
TEST(Prerequisites, HostAbiFollowsTheCompiler)
{
#if defined(_MSC_VER)
    EXPECT_EQ(PluginToolchain::HostAbi(), PluginAbi::Msvc);
#else
    EXPECT_EQ(PluginToolchain::HostAbi(), PluginAbi::Gnu);
#endif
}

TEST(Prerequisites, GnuAbiNeverAsksForClangCl)
{
    if (PluginToolchain::HostAbi() != PluginAbi::Gnu)
        GTEST_SKIP() << "MSVC-ABI editor: clang-cl is a legitimate requirement";

    const auto results = CheckPrerequisites();
    ASSERT_FALSE(results.empty());
    for (const auto& p : results)
        EXPECT_NE(p.name, "clang-cl") << "clang-cl required under the GNU ABI";
}

// When the toolchain resolves, the reflection-compiler entry is the one the
// build invokes, so a valid toolchain must not coexist with a failed probe.
TEST(Prerequisites, ValidToolchainMeansCompilerPrerequisiteHolds)
{
    const PluginToolchainInfo& tc = PluginToolchain::Get();
    if (!tc.valid)
        GTEST_SKIP() << "no plugin toolchain on this machine: " << tc.error;

    const auto results = CheckPrerequisites();
    const auto it = std::find_if(results.begin(), results.end(), [](const Prerequisite& p) {
        return p.name.rfind("GCC 16+", 0) == 0;
    });
    ASSERT_NE(it, results.end());
    EXPECT_TRUE(it->ok) << it->install;
}

TEST(Prerequisites, SatisfiedIgnoresOptionalTools)
{
    std::vector<Prerequisite> v(2);
    v[0].name = "cmake"; v[0].required = true;  v[0].ok = true;
    v[1].name = "curl";  v[1].required = false; v[1].ok = false;
    EXPECT_TRUE(PrerequisitesSatisfied(v));

    v[0].ok = false;
    EXPECT_FALSE(PrerequisitesSatisfied(v));

    const auto lines = DescribeMissing(v);
    EXPECT_EQ(lines.size(), 6u);  // three lines per missing tool, optional ones included
}
