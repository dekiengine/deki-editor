#pragma once

#include <string>

#include <deki/Engine.h>  // Deki::ColorFormat
#include <deki-editor/TextureData.h>

/**
 * @brief Which format an image is stored in for a given target.
 *
 * An image's sidecar (`<image>.data`, `settings.texture`) says:
 *   "format":  "Automatic" (or absent) or a format name, for every target
 *   "targets": { "<platformId>": "Automatic" | a format name }
 *
 * The first that names a format wins: the target's entry, then the image's
 * own. Automatic follows the target's screen (its platform `colorFormat`):
 *
 *   screen          opaque image   image with alpha
 *   RGB565          RGB565         RGB565A8
 *   RGB888/ARGB8888 RGB888         RGBA8888
 *   RGB565A8        RGB565A8       RGB565A8
 *
 * The editor cache, the per-target export and the texture inspector all ask
 * this one function, so they cannot disagree.
 */

namespace DekiEditor
{

constexpr const char* kAutomaticFormat = "Automatic";

/// A texture format by the name the sidecar and the inspector use. False for
/// "Automatic", an empty string, or anything unknown.
inline bool ParseTextureFormat(const std::string& name, TextureFormat& out)
{
    if (name == "RGB888")
    {
        out = TextureFormat::RGB888;
        return true;
    }
    if (name == "RGBA8888")
    {
        out = TextureFormat::RGBA8888;
        return true;
    }
    if (name == "RGB565")
    {
        out = TextureFormat::RGB565;
        return true;
    }
    if (name == "RGB565A8")
    {
        out = TextureFormat::RGB565A8;
        return true;
    }
    if (name == "ALPHA8")
    {
        out = TextureFormat::ALPHA8;
        return true;
    }
    return false;
}

inline const char* TextureFormatName(TextureFormat f)
{
    switch (f)
    {
        case TextureFormat::RGB888: return "RGB888";
        case TextureFormat::RGBA8888: return "RGBA8888";
        case TextureFormat::RGB565: return "RGB565";
        case TextureFormat::RGB565A8: return "RGB565A8";
        case TextureFormat::ALPHA8: return "ALPHA8";
    }
    return "RGB565A8";
}

/// A screen format by the name a platform file uses ("RGB565", ...).
inline bool ParseColorFormat(const std::string& name, Deki::ColorFormat& out)
{
    if (name == "RGB565")
    {
        out = Deki::ColorFormat::RGB565;
        return true;
    }
    if (name == "RGB888")
    {
        out = Deki::ColorFormat::RGB888;
        return true;
    }
    if (name == "ARGB8888")
    {
        out = Deki::ColorFormat::ARGB8888;
        return true;
    }
    if (name == "RGB565A8")
    {
        out = Deki::ColorFormat::RGB565A8;
        return true;
    }
    return false;
}

inline const char* ColorFormatName(Deki::ColorFormat f)
{
    switch (f)
    {
        case Deki::ColorFormat::RGB565: return "RGB565";
        case Deki::ColorFormat::RGB888: return "RGB888";
        case Deki::ColorFormat::ARGB8888: return "ARGB8888";
        case Deki::ColorFormat::RGB565A8: return "RGB565A8";
    }
    return "RGB565";
}

/// Automatic for a screen of this format.
inline TextureFormat AutomaticTextureFormat(Deki::ColorFormat screen, bool hasAlpha)
{
    switch (screen)
    {
        case Deki::ColorFormat::RGB565: return hasAlpha ? TextureFormat::RGB565A8 : TextureFormat::RGB565;
        case Deki::ColorFormat::RGB888:
        case Deki::ColorFormat::ARGB8888: return hasAlpha ? TextureFormat::RGBA8888 : TextureFormat::RGB888;
        case Deki::ColorFormat::RGB565A8: return TextureFormat::RGB565A8;
    }
    return hasAlpha ? TextureFormat::RGB565A8 : TextureFormat::RGB565;
}

/// The stored format for one target. `assetFormat` is settings.texture.format
/// and `targetFormat` that target's entry in settings.texture.targets; either
/// may be empty or "Automatic".
inline TextureFormat ResolveTextureFormat(const std::string& assetFormat, const std::string& targetFormat,
                                          Deki::ColorFormat screen, bool hasAlpha)
{
    TextureFormat f;
    if (ParseTextureFormat(targetFormat, f))
    {
        return f;
    }
    if (ParseTextureFormat(assetFormat, f))
    {
        return f;
    }
    return AutomaticTextureFormat(screen, hasAlpha);
}

}  // namespace DekiEditor
