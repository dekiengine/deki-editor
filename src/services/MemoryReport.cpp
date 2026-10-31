#include "MemoryReport.h"

#include <deki-editor/Localization.h>
#include <deki/providers/Memory.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <utility>

namespace DekiEditor
{

namespace
{

// The "name=value" fields on one line. A site's function has spaces in it, so
// the caller cuts it off ("fn=" comes last) before this splits the rest.
std::map<std::string, std::string> Fields(const std::string& line)
{
    std::map<std::string, std::string> out;
    std::istringstream in(line);
    std::string field;
    while (in >> field)
    {
        const size_t eq = field.find('=');
        if (eq != std::string::npos)
        {
            out[field.substr(0, eq)] = field.substr(eq + 1);
        }
    }
    return out;
}

uint64_t Unsigned(const std::map<std::string, std::string>& fields, const char* name)
{
    auto it = fields.find(name);
    return it == fields.end() ? 0 : std::strtoull(it->second.c_str(), nullptr, 10);
}

// -1 for a figure the board did not send. A heap figure of 0 for external RAM
// means the board has none: reported as not there rather than as full.
int64_t Signed(const std::map<std::string, std::string>& fields, const char* name, bool zeroIsAbsent)
{
    auto it = fields.find(name);
    if (it == fields.end())
    {
        return -1;
    }
    const int64_t value = std::strtoll(it->second.c_str(), nullptr, 10);
    return (zeroIsAbsent && value == 0) ? -1 : value;
}

std::string LineAt(const std::string& text, size_t at)
{
    const size_t end = text.find_first_of("\r\n", at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

bool Contains(const std::string& text, const char* part)
{
    return text.find(part) != std::string::npos;
}

std::unique_ptr<MemoryReport> s_PlayStart;

}  // namespace

bool ParseMemoryReport(const std::string& received, MemoryReport& report, std::string& error)
{
    static const std::string kHead = "DEKI:MEMORY ";
    static const std::string kSite = "DEKI:MEMORY:SITE ";
    static const std::string kEnd = "DEKI:MEMORY:END";

    const size_t head = received.rfind(kHead);
    if (head == std::string::npos)
    {
        error = "the board did not answer. Its firmware may be older than the memory report (flash it again), or it "
                "is not running";
        return false;
    }
    const size_t end = received.find(kEnd, head);
    if (end == std::string::npos)
    {
        error = "the board's memory report was cut short";
        return false;
    }

    report = MemoryReport{};
    const auto totals = Fields(LineAt(received, head + kHead.size()));
    report.internalUsed = Unsigned(totals, "internal");
    report.externalUsed = Unsigned(totals, "external");
    report.internalPeak = Unsigned(totals, "internalPeak");
    report.externalPeak = Unsigned(totals, "externalPeak");
    report.internalFree = Signed(totals, "internalFree", false);
    report.internalMinFree = Signed(totals, "internalMinFree", false);
    report.internalTotal = Signed(totals, "internalTotal", false);
    report.externalTotal = Signed(totals, "externalTotal", true);
    if (report.externalTotal >= 0)
    {
        report.externalFree = Signed(totals, "externalFree", false);
        report.externalMinFree = Signed(totals, "externalMinFree", false);
    }

    for (size_t at = received.find(kSite, head); at != std::string::npos && at < end;
         at = received.find(kSite, at + kSite.size()))
    {
        std::string line = LineAt(received, at + kSite.size());
        MemoryReportSite site;
        const size_t fn = line.find(" fn=");
        if (fn != std::string::npos)
        {
            site.function = line.substr(fn + 4);
            line.resize(fn);
        }
        if (site.function == "-")
        {
            site.function.clear();
        }
        const auto fields = Fields(line);
        site.internalBytes = Unsigned(fields, "internal");
        site.externalBytes = Unsigned(fields, "external");
        site.peakBytes = Unsigned(fields, "peak");
        site.blocks = static_cast<uint32_t>(Unsigned(fields, "blocks"));
        auto where = fields.find("at");
        if (where != fields.end() && where->second != "-:0")
        {
            const size_t colon = where->second.rfind(':');
            site.file = where->second.substr(0, colon);
            site.line = colon == std::string::npos
                            ? 0
                            : static_cast<uint32_t>(std::strtoul(where->second.c_str() + colon + 1, nullptr, 10));
        }
        report.sites.push_back(std::move(site));
    }
    return true;
}

MemoryReport CaptureLocalMemory()
{
    MemoryReport report;
    const Deki::MemoryStats stats = Deki::Memory::GetStats();
    report.internalUsed = stats.internalRamAllocated;
    report.externalUsed = stats.externalAllocated;
    report.internalPeak = Deki::Memory::GetPeakInternal();
    report.externalPeak = Deki::Memory::GetPeakExternal();
    // A desktop heap reports 0 for "not measured": the OS pages.
    if (const size_t free = Deki::Memory::GetAvailableInternal(); free > 0)
    {
        report.internalFree = static_cast<int64_t>(free);
    }
    if (Deki::Memory::IsExternalAvailable())
    {
        if (const size_t free = Deki::Memory::GetAvailableExternal(); free > 0)
        {
            report.externalFree = static_cast<int64_t>(free);
        }
    }
    const uint32_t count = Deki::Memory::GetSiteCount();
    for (uint32_t i = 0; i < count; ++i)
    {
        Deki::MemorySite raw{};
        if (!Deki::Memory::GetSite(i, raw))
        {
            break;
        }
        MemoryReportSite site;
        site.function = raw.function ? raw.function : "";
        site.file = raw.file ? ShortSourcePath(raw.file) : "";
        site.line = raw.line;
        site.internalBytes = raw.internalBytes;
        site.externalBytes = raw.externalBytes;
        site.peakBytes = raw.peakBytes;
        site.blocks = raw.blocks;
        report.sites.push_back(std::move(site));
    }
    return report;
}

std::string ShortSourcePath(const std::string& path)
{
    const size_t last = path.find_last_of("/\\");
    if (last == std::string::npos || last == 0)
    {
        return path;
    }
    const size_t before = path.find_last_of("/\\", last - 1);
    return before == std::string::npos ? path : path.substr(before + 1);
}

const std::vector<const char*>& MemoryCategories()
{
    static const std::vector<const char*> s_Categories = {
        TrMark("Frame buffer and strips"),
        TrMark("Display driver"),
        TrMark("Images and sprites"),
        TrMark("Fonts and text"),
        TrMark("Tilemaps"),
        TrMark("Particles"),
        TrMark("3D meshes"),
        TrMark("Asset files"),
        TrMark("Project settings"),
        TrMark("Other"),
        TrMark("More call sites than the table holds"),
    };
    return s_Categories;
}

const char* MemoryCategory(const MemoryReportSite& site)
{
    const auto& c = MemoryCategories();
    if (site.file.empty())
    {
        return c[10];  // the overflow row: sites past the 63 the table holds
    }
    // By file, which every build captures, so the grouping is the same on a
    // board and in the editor. The first match wins, so the specific names
    // come before the general ones.
    const std::string& f = site.file;
    if (Contains(f, "DekiRenderSystem"))
    {
        return c[0];
    }
    if (Contains(f, "Display") || Contains(f, "PanelIO") || Contains(f, "LovyanGFX"))
    {
        return c[1];
    }
    if (Contains(f, "Font") || Contains(f, "TextComponent"))
    {
        return c[3];
    }
    if (Contains(f, "Tilemap") || Contains(f, "Tileset"))
    {
        return c[4];
    }
    if (Contains(f, "Particle"))
    {
        return c[5];
    }
    if (Contains(f, "Mesh") || Contains(f, "deki-3d/"))
    {
        return c[6];
    }
    if (Contains(f, "Sprite") || Contains(f, "Texture") || Contains(f, "QuadBlit") || Contains(f, "Gradient"))
    {
        return c[2];
    }
    if (Contains(f, "AssetManager") || Contains(f, "AssetPack") || Contains(f, "FileImage"))
    {
        return c[7];
    }
    if (Contains(f, "ProjectSettings"))
    {
        return c[8];
    }
    return c[9];
}

void MarkPlayStart()
{
    Deki::Memory::ResetPeaks();
    s_PlayStart = std::make_unique<MemoryReport>(CaptureLocalMemory());
}

const MemoryReport* PlayStartMemory()
{
    return s_PlayStart.get();
}

MemoryReport SincePlayStart(const MemoryReport& report, const MemoryReport& start)
{
    std::map<std::pair<std::string, uint32_t>, const MemoryReportSite*> before;
    for (const MemoryReportSite& s : start.sites)
    {
        before[{ s.function, s.line }] = &s;
    }
    auto minus = [](uint64_t a, uint64_t b) { return a > b ? a - b : 0; };

    MemoryReport out = report;
    out.internalUsed = minus(report.internalUsed, start.internalUsed);
    out.externalUsed = minus(report.externalUsed, start.externalUsed);
    out.internalPeak = minus(report.internalPeak, start.internalUsed);
    out.externalPeak = minus(report.externalPeak, start.externalUsed);
    out.sites.clear();
    for (const MemoryReportSite& s : report.sites)
    {
        MemoryReportSite d = s;
        auto it = before.find({ s.function, s.line });
        if (it != before.end())
        {
            d.internalBytes = minus(s.internalBytes, it->second->internalBytes);
            d.externalBytes = minus(s.externalBytes, it->second->externalBytes);
            d.peakBytes = minus(s.peakBytes, it->second->Bytes());
            d.blocks = s.blocks > it->second->blocks ? s.blocks - it->second->blocks : 0;
        }
        if (d.Bytes() > 0 || d.peakBytes > 0)
        {
            out.sites.push_back(std::move(d));
        }
    }
    return out;
}

std::string FormatMemoryBytes(uint64_t bytes)
{
    char buf[32];
    if (bytes >= 1024ull * 1024ull)
    {
        std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    else if (bytes >= 1024)
    {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "%u B", static_cast<unsigned>(bytes));
    }
    return buf;
}

}  // namespace DekiEditor
