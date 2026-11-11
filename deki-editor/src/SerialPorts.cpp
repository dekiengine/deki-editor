#include <deki-editor/SerialPorts.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace DekiEditor
{

std::vector<std::string> EnumerateSerialPorts()
{
    std::vector<std::string> ports;

#ifdef _WIN32
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        char valueName[256], portName[256];
        DWORD valueNameSize, portNameSize, type;
        DWORD i = 0;

        while (true)
        {
            valueNameSize = sizeof(valueName);
            portNameSize = sizeof(portName);
            LONG result = RegEnumValueA(hKey, i++, valueName, &valueNameSize, nullptr, &type,
                                        reinterpret_cast<LPBYTE>(portName), &portNameSize);
            if (result != ERROR_SUCCESS)
            {
                break;
            }

            if (type == REG_SZ)
            {
                ports.push_back(portName);
            }
        }
        RegCloseKey(hKey);
    }

    // COM1 before COM10
    std::sort(ports.begin(), ports.end(),
              [](const std::string& a, const std::string& b)
              {
                  int numA = 0, numB = 0;
                  if (a.substr(0, 3) == "COM")
                  {
                      numA = std::atoi(a.c_str() + 3);
                  }
                  if (b.substr(0, 3) == "COM")
                  {
                      numB = std::atoi(b.c_str() + 3);
                  }
                  return numA < numB;
              });
#else
    // Linux/macOS: scan /dev for the common serial port prefixes
    DIR* dir = opendir("/dev");
    if (dir)
    {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr)
        {
            std::string name = entry->d_name;
            if (name.find("ttyUSB") == 0 || name.find("ttyACM") == 0 || name.find("tty.usb") == 0 ||
                name.find("cu.usb") == 0)
            {
                ports.push_back("/dev/" + name);
            }
        }
        closedir(dir);
    }
    std::sort(ports.begin(), ports.end());
#endif

    return ports;
}

bool SerialExchange(const std::string& port, const std::string& request,
                    const std::function<bool(const std::string&)>& isDone, int timeoutMs, std::string& received,
                    std::string& error)
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

}  // namespace DekiEditor
