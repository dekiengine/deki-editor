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
#include <deki-editor/Localization.h>
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

    // The pipeline's GUID for the file (the parent, for a sub-asset).
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

    // For a sub-asset the selected GUID differs from the pipeline's.
    std::string selectedGuid = m_App->GetSelectedAssetGuid();
    std::string assetGuid = pipelineGuid.empty() ? selectedGuid : pipelineGuid;

    // A sub-asset with a preview (a font atlas, say) has no editable properties.
    // Only when one is selected: a file whose own GUID is also another asset's
    // sub-asset (a tileset, listed under its map) is still the file.
    bool isSubAssetWithPreview = false;
    if (pipeline && !selectedGuid.empty() && selectedGuid != pipelineGuid && m_App->GetSelectedAssetHasCached())
    {
        const auto* subInfo = pipeline->GetSubAsset(selectedGuid);
        if (subInfo && subInfo->hasPreview)
        {
            isSubAssetWithPreview = true;
        }
    }

    // An asset that exists only in the cache (a font atlas baked for deploy, say)
    // has no source path.
    bool isBakedCacheAsset = assetPath.empty() && m_App->GetSelectedAssetHasCached();

    // A JSON asset that compiles to a texture (ProceduralSprite, say) gets the
    // preview of its baked DTEX, like an image.
    bool isCompiledTextureAsset = (ext == ".asset" || ext == ".dpack") && CompilesToTexture(assetPath);

    // Textures: a fixed preview above scrolling sections.
    if (TextureInspector::IsTextureFile(assetPath) || isBakedCacheAsset || isCompiledTextureAsset)
    {
        m_TextureInspector->DrawPreview();
        m_TextureInspector->BeginScrollRegion();

        if (!isSubAssetWithPreview && !isBakedCacheAsset)
        {
            if (ext == ".asset" || ext == ".dpack")
            {
                DrawAssetTypeEditor(assetPath, assetGuid);
            }
            // Other file types (.ttf, .otf and such) may have a registered inspector.
            else if (auto* fileInspector = FileInspectorRegistry::Instance().GetInspector(ext))
            {
                fileInspector->OnInspectorGUI(assetPath, assetGuid);
            }
        }

        // Format and chroma-key settings apply only to imported images; a
        // compiled texture is always baked as RGB565A8.
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

    // Only a sure answer is cached. An unreadable file is tried again next
    // frame, since it may be mid-write; the type editor shows the parse error.
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
    std::ifstream file(assetPath);
    if (!file.is_open())
    {
        ImGui::TextColored(Palette::Red, "%s", Tr("Failed to open asset file"));
        return;
    }

    nlohmann::json jsonData;
    try
    {
        file >> jsonData;
    }
    catch (const std::exception& e)
    {
        ImGui::TextColored(Palette::Red, Tr("Failed to parse JSON: %s"), e.what());
        return;
    }
    file.close();

    if (!jsonData.contains("type") || !jsonData["type"].is_string())
    {
        ImGui::TextDisabled("%s", Tr("Asset has no 'type' field"));
        return;
    }

    std::string typeName = jsonData["type"].get<std::string>();

    auto* editor = AssetTypeEditorRegistry::Instance().GetEditor(typeName);
    if (!editor)
    {
        ImGui::TextDisabled(Tr("No editor for type: %s"), typeName.c_str());
        return;
    }

    // The asset's type heads a section, as a component type does in the
    // object inspector.
    DekiEditor::FullBleedSeparator();
    if (!DekiEditor::SchematicSectionBegin(editor->GetDisplayName()))
    {
        return;
    }

    // The editor draws with ImGui and edits the JSON text.
    std::string jsonStr = jsonData.dump(2);
    bool modified = editor->OnInspectorGUI(jsonStr, assetPath, assetGuid);

    // Changes go through the command history, so they can be undone and the
    // caches are refreshed.
    if (modified)
    {
        nlohmann::json newData = nlohmann::json::parse(jsonStr);

        auto command = std::make_unique<ModifyAssetCommand>(
            assetPath, assetGuid, jsonData, newData,
            [this, assetGuid]()
            {
                // Recompile the DTEX cache from the new JSON.
                AssetDatabase::ImportAsset(AssetDatabase::GUIDToAssetPath(assetGuid));

                // Components drop their AssetRef pointers, which would dangle.
                if (m_App->GetSceneViewPanel())
                {
                    m_App->GetSceneViewPanel()->InvalidateAllAssets();
                }

                Deki::AssetManager::Get()->InvalidateAsset("Sprite", assetGuid);

                EditorAssets::Get()->InvalidateTexture(assetGuid);
                if (m_App->GetSceneViewPanel())
                {
                    m_App->GetSceneViewPanel()->InvalidateTexture(assetGuid);
                }

                // The cache path is the same, so the preview must be told to
                // drop its old GL texture.
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
            ImGui::TextDisabled("%s", Tr("No project loaded"));
            DekiEditor::SchematicSectionEnd();
            return;
        }
    }

    // Scenes always ship.
    const std::string& assetPath = m_App->GetSelectedAssetPath();
    bool isScene = assetPath.size() >= 7 && assetPath.compare(assetPath.size() - 7, 7, ".scene") == 0;

    bool isUsed = isScene || m_UsedGuids.count(assetGuid) > 0;
    Project* proj = m_App->GetProject();
    bool isForceIncluded = proj && proj->IsForceIncluded(assetGuid);

    // One row: does it ship. Scenes always do, and so does anything a scene
    // uses (ticked and locked, saying why). Anything else ships only when
    // ticked here.
    InspectorUtils::SetNextRowTooltip(isScene  ? TrMark("Every scene ships")
                                      : isUsed ? TrMark("A scene uses it, so it ships")
                                               : TrMark("Ship it even though no scene uses it"));
    InspectorUtils::BeginPropertyRow("Included in builds");
    if (isScene || isUsed)
    {
        bool included = true;
        ImGui::BeginDisabled();
        DekiEditor::SchematicCheckbox("##Included", &included);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Palette::Dim, "%s", isScene ? Tr("scene") : Tr("used by a scene"));
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
