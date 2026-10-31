#pragma once

// MemoryReport: where a running game's memory is, measured. The engine counts
// every allocation it places by call site (Deki::Memory::GetSite); a report is
// those rows plus each region's totals, read from this process (Play mode) or
// from a board's answer to "DEKI:MEMORY" (deki-esp32-integration's
// ESP32ScreenCapture.cpp writes it).
//
// The rows name code, not things. Category() groups them into what a person
// asks about (the frame buffer, images, fonts) from the file each one is in.

#include <cstdint>
#include <string>
#include <vector>

namespace DekiEditor
{

/// One call site's memory, as the engine counted it.
struct MemoryReportSite
{
    std::string function;  ///< empty for the overflow row
    std::string file;      ///< the last two parts of the path, such as "deki-2d/BitmapFont.cpp"
    uint32_t line = 0;
    uint64_t internalBytes = 0;
    uint64_t externalBytes = 0;
    uint64_t peakBytes = 0;
    uint32_t blocks = 0;

    uint64_t Bytes() const { return internalBytes + externalBytes; }
};

/// Every measured figure there is. -1 is "not reported": a desktop heap has no
/// size to report, and a board without external RAM has no external figures.
struct MemoryReport
{
    uint64_t internalUsed = 0;  ///< what the engine placed outside the External region
    uint64_t externalUsed = 0;
    uint64_t internalPeak = 0;  ///< the most held at once since boot or Play
    uint64_t externalPeak = 0;
    int64_t internalFree = -1;  ///< the heap's own figures, including memory the engine did not place
    int64_t externalFree = -1;
    int64_t internalMinFree = -1;  ///< the least there has been free since boot
    int64_t externalMinFree = -1;
    int64_t internalTotal = -1;
    int64_t externalTotal = -1;
    std::vector<MemoryReportSite> sites;
};

/// Reads a board's answer to "DEKI:MEMORY" (log lines around it are skipped).
/// False, with why, when there is no complete answer.
bool ParseMemoryReport(const std::string& received, MemoryReport& report, std::string& error);

/// This process's memory, from Deki::Memory: Play mode in the editor.
MemoryReport CaptureLocalMemory();

/// The last two parts of a path: "C:/x/packages/deki-2d/Sprite.cpp" gives
/// "deki-2d/Sprite.cpp".
std::string ShortSourcePath(const std::string& path);

/// What a call site holds, in a person's words ("Fonts and text"), in English;
/// translate it where it is drawn. From the file and function the site is in.
const char* MemoryCategory(const MemoryReportSite& site);

/// The categories MemoryCategory gives, in the order a report lists them.
const std::vector<const char*>& MemoryCategories();

/// Remembers this process's memory as Play starts and starts the peaks again,
/// so Play mode's report can show what the game added. PlayMode calls it.
void MarkPlayStart();

/// What this process held when Play last started, or null before the first.
const MemoryReport* PlayStartMemory();

/// `report` with what was held when Play started taken off each site, and the
/// rows that are left empty dropped. A site freed below its start shows 0.
MemoryReport SincePlayStart(const MemoryReport& report, const MemoryReport& start);

/// "12.3 KB", "1.20 MB", "512 B".
std::string FormatMemoryBytes(uint64_t bytes);

}  // namespace DekiEditor
