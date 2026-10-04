#include <deki-editor/SerialPorts.h>

#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
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
            LONG result =
                RegEnumValueA(hKey, i++, valueName, &valueNameSize, nullptr, &type, (LPBYTE)portName, &portNameSize);
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

}  // namespace DekiEditor
