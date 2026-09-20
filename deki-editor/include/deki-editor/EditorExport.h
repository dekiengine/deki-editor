#pragma once

/**
 * @file EditorExport.h
 * @brief The one definition of deki-editor.dll's export macro.
 *
 * Every public header in this directory needs this and used to paste its own
 * copy of the ifdef ladder below - 24 copies of the same six lines. Three extra
 * spellings had also accumulated (DEKI_THEME_API, DEKI_NAMING_API,
 * DEKI_COMMAND_API) that expanded to exactly the same thing, and CommandHistory.h
 * used one it never defined, compiling only because Command.h happened to be
 * included first.
 *
 * Mirrors deki-engine's src/DekiExport.h.
 */

#ifdef _WIN32
#ifdef DEKI_EDITOR_EXPORTS
#define DEKI_EDITOR_API __declspec(dllexport)
#else
#define DEKI_EDITOR_API __declspec(dllimport)
#endif
#else
#define DEKI_EDITOR_API
#endif
