// MCP tools that drive the editor shell rather than the open scene:
// project creation, the package manager, plugin rebuilds, and screenshot /
// frame-sequence capture of the editor's own window.
//
// McpTools.cpp stays focused on scene + asset editing; McpTools.Build.cpp on
// firmware. This file is the "operate the application" surface, which is what
// automation (docs screenshots, smoke runs) actually needs.

#include "McpTools.h"
#include "services/EditorInput.h"
#include <deki-editor/Paths.h>
#include "McpToolRegistry.h"
#include "../EditorApp.h"
#include "../core/Project.h"
#include <deki-editor/build/PlatformConfig.h>
#include "../panels/PackageManagerPanel.h"
#include "../services/ProjectTemplate.h"
#include "../services/HotReloadService.h"
#include "../services/EditorCapture.h"
#include <deki-editor/AssetPipeline.h>

#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

Project* RequireProject(EditorApp* editor)
{
    Project* project = editor->GetProject();
    if (!project)
        throw std::runtime_error("No project open");
    return project;
}

PackageManagerPanel* RequirePackagePanel(EditorApp* editor)
{
    auto* panel = editor->GetPackageManagerPanel();
    if (!panel)
        throw std::runtime_error("Package manager not available");
    return panel;
}

// Pull an optional pixel rect out of the arguments. Absent or degenerate means
// "whole window", which ResolveRect treats as the framebuffer.
EditorCapture::Region ReadRegion(const nlohmann::json& args)
{
    EditorCapture::Region r;
    r.x = args.value("x", 0.0f);
    r.y = args.value("y", 0.0f);
    r.w = args.value("width", 0.0f);
    r.h = args.value("height", 0.0f);
    return r;
}

}  // namespace

void RegisterEditorTools(McpToolRegistry& registry, EditorApp* editor)
{
    // ── Project ──────────────────────────────────────────────────────────────

    registry.Register({ "create_project",
                        "Create a new Deki project on disk and optionally open it. Mirrors the "
                        "New Project dialog: scaffolds project/{src,assets,platforms}, a default "
                        "Simulator platform, deki.json, .gitignore, and the default render pipeline.",
                        { { "type", "object" }, { "properties", { { "parent_path", { { "type", "string" }, { "description", "Directory the project folder is created in" } } }, { "name", { { "type", "string" }, { "description", "Project name (also the folder name)" } } }, { "platform_id", { { "type", "string" }, { "description", "Platform to generate build files for (see list_platforms). Omit for none." } } }, { "open", { { "type", "boolean" }, { "description", "Open the project after creating it (default true)" } } } } }, { "required", nlohmann::json::array({ "parent_path", "name" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string parent = args.at("parent_path").get<std::string>();
                            const std::string name = args.at("name").get<std::string>();

                            const PlatformConfig* platform = nullptr;
                            if (args.contains("platform_id") && args["platform_id"].is_string())
                            {
                                const std::string id = args["platform_id"].get<std::string>();
                                platform = PlatformRegistry::Instance().GetPlatform(id);
                                if (!platform)
                                    throw std::runtime_error("Unknown platform: " + id);
                            }

                            const fs::path projectPath = fs::path(parent) / name;
                            if (fs::exists(projectPath))
                                throw std::runtime_error("Path already exists: " + projectPath.string());

                            if (!ProjectTemplate::CreateProject(parent, name, platform))
                                throw std::runtime_error("Project creation failed (check permissions)");

                            const bool open = args.value("open", true);
                            if (open)
                                editor->OpenProjectShowingProgress(projectPath.string());

                            return { { "path", projectPath.string() }, { "opened", open } };
                        } });

    registry.Register({ "open_project",
                        "Open an existing Deki project (the folder containing deki.json). Restores "
                        "missing packages, imports assets, and queues a plugin build.",
                        { { "type", "object" }, { "properties", { { "path", { { "type", "string" }, { "description", "Project root path" } } } } }, { "required", nlohmann::json::array({ "path" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string path = args.at("path").get<std::string>();
                            if (!fs::exists(fs::path(path) / "deki.json"))
                                throw std::runtime_error("No deki.json at " + path);

                            if (!editor->OpenProjectShowingProgress(path))
                                throw std::runtime_error(editor->GetOpenProjectError());

                            auto* project = editor->GetProject();
                            return {
                                { "path", path },
                                { "opened", project != nullptr },
                                { "name", project ? project->GetName() : "" }
                            };
                        } });

    // ── Packages ──────────────────────────────────────────────────────────────

    registry.Register({ "list_packages",
                        "List Deki packages from the registry plus what the open project has "
                        "installed. Blocks on the registry fetch the first time (cached after).",
                        { { "type", "object" }, { "properties", { { "refresh", { { "type", "boolean" }, { "description", "Re-fetch the registry index before listing (default true)" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            RequireProject(editor);
                            auto* panel = RequirePackagePanel(editor);

                            bool registryOk = true;
                            if (args.value("refresh", true))
                                registryOk = panel->RefreshPackagesBlocking(30);

                            nlohmann::json packages = nlohmann::json::array();
                            for (const auto& pkg : panel->GetPackages())
                            {
                                packages.push_back({ { "id", pkg.id },
                                                     { "displayName", pkg.displayName },
                                                     { "description", pkg.description },
                                                     { "installed", pkg.isInstalled },
                                                     { "installedVersion", pkg.installedVersion },
                                                     { "latest", pkg.availableVersions.empty() ? "" : pkg.availableVersions[0] },
                                                     { "requires", pkg.requiredPackages } });
                            }
                            return { { "registryReachable", registryOk }, { "packages", packages } };
                        } });

    registry.Register({ "install_package",
                        "Install a package by registry id (e.g. \"deki-2d\") into the open project, "
                        "with its declared dependencies. Clones via git and queues a rebuild - this "
                        "blocks for as long as the clone takes.",
                        { { "type", "object" }, { "properties", { { "id", { { "type", "string" }, { "description", "Registry package id, e.g. deki-2d" } } }, { "version", { { "type", "string" }, { "description", "Tag or \"branch:name\". Omit for the registry's latest." } } } } }, { "required", nlohmann::json::array({ "id" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            RequireProject(editor);
                            auto* panel = RequirePackagePanel(editor);

                            const std::string id = args.at("id").get<std::string>();
                            const std::string version = args.value("version", "");

                            // Nothing can be installed before the registry index is known.
                            panel->RefreshPackagesBlocking(30);

                            std::string message;
                            const bool ok = panel->InstallPackageById(id, version, message);
                            if (!ok)
                                throw std::runtime_error(message);

                            return { { "id", id }, { "message", message } };
                        } });

    registry.Register({ "rebuild",
                        "Force a full rebuild of the project's package DLLs and plugin DLL, then "
                        "reload them. Blocks until the build finishes. Run this after installing "
                        "packages or editing C++ sources.",
                        { { "type", "object" }, { "properties", nlohmann::json::object() } },
                        [editor](const nlohmann::json&) -> nlohmann::json
                        {
                            auto* project = RequireProject(editor);
                            auto* hotReload = editor->GetHotReloadService();
                            if (!hotReload)
                                throw std::runtime_error("Hot reload service not available");

                            hotReload->ForceNextBuild();
                            hotReload->RequestBuildCheck(project->GetPath());
                            hotReload->RunPendingBuildNow();

                            return {
                                { "built", true },
                                { "hasErrors", hotReload->HasBuildErrors() }
                            };
                        } });

    // ── Assets ───────────────────────────────────────────────────────────────

    registry.Register({ "import_asset",
                        "Copy a file from anywhere on disk into the project's assets tree and "
                        "import it (assigns a GUID, converts to the engine's binary format).",
                        { { "type", "object" }, { "properties", { { "source_path", { { "type", "string" }, { "description", "File to import" } } }, { "dest_subdir", { { "type", "string" }, { "description", "Subdirectory under project/assets/, e.g. \"sprites\". Default: assets root." } } } } }, { "required", nlohmann::json::array({ "source_path" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* project = RequireProject(editor);
                            auto* pipeline = editor->GetAssetPipeline();
                            if (!pipeline)
                                throw std::runtime_error("Asset pipeline not available");

                            const fs::path source = args.at("source_path").get<std::string>();
                            if (!fs::exists(source))
                                throw std::runtime_error("No such file: " + source.string());

                            fs::path destDir = fs::path(GetAssetsDirectory(project->GetPath()));
                            if (args.contains("dest_subdir") && args["dest_subdir"].is_string())
                                destDir /= args["dest_subdir"].get<std::string>();

                            std::error_code ec;
                            fs::create_directories(destDir, ec);
                            const fs::path dest = destDir / source.filename();
                            fs::copy_file(source, dest, fs::copy_options::overwrite_existing, ec);
                            if (ec)
                                throw std::runtime_error("Copy failed: " + ec.message());

                            pipeline->ImportAllAssets();

                            return { { "path", dest.string() } };
                        } });

    // ── Dialogs ──────────────────────────────────────────────────────────────

    registry.Register({ "get_dialog",
                        "Report whether a modal dialog is holding the editor. MCP requests keep "
                        "being serviced while a modal is up, so a caller that does not check "
                        "this can issue a run of commands that all report success while the "
                        "editor is still on the scene the dialog is guarding.",
                        { { "type", "object" }, { "properties", nlohmann::json::object() } },
                        [editor](const nlohmann::json&) -> nlohmann::json
                        {
                            const bool unsaved = editor->IsUnsavedChangesDialogOpen();

                            // Any other modal still shows up as a live ImGui window, so report
                            // those too even though they have no typed answer yet.
                            nlohmann::json others = nlohmann::json::array();
                            for (const auto& p : EditorCapture::ListPanels())
                            {
                                if (!p.visible || p.docked) continue;
                                if (p.name == "DockSpace" || p.name.rfind("Debug##", 0) == 0) continue;
                                others.push_back(p.name);
                            }

                            return {
                                { "unsavedChangesDialog", unsaved },
                                { "blocking", unsaved },
                                { "floatingWindows", others }
                            };
                        } });

    registry.Register({ "answer_dialog",
                        "Answer a dialog without clicking. The unsaved-changes dialog: \"save\", \"discard\" or "
                        "\"cancel\" (\"save\" fails if the scene has no path yet - give it one with save_scene_as "
                        "first). The crash-recovery window (Recover Scenes): \"later\", which dismisses it and "
                        "keeps every recovered file for the next open. Applied on the next frame.",
                        { { "type", "object" }, { "properties", { { "answer", { { "type", "string" }, { "enum", nlohmann::json::array({ "save", "discard", "cancel", "later" }) } } } } }, { "required", nlohmann::json::array({ "answer" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string answer = args.at("answer").get<std::string>();
                            // Restoring or discarding recovered scenes overwrites or
                            // deletes a person's work, so neither is offered here.
                            if (answer == "later")
                            {
                                if (!editor->IsRecoveryDialogOpen())
                                    throw std::runtime_error("The crash-recovery window is not open");
                                editor->PostponeRecoveryDialog();
                                return { { "answered", answer } };
                            }
                            if (!editor->IsUnsavedChangesDialogOpen())
                                throw std::runtime_error("No unsaved-changes dialog is open");

                            DialogAnswer value;
                            if (answer == "save")
                                value = DialogAnswer::Save;
                            else if (answer == "discard")
                                value = DialogAnswer::Discard;
                            else if (answer == "cancel")
                                value = DialogAnswer::Cancel;
                            else
                                throw std::runtime_error("answer must be save, discard, cancel or later");

                            editor->AnswerUnsavedChangesDialog(value);
                            return { { "answered", answer } };
                        } });

    // ── Capture ──────────────────────────────────────────────────────────────

    registry.Register({ "list_panels",
                        "List the editor's live ImGui windows with their pixel rects. Use the "
                        "names as the `panel` argument to screenshot / capture_sequence / focus_panel. "
                        "A docked panel whose tab is not selected reports visible=false.",
                        { { "type", "object" }, { "properties", nlohmann::json::object() } },
                        [](const nlohmann::json&) -> nlohmann::json
                        {
                            nlohmann::json panels = nlohmann::json::array();
                            for (const auto& p : EditorCapture::ListPanels())
                            {
                                panels.push_back({ { "name", p.name },
                                                   { "x", p.x },
                                                   { "y", p.y },
                                                   { "width", p.w },
                                                   { "height", p.h },
                                                   { "visible", p.visible },
                                                   { "docked", p.docked } });
                            }
                            return { { "panels", panels } };
                        } });

    // list_panels and focus_panel only see windows that are being drawn, and a
    // panel closed in the layout is not: this is the Window menu's checkbox.
    registry.Register({ "open_panel",
                        "Open or close one of the editor's panels, as its Window menu entry does. "
                        "Ids are the panels' own (\"Build\", \"Console\", \"Hierarchy\", ...); the "
                        "error for an unknown id lists them. A panel opened this way appears in "
                        "list_panels from the next frame.",
                        { { "type", "object" },
                          { "properties",
                            { { "id", { { "type", "string" }, { "description", "Panel id" } } },
                              { "open", { { "type", "boolean" }, { "description", "false to close it (default true)" } } } } },
                          { "required", nlohmann::json::array({ "id" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            if (!editor)
                                throw std::runtime_error("open_panel needs the editor window");

                            const std::string id = args.at("id").get<std::string>();
                            const bool open = args.value("open", true);

                            std::string known;
                            for (IPanel* panel : editor->GetPanels())
                            {
                                if (id == panel->Id())
                                {
                                    panel->SetOpen(open);
                                    return { { "id", id }, { "open", open } };
                                }
                                known += (known.empty() ? "" : ", ") + std::string(panel->Id());
                            }
                            throw std::runtime_error("no panel '" + id + "'. Panels: " + known);
                        } });

    registry.Register({ "focus_panel",
                        "Select a docked panel's tab so it is actually drawn. Takes effect on the "
                        "next frame - screenshot it with wait_frames >= 2.",
                        { { "type", "object" }, { "properties", { { "name", { { "type", "string" }, { "description", "Panel name from list_panels" } } } } }, { "required", nlohmann::json::array({ "name" }) } },
                        [](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string name = args.at("name").get<std::string>();
                            EditorCapture::Get().RequestFocus(name);
                            return { { "focused", name } };
                        } });

    registry.Register({ "screenshot",
                        "Capture the editor window (or one panel) to a PNG. The capture is queued "
                        "and taken at the end of a later frame, so this returns as soon as it is "
                        "scheduled - poll for the file. The PNG is renamed into place, so seeing "
                        "the path means the file is complete.",
                        { { "type", "object" }, { "properties", { { "path", { { "type", "string" }, { "description", "Absolute output .png path" } } }, { "panel", { { "type", "string" }, { "description", "Panel name to crop to. Omit for the whole window." } } }, { "x", { { "type", "number" }, { "description", "Explicit rect, used when panel is omitted" } } }, { "y", { { "type", "number" } } }, { "width", { { "type", "number" } } }, { "height", { { "type", "number" } } }, { "padding", { { "type", "number" }, { "description", "Grow the rect by this many pixels on each side (default 0)" } } }, { "wait_frames", { { "type", "integer" }, { "description", "Frames to let pass before capturing (default 2)" } } } } }, { "required", nlohmann::json::array({ "path" }) } },
                        [](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string path = args.at("path").get<std::string>();
                            EditorCapture::Get().RequestScreenshot(
                                path,
                                args.value("panel", ""),
                                ReadRegion(args),
                                args.value("padding", 0.0f),
                                args.value("wait_frames", 2));
                            return { { "path", path }, { "status", "queued" } };
                        } });

    // Input. Positions are in the same pixels as a screenshot of the panel
    // (or of the window when no panel is named), so a point read off a
    // screenshot can be clicked as is. Queued and played one step per frame; a
    // screenshot asked for afterwards waits for them.
    auto pointSchema = [](const char* extraName, nlohmann::json extra) {
        nlohmann::json props = {
            { "panel", { { "type", "string" }, { "description", "Panel name from list_panels; x and y are then relative to its top-left corner. Omit for the whole window." } } },
            { "x", { { "type", "number" }, { "description", "Pixels from the left, as in a screenshot of the panel" } } },
            { "y", { { "type", "number" }, { "description", "Pixels from the top" } } } };
        if (extraName)
            for (auto it = extra.begin(); it != extra.end(); ++it)
                props[it.key()] = it.value();
        return nlohmann::json{ { "type", "object" }, { "properties", props },
                               { "required", nlohmann::json::array({ "x", "y" }) } };
    };
    auto checkPanel = [](const nlohmann::json& args) {
        const std::string panel = args.value("panel", "");
        if (!EditorInput::PanelExists(panel))
            throw std::runtime_error("panel '" + panel + "' is not drawn; see list_panels, and focus_panel a docked tab first");
        return panel;
    };

    registry.Register({ "click",
                        "Click in the editor at a point: moves the mouse there, then presses and "
                        "releases. Takes a few frames; a screenshot asked for after it shows the result.",
                        pointSchema("button",
                                    { { "button", { { "type", "string" }, { "enum", nlohmann::json::array({ "left", "right", "middle" }) }, { "description", "Default left" } } },
                                      { "double", { { "type", "boolean" }, { "description", "Double click (default false)" } } },
                                      { "hold_seconds", { { "type", "number" }, { "description", "Keep the button down this long before releasing (default 0), for press states and long presses" } } },
                                      { "ctrl", { { "type", "boolean" }, { "description", "Hold Ctrl during the click (multi-select)" } } },
                                      { "shift", { { "type", "boolean" }, { "description", "Hold Shift during the click (range select)" } } },
                                      { "alt", { { "type", "boolean" }, { "description", "Hold Alt during the click" } } } }),
                        [checkPanel](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string panel = checkPanel(args);
                            const std::string b = args.value("button", "left");
                            EditorInput::Button button = EditorInput::Button::Left;
                            if (b == "right")
                                button = EditorInput::Button::Right;
                            else if (b == "middle")
                                button = EditorInput::Button::Middle;
                            else if (b != "left")
                                throw std::runtime_error("button is left, right or middle");
                            const float x = args.at("x").get<float>(), y = args.at("y").get<float>();
                            EditorInput::Modifiers mods;
                            mods.ctrl = args.value("ctrl", false);
                            mods.shift = args.value("shift", false);
                            mods.alt = args.value("alt", false);
                            EditorInput::Get().RequestClick(panel, x, y, button, args.value("double", false) ? 2 : 1,
                                                            args.value("hold_seconds", 0.0f), mods);
                            return { { "status", "queued" }, { "x", x }, { "y", y } };
                        } });

    registry.Register({ "drag",
                        "Drag with a mouse button held: press at (x, y), move to (to_x, to_y) over a "
                        "number of frames, rest there, release. For moving gizmos, reparenting, "
                        "dragging assets between panels and docking tabs. Each end may be relative "
                        "to its own panel (panel / to_panel); both are fixed when the drag starts.",
                        pointSchema("to_x",
                                    { { "to_panel", { { "type", "string" }, { "description", "Panel the end point is relative to (default: the start's panel; \"\" for the window)" } } },
                                      { "to_x", { { "type", "number" }, { "description", "End point, pixels from the left" } } },
                                      { "to_y", { { "type", "number" }, { "description", "End point, pixels from the top" } } },
                                      { "frames", { { "type", "integer" }, { "description", "Frames to move over (default 12)" } } },
                                      { "hold_seconds", { { "type", "number" }, { "description", "Rest at the end point before releasing (default 0.2): drop targets and dock overlays need the pointer to settle" } } },
                                      { "button", { { "type", "string" }, { "enum", nlohmann::json::array({ "left", "right", "middle" }) }, { "description", "Default left" } } },
                                      { "ctrl", { { "type", "boolean" }, { "description", "Hold Ctrl for the whole drag" } } },
                                      { "shift", { { "type", "boolean" }, { "description", "Hold Shift for the whole drag" } } },
                                      { "alt", { { "type", "boolean" }, { "description", "Hold Alt for the whole drag" } } } }),
                        [checkPanel](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string panel = checkPanel(args);
                            // The end is in the start's panel unless told otherwise: a drag
                            // within one panel is the common case.
                            const nlohmann::json toArgs = { { "panel", args.contains("to_panel") ? args["to_panel"].get<std::string>() : panel } };
                            const std::string toPanel = checkPanel(toArgs);
                            const std::string b = args.value("button", "left");
                            EditorInput::Button button = EditorInput::Button::Left;
                            if (b == "right")
                                button = EditorInput::Button::Right;
                            else if (b == "middle")
                                button = EditorInput::Button::Middle;
                            else if (b != "left")
                                throw std::runtime_error("button is left, right or middle");
                            EditorInput::Modifiers mods;
                            mods.ctrl = args.value("ctrl", false);
                            mods.shift = args.value("shift", false);
                            mods.alt = args.value("alt", false);
                            EditorInput::Get().RequestDrag(panel, args.at("x").get<float>(), args.at("y").get<float>(), toPanel,
                                                           args.at("to_x").get<float>(), args.at("to_y").get<float>(), button,
                                                           args.value("frames", 12), args.value("hold_seconds", 0.2f), mods);
                            return { { "status", "queued" } };
                        } });

    registry.Register({ "hover",
                        "Move the mouse to a point and hold it there, for tooltips and hover "
                        "states. A tooltip appears after about half a second, so wait that long "
                        "before taking the screenshot, within the hold. The editor's frame rate "
                        "varies (often well over 60), so wait_frames alone is not a time.",
                        pointSchema("hold_seconds",
                                    { { "hold_seconds", { { "type", "number" }, { "description", "How long to keep the mouse there (default 3)" } } } }),
                        [checkPanel](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string panel = checkPanel(args);
                            EditorInput::Get().RequestHover(panel, args.at("x").get<float>(), args.at("y").get<float>(),
                                                            args.value("hold_seconds", 3.0f));
                            return { { "status", "queued" } };
                        } });

    registry.Register({ "scroll",
                        "Turn the mouse wheel over a point. Positive dy scrolls up (towards the top).",
                        pointSchema("dy",
                                    { { "dy", { { "type", "number" }, { "description", "Wheel steps, positive up" } } },
                                      { "dx", { { "type", "number" }, { "description", "Horizontal wheel steps (default 0)" } } } }),
                        [checkPanel](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string panel = checkPanel(args);
                            EditorInput::Get().RequestScroll(panel, args.at("x").get<float>(), args.at("y").get<float>(),
                                                             args.value("dx", 0.0f), args.value("dy", 0.0f));
                            return { { "status", "queued" } };
                        } });

    registry.Register({ "key",
                        "Press and release a key in the editor, with Ctrl, Shift or Alt held. Keys use "
                        "Dear ImGui's names: Enter, Escape, Tab, Space, Backspace, Delete, LeftArrow, "
                        "UpArrow, A..Z, 0..9, F1..F12.",
                        { { "type", "object" },
                          { "properties",
                            { { "key", { { "type", "string" } } },
                              { "ctrl", { { "type", "boolean" } } },
                              { "shift", { { "type", "boolean" } } },
                              { "alt", { { "type", "boolean" } } } } },
                          { "required", nlohmann::json::array({ "key" }) } },
                        [](const nlohmann::json& args) -> nlohmann::json
                        {
                            std::string error;
                            if (!EditorInput::Get().RequestKey(args.at("key").get<std::string>(), args.value("ctrl", false),
                                                               args.value("shift", false), args.value("alt", false), error))
                                throw std::runtime_error(error);
                            return { { "status", "queued" } };
                        } });

    registry.Register({ "type_text",
                        "Type text into whatever has keyboard focus, e.g. a field opened with click.",
                        { { "type", "object" },
                          { "properties", { { "text", { { "type", "string" } } } } },
                          { "required", nlohmann::json::array({ "text" }) } },
                        [](const nlohmann::json& args) -> nlohmann::json
                        {
                            EditorInput::Get().RequestText(args.at("text").get<std::string>());
                            return { { "status", "queued" } };
                        } });

    registry.Register({ "capture_sequence",
                        "Capture a PNG sequence (frame_0000.png …) into a directory, for assembling "
                        "into a GIF or video outside the editor. Writes done.txt with the frame "
                        "count when finished; poll for that file. Unlike screenshot it does not "
                        "wait for queued input: start it, then click (hold_seconds for a press), "
                        "and the recording shows the input as it plays out.",
                        { { "type", "object" }, { "properties", { { "dir", { { "type", "string" }, { "description", "Absolute output directory" } } }, { "frames", { { "type", "integer" }, { "description", "Number of PNGs to write" } } }, { "stride", { { "type", "integer" }, { "description", "Capture every Nth frame (default 1)" } } }, { "panel", { { "type", "string" }, { "description", "Panel name to crop to. Omit for the whole window." } } }, { "x", { { "type", "number" } } }, { "y", { { "type", "number" } } }, { "width", { { "type", "number" } } }, { "height", { { "type", "number" } } }, { "padding", { { "type", "number" } } }, { "wait_frames", { { "type", "integer" }, { "description", "Frames to let pass before starting (default 2)" } } } } }, { "required", nlohmann::json::array({ "dir", "frames" }) } },
                        [](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string dir = args.at("dir").get<std::string>();
                            const int frames = args.at("frames").get<int>();
                            if (frames <= 0)
                                throw std::runtime_error("frames must be > 0");

                            EditorCapture::Get().RequestSequence(
                                dir,
                                args.value("panel", ""),
                                ReadRegion(args),
                                args.value("padding", 0.0f),
                                frames,
                                args.value("stride", 1),
                                args.value("wait_frames", 2));

                            return { { "dir", dir }, { "frames", frames }, { "status", "queued" } };
                        } });
}

}  // namespace DekiEditor
