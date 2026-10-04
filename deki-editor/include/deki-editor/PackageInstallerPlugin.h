#pragma once

// How a package contributes a package installer, the same way it supplies a
// build backend. The package manager names no installer itself.
//
// Exports a package's editor-side DLL may define, all optional. Most packages
// define none and provide no installer.
//
//   int                DekiInstallerGetCount();
//   IPackageInstaller* DekiInstallerCreate(int index);
//   void               DekiInstallerDestroy(IPackageInstaller*);
//
// Destroy is required alongside Create: the installer's vtable and its
// operator delete live in the package's module, so the editor must never
// `delete` one itself.
//
// No ABI handshake, unlike the builder plugin: PackageLoader has already
// loaded these DLLs and checked their reflection ABI.

#include <deki-editor/PackageContract.h>

// Not DEKI_EDITOR_API: that is dllimport inside a package, and these are
// definitions the package exports. Package DLLs build with export-all-symbols,
// so an empty macro is enough on Windows; the name keeps the intent readable
// and mirrors DEKI_BUILDER_API.
#ifdef _WIN32
#define DEKI_INSTALLER_API
#else
#define DEKI_INSTALLER_API __attribute__((visibility("default")))
#endif
