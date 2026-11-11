#include "ProjectChecks.h"

#include <deki-editor/FileIO.h>
#include <deki-editor/Localization.h>
#include <deki-editor/Paths.h>
#include <deki-editor/VersionCompare.h>

#include <deki/LogSystem.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

const char kStateKey[] = "projectChecks";

void ForEachFile(const fs::path& dir, const std::function<void(const fs::path&)>& visit)
{
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec) && (name == "generated" || name == "build" || name == ".git"))
        {
            it.disable_recursion_pending();
        }
        else if (it->is_regular_file(ec))
        {
            visit(it->path());
        }
    }
}

std::string Relative(const fs::path& file, const std::string& projectPath)
{
    std::error_code ec;
    return fs::relative(file, projectPath, ec).generic_string();
}

nlohmann::json ReadJson(const fs::path& file)
{
    std::ifstream in(file);
    return nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
}

std::string ReadText(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// Each package this project marks editable: its own code, which no upgrade
/// touches.
std::vector<fs::path> EditablePackages(const std::string& projectPath)
{
    std::vector<fs::path> dirs;
    const nlohmann::json manifest = ReadJson(GetPackageManifestFile(projectPath));
    if (!manifest.is_object() || !manifest.contains("packages") || !manifest["packages"].is_object())
    {
        return dirs;
    }
    for (const auto& [id, entry] : manifest["packages"].items())
    {
        if (entry.is_object() && entry.value("editable", false))
        {
            dirs.push_back(fs::path(GetPackagesDirectory(projectPath)) / id);
        }
    }
    return dirs;
}

bool IsCodeFile(const fs::path& file)
{
    const fs::path ext = file.extension();
    return ext == ".cpp" || ext == ".cc" || ext == ".h" || ext == ".hpp";
}

bool IsWordChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool LineHasName(const std::string& line, std::string_view name)
{
    const bool anyEnding = !name.empty() && name.back() == '*';
    if (anyEnding)
    {
        name.remove_suffix(1);
    }
    for (size_t at = line.find(name); at != std::string::npos; at = line.find(name, at + 1))
    {
        const size_t after = at + name.size();
        const bool startsWord = !IsWordChar(name.front()) || at == 0 || !IsWordChar(line[at - 1]);
        const bool endsWord = anyEnding || !IsWordChar(name.back()) || after == line.size() || !IsWordChar(line[after]);
        if (startsWord && endsWord)
        {
            return true;
        }
    }
    return false;
}

void FindNamesIn(const fs::path& dir, const std::vector<const char*>& names, const std::string& projectPath,
                 std::vector<std::string>& found)
{
    ForEachFile(dir,
                [&](const fs::path& file)
                {
                    if (!IsCodeFile(file))
                    {
                        return;
                    }
                    std::ifstream in(file);
                    std::string line;
                    for (int number = 1; std::getline(in, line); ++number)
                    {
                        for (const char* name : names)
                        {
                            if (LineHasName(line, name))
                            {
                                found.push_back(Relative(file, projectPath) + ":" + std::to_string(number));
                                break;
                            }
                        }
                    }
                });
}

std::vector<std::string> FindNames(const std::string& projectPath, const std::vector<const char*>& names)
{
    std::vector<std::string> found;
    FindNamesIn(GetSourceDirectory(projectPath), names, projectPath, found);
    for (const fs::path& package : EditablePackages(projectPath))
    {
        FindNamesIn(package, names, projectPath, found);
    }
    return found;
}

using ObjectVisitor = std::function<void(const nlohmann::json& object, const std::string& path)>;

void WalkObjects(const nlohmann::json& objects, const std::string& parent, int depth, const ObjectVisitor& visit)
{
    if (!objects.is_array() || depth > 256)  // a damaged file can nest without end
    {
        return;
    }
    for (const nlohmann::json& object : objects)
    {
        if (!object.is_object())
        {
            continue;
        }
        const auto name = object.find("name");
        const std::string path = parent + (name != object.end() && name->is_string() ? name->get<std::string>() : "?");
        visit(object, path);
        const auto children = object.find("children");
        if (children != object.end())
        {
            WalkObjects(*children, path + "/", depth + 1, visit);
        }
    }
}

/// `check` on every object of every scene; a true result lists the object.
std::vector<std::string> FindObjects(const std::string& projectPath, bool (*check)(const nlohmann::json& object))
{
    std::vector<std::string> found;
    ForEachFile(GetAssetsDirectory(projectPath),
                [&](const fs::path& file)
                {
                    if (file.extension() != ".scene")
                    {
                        return;
                    }
                    const nlohmann::json scene = ReadJson(file);
                    if (!scene.is_object() || !scene.contains("objects"))
                    {
                        return;
                    }
                    WalkObjects(scene["objects"], "", 0,
                                [&](const nlohmann::json& object, const std::string& path)
                                {
                                    if (check(object))
                                    {
                                        found.push_back(Relative(file, projectPath) + ": " + path);
                                    }
                                });
                });
    return found;
}

bool IsRotated(const nlohmann::json& object)
{
    const auto transform = object.find("transform");
    if (transform == object.end() || !transform->is_object())
    {
        return false;
    }
    const auto rotation = transform->find("rotation");
    return rotation != transform->end() && rotation->is_number() && std::fabs(rotation->get<double>()) > 1e-6;
}

bool HasTintedMesh(const nlohmann::json& object)
{
    const auto components = object.find("components");
    if (components == object.end() || !components->is_array())
    {
        return false;
    }
    for (const nlohmann::json& component : *components)
    {
        if (!component.is_object() || component.value("type", nlohmann::json()) != "Deki3D::MeshComponent")
        {
            continue;
        }
        const auto properties = component.find("properties");
        if (properties == component.end() || !properties->is_object())
        {
            continue;
        }
        const auto tint = properties->find("tintColor");
        if (tint != properties->end() && tint->is_string() && *tint != "#FFFFFFFF")
        {
            return true;
        }
    }
    return false;
}

std::vector<std::string> FindRotatedObjects(const std::string& projectPath)
{
    return FindObjects(projectPath, IsRotated);
}

std::vector<std::string> FindTintedMeshes(const std::string& projectPath)
{
    return FindObjects(projectPath, HasTintedMesh);
}

std::vector<std::string> FindOldEditablePackages(const std::string& projectPath)
{
    std::vector<std::string> found;
    for (const fs::path& package : EditablePackages(projectPath))
    {
        const nlohmann::json manifest = ReadJson(package / "package.json");
        if (!manifest.is_object() || !manifest.contains("minEngine") || !manifest["minEngine"].is_string())
        {
            continue;
        }
        const std::string minEngine = manifest["minEngine"];
        if (CompareVersions(minEngine, "0.22.0") < 0)
        {
            found.push_back(package.filename().string() + " (made for " + minEngine + ")");
        }
    }
    return found;
}

std::vector<std::string> FindEditorOpenGl(const std::string& projectPath)
{
    static const std::vector<const char*> kNames = { "glGenTextures",   "glBindTexture",    "glTexImage2D",
                                                     "glTexSubImage2D", "glTexParameteri",  "glDeleteTextures",
                                                     "glPixelStorei",   "GL/gl.h",          "glad/glad.h" };
    std::vector<std::string> found;
    for (const fs::path& package : EditablePackages(projectPath))
    {
        FindNamesIn(package / "editor", kNames, projectPath, found);
    }
    return found;
}

std::vector<std::string> FindModelTextureSettings(const std::string& projectPath)
{
    std::vector<std::string> found;
    ForEachFile(GetAssetsDirectory(projectPath),
                [&](const fs::path& file)
                {
                    const std::string name = file.filename().string();
                    const std::string suffix = ".obj.data";
                    if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
                    {
                        return;
                    }
                    const nlohmann::json data = ReadJson(file);
                    if (data.is_object() && data.contains("settings") && data["settings"].is_object() &&
                        data["settings"].contains("texture"))
                    {
                        found.push_back(Relative(file, projectPath));
                    }
                });
    return found;
}

std::vector<std::string> FindUndeclaredCapabilities(const std::string& projectPath)
{
    static const std::vector<const char*> kNames = { "DEKI_CAPABILITY_*" };
    std::vector<std::string> found;
    for (const fs::path& package : EditablePackages(projectPath))
    {
        const nlohmann::json manifest = ReadJson(package / "package.json");
        if (manifest.is_object() && manifest.contains("capabilityDefines"))
        {
            continue;
        }
        std::vector<std::string> uses;
        FindNamesIn(package, kNames, projectPath, uses);
        if (!uses.empty())
        {
            found.push_back(package.filename().string());
        }
    }
    return found;
}

/// Whether the text sets `"key": false`, whatever the spacing.
bool SetsFalse(const std::string& text, const std::string& key)
{
    const std::string quoted = "\"" + key + "\"";
    for (size_t at = text.find(quoted); at != std::string::npos; at = text.find(quoted, at + 1))
    {
        size_t value = at + quoted.size();
        while (value < text.size() && (std::isspace(static_cast<unsigned char>(text[value])) || text[value] == ':'))
        {
            ++value;
        }
        if (text.compare(value, 5, "false") == 0)
        {
            return true;
        }
    }
    return false;
}

std::vector<std::string> FindPlatformsDrawing2DOnGpu(const std::string& projectPath)
{
    std::vector<std::string> found;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(projectPath) / "platforms", ec))
    {
        const std::string id = entry.path().filename().string();
        const nlohmann::json platform = ReadJson(entry.path() / (id + ".json"));
        if (!platform.is_object() || !platform.contains("provides") || !platform["provides"].is_array())
        {
            continue;
        }
        const nlohmann::json& provides = platform["provides"];
        if (std::find(provides.begin(), provides.end(), "opengl") == provides.end())
        {
            continue;
        }
        bool turnedOff = false;
        ForEachFile(entry.path(),
                    [&](const fs::path& file)
                    { turnedOff = turnedOff || (file.extension() == ".scene" && SetsFalse(ReadText(file), "draw2DOnGpu")); });
        if (!turnedOff)
        {
            found.push_back(Relative(entry.path(), projectPath));
        }
    }
    return found;
}

std::vector<std::string> FindUnincludedVersionMacros(const std::string& projectPath)
{
    static const std::vector<const char*> kNames = { "DEKI_ENGINE_VERSION", "DEKI_OLDEST_SUPPORTED_PACKAGE" };
    std::vector<std::string> found;
    for (const std::string& use : FindNames(projectPath, kNames))
    {
        const std::string file = use.substr(0, use.rfind(':'));
        if (ReadText(fs::path(projectPath) / file).find("deki/EngineVersion.h") == std::string::npos)
        {
            found.push_back(use);
        }
    }
    return found;
}

// One entry for every breaking change a project has to migrate by hand. A
// project sees the entries newer than the version that last saved it. The list
// starts at 0.22.
const std::vector<ProjectCheckDefinition> kChecks = {
    { "old-editable-packages", "0.22.0", TrMark("Editable packages from an older release"),
      TrMark("Packages made for an engine older than 0.22 are refused, and upgrading leaves editable ones alone. Port "
             "each one and raise its minEngine."),
      FindOldEditablePackages },
    { "editor-opengl", "0.22.0", TrMark("OpenGL in a package's editor code"),
      TrMark("The editor has no OpenGL and packages cannot link it. Use EditorTexture instead."), FindEditorOpenGl },

    { "mesh-tint", "0.30.0", TrMark("Tinted meshes"),
      TrMark("Mesh tints had their channels swapped and now draw as written; a saved black tint now draws black. "
             "Check each colour."),
      FindTintedMeshes },
    { "mesh-indices-lights", "0.30.0", TrMark("Mesh indices and software lighting"),
      TrMark("Mesh3D::indices is read through IndexAt. RasterConfig's lightDirection and float ambient are now a "
             "colour ambient and lights. Port any of these that use them."),
      nullptr, { "lightDirection", "RasterConfig", ".indices", "->indices" } },
    { "model-texture-settings", "0.30.0", TrMark("Texture settings on models"),
      TrMark("A model's own texture settings are lost when it is saved. Set them on the images instead."),
      FindModelTextureSettings },
    { "asset-ref-headers", "0.30.0", TrMark("AssetRef and animation loading"),
      TrMark("A component's header needs the full type of each AssetRef it uses, not a forward declaration. The "
             "loader name \"FrameAnimationData\" is now \"Animation\"."),
      nullptr, { "AssetRef<", "FrameAnimationData" } },
    { "feature-defines", "0.30.0", TrMark("Code behind feature defines"),
      TrMark("The scan that strips unused packages ignores code inside #ifdef DEKI_FEATURE_*. If a feature is used "
             "only there, pin it on."),
      nullptr, { "DEKI_FEATURE_*" } },
    { "host-file-system", "0.30.0", TrMark("DesktopFileSystem"),
      TrMark("It moved to the engine as Deki::HostFileSystem. Rename it."), nullptr, { "DesktopFileSystem" } },
    { "capability-defines", "0.30.0", TrMark("Packages with undeclared capabilities"),
      TrMark("These packages use DEKI_CAPABILITY_ defines. Declare them as capabilityDefines in package.json; until "
             "then each build warns."),
      FindUndeclaredCapabilities },
    { "gpu-2d-default", "0.30.0", TrMark("2D drawn on the GPU"),
      TrMark("On these platforms 2D now draws on the GPU, so the CPU buffer holds none of it. If the game reads the "
             "screen back, turn off draw2DOnGpu in the display setup."),
      FindPlatformsDrawing2DOnGpu },
    { "translation-keys", "0.30.0", TrMark("Translations in your own packages"),
      TrMark("Inspector names changed, and translation keys follow them. Re-key these tables; "
             "tools/ci/check_translations.py lists what is missing."),
      [](const std::string& projectPath)
      {
          std::vector<std::string> found;
          std::error_code ec;
          for (const fs::path& package : EditablePackages(projectPath))
          {
              if (fs::is_directory(package / "editor" / "lang", ec))
              {
                  found.push_back(Relative(package / "editor" / "lang", projectPath));
              }
          }
          return found;
      } },
    { "editor-api-030", "0.30.0", TrMark("Removed editor API"),
      TrMark("PropertyTypeHandlers.h and PlatformConfig::CapabilityDefines are gone, and builders need rebuilding "
             "for the new ABI. Port these."),
      nullptr, { "PropertyTypeHandlers.h", "CapabilityDefines" } },

    { "mesh-renderer-gpu", "0.30.3", TrMark("Mesh3DRenderer::Gpu"), TrMark("It is now Mesh3DRenderer::GPU. Rename it."),
      nullptr, { "Mesh3DRenderer::Gpu" } },

    { "rotation-scenes", "0.40.0", TrMark("Rotated objects in scenes"),
      TrMark("Positive rotation now draws counter-clockwise. If one of these was rotated by eye, flip the sign of its "
             "rotation."),
      FindRotatedObjects },
    { "rotation-code", "0.40.0", TrMark("Code that sets a rotation"),
      TrMark("These lines now turn the other way on screen. Flip the sign where the direction matters."), nullptr,
      { "SetRotation" } },
    { "version-macros", "0.40.0", TrMark("Engine version macros"),
      TrMark("DEKI_ENGINE_VERSION and DEKI_OLDEST_SUPPORTED_PACKAGE are no longer compile definitions. Include "
             "<deki/EngineVersion.h> in these files."),
      FindUnincludedVersionMacros },
    { "deferred-destroy", "0.40.0", TrMark("Destroying objects and removing components"),
      TrMark("From a component's Awake, Start or Update these now free the object or component when that phase ends, "
             "not at once. Check that nothing here relies on its destructor having run by the next line."),
      nullptr, { "DestroyDekiObject", "RemoveComponent" } },
};

nlohmann::json ReadProjectFile(const std::string& projectPath)
{
    return ReadJson(fs::path(projectPath) / "deki.json");
}

bool WriteProjectFile(const std::string& projectPath, const nlohmann::json& project, std::string& why)
{
    return AtomicWriteFile((fs::path(projectPath) / "deki.json").string(), project.dump(2), why);
}

}  // namespace

std::vector<ProjectCheck> RunProjectChecks(const std::string& projectPath)
{
    return RunProjectChecks(projectPath, kChecks);
}

std::vector<ProjectCheck> RunProjectChecks(const std::string& projectPath,
                                           const std::vector<ProjectCheckDefinition>& definitions)
{
    std::vector<ProjectCheck> open;
    nlohmann::json project = ReadProjectFile(projectPath);
    if (!project.is_object())
    {
        return open;
    }

    const auto saved = project.find("lastSavedBy");
    const std::string savedBy = saved != project.end() && saved->is_string() ? saved->get<std::string>() : "";
    const auto stored = project.find(kStateKey);
    nlohmann::json state = stored != project.end() && stored->is_object() ? *stored : nlohmann::json::object();

    bool changed = false;
    for (const ProjectCheckDefinition& check : definitions)
    {
        const auto answer = state.find(check.id);
        const bool known = answer != state.end() && answer->is_boolean();
        if (known ? answer->get<bool>() : !savedBy.empty() && CompareVersions(check.version, savedBy) <= 0)
        {
            continue;
        }
        const bool scanned = check.scan || !check.names.empty();
        std::vector<std::string> findings = check.scan ? check.scan(projectPath) : FindNames(projectPath, check.names);
        const bool unaffected = scanned && findings.empty();
        if (!known || unaffected)
        {
            state[check.id] = unaffected;
            changed = true;
        }
        if (!unaffected)
        {
            open.push_back({ check.id, check.version, check.title, check.todo, std::move(findings) });
        }
    }

    if (changed)
    {
        project[kStateKey] = state;
        std::string why;
        if (!WriteProjectFile(projectPath, project, why))
        {
            DEKI_LOG_WARNING("Project checks: could not write deki.json: %s", why.c_str());
        }
    }

    // In the order the changes happened, however many releases were skipped.
    std::stable_sort(open.begin(), open.end(), [](const ProjectCheck& a, const ProjectCheck& b)
                     { return CompareVersions(a.version, b.version) < 0; });
    return open;
}

bool SetProjectCheckDone(const std::string& projectPath, const std::string& id, bool done, std::string& why)
{
    bool exists = false;
    for (const ProjectCheckDefinition& check : kChecks)
    {
        exists = exists || id == check.id;
    }
    if (!exists)
    {
        why = "There is no project check called '" + id + "'.";
        return false;
    }
    nlohmann::json project = ReadProjectFile(projectPath);
    if (!project.is_object())
    {
        why = "deki.json could not be read.";
        return false;
    }
    if (!project.contains(kStateKey) || !project[kStateKey].is_object())
    {
        project[kStateKey] = nlohmann::json::object();
    }
    project[kStateKey][id] = done;
    return WriteProjectFile(projectPath, project, why);
}

}  // namespace DekiEditor
