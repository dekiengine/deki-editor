#pragma once

#include "IPanel.h"

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

#include "imgui.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace DekiEditor
{

class EditorApp;

struct AnsiSegment
{
    std::string text;
    ImVec4 color;
};

struct SerialOutputLine
{
    std::string text;                   // Plain text (for filtering)
    std::vector<AnsiSegment> segments;  // Colored segments for rendering
    std::chrono::system_clock::time_point timestamp;
    bool isError = false;
};

class SerialMonitorPanel : public IPanel
{
public:
    const char* Id() const override { return "SerialMonitor"; }
    SerialMonitorPanel(EditorApp* app);
    ~SerialMonitorPanel();

    void Draw();


private:
    void DrawToolbar();
    void DrawOutput();
    void DrawInputArea();

    // Serial port operations
    bool Connect();
    void Disconnect();
    void SendCommand(const std::string& cmd);
    void ReadThreadFunc();

    // Port enumeration
    std::vector<std::string> EnumeratePorts();

    // Add output line (thread-safe)
    void AddOutputLine(const std::string& text, bool isError = false);

    // ANSI escape code parsing
    static std::vector<AnsiSegment> ParseAnsi(const std::string& text);
    static ImVec4 AnsiCodeToColor(int code);

    EditorApp* m_App;

    // Port settings
    std::string m_SelectedPort = "COM3";
    char m_PortBuffer[32] = "COM3";
    int m_BaudRateIndex = 4;  // 115200 default
    bool m_DtrEnabled = true;
    bool m_RtsEnabled = true;

    // Connection state
    bool m_IsConnected = false;
    std::atomic<bool> m_Running{ false };
    std::thread m_ReadThread;

    // Output buffer
    std::vector<SerialOutputLine> m_OutputLines;
    std::mutex m_OutputMutex;
    bool m_AutoScroll = true;
    bool m_ShowTimestamps = true;
    char m_FilterText[256] = "";

    // Input
    char m_InputBuffer[256] = "";
    bool m_SendNewline = true;

    // Available ports cache
    std::vector<std::string> m_AvailablePorts;
    int m_SelectedPortIndex = 0;

    // Partial line buffer (for lines that don't end in newline)
    std::string m_PartialLine;

    // Platform-specific handle
#ifdef _WIN32
    HANDLE m_SerialHandle = INVALID_HANDLE_VALUE;
#else
    int m_SerialFd = -1;
#endif

    // Baud rate options
    static const int s_BaudRates[];
    static const char* s_BaudRateNames[];
    static const int s_BaudRateCount;
};

}  // namespace DekiEditor
