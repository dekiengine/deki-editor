#include "AssetExporter.h"
#include <deki-editor/build/ProjectPaths.h>
#include <deki-editor/build/PlatformConfig.h>
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
#include <deki/assets/AssetManager.h>
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

// The target board's screen diagonal for project_data.bin, 0 when unknown or
// when exporting for the editor itself.
static float TargetScreenDiagonalInches(const AssetExportTarget& target)
{
    const PlatformConfig* platform =
        target.platformId.empty() ? nullptr : PlatformRegistry::Instance().GetPlatform(target.platformId);
    return platform ? platform->screenDiagonalInches : 0.0f;
}

// Reads a render pipeline asset's renderer name and pass type names.
static void ResolvePipelineConfig(const std::string& pipelineGuid, AssetPipeline* pipeline,
                                  std::string& outRendererName, std::vector<std::string>& outPassNames)
{
    outRendererName.clear();
    outPassNames.clear();

    if (pipelineGuid.empty() || !pipeline)
    {
        return;
    }

    const AssetInfo* pipelineInfo = pipeline->GetAssetInfoByGuid(pipelineGuid);
    if (!pipelineInfo)
    {
        return;
    }

    namespace fs = std::filesystem;
    fs::path pipelinePath = fs::path(pipeline->GetProjectPath()) / pipelineInfo->path;
    if (!fs::exists(pipelinePath))
    {
        return;
    }

    std::ifstream file(pipelinePath);
    if (!file.is_open())
    {
        DEKI_LOG_ERROR("AssetExporter: Cannot open render pipeline file '%s'", pipelinePath.string().c_str());
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
                    {
                        outPassNames.push_back(passType);
                    }
                }
            }
        }
    }
    catch (const nlohmann::json::parse_error& e)
    {
        DEKI_LOG_ERROR("AssetExporter: Malformed JSON in render pipeline '%s': %s", pipelinePath.string().c_str(),
                       e.what());
    }
    catch (const nlohmann::json::exception& e)
    {
        DEKI_LOG_ERROR("AssetExporter: JSON error reading render pipeline '%s': %s", pipelinePath.string().c_str(),
                       e.what());
    }
}

AssetExporter::AssetExporter(Project* project, AssetPipeline* pipeline, PackageLoader* packageLoader)
    : m_Project(project),
      m_Pipeline(pipeline),
      m_PackageLoader(packageLoader)
{
}

std::string AssetExporter::ExportSourceFor(const std::string& guid)
{
    namespace fs = std::filesystem;
    const AssetInfo* info = m_Pipeline->GetAssetInfoByGuid(guid);
    const std::string editorCache = (info && info->hasCachedVersion && !info->cachePath.empty())
                                        ? m_Pipeline->GetAbsolutePath(info->cachePath)
                                        : GetCachePath(m_Project->GetPath(), guid);

    // Without a target the editor cache ships as it is. With one, an asset
    // type that can encode for it always does, because the editor cache may
    // have been made for another active platform.
    if (!m_HasTarget || !info)
    {
        return fs::exists(editorCache) ? editorCache : std::string();
    }

    std::string ext = fs::path(info->path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    const AssetExportEncoder* encoder = m_Pipeline->FindExportEncoder(ext);
    if (!encoder)
    {
        return fs::exists(editorCache) ? editorCache : std::string();
    }

    // This target's version, kept between exports and rebuilt when the source
    // or its settings are newer.
    const std::string source = m_Pipeline->GetAbsolutePath(info->path);
    const fs::path outDir = ProjectPaths::Generated(m_Project->GetPath()) / "export-cache" /
                            (m_Target.platformId.empty() ? std::string("editor") : m_Target.platformId);
    const fs::path out = outDir / guid;
    // A stamp beside it records what it was built from and for. Comparing
    // file times is not enough: it misses a source restored with an older
    // time, a new screen colour format on the same platform, and a cache
    // format change.
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
            DEKI_LOG_ERROR("  Could not store %s for target '%s'; it is left out of this export", info->path.c_str(),
                           m_Target.platformId.c_str());
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

    m_ExportedCount = 0;
    m_Problems.clear();

    std::string projectPath = m_Project->GetPath();
    std::string cachePath = ProjectPaths::Cache(projectPath).string();

    fs::path storagePath = outDir;

    if (fs::exists(storagePath))
    {
        fs::remove_all(storagePath);
    }
    fs::create_directories(storagePath);

    DEKI_LOG_EDITOR("Exporting assets to storage (from cache)...");

    m_Pipeline->UpdateAssetTable();

    // Compile every scene again so instances are expanded correctly. A cached
    // scene may have been compiled before the GUID resolver was set (in an
    // earlier session, say), leaving instances unexpanded and missing
    // components.
    DEKI_LOG_EDITOR("Recompiling scene cache files for export...");
    {
        const auto& allAssets = m_Pipeline->GetAllAssets();
        int recompiledCount = 0;
        for (const auto& [path, info] : allAssets)
        {
            std::string ext = fs::path(info.path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".scene" || info.cachePath.empty())
            {
                continue;
            }

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

    // Step 1: the assets the scenes use.
    AssetUsageCollection usedAssets = CollectUsedAssets();
    for (const std::string& problem : usedAssets.problems)
    {
        AddProblem(problem);
    }

    // Step 1.5: the force-included assets. Every build ships them, not only
    // an SD card copy.
    const std::unordered_set<std::string>& forceIncluded = m_Project->GetForceIncludeGuids();
    for (const auto& guid : forceIncluded)
    {
        usedAssets.usedGuids.insert(guid);
    }

    AddForceIncludedFontSizes(usedAssets);

    // Step 1.6: what those cannot load without (a frame's sheet, ...).
    AddAssetDependencies(usedAssets);

    DEKI_LOG_EDITOR("Found %zu used assets (%zu force-included), %zu font usages", usedAssets.usedGuids.size(),
                    forceIncluded.size(), usedAssets.fontUsages.size());

    // Step 2: make sure every font is baked in the cache. FontSyncHandler
    // usually has done it already.
    if (!usedAssets.fontUsages.empty())
    {
        auto bakedFonts = BakeFontsForExport(usedAssets.fontUsages);

        for (const auto& [key, bakedGuid] : bakedFonts)
        {
            usedAssets.usedGuids.insert(bakedGuid);
        }
    }

    // Step 3: build the asset packs from .dpack files. Before the file copy,
    // so it knows which GUIDs are packed.
    std::unordered_set<std::string> packedGuids;
    {
        std::string assetsPath = m_Project->GetAssetsPath();
        std::string packIndexPath = (storagePath / "pack_index.bin").string();
        AssetPacker::BuildPackIndex(assetsPath, cachePath, storagePath.string(), packIndexPath, usedAssets.fontUsages,
                                    &packedGuids, [this](const std::string& guid) { return ExportSourceFor(guid); });
    }

    // Step 4: copy the used assets that are not in a pack.
    CopyUsedAssetsToOutput(storagePath.string(), usedAssets.usedGuids, packedGuids, &usedAssets.referencedFrom);

    // Step 5: the asset lookup table. It lists every used asset, packed ones
    // too, because the runtime's LookupPath() needs them all.
    std::string lookupTablePath = (storagePath / "asset_table.bin").string();
    if (!ExportAssetLookupTable(lookupTablePath, usedAssets.usedGuids))
    {
        AddProblem("Could not write " + lookupTablePath);
    }

    // Step 6: project settings (project_data.bin).
    WriteProjectSettings(storagePath.string());

    // Summary: pack files and their total size.
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
        DEKI_LOG_INFO("Export complete: %d individual assets + %d pack files (%.1f KB packed) in %s", m_ExportedCount,
                      packCount, packTotalBytes / 1024.0f, storagePath.string().c_str());
    }
    else
    {
        DEKI_LOG_INFO("Export complete: %d assets in %s", m_ExportedCount, storagePath.string().c_str());
    }

    // Any problem fails the export: a build missing what its scenes use still
    // boots, and shows nothing where the asset should be.
    if (!m_Problems.empty())
    {
        DEKI_LOG_ERROR("Export failed: %zu problem%s (listed above). Fix them and export again.", m_Problems.size(),
                       m_Problems.size() == 1 ? "" : "s");
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
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

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

    for (const auto& [path, info] : allAssets)
    {
        std::string ext = fs::path(info.path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext != ".scene")
        {
            continue;
        }

        std::string absolutePath = m_Pipeline->GetAbsolutePath(info.path);
        if (!fs::exists(absolutePath))
        {
            DEKI_LOG_WARNING("  Scene not found: %s", absolutePath.c_str());
            continue;
        }

        Deki::Scene scene;
        if (!scene.LoadFromFile(absolutePath))
        {
            collection.problems.push_back("The scene " + info.path +
                                          " could not be read, so the assets it uses are unknown");
            continue;
        }

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
        {
            if (!before.count(guid))
            {
                collection.referencedFrom.emplace(guid, info.path);
            }
        }
    }

    AddFontExtraSizes(collection);
    return collection;
}

bool AssetExporter::BakeUsedFonts()
{
    m_Problems.clear();
    const AssetUsageCollection usage = CollectUsedAssets();
    for (const std::string& problem : usage.problems)
    {
        AddProblem(problem);
    }
    // Registered as deki-2d registers its own bakes (the cache file is named
    // by its GUID), so a scene loaded after this finds them.
    for (const auto& [key, guid] : BakeFontsForExport(usage.fontUsages))
    {
        if (key.size() < 6 || key.compare(key.size() - 6, 6, ":atlas") != 0)
        {
            Deki::AssetManager::Get()->RegisterGuid(guid, guid);
        }
    }
    return m_Problems.empty();
}

void AssetExporter::AddFontExtraSizes(AssetUsageCollection& collection)
{
    // deki-2d reads the font's settings, so the editor needs no knowledge of
    // their format.
    using GetExtraSizesFunc = int (*)(const char*, int*, int);
    auto getExtraSizes = m_PackageLoader ? reinterpret_cast<GetExtraSizesFunc>(m_PackageLoader->GetFunction(
                                               "Deki 2D Package", "DekiPluginGetFontExtraSizes"))
                                         : nullptr;
    if (!getExtraSizes)
    {
        return;
    }

    std::set<std::string> fonts;
    for (const auto& usage : collection.fontUsages)
    {
        if (!usage.bdf)
        {
            fonts.insert(usage.sourceGuid);
        }
    }
    for (const std::string& sourceGuid : fonts)
    {
        const AssetInfo* info = m_Pipeline->GetAssetInfoByGuid(sourceGuid);
        if (!info)
        {
            continue;
        }
        constexpr int kMaxExtraSizes = 64;
        int sizes[kMaxExtraSizes];
        const std::string fontPath = m_Pipeline->GetAbsolutePath(info->path);
        const int count = (std::min)(getExtraSizes(fontPath.c_str(), sizes, kMaxExtraSizes), kMaxExtraSizes);
        for (int i = 0; i < count; ++i)
        {
            FontUsageInfo extra;
            extra.sourceGuid = sourceGuid;
            extra.fontSize = sizes[i];
            collection.fontUsages.insert(extra);
            DEKI_LOG_DEBUG("  Font %s also ships %d px", sourceGuid.c_str(), sizes[i]);
        }
    }
}

void AssetExporter::AddForceIncludedFontSizes(AssetUsageCollection& collection)
{
    namespace fs = std::filesystem;
    const std::unordered_set<std::string>& forceIncluded = m_Project->GetForceIncludeGuids();
    if (forceIncluded.empty())
    {
        return;
    }

    // Every size up to this is tried.
    constexpr int kMaxFontSize = 512;
    for (const auto& [path, info] : m_Pipeline->GetAllAssets())
    {
        std::string ext = fs::path(info.path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if ((ext != ".ttf" && ext != ".otf") || info.guid.empty())
        {
            continue;
        }
        for (int size = 1; size <= kMaxFontSize; ++size)
        {
            FontUsageInfo usage;
            usage.sourceGuid = info.guid;
            usage.fontSize = size;
            if (forceIncluded.count(usage.FontGuid()) || forceIncluded.count(usage.AtlasGuid()))
            {
                collection.fontUsages.insert(usage);
                DEKI_LOG_DEBUG("  Font %s ships %d px (force-included)", info.guid.c_str(), size);
            }
        }
    }
}

// A sprite frame is not a file: it lives in its sheet, and an animation plays
// frames of one sheet. The scan above reads the scene as saved, where a frame
// is referenced by its own id alone, so the sheets must be added here. So are
// the assets a package's asset type reports it uses (a tilemap's tilesets, a
// tileset's image), and theirs in turn. Without them, sprites, animations and
// tilemaps draw nothing on a device, with no error.
void AssetExporter::AddAssetDependencies(AssetUsageCollection& collection)
{
    namespace fs = std::filesystem;
    std::vector<std::string> pending(collection.usedGuids.begin(), collection.usedGuids.end());
    auto add = [&](const std::string& guid)
    {
        if (!guid.empty() && collection.usedGuids.insert(guid).second)
        {
            pending.push_back(guid);
        }
    };

    while (!pending.empty())
    {
        const std::string guid = pending.back();
        pending.pop_back();

        if (const SubAssetInfo* sub = m_Pipeline->GetSubAsset(guid))
        {
            add(sub->parentGuid);
        }

        const AssetInfo* info = m_Pipeline->GetAssetInfoByGuid(guid);
        if (!info)
        {
            continue;
        }
        std::string ext = fs::path(info->path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (const AssetDependencyReporter* reporter = m_Pipeline->FindDependencyReporter(ext))
        {
            for (const std::string& dependency : (*reporter)(m_Pipeline->GetAbsolutePath(info->path), m_Pipeline))
            {
                add(dependency);
            }
        }
        if (ext != ".anim")
        {
            continue;
        }

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
    {
        return;
    }

    for (Deki::Component* comp : obj->GetComponents())
    {
        CollectAssetsFromComponent(comp, collection);
    }

    for (Deki::Object* child : obj->GetChildren())
    {
        CollectAssetsFromObject(child, collection);
    }
}

void AssetExporter::CollectAssetsFromComponent(Deki::Component* comp, AssetUsageCollection& collection)
{
    if (!comp)
    {
        return;
    }

    const Deki::ComponentMeta* meta = Deki::ComponentRegistry::Instance().GetMeta(comp->GetType());
    if (!meta)
    {
        return;
    }

    for (int i = 0; i < meta->propertyCount; ++i)
    {
        const Deki::PropertyInfo& prop = meta->properties[i];

        if (prop.type == Deki::PropertyType::AssetRef)
        {
            const Deki::AssetRefBase& assetRef =
                *reinterpret_cast<const Deki::AssetRefBase*>(reinterpret_cast<const char*>(comp) + prop.offset);

            // The baked or compiled asset.
            if (!assetRef.guid.empty())
            {
                collection.usedGuids.insert(assetRef.guid);
            }

            // Its source asset (a sprite's texture, for example).
            if (!assetRef.source.empty())
            {
                collection.usedGuids.insert(assetRef.source);
            }

            // A font also needs its size.
            if (prop.assetType && strcmp(prop.assetType, "BitmapFont") == 0)
            {
                CollectFontUsage(comp, assetRef, collection);
            }
        }
    }
}

void AssetExporter::CollectFontUsage(Deki::Component* comp, const Deki::AssetRefBase& assetRef,
                                     AssetUsageCollection& collection)
{
    // The source GUID is the TTF file; baking it also needs the fontSize
    // property of the same component.

    if (assetRef.source.empty())
    {
        // No TTF source: a pre-baked font, used through its guid alone.
        return;
    }

    const Deki::ComponentMeta* meta = Deki::ComponentRegistry::Instance().GetMeta(comp->GetType());
    if (!meta)
    {
        return;
    }

    int fontSize = 16;  // Default
    for (int i = 0; i < meta->propertyCount; ++i)
    {
        const Deki::PropertyInfo& prop = meta->properties[i];
        if (strcmp(prop.name, "fontSize") == 0)
        {
            if (prop.type == Deki::PropertyType::Int32)
            {
                fontSize = *reinterpret_cast<const int32_t*>(reinterpret_cast<const char*>(comp) + prop.offset);
            }
            break;
        }
    }

    FontUsageInfo fontInfo;
    fontInfo.sourceGuid = assetRef.source;
    fontInfo.bdf = m_Pipeline && m_Pipeline->IsBdfFont(assetRef.source);
    fontInfo.fontSize = fontInfo.bdf ? 0 : fontSize;  // One baked form per BDF font
    collection.fontUsages.insert(fontInfo);

    DEKI_LOG_DEBUG("  Found font usage: %s @ %d px", assetRef.source.c_str(), fontSize);
}

// =============================================================================
// Font Baking
// =============================================================================

std::unordered_map<std::string, std::string>
AssetExporter::BakeFontsForExport(const std::set<FontUsageInfo>& fontUsages)
{
    namespace fs = std::filesystem;
    std::unordered_map<std::string, std::string> bakedFonts;

    std::string projectPath = m_Project->GetPath();
    std::string cachePath = GetCacheDirectory(projectPath);

    for (const auto& fontInfo : fontUsages)
    {
        // GUIDs derived from the font and its size (BakedFontGuid), so the
        // same font and size always get the same ones. AssetPacker uses them
        // too.
        std::string fontGuid = fontInfo.FontGuid();
        std::string atlasGuid = fontInfo.AtlasGuid();

        std::string fontCachePath = GetCachePath(projectPath, fontGuid);
        std::string atlasCachePath = GetCachePath(projectPath, atlasGuid);
        std::string key = fontInfo.sourceGuid + ":" + (fontInfo.bdf ? "bdf" : std::to_string(fontInfo.fontSize));

        if (fs::exists(fontCachePath) && fs::exists(atlasCachePath))
        {
            DEKI_LOG_DEBUG("  Font already baked: %s", key.c_str());
            bakedFonts[key] = fontGuid;
            bakedFonts[key + ":atlas"] = atlasGuid;
            continue;
        }

        const AssetInfo* assetInfo = m_Pipeline->GetAssetInfoByGuid(fontInfo.sourceGuid);
        if (!assetInfo)
        {
            DEKI_LOG_WARNING("  Font source not found: %s", fontInfo.sourceGuid.c_str());
            continue;
        }

        // A BDF font is baked by deki-2d when the font is imported, once
        // whatever the size; the TrueType compiler below cannot read it. Not
        // baked yet (a cleaned cache), it is imported again, which bakes it.
        if (fontInfo.bdf)
        {
            m_Pipeline->RefreshAsset(assetInfo->path);
            if (fs::exists(fontCachePath) && fs::exists(atlasCachePath))
            {
                DEKI_LOG_INFO("  Baked BDF font: %s -> %s", assetInfo->path.c_str(), fontGuid.c_str());
                bakedFonts[key] = fontGuid;
                bakedFonts[key + ":atlas"] = atlasGuid;
            }
            else
            {
                DEKI_LOG_ERROR("  BDF font not baked: %s", assetInfo->path.c_str());
                AddProblem("The BDF font " + assetInfo->path + " could not be baked (is the deki-2d package loaded?)");
            }
            continue;
        }

        std::string ttfPath = m_Pipeline->GetAbsolutePath(assetInfo->path);
        if (!fs::exists(ttfPath))
        {
            DEKI_LOG_WARNING("  TTF file not found: %s", ttfPath.c_str());
            continue;
        }

        // The font compiler lives in the deki-2d package.
        using CompileFontFunc = void* (*)(const char*, int, int, int, int, int);
        using GetAtlasFunc = bool (*)(const void*, const uint8_t**, int*, int*);
        using WriteDfontFunc = bool (*)(const char*, const void*, const char*);
        using FreeResultFunc = void (*)(void*);

        auto compileFont = m_PackageLoader ? reinterpret_cast<CompileFontFunc>(m_PackageLoader->GetFunction(
                                                 "Deki 2D Package", "DekiPluginCompileFont"))
                                           : nullptr;
        auto getAtlas = m_PackageLoader ? reinterpret_cast<GetAtlasFunc>(m_PackageLoader->GetFunction(
                                              "Deki 2D Package", "DekiPluginGetCompiledFontAtlas"))
                                        : nullptr;
        auto writeDfont = m_PackageLoader ? reinterpret_cast<WriteDfontFunc>(
                                                m_PackageLoader->GetFunction("Deki 2D Package", "DekiPluginWriteDfont"))
                                          : nullptr;
        auto freeResult = m_PackageLoader ? reinterpret_cast<FreeResultFunc>(m_PackageLoader->GetFunction(
                                                "Deki 2D Package", "DekiPluginFreeCompileResult"))
                                          : nullptr;

        if (!compileFont || !getAtlas || !writeDfont || !freeResult)
        {
            AddProblem("The font " + assetInfo->path + " at " + std::to_string(fontInfo.fontSize) +
                       " px is not baked, and the deki-2d package that bakes it is not loaded");
            continue;
        }

        void* compileResult = compileFont(ttfPath.c_str(), fontInfo.fontSize, 32, 126, 2, 2048);
        if (!compileResult)
        {
            DEKI_LOG_ERROR("  Failed to compile font: %s @ %d px", ttfPath.c_str(), fontInfo.fontSize);
            continue;
        }

        const uint8_t* atlasData = nullptr;
        int atlasWidth = 0, atlasHeight = 0;
        if (!getAtlas(compileResult, &atlasData, &atlasWidth, &atlasHeight))
        {
            DEKI_LOG_ERROR("  Failed to get font atlas data: %s", ttfPath.c_str());
            freeResult(compileResult);
            continue;
        }

        fs::create_directories(cachePath);

        if (!TextureImporter::WriteTexFile(atlasCachePath, atlasData, atlasWidth, atlasHeight, TextureFormat::ALPHA8))
        {
            DEKI_LOG_ERROR("  Failed to write font atlas: %s", atlasCachePath.c_str());
            freeResult(compileResult);
            continue;
        }

        if (!writeDfont(fontCachePath.c_str(), compileResult, atlasGuid.c_str()))
        {
            DEKI_LOG_ERROR("  Failed to write dfont file: %s", fontCachePath.c_str());
            freeResult(compileResult);
            continue;
        }

        freeResult(compileResult);

        DEKI_LOG_INFO("  Baked font: %s @ %d px -> %s", fontInfo.sourceGuid.c_str(), fontInfo.fontSize,
                      fontGuid.c_str());

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

    DEKI_LOG_EDITOR("Exporting used assets as GUID-named files (%zu used, %zu in packs)...", usedGuids.size(),
                    skipGuids.size());

    // First the pipeline's assets (textures, animations, scenes, ...), each
    // named by its GUID, with no folders and no extension.
    for (const auto& [path, info] : allAssets)
    {
        if (info.guid.empty())
        {
            continue;
        }

        if (usedGuids.find(info.guid) == usedGuids.end())
        {
            DEKI_LOG_DEBUG("  Skipping unused: %s", info.path.c_str());
            continue;
        }

        if (skipGuids.find(info.guid) != skipGuids.end())
        {
            DEKI_LOG_DEBUG("  Skipping packed: %s", info.path.c_str());
            continue;
        }

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

    // Then the used GUIDs the pipeline does not export, such as baked fonts,
    // which exist only in the cache directory.
    for (const std::string& guid : usedGuids)
    {
        // Exported by the loop above.
        const AssetInfo* assetInfo = m_Pipeline->GetAssetInfoByGuid(guid);
        if (assetInfo && assetInfo->hasCachedVersion)
        {
            continue;
        }

        if (skipGuids.find(guid) != skipGuids.end())
        {
            continue;
        }

        // This target's version of it, or what the cache directory holds.
        std::string cachePath = ExportSourceFor(guid);
        if (cachePath.empty())
        {
            // Not an asset of this project and nothing baked under that GUID:
            // the reference points at something deleted or never imported.
            // An asset the project has but does not compile, like a font's
            // source file, is fine: what it bakes into is exported instead.
            // So is a sheet's frame: it ships inside its sheet.
            if (!assetInfo && !m_Pipeline->GetSubAsset(guid))
            {
                std::string where;
                if (referencedFrom)
                {
                    auto it = referencedFrom->find(guid);
                    if (it != referencedFrom->end())
                    {
                        where = ", used by " + it->second;
                    }
                }
                AddProblem("Missing asset " + guid + where +
                           ". It was deleted or never imported; pick it again "
                           "or clear the reference.");
            }
            continue;
        }

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

    fs::path projectSettingsPath = fs::path(storagePath) / Deki::ProjectSettings::kExportFilename;
    std::ofstream settingsFile(projectSettingsPath, std::ios::binary);

    if (!settingsFile.is_open())
    {
        DEKI_LOG_WARNING("Failed to create %s", Deki::ProjectSettings::kExportFilename);
        return;
    }

    const auto& startupScene = m_Project->GetStartupScene();
    // Frame pacing comes from Core. Cameras carry their own framing, and the
    // framebuffer's size and format come from the device's display.
    auto* coreSettings = DekiEditor::GetCoreSettings();
    int32_t targetFPS = coreSettings ? coreSettings->targetFps : 60;

    std::string rendererName;
    std::vector<std::string> passNames;
    ResolvePipelineConfig(m_Project->GetRenderPipelineGuid(), m_Pipeline, rendererName, passNames);

    std::vector<const char*> passNamePtrs;
    for (const auto& name : passNames)
    {
        passNamePtrs.push_back(name.c_str());
    }

    std::vector<uint8_t> settingsBuffer = Deki::ProjectSettings::Write(
        m_Target.assetStorage, targetFPS, startupScene.empty() ? nullptr : startupScene.c_str(),
        rendererName.empty() ? nullptr : rendererName.c_str(), passNamePtrs.empty() ? nullptr : passNamePtrs.data(),
        static_cast<int>(passNamePtrs.size()), TargetScreenDiagonalInches(m_Target));
    settingsFile.write(reinterpret_cast<const char*>(settingsBuffer.data()), settingsBuffer.size());

    DEKI_LOG_EDITOR("Wrote %s (%zu bytes, pipeline=%s, %d passes)", Deki::ProjectSettings::kExportFilename,
                    settingsBuffer.size(), rendererName.empty() ? "(none)" : rendererName.c_str(),
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

    // project_data.bin, under its plain filename rather than the F:/ path.
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

    std::string rendererName2;
    std::vector<std::string> passNames2;
    ResolvePipelineConfig(m_Project->GetRenderPipelineGuid(), m_Pipeline, rendererName2, passNames2);

    std::vector<const char*> passNamePtrs2;
    for (const auto& name : passNames2)
    {
        passNamePtrs2.push_back(name.c_str());
    }

    std::vector<uint8_t> settingsBuffer = Deki::ProjectSettings::Write(
        m_Target.assetStorage, targetFPS, startupScene.empty() ? nullptr : startupScene.c_str(),
        rendererName2.empty() ? nullptr : rendererName2.c_str(), passNamePtrs2.empty() ? nullptr : passNamePtrs2.data(),
        static_cast<int>(passNamePtrs2.size()), TargetScreenDiagonalInches(m_Target));
    settingsFile.write(reinterpret_cast<const char*>(settingsBuffer.data()), settingsBuffer.size());
    settingsFile.close();

    DEKI_LOG_EDITOR("Wrote project_data.bin to the boot payload (%zu bytes)", settingsBuffer.size());

    if (!bootSceneAssetPath.empty() && m_Pipeline)
    {
        DEKI_LOG_EDITOR("Boot scene path: '%s'", bootSceneAssetPath.c_str());

        fs::path destPath = fs::path(payloadDir) / "boot.scene";
        bool exported = false;

        fs::path sourcePath = fs::path(m_Project->GetPath()) / bootSceneAssetPath;
        if (!fs::exists(sourcePath))
        {
            // Try as assets-relative path
            sourcePath = fs::path(GetAssetsDirectory(m_Project->GetPath())) / bootSceneAssetPath;
        }

        if (fs::exists(sourcePath))
        {
            // Objects this target cannot support are not part of its build, so
            // they are dropped before the scene is compiled. This is done on
            // the JSON, so the runtime never needs to know about targets and
            // the scene format stays readable by anything. The filtered copy
            // goes beside the output, never over the source.
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
                    DEKI_LOG_WARNING("Boot scene could not be filtered (%s); exporting it whole", e.what());
                    compileFrom = sourcePath;
                }
            }

            if (m_Pipeline->CompileSceneFile(compileFrom.string(), destPath.string()))
            {
                DEKI_LOG_EDITOR("Compiled boot scene into the boot payload: %s (%zu bytes)", destPath.string().c_str(),
                                fs::file_size(destPath));
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

        // Must fail the export: without its boot scene a device starts to a
        // blank screen.
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
        {
            DEKI_LOG_INFO("No boot scene specified in platform config");
        }
        if (!m_Pipeline)
        {
            DEKI_LOG_WARNING("No asset pipeline available for boot scene export");
        }
    }

    DEKI_LOG_EDITOR("Boot payload export complete");
    return true;
}

// FNV-1a hash of a "path:type" key.
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

bool AssetExporter::ExportAssetLookupTable(const std::string& outputPath,
                                           const std::unordered_set<std::string>& usedGuids)
{
    namespace fs = std::filesystem;

    // One (hash, guid) entry per used asset.
    struct LookupEntry
    {
        uint32_t hash;
        char guid[37];    // 36 chars + null terminator
        char padding[3];  // Align to 44 bytes

        bool operator<(const LookupEntry& other) const { return hash < other.hash; }
    };
    static_assert(sizeof(LookupEntry) == 44, "LookupEntry must be 44 bytes");

    std::vector<LookupEntry> entries;
    std::unordered_set<std::string> addedGuids;

    const auto& allAssets = m_Pipeline->GetAllAssets();
    std::string projectPath = m_Project->GetPath();

    // First the pipeline's assets, keyed by "path:type".
    for (const auto& [path, info] : allAssets)
    {
        if (info.guid.empty())
        {
            continue;
        }

        if (usedGuids.find(info.guid) == usedGuids.end())
        {
            continue;
        }

        std::string ext = fs::path(info.path).extension().string();
        std::string assetType = AssetPipeline::GetAssetTypeFromExtension(ext);
        if (assetType.empty())
        {
            continue;
        }

        // The path without the "project/assets/" or "assets/" prefix and without the extension.
        std::string exportPath = info.path;
        const std::string assetsPrefix = GetAssetsDirectoryRelative() + "/";
        if (exportPath.rfind(assetsPrefix, 0) == 0)
        {
            exportPath = exportPath.substr(assetsPrefix.size());
        }
        else if (exportPath.rfind("assets/", 0) == 0)
        {
            exportPath = exportPath.substr(7);
        }
        size_t dotPos = exportPath.rfind('.');
        if (dotPos != std::string::npos)
        {
            exportPath = exportPath.substr(0, dotPos);
        }

        std::string key = exportPath + ":" + assetType;

        LookupEntry entry;
        memset(&entry, 0, sizeof(entry));
        entry.hash = FNV1aHash(key.c_str());
        strncpy(entry.guid, info.guid.c_str(), sizeof(entry.guid) - 1);

        DEKI_LOG_DEBUG("ExportAssetLookupTable: key='%s' hash=0x%08X guid=%s", key.c_str(), entry.hash,
                       info.guid.c_str());
        entries.push_back(entry);
        addedGuids.insert(info.guid);
    }

    // Then the assets only in the cache (baked fonts, ...), keyed by the GUID
    // itself for direct lookup.
    for (const std::string& guid : usedGuids)
    {
        if (addedGuids.find(guid) != addedGuids.end())
        {
            continue;
        }

        std::string cachePath = GetCachePath(projectPath, guid);
        if (!fs::exists(cachePath))
        {
            continue;
        }

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

    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open())
    {
        DEKI_LOG_ERROR("ExportAssetLookupTable: Failed to create file: %s", outputPath.c_str());
        return false;
    }

    uint32_t count = static_cast<uint32_t>(entries.size());
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& entry : entries)
    {
        file.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
    }

    DEKI_LOG_EDITOR("ExportAssetLookupTable: Wrote %u entries to %s", count, outputPath.c_str());
    return true;
}

}  // namespace DekiEditor
