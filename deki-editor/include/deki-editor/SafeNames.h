#pragma once

#include <deki-editor/EditorExport.h>

// Names that arrive from outside the editor and become path segments or build
// tokens: registry ids and `owner/repo` slugs, manifest and lock-file names,
// board-pack ids, PACKAGE_TARGET read out of a cloned package.cmake.
//
// The rule is one allowlist, applied before the string touches the file
// system: `[A-Za-z0-9._-]`, no leading '.' or '-', never "." or "..", at most
// 100 characters. A name that fails is dropped or refused with the reason. It
// is never rewritten into a safe name, because the source (a registry someone
// else serves, a project someone sent you) is not trusted.
//
// IsInsideDirectory is the second layer for paths built from those names,
// checked before anything is deleted or handed to git.

#include <filesystem>
#include <string>

namespace DekiEditor::SafeNames
{

/// One path segment or identifier. Fills `reason` on rejection.
DEKI_EDITOR_API bool IsSafeName(const std::string& name, std::string& reason);

/// "owner/name" as the registry's `repo` field and the manifest's `name`
/// carry it: exactly one '/', both halves IsSafeName. A bare name (no '/')
/// is also accepted, since older lock files store only the repository name.
DEKI_EDITOR_API bool IsSafeRepoSlug(const std::string& slug, std::string& reason);

/// A serial port as a backend's deploy step passes it to its tools: `COM<n>`
/// on Windows, `/dev/<name>` elsewhere. Anything else (a space, a quote, an
/// option) is refused before it reaches a shell line.
DEKI_EDITOR_API bool IsSafeSerialPort(const std::string& port, std::string& reason);

/// A preprocessor define as a package declares it: `NAME` or `NAME=value`
/// with `[A-Za-z_][A-Za-z0-9_]*` and a value of `[A-Za-z0-9_.]*`.
DEKI_EDITOR_API bool IsSafeDefine(const std::string& define, std::string& reason);

/// True when `child` resolves to `parent` or somewhere below it. Both are
/// canonicalised weakly, since the child may not exist yet; if that fails,
/// the answer is false.
DEKI_EDITOR_API bool IsInsideDirectory(const std::filesystem::path& child, const std::filesystem::path& parent);

}  // namespace DekiEditor::SafeNames

// The package manager, the installers and the registry source live at global
// scope; this alias lets them use the same spelling.
namespace SafeNames = DekiEditor::SafeNames;
