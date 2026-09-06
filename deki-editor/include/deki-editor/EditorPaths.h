#pragma once

#include <string>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

/**
 * EditorPaths
 *
 * Centralized path utilities for the Deki Editor.
 */
class DEKI_EDITOR_API EditorPaths
{
public:
    // User home directory (forward slashes, no trailing slash)
    static std::string GetUserHomeDir();

    // Editor data directory: ~/.deki-editor
    static std::string GetEditorDataDir();

    // Toolchains directory: ~/.deki-editor/toolchains
    static std::string GetToolchainsDir();

    // Board packs directory: ~/.deki-editor/board-packs
    static std::string GetBoardPacksDir();

    // Custom builders directory: ~/.deki-editor/builders
    static std::string GetBuildersDir();

    // Editor executable directory (where DekiEditor.exe lives, forward slashes)
    static std::string GetExecutableDir();
};

}  // namespace DekiEditor
