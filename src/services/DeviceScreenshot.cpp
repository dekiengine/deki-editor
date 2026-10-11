#include "DeviceScreenshot.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace DekiEditor
{

namespace
{

constexpr const char* kBegin = "DEKI:SCREENSHOT:BEGIN";
constexpr const char* kEnd = "DEKI:SCREENSHOT:END";
constexpr const char* kError = "DEKI:SCREENSHOT:ERROR";

uint32_t Crc32(const uint8_t* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

int Base64Value(char c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z')
    {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9')
    {
        return c - '0' + 52;
    }
    if (c == '+')
    {
        return 62;
    }
    if (c == '/')
    {
        return 63;
    }
    return -1;
}

// One line of base64 appended to `out`; false on a character that is not one.
bool DecodeBase64Line(const std::string& line, std::vector<uint8_t>& out)
{
    uint32_t bits = 0;
    int count = 0;
    for (char c : line)
    {
        if (c == '=')
        {
            break;
        }
        const int v = Base64Value(c);
        if (v < 0)
        {
            return false;
        }
        bits = (bits << 6) | static_cast<uint32_t>(v);
        count += 6;
        if (count >= 8)
        {
            count -= 8;
            out.push_back(static_cast<uint8_t>((bits >> count) & 0xFF));
        }
    }
    return true;
}

size_t BytesPerPixel(const std::string& format)
{
    if (format == "RGB565")
    {
        return 2;
    }
    if (format == "RGB888" || format == "RGB565A8")
    {
        return 3;
    }
    if (format == "ARGB8888")
    {
        return 4;
    }
    return 0;
}

}  // namespace

bool ParseDeviceScreenshot(const std::string& received, DeviceFrame& frame, std::string& error)
{
    // The last answer: a board that answered twice (a request sent twice)
    // sends two, and only the last can be complete.
    const size_t begin = received.rfind(kBegin);
    if (begin == std::string::npos)
    {
        const size_t failed = received.rfind(kError);
        if (failed != std::string::npos)
        {
            const size_t start = failed + std::strlen(kError);
            const size_t end = received.find_first_of("\r\n", start);
            std::string why = received.substr(start, end == std::string::npos ? std::string::npos : end - start);
            while (!why.empty() && why.front() == ' ')
            {
                why.erase(why.begin());
            }
            error = "the board could not send its frame: " + why;
            return false;
        }
        error = "the board did not answer. Its firmware may be older than the screen capture (flash it again), "
                "or it is not running";
        return false;
    }

    std::istringstream lines(received.substr(begin));
    std::string line;
    std::getline(lines, line);
    {
        std::istringstream header(line.substr(std::strlen(kBegin)));
        size_t bytes = 0;
        if (!(header >> frame.width >> frame.height >> frame.format >> bytes) || frame.width <= 0 ||
            frame.height <= 0)
        {
            error = "the board's answer starts with a header that cannot be read: " + line;
            return false;
        }
        const size_t pixelBytes = BytesPerPixel(frame.format);
        if (pixelBytes == 0)
        {
            error = "the board's framebuffer is " + frame.format + ", a format this cannot read";
            return false;
        }
        if (bytes != pixelBytes * static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height))
        {
            error = "the board's answer gives " + std::to_string(bytes) + " bytes for a " +
                    std::to_string(frame.width) + "x" + std::to_string(frame.height) + " " + frame.format + " frame";
            return false;
        }

        std::vector<uint8_t> data;
        data.reserve(bytes);
        bool ended = false;
        uint32_t expectedCrc = 0;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.rfind(kEnd, 0) == 0)
            {
                expectedCrc = static_cast<uint32_t>(std::strtoul(line.c_str() + std::strlen(kEnd), nullptr, 16));
                ended = true;
                break;
            }
            if (!DecodeBase64Line(line, data))
            {
                error = "the frame data was broken in transit (a line that is not base64)";
                return false;
            }
        }
        if (!ended)
        {
            error = "the frame was cut short: " + std::to_string(data.size()) + " of " + std::to_string(bytes) +
                    " bytes arrived";
            return false;
        }
        if (data.size() != bytes || Crc32(data.data(), data.size()) != expectedCrc)
        {
            error = "the frame was damaged in transit (" + std::to_string(data.size()) + " of " +
                    std::to_string(bytes) + " bytes, CRC mismatch); take it again";
            return false;
        }

        const size_t pixels = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
        frame.rgba.assign(pixels * 4, 255);
        for (size_t i = 0; i < pixels; ++i)
        {
            uint8_t* px = &frame.rgba[i * 4];
            if (frame.format == "RGB565" || frame.format == "RGB565A8")
            {
                // RGB565A8 keeps its colour plane first, as two bytes a pixel.
                const uint16_t v = static_cast<uint16_t>(data[i * 2] | (data[i * 2 + 1] << 8));
                px[0] = static_cast<uint8_t>(((v >> 11) & 31) * 255 / 31);
                px[1] = static_cast<uint8_t>(((v >> 5) & 63) * 255 / 63);
                px[2] = static_cast<uint8_t>((v & 31) * 255 / 31);
            }
            else if (frame.format == "RGB888")
            {
                px[0] = data[i * 3];
                px[1] = data[i * 3 + 1];
                px[2] = data[i * 3 + 2];
            }
            else  // ARGB8888, a little-endian 0xAARRGGBB word: B G R A in memory
            {
                px[0] = data[i * 4 + 2];
                px[1] = data[i * 4 + 1];
                px[2] = data[i * 4];
            }
        }
    }
    return true;
}

namespace
{
// Opens `port`, sends `request` and collects what comes back until
// `finished(received)` or the time is up. The port is opened without raising
// DTR or RTS, which would reset an ESP32. False only when the port could not
// be opened or written; what arrived is in `received` either way.
bool Exchange(const std::string& port, const std::string& request, const std::function<bool(const std::string&)>& isDone,
              int timeoutMs, std::string& received, std::string& error)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto finished = [&]() { return isDone(received); };

#ifdef _WIN32
    const std::string path = port.rfind("\\\\.\\", 0) == 0 ? port : "\\\\.\\" + port;
    HANDLE handle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        const DWORD code = GetLastError();
        error = "could not open " + port +
                (code == ERROR_ACCESS_DENIED ? " (in use: close the Serial Monitor or whatever else has it open)"
                                             : " (error " + std::to_string(code) + ")");
        return false;
    }
    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(handle, &dcb);
    dcb.BaudRate = 115200;  // USB serial ignores it; a UART console needs it
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    SetCommState(handle, &dcb);
    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout = 20;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutConstant = 500;
    SetCommTimeouts(handle, &timeouts);
    PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

    DWORD written = 0;
    if (!WriteFile(handle, request.data(), static_cast<DWORD>(request.size()), &written, nullptr) ||
        written != request.size())
    {
        CloseHandle(handle);
        error = "could not send the request to " + port;
        return false;
    }
    char buffer[16384];
    while (!finished() && std::chrono::steady_clock::now() < deadline)
    {
        DWORD read = 0;
        if (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0)
        {
            received.append(buffer, read);
        }
    }
    CloseHandle(handle);
#else
    const int fd = open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
    {
        error = "could not open " + port;
        return false;
    }
    termios tty = {};
    if (tcgetattr(fd, &tty) == 0)
    {
        cfmakeraw(&tty);
        cfsetispeed(&tty, B115200);
        cfsetospeed(&tty, B115200);
        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~(CRTSCTS | HUPCL);  // HUPCL drops DTR on close
        tcsetattr(fd, TCSANOW, &tty);
    }
    tcflush(fd, TCIOFLUSH);
    if (write(fd, request.data(), request.size()) != static_cast<ssize_t>(request.size()))
    {
        close(fd);
        error = "could not send the request to " + port;
        return false;
    }
    char buffer[16384];
    while (!finished() && std::chrono::steady_clock::now() < deadline)
    {
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n > 0)
        {
            received.append(buffer, static_cast<size_t>(n));
        }
        else
        {
            usleep(5000);
        }
    }
    close(fd);
#endif
    return true;
}
}  // namespace

bool CaptureDeviceScreenshot(const std::string& port, int timeoutMs, DeviceFrame& frame, std::string& error)
{
    std::string received;
    auto finished = [](const std::string& text)
    {
        const size_t begin = text.rfind(kBegin);
        return (begin != std::string::npos && text.find(kEnd, begin) != std::string::npos &&
                text.find('\n', text.find(kEnd, begin)) != std::string::npos) ||
               text.find(kError) != std::string::npos;
    };
    if (!Exchange(port, "\nDEKI:SCREENSHOT\n", finished, timeoutMs, received, error))
    {
        return false;
    }
    if (!finished(received) && received.find(kBegin) == std::string::npos && received.find(kError) == std::string::npos)
    {
        error = "no answer from the board on " + port + " within " + std::to_string(timeoutMs / 1000) +
                " s. Its firmware may be older than the screen capture (flash it again), or it is not running";
        return false;
    }
    return ParseDeviceScreenshot(received, frame, error);
}

bool ParseDeviceStats(const std::string& received, std::map<std::string, std::string>& stats, std::string& error)
{
    static const std::string kStats = "DEKI:STATS ";
    static const std::string kStatsError = "DEKI:STATS:ERROR";
    const size_t at = received.rfind(kStats);
    if (at == std::string::npos)
    {
        const size_t failed = received.rfind(kStatsError);
        if (failed != std::string::npos)
        {
            const size_t start = failed + kStatsError.size();
            const size_t end = received.find_first_of("\r\n", start);
            error = "the board has no statistics yet:" +
                    received.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
        else
        {
            error = "the board did not answer. Its firmware may be older than the statistics (flash it again), or "
                    "it is not running";
        }
        return false;
    }
    const size_t end = received.find_first_of("\r\n", at);
    if (end == std::string::npos)
    {
        error = "the board's statistics were cut short";
        return false;
    }
    std::istringstream fields(received.substr(at + kStats.size(), end - at - kStats.size()));
    std::string field;
    while (fields >> field)
    {
        const size_t eq = field.find('=');
        if (eq != std::string::npos)
        {
            stats[field.substr(0, eq)] = field.substr(eq + 1);
        }
    }
    return true;
}

bool QueryDeviceStats(const std::string& port, int timeoutMs, std::map<std::string, std::string>& stats,
                      std::string& error)
{
    std::string received;
    auto finished = [](const std::string& text)
    {
        const size_t at = text.rfind("DEKI:STATS");
        return at != std::string::npos && text.find('\n', at) != std::string::npos;
    };
    if (!Exchange(port, "\nDEKI:STATS\n", finished, timeoutMs, received, error))
    {
        return false;
    }
    return ParseDeviceStats(received, stats, error);
}

}  // namespace DekiEditor
