#pragma once

// This release renamed the desktop-os capability to hosted, and the SIMULATOR
// define to DEKI_HOSTED: both mark a game an operating system starts as a
// program, which the Miyoo Mini, a handheld, is too. A project from before
// keeps both names in its files, so the editor changes them when it opens it,
// and says which files it changed (they show in version control, and can be
// undone there).
//
// The editor still reads desktop-os for one release (CurrentCapabilityName),
// for files this does not reach: packages the editor installs from the
// registry, which their own release updates.

#include <string>
#include <vector>

namespace DekiEditor::HostedRename
{

/// A platform's JSON with the old names changed: "desktop-os" in provides is
/// "hosted", and "SIMULATOR" leaves the defines of a hosted platform, whose
/// build defines DEKI_HOSTED itself. Keys keep their order. False, `text`
/// untouched, when it has neither, or is not JSON.
bool MigratePlatformJson(std::string& text);

/// A package.json with "desktop-os" in platformRequires as "hosted". False,
/// untouched, when it has none.
bool MigratePackageJson(std::string& text);

/// C or C++ source with SIMULATOR, the exact word, as DEKI_HOSTED on its
/// conditional preprocessor lines (#if, #ifdef, #ifndef, #elif, #elifdef,
/// #elifndef). Nothing else is touched. False, untouched, when there is none.
bool MigrateSimulatorDefine(std::string& text);

/// Applies all three to the project at `projectPath`: platforms/*/*.json, and
/// the package.json and code of the packages its deki-packages.json marks
/// editable, and the code under src/. Returns the files it changed, relative
/// to the project.
std::vector<std::string> MigrateProject(const std::string& projectPath);

}  // namespace DekiEditor::HostedRename
