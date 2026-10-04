#pragma once

#include <string>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

/// Where the editor keeps its files.
class DEKI_EDITOR_API EditorPaths
{
public:
    /// The user's home folder (forward slashes, no trailing slash).
    static std::string GetUserHomeDir();

    /// ~/.deki-editor
    static std::string GetEditorDataDir();

    /// ~/.deki-editor/toolchains
    static std::string GetToolchainsDir();

    /// ~/.deki-editor/board-packs
    static std::string GetBoardPacksDir();

    /// ~/.deki-editor/builders, for custom builders.
    static std::string GetBuildersDir();

    /// The folder of the editor executable (DekiEditor.exe), forward slashes.
    static std::string GetExecutableDir();
};

}  // namespace DekiEditor
