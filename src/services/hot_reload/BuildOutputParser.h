#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace DekiEditor
{

class ConsolePanel;

enum class BuildLineKind
{
    Error,     // compiler/linker error line
    Warning,   // warning line
    Explain,   // "ninja explain:" (why a target rebuilt)
    Progress,  // build-tool progress noise
    Note,      // file paths, notes, "In file included from" context
    Other,
};

// Classifies one line of cmake/ninja/compiler output (MSVC and GCC patterns).
BuildLineKind ClassifyBuildLine(const std::string& line);

struct BuildOutputOptions
{
    bool warnings = false;         // log warning lines
    bool notes = false;            // log note/context lines
    bool explain = false;          // surface "ninja explain:" lines
    bool rawWhenNoErrors = false;  // a failed build with no error line: show its first lines verbatim
};

struct BuildOutputSummary
{
    int errors = 0;
    int warnings = 0;
};

// Logs `output` to the Console: errors pinned under `tag` (a package target or
// a pseudo-package such as "__plugin__"), warnings and notes as configured.
// Every build report goes through here, so the classification and the "show
// the raw text when nothing matched" fallback exist once.
BuildOutputSummary ReportBuildOutput(ConsolePanel& console, const std::string& output, const std::string& tag,
                                     const BuildOutputOptions& options);

// The error lines of `output`, at most `maxLines`, as the Console shows them:
// the lines ClassifyBuildLine calls errors, or when there is none (a command
// that could not run at all) its first lines as they are, as those are then
// the only explanation.
std::vector<std::string> BuildErrorLines(const std::string& output, size_t maxLines);

}  // namespace DekiEditor
