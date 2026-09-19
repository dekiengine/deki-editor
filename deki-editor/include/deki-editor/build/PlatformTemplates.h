#pragma once

// Platform templates: the platforms a PACKAGE offers a project.
//
// A target is more than a build backend. It needs a platform description
// (what the board or machine provides, which packages it always needs) and
// usually a boot scene that brings its display and input up. Both name the
// target's own components, so both belong with the package that ships those
// components:
//
//     <package>/platforms/<platform id>/<platform id>.json
//     <package>/platforms/<platform id>/boot.scene          (and anything else)
//
// The editor used to carry one of these in its own source: a hardcoded
// "native_simulator" config and a boot scene naming SDL3 display and input
// setup components, written into every new project. That made the desktop
// simulator the one target the editor knew by name, and gave any other target
// no way to offer the same thing. The editor knows none now. A project gets a
// platform by ADOPTING a template - a plain copy into <project>/platforms/, so
// from then on it is the project's own file to edit, exactly as before.

#include <deki-editor/EditorExport.h>

#include <string>
#include <vector>

namespace DekiEditor
{

struct DEKI_EDITOR_API PlatformTemplate
{
    std::string platformId;  // the directory name, which is also the platform id
    std::string packageId;   // directory name of the package under <project>/packages
    std::string directory;   // absolute path of <package>/platforms/<platform id>
};

/// Every template offered by a package installed in the project, sorted by
/// platform id then package id. Reads directories only; no package is loaded.
DEKI_EDITOR_API std::vector<PlatformTemplate> FindPlatformTemplates(const std::string& projectPath);

enum class AdoptPlatformResult
{
    AlreadyDefined,  // <project>/platforms/<id>/<id>.json exists; nothing touched
    Adopted,         // copied from a template
    NoTemplate,      // the project does not define it and no installed package offers it
    Ambiguous,       // two packages offer it; choosing would be a guess
    Failed           // the copy itself failed
};

/// Make `platformId` the project's own, from a template if it has to be.
/// `detail` names the package adopted from, the packages in conflict, or the
/// error. Never overwrites a platform the project already defines.
DEKI_EDITOR_API AdoptPlatformResult AdoptPlatformTemplate(const std::string& projectPath,
                                                          const std::string& platformId,
                                                          std::string& detail);

/// What every user of a platform calls first - opening a project on it,
/// switching to it, building for it: adopt it if it is only on offer, and load
/// the adopted copy into PlatformRegistry. A template's boot scene path is
/// relative to the PROJECT, so a platform that was chosen but never copied in
/// would build without one.
///
/// False, with the reason, only when adopting was needed and could not be
/// done. A platform nobody offers is not this function's error: it may come
/// from a board pack, and if it comes from nowhere the caller's own lookup
/// says so.
DEKI_EDITOR_API bool EnsureProjectPlatform(const std::string& projectPath, const std::string& platformId,
                                           std::string& err);

/// Register, in PlatformRegistry, every template the project has NOT adopted,
/// so a package's platforms can be seen and chosen before they are copied in.
/// Call before loading the project's own platforms, which override these.
DEKI_EDITOR_API void RegisterPlatformTemplates(const std::string& projectPath);

}  // namespace DekiEditor
