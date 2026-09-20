#include <deki-editor/build/BuilderDefinition.h>
#include <deki-editor/build/PlatformConfig.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace DekiEditor
{

static void ParseComponent(const json& j, ComponentDef& comp)
{
    comp.id = j.value("id", "");
    comp.displayName = j.value("displayName", "");
    comp.required = j.value("required", false);
    comp.canInstall = j.value("canInstall", false);
    comp.canSetup = j.value("canSetup", false);
    comp.installPath = j.value("installPath", "");
    comp.extractedName = j.value("extractedName", "");
    // Optional pinned digest for the component archive; see BuilderDefinition.h.
    comp.sha256 = j.value("sha256", "");
    comp.tooltip = j.value("tooltip", "");

    // Version check
    if (j.contains("versionCheck"))
    {
        const auto& vc = j["versionCheck"];
        comp.versionCheck.type = vc.value("type", "");
        comp.versionCheck.repo = vc.value("repo", "");
        comp.versionCheck.assetPattern = vc.value("assetPattern", "");
        comp.versionCheck.version = vc.value("version", "");
    }

    // Fallback
    if (j.contains("fallback"))
    {
        const auto& fb = j["fallback"];
        comp.fallback.version = fb.value("version", "");
        comp.fallback.url = fb.value("url", "");
        // The digest pins the fallback archive, so it may be written beside
        // that URL; a component-level "sha256" wins when both are present.
        if (comp.sha256.empty())
            comp.sha256 = fb.value("sha256", "");
    }

    // Detection - map of platform -> path pattern
    if (j.contains("detection") && j["detection"].is_object())
    {
        for (auto& [key, val] : j["detection"].items())
        {
            comp.detection[key] = val.get<std::string>();
        }
    }

    // Post-install
    if (j.contains("postInstall"))
    {
        const auto& pi = j["postInstall"];
        comp.postInstall.windows = pi.value("windows", "");
        comp.postInstall.unix_ = pi.value("unix", "");
    }

    // Sub-components
    if (j.contains("subComponents") && j["subComponents"].is_array())
    {
        for (const auto& sc : j["subComponents"])
        {
            SubComponentDef sub;
            sub.name = sc.value("name", "");
            sub.version = sc.value("version", "");
            sub.url = sc.value("url", "");
            sub.sha256 = sc.value("sha256", "");
            sub.extractedName = sc.value("extractedName", "");
            sub.targetName = sc.value("targetName", "");
            comp.subComponents.push_back(sub);
        }
    }
}

bool ParseBuilderDefinition(const std::string& jsonText, BuilderDefinition& outDef, std::string& outError)
{
    try
    {
        json j = json::parse(jsonText);

        outDef.id = j.value("id", "");
        outDef.name = j.value("name", "");
        outDef.family = j.value("family", "");
        outDef.buildDirectory = j.value("buildDirectory", "");

        if (j.contains("supportedTargets") && j["supportedTargets"].is_array())
        {
            for (const auto& t : j["supportedTargets"])
                outDef.supportedTargets.push_back(t.get<std::string>());
        }

        if (j.contains("components") && j["components"].is_array())
        {
            for (const auto& compJson : j["components"])
            {
                ComponentDef comp;
                ParseComponent(compJson, comp);
                if (!comp.id.empty())
                    outDef.components.push_back(comp);
            }
        }

        if (outDef.id.empty())
        {
            outError = "the definition has no \"id\"";
            return false;
        }
        return true;
    }
    catch (const std::exception& e)
    {
        outError = e.what();
        return false;
    }
}

bool LoadBuilderDefinition(const std::string& jsonPath, BuilderDefinition& outDef, std::string& outError)
{
    std::ifstream file(jsonPath, std::ios::binary);
    if (!file.is_open())
    {
        outError = "cannot open " + jsonPath;
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return ParseBuilderDefinition(text, outDef, outError);
}

}  // namespace DekiEditor
