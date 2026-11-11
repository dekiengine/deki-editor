#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>

#include <deki-editor/EditorExport.h>
#include <deki/assets/TextureFilter.h>
#include <nlohmann/json.hpp>

// How an image is stored and drawn, per target: its format, its Max Size and
// its filter. The image's settings are the only ones: a model's textures
// follow the images they are, as sprites do.
//
// An image's sidecar (`<image>.data`, `settings.texture`) says:
//
//   "format":  "Automatic" (or absent) or a format name, for every target
//   "maxSize": the largest side it is stored at, 0 (or absent) for no limit
//   "filter":  "Point" (or absent), "Bilinear" or "Trilinear" (TextureFilter.h)
//   "targets": { "<platformId>": { "format": ..., "maxSize": ..., "filter": ... } }
//
// A target's entry overrides the image's own value for whichever it names;
// its "maxSize" of 0 means no limit on that target, even when the image has
// one. A target without a GPU draws Point whatever the filter says
// (ResolveTextureFilter).
//
// Max Size works like Unity's: an image whose larger side is over it is stored
// scaled so that side is exactly Max Size, the other side in proportion.
// Everything that addresses the image's pixels (sprite frames, nine-slice
// borders, tileset tiles) keeps using the image's own pixels; the stored file
// records the original size, and the runtime maps a source rect to stored
// pixels with SourceToStored below.

namespace DekiEditor
{

/// The Max Size choices, as Unity offers them. 0 means no limit.
constexpr int kTextureMaxSizes[] = { 0, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192 };
constexpr int kTextureMaxSizeCount = sizeof(kTextureMaxSizes) / sizeof(kTextureMaxSizes[0]);

/// One target's own values. Format "", maxSize -1 and filter "" mean "use
/// the image's default"; maxSize 0 means no limit on this target.
struct TextureTargetSettings
{
    std::string format;
    int maxSize = -1;
    std::string filter;

    bool Empty() const { return format.empty() && maxSize < 0 && filter.empty(); }
};

/// An image's storage and filter settings, with per-target overrides.
struct TextureSettings
{
    std::string format;  // "" or "Automatic" = Automatic
    int maxSize = 0;     // 0 = no limit
    std::string filter;  // "" = Point
    std::map<std::string, TextureTargetSettings> targets;

    /// The filter an image asks for on a target: its own, else the image's.
    /// What the target can draw is ResolveTextureFilter's to say.
    std::string FilterFor(const std::string& platformId) const
    {
        auto it = targets.find(platformId);
        return it != targets.end() && !it->second.filter.empty() ? it->second.filter : filter;
    }

    /// The format a target names for itself, or "" to use the image's.
    std::string TargetFormat(const std::string& platformId) const
    {
        auto it = targets.find(platformId);
        return it == targets.end() ? std::string() : it->second.format;
    }

    /// The Max Size that applies on a target: its own, else the image's.
    int MaxSizeFor(const std::string& platformId) const
    {
        auto it = targets.find(platformId);
        if (it != targets.end() && it->second.maxSize >= 0)
        {
            return it->second.maxSize;
        }
        return maxSize;
    }
};

/// Reads `settings.texture`. Pass the object itself; a missing one gives defaults.
inline TextureSettings ReadTextureSettings(const nlohmann::json& tex)
{
    TextureSettings out;
    if (!tex.is_object())
    {
        return out;
    }
    if (tex.contains("format") && tex["format"].is_string())
    {
        out.format = tex["format"].get<std::string>();
    }
    if (tex.contains("maxSize") && tex["maxSize"].is_number_integer())
    {
        out.maxSize = std::max(0, tex["maxSize"].get<int>());
    }
    if (tex.contains("filter") && tex["filter"].is_string())
    {
        out.filter = tex["filter"].get<std::string>();
    }
    if (tex.contains("targets") && tex["targets"].is_object())
    {
        for (auto it = tex["targets"].begin(); it != tex["targets"].end(); ++it)
        {
            TextureTargetSettings t;
            if (it.value().is_object())
            {
                if (it.value().contains("format") && it.value()["format"].is_string())
                {
                    t.format = it.value()["format"].get<std::string>();
                }
                if (it.value().contains("maxSize") && it.value()["maxSize"].is_number_integer())
                {
                    t.maxSize = std::max(0, it.value()["maxSize"].get<int>());
                }
                if (it.value().contains("filter") && it.value()["filter"].is_string())
                {
                    t.filter = it.value()["filter"].get<std::string>();
                }
            }
            if (!t.Empty())
            {
                out.targets[it.key()] = t;
            }
        }
    }
    return out;
}

/// Writes one target's values into `settings.texture`, always in the object
/// form. An empty entry is removed.
inline void WriteTextureTarget(nlohmann::json& tex, const std::string& platformId, const TextureTargetSettings& t)
{
    if (t.Empty())
    {
        if (tex.contains("targets") && tex["targets"].is_object())
        {
            tex["targets"].erase(platformId);
            if (tex["targets"].empty())
            {
                tex.erase("targets");
            }
        }
        return;
    }
    nlohmann::json entry = nlohmann::json::object();
    if (!t.format.empty())
    {
        entry["format"] = t.format;
    }
    if (t.maxSize >= 0)
    {
        entry["maxSize"] = t.maxSize;
    }
    if (!t.filter.empty())
    {
        entry["filter"] = t.filter;
    }
    tex["targets"][platformId] = entry;
}

/// The size an image of srcW x srcH is stored at under `maxSize` (0 = no
/// limit): unchanged when it fits, else its larger side becomes maxSize and
/// the other keeps the proportion (at least 1).
inline void MaxSizeDims(int srcW, int srcH, int maxSize, int& outW, int& outH)
{
    outW = srcW;
    outH = srcH;
    const int larger = std::max(srcW, srcH);
    if (maxSize <= 0 || larger <= maxSize)
    {
        return;
    }
    if (srcW >= srcH)
    {
        outW = maxSize;
        outH = std::max(1, static_cast<int>((static_cast<int64_t>(srcH) * maxSize * 2 + srcW) / (2 * srcW)));
    }
    else
    {
        outH = maxSize;
        outW = std::max(1, static_cast<int>((static_cast<int64_t>(srcW) * maxSize * 2 + srcH) / (2 * srcH)));
    }
}

/// The texture settings of the image at `imagePath` (absolute), from its
/// sidecar (`<image>.data`, settings.texture); defaults when it has none. How
/// anything that stores an image (a model's textures) reads the image's own
/// settings.
DEKI_EDITOR_API TextureSettings ReadImageTextureSettings(const std::string& imagePath);

/// The capabilities with which the project's packages draw textures filtered
/// (CMakeGen::TextureFilteringCapabilities of its packages). The editor sets
/// them when it opens a project; none are set before.
DEKI_EDITOR_API void SetTextureFilteringCapabilities(std::set<std::string> capabilities);

/// Whether `platformId` draws textures filtered, so a texture's filter means
/// something there: a platform that provides a capability some package
/// filters with (deki-rendering's "opengl"), or "" (the editor's own desktop).
DEKI_EDITOR_API bool TargetSamplesTextures(const std::string& platformId);

/// The filter an image is drawn with on `platformId`: FilterFor's, or Point
/// where the target has no GPU (TargetSamplesTextures), and for a name that
/// is not a filter. What the compiled image and mesh record.
DEKI_EDITOR_API Deki::TextureFilter ResolveTextureFilter(const TextureSettings& settings,
                                                         const std::string& platformId);

/// A source-pixel coordinate on one axis (a rect edge, not a size) in stored
/// pixels: round(v * stored / source). The runtime (Deki2D::Sprite) uses the
/// same rule, so a frame the encoder resampled lands where the loader looks.
inline int SourceToStored(int v, int sourceSize, int storedSize)
{
    if (sourceSize <= 0 || sourceSize == storedSize)
    {
        return v;
    }
    return static_cast<int>((static_cast<int64_t>(v) * storedSize * 2 + sourceSize) /
                            (2 * static_cast<int64_t>(sourceSize)));
}

}  // namespace DekiEditor
