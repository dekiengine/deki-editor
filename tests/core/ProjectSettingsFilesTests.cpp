// Project settings files under settings/.

#include <gtest/gtest.h>

#include "core/Project.h"
#include <deki-editor/build/ProjectPaths.h>

#include <filesystem>
#include <fstream>
#include <string>

using DekiEditor::Project;

// A settings file that does not parse (a merge conflict) is never written
// over: its sections load as defaults and saving leaves the file alone, so
// what the conflict holds is not lost.
TEST(ProjectSettingsFiles, AConflictedFileIsNotWrittenOver)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "deki-settings-conflict-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path core = DekiEditor::ProjectPaths::Settings(root.string()) / "core.json";
    fs::create_directories(core.parent_path());
    const std::string conflicted = "{\n<<<<<<< HEAD\n  \"Core\": {\"targetFps\": 30}\n=======\n"
                                   "  \"Core\": {\"targetFps\": 60}\n>>>>>>> theirs\n}\n";
    std::ofstream(core, std::ios::binary) << conflicted;

    Project project;
    project.LoadSettingsFiles(root.string());
    project.SaveSettingsFiles(root.string());

    std::ifstream in(core, std::ios::binary);
    const std::string after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    EXPECT_EQ(after, conflicted);
    fs::remove_all(root, ec);
}
