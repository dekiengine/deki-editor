// The editor's languages: text falls back to English, and an ImGui label
// keeps the same ID in every language.

#include <gtest/gtest.h>

#include <deki-editor/Localization.h>

#include "imgui.h"
#include "imgui_internal.h"  // ImHashStr

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace
{

// A table directory with ja.json holding `json`, removed afterwards.
class TableDir
{
public:
    TableDir(const char* name, const char* json)
        : m_Dir(fs::temp_directory_path() / name)
    {
        fs::create_directories(m_Dir);
        std::ofstream(m_Dir / "ja.json", std::ios::binary) << json;
    }
    ~TableDir()
    {
        std::error_code ec;
        fs::remove_all(m_Dir, ec);
    }
    std::string Path() const { return m_Dir.string(); }

private:
    fs::path m_Dir;
};

// Each test starts and ends in English, the default.
class Localization : public ::testing::Test
{
protected:
    void TearDown() override { DekiEditor::Localization::UseLanguage("en"); }
};

}  // namespace

TEST_F(Localization, ShowsTheTranslationAndFallsBackToEnglish)
{
    TableDir dir("deki-loc-basic", R"({ "Save": "保存", "Empty": "" })");
    DekiEditor::Localization::AddTableDirectory(dir.Path());

    EXPECT_STREQ(DekiEditor::Tr("Save"), "Save");
    DekiEditor::Localization::UseLanguage("ja");
    EXPECT_EQ(DekiEditor::Localization::Current(), "ja");
    EXPECT_STREQ(DekiEditor::Tr("Save"), "保存");
    EXPECT_STREQ(DekiEditor::Tr("Not in the table"), "Not in the table");
    EXPECT_STREQ(DekiEditor::Tr("Empty"), "Empty");  // an empty translation is not one
}

TEST_F(Localization, AnUnknownLanguageIsEnglish)
{
    DekiEditor::Localization::UseLanguage("xx");
    EXPECT_EQ(DekiEditor::Localization::Current(), "en");
}

TEST_F(Localization, ALabelKeepsItsIdInEveryLanguage)
{
    TableDir dir("deki-loc-labels", R"({ "Open": "開く", "PLAY VIEW": "プレイビュー" })");
    DekiEditor::Localization::AddTableDirectory(dir.Path());

    const std::string english = DekiEditor::TrLabel("Open");
    const std::string englishWindow = DekiEditor::TrLabel("PLAY VIEW###Play View");
    DekiEditor::Localization::UseLanguage("ja");
    const std::string japanese = DekiEditor::TrLabel("Open");

    EXPECT_EQ(english, "Open###Open");
    EXPECT_EQ(japanese, "開く###Open");
    EXPECT_EQ(ImHashStr(english.c_str()), ImHashStr(japanese.c_str()));

    // A label with its own "###" keeps it, so a window keeps its layout.
    EXPECT_EQ(englishWindow, "PLAY VIEW###Play View");
    EXPECT_STREQ(DekiEditor::TrLabel("PLAY VIEW###Play View"), "プレイビュー###Play View");

    // "##" alone is part of the id; the shown text is still translated.
    EXPECT_STREQ(DekiEditor::TrLabel("Open##file"), "開く###Open##file");
    EXPECT_STREQ(DekiEditor::TrLabel("##hidden"), "##hidden");
}

TEST_F(Localization, TheFirstTableToTranslateATextKeepsIt)
{
    TableDir first("deki-loc-first", R"({ "Scene": "シーン" })");
    TableDir second("deki-loc-second", R"({ "Scene": "場面", "Prefab": "プレハブ" })");
    DekiEditor::Localization::AddTableDirectory(first.Path());
    DekiEditor::Localization::AddTableDirectory(second.Path());
    DekiEditor::Localization::UseLanguage("ja");

    EXPECT_STREQ(DekiEditor::Tr("Scene"), "シーン");
    EXPECT_STREQ(DekiEditor::Tr("Prefab"), "プレハブ");
}

// An icon in front of a label is kept and not part of the lookup.
TEST_F(Localization, AnIconInFrontIsKept)
{
    TableDir dir("deki-loc-icons", R"({ "New Project": "新規プロジェクト" })");
    DekiEditor::Localization::AddTableDirectory(dir.Path());
    DekiEditor::Localization::UseLanguage("ja");

    // U+EB0B, a Tabler icon, then two spaces.
    EXPECT_STREQ(DekiEditor::Tr("\xee\xac\x8b  New Project"), "\xee\xac\x8b  新規プロジェクト");
    EXPECT_STREQ(DekiEditor::TrLabel("\xee\xac\x8b  New Project"),
                 "\xee\xac\x8b  新規プロジェクト###\xee\xac\x8b  New Project");
    EXPECT_STREQ(DekiEditor::Tr("\xee\xac\x8b  Unknown"), "\xee\xac\x8b  Unknown");
}

// Files in <dir>/ja/ add to <dir>/ja.json.
TEST_F(Localization, ATableCanBeSplitIntoFiles)
{
    TableDir dir("deki-loc-split", R"({ "Hierarchy": "ヒエラルキー" })");
    std::filesystem::create_directories(std::filesystem::path(dir.Path()) / "ja");
    std::ofstream(std::filesystem::path(dir.Path()) / "ja" / "panels.json", std::ios::binary)
        << R"({ "Console": "コンソール" })";
    DekiEditor::Localization::AddTableDirectory(dir.Path());
    DekiEditor::Localization::UseLanguage("ja");

    EXPECT_STREQ(DekiEditor::Tr("Hierarchy"), "ヒエラルキー");
    EXPECT_STREQ(DekiEditor::Tr("Console"), "コンソール");
}

// A label built before a package added its table is rebuilt, and the text
// handed out before stays readable.
TEST_F(Localization, ALaterTableUpdatesLabelsWithoutFreeingOldOnes)
{
    DekiEditor::Localization::UseLanguage("ja");
    const char* before = DekiEditor::TrLabel("Tilemap Brush");
    EXPECT_STREQ(before, "Tilemap Brush###Tilemap Brush");

    TableDir dir("deki-loc-later", R"({ "Tilemap Brush": "タイルマップブラシ" })");
    DekiEditor::Localization::AddTableDirectory(dir.Path());

    EXPECT_STREQ(DekiEditor::TrLabel("Tilemap Brush"), "タイルマップブラシ###Tilemap Brush");
    EXPECT_STREQ(before, "Tilemap Brush###Tilemap Brush");
}
