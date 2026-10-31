#include "SceneMemoryEstimate.h"

#include <deki-editor/AssetData.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/Localization.h>
#include <deki-editor/build/ProjectPaths.h>
#include <deki-editor/TextureFormatResolve.h>
#include <deki-editor/TextureImporter.h>
#include <deki-editor/TextureSettings.h>
#include <deki-editor/build/PlatformConfig.h>

#include <deki/Component.h>
#include <deki/Engine.h>
#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/assets/AssetRef.h>
#include <deki/reflection/ComponentRegistry.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

int BytesPerTexel(TextureFormat f)
{
    switch (f)
    {
        case TextureFormat::RGB888: return 3;
        case TextureFormat::RGBA8888: return 4;
        case TextureFormat::RGB565: return 2;
        case TextureFormat::RGB565A8: return 3;
        case TextureFormat::ALPHA8: return 1;
    }
    return 3;
}

int BytesPerPixel(Deki::ColorFormat f)
{
    switch (f)
    {
        case Deki::ColorFormat::RGB565: return 2;
        case Deki::ColorFormat::RGB888: return 3;
        case Deki::ColorFormat::ARGB8888: return 4;
        case Deki::ColorFormat::RGB565A8: return 3;
    }
    return 2;
}

nlohmann::json ReadJson(const fs::path& path)
{
    std::ifstream in(path);
    if (!in.is_open())
    {
        return nlohmann::json::object();
    }
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

uint64_t FileSize(const std::string& path)
{
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(size);
}

// The frame buffer or strip on `platform`, as deki-rendering's render system
// picks it (DekiRenderSystem::ChooseFrameMode), from the project's Rendering
// settings: frameMode 0 Auto, 1 whole frames, 2 strips; bandRows rounded to
// pairs.
void AddRendering(const std::string& projectPath, const PlatformConfig* platform, int64_t measuredFree,
                  SceneMemoryEstimate& out)
{
    int width = platform ? platform->screenWidth : 0;
    int height = platform ? platform->screenHeight : 0;
    Deki::ColorFormat format = Deki::ColorFormat::RGB565;
    if (platform)
    {
        ParseColorFormat(platform->colorFormat, format);
    }
    if (width <= 0 || height <= 0)
    {
        return;
    }
    const int bpp = BytesPerPixel(format);
    const uint64_t frameBytes = static_cast<uint64_t>(width) * height * bpp;

    const nlohmann::json settings =
        ReadJson(ProjectPaths::Settings(projectPath) / "packages" / "deki-rendering" / "rendering.json");
    const nlohmann::json rendering = settings.value("Rendering", nlohmann::json::object());
    const int mode = rendering.value("frameMode", 0);
    int rows = rendering.value("bandRows", 40);
    rows = std::max(2, rows - (rows % 2));
    const uint64_t stripBytes = static_cast<uint64_t>(width) * rows * bpp;

    // The editor's desktop and the simulator draw whole frames: their heap
    // has no limit worth splitting the frame for.
    const bool desktop = !platform || platform->family == "native";
    bool strips = false;
    if (!desktop)
    {
        if (mode == 2)
        {
            strips = true;
            out.frameModeNote = TrMark("strips (Rendering setting)");
        }
        else if (mode == 1)
        {
            out.frameModeNote = TrMark("whole frames (Rendering setting)");
        }
        else if (measuredFree >= 0)
        {
            // The board's rule: whole frames when the frame fits with 64 KB
            // to spare (DekiRenderSystem::kInternalHeadroom).
            strips = frameBytes + 64 * 1024 > static_cast<uint64_t>(measuredFree);
            out.frameModeNote = strips
                                    ? TrMark("Auto: strips, the frame does not fit in the free internal RAM measured")
                                    : TrMark("Auto: whole frames, the frame fits in the free internal RAM measured");
        }
        else
        {
            out.frameModeNote = TrMark("Auto: whole frames if the frame fits in internal RAM with 64 KB to spare, "
                                       "otherwise strips. Read a board in Live to know which");
        }
    }
    else
    {
        out.frameModeNote = TrMark("whole frames");
    }

    char detail[96];
    MemoryEstimateItem frame;
    frame.kind = TrMark("Rendering");
    if (strips)
    {
        frame.name = TrMark("Render strip");
        std::snprintf(detail, sizeof(detail), "%d x %d %s", width, rows, platform->colorFormat.c_str());
        frame.detail = detail;
        frame.bytes = stripBytes;
        out.rendering.push_back(frame);

        // deki-display sends a strip while the next is drawn, from two copies
        // at 2 bytes a pixel (RenderingProjectSettings' bandRows tooltip).
        MemoryEstimateItem send;
        send.kind = TrMark("Rendering");
        send.name = TrMark("Strip copies for sending");
        std::snprintf(detail, sizeof(detail), "2 x %d x %d", width, rows);
        send.detail = detail;
        send.bytes = 2ull * width * rows * 2;
        out.rendering.push_back(send);
    }
    else
    {
        frame.name = TrMark("Frame buffer");
        std::snprintf(detail, sizeof(detail), "%d x %d %s", width, height,
                      platform ? platform->colorFormat.c_str() : "RGB565");
        frame.detail = detail;
        frame.bytes = frameBytes;
        out.rendering.push_back(frame);
    }
}

const char* AssetTypeOf(const Deki::PropertyInfo& prop)
{
    return prop.assetType && prop.assetType[0] ? prop.assetType : "Asset";
}

}  // namespace

bool EstimateImageBytes(const std::string& assetPath, const std::string& cachePath, const PlatformConfig* platform,
                        uint64_t& bytes, std::string& detail)
{
    // Reading a compiled image means reading all its pixels, so what it says
    // is kept until the file changes: the Memory panel asks every few seconds.
    struct Source
    {
        fs::file_time_type written;
        bool image = false;
        int width = 0;
        int height = 0;
        bool alpha = false;
    };
    static std::mutex s_Mutex;
    static std::unordered_map<std::string, Source> s_Sources;
    std::error_code ec;
    const fs::file_time_type written = cachePath.empty() ? fs::file_time_type{} : fs::last_write_time(cachePath, ec);
    if (cachePath.empty() || ec)
    {
        return false;
    }
    Source src;
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        auto it = s_Sources.find(cachePath);
        if (it != s_Sources.end() && it->second.written == written)
        {
            src = it->second;
        }
        else
        {
            TexData tex;
            src.written = written;
            src.image = TextureImporter::ReadTexFile(cachePath, tex) &&
                        TextureImporter::ReadSourceSize(cachePath, src.width, src.height) && src.width > 0 &&
                        src.height > 0;
            src.alpha = src.image && tex.HasAlpha();
            s_Sources[cachePath] = src;
        }
    }
    if (!src.image)
    {
        return false;
    }
    const int srcW = src.width;
    const int srcH = src.height;

    // The image's own settings, from its .data sidecar, as the export reads
    // them: a format and Max Size for every target, and its own per target.
    TextureSettings settings;
    const nlohmann::json data = ReadJson(assetPath + ".data");
    if (data.contains("settings") && data["settings"].contains("texture"))
    {
        settings = ReadTextureSettings(data["settings"]["texture"]);
    }
    const std::string platformId = platform ? platform->id : "";
    Deki::ColorFormat screen = Deki::ColorFormat::RGB565;
    if (platform)
    {
        ParseColorFormat(platform->colorFormat, screen);
    }
    const TextureFormat stored =
        ResolveTextureFormat(settings.format, settings.TargetFormat(platformId), screen, src.alpha);
    int w = 0, h = 0;
    MaxSizeDims(srcW, srcH, settings.MaxSizeFor(platformId), w, h);
    bytes = static_cast<uint64_t>(w) * h * BytesPerTexel(stored);

    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d x %d %s", w, h, TextureFormatName(stored));
    detail = buf;
    return true;
}

SceneMemoryEstimate EstimateSceneMemory(const std::string& projectPath, AssetPipeline* pipeline, Deki::Scene* scene,
                                        const PlatformConfig* platform, int64_t measuredInternalTotal,
                                        int64_t measuredInternalFree)
{
    SceneMemoryEstimate out;
    if (platform)
    {
        out.targetId = platform->id;
        out.targetName = platform->displayName.empty() ? platform->id : platform->displayName;
        out.hasExternal = platform->externalMemorySize > 0;
        out.externalBudget = platform->externalMemorySize;
    }
    if (measuredInternalTotal > 0)
    {
        out.internalBudget = static_cast<uint64_t>(measuredInternalTotal);
        out.internalBudgetMeasured = true;
    }

    AddRendering(projectPath, platform, measuredInternalFree, out);
    if (!scene)
    {
        return out;
    }

    // Each asset once, however many objects use it.
    std::unordered_map<std::string, size_t> assetIndex;
    auto addAsset = [&](const std::string& guid, const std::string& kind, const std::string& lookupGuid) -> bool
    {
        if (guid.empty())
        {
            return false;
        }
        if (assetIndex.count(guid))
        {
            return true;
        }
        MemoryEstimateItem item;
        item.guid = guid;
        item.kind = kind;
        item.external = out.hasExternal;

        const AssetInfo* info = pipeline ? pipeline->GetAssetInfoByGuid(lookupGuid) : nullptr;
        const std::string cacheAbs = info && info->hasCachedVersion
                                         ? pipeline->GetAbsolutePath(info->cachePath)
                                         : (ProjectPaths::Cache(projectPath) / lookupGuid).string();
        item.name = info ? info->path : guid;
        const std::string assetsPrefix = "assets/";
        if (item.name.rfind(assetsPrefix, 0) == 0)
        {
            item.name = item.name.substr(assetsPrefix.size());
        }
        uint64_t bytes = 0;
        std::string detail;
        if (info && EstimateImageBytes(pipeline->GetAbsolutePath(info->path), cacheAbs, platform, bytes, detail))
        {
            item.bytes = bytes;
            item.detail = detail;
        }
        else
        {
            item.bytes = FileSize(cacheAbs);
            item.fileSize = true;
            item.detail = TrMark("file size");
        }
        assetIndex[guid] = out.assets.size();
        out.assets.push_back(item);
        return true;
    };

    std::unordered_set<Deki::Object*> seen;
    std::function<void(Deki::Object*, const std::string&)> visit = [&](Deki::Object* obj, const std::string& parent)
    {
        if (!obj || !seen.insert(obj).second)
        {
            return;
        }
        ObjectMemoryEstimate o;
        o.object = obj;
        o.name = obj->GetName();
        o.path = parent.empty() ? o.name : parent + "/" + o.name;
        o.ownBytes = sizeof(Deki::Object);

        std::set<std::string> objectAssets;
        for (Deki::Component* comp : obj->GetComponents())
        {
            const Deki::ComponentMeta* meta =
                comp ? Deki::ComponentRegistry::Instance().GetMeta(comp->GetType()) : nullptr;
            if (!meta)
            {
                continue;
            }
            o.ownBytes += meta->instanceSize;
            o.components.push_back(meta->GetDisplayName());

            int fontSize = 16;
            for (int i = 0; i < meta->propertyCount; ++i)
            {
                const Deki::PropertyInfo& prop = meta->properties[i];
                if (prop.type == Deki::PropertyType::Int32 && std::strcmp(prop.name, "fontSize") == 0)
                {
                    fontSize = *reinterpret_cast<const int32_t*>(reinterpret_cast<const char*>(comp) + prop.offset);
                }
            }
            for (int i = 0; i < meta->propertyCount; ++i)
            {
                const Deki::PropertyInfo& prop = meta->properties[i];
                if (prop.type != Deki::PropertyType::AssetRef)
                {
                    continue;
                }
                const auto& ref =
                    *reinterpret_cast<const Deki::AssetRefBase*>(reinterpret_cast<const char*>(comp) + prop.offset);
                // A scene a component names (a Scene Link's) is opened in place
                // of this one, never beside it, so it takes nothing here.
                if (prop.assetType && std::strcmp(prop.assetType, "Scene") == 0)
                {
                    continue;
                }
                const bool font = prop.assetType && std::strcmp(prop.assetType, "BitmapFont") == 0;
                if (font && ref.HasSource())
                {
                    // A TrueType font is baked per size: the build ships the
                    // baked font and its atlas, which exist only in the cache.
                    const bool bdf = pipeline && pipeline->IsBdfFont(ref.source);
                    const int size = bdf ? 0 : fontSize;
                    const std::string baked = BakedFontGuid(ref.source, size, bdf);
                    const std::string atlas = BakedFontAtlasGuid(ref.source, size, bdf);
                    if (addAsset(baked, "BitmapFont", baked))
                    {
                        const AssetInfo* src = pipeline ? pipeline->GetAssetInfoByGuid(ref.source) : nullptr;
                        char label[256];
                        std::snprintf(label, sizeof(label), "%s @ %dpx",
                                      src ? fs::path(src->path).stem().string().c_str() : ref.source.c_str(), size);
                        out.assets[assetIndex[baked]].name = label;
                        objectAssets.insert(baked);
                    }
                    if (addAsset(atlas, "BitmapFont", atlas))
                    {
                        const AssetInfo* src = pipeline ? pipeline->GetAssetInfoByGuid(ref.source) : nullptr;
                        char label[256];
                        std::snprintf(label, sizeof(label), Tr("%s atlas @ %dpx"),
                                      src ? fs::path(src->path).stem().string().c_str() : ref.source.c_str(), size);
                        out.assets[assetIndex[atlas]].name = label;
                        objectAssets.insert(atlas);
                    }
                    continue;
                }
                // A frame cut from a sheet is not an asset of its own: its
                // source is the sheet that loads.
                const bool own = pipeline && pipeline->GetAssetInfoByGuid(ref.guid);
                const std::string guid = own || ref.source.empty() ? ref.guid : ref.source;
                if (addAsset(guid, AssetTypeOf(prop), guid))
                {
                    objectAssets.insert(guid);
                }
            }
        }
        // An asset counts once per object, however many fields name it.
        for (const std::string& guid : objectAssets)
        {
            o.assetGuids.push_back(guid);
        }
        // A copy: visiting the children adds rows, so back() moves on.
        const std::string path = o.path;
        out.objects.push_back(std::move(o));

        for (Deki::Object* child : obj->GetChildren())
        {
            visit(child, path);
        }
    };
    for (Deki::Object* obj : scene->GetObjects())
    {
        if (!obj->GetParent())
        {
            visit(obj, "");
        }
    }
    // Anything GetObjects lists whose parent is not in it.
    for (Deki::Object* obj : scene->GetObjects())
    {
        visit(obj, "");
    }

    for (ObjectMemoryEstimate& o : out.objects)
    {
        for (const std::string& guid : o.assetGuids)
        {
            MemoryEstimateItem& a = out.assets[assetIndex[guid]];
            ++a.users;
            o.assetBytes += a.bytes;
        }
        out.objectBytes += o.ownBytes;
    }

    for (const MemoryEstimateItem& r : out.rendering)
    {
        out.internalBytes += r.bytes;
    }
    for (const MemoryEstimateItem& a : out.assets)
    {
        (a.external ? out.externalBytes : out.internalBytes) += a.bytes;
    }
    out.internalBytes += out.objectBytes;

    std::sort(out.assets.begin(), out.assets.end(),
              [](const MemoryEstimateItem& a, const MemoryEstimateItem& b) { return a.bytes > b.bytes; });
    std::sort(out.objects.begin(), out.objects.end(), [](const ObjectMemoryEstimate& a, const ObjectMemoryEstimate& b)
              { return a.ownBytes + a.assetBytes > b.ownBytes + b.assetBytes; });
    return out;
}

}  // namespace DekiEditor
