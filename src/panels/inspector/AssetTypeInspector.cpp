#include "AssetTypeInspector.h"
#include <deki-editor/Paths.h>
#include <deki-editor/EditorTheme.h>
#include "TextureInspector.h"
#include "SceneInspector.h"
#include "InspectorUtils.h"
#include "../../EditorApp.h"
#include "../../core/Project.h"
#include <deki-editor/CommandHistory.h>
#include "../../core/commands/AssetCommands.h"
#include "../../services/AssetExporter.h"
#include <deki-editor/build/PlatformConfig.h>
#include "../SceneViewPanel.h"
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/SubAsset.h>
#include <deki-editor/AssetDatabase.h>
#include <deki-editor/EditorAssets.h>
#include <deki-editor/EditorExtension.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <deki-editor/EditorUI.h>
#include <deki/assets/AssetManager.h>
#include "imgui.h"
#include "imgui_internal.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

namespace DekiEditor
{

AssetTypeInspector::AssetTypeInspector(EditorApp* app, TextureInspector* textureInspector)
    : m_App(app), m_TextureInspector(textureInspector), m_SceneInspector(std::make_unique<SceneInspector>(app))
{
}

AssetTypeInspector::~AssetTypeInspector() = default;

void AssetTypeInspector::Draw()
{
    const std::string& assetPath = m_App->GetSelectedAssetPath();

    namespace fs = std::filesystem;
    fs::path path(assetPath);
    std::string ext = path.extension().string();

    // Get GUID for this asset from pipeline (parent asset GUID)
    std::string pipelineGuid;
    auto* pipeline = m_App->GetAssetPipeline();
    if (pipeline)
    {
        const AssetInfo* info = pipeline->GetAssetInfo(assetPath);
        if (info)
            pipelineGuid = info->guid;
    }

    // Selected GUID may differ from pipeline GUID for sub-assets
    std::string selectedGuid = m_App->GetSelectedAssetGuid();
    std::string assetGuid = pipelineGuid.empty() ? selectedGuid : pipelineGuid;

    // Sub-asset with preview (e.g., font atlas) - skip editable properties
    bool isSubAssetWithPreview = false;
    if (pipeline && !selectedGuid.empty() && m_App->GetSelectedAssetHasCached())
    {
        const auto* subInfo = pipeline->GetSubAsset(selectedGuid);
        if (subInfo && subInfo->hasPreview)
            isSubAssetWithPreview = true;
    }

    // Baked cache-only asset (e.g., font atlas from deploy analysis) - no source path
    bool isBakedCacheAsset = assetPath.empty() && m_App->GetSelectedAssetHasCached();

    // Typed JSON asset whose handler compiles to a texture (e.g. ProceduralSprite) -
    // gets the anchored preview of its baked DTEX cache like a raster image
    bool isCompiledTextureAsset = (ext == ".asset" || ext == ".dpack") && CompilesToTexture(assetPath);

    // Texture inspector with anchored preview + scrollable sections
    if (TextureInspector::IsTextureFile(assetPath) || isBakedCacheAsset || isCompiledTextureAsset)
    {
        m_TextureInspector->DrawPreview();
        m_TextureInspector->BeginScrollRegion();

        if (!isSubAssetWithPreview && !isBakedCacheAsset)
        {
            // Handle .asset and .dpack files with type-specific editors
            if (ext == ".asset" || ext == ".dpack")
            {
                DrawAssetTypeEditor(assetPath, assetGuid);
            }
            // Check file inspector registry for native file types (e.g., .ttf, .otf)
            else if (auto* fileInspector = FileInspectorRegistry::Instance().GetInspector(ext))
            {
                fileInspector->OnInspectorGUI(assetPath, assetGuid);
            }
        }

        // Format/chroma-key sidecar settings only apply to imported images;
        // the procedural compile path bakes RGB565A8 and ignores the sidecar
        if (!isCompiledTextureAsset)
            m_TextureInspector->DrawSettings();

        if (!isSubAssetWithPreview && !isBakedCacheAsset)
            DrawDeployProperties(assetGuid);

        m_TextureInspector->EndScrollRegion();
    }
    else
    {
        bool isScene = ext == ".scene";

        if (isScene)
            m_SceneInspector->DrawPreview(assetPath);

        if (!isSubAssetWithPreview)
        {
            if (ext == ".asset" || ext == ".dpack")
            {
                DrawAssetTypeEditor(assetPath, assetGuid);
            }
            else if (auto* fileInspector = FileInspectorRegistry::Instance().GetInspector(ext))
            {
                fileInspector->OnInspectorGUI(assetPath, assetGuid);
            }

            DrawDeployProperties(assetGuid);
        }

    }
}

bool AssetTypeInspector::CompilesToTexture(const std::string& assetPath)
{
    if (assetPath == m_CompileTargetPath)
        return m_CompilesToTexture;

    // Only cache definitive answers; unreadable/broken files retry next frame
    // (the type editor shows the parse error, and the file may be mid-write)
    m_CompileTargetPath.clear();
    m_CompilesToTexture = false;

    std::ifstream file(assetPath);
    if (!file.is_open())
        return false;

    nlohmann::json data;
    try
    {
        data = nlohmann::json::parse(file);
    }
    catch (const std::exception&)
    {
        return false;
    }

    m_CompileTargetPath = assetPath;
    if (data.contains("type") && data["type"].is_string())
    {
        const auto* handler = AssetTypeRegistry::Instance().GetHandler(data["type"].get<std::string>());
        m_CompilesToTexture = handler && handler->compilesTo == AssetCompileTarget::Texture;
    }
    return m_CompilesToTexture;
}

void AssetTypeInspector::DrawAssetTypeEditor(const std::string& assetPath, const std::string& assetGuid)
{
    // Read the JSON file
    std::ifstream file(assetPath);
    if (!file.is_open())
    {
        ImGui::TextColored(Palette::Red, "Failed to open asset file");
        return;
    }

    nlohmann::json jsonData;
    try
    {
        file >> jsonData;
    }
    catch (const std::exception& e)
    {
        ImGui::TextColored(Palette::Red, "Failed to parse JSON: %s", e.what());
        return;
    }
    file.close();

    // Get the type field
    if (!jsonData.contains("type") || !jsonData["type"].is_string())
    {
        ImGui::TextDisabled("Asset has no 'type' field");
        return;
    }

    std::string typeName = jsonData["type"].get<std::string>();

    // Look up the editor
    auto* editor = AssetTypeEditorRegistry::Instance().GetEditor(typeName);
    if (!editor)
    {
        ImGui::TextDisabled("No editor for type: %s", typeName.c_str());
        return;
    }

    // The asset's type heads its own section, exactly as a component type heads
    // one in the object inspector: separator boundary, band, rows underneath.
    DekiEditor::FullBleedSeparator();
    if (!DekiEditor::SchematicSectionBegin(editor->GetDisplayName()))
        return;

    // Call the editor - it uses ImGui directly
    std::string jsonStr = jsonData.dump(2);
    bool modified = editor->OnInspectorGUI(jsonStr, assetPath, assetGuid);

    // Apply changes through command system (undo support + cache invalidation)
    if (modified)
    {
        nlohmann::json newData = nlohmann::json::parse(jsonStr);

        auto command = std::make_unique<ModifyAssetCommand>(
            assetPath, assetGuid, jsonData, newData,
            [this, assetGuid]()
            {
                // Recompile DTEX cache from updated JSON
                AssetDatabase::ImportAsset(AssetDatabase::GUIDToAssetPath(assetGuid));

                // Clear component AssetRef pointers (prevents dangling pointers)
                if (m_App->GetSceneViewPanel())
                    m_App->GetSceneViewPanel()->InvalidateAllAssets();

                // Evict stale sprite from engine cache
                Deki::AssetManager::Get()->InvalidateAsset("Sprite", assetGuid);

                // Invalidate GPU texture caches
                EditorAssets::Get()->InvalidateTexture(assetGuid);
                if (m_App->GetSceneViewPanel())
                    m_App->GetSceneViewPanel()->InvalidateTexture(assetGuid);

                // Refresh the anchored inspector preview - the cache path is
                // unchanged, so it would otherwise keep the stale GL texture
                m_TextureInspector->InvalidatePreview();
            });

        CommandHistory::Instance().Execute(std::move(command));
    }

    DekiEditor::SchematicSectionEnd();
}

void AssetTypeInspector::DrawDeployProperties(const std::string& assetGuid)
{
    if (assetGuid.empty())
        return;

    DekiEditor::FullBleedSeparator();
    if (!DekiEditor::SchematicSectionBegin("Deploy"))
        return;

    if (!m_HasScannedUsage)
    {
        Project* proj = m_App->GetProject();
        AssetPipeline* pipeline = m_App->GetAssetPipeline();
        if (proj && pipeline)
        {
            AssetExporter exporter(proj, pipeline);
            AssetUsageCollection usage = exporter.CollectUsedAssets();
            m_UsedGuids = std::move(usage.usedGuids);
            m_HasScannedUsage = true;
        }
        else
        {
            ImGui::TextDisabled("No project loaded");
            DekiEditor::SchematicSectionEnd();
            return;
        }
    }

    // Scenes are always deployed - treat them as always included
    const std::string& assetPath = m_App->GetSelectedAssetPath();
    bool isScene = assetPath.size() >= 7 &&
                   assetPath.compare(assetPath.size() - 7, 7, ".scene") == 0;

    bool isUsed = isScene || m_UsedGuids.count(assetGuid) > 0;
    Project* proj = m_App->GetProject();
    bool isForceIncluded = proj && proj->IsForceIncluded(assetGuid);

    // Referenced status. Palette colors, not hand-mixed greens and grays: green
    // means "in the build" here for the same reason it does everywhere else.
    InspectorUtils::BeginPropertyRow("Referenced");
    ImGui::AlignTextToFramePadding();
    if (isScene)
        ImGui::TextColored(Palette::Green, "Always included");
    else if (isUsed)
        ImGui::TextColored(Palette::Green, "Yes");
    else
        ImGui::TextColored(Palette::Dim, "No");

    // Force Include (not needed for scenes - they're always included). A row
    // with its label in the left column and the box in the value column, like
    // every other boolean in the inspector, instead of a labeled checkbox that
    // starts wherever its text happens to end.
    if (!isScene)
    {
        if (isUsed) ImGui::BeginDisabled();
        InspectorUtils::SetNextRowTooltip(
            isUsed ? "Already referenced by a scene, so it ships either way"
                   : "Ship this asset even though nothing references it");
        InspectorUtils::BeginPropertyRow("Force Include");
        if (DekiEditor::SchematicCheckbox("##ForceInclude", &isForceIncluded))
        {
            if (proj)
            {
                if (isForceIncluded)
                    proj->AddForceIncludeGuid(assetGuid);
                else
                    proj->RemoveForceIncludeGuid(assetGuid);
            }
        }
        if (isUsed) ImGui::EndDisabled();
    }

    // Target selector
    if (isUsed || isForceIncluded)
    {
        bool isBootScene = false;
        if (proj && !assetGuid.empty())
        {
            // Get assets-relative path for this asset (e.g., "scenes/boot.scene")
            std::string assetsRelPath;
            auto* pl = m_App->GetAssetPipeline();
            if (pl)
            {
                const AssetInfo* info = pl->GetAssetInfoByGuid(assetGuid);
                if (info)
                {
                    assetsRelPath = info->path;
                    const std::string prefix = GetAssetsDirectoryRelative() + "/";
                    if (assetsRelPath.rfind(prefix, 0) == 0)
                        assetsRelPath = assetsRelPath.substr(prefix.size());
                }
            }

            // bootScene in PlatformConfig is an assets-relative path
            // (backward compat: old configs may still have a GUID)
            auto& registry = PlatformRegistry::Instance();
            for (const auto& id : registry.GetAvailablePlatforms())
            {
                const PlatformConfig* platform = registry.GetPlatform(id);
                if (platform && !platform->bootScene.empty())
                {
                    if (platform->bootScene == assetsRelPath || platform->bootScene == assetGuid)
                    {
                        isBootScene = true;
                        break;
                    }
                }
            }
        }

        InspectorUtils::BeginPropertyRow("Target");
        if (isBootScene)
        {
            // Boot scene always goes to flash (locked).
            ImGui::BeginDisabled();
            int locked = 0;
            DekiEditor::SchematicCombo("##Target", &locked, "Flash\0");
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                DekiEditor::Tooltip("Boot scene is always stored in flash");
            }
        }
        else
        {
            bool isFlash = proj && proj->IsFlashTargeted(assetGuid);
            int targetIndex = isFlash ? 0 : 1;
            if (DekiEditor::SchematicCombo("##Target", &targetIndex, "Flash\0SD Card\0"))
            {
                if (proj)
                {
                    if (targetIndex == 0)
                        proj->AddFlashGuid(assetGuid);
                    else
                        proj->RemoveFlashGuid(assetGuid);
                }
            }
        }
    }
    else
    {
        InspectorUtils::BeginPropertyRow("Target");
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Palette::Dim, "Not included in deploy");
    }

    DekiEditor::SchematicSectionEnd();
}

}  // namespace DekiEditor
