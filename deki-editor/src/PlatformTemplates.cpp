#include <deki-editor/build/PlatformTemplates.h>

#include <deki-editor/Paths.h>
#include <deki-editor/SafeNames.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki/LogSystem.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace DekiEditor
{

namespace
{

constexpr const char* kOriginFile = "origin.json";

// Atomic: a build prepares, and so reloads, off the UI thread.
std::atomic<unsigned> g_PlatformsRevision{ 0 };

std::string ReadText(const fs::path& file)
{
    std::ifstream f(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// "" for a file that is not there. CR bytes are skipped: a project kept in git
// with autocrlf comes back with other line endings than the package's
// checkout, and that is not an edit.
std::string HashFile(const fs::path& file)
{
    std::ifstream f(file, std::ios::binary);
    if (!f)
        return "";

    uint64_t hash = 1469598103934665603ull;  // FNV-1a
    char buffer[4096];
    while (f)
    {
        f.read(buffer, sizeof(buffer));
        for (std::streamsize i = 0; i < f.gcount(); ++i)
        {
            if (buffer[i] == '\r')
                continue;
            hash ^= static_cast<uint8_t>(buffer[i]);
            hash *= 1099511628211ull;
        }
    }

    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

// Every file of a platform directory but the platform JSON and the origin
// record, as generic relative paths.
std::vector<std::string> PlatformFiles(const fs::path& directory, const std::string& platformId)
{
    std::vector<std::string> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        const std::string relative = fs::relative(it->path(), directory, ec).generic_string();
        if (relative == platformId + ".json" || relative == kOriginFile)
            continue;
        files.push_back(relative);
    }
    std::sort(files.begin(), files.end());
    return files;
}

// A platform directory reduced to what is compared: its JSON in the written
// form, and a hash per other file.
struct Snapshot
{
    json platform;  // null when the JSON could not be read
    std::map<std::string, std::string> files;
};

Snapshot TakeSnapshot(const fs::path& directory, const std::string& platformId,
                      const std::vector<std::string>& files)
{
    Snapshot snapshot;
    PlatformConfig config;
    if (PlatformFromJson(ReadText(directory / (platformId + ".json")), config))
        snapshot.platform = json::parse(PlatformToJson(config), nullptr, false);
    for (const std::string& file : files)
        snapshot.files[file] = HashFile(directory / fs::path(file));
    return snapshot;
}

struct OriginRecord
{
    std::string package;
    std::string version;
    Snapshot adopted;
};

bool ReadOrigin(const fs::path& platformDirectory, OriginRecord& record)
{
    const json j = json::parse(ReadText(platformDirectory / kOriginFile), nullptr, false);
    if (!j.is_object() || !j.contains("package") || !j["package"].is_string() || !j.contains("platform") ||
        !j["platform"].is_object())
        return false;

    record.package = j["package"].get<std::string>();
    record.version = j.value("version", std::string());
    record.adopted.platform = j["platform"];
    if (j.contains("files") && j["files"].is_object())
        for (auto it = j["files"].begin(); it != j["files"].end(); ++it)
            if (it.value().is_string())
                record.adopted.files[it.key()] = it.value().get<std::string>();
    return true;
}

bool WriteOrigin(const fs::path& platformDirectory, const OriginRecord& record)
{
    json j;
    j["package"] = record.package;
    if (!record.version.empty())
        j["version"] = record.version;
    j["platform"] = record.adopted.platform;
    j["files"] = record.adopted.files;

    std::ofstream f(platformDirectory / kOriginFile, std::ios::binary);
    f << j.dump(2);
    return f.good();
}

// The version the project has of a package, from its lockfile; "" when it
// does not say (a junctioned or hand-copied package).
std::string InstalledPackageVersion(const std::string& projectPath, const std::string& packageId)
{
    const json lock = json::parse(ReadText(GetPackageLockFile(projectPath)), nullptr, false);
    if (!lock.is_object() || !lock.contains("packages") || !lock["packages"].is_object())
        return "";
    const json& packages = lock["packages"];
    if (!packages.contains(packageId) || !packages[packageId].is_object())
        return "";
    return packages[packageId].value("installedVersion", std::string());
}

json Member(const json& object, const std::string& key)
{
    return object.is_object() && object.contains(key) ? object[key] : json();
}

std::set<std::string> KeysOf(std::initializer_list<const json*> objects)
{
    std::set<std::string> keys;
    for (const json* object : objects)
        if (object->is_object())
            for (auto it = object->begin(); it != object->end(); ++it)
                keys.insert(it.key());
    return keys;
}

std::vector<std::string> FileNames(const Snapshot& a, const Snapshot& b)
{
    std::set<std::string> names;
    for (const auto& entry : a.files) names.insert(entry.first);
    for (const auto& entry : b.files) names.insert(entry.first);
    return { names.begin(), names.end() };
}

std::string HashIn(const Snapshot& snapshot, const std::string& file)
{
    const auto it = snapshot.files.find(file);
    return it == snapshot.files.end() ? std::string() : it->second;
}

// Names of what differs between two snapshots: settings, a framework's own
// settings by their own names, then files.
std::vector<std::string> Differences(const Snapshot& a, const Snapshot& b)
{
    std::vector<std::string> names;
    for (const std::string& key : KeysOf({ &a.platform, &b.platform }))
    {
        const json left = Member(a.platform, key);
        const json right = Member(b.platform, key);
        if (left == right)
            continue;
        if (key != "frameworkOptions")
        {
            names.push_back(key);
            continue;
        }
        for (const std::string& option : KeysOf({ &left, &right }))
            if (Member(left, option) != Member(right, option))
                names.push_back(option);
    }

    for (const std::string& file : FileNames(a, b))
        if (HashIn(a, file) != HashIn(b, file))
            names.push_back(file);
    return names;
}

// Three-way, setting by setting: what only the package changed is taken, what
// only the project changed is kept, and where both changed one setting the
// project's is kept and named.
json MergeSettings(const json& base, const json& mine, const json& theirs, bool descend,
                   std::vector<std::string>& taken, std::vector<std::string>& kept)
{
    json merged = json::object();
    for (const std::string& key : KeysOf({ &base, &mine, &theirs }))
    {
        const json b = Member(base, key);
        const json m = Member(mine, key);
        const json t = Member(theirs, key);

        if (descend && key == "frameworkOptions")
        {
            json options = MergeSettings(b, m, t, false, taken, kept);
            if (!options.empty())
                merged[key] = std::move(options);
            continue;
        }

        json result = m;
        if (m == b)
        {
            result = t;
            if (t != b)
                taken.push_back(key);
        }
        else if (t != b && t != m)
        {
            kept.push_back(key);
        }

        if (!result.is_null())
            merged[key] = std::move(result);
    }
    return merged;
}

bool CopyFileOver(const fs::path& from, const fs::path& to, std::string& error)
{
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (!ec)
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec)
        error = "could not write " + to.string() + ": " + ec.message();
    return !ec;
}

}  // namespace

std::vector<PlatformTemplate> FindPlatformTemplates(const std::string& projectPath)
{
    std::vector<PlatformTemplate> found;

    std::error_code ec;
    const fs::path packagesDir = GetPackagesDirectory(projectPath);
    if (!fs::is_directory(packagesDir, ec))
        return found;

    for (const auto& package : fs::directory_iterator(packagesDir, ec))
    {
        const fs::path platformsDir = package.path() / "platforms";
        if (!fs::is_directory(platformsDir, ec))
            continue;

        for (const auto& entry : fs::directory_iterator(platformsDir, ec))
        {
            if (!entry.is_directory(ec))
                continue;

            // The directory name becomes a directory name in the project, and
            // a package is downloaded content.
            const std::string id = entry.path().filename().string();
            std::string reason;
            if (!SafeNames::IsSafeName(id, reason))
                continue;
            if (!fs::is_regular_file(entry.path() / (id + ".json"), ec))
                continue;

            found.push_back({ id, package.path().filename().string(), entry.path().string() });
        }
    }

    std::sort(found.begin(), found.end(),
              [](const PlatformTemplate& a, const PlatformTemplate& b)
              { return a.platformId != b.platformId ? a.platformId < b.platformId : a.packageId < b.packageId; });
    return found;
}

namespace
{

// The template for `platformId` from `packageId`, or from the only package
// offering it when no package is named. Two unnamed offers are no offer.
bool FindOffer(const std::string& projectPath, const std::string& platformId, const std::string& packageId,
               PlatformTemplate& offer)
{
    int found = 0;
    for (const PlatformTemplate& t : FindPlatformTemplates(projectPath))
    {
        if (t.platformId != platformId || (!packageId.empty() && t.packageId != packageId))
            continue;
        offer = t;
        ++found;
    }
    return found == 1;
}

// A platform adopted before origins were recorded, and not changed since, is
// provably the package's: say so, and it can be updated like any other.
void RecordOriginIfPristine(const std::string& projectPath, const std::string& platformId)
{
    const fs::path directory = fs::path(GetPlatformsDirectory(projectPath)) / platformId;
    std::error_code ec;
    if (fs::exists(directory / kOriginFile, ec))
        return;

    PlatformTemplate offer;
    if (!FindOffer(projectPath, platformId, "", offer))
        return;

    const std::vector<std::string> files = PlatformFiles(offer.directory, platformId);
    OriginRecord record;
    record.adopted = TakeSnapshot(offer.directory, platformId, files);
    if (record.adopted.platform.is_null() ||
        !Differences(record.adopted, TakeSnapshot(directory, platformId, files)).empty())
        return;

    record.package = offer.packageId;
    record.version = InstalledPackageVersion(projectPath, offer.packageId);
    if (WriteOrigin(directory, record))
        ++g_PlatformsRevision;
}

}  // namespace

AdoptPlatformResult AdoptPlatformTemplate(const std::string& projectPath, const std::string& platformId,
                                          std::string& detail)
{
    std::string reason;
    if (!SafeNames::IsSafeName(platformId, reason))
    {
        detail = "'" + platformId + "' is not a usable platform id: " + reason;
        return AdoptPlatformResult::Failed;
    }

    std::error_code ec;
    const fs::path destination = fs::path(GetPlatformsDirectory(projectPath)) / platformId;
    if (fs::is_regular_file(destination / (platformId + ".json"), ec))
        return AdoptPlatformResult::AlreadyDefined;

    std::vector<PlatformTemplate> offers;
    for (const PlatformTemplate& t : FindPlatformTemplates(projectPath))
        if (t.platformId == platformId)
            offers.push_back(t);

    if (offers.empty())
        return AdoptPlatformResult::NoTemplate;

    if (offers.size() > 1)
    {
        for (const PlatformTemplate& t : offers)
            detail += (detail.empty() ? "" : ", ") + t.packageId;
        return AdoptPlatformResult::Ambiguous;
    }

    // Files the project already has under that directory win: a boot scene
    // somebody wrote before the platform JSON existed is theirs.
    fs::create_directories(destination, ec);
    if (!ec)
        fs::copy(offers.front().directory, destination,
                 fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
    if (ec)
    {
        detail = ec.message();
        return AdoptPlatformResult::Failed;
    }

    // What was adopted, so the copy can later be compared with it. A file the
    // project already had differs from it and will read as modified: it is.
    OriginRecord record;
    record.package = offers.front().packageId;
    record.version = InstalledPackageVersion(projectPath, record.package);
    record.adopted = TakeSnapshot(offers.front().directory, platformId,
                                  PlatformFiles(offers.front().directory, platformId));
    if (!WriteOrigin(destination, record))
        DEKI_LOG_WARNING("Platform '%s': could not record that it came from package '%s'; it cannot be "
                         "updated from the package later, only restored",
                         platformId.c_str(), record.package.c_str());
    ++g_PlatformsRevision;

    detail = offers.front().packageId;
    return AdoptPlatformResult::Adopted;
}

bool EnsureProjectPlatform(const std::string& projectPath, const std::string& platformId, std::string& err)
{
    if (platformId.empty())
        return true;

    std::string detail;
    switch (AdoptPlatformTemplate(projectPath, platformId, detail))
    {
        case AdoptPlatformResult::AlreadyDefined:
            RecordOriginIfPristine(projectPath, platformId);
            return true;

        case AdoptPlatformResult::NoTemplate:
            return true;

        case AdoptPlatformResult::Adopted:
            DEKI_LOG_INFO("Platform '%s' adopted into the project from package '%s'", platformId.c_str(),
                          detail.c_str());
            PlatformRegistry::Instance().LoadPlatformsFromDirectory(GetPlatformsDirectory(projectPath));
            return true;

        case AdoptPlatformResult::Ambiguous:
            err = "Platform '" + platformId + "' is offered by more than one installed package (" + detail +
                  "); copy the one you want into the project's platforms/ directory.";
            return false;

        case AdoptPlatformResult::Failed:
            err = "Could not adopt platform '" + platformId + "': " + detail;
            return false;
    }
    return true;
}

void RegisterPlatformTemplates(const std::string& projectPath)
{
    std::error_code ec;
    const fs::path packagesDir = GetPackagesDirectory(projectPath);
    if (!fs::is_directory(packagesDir, ec))
        return;

    for (const auto& package : fs::directory_iterator(packagesDir, ec))
    {
        const fs::path platformsDir = package.path() / "platforms";
        if (fs::is_directory(platformsDir, ec))
            PlatformRegistry::Instance().LoadPlatformsFromDirectory(platformsDir.string());
    }
}

PlatformOriginStatus GetPlatformOrigin(const std::string& projectPath, const std::string& platformId)
{
    PlatformOriginStatus status;

    std::string reason;
    if (!SafeNames::IsSafeName(platformId, reason))
        return status;

    const fs::path directory = fs::path(GetPlatformsDirectory(projectPath)) / platformId;
    std::error_code ec;
    const bool adopted = fs::is_regular_file(directory / (platformId + ".json"), ec);

    OriginRecord record;
    status.recorded = adopted && ReadOrigin(directory, record);

    PlatformTemplate offer;
    status.packageInstalled = FindOffer(projectPath, platformId, status.recorded ? record.package : "", offer);
    if (!status.recorded && !status.packageInstalled)
        return status;

    status.fromPackage = true;
    status.packageId = status.recorded ? record.package : offer.packageId;
    status.adoptedVersion = record.version;
    if (!adopted)
        return status;  // on offer, not copied in yet: nothing to compare

    Snapshot theirs;
    if (status.packageInstalled)
    {
        status.offeredVersion = InstalledPackageVersion(projectPath, status.packageId);
        theirs = TakeSnapshot(offer.directory, platformId, PlatformFiles(offer.directory, platformId));
    }

    // Against what was adopted when that is known; otherwise against what the
    // package offers, which is all there is to compare with.
    const Snapshot& reference = status.recorded ? record.adopted : theirs;
    std::vector<std::string> files;
    for (const auto& entry : reference.files)
        files.push_back(entry.first);
    status.modifiedItems = Differences(reference, TakeSnapshot(directory, platformId, files));
    if (status.recorded && status.packageInstalled)
        status.updatedItems = Differences(record.adopted, theirs);

    status.modified = !status.modifiedItems.empty();
    status.updateAvailable = !status.updatedItems.empty();
    return status;
}

PlatformSyncResult UpdatePlatformFromPackage(const std::string& projectPath, const std::string& platformId)
{
    PlatformSyncResult result;

    std::string reason;
    if (!SafeNames::IsSafeName(platformId, reason))
    {
        result.error = "'" + platformId + "' is not a usable platform id: " + reason;
        return result;
    }

    const std::string platformsDirectory = GetPlatformsDirectory(projectPath);
    const fs::path directory = fs::path(platformsDirectory) / platformId;

    OriginRecord record;
    PlatformTemplate offer;
    if (!ReadOrigin(directory, record))
    {
        result.error = FindOffer(projectPath, platformId, "", offer)
                           ? "Platform '" + platformId + "' has no record of what it was adopted from, so a "
                             "package's change cannot be told from the project's own. Restore it from the "
                             "package instead."
                           : "Platform '" + platformId + "' did not come from a package.";
        return result;
    }

    if (!FindOffer(projectPath, platformId, record.package, offer))
    {
        result.error = "Platform '" + platformId + "' came from package '" + record.package +
                       "', which is not installed or no longer offers it.";
        return result;
    }

    const Snapshot theirs = TakeSnapshot(offer.directory, platformId, PlatformFiles(offer.directory, platformId));
    const Snapshot mine = TakeSnapshot(directory, platformId, FileNames(record.adopted, theirs));
    if (theirs.platform.is_null() || mine.platform.is_null())
    {
        result.error = "Platform '" + platformId + "': " +
                       (theirs.platform.is_null() ? "the package's" : "the project's") +
                       " platform JSON could not be read.";
        return result;
    }

    const json merged =
        MergeSettings(record.adopted.platform, mine.platform, theirs.platform, true, result.taken, result.kept);
    PlatformConfig config;
    if (!PlatformFromJson(merged.dump(), config) ||
        !PlatformRegistry::Instance().SavePlatformToFile(config, platformsDirectory))
    {
        result.error = "Platform '" + platformId + "': could not write the updated platform JSON.";
        return result;
    }

    for (const auto& [file, theirHash] : theirs.files)
    {
        const std::string baseHash = HashIn(record.adopted, file);
        const std::string myHash = HashIn(mine, file);
        if (theirHash == baseHash || theirHash == myHash)
            continue;
        if (myHash != baseHash)
        {
            result.kept.push_back(file);
            continue;
        }
        if (!CopyFileOver(fs::path(offer.directory) / fs::path(file), directory / fs::path(file), result.error))
            return result;
        result.taken.push_back(file);
    }

    record.version = InstalledPackageVersion(projectPath, record.package);
    record.adopted = theirs;
    if (!WriteOrigin(directory, record))
    {
        result.error = "Platform '" + platformId + "': updated, but the origin record could not be written.";
        return result;
    }

    ++g_PlatformsRevision;
    result.ok = true;
    return result;
}

PlatformSyncResult RestorePlatformFromPackage(const std::string& projectPath, const std::string& platformId)
{
    PlatformSyncResult result;

    std::string reason;
    if (!SafeNames::IsSafeName(platformId, reason))
    {
        result.error = "'" + platformId + "' is not a usable platform id: " + reason;
        return result;
    }

    const std::string platformsDirectory = GetPlatformsDirectory(projectPath);
    const fs::path directory = fs::path(platformsDirectory) / platformId;

    OriginRecord record;
    const bool recorded = ReadOrigin(directory, record);

    PlatformTemplate offer;
    if (!FindOffer(projectPath, platformId, recorded ? record.package : "", offer))
    {
        result.error = recorded ? "Platform '" + platformId + "' came from package '" + record.package +
                                      "', which is not installed or no longer offers it."
                                : "No single installed package offers platform '" + platformId + "'.";
        return result;
    }

    const std::vector<std::string> files = PlatformFiles(offer.directory, platformId);
    const Snapshot theirs = TakeSnapshot(offer.directory, platformId, files);
    if (theirs.platform.is_null())
    {
        result.error = "Platform '" + platformId + "': the package's platform JSON could not be read.";
        return result;
    }
    result.taken = Differences(theirs, TakeSnapshot(directory, platformId, files));

    // The package's files over the project's. What only the project has (an
    // asset sidecar, a scene of its own) is not the package's to remove.
    std::vector<std::string> toCopy = files;
    toCopy.push_back(platformId + ".json");
    for (const std::string& file : toCopy)
        if (!CopyFileOver(fs::path(offer.directory) / fs::path(file), directory / fs::path(file), result.error))
            return result;

    record.package = offer.packageId;
    record.version = InstalledPackageVersion(projectPath, offer.packageId);
    record.adopted = theirs;
    if (!WriteOrigin(directory, record))
    {
        result.error = "Platform '" + platformId + "': restored, but the origin record could not be written.";
        return result;
    }

    PlatformRegistry::Instance().LoadPlatformsFromDirectory(platformsDirectory);
    ++g_PlatformsRevision;
    result.ok = true;
    return result;
}

unsigned ProjectPlatformsRevision()
{
    return g_PlatformsRevision;
}

void NoteProjectPlatformsChanged()
{
    ++g_PlatformsRevision;
}

void ReloadProjectPlatforms(const std::string& projectPath)
{
    ++g_PlatformsRevision;
    auto& platforms = PlatformRegistry::Instance();
    platforms.Clear();
    platforms.LoadPlatformsFromBoardPacks();
    RegisterPlatformTemplates(projectPath);
    platforms.LoadPlatformsFromDirectory(GetPlatformsDirectory(projectPath));
}

}  // namespace DekiEditor
