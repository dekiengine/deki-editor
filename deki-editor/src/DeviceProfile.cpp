#include <deki-editor/DeviceProfile.h>
#include <deki-editor/SerialPorts.h>

#include <chrono>
#include <cstdlib>
#include <sstream>
#include <thread>

namespace DekiEditor
{

namespace
{
// "name=value" pairs from `rest`, numbers only.
void ReadValues(const std::string& rest, DeviceProfileRow& row)
{
    std::istringstream in(rest);
    std::string token;
    while (in >> token)
    {
        const size_t eq = token.find('=');
        if (eq == std::string::npos || eq == 0)
        {
            continue;
        }
        const std::string key = token.substr(0, eq);
        const std::string value = token.substr(eq + 1);
        if (key == "type")
        {
            row.type = value;
            continue;
        }
        char* end = nullptr;
        const double number = std::strtod(value.c_str(), &end);
        if (end && end != value.c_str())
        {
            row.values[key] = number;
        }
    }
}

// A line's name: everything before its first "name=value" (a kind's name may
// have spaces: "Render passes").
DeviceProfileRow ReadNamedRow(const std::string& rest)
{
    DeviceProfileRow row;
    size_t valuesAt = rest.size();
    for (size_t i = 0; i < rest.size();)
    {
        const size_t space = rest.find(' ', i);
        const std::string word = rest.substr(i, space == std::string::npos ? std::string::npos : space - i);
        if (word.find('=') != std::string::npos)
        {
            valuesAt = i;
            break;
        }
        if (space == std::string::npos)
        {
            break;
        }
        i = space + 1;
    }
    row.name = rest.substr(0, valuesAt);
    while (!row.name.empty() && row.name.back() == ' ')
    {
        row.name.pop_back();
    }
    ReadValues(rest.substr(valuesAt), row);
    return row;
}
}  // namespace

bool ParseDeviceProfile(const std::string& received, DeviceProfile& profile, std::string& error)
{
    profile = DeviceProfile{};
    const size_t start = received.rfind("DEKI:PROFILE frames=");
    if (start == std::string::npos)
    {
        error = "no profile report (a board that has just started answers its first request with DEKI:PROFILE:ON)";
        return false;
    }
    if (received.find("DEKI:PROFILE:END", start) == std::string::npos)
    {
        error = "the profile report was cut short";
        return false;
    }
    std::istringstream lines(received.substr(start));
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        auto rest = [&](const char* prefix) -> const char*
        {
            const size_t n = std::char_traits<char>::length(prefix);
            return line.compare(0, n, prefix) == 0 ? line.c_str() + n : nullptr;
        };
        if (const char* r = rest("DEKI:PROFILE:ITEM "))
        {
            profile.kinds.push_back(ReadNamedRow(r));
        }
        else if (const char* r2 = rest("DEKI:PROFILE:PATH "))
        {
            profile.paths.push_back(ReadNamedRow(r2));
        }
        else if (const char* r3 = rest("DEKI:PROFILE:OBJECT "))
        {
            // The path is quoted: it may hold spaces.
            const std::string s = r3;
            DeviceProfileRow row;
            size_t after = 0;
            if (!s.empty() && s[0] == '"')
            {
                const size_t close = s.find('"', 1);
                row.name = s.substr(1, close == std::string::npos ? std::string::npos : close - 1);
                after = close == std::string::npos ? s.size() : close + 1;
            }
            ReadValues(s.substr(after), row);
            profile.objects.push_back(std::move(row));
        }
        else if (const char* r4 = rest("DEKI:PROFILE:OBJECTS "))
        {
            DeviceProfileRow counts;
            ReadValues(r4, counts);
            profile.objectsTotal = static_cast<int>(counts.values["of"]);
        }
        else if (const char* r5 = rest("DEKI:PROFILE "))
        {
            DeviceProfileRow head;
            ReadValues(r5, head);
            profile.frames = head.values["frames"];
        }
        else if (line.compare(0, 16, "DEKI:PROFILE:END") == 0)
        {
            break;
        }
    }
    if (profile.objectsTotal < static_cast<int>(profile.objects.size()))
    {
        profile.objectsTotal = static_cast<int>(profile.objects.size());
    }
    return true;
}

bool QueryDeviceProfile(const std::string& port, int seconds, DeviceProfile& profile, std::string& error)
{
    // The first request starts measuring (or ends a measure already running,
    // and starts again); the second, after `seconds`, reports.
    std::string received;
    auto started = [](const std::string& text)
    {
        return text.find("DEKI:PROFILE:ON") != std::string::npos ||
               text.find("DEKI:PROFILE:END") != std::string::npos;
    };
    if (!SerialExchange(port, "\nDEKI:PROFILE\n", started, 5000, received, error))
    {
        return false;
    }
    if (!started(received))
    {
        error = "the board on " + port + " did not answer DEKI:PROFILE (its firmware may be older than the profiler)";
        return false;
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds < 1 ? 1 : seconds));
    received.clear();
    auto finished = [](const std::string& text)
    {
        const size_t at = text.rfind("DEKI:PROFILE:END");
        return at != std::string::npos && text.find('\n', at) != std::string::npos;
    };
    if (!SerialExchange(port, "\nDEKI:PROFILE\n", finished, 10000, received, error))
    {
        return false;
    }
    return ParseDeviceProfile(received, profile, error);
}

}  // namespace DekiEditor
