#pragma once

#include <string>

#include <deki-editor/AssetPipeline.h>
#include <deki-editor/TextureFormatResolve.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki/LogSystem.h>

namespace DekiEditor
{

/// The export target a platform describes: its id, its screen's format and
/// where it keeps its assets. False, and says why, when the platform is
/// unknown, its colorFormat is not one Deki draws, or it asks for external
/// storage it has not got.
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
    if (platform->assetStorage == "external")
    {
        if (platform->Capabilities().count("external-storage") == 0)
        {
            DEKI_LOG_ERROR("Platform '%s' keeps its assets on external storage but does not provide "
                           "\"external-storage\"",
                           platformId.c_str());
            return false;
        }
        out.assetStorage = Deki::Storage::External;
    }
    else if (platform->assetStorage == "internal")
        out.assetStorage = Deki::Storage::Internal;
    else
    {
        DEKI_LOG_ERROR("Platform '%s' has assetStorage '%s'; expected internal or external",
                       platformId.c_str(), platform->assetStorage.c_str());
        return false;
    }
    out.platformId = platformId;
    out.colorFormat = format;
    return true;
}

}  // namespace DekiEditor
