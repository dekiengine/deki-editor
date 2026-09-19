#pragma once

#include <deki-editor/EditorExport.h>
#include <deki-editor/EditorExtension.h>

#include <functional>
#include <string>

namespace DekiEditor
{

/**
 * @brief What a package's CLI tool may use of the running editor.
 */
struct CliToolContext
{
    /// Root of the open project; empty when none is open.
    std::string projectPath;
    /// Tell the asset pipeline a file under the project (relative path, e.g.
    /// "assets/fsm/bob.asset") was created or changed, so it is (re)imported.
    std::function<void(const std::string& relativePath)> refreshAsset;
};

/**
 * @brief A tool a package adds to the editor's command line and MCP server.
 *
 * The editor's own tools cover what the editor knows. A package that brings
 * its own kind of content - a node graph, say - brings the tools that author
 * it too, so that content can be made from `--tool` / `--script` like any
 * scene edit. Register with REGISTER_EDITOR like any editor extension; the
 * tool appears in `--list-tools` and is callable while its package is loaded.
 *
 * Arguments and results cross the package boundary as JSON text.
 */
class DEKI_EDITOR_API CliTool : public EditorExtension
{
   public:
    // Out of line (CliTool.cpp) so deki-editor.dll emits the vtable a
    // package's tool links against.
    ~CliTool() override;

    EditorType GetEditorType() const override { return EditorType::CliTool; }
    const char* GetName() const override { return GetToolName(); }

    /// Unique across the editor and every package, e.g. "graph_add_node".
    virtual const char* GetToolName() const = 0;
    virtual const char* GetToolDescription() const = 0;
    /// JSON Schema of the arguments object, as text.
    virtual const char* GetInputSchema() const = 0;

    /// Run with `argsJson` (an object). On success fill `resultJson` with a
    /// JSON value and return true; on failure fill `error` and return false.
    virtual bool Run(const CliToolContext& context, const std::string& argsJson, std::string& resultJson,
                     std::string& error) = 0;
};

}  // namespace DekiEditor
