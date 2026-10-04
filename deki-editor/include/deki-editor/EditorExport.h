#pragma once

// The one definition of deki-editor.dll's export macro. Every public header
// in this folder includes it; do not define another copy or spelling.
// Mirrors deki-engine's src/DekiExport.h.

#ifdef _WIN32
#ifdef DEKI_EDITOR_EXPORTS
#define DEKI_EDITOR_API __declspec(dllexport)
#else
#define DEKI_EDITOR_API __declspec(dllimport)
#endif
#else
#define DEKI_EDITOR_API
#endif
