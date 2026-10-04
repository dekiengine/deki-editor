#include "BuildOutputParser.h"
#include "../../panels/ConsolePanel.h"

#include <sstream>

namespace DekiEditor
{

BuildLineKind ClassifyBuildLine(const std::string& line)
{
    auto has = [&](const char* needle) { return line.find(needle) != std::string::npos; };

    if (has("ninja explain:"))
    {
        return BuildLineKind::Explain;
    }
    if (has("error") || has("Error") || has("ERROR"))
    {
        return BuildLineKind::Error;
    }
    // The linker's own messages do not say "error"; only its closing
    // "ld returned 1" does, and that names nothing.
    if (has("undefined reference to") || has("multiple definition of") || has("cannot find -l"))
    {
        return BuildLineKind::Error;
    }
    if (has("warning") || has("Warning") || has("WARNING"))
    {
        return BuildLineKind::Warning;
    }
    // MSBuild/ninja progress lines
    if (has("->") || has("Building") || has("Compiling") || has("Microsoft (R)") || has("Copyright (C)"))
    {
        return BuildLineKind::Progress;
    }
    if (line.length() > 2 && (has(".cpp") || has(".h") || has("note:") || has("In file") || has("from ")))
    {
        return BuildLineKind::Note;
    }
    return BuildLineKind::Other;
}

BuildOutputSummary ReportBuildOutput(ConsolePanel& console, const std::string& output, const std::string& tag,
                                     const BuildOutputOptions& options)
{
    BuildOutputSummary summary;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }

        switch (ClassifyBuildLine(line))
        {
            case BuildLineKind::Explain:
                if (options.explain)
                {
                    console.LogEditor(line);
                }
                break;
            case BuildLineKind::Error:
                console.LogBuildError(line, tag);
                ++summary.errors;
                break;
            case BuildLineKind::Warning:
                if (options.warnings)
                {
                    console.LogWarning(line);
                }
                ++summary.warnings;
                break;
            case BuildLineKind::Note:
                if (options.notes)
                {
                    console.LogEditor(line);
                }
                break;
            case BuildLineKind::Progress:
            case BuildLineKind::Other: break;
        }
    }

    if (options.rawWhenNoErrors && summary.errors == 0)
    {
        // Nothing matched the error patterns (say the command could not run
        // at all), so the raw output is the only explanation.
        std::istringstream raw(output);
        int shown = 0;
        while (std::getline(raw, line) && shown < 12)
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.empty())
            {
                continue;
            }
            console.LogBuildError(line, tag);
            ++shown;
            ++summary.errors;
        }
    }
    return summary;
}

}  // namespace DekiEditor
