#pragma once

// What a brand-new project is made of, as plain file writers.
//
// ProjectTemplate::CreateProject used to write a deki.json with no packages,
// no platform and no scene: the editor opened an empty project that rendered
// nothing until the user found the Package Manager and knew which four
// packages to add by name. The writers here produce a project that opens to
// a camera, a gradient and a line of text, with the desktop packages declared
// so the first open restores and builds them. They take paths and strings
// only, so they are testable without booting the editor.

#include <string>
#include <vector>

namespace DekiEditor::ProjectScaffold
{

/// Startup scene, relative to the assets directory (forward slashes, as
/// Project::Save writes it).
inline constexpr const char* kStartupScene = "scenes/main.scene";

/// Font shipped with the editor that the starter scene's text uses, and the
/// one size baked for it.
inline constexpr const char* kStarterFontFile = "JetBrainsMono-Regular.ttf";
inline constexpr int kStarterFontSize = 16;

/// Registry packages every new desktop project declares: rendering, the 2D
/// components, input, and the SDL3 window plus desktop providers that make the
/// simulator run.
std::vector<std::string> DefaultPackageIds();

/// deki.json with the current structure version, the startup scene and the
/// active platform. Everything Project::Save would rewrite on first open is
/// written the same way here, so a new project is not "unmigrated".
bool WriteDekiJson(const std::string& projectPath, const std::string& projectName,
                   const std::string& activePlatformId, std::string& err);

/// deki-packages.json declaring `packageIds` at the newest version.
bool WritePackageManifestFor(const std::string& projectPath,
                             const std::vector<std::string>& packageIds, std::string& err);

/// .gitignore covering generated/, package clones, exported storage/ and IDE
/// folders.
bool WriteGitIgnore(const std::string& projectPath, std::string& err);

/// assets/rendering/Standard2D.asset: the default render pipeline with the
/// clip2d pass (an empty pass list is valid but draws nothing).
bool WriteRenderPipeline(const std::string& projectPath, std::string& err);

/// Copies kStarterFontFile from `editorFontsDir` into assets/fonts/ and
/// writes its .data sidecar (GUID + kStarterFontSize baked). Returns the
/// font's GUID, or "" when the editor's font is not available — the caller
/// then writes the scene without the text object.
std::string InstallStarterFont(const std::string& projectPath, const std::string& editorFontsDir,
                               std::string& err);

/// assets/<kStartupScene>: Root → Camera, a gradient background, and (when
/// `fontSourceGuid` is non-empty) a "Hello, Deki" text using that font's
/// kStarterFontSize variant. Property keys are the components' reflected
/// names; `DekiEditor --verify-scenes` on a new project guards them.
bool WriteStarterScene(const std::string& projectPath, const std::string& fontSourceGuid,
                       std::string& err);

}  // namespace DekiEditor::ProjectScaffold
