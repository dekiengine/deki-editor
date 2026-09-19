#include "ProjectScaffold.h"

#include "PackageManifest.h"
#include <deki-editor/build/ProjectPaths.h>

#include "AuthoredFormat.h"

#include <deki-editor/AssetData.h>
#include <deki-editor/FileIO.h>
#include <deki-editor/Paths.h>
#include <deki-editor/EditorPaths.h>
#include <deki-editor/SafeNames.h>

#include <fstream>

#include <filesystem>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace DekiEditor::ProjectScaffold
{

namespace
{

// Component types of the packages a new project starts with: the qualified
// class name is the scene file's "type" key (see deki/TypeId.h).
constexpr const char* kCameraType = "CameraComponent";      // deki-rendering
constexpr const char* kGradientType = "GradientComponent";  // deki-2d
constexpr const char* kTextType = "TextComponent";          // deki-2d

json Transform(double x, double y)
{
    return json{ { "rotation", 0.0 }, { "scaleX", 1.0 }, { "scaleY", 1.0 }, { "x", x }, { "y", y } };
}

json Object(uint32_t id, const char* name, json components, json children, double x = 0.0, double y = 0.0)
{
    return json{ { "active", true },
                 { "children", std::move(children) },
                 { "components", std::move(components) },
                 { "id", id },
                 { "name", name },
                 { "transform", Transform(x, y) } };
}

json Component(const char* type, const char* className, json properties)
{
    return json{ { "class", className }, { "properties", std::move(properties) }, { "type", type } };
}

bool WriteText(const fs::path& path, const std::string& text, std::string& err)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    return AtomicWriteFile(path.string(), text, err);
}

}  // namespace

std::string DefaultStarterPath()
{
    return (fs::path(EditorPaths::GetExecutableDir()) / "resources" / "starter.json").string();
}

bool LoadProjectStarter(const std::string& path, ProjectStarter& out, std::string& err)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        err = "cannot open the project starter " + path;
        return false;
    }

    const json j = json::parse(file, nullptr, /*allow_exceptions*/ false);
    if (!j.is_object())
    {
        err = path + " is not a JSON object";
        return false;
    }

    ProjectStarter starter;
    if (j.contains("packages") && j["packages"].is_array())
    {
        for (const auto& id : j["packages"])
        {
            std::string reason;
            if (!id.is_string() || !SafeNames::IsSafeName(id.get<std::string>(), reason))
            {
                err = path + ": \"packages\" holds something that is not a package id";
                return false;
            }
            starter.packageIds.push_back(id.get<std::string>());
        }
    }
    if (starter.packageIds.empty())
    {
        err = path + " declares no packages";
        return false;
    }

    starter.platformId = j.value("platform", std::string());
    std::string reason;
    if (!SafeNames::IsSafeName(starter.platformId, reason))
    {
        err = path + ": \"platform\" is not a platform id: " + reason;
        return false;
    }

    out = std::move(starter);
    return true;
}

bool WriteDekiJson(const std::string& projectPath, const std::string& projectName,
                   const std::string& activePlatformId, std::string& err)
{
    json j;
    j["name"] = projectName;
    j["version"] = "1.0";
    j["engine"] = "deki";
    // Both carry the same thing. Project::Save rewrites them on every save;
    // the load gate reads lastSavedBy (see Project::Load). There is no fallback
    // for a missing DEKI_ENGINE_VERSION on purpose: it is always defined, and
    // the one that used to stand here wrote a literal "1.0.0" that then
    // outranked every real version.
    j["lastSavedBy"] = DEKI_ENGINE_VERSION;
    j["engineVersion"] = DEKI_ENGINE_VERSION;
    j["structure"] = ProjectPaths::kStructureVersion;
    // The scaffold's scene positions its objects and its scripts may move them,
    // so the project declares the 2D transform for its own code (COMPATIBILITY.md).
    j["tags"] = nlohmann::json::array({ "transform_2d" });
    j["startupScene"] = kStartupScene;
    j["activePlatformId"] = activePlatformId;
    // Adopted from assets/rendering on first open (EditorProjectManager), the
    // same way an existing project without one is handled.
    j["renderPipelineGuid"] = "";
    j["forceIncludeGuids"] = json::array();
    j["flashGuids"] = json::array();
    return WriteText(fs::path(projectPath) / "deki.json", j.dump(2), err);
}

bool WritePackageManifestFor(const std::string& projectPath,
                             const std::vector<std::string>& packageIds, std::string& err)
{
    return WritePackageManifest((fs::path(projectPath) / "deki-packages.json").string(),
                                BuildPackageManifestForIds(packageIds), err);
}

bool WriteGitIgnore(const std::string& projectPath, std::string& err)
{
    // One ignored folder for editor state instead of an inventory of them. A
    // previous version listed twelve entries and two were wrong — one named a
    // directory nothing creates, another hid a tracked file — because nothing
    // forced the list to track reality. With all generated state under
    // generated/, there is no list to drift.
    const std::string text =
        "# Everything the Deki Editor generates. Safe to delete; recreated on open/build.\n"
        "generated/\n"
        "\n"
        "# Package sources — Package Manager clones of the source repos\n"
        "packages/deki-*/\n"
        "\n"
        "# Exported asset packs (--export / the Build panel); rebuilt from assets/\n"
        "storage/\n"
        "\n"
        "# IDE / tooling\n"
        ".vscode/\n"
        ".idea/\n"
        ".vs/\n"
        ".claude/\n"
        ".cache/\n"
        "*.swp\n";
    return WriteText(fs::path(projectPath) / ".gitignore", text, err);
}

bool WriteRenderPipeline(const std::string& projectPath, std::string& err)
{
    // No passes. This used to write a "clip2d" entry, with a comment saying a
    // pipeline needs it or the project renders black — which was true once and
    // is not now: clipping became a builtin of Standard2DRenderer, which
    // pushes and pops a clip rect around any object exposing IClipProvider,
    // and nothing registers a pass by that name. So every new project logged
    // "No pass registered for 'clip2d'" on its first run while rendering
    // perfectly, which is how a user learns to ignore warnings.
    //
    // Passes are opt-in extras (the tilemap pass, deki-3d's mesh pass), and
    // most attach themselves. deki-rendering still recognises a clip2d entry
    // from an older project without complaining; see IsBuiltinPassName there.
    json j;
    j[Deki::AuthoredFormat::VersionKey] = Deki::AuthoredFormat::AssetVersion;
    j["type"] = "RenderPipeline";
    j["renderer"] = "standard2d";
    j["passes"] = json::array();
    const fs::path path = fs::path(GetAssetsDirectory(projectPath)) / "rendering" / "Standard2D.asset";
    return WriteText(path, j.dump(2), err);
}

std::string InstallStarterFont(const std::string& projectPath, const std::string& editorFontsDir,
                               std::string& err)
{
    const fs::path src = fs::path(editorFontsDir) / kStarterFontFile;
    std::error_code ec;
    if (!fs::exists(src, ec))
    {
        err = "editor font not found: " + src.string();
        return "";
    }

    const fs::path fontsDir = fs::path(GetAssetsDirectory(projectPath)) / "fonts";
    fs::create_directories(fontsDir, ec);
    const fs::path dst = fontsDir / kStarterFontFile;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        err = "could not copy " + src.string() + ": " + ec.message();
        return "";
    }

    // The sidecar the asset pipeline would write on import, with one size
    // baked so the text renders on the first open.
    AssetData data;
    data.guid = GenerateGuid();
    data.settings["fontSettings"] = json{ { "firstChar", 32 },
                                          { "lastChar", 126 },
                                          { "sizes", json::array({ kStarterFontSize }) } };
    if (!SaveAssetData(GetAssetDataPath(dst.string()), data))
    {
        err = "could not write the font sidecar for " + dst.string();
        return "";
    }
    return data.guid;
}

bool WriteStarterScene(const std::string& projectPath, const std::string& fontSourceGuid,
                       std::string& err)
{
    // A 320x240 display at the default pixels-per-metre is 20x15 world units,
    // so the gradient fills it exactly.
    json children = json::array();

    children.push_back(Object(2, "Camera",
        json::array({ Component(kCameraType, "CameraComponent",
            json{ { "pixelsPerMeter", 0.0 }, { "clearColor", 202250495 }, { "pixelSnap", false } }) }),
        json::array()));

    children.push_back(Object(3, "Sky",
        json::array({ Component(kGradientType, "GradientComponent",
            json{ { "width", 20.0 }, { "height", 15.0 }, { "angle", 90.0 },
                  { "gradientType", 0 }, { "stopCount", 4 },
                  { "stop1Color", 390692863 }, { "stop1Position", 0.0 },
                  { "stop2Color", 779860223 }, { "stop2Position", 0.45 },
                  { "stop3Color", 1656020223 }, { "stop3Position", 0.78 },
                  { "stop4Color", 2849894655 }, { "stop4Position", 1.0 },
                  { "ditherMode", 3 }, { "ditherScale", 1 }, { "sortingOrder", 0 },
                  { "pixelSnap", true } }) }),
        json::array()));

    if (!fontSourceGuid.empty())
    {
        // The baked variant's GUID is derived from (font, size) exactly as the
        // asset exporter and the editor's font-resolve hook derive it.
        const std::string variantGuid =
            GenerateDeterministicGuid(fontSourceGuid + ":" + std::to_string(kStarterFontSize));
        children.push_back(Object(4, "Hello",
            json::array({ Component(kTextType, "TextComponent",
                json{ { "text", "Hello, Deki" },
                      { "fontSize", kStarterFontSize },
                      { "font", json{ { "guid", variantGuid }, { "source", fontSourceGuid } } },
                      { "color", 4294967295u },
                      { "align", 1 },          // TextAlign::Center
                      { "verticalAlign", 1 },
                      { "width", 20.0 }, { "height", 2.0 },
                      { "sortingOrder", 1 }, { "pixelSnap", true } }) }),
            json::array()));
    }

    json scene;
    scene["objects"] = json::array({ Object(1, "Root", json::array(), std::move(children)) });

    const fs::path path = fs::path(GetAssetsDirectory(projectPath)) / kStartupScene;
    return WriteText(path, scene.dump(2), err);
}

}  // namespace DekiEditor::ProjectScaffold
