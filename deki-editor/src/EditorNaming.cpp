#include <deki-editor/EditorNaming.h>

#include <deki/reflection/Property.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace DekiEditor
{

namespace EditorNaming
{

namespace
{

// Words the camelCase split gets wrong, written as people write them: bus and
// pin names, chips and units. Matched as whole words of the split name.
const std::unordered_map<std::string, const char*>& AcronymWords()
{
    static const std::unordered_map<std::string, const char*> kWords = {
        { "Spi", "SPI" },     { "Qspi", "QSPI" },   { "I2c", "I2C" },   { "I2s", "I2S" },   { "Uart", "UART" },
        { "Gpio", "GPIO" },   { "Mosi", "MOSI" },   { "Miso", "MISO" }, { "Sclk", "SCLK" }, { "Sck", "SCK" },
        { "Sda", "SDA" },     { "Scl", "SCL" },     { "Cs", "CS" },     { "Dc", "DC" },     { "Rst", "RST" },
        { "Te", "TE" },       { "Bl", "BL" },       { "Tx", "TX" },     { "Rx", "RX" },     { "Rf", "RF" },
        { "Tcxo", "TCXO" },   { "Dbm", "dBm" },     { "Rgb", "RGB" },   { "Rgba", "RGBA" }, { "Psram", "PSRAM" },
        { "Gps", "GPS" },     { "Ble", "BLE" },     { "Imu", "IMU" },   { "Rtc", "RTC" },   { "Lcd", "LCD" },
        { "Http", "HTTP" },   { "Url", "URL" },     { "Usb", "USB" },   { "Sd", "SD" },     { "Fps", "FPS" },
        { "Ui", "UI" },       { "Fsm", "FSM" },     { "Ttf", "TTF" },   { "Bdf", "BDF" },   { "Pwm", "PWM" },
    };
    return kWords;
}

// Two words of the split that are one: "I 2c" (i2cPort), "Lo Ra" (loRa),
// "Ki B" (chunkCacheKiB); and a pin family and its number, "Dio 1" -> "DIO1".
std::string JoinAcronyms(const std::string& split)
{
    std::vector<std::string> words;
    size_t start = 0;
    while (start <= split.size())
    {
        const size_t space = split.find(' ', start);
        const size_t end = space == std::string::npos ? split.size() : space;
        words.push_back(split.substr(start, end - start));
        start = end + 1;
    }

    auto isNumber = [](const std::string& w)
    { return !w.empty() && std::all_of(w.begin(), w.end(), [](unsigned char c) { return std::isdigit(c); }); };

    std::string out;
    for (size_t i = 0; i < words.size(); ++i)
    {
        std::string word = words[i];
        const std::string next = i + 1 < words.size() ? words[i + 1] : std::string();
        if ((word == "I" && (next == "2c" || next == "2s")) || (word == "Lo" && next == "Ra") ||
            ((word == "Ki" || word == "Mi") && next == "B"))
        {
            word = word == "I" ? "I2" + std::string(1, static_cast<char>(std::toupper(next[1]))) : word + next;
            ++i;
        }
        else if ((word == "Dio" || word == "Io" || word == "Gpio") && isNumber(next))
        {
            std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return std::toupper(c); });
            word += next;
            ++i;
        }
        else
        {
            auto it = AcronymWords().find(word);
            if (it != AcronymWords().end())
            {
                word = it->second;
            }
        }
        if (!out.empty())
        {
            out += ' ';
        }
        out += word;
    }
    return out;
}

}  // namespace

std::string NicifyName(const char* name)
{
    if (!name || !name[0])
    {
        return "";
    }

    std::string result;
    bool capitalizeNext = true;

    for (size_t i = 0; name[i]; ++i)
    {
        char c = name[i];

        // Separators (underscores, and the dashes and spaces of file names)
        // become one space, and the next letter is capitalized.
        if (c == '_' || c == '-' || c == ' ')
        {
            if (!result.empty() && result.back() != ' ')
            {
                result += ' ';
            }
            capitalizeNext = true;
            continue;
        }

        // A space before an uppercase letter that follows a lowercase one, or
        // that starts a word after a digit ("stop1Position" -> "Stop 1
        // Position", but "texture2D" -> "Texture 2D").
        if (std::isupper(c) && i > 0 &&
            (std::islower(name[i - 1]) || (std::isdigit(name[i - 1]) && std::islower(name[i + 1]))))
        {
            result += ' ';
        }
        // A space before a digit that follows a letter.
        else if (std::isdigit(c) && i > 0 && std::isalpha(name[i - 1]))
        {
            result += ' ';
        }

        if (capitalizeNext)
        {
            result += static_cast<char>(std::toupper(c));
            capitalizeNext = false;
        }
        else
        {
            result += c;
        }
    }

    return JoinAcronyms(result);
}

namespace
{
struct CachedName
{
    const char* name;         // the table entry's name pointer at caching time
    const char* displayName;  // and its displayName pointer, to detect a reused address
    std::string text;
};
std::unordered_map<const Deki::PropertyInfo*, CachedName> s_DisplayNames;
}  // namespace

const std::string& GetDisplayName(const Deki::PropertyInfo& prop)
{
    auto it = s_DisplayNames.find(&prop);
    if (it != s_DisplayNames.end() && it->second.name == prop.name && it->second.displayName == prop.displayName)
    {
        return it->second.text;
    }

    CachedName entry{ prop.name, prop.displayName,
                      prop.displayName ? std::string(prop.displayName) : NicifyName(prop.name) };
    return (s_DisplayNames[&prop] = std::move(entry)).text;
}

void ClearDisplayNameCache()
{
    s_DisplayNames.clear();
}

}  // namespace EditorNaming

}  // namespace DekiEditor
