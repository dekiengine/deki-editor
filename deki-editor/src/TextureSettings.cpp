#include <deki-editor/TextureSettings.h>

#include <deki-editor/build/PlatformConfig.h>

#include <fstream>

namespace DekiEditor
{

TextureSettings ReadImageTextureSettings(const std::string& imagePath)
{
    std::ifstream in(imagePath + ".data");
    if (!in.is_open())
    {
        return {};
    }
    try
    {
        const nlohmann::json data = nlohmann::json::parse(in);
        if (data.is_object() && data.contains("settings") && data["settings"].is_object() &&
            data["settings"].contains("texture"))
        {
            return ReadTextureSettings(data["settings"]["texture"]);
        }
    }
    catch (const nlohmann::json::exception&)
    {
    }
    return {};
}

namespace
{
std::set<std::string>& FilteringCapabilities()
{
    static std::set<std::string> capabilities;
    return capabilities;
}
}  // namespace

void SetTextureFilteringCapabilities(std::set<std::string> capabilities)
{
    FilteringCapabilities() = std::move(capabilities);
}

bool TargetSamplesTextures(const std::string& platformId)
{
    if (platformId.empty())
    {
        return true;
    }
    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(platformId);
    if (!platform)
    {
        return false;
    }
    const std::set<std::string> offered = platform->Capabilities();
    for (const std::string& capability : FilteringCapabilities())
    {
        if (offered.count(capability) != 0)
        {
            return true;
        }
    }
    return false;
}

Deki::TextureFilter ResolveTextureFilter(const TextureSettings& settings, const std::string& platformId)
{
    Deki::TextureFilter filter = Deki::TextureFilter::Point;
    if (!TargetSamplesTextures(platformId) || !Deki::ParseTextureFilter(settings.FilterFor(platformId).c_str(), filter))
    {
        return Deki::TextureFilter::Point;
    }
    return filter;
}

}  // namespace DekiEditor
