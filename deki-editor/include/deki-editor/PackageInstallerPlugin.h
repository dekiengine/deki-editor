#pragma once

// How a package contributes a package installer.
//
// PackageManager used to construct ESPIDFInstaller by name, so the editor knew
// one framework's package manager. A package supplies its own now, the same
// way it supplies a build backend, and the manager names none.
//
// Exports a package's editor-side DLL may define; all optional. A package that
// defines none is simply not an installer provider, which is almost all of
// them.
//
//   int                DekiInstaller_GetCount();
//   IPackageInstaller* DekiInstaller_Create(int index);
//   void               DekiInstaller_Destroy(IPackageInstaller*);
//
// Destroy is required alongside Create: the installer's vtable and its
// operator delete live in the package's module, so the editor must never
// `delete` one itself.
//
// No ABI handshake here, unlike the builder plugin: these DLLs are already
// loaded by PackageLoader, which checks the reflection ABI before anything is
// asked of them.

#include <deki-editor/PackageContract.h>

// NOT DEKI_EDITOR_API: that is dllimport inside a package, and these are
// definitions the package EXPORTS. Package DLLs build with export-all-symbols,
// so an empty macro suffices; the spelling keeps the intent readable and
// mirrors DEKI_BUILDER_API.
#ifdef _WIN32
#define DEKI_INSTALLER_API
#else
#define DEKI_INSTALLER_API __attribute__((visibility("default")))
#endif
