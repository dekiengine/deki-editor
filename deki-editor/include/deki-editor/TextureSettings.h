#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

/**
 * @brief How an image is stored, per target: its format and its Max Size.
 *
 * An image's sidecar (`<image>.data`, `settings.texture`) says:
 *
 *   "format":  "Automatic" (or absent) or a format name, for every target
 *   "maxSize": the largest side it is stored at, 0 (or absent) for no limit
 *   "targets": { "<platformId>": { "format": ..., "maxSize": ... } }
 *
 * A target's entry overrides the image's own value for whichever of the two
 * it names; its "maxSize" of 0 is "no limit" on that target, even when the
 * image has one. A target entry that is a plain string is the format only
 * (the form before Max Size existed), and is read as such.
 *
 * Max Size works like Unity's: an image whose larger side is over it is
 * stored scaled so that side is exactly Max Size, the other side in
 * proportion. Everything that addresses the image's pixels (sprite frames,
 * nine-slice borders, tileset tiles) keeps using the image's own pixels; the
 * stored file records the original size, and the runtime maps a source rect
 * to the stored pixels with SourceToStored below.
 */

namespace DekiEditor
{

/// The Max Size choices, as Unity offers them. 0 is "no limit".
constexpr int kTextureMaxSizes[] = { 0, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192 };
constexpr int kTextureMaxSizeCount = sizeof(kTextureMaxSizes) / sizeof(kTextureMaxSizes[0]);

/// One target's own values: format "" and maxSize -1 mean "as the image's
/// Default"; maxSize 0 is "no limit" here.
struct TextureTargetSettings
{
    std::string format;
    int maxSize = -1;

    bool Empty() const { return format.empty() && maxSize < 0; }
};

struct TextureSettings
{
    std::string format;  // "" or "Automatic" = Automatic
    int maxSize = 0;     // 0 = no limit
    std::map<std::string, TextureTargetSettings> targets;

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

/// Read `settings.texture` (pass the object itself; a missing one is fine).
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
    if (tex.contains("targets") && tex["targets"].is_object())
    {
        for (auto it = tex["targets"].begin(); it != tex["targets"].end(); ++it)
        {
            TextureTargetSettings t;
            if (it.value().is_string())
            {
                t.format = it.value().get<std::string>();
            }
            else if (it.value().is_object())
            {
                if (it.value().contains("format") && it.value()["format"].is_string())
                {
                    t.format = it.value()["format"].get<std::string>();
                }
                if (it.value().contains("maxSize") && it.value()["maxSize"].is_number_integer())
                {
                    t.maxSize = std::max(0, it.value()["maxSize"].get<int>());
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

/// Write one target's values into `settings.texture`; an empty entry is
/// removed. Always written in the object form.
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
