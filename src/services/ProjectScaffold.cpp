#include "ProjectScaffold.h"

#include "PackageManifest.h"
#include "core/ProjectPaths.h"

#include "AuthoredFormat.h"

#include <deki-editor/AssetData.h>
#include <deki-editor/FileIO.h>
#include <deki-editor/Paths.h>

#include <filesystem>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace DekiEditor::ProjectScaffold
{

namespace
{

// Component type GUIDs, as declared by DEKI_COMPONENT in the packages a new
// project starts with. They are the scene file's "type" key and never change.
constexpr const char* kCameraGuid = "146999a7-398f-4e52-a7c7-1e6a78bfb9c4";     // deki-rendering CameraComponent
constexpr const char* kGradientGuid = "8d84cca3-7eb9-4b92-ba49-968ceec203c8";   // deki-2d GradientComponent
constexpr const char* kTextGuid = "5447ea24-d11f-4161-ae10-2f11e0a18d09";       // deki-2d TextComponent

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

json Component(const char* guid, const char* className, json properties)
{
    return json{ { "class", className }, { "properties", std::move(properties) }, { "type", guid } };
}

bool WriteText(const fs::path& path, const std::string& text, std::string& err)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    return AtomicWriteFile(path.string(), text, err);
}

}  // namespace

std::vector<std::string> DefaultPackageIds()
{
    return { "deki-rendering", "deki-2d", "deki-input", "deki-sdl3-integration", "deki-desktop-integration" };
}

bool WriteDekiJson(const std::string& projectPath, const std::string& projectName,
                   const std::string& activePlatformId, std::string& err)
{
    json j;
    j["name"] = projectName;
    j["version"] = "1.0";
    j["engine"] = "deki";
#ifdef DEKI_ENGINE_VERSION
    j["engineVersion"] = DEKI_ENGINE_VERSION;
#else
    j["engineVersion"] = "1.0.0";
#endif
    j["structure"] = ProjectPaths::kStructureVersion;
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
    // The clip2d pass has to be here. A pipeline with an empty pass list is
    // valid but draws nothing, so a brand-new project would enter play mode
    // and render a black screen with no error to explain it.
    json j;
    j[Deki::AuthoredFormat::VersionKey] = Deki::AuthoredFormat::AssetVersion;
    j["type"] = "RenderPipeline";
    j["renderer"] = "standard2d";
    j["passes"] = json::array({ json{ { "type", "clip2d" }, { "settings", json::object() } } });
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
        json::array({ Component(kCameraGuid, "CameraComponent",
            json{ { "pixelsPerMeter", 0.0 }, { "clearColor", 202250495 }, { "pixelSnap", false } }) }),
        json::array()));

    children.push_back(Object(3, "Sky",
        json::array({ Component(kGradientGuid, "GradientComponent",
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
            json::array({ Component(kTextGuid, "TextComponent",
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
