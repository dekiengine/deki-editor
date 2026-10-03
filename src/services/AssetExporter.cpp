#include "AssetExporter.h"
#include <deki-editor/build/ProjectPaths.h>
#include <deki-editor/FileIO.h>
#include "AssetPacker.h"
#include "PackageLoader.h"
#include <deki-editor/SceneTargetFilter.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetData.h>
#include <deki-editor/Paths.h>
#include <deki-editor/TextureImporter.h>
#include "../core/Project.h"
#include <deki/ProjectSettings.h>
#include <deki/LogSystem.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/Property.h>
#include <deki/assets/AssetRef.h>
#include <deki/Scene.h>
#include <deki/Object.h>
#include <deki/Component.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <vector>
#include <nlohmann/json.hpp>

namespace DekiEditor
{

// Helper: resolve .rpipeline asset GUID into renderer name + pass type names
static void ResolvePipelineConfig(
    const std::string& pipelineGuid,
    AssetPipeline* pipeline,
    std::string& outRendererName,
    std::vector<std::string>& outPassNames)
{
    outRendererName.clear();
    outPassNames.clear();

    if (pipelineGuid.empty() || !pipeline)
        return;

    // Find the render pipeline .asset file
    const AssetInfo* pipelineInfo = pipeline->GetAssetInfoByGuid(pipelineGuid);
    if (!pipelineInfo)
        return;

    namespace fs = std::filesystem;
    fs::path pipelinePath = fs::path(pipeline->GetProjectPath()) / pipelineInfo->path;
    if (!fs::exists(pipelinePath))
        return;

    // Parse the .rpipeline JSON
    std::ifstream file(pipelinePath);
    if (!file.is_open())
    {
        DEKI_LOG_ERROR("AssetExporter: Cannot open render pipeline file '%s'",
                       pipelinePath.string().c_str());
        return;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(file);

        outRendererName = json.value("renderer", "");

        if (json.contains("passes") && json["passes"].is_array())
        {
            for (const auto& passEntry : json["passes"])
            {
                // Passes are inline objects: { "type": "clip2d", "settings": {} }
                if (passEntry.is_object())
                {
                    std::string passType = passEntry.value("type", "");
                    if (!passType.empty())
                        outPassNames.push_back(passType);
                }
            }
        }
    }
    catch (const nlohmann::json::parse_error& e)
    {
        DEKI_LOG_ERROR("AssetExporter: Malformed JSON in render pipeline '%s': %s",
                       pipelinePath.string().c_str(), e.what());
    }
    catch (const nlohmann::json::exception& e)
    {
        DEKI_LOG_ERROR("AssetExporter: JSON error reading render pipeline '%s': %s",
                       pipelinePath.string().c_str(), e.what());
    }
}

AssetExporter::AssetExporter(Project* project, AssetPipeline* pipeline, PackageLoader* packageLoader)
    : m_Project(project), m_Pipeline(pipeline), m_PackageLoader(packageLoader)
{
}

std::string AssetExporter::ExportSourceFor(const std::string& guid)
{
    namespace fs = std::filesystem;
    const AssetInfo* info = m_Pipeline->GetAssetInfoByGuid(guid);
    const std::string editorCache =
        (info && info->hasCachedVersion && !info->cachePath.empty()) ? m_Pipeline->GetAbsolutePath(info->cachePath)
                                                                       : GetCachePath(m_Project->GetPath(), guid);

    // Without a target the editor cache ships as it is. With one, an asset
    // type that can encode for it always does: the editor cache may have been
    // made for an earlier active platform.
    if (!m_HasTarget || !info)
        return fs::exists(editorCache) ? editorCache : std::string();

    std::string ext = fs::path(info->path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    const AssetExportEncoder* encoder = m_Pipeline->FindExportEncoder(ext);
    if (!encoder)
        return fs::exists(editorCache) ? editorCache : std::string();

    // This target's version, kept between exports and rebuilt when the source
    // or its settings are newer.
    const std::string source = m_Pipeline->GetAbsolutePath(info->path);
    const fs::path outDir = ProjectPaths::Generated(m_Project->GetPath()) / "export-cache" /
                            (m_Target.platformId.empty() ? std::string("editor") : m_Target.platformId);
    const fs::path out = outDir / guid;
    // Kept beside it: what it was built from and for. Comparing times only
    // missed a source restored with an older time, a new screen colour format
    // for the same platform, and a cache format change.
    fs::path stampPath = out;
    stampPath += ".stamp";
    const std::string stamp = m_Pipeline->MakeCacheStamp(source) + "|" + m_Target.platformId + "|" +
                              std::to_string(static_cast<int>(m_Target.colorFormat)) + "|" +
                              std::to_string(static_cast<int>(m_Target.assetStorage));
    std::error_code ec;
    bool fresh = fs::exists(out, ec);
    if (fresh)
    {
        std::ifstream in(stampPath, std::ios::binary);
        std::string recorded((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        fresh = recorded == stamp;
    }
    if (!fresh)
    {
        fs::remove(stampPath, ec);
        fs::create_directories(outDir, ec);
        AssetExportContext ctx;
        ctx.relativePath = info->path;
        ctx.absolutePath = source;
        ctx.guid = guid;
        ctx.extension = ext;
        ctx.projectPath = m_Project->GetPath();
        ctx.outPath = out.string();
        ctx.target = m_Target;
        ctx.pipeline = m_Pipeline;
        if (!(*encoder)(ctx))
        {
            DEKI_LOG_ERROR("  Could not store %s for target '%s'; it is left out of this export",
                           info->path.c_str(), m_Target.platformId.c_str());
            return std::string();
        }
        std::string err;
        AtomicWriteFile(stampPath.string(), stamp, err, /*durable=*/false);
    }
    return out.string();
}

bool AssetExporter::ExportToStorage()
{
    if (!m_Project)
    {
        DEKI_LOG_ERROR("AssetExporter: No project");
        return false;
    }
    return ExportAssets((std::filesystem::path(m_Project->GetPath()) / "storage").string());
}

bool AssetExporter::ExportAssets(const std::string& outDir)
{
    namespace fs = std::filesystem;

    if (!m_Project)
    {
        DEKI_LOG_ERROR("AssetExporter: No project");
        return false;
    }

    if (!m_Pipeline)
    {
        DEKI_LOG_ERROR("AssetExporter: No asset pipeline");
        return false;
    }

    // Clear previous export state
    m_ExportedCount = 0;
    m_Problems.clear();

    std::string projectPath = m_Project->GetPath();
    std::string cachePath = ProjectPaths::Cache(projectPath).string();

    fs::path storagePath = outDir;

    // Clear and recreate storage directory
    if (fs::exists(storagePath))
    {
        fs::remove_all(storagePath);
    }
    fs::create_directories(storagePath);

    DEKI_LOG_EDITOR("Exporting assets to storage (from cache)...");

    // Ensure cache is up-to-date before export
    m_Pipeline->UpdateAssetTable();

    // Recompile all scene cache files to ensure instance expansion is correct.
    // This is a safety net: cached scenes may have been compiled before the
    // GUID resolver was set (e.g., from a previous session), leaving instances
    // unexpanded with missing components.
    DEKI_LOG_EDITOR("Recompiling scene cache files for export...");
    {
        const auto& allAssets = m_Pipeline->GetAllAssets();
        int recompiledCount = 0;
        for (const auto& [path, info] : allAssets)
        {
            std::string ext = fs::path(info.path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".scene" || info.cachePath.empty())
                continue;

            std::string absolutePath = m_Pipeline->GetAbsolutePath(info.path);
            std::string absoluteCachePath = m_Pipeline->GetAbsolutePath(info.cachePath);

            if (m_Pipeline->CompileSceneFile(absolutePath, absoluteCachePath))
            {
                recompiledCount++;
            }
            else
            {
                AddProblem("The scene " + info.path + " does not compile");
            }
        }
        DEKI_LOG_EDITOR("Recompiled %d scene cache files", recompiledCount);
    }

    // Step 1: Collect used assets from all scenes
    AssetUsageCollection usedAssets = CollectUsedAssets();
    for (const std::string& problem : usedAssets.problems)
        AddProblem(problem);

    // Step 1.5: Merge in the force-included assets. Every build ships them,
    // not only an SD card copy.
    const std::unordered_set<std::string>& forceIncluded = m_Project->GetForceIncludeGuids();
    for (const auto& guid : forceIncluded)
    {
        usedAssets.usedGuids.insert(guid);
    }

    // Step 1.6: What those cannot load without (a frame's sheet, ...)
    AddAssetDependencies(usedAssets);

    DEKI_LOG_EDITOR("Found %zu used assets (%zu force-included), %zu font usages",
                    usedAssets.usedGuids.size(), forceIncluded.size(), usedAssets.fontUsages.size());

    // Step 2: Ensure fonts are baked in cache
    // (Fonts should already be cached via FontSyncHandler, but verify)
    if (!usedAssets.fontUsages.empty())
    {
        auto bakedFonts = BakeFontsForExport(usedAssets.fontUsages);

        // Add baked font GUIDs to used set
        for (const auto& [key, bakedGuid] : bakedFonts)
        {
            usedAssets.usedGuids.insert(bakedGuid);
        }
    }

    // Step 3: Build asset packs from .dpack files (if any exist)
    // Done before individual file copy so we know which GUIDs are packed
    std::unordered_set<std::string> packedGuids;
    {
        std::string assetsPath = m_Project->GetAssetsPath();
        std::string packIndexPath = (storagePath / "pack_index.bin").string();
        AssetPacker::BuildPackIndex(assetsPath, cachePath, storagePath.string(), packIndexPath,
                                    usedAssets.fontUsages, &packedGuids,
                                    [this](const std::string& guid) { return ExportSourceFor(guid); });
    }

    // Step 4: Copy only used assets that aren't already in packs
    CopyUsedAssetsToOutput(storagePath.string(), usedAssets.usedGuids, packedGuids, &usedAssets.referencedFrom);

    // Step 5: Export filtered asset lookup table for embedded runtime
    // (Contains ALL used assets including packed ones - runtime needs LookupPath() to work)
    std::string lookupTablePath = (storagePath / "asset_table.bin").string();
    if (!ExportAssetLookupTable(lookupTablePath, usedAssets.usedGuids))
        AddProblem("Could not write " + lookupTablePath);

    // Step 6: Write project settings (project_data.bin)
    WriteProjectSettings(storagePath.string());

    // Summary: count pack files and their total size
    int packCount = 0;
    uint64_t packTotalBytes = 0;
    for (const auto& entry : fs::directory_iterator(storagePath))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".pack")
        {
            packCount++;
            packTotalBytes += entry.file_size();
        }
    }

    if (packCount > 0)
    {
        DEKI_LOG_INFO("Export complete: %d individual assets + %d pack files (%.1f KB packed) in %s",
                      m_ExportedCount, packCount, packTotalBytes / 1024.0f, storagePath.string().c_str());
    }
    else
    {
        DEKI_LOG_INFO("Export complete: %d assets in %s", m_ExportedCount, storagePath.string().c_str());
    }

    // A build that boots but is missing what its scenes use used to report
    // success; the device then showed nothing where the asset should be.
    if (!m_Problems.empty())
    {
        DEKI_LOG_ERROR("Export failed: %zu problem%s (listed above). Fix them and export again.",
                       m_Problems.size(), m_Problems.size() == 1 ? "" : "s");
        return false;
    }
    return true;
}

void AssetExporter::AddProblem(const std::string& problem)
{
    DEKI_LOG_ERROR("Export: %s", problem.c_str());
    m_Problems.push_back(problem);
}

ExportAssetType AssetExporter::DetermineAssetType(const std::string& extension, std::string& outDestExtension)
{
    std::string ext = extension;
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" || ext == ".gif")
    {
        outDestExtension = ".tex";
        return ExportAssetType::Texture;
    }
    else if (ext == ".anim")
    {
        outDestExtension = ".anim";
        return ExportAssetType::Animation;
    }
    else if (ext == ".ttf" || ext == ".otf" || ext == ".dfont")
    {
        outDestExtension = ".dfont";
        return ExportAssetType::Font;
    }
    else if (ext == ".scene")
    {
        outDestExtension = ".scene";
        return ExportAssetType::Scene;
    }
    else if (ext == ".wav" || ext == ".mp3" || ext == ".ogg")
    {
        outDestExtension = ext;  // Keep original extension for audio
        return ExportAssetType::Audio;
    }

    // Default to texture for unknown types
    outDestExtension = "";
    return ExportAssetType::Texture;
}

// =============================================================================
// Asset Usage Collection
// =============================================================================

AssetUsageCollection AssetExporter::CollectUsedAssets()
{
    namespace fs = std::filesystem;
    AssetUsageCollection collection;

    const auto& allAssets = m_Pipeline->GetAllAssets();

    // Find all scene files
    for (const auto& [path, info] : allAssets)
    {
        std::string ext = fs::path(info.path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext != ".scene")
            continue;

        // Load the scene from its source path
        std::string absolutePath = m_Pipeline->GetAbsolutePath(info.path);
        if (!fs::exists(absolutePath))
        {
            DEKI_LOG_WARNING("  Scene not found: %s", absolutePath.c_str());
            continue;
        }

        // Load the scene (using editor JSON loading)
        Deki::Scene scene;
        if (!scene.LoadFromFile(absolutePath))
        {
            collection.problems.push_back("The scene " + info.path + " could not be read, so the assets it uses are unknown");
            continue;
        }

        // Add the scene's own GUID to the used set
        if (!info.guid.empty())
        {
            collection.usedGuids.insert(info.guid);
        }

        DEKI_LOG_DEBUG("Scanning scene: %s", info.path.c_str());

        // Scan all objects in the scene, noting where each new GUID came from
        const std::unordered_set<std::string> before = collection.usedGuids;
        for (Deki::Object* obj : scene.GetObjects())
        {
            CollectAssetsFromObject(obj, collection);
        }
        for (const std::string& guid : collection.usedGuids)
            if (!before.count(guid))
                collection.referencedFrom.emplace(guid, info.path);
    }

    return collection;
}

// A sprite frame is not a file: it lives in its sheet. The compiled scene
// already loads a frame through its sheet (it writes the sheet as the guid and
// the frame as the source), and an animation plays frames of one sheet - but
// the scan above reads the scene as saved, where a frame is referenced by its
// own id alone, so the sheet was never exported. Every animated character
// then rendered as nothing on a device, with no error: the sheet simply was
// not there to load.
void AssetExporter::AddAssetDependencies(AssetUsageCollection& collection)
{
    namespace fs = std::filesystem;
    std::vector<std::string> pending(collection.usedGuids.begin(), collection.usedGuids.end());
    auto add = [&](const std::string& guid)
    {
        if (!guid.empty() && collection.usedGuids.insert(guid).second)
            pending.push_back(guid);
    };

    while (!pending.empty())
    {
        const std::string guid = pending.back();
        pending.pop_back();

        if (const SubAssetInfo* sub = m_Pipeline->GetSubAsset(guid))
            add(sub->parentGuid);

        const AssetInfo* info = m_Pipeline->GetAssetInfoByGuid(guid);
        if (!info)
            continue;
        std::string ext = fs::path(info->path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".anim")
            continue;

        std::ifstream in(m_Pipeline->GetAbsolutePath(info->path));
        const nlohmann::json anim = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
        if (!anim.is_object())
        {
            DEKI_LOG_WARNING("Export: animation %s is not readable JSON; its sheet may be missing", info->path.c_str());
            continue;
        }
        add(anim.value("spritesheetGuid", std::string()));
    }
}

void AssetExporter::CollectAssetsFromObject(Deki::Object* obj, AssetUsageCollection& collection)
{
    if (!obj)
        return;

    // Scan all components on this object
    for (Deki::Component* comp : obj->GetComponents())
    {
        CollectAssetsFromComponent(comp, collection);
    }

    // Recurse into children
    for (Deki::Object* child : obj->GetChildren())
    {
        CollectAssetsFromObject(child, collection);
    }
}

void AssetExporter::CollectAssetsFromComponent(Deki::Component* comp, AssetUsageCollection& collection)
{
    if (!comp)
        return;

    const Deki::ComponentMeta* meta = Deki::ComponentRegistry::Instance().GetMeta(comp->GetType());
    if (!meta)
        return;

    // Iterate through all properties using reflection
    for (int i = 0; i < meta->propertyCount; ++i)
    {
        const Deki::PropertyInfo& prop = meta->properties[i];

        if (prop.type == Deki::PropertyType::AssetRef)
        {
            // Get the AssetRefBase at the property's offset
            const Deki::AssetRefBase& assetRef =
                *reinterpret_cast<const Deki::AssetRefBase*>(
                    reinterpret_cast<const char*>(comp) + prop.offset);

            // Add the baked/compiled asset GUID
            if (!assetRef.guid.empty())
            {
                collection.usedGuids.insert(assetRef.guid);
            }

            // Add the source asset GUID (for textures from sprites, etc.)
            if (!assetRef.source.empty())
            {
                collection.usedGuids.insert(assetRef.source);
            }

            // Special handling for fonts - need to also collect font size
            if (prop.assetType && strcmp(prop.assetType, "BitmapFont") == 0)
            {
                CollectFontUsage(comp, assetRef, collection);
            }
        }
        else if (prop.type == Deki::PropertyType::Asset)
        {
            // Legacy Asset type (string GUID)
            const std::string& guid =
                *reinterpret_cast<const std::string*>(
                    reinterpret_cast<const char*>(comp) + prop.offset);

            if (!guid.empty())
            {
                collection.usedGuids.insert(guid);
            }
        }
    }
}

void AssetExporter::CollectFontUsage(Deki::Component* comp, const Deki::AssetRefBase& assetRef, AssetUsageCollection& collection)
{
    // For fonts, we need to find the fontSize property on the same component
    // The source GUID is the TTF file, and we need the font size to bake

    if (assetRef.source.empty())
    {
        // No TTF source - might be a pre-baked font, just use the guid directly
        return;
    }

    const Deki::ComponentMeta* meta = Deki::ComponentRegistry::Instance().GetMeta(comp->GetType());
    if (!meta)
        return;

    // Find the fontSize property
    int fontSize = 16;  // Default
    for (int i = 0; i < meta->propertyCount; ++i)
    {
        const Deki::PropertyInfo& prop = meta->properties[i];
        if (strcmp(prop.name, "fontSize") == 0)
        {
            if (prop.type == Deki::PropertyType::Int32)
            {
                fontSize = *reinterpret_cast<const int32_t*>(
                    reinterpret_cast<const char*>(comp) + prop.offset);
            }
            break;
        }
    }

    // Add font usage info
    FontUsageInfo fontInfo;
    fontInfo.sourceGuid = assetRef.source;
    fontInfo.fontSize = fontSize;
    collection.fontUsages.insert(fontInfo);

    DEKI_LOG_DEBUG("  Found font usage: %s @ %d px", assetRef.source.c_str(), fontSize);
}

// =============================================================================
// Font Baking
// =============================================================================

std::unordered_map<std::string, std::string> AssetExporter::BakeFontsForExport(const std::set<FontUsageInfo>& fontUsages)
{
    namespace fs = std::filesystem;
    std::unordered_map<std::string, std::string> bakedFonts;

    std::string projectPath = m_Project->GetPath();
    std::string cachePath = GetCacheDirectory(projectPath);

    for (const auto& fontInfo : fontUsages)
    {
        // Generate deterministic GUID for this font+size combination
        std::string seedString = fontInfo.sourceGuid + ":" + std::to_string(fontInfo.fontSize);
        std::string fontGuid = GenerateDeterministicGuid(seedString);

        // Generate deterministic GUID for the atlas
        std::string atlasSeedString = fontInfo.sourceGuid + ":" + std::to_string(fontInfo.fontSize) + ":atlas";
        std::string atlasGuid = GenerateDeterministicGuid(atlasSeedString);

        // Check if already cached
        std::string fontCachePath = GetCachePath(projectPath, fontGuid);
        std::string atlasCachePath = GetCachePath(projectPath, atlasGuid);

        if (fs::exists(fontCachePath) && fs::exists(atlasCachePath))
        {
            DEKI_LOG_DEBUG("  Font already baked: %s @ %d px", fontInfo.sourceGuid.c_str(), fontInfo.fontSize);
            std::string key = fontInfo.sourceGuid + ":" + std::to_string(fontInfo.fontSize);
            bakedFonts[key] = fontGuid;
            bakedFonts[key + ":atlas"] = atlasGuid;
            continue;
        }

        // Resolve the source TTF path
        const AssetInfo* assetInfo = m_Pipeline->GetAssetInfoByGuid(fontInfo.sourceGuid);
        if (!assetInfo)
        {
            DEKI_LOG_WARNING("  Font source not found: %s", fontInfo.sourceGuid.c_str());
            continue;
        }

        std::string ttfPath = m_Pipeline->GetAbsolutePath(assetInfo->path);
        if (!fs::exists(ttfPath))
        {
            DEKI_LOG_WARNING("  TTF file not found: %s", ttfPath.c_str());
            continue;
        }

        // Resolve font compilation functions from deki-2d package
        using CompileFontFunc = void* (*)(const char*, int, int, int, int, int);
        using GetAtlasFunc = bool (*)(const void*, const uint8_t**, int*, int*);
        using WriteDfontFunc = bool (*)(const char*, const void*, const char*);
        using FreeResultFunc = void (*)(void*);

        auto compileFont = m_PackageLoader ? reinterpret_cast<CompileFontFunc>(
                                                 m_PackageLoader->GetFunction("Deki 2D Package", "DekiPlugin_CompileFont"))
                                           : nullptr;
        auto getAtlas = m_PackageLoader ? reinterpret_cast<GetAtlasFunc>(
                                              m_PackageLoader->GetFunction("Deki 2D Package", "DekiPlugin_GetCompiledFontAtlas"))
                                        : nullptr;
        auto writeDfont = m_PackageLoader ? reinterpret_cast<WriteDfontFunc>(
                                                m_PackageLoader->GetFunction("Deki 2D Package", "DekiPlugin_WriteDfont"))
                                          : nullptr;
        auto freeResult = m_PackageLoader ? reinterpret_cast<FreeResultFunc>(
                                                m_PackageLoader->GetFunction("Deki 2D Package", "DekiPlugin_FreeCompileResult"))
                                          : nullptr;

        if (!compileFont || !getAtlas || !writeDfont || !freeResult)
        {
            DEKI_LOG_WARNING("  deki-2d package not loaded, skipping font baking");
            continue;
        }

        // Compile the font via deki-2d package
        void* compileResult = compileFont(ttfPath.c_str(), fontInfo.fontSize, 32, 126, 2, 2048);
        if (!compileResult)
        {
            DEKI_LOG_ERROR("  Failed to compile font: %s @ %d px", ttfPath.c_str(), fontInfo.fontSize);
            continue;
        }

        // Get atlas data from compile result
        const uint8_t* atlasData = nullptr;
        int atlasWidth = 0, atlasHeight = 0;
        if (!getAtlas(compileResult, &atlasData, &atlasWidth, &atlasHeight))
        {
            DEKI_LOG_ERROR("  Failed to get font atlas data: %s", ttfPath.c_str());
            freeResult(compileResult);
            continue;
        }

        // Ensure cache directory exists
        fs::create_directories(cachePath);

        // Write atlas as DTEX
        if (!TextureImporter::WriteTexFile(atlasCachePath, atlasData,
                                           atlasWidth, atlasHeight,
                                           TextureFormat::ALPHA8))
        {
            DEKI_LOG_ERROR("  Failed to write font atlas: %s", atlasCachePath.c_str());
            freeResult(compileResult);
            continue;
        }

        // Write DFONT file
        if (!writeDfont(fontCachePath.c_str(), compileResult, atlasGuid.c_str()))
        {
            DEKI_LOG_ERROR("  Failed to write dfont file: %s", fontCachePath.c_str());
            freeResult(compileResult);
            continue;
        }

        freeResult(compileResult);

        DEKI_LOG_INFO("  Baked font: %s @ %d px -> %s",
                      fontInfo.sourceGuid.c_str(), fontInfo.fontSize, fontGuid.c_str());

        std::string key = fontInfo.sourceGuid + ":" + std::to_string(fontInfo.fontSize);
        bakedFonts[key] = fontGuid;
        bakedFonts[key + ":atlas"] = atlasGuid;
    }

    return bakedFonts;
}

// =============================================================================
// Export Stage
// =============================================================================

void AssetExporter::CopyUsedAssetsToOutput(const std::string& storagePath,
                                           const std::unordered_set<std::string>& usedGuids,
                                           const std::unordered_set<std::string>& skipGuids,
                                           const std::unordered_map<std::string, std::string>* referencedFrom)
{
    namespace fs = std::filesystem;

    const auto& allAssets = m_Pipeline->GetAllAssets();
    std::string projectPath = m_Project->GetPath();
    std::string cacheDir = GetCacheDirectory(projectPath);

    DEKI_LOG_EDITOR("Exporting used assets as GUID-named files (%zu used, %zu in packs)...",
                    usedGuids.size(), skipGuids.size());

    // First, export assets from the pipeline (textures, animations, scenes, etc.)
    // Files are named by GUID (no path structure, no extension)
    for (const auto& [path, info] : allAssets)
    {
        if (info.guid.empty())
            continue;

        // Skip if not in used set
        if (usedGuids.find(info.guid) == usedGuids.end())
        {
            DEKI_LOG_DEBUG("  Skipping unused: %s", info.path.c_str());
            continue;
        }

        // Skip if already in a pack file
        if (skipGuids.find(info.guid) != skipGuids.end())
        {
            DEKI_LOG_DEBUG("  Skipping packed: %s", info.path.c_str());
            continue;
        }

        // Check if cache file exists (cachePath is project-relative)
        if (!info.hasCachedVersion || info.cachePath.empty())
        {
            DEKI_LOG_DEBUG("  Skipping %s - no cached version", info.path.c_str());
            continue;
        }

        // This target's version of it, or the editor cache.
        std::string absoluteCachePath = ExportSourceFor(info.guid);
        if (absoluteCachePath.empty())
        {
            AddProblem(info.path + " could not be exported for this target (" + info.cachePath + ")");
            continue;
        }

        // Export as GUID-named file (no path structure, no extension)
        fs::path destPath = fs::path(storagePath) / info.guid;

        try
        {
            fs::copy_file(absoluteCachePath, destPath, fs::copy_options::overwrite_existing);
            m_ExportedCount++;

            DEKI_LOG_DEBUG("  Exported: %s -> %s", info.path.c_str(), destPath.string().c_str());
        }
        catch (const std::exception& e)
        {
            AddProblem("Could not copy " + info.path + ": " + e.what());
        }
    }

    // Then, export baked fonts from cache directory (not in pipeline)
    // These are GUIDs in usedGuids that are in the cache dir but not in the pipeline
    for (const std::string& guid : usedGuids)
    {
        // Skip if already exported from pipeline
        const AssetInfo* assetInfo = m_Pipeline->GetAssetInfoByGuid(guid);
        if (assetInfo && assetInfo->hasCachedVersion)
            continue;

        // Skip if already in a pack file
        if (skipGuids.find(guid) != skipGuids.end())
            continue;

        // This target's version of it, or what the cache directory holds
        std::string cachePath = ExportSourceFor(guid);
        if (cachePath.empty())
        {
            // Not an asset of this project and nothing baked under that GUID:
            // the reference points at something deleted or never imported.
            // (An asset the project has but nothing compiles, like a font's
            // source file, is fine: what it bakes into is exported instead.
            // So is a sheet's frame: it ships inside its sheet.)
            if (!assetInfo && !m_Pipeline->GetSubAsset(guid))
            {
                std::string where;
                if (referencedFrom)
                {
                    auto it = referencedFrom->find(guid);
                    if (it != referencedFrom->end())
                        where = ", used by " + it->second;
                }
                AddProblem("Missing asset " + guid + where + ". It was deleted or never imported; pick it again "
                           "or clear the reference.");
            }
            continue;
        }

        // Export as GUID-named file (no subdirectory)
        fs::path destPath = fs::path(storagePath) / guid;

        try
        {
            fs::copy_file(cachePath, destPath, fs::copy_options::overwrite_existing);
            m_ExportedCount++;

            DEKI_LOG_DEBUG("  Exported baked font: %s", guid.c_str());
        }
        catch (const std::exception& e)
        {
            AddProblem("Could not copy baked font " + guid + ": " + e.what());
        }
    }
}

void AssetExporter::WriteProjectSettings(const std::string& storagePath)
{
    namespace fs = std::filesystem;

    fs::path projectSettingsPath = fs::path(storagePath) / Deki::ProjectSettings::EXPORT_FILENAME;
    std::ofstream settingsFile(projectSettingsPath, std::ios::binary);

    if (!settingsFile.is_open())
    {
        DEKI_LOG_WARNING("Failed to create %s", Deki::ProjectSettings::EXPORT_FILENAME);
        return;
    }

    const auto& startupScene = m_Project->GetStartupScene();
    // Frame pacing comes from Core. Cameras carry their own framing, and the
    // framebuffer's size and format come from the device's display.
    auto* coreSettings = DekiEditor::GetCoreSettings();
    int32_t targetFPS = coreSettings ? coreSettings->targetFps : 60;

    // Resolve render pipeline config from .rpipeline asset
    std::string rendererName;
    std::vector<std::string> passNames;
    ResolvePipelineConfig(m_Project->GetRenderPipelineGuid(), m_Pipeline, rendererName, passNames);

    std::vector<const char*> passNamePtrs;
    for (const auto& name : passNames)
        passNamePtrs.push_back(name.c_str());

    std::vector<uint8_t> settingsBuffer = Deki::ProjectSettings::Write(
        m_Target.assetStorage, targetFPS,
        startupScene.empty() ? nullptr : startupScene.c_str(),
        rendererName.empty() ? nullptr : rendererName.c_str(),
        passNamePtrs.empty() ? nullptr : passNamePtrs.data(),
        static_cast<int>(passNamePtrs.size()));
    settingsFile.write(reinterpret_cast<const char*>(settingsBuffer.data()), settingsBuffer.size());

    DEKI_LOG_EDITOR("Wrote %s (%zu bytes, pipeline=%s, %d passes)",
                    Deki::ProjectSettings::EXPORT_FILENAME, settingsBuffer.size(),
                    rendererName.empty() ? "(none)" : rendererName.c_str(),
                    (int)passNames.size());
}

bool AssetExporter::ExportBootPayload(const std::string& payloadDir, const std::string& bootSceneAssetPath)
{
    namespace fs = std::filesystem;

    if (!m_Project)
    {
        DEKI_LOG_ERROR("ExportBootPayload: No project");
        return false;
    }

    DEKI_LOG_EDITOR("Exporting the boot payload to %s", payloadDir.c_str());
    m_Problems.clear();

    // Create/clear the payload directory
    try
    {
        if (fs::exists(payloadDir))
        {
            fs::remove_all(payloadDir);
        }
        fs::create_directories(payloadDir);
    }
    catch (const std::exception& e)
    {
        DEKI_LOG_ERROR("Failed to create the boot payload directory: %s", e.what());
        return false;
    }

    // Write project_data.bin (uses the raw filename, not the F:/ prefixed path)
    fs::path projectSettingsPath = fs::path(payloadDir) / "project_data.bin";
    std::ofstream settingsFile(projectSettingsPath, std::ios::binary);

    if (!settingsFile.is_open())
    {
        DEKI_LOG_ERROR("Failed to create project_data.bin in the boot payload directory");
        return false;
    }

    const auto& startupScene = m_Project->GetStartupScene();
    // Frame pacing comes from Core. Cameras carry their own framing, and the
    // framebuffer's size and format come from the device's display.
    auto* coreSettings = DekiEditor::GetCoreSettings();
    int32_t targetFPS = coreSettings ? coreSettings->targetFps : 60;

    // Resolve render pipeline config from .rpipeline asset
    std::string rendererName2;
    std::vector<std::string> passNames2;
    ResolvePipelineConfig(m_Project->GetRenderPipelineGuid(), m_Pipeline, rendererName2, passNames2);

    std::vector<const char*> passNamePtrs2;
    for (const auto& name : passNames2)
        passNamePtrs2.push_back(name.c_str());

    std::vector<uint8_t> settingsBuffer = Deki::ProjectSettings::Write(
        m_Target.assetStorage, targetFPS,
        startupScene.empty() ? nullptr : startupScene.c_str(),
        rendererName2.empty() ? nullptr : rendererName2.c_str(),
        passNamePtrs2.empty() ? nullptr : passNamePtrs2.data(),
        static_cast<int>(passNamePtrs2.size()));
    settingsFile.write(reinterpret_cast<const char*>(settingsBuffer.data()), settingsBuffer.size());
    settingsFile.close();

    DEKI_LOG_EDITOR("Wrote project_data.bin to the boot payload (%zu bytes)", settingsBuffer.size());

    // Compile and export boot scene if specified
    if (!bootSceneAssetPath.empty() && m_Pipeline)
    {
        DEKI_LOG_EDITOR("Boot scene path: '%s'", bootSceneAssetPath.c_str());

        fs::path destPath = fs::path(payloadDir) / "boot.scene";
        bool exported = false;

        // Resolve the JSON scene file path
        fs::path sourcePath = fs::path(m_Project->GetPath()) / bootSceneAssetPath;
        if (!fs::exists(sourcePath))
        {
            // Try as assets-relative path
            sourcePath = fs::path(GetAssetsDirectory(m_Project->GetPath())) / bootSceneAssetPath;
        }

        if (fs::exists(sourcePath))
        {
            // Objects this target cannot satisfy are not part of its build, so
            // they are dropped before the scene is compiled. Done on the JSON
            // rather than after loading, so the runtime never learns that
            // targets exist and the scene format stays readable by anything.
            // The filtered copy goes beside the output, never over the source.
            fs::path compileFrom = sourcePath;
            fs::path filtered;
            if (!m_TargetCapabilities.empty())
            {
                try
                {
                    std::ifstream in(sourcePath);
                    nlohmann::json scene;
                    in >> scene;
                    if (SceneHasTargetRequirements(scene))
                    {
                        filtered = fs::path(payloadDir) / "boot.filtered.scene";
                        std::ofstream out(filtered);
                        out << FilterSceneForTarget(scene, m_TargetCapabilities).dump(2);
                        out.close();
                        compileFrom = filtered;
                        DEKI_LOG_EDITOR("Boot scene filtered for this target");
                    }
                }
                catch (const std::exception& e)
                {
                    DEKI_LOG_WARNING("Boot scene could not be filtered (%s); exporting it whole",
                                     e.what());
                    compileFrom = sourcePath;
                }
            }

            // Compile JSON scene to MessagePack binary
            if (m_Pipeline->CompileSceneFile(compileFrom.string(), destPath.string()))
            {
                DEKI_LOG_EDITOR("Compiled boot scene into the boot payload: %s (%zu bytes)",
                                destPath.string().c_str(), fs::file_size(destPath));
                exported = true;
                if (!filtered.empty())
                {
                    std::error_code rmEc;
                    fs::remove(filtered, rmEc);  // scratch, not something to flash
                }
            }
            else
            {
                DEKI_LOG_ERROR("Failed to compile boot scene: %s", sourcePath.string().c_str());
            }
        }

        // Without its boot scene a device starts to a blank screen; this used to
        // be a warning and the build went on to report success.
        if (!exported)
        {
            AddProblem(fs::exists(sourcePath) ? "The boot scene " + bootSceneAssetPath + " does not compile"
                                              : "The boot scene " + bootSceneAssetPath + " was not found");
            return false;
        }
    }
    else
    {
        if (bootSceneAssetPath.empty())
            DEKI_LOG_INFO("No boot scene specified in platform config");
        if (!m_Pipeline)
            DEKI_LOG_WARNING("No asset pipeline available for boot scene export");
    }

    DEKI_LOG_EDITOR("Boot payload export complete");
    return true;
}

// FNV-1a hash function for path:type keys
static uint32_t FNV1aHash(const char* str)
{
    uint32_t hash = 2166136261u;  // FNV offset basis
    while (*str)
    {
        hash ^= static_cast<uint8_t>(*str++);
        hash *= 16777619u;  // FNV prime
    }
    return hash;
}

bool AssetExporter::ExportAssetLookupTable(const std::string& outputPath, const std::unordered_set<std::string>& usedGuids)
{
    namespace fs = std::filesystem;

    // Build list of entries: (hash, guid) for all used assets
    struct LookupEntry
    {
        uint32_t hash;
        char guid[37];    // 36 chars + null terminator
        char padding[3];  // Align to 44 bytes

        bool operator<(const LookupEntry& other) const { return hash < other.hash; }
    };
    static_assert(sizeof(LookupEntry) == 44, "LookupEntry must be 44 bytes");

    std::vector<LookupEntry> entries;
    std::unordered_set<std::string> addedGuids;  // Track which GUIDs we've added

    const auto& allAssets = m_Pipeline->GetAllAssets();
    std::string projectPath = m_Project->GetPath();

    // First pass: Add pipeline assets (with path:type keys)
    for (const auto& [path, info] : allAssets)
    {
        if (info.guid.empty())
            continue;

        // Skip if not in used set
        if (usedGuids.find(info.guid) == usedGuids.end())
            continue;

        // Determine asset type from extension
        std::string ext = fs::path(info.path).extension().string();
        std::string assetType = AssetPipeline::GetAssetTypeFromExtension(ext);
        if (assetType.empty())
            continue;

        // Build export path (remove "project/assets/" or "assets/" prefix and extension)
        std::string exportPath = info.path;
        const std::string assetsPrefix = GetAssetsDirectoryRelative() + "/";
        if (exportPath.rfind(assetsPrefix, 0) == 0)
            exportPath = exportPath.substr(assetsPrefix.size());
        else if (exportPath.rfind("assets/", 0) == 0)
            exportPath = exportPath.substr(7);
        size_t dotPos = exportPath.rfind('.');
        if (dotPos != std::string::npos)
            exportPath = exportPath.substr(0, dotPos);

        // Build key: "path:type"
        std::string key = exportPath + ":" + assetType;

        // Create entry
        LookupEntry entry;
        memset(&entry, 0, sizeof(entry));
        entry.hash = FNV1aHash(key.c_str());
        strncpy(entry.guid, info.guid.c_str(), sizeof(entry.guid) - 1);

        DEKI_LOG_DEBUG("ExportAssetLookupTable: key='%s' hash=0x%08X guid=%s", key.c_str(), entry.hash, info.guid.c_str());
        entries.push_back(entry);
        addedGuids.insert(info.guid);
    }

    // Second pass: Add cache-only assets (baked fonts, etc.) that aren't in the pipeline
    // These use the GUID itself as the key for direct lookup
    for (const std::string& guid : usedGuids)
    {
        // Skip if already added from pipeline
        if (addedGuids.find(guid) != addedGuids.end())
            continue;

        // Check if this GUID exists in cache directory
        std::string cachePath = GetCachePath(projectPath, guid);
        if (!fs::exists(cachePath))
            continue;

        // Create entry with GUID as the key (for direct GUID lookup)
        LookupEntry entry;
        memset(&entry, 0, sizeof(entry));
        entry.hash = FNV1aHash(guid.c_str());
        strncpy(entry.guid, guid.c_str(), sizeof(entry.guid) - 1);

        DEKI_LOG_DEBUG("ExportAssetLookupTable: cache-only key='%s' hash=0x%08X", guid.c_str(), entry.hash);
        entries.push_back(entry);
        addedGuids.insert(guid);
    }

    // Sort by hash for binary search
    std::sort(entries.begin(), entries.end());

    // Write binary file
    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open())
    {
        DEKI_LOG_ERROR("ExportAssetLookupTable: Failed to create file: %s", outputPath.c_str());
        return false;
    }

    // Write entry count
    uint32_t count = static_cast<uint32_t>(entries.size());
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    // Write entries
    for (const auto& entry : entries)
    {
        file.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
    }

    DEKI_LOG_EDITOR("ExportAssetLookupTable: Wrote %u entries to %s", count, outputPath.c_str());
    return true;
}

}  // namespace DekiEditor
