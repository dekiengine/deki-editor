// The file-type filter the dialog call sites are written in is Win32's: a run
// of NUL-separated strings ended by a double NUL. The portable backend wants
// bare extensions instead, and this is the conversion between them.

#include <gtest/gtest.h>

#include "services/FileDialogFilter.h"

using namespace DekiEditor::FileDialogs;

TEST(FileDialogFilter, ReadsTheSceneFilterTheEditorUses)
{
    const auto filters = ParseWin32Filter("Scene Files (*.scene)\0*.scene\0");
    ASSERT_EQ(filters.size(), 1u);
    EXPECT_EQ(filters[0].name, "Scene Files (*.scene)");
    EXPECT_EQ(filters[0].pattern, "scene");
}

TEST(FileDialogFilter, ReadsSeveralEntries)
{
    const auto filters = ParseWin32Filter("Images\0*.png;*.jpg\0All Files\0*.*\0");
    ASSERT_EQ(filters.size(), 2u);
    EXPECT_EQ(filters[0].name, "Images");
    EXPECT_EQ(filters[0].pattern, "png;jpg");
    EXPECT_EQ(filters[1].name, "All Files");
    EXPECT_EQ(filters[1].pattern, "*");
}

TEST(FileDialogFilter, StarMeansEveryFile)
{
    const auto filters = ParseWin32Filter("Anything\0*\0");
    ASSERT_EQ(filters.size(), 1u);
    EXPECT_EQ(filters[0].pattern, "*");
}

TEST(FileDialogFilter, TrimsSpacesBetweenGlobs)
{
    const auto filters = ParseWin32Filter("Images\0*.png; *.jpg\0");
    ASSERT_EQ(filters.size(), 1u);
    EXPECT_EQ(filters[0].pattern, "png;jpg");
}

TEST(FileDialogFilter, AlreadyBareExtensionsSurvive)
{
    const auto filters = ParseWin32Filter("Text\0txt\0");
    ASSERT_EQ(filters.size(), 1u);
    EXPECT_EQ(filters[0].pattern, "txt");
}

// No filter is a legitimate request: it means "show me everything", which the
// dialogs express as an empty filter list rather than a wildcard entry.
TEST(FileDialogFilter, NoFilterYieldsNoEntries)
{
    EXPECT_TRUE(ParseWin32Filter(nullptr).empty());
    EXPECT_TRUE(ParseWin32Filter("").empty());
}

// A name with no spec behind it would otherwise send the parser reading past
// the terminator looking for one.
TEST(FileDialogFilter, DanglingNameIsDropped)
{
    const auto filters = ParseWin32Filter("Scene Files\0");
    EXPECT_TRUE(filters.empty());
}
