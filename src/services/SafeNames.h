#pragma once

// Names that arrive from outside the editor and become path segments or build
// tokens: registry ids and `owner/repo` slugs, manifest and lock-file names,
// board-pack ids, PACKAGE_TARGET read out of a cloned package.cmake.
//
// The rule is one allowlist, applied before the string touches the file
// system: `[A-Za-z0-9._-]`, no leading '.' or '-', never "." or "..", at most
// 100 characters. A name that fails is dropped or refused with the reason;
// nothing tries to "sanitize" it into something else, because the source
// (a registry someone else serves, a file in a project someone sent you) is
// not one to be helpful to.
//
// IsInsideDirectory is the second layer for the paths built from those names:
// asserted before anything is deleted or handed to git.

#include <filesystem>
#include <string>

namespace DekiEditor::SafeNames
{

/// One path segment / identifier. Fills `reason` on rejection.
bool IsSafeName(const std::string& name, std::string& reason);

/// "owner/name" as the registry's `repo` field and the manifest's `name`
/// carry it: exactly one '/', both halves IsSafeName. A bare name (no '/')
/// is accepted too, since older lock files store just the repository name.
bool IsSafeRepoSlug(const std::string& slug, std::string& reason);

/// A serial port as the flash step passes it to idf.py: `COM<n>` on Windows,
/// `/dev/<name>` elsewhere. Anything else (a space, a quote, an option) is
/// refused before it reaches the shell that runs the ESP-IDF export script.
bool IsSafeSerialPort(const std::string& port, std::string& reason);

/// A preprocessor define as a package declares it: `NAME` or `NAME=value`
/// with `[A-Za-z_][A-Za-z0-9_]*` and a value of `[A-Za-z0-9_.]*`.
bool IsSafeDefine(const std::string& define, std::string& reason);

/// True when `child` resolves to `parent` or somewhere below it. Both are
/// canonicalised weakly (the child may not exist yet); a failure to
/// canonicalise is "not inside".
bool IsInsideDirectory(const std::filesystem::path& child, const std::filesystem::path& parent);

}  // namespace DekiEditor::SafeNames

// The package manager, the installers and the registry source predate the
// DekiEditor namespace and live at global scope; the same spelling works there.
namespace SafeNames = DekiEditor::SafeNames;
