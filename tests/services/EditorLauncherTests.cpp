// EditorLauncher: when the command line opens an editor for its tools rather
// than running them hidden, and the command line that opens it.
//
// Starting a real editor is left to a hand run: these cover the decisions
// and the quoting, which decide whether a project path with spaces, quotes
// or a trailing backslash reaches the editor intact.

#include <gtest/gtest.h>

#include "services/EditorLauncher.h"

#include <map>
#include <string>

namespace EL = DekiEditor::EditorLauncher;

namespace
{
EL::EnvFn Env(std::map<std::string, std::string> values)
{
    return [values](const char* name) -> const char*
    {
        const auto it = values.find(name);
        return it == values.end() ? nullptr : it->second.c_str();
    };
}
}  // namespace

// A CI job runs its tools hidden, whatever else it has.
TEST(EditorLauncher, ACiJobNeverOpensAnEditor)
{
    EXPECT_FALSE(EL::CanShowEditor(Env({ { "CI", "true" }, { "DISPLAY", ":0" } })));
    EXPECT_TRUE(EL::CanShowEditor(Env({ { "CI", "" }, { "DISPLAY", ":0" }, { "WAYLAND_DISPLAY", "wayland-0" } })));
}

#ifdef _WIN32
// Windows always has a desktop to show it on.
TEST(EditorLauncher, WindowsShowsTheEditorOutsideCi)
{
    EXPECT_TRUE(EL::CanShowEditor(Env({})));
}
#else
// Linux needs a display: over SSH or in a container there is none.
TEST(EditorLauncher, LinuxShowsTheEditorOnlyWithADisplay)
{
    EXPECT_FALSE(EL::CanShowEditor(Env({})));
    EXPECT_TRUE(EL::CanShowEditor(Env({ { "DISPLAY", ":0" } })));
    EXPECT_TRUE(EL::CanShowEditor(Env({ { "WAYLAND_DISPLAY", "wayland-0" } })));
}
#endif

TEST(EditorLauncher, TheCommandLineQuotesBothPaths)
{
    EXPECT_EQ(EL::BackgroundCommandLine("C:\\Deki\\DekiEditor.exe", "C:\\My Games\\demo"),
              "\"C:\\Deki\\DekiEditor.exe\" --background \"C:\\My Games\\demo\"");
}

// A trailing backslash would escape the closing quote, and a quote inside a
// path would end the argument: both come back as they were.
TEST(EditorLauncher, ATrailingBackslashAndAQuoteSurvive)
{
    EXPECT_EQ(EL::BackgroundCommandLine("e.exe", "C:\\demo\\"), "\"e.exe\" --background \"C:\\demo\\\\\"");
    EXPECT_EQ(EL::BackgroundCommandLine("e.exe", "a\"b"), "\"e.exe\" --background \"a\\\"b\"");
    EXPECT_EQ(EL::BackgroundCommandLine("e.exe", "a\\\"b"), "\"e.exe\" --background \"a\\\\\\\"b\"");
}
