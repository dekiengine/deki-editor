#include "AgentCursorIntent.h"

#include <filesystem>

namespace DekiEditor
{

namespace
{

// The panels the pointer falls back to, by their ids (see IPanel::Id).
constexpr const char* kHierarchy = "###Hierarchy";
constexpr const char* kInspector = "###Inspector";
constexpr const char* kAssetBrowser = "###Asset Browser";

std::string Str(const nlohmann::json& args, const char* key)
{
    const auto it = args.find(key);
    return it != args.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// An object id, as a number or (from a --script) a string of digits.
bool Id(const nlohmann::json& args, const char* key, uint32_t& out)
{
    const auto it = args.find(key);
    if (it == args.end())
    {
        return false;
    }
    if (it->is_number_unsigned() || (it->is_number_integer() && it->get<int64_t>() >= 0))
    {
        out = static_cast<uint32_t>(it->get<uint64_t>());
        return true;
    }
    if (it->is_string())
    {
        const std::string s = it->get<std::string>();
        if (!s.empty() && s.find_first_not_of("0123456789") == std::string::npos && s.size() <= 10)
        {
            out = static_cast<uint32_t>(std::stoull(s));
            return true;
        }
    }
    return false;
}

float Num(const nlohmann::json& args, const char* key)
{
    const auto it = args.find(key);
    return it != args.end() && it->is_number() ? it->get<float>() : 0.0f;
}

// A component type as people read it: "Deki::ScreenAnchor" is ScreenAnchor.
// Labels only; the tools and the matching use the full name.
std::string ShortType(const std::string& type)
{
    const size_t at = type.rfind("::");
    return at == std::string::npos ? type : type.substr(at + 2);
}

std::string FileName(const std::string& path)
{
    const std::string name = std::filesystem::path(path).filename().string();
    return name.empty() ? path : name;
}

AgentIntent OnObject(const nlohmann::json& args, AgentIntent::Target target, std::string label)
{
    AgentIntent in;
    in.label = std::move(label);
    if (Id(args, "object_id", in.objectId))
    {
        in.hasObject = true;
        in.target = target;
        in.panel = (target == AgentIntent::Target::Object) ? kHierarchy : kInspector;
    }
    return in;
}

AgentIntent OnAsset(const std::string& path, std::string label)
{
    AgentIntent in;
    in.label = std::move(label);
    if (!path.empty())
    {
        in.target = AgentIntent::Target::Asset;
        in.assetPath = path;
        in.panel = kAssetBrowser;
    }
    return in;
}

AgentIntent LabelOnly(std::string label)
{
    AgentIntent in;
    in.label = std::move(label);
    return in;
}

AgentIntent AtPoint(const nlohmann::json& args, std::string label)
{
    AgentIntent in;
    in.label = std::move(label);
    in.target = AgentIntent::Target::Point;
    in.panel = Str(args, "panel");
    in.x = Num(args, "x");
    in.y = Num(args, "y");
    return in;
}

}  // namespace

AgentIntent IntentForTool(const std::string& tool, const nlohmann::json& args)
{
    using T = AgentIntent::Target;

    // Objects and their components
    if (tool == "create_object")
    {
        const std::string name = Str(args, "name").empty() ? "DekiObject" : Str(args, "name");
        AgentIntent in;
        uint32_t parent = 0;
        if (Id(args, "parent_id", parent))
        {
            in.target = T::Object;
            in.objectId = parent;
            in.hasObject = true;
            in.panel = kHierarchy;
            in.label = "Creating '" + name + "' under {object}";
        }
        else
        {
            in.target = T::Panel;
            in.panel = kHierarchy;
            in.label = "Creating '" + name + "'";
        }
        return in;
    }
    if (tool == "delete_object")
    {
        return OnObject(args, T::Object, "Deleting {object}");
    }
    if (tool == "duplicate_object")
    {
        return OnObject(args, T::Object, "Duplicating {object}");
    }
    if (tool == "reparent_object")
    {
        return OnObject(args, T::Object, "Moving {object} in the hierarchy");
    }
    if (tool == "rename_object")
    {
        return OnObject(args, T::Object, "Renaming {object} to '" + Str(args, "name") + "'");
    }
    if (tool == "set_position" || tool == "set_rotation" || tool == "set_scale")
    {
        const std::string field = tool.substr(4);  // position / rotation / scale
        const char* verb = field == "position" ? "Moving" : field == "rotation" ? "Rotating" : "Scaling";
        AgentIntent in = OnObject(args, T::Transform, std::string(verb) + " {object}");
        in.property = field;
        return in;
    }
    if (tool == "add_component" || tool == "remove_component")
    {
        const std::string comp = Str(args, "component_type");
        AgentIntent in = OnObject(args, T::Component,
                                  tool == "add_component" ? "Adding " + ShortType(comp) + " to {object}"
                                                          : "Removing " + ShortType(comp) + " from {object}");
        in.component = comp;
        return in;
    }
    if (tool == "set_property")
    {
        const std::string comp = Str(args, "component_type");
        const std::string prop = Str(args, "property_name");
        AgentIntent in = OnObject(args, T::Property, "Setting " + ShortType(comp) + "." + prop);
        in.component = comp;
        in.property = prop;
        return in;
    }
    if (tool == "set_selection")
    {
        AgentIntent in;
        const auto it = args.find("object_ids");
        const size_t count = (it != args.end() && it->is_array()) ? it->size() : 0;
        if (count == 0)
        {
            in.label = "Clearing the selection";
            return in;
        }
        const nlohmann::json first = { { "object_id", (*it)[0] } };
        in = OnObject(first, T::Object, count == 1 ? "Selecting {object}" : "Selecting " + std::to_string(count) + " objects");
        return in;
    }

    // History and scenes
    if (tool == "undo")
    {
        return LabelOnly("Undo");
    }
    if (tool == "redo")
    {
        return LabelOnly("Redo");
    }
    if (tool == "save_scene")
    {
        return LabelOnly("Saving the scene");
    }
    if (tool == "save_scene_as")
    {
        return LabelOnly("Saving the scene as " + FileName(Str(args, "path")));
    }
    if (tool == "new_scene")
    {
        return LabelOnly("New scene");
    }
    if (tool == "open_scene")
    {
        return LabelOnly("Opening " + FileName(Str(args, "path")));
    }
    if (tool == "set_project_setting")
    {
        return LabelOnly("Changing a project setting");
    }

    // Assets
    if (tool == "select_asset")
    {
        const std::string path = Str(args, "path");
        return path.empty() ? LabelOnly("Selecting an asset") : OnAsset(path, "Selecting " + FileName(path));
    }
    if (tool == "open_asset")
    {
        const std::string path = Str(args, "path");
        return OnAsset(path, "Opening " + FileName(path));
    }
    if (tool == "move_asset")
    {
        const std::string from = Str(args, "from");
        return OnAsset(from, "Moving " + FileName(from) + " to " + Str(args, "to"));
    }
    if (tool == "set_startup_scene")
    {
        const std::string path = Str(args, "path");
        return OnAsset(path, "Making " + FileName(path) + " the startup scene");
    }
    if (tool == "create_asset")
    {
        AgentIntent in = LabelOnly("Creating " + FileName(Str(args, "path")));
        in.target = T::Panel;
        in.panel = kAssetBrowser;
        return in;
    }
    if (tool == "import_asset")
    {
        AgentIntent in = LabelOnly("Importing " + FileName(Str(args, "source_path")));
        in.target = T::Panel;
        in.panel = kAssetBrowser;
        return in;
    }

    // Panels and input
    if (tool == "open_panel")
    {
        const std::string id = Str(args, "id");
        const bool open = !args.contains("open") || !args["open"].is_boolean() || args["open"].get<bool>();
        AgentIntent in = LabelOnly((open ? "Opening " : "Closing ") + id);
        // A panel being opened is not drawn yet; one being closed still is.
        if (!open && !id.empty())
        {
            in.target = T::Panel;
            in.panel = "###" + id;
        }
        return in;
    }
    if (tool == "focus_panel")
    {
        // "HIERARCHY###Hierarchy" reads as its id, the part after ###.
        const std::string name = Str(args, "name");
        const size_t hashes = name.find("###");
        AgentIntent in = LabelOnly("Showing " + (hashes == std::string::npos ? name : name.substr(hashes + 3)));
        in.target = T::Panel;
        in.panel = Str(args, "name");
        return in;
    }
    if (tool == "click")
    {
        const std::string button = Str(args, "button");
        const bool twice = args.contains("double") && args["double"].is_boolean() && args["double"].get<bool>();
        return AtPoint(args, twice ? "Double-clicking" : button == "right" ? "Right-clicking" : "Clicking");
    }
    if (tool == "drag")
    {
        return AtPoint(args, "Dragging");
    }
    if (tool == "hover")
    {
        return AtPoint(args, "Pointing");
    }
    if (tool == "scroll")
    {
        return AtPoint(args, "Scrolling");
    }
    if (tool == "key")
    {
        std::string combo;
        for (const char* mod : { "ctrl", "shift", "alt" })
        {
            if (args.contains(mod) && args[mod].is_boolean() && args[mod].get<bool>())
            {
                combo += std::string(1, static_cast<char>(mod[0] - 'a' + 'A')) + (mod + 1) + "+";
            }
        }
        return LabelOnly("Pressing " + combo + Str(args, "key"));
    }
    if (tool == "type_text")
    {
        return LabelOnly("Typing");
    }
    if (tool == "answer_dialog")
    {
        return LabelOnly("Answering the dialog: " + Str(args, "answer"));
    }

    // Project, packages, code and firmware: long jobs with nothing to point at.
    if (tool == "create_project")
    {
        return LabelOnly("Creating project " + Str(args, "name"));
    }
    if (tool == "open_project")
    {
        return LabelOnly("Opening project " + FileName(Str(args, "path")));
    }
    if (tool == "install_package")
    {
        return LabelOnly("Installing " + Str(args, "id"));
    }
    if (tool == "rebuild")
    {
        return LabelOnly("Rebuilding the code");
    }
    if (tool == "build_firmware")
    {
        return LabelOnly("Building firmware");
    }
    if (tool == "deploy_build")
    {
        return LabelOnly("Deploying the build");
    }
    if (tool == "clean_build")
    {
        return LabelOnly("Cleaning the build");
    }
    if (tool == "install_toolchain_component" || tool == "setup_toolchain_component")
    {
        return LabelOnly("Installing " + Str(args, "component_id"));
    }

    return {};
}

}  // namespace DekiEditor
