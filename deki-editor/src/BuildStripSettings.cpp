#include <deki-editor/BuildStripSettings.h>

#include <deki-editor/build/ProjectPaths.h>
#include <deki/LogSystem.h>

#include <nlohmann/json.hpp>

#include <fstream>

namespace fs = std::filesystem;

namespace DekiEditor
{

std::string BuildStripSettings::Override(const std::string& packageId, const std::string& featureId) const
{
    auto p = featureOverrides.find(packageId);
    if (p == featureOverrides.end())
        return "";
    auto f = p->second.find(featureId);
    return f == p->second.end() ? "" : f->second;
}

void BuildStripSettings::SetOverride(const std::string& packageId, const std::string& featureId,
                                     const std::string& value)
{
    if (value.empty())
    {
        auto p = featureOverrides.find(packageId);
        if (p != featureOverrides.end())
        {
            p->second.erase(featureId);
            if (p->second.empty())
                featureOverrides.erase(p);
        }
        return;
    }
    featureOverrides[packageId][featureId] = value;
}

fs::path BuildStripSettings::FilePath(const std::string& projectPath)
{
    return ProjectPaths::Settings(projectPath) / "build.json";
}

BuildStripSettings BuildStripSettings::Load(const std::string& projectPath)
{
    BuildStripSettings s;
    if (projectPath.empty())
        return s;
    std::ifstream in(FilePath(projectPath));
    if (!in)
        return s;
    try
    {
        nlohmann::json j;
        in >> j;
        if (j.contains("stripping") && j["stripping"].is_object())
        {
            const auto& st = j["stripping"];
            s.stripUnusedComponents = st.value("unusedComponents", s.stripUnusedComponents);
            s.stripUnusedPackages = st.value("unusedPackages", s.stripUnusedPackages);
        }
        if (j.contains("features") && j["features"].is_object())
        {
            for (const auto& pkg : j["features"].items())
            {
                if (!pkg.value().is_object())
                    continue;
                for (const auto& feat : pkg.value().items())
                {
                    if (!feat.value().is_string())
                        continue;
                    const std::string v = feat.value().get<std::string>();
                    if (v == "on" || v == "off")
                        s.featureOverrides[pkg.key()][feat.key()] = v;
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        DEKI_LOG_WARNING("settings/build.json: could not be read (%s); using defaults", e.what());
        return BuildStripSettings{};
    }
    return s;
}

bool BuildStripSettings::Save(const std::string& projectPath) const
{
    if (projectPath.empty())
        return false;
    nlohmann::json j;
    j["stripping"]["unusedComponents"] = stripUnusedComponents;
    j["stripping"]["unusedPackages"] = stripUnusedPackages;
    j["features"] = nlohmann::json::object();
    for (const auto& pkg : featureOverrides)
        for (const auto& feat : pkg.second)
            j["features"][pkg.first][feat.first] = feat.second;

    const fs::path path = FilePath(projectPath);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out)
    {
        DEKI_LOG_ERROR("settings/build.json: could not write %s", path.string().c_str());
        return false;
    }
    out << j.dump(2) << "\n";
    return true;
}

}  // namespace DekiEditor
