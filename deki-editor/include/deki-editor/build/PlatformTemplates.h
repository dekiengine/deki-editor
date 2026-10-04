#pragma once

// Platform templates: the platforms a package offers a project.
//
// A target is more than a build backend. It needs a platform description
// (what the board or machine provides, which packages it always needs) and
// usually a boot scene that starts its display and input. Both name the
// target's own components, so both belong with the package that ships those
// components:
//
//     <package>/platforms/<platform id>/<platform id>.json
//     <package>/platforms/<platform id>/boot.scene          (and anything else)
//
// The editor knows no platform by name. A project gets a platform by adopting
// a template: a plain copy into <project>/platforms/, which from then on is
// the project's own file to edit.

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
    Ambiguous,       // two packages offer it; picking one would be a guess
    Failed           // the copy itself failed
};

/// Makes `platformId` the project's own, copying a template if needed.
/// `detail` names the package adopted from, the packages in conflict, or the
/// error. Never overwrites a platform the project already defines.
DEKI_EDITOR_API AdoptPlatformResult AdoptPlatformTemplate(const std::string& projectPath, const std::string& platformId,
                                                          std::string& detail);

/// Call first whenever a platform is used (opening a project on it, switching
/// to it, building for it): adopts it if it is only on offer, and loads the
/// adopted copy into PlatformRegistry. A template's boot scene path is
/// relative to the project, so a platform chosen but never copied in would
/// build without one.
///
/// False, with the reason, only when adopting was needed and failed. A
/// platform nobody offers is not an error here: it may come from a board pack,
/// and if it comes from nowhere the caller's own lookup reports it.
DEKI_EDITOR_API bool EnsureProjectPlatform(const std::string& projectPath, const std::string& platformId,
                                           std::string& err);

/// Registers in PlatformRegistry every template the project has not adopted,
/// so a package's platforms can be seen and chosen before they are copied in.
/// Call before loading the project's own platforms, which override these.
DEKI_EDITOR_API void RegisterPlatformTemplates(const std::string& projectPath);

// ---------------------------------------------------------------------------
// Where an adopted platform came from.
//
// Adopting is a copy, and a bare copy forgets its source: the project would
// never hear of a fix in the package, and a project that edited its copy
// would have no way back. So an adoption also writes
//
//     <project>/platforms/<platform id>/origin.json
//
// naming the package and holding the template as adopted: the platform JSON in
// full, a hash for every other file. Knowing what was adopted, what the
// project has now and what the package offers now gives:
//
//   modified          the project's copy differs from what was adopted
//   update available  the package offers something other than what was adopted
//   Update            take what the package changed, keep what the project
//                     changed; where both changed the same thing the project's
//                     wins and the result says so
//   Restore           the package's template as it is now, over the project's
//
// Platform JSON is compared setting by setting in the form PlatformToJson
// writes, so a copy the editor only re-saved is not "modified". Other files
// (the boot scene) are compared whole.
// ---------------------------------------------------------------------------

struct DEKI_EDITOR_API PlatformOriginStatus
{
    bool fromPackage = false;  // adopted from a package, or an installed package offers this id
    bool recorded = false;     // origin.json exists, so what was adopted is known
    std::string packageId;
    std::string adoptedVersion;     // the package's version when adopted; "" when not known
    bool packageInstalled = false;  // the package is installed and still offers this platform
    std::string offeredVersion;

    // Without a record (an older adoption), "modified" means "differs from
    // what the package offers", and an update cannot be told from an edit, so
    // only Restore is possible.
    bool modified = false;
    bool updateAvailable = false;
    std::vector<std::string> modifiedItems;  // setting names and file names
    std::vector<std::string> updatedItems;

    bool CanUpdate() const { return recorded && packageInstalled && updateAvailable; }
    bool CanRestore() const { return packageInstalled && (modified || updateAvailable); }
};

/// Where `platformId` came from and how it differs. Only reads files; the
/// registry is not touched.
DEKI_EDITOR_API PlatformOriginStatus GetPlatformOrigin(const std::string& projectPath, const std::string& platformId);

struct DEKI_EDITOR_API PlatformSyncResult
{
    bool ok = false;
    std::string error;
    std::vector<std::string> taken;  // taken from the package
    std::vector<std::string> kept;   // changed on both sides; the project's kept
};

DEKI_EDITOR_API PlatformSyncResult UpdatePlatformFromPackage(const std::string& projectPath,
                                                             const std::string& platformId);
DEKI_EDITOR_API PlatformSyncResult RestorePlatformFromPackage(const std::string& projectPath,
                                                              const std::string& platformId);

/// Goes up whenever the project's platforms or their origins may have changed
/// (a reload, an adoption, an update, a restore), so a panel can cache
/// GetPlatformOrigin instead of reading files every frame.
DEKI_EDITOR_API unsigned ProjectPlatformsRevision();
DEKI_EDITOR_API void NoteProjectPlatformsChanged();

/// Rebuilds PlatformRegistry for the project from scratch: board packs, then
/// what its installed packages offer, then its own. For when the installed
/// packages change under an open project, so a package's boards appear when
/// it is installed and go when it is removed.
DEKI_EDITOR_API void ReloadProjectPlatforms(const std::string& projectPath);

}  // namespace DekiEditor
