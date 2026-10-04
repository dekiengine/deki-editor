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
    : m_App(app),
      m_TextureInspector(textureInspector),
      m_SceneInspector(std::make_unique<SceneInspector>(app))
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
        {
            pipelineGuid = info->guid;
        }
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
        {
            isSubAssetWithPreview = true;
        }
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
        {
            m_TextureInspector->DrawSettings();
        }

        if (!isSubAssetWithPreview && !isBakedCacheAsset)
        {
            DrawDeployProperties(assetGuid);
        }

        m_TextureInspector->EndScrollRegion();
    }
    else
    {
        bool isScene = ext == ".scene";

        if (isScene)
        {
            m_SceneInspector->DrawPreview(assetPath);
        }

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
    {
        return m_CompilesToTexture;
    }

    // Only cache definitive answers; unreadable/broken files retry next frame
    // (the type editor shows the parse error, and the file may be mid-write)
    m_CompileTargetPath.clear();
    m_CompilesToTexture = false;

    std::ifstream file(assetPath);
    if (!file.is_open())
    {
        return false;
    }

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
    {
        return;
    }

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
                {
                    m_App->GetSceneViewPanel()->InvalidateAllAssets();
                }

                // Evict stale sprite from engine cache
                Deki::AssetManager::Get()->InvalidateAsset("Sprite", assetGuid);

                // Invalidate GPU texture caches
                EditorAssets::Get()->InvalidateTexture(assetGuid);
                if (m_App->GetSceneViewPanel())
                {
                    m_App->GetSceneViewPanel()->InvalidateTexture(assetGuid);
                }

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
    {
        return;
    }

    DekiEditor::FullBleedSeparator();
    if (!DekiEditor::SchematicSectionBegin("Deploy"))
    {
        return;
    }

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
    bool isScene = assetPath.size() >= 7 && assetPath.compare(assetPath.size() - 7, 7, ".scene") == 0;

    bool isUsed = isScene || m_UsedGuids.count(assetGuid) > 0;
    Project* proj = m_App->GetProject();
    bool isForceIncluded = proj && proj->IsForceIncluded(assetGuid);

    // One row: is it in the builds. Scenes always are, and so is anything a
    // scene uses (ticked and locked, saying why); anything else ships only
    // when ticked here (force include).
    InspectorUtils::SetNextRowTooltip(isScene  ? "Every scene ships"
                                      : isUsed ? "A scene uses it, so it ships"
                                               : "Ship it even though no scene uses it");
    InspectorUtils::BeginPropertyRow("Included in builds");
    if (isScene || isUsed)
    {
        bool included = true;
        ImGui::BeginDisabled();
        DekiEditor::SchematicCheckbox("##Included", &included);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Palette::Dim, isScene ? "scene" : "used by a scene");
    }
    else if (DekiEditor::SchematicCheckbox("##Included", &isForceIncluded) && proj)
    {
        if (isForceIncluded)
        {
            proj->AddForceIncludeGuid(assetGuid);
        }
        else
        {
            proj->RemoveForceIncludeGuid(assetGuid);
        }
    }

    DekiEditor::SchematicSectionEnd();
}

}  // namespace DekiEditor
