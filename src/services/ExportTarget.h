#pragma once

#include <string>

#include <deki-editor/AssetPipeline.h>
#include <deki-editor/TextureFormatResolve.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki/LogSystem.h>

namespace DekiEditor
{

/// The export target a platform describes: its id and its screen's format.
/// False, and says why, when the platform is unknown or its colorFormat is not
/// one Deki draws.
inline bool ExportTargetForPlatform(const std::string& platformId, AssetExportTarget& out)
{
    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(platformId);
    if (!platform)
    {
        DEKI_LOG_ERROR("No platform '%s' to export for", platformId.c_str());
        return false;
    }
    Deki::ColorFormat format;
    if (!ParseColorFormat(platform->colorFormat, format))
    {
        DEKI_LOG_ERROR("Platform '%s' has colorFormat '%s'; expected RGB565, RGB888, ARGB8888 or RGB565A8",
                       platformId.c_str(), platform->colorFormat.c_str());
        return false;
    }
    out.platformId = platformId;
    out.colorFormat = format;
    return true;
}

}  // namespace DekiEditor
