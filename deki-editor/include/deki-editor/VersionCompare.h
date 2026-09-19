#pragma once

#include <deki-editor/EditorExport.h>

#include <string>
#include <vector>

namespace DekiEditor
{

/// Compare two dotted version strings ("3.1.1" vs "3.2.0").
/// Returns -1 if a < b, 0 if equal, 1 if a > b.
///
/// Missing trailing segments count as 0, so "1.1" == "1.1.0" and "1.1" < "1.1.2".
/// Segments parse with atoi, so a trailing suffix on a segment is ignored
/// ("1.2.0-rc1" compares equal to "1.2.0"): the package and framework manifests
/// this reads only ever carry numeric versions.
///
/// This lived as two byte-identical `static` copies, in the build base class and
/// in an installer, before the engine-version gate needed a third.
DEKI_EDITOR_API int CompareVersions(const std::string& a, const std::string& b);

/// The highest version in `versions` by CompareVersions, or "" when empty.
/// Registry and lock files list versions oldest-first, so taking the first
/// entry (which two version choosers did) installed the OLDEST release.
DEKI_EDITOR_API std::string HighestVersion(const std::vector<std::string>& versions);

}  // namespace DekiEditor
