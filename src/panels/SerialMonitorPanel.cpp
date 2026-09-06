#include "SerialMonitorPanel.h"
#include "../services/SerialPorts.h"
#include "../PanelMaximize.h"
#include <deki-editor/EditorTheme.h>
#include "EditorApp.h"
#include "imgui.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <dirent.h>
#endif

namespace DekiEditor
{

// Baud rate options
const int SerialMonitorPanel::s_BaudRates[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };
const char* SerialMonitorPanel::s_BaudRateNames[] = { "9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600" };
const int SerialMonitorPanel::s_BaudRateCount = 8;

SerialMonitorPanel::SerialMonitorPanel(EditorApp* app)
    : m_App(app)
{
    // Enumerate available ports on startup
    m_AvailablePorts = EnumeratePorts();
    if (!m_AvailablePorts.empty())
    {
        m_SelectedPort = m_AvailablePorts[0];
        strncpy(m_PortBuffer, m_SelectedPort.c_str(), sizeof(m_PortBuffer) - 1);
    }
}

SerialMonitorPanel::~SerialMonitorPanel()
{
    Disconnect();
}

void SerialMonitorPanel::Draw()
{
    if (!m_Open)
        return;

    ImGui::SetNextWindowSize(ImVec2(700, 400), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("SERIAL MONITOR###Serial Monitor", &m_Open, DekiEditor::MaximizedWindowFlags("SERIAL MONITOR###Serial Monitor")))
    {
        ImGui::End();
        return;
    }

    DrawToolbar();
    ImGui::Separator();
    DrawOutput();
    ImGui::Separator();
    DrawInputArea();

    ImGui::End();
}

void SerialMonitorPanel::DrawToolbar()
{
    // Port selection
    ImGui::Text("Port:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(m_App->Dpi(100));

    // Combo for available ports
    if (ImGui::BeginCombo("##port", m_SelectedPort.c_str()))
    {
        for (size_t i = 0; i < m_AvailablePorts.size(); ++i)
        {
            bool selected = (m_AvailablePorts[i] == m_SelectedPort);
            if (ImGui::Selectable(m_AvailablePorts[i].c_str(), selected))
            {
                m_SelectedPort = m_AvailablePorts[i];
                strncpy(m_PortBuffer, m_SelectedPort.c_str(), sizeof(m_PortBuffer) - 1);
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();

    // Refresh ports button
    if (ImGui::Button("Refresh"))
    {
        m_AvailablePorts = EnumeratePorts();
    }

    ImGui::SameLine();
    ImGui::Text("Baud:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(m_App->Dpi(80));

    // Baud rate dropdown
    if (ImGui::BeginCombo("##baud", s_BaudRateNames[m_BaudRateIndex]))
    {
        for (int i = 0; i < s_BaudRateCount; ++i)
        {
            bool selected = (i == m_BaudRateIndex);
            if (ImGui::Selectable(s_BaudRateNames[i], selected))
            {
                m_BaudRateIndex = i;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();

    // Connect/Disconnect button
    if (m_IsConnected)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, DekiEditor::Palette::Red);
        if (ImGui::Button("Disconnect"))
        {
            Disconnect();
        }
        ImGui::PopStyleColor(2);
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        if (ImGui::Button("Connect"))
        {
            Connect();
        }
        ImGui::PopStyleColor(2);
    }

    ImGui::SameLine();
    DekiEditor::SchematicCheckbox("DTR", &m_DtrEnabled);
    ImGui::SameLine();
    DekiEditor::SchematicCheckbox("RTS", &m_RtsEnabled);

    // Second row: Clear, auto-scroll, timestamps, filter
    if (ImGui::Button("Clear"))
    {
        std::lock_guard<std::mutex> lock(m_OutputMutex);
        m_OutputLines.clear();
        m_PartialLine.clear();
    }

    ImGui::SameLine();
    DekiEditor::SchematicCheckbox("Auto-scroll", &m_AutoScroll);
    ImGui::SameLine();
    DekiEditor::SchematicCheckbox("Timestamps", &m_ShowTimestamps);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(m_App->Dpi(150));
    ImGui::InputTextWithHint("##filter", "Filter...", m_FilterText, sizeof(m_FilterText));

    // Connection status indicator
    ImGui::SameLine();
    float availWidth = ImGui::GetContentRegionAvail().x;
    if (availWidth > 80)
    {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + availWidth - 80);
        if (m_IsConnected)
        {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Connected");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Disconnected");
        }
    }
}

void SerialMonitorPanel::DrawOutput()
{
    // Calculate available height for output (leave room for input area)
    float inputHeight = ImGui::GetFrameHeightWithSpacing() * 2;
    float availHeight = ImGui::GetContentRegionAvail().y - inputHeight;

    ImGui::BeginChild("SerialOutput", ImVec2(0, availHeight), true,
                      ImGuiWindowFlags_HorizontalScrollbar);

    std::lock_guard<std::mutex> lock(m_OutputMutex);

    std::string filterStr(m_FilterText);
    std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(), ::tolower);

    int lineIndex = 0;
    for (const auto& line : m_OutputLines)
    {
        // Filter by text
        if (!filterStr.empty())
        {
            std::string textLower = line.text;
            std::transform(textLower.begin(), textLower.end(), textLower.begin(), ::tolower);
            if (textLower.find(filterStr) == std::string::npos)
                continue;
        }

        ImGui::PushID(lineIndex);

        // Format timestamp
        std::string displayText;
        if (m_ShowTimestamps)
        {
            auto time_t_val = std::chrono::system_clock::to_time_t(line.timestamp);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          line.timestamp.time_since_epoch()) %
                      1000;

            std::tm tm_val;
#ifdef _WIN32
            localtime_s(&tm_val, &time_t_val);
#else
            localtime_r(&time_t_val, &tm_val);
#endif
            char timeBuf[24];
            snprintf(timeBuf, sizeof(timeBuf), "[%02d:%02d:%02d.%03d] ",
                     tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec, (int)ms.count());
            displayText = timeBuf;
        }
        displayText += line.text;

        // Alternating row background
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        float availWidth = ImGui::GetContentRegionAvail().x;
        ImVec2 textSize = ImGui::CalcTextSize(displayText.c_str());
        float rowHeight = textSize.y + ImGui::GetStyle().ItemSpacing.y;

        ImU32 bgColor = (lineIndex % 2 == 0)
                            ? IM_COL32(40, 40, 40, 255)
                            : IM_COL32(50, 50, 50, 255);

        ImGui::GetWindowDrawList()->AddRectFilled(
            cursorPos,
            ImVec2(cursorPos.x + availWidth, cursorPos.y + rowHeight),
            bgColor);

        // Render with ANSI colors
        if (line.isError)
        {
            // Error lines are always red
            ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Red);
            ImGui::TextUnformatted(displayText.c_str());
            ImGui::PopStyleColor();
        }
        else
        {
            // Render timestamp prefix in default color if present
            if (m_ShowTimestamps)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                auto bracketEnd = displayText.find("] ");
                if (bracketEnd != std::string::npos)
                {
                    std::string tsText = displayText.substr(0, bracketEnd + 2);
                    ImGui::TextUnformatted(tsText.c_str());
                    ImGui::SameLine(0, 0);
                }
                ImGui::PopStyleColor();
            }

            // Render each ANSI segment with its color
            for (size_t s = 0; s < line.segments.size(); ++s)
            {
                const auto& seg = line.segments[s];
                ImGui::PushStyleColor(ImGuiCol_Text, seg.color);
                ImGui::TextUnformatted(seg.text.c_str());
                ImGui::PopStyleColor();
                if (s + 1 < line.segments.size())
                    ImGui::SameLine(0, 0);
            }
        }

        // Right-click context menu
        PushContextMenuPopupStyle();
        if (ImGui::BeginPopupContextItem("line_ctx"))
        {
            PushContextMenuItemStyle();
            if (ImGui::MenuItem("Copy"))
            {
                ImGui::SetClipboardText(line.text.c_str());
            }
            if (ImGui::MenuItem("Copy All"))
            {
                std::string allText;
                for (const auto& l : m_OutputLines)
                {
                    allText += l.text + "\n";
                }
                ImGui::SetClipboardText(allText.c_str());
            }
            ImGui::PopStyleVar();  // context menu ItemSpacing
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();  // context menu WindowPadding

        ImGui::PopID();
        lineIndex++;
    }

    // Auto-scroll to bottom
    if (m_AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
    {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
}

void SerialMonitorPanel::DrawInputArea()
{
    ImGui::Text("Send:");
    ImGui::SameLine();

    // Input field
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - m_App->Dpi(150));

    bool enterPressed = ImGui::InputText("##input", m_InputBuffer, sizeof(m_InputBuffer),
                                         ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::SameLine();
    if (ImGui::Button("Send") || enterPressed)
    {
        if (m_IsConnected && strlen(m_InputBuffer) > 0)
        {
            SendCommand(m_InputBuffer);
            m_InputBuffer[0] = '\0';
        }
    }

    ImGui::SameLine();
    DekiEditor::SchematicCheckbox("\\n", &m_SendNewline);
    if (ImGui::IsItemHovered())
    {
        DekiEditor::Tooltip("Append newline to sent commands");
    }
}

std::vector<std::string> SerialMonitorPanel::EnumeratePorts()
{
    return EnumerateSerialPorts();
}

bool SerialMonitorPanel::Connect()
{
    if (m_IsConnected)
        return true;

#ifdef _WIN32
    // Format port name for Windows
    std::string portName = m_SelectedPort;
    if (portName.find("\\\\.\\") != 0)
    {
        portName = "\\\\.\\" + portName;
    }

    m_SerialHandle = CreateFileA(
        portName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (m_SerialHandle == INVALID_HANDLE_VALUE)
    {
        AddOutputLine("Failed to open " + m_SelectedPort + " (Error: " +
                          std::to_string(GetLastError()) + ")",
                      true);
        return false;
    }

    // Configure serial port
    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(m_SerialHandle, &dcb))
    {
        CloseHandle(m_SerialHandle);
        m_SerialHandle = INVALID_HANDLE_VALUE;
        AddOutputLine("Failed to get serial port state", true);
        return false;
    }

    dcb.BaudRate = s_BaudRates[m_BaudRateIndex];
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    dcb.fDtrControl = m_DtrEnabled ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    dcb.fRtsControl = m_RtsEnabled ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;

    if (!SetCommState(m_SerialHandle, &dcb))
    {
        CloseHandle(m_SerialHandle);
        m_SerialHandle = INVALID_HANDLE_VALUE;
        AddOutputLine("Failed to configure serial port", true);
        return false;
    }

    // Set timeouts for non-blocking reads
    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(m_SerialHandle, &timeouts);

    // Clear buffers
    PurgeComm(m_SerialHandle, PURGE_RXCLEAR | PURGE_TXCLEAR);

#else
    m_SerialFd = open(m_SelectedPort.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (m_SerialFd < 0)
    {
        AddOutputLine("Failed to open " + m_SelectedPort, true);
        return false;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(m_SerialFd, &tty) != 0)
    {
        close(m_SerialFd);
        m_SerialFd = -1;
        AddOutputLine("Failed to get serial port attributes", true);
        return false;
    }

    // Set baud rate
    speed_t baudRate;
    switch (s_BaudRates[m_BaudRateIndex])
    {
        case 9600: baudRate = B9600; break;
        case 19200: baudRate = B19200; break;
        case 38400: baudRate = B38400; break;
        case 57600: baudRate = B57600; break;
        case 115200: baudRate = B115200; break;
        case 230400: baudRate = B230400; break;
        case 460800: baudRate = B460800; break;
        case 921600: baudRate = B921600; break;
        default: baudRate = B115200; break;
    }
    cfsetospeed(&tty, baudRate);
    cfsetispeed(&tty, baudRate);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag &= ~IGNBRK;
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(m_SerialFd, TCSANOW, &tty) != 0)
    {
        close(m_SerialFd);
        m_SerialFd = -1;
        AddOutputLine("Failed to set serial port attributes", true);
        return false;
    }
#endif

    m_IsConnected = true;
    m_Running = true;

    // Start read thread
    m_ReadThread = std::thread(&SerialMonitorPanel::ReadThreadFunc, this);

    AddOutputLine("Connected to " + m_SelectedPort + " at " +
                  std::to_string(s_BaudRates[m_BaudRateIndex]) + " baud");

    return true;
}

void SerialMonitorPanel::Disconnect()
{
    if (!m_IsConnected)
        return;

    // Stop read thread
    m_Running = false;
    if (m_ReadThread.joinable())
    {
        m_ReadThread.join();
    }

#ifdef _WIN32
    if (m_SerialHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_SerialHandle);
        m_SerialHandle = INVALID_HANDLE_VALUE;
    }
#else
    if (m_SerialFd >= 0)
    {
        close(m_SerialFd);
        m_SerialFd = -1;
    }
#endif

    m_IsConnected = false;
    AddOutputLine("Disconnected from " + m_SelectedPort);
}

void SerialMonitorPanel::SendCommand(const std::string& cmd)
{
    if (!m_IsConnected)
        return;

    std::string data = cmd;
    if (m_SendNewline)
    {
        data += "\n";
    }

#ifdef _WIN32
    DWORD bytesWritten = 0;
    if (!WriteFile(m_SerialHandle, data.c_str(), (DWORD)data.size(), &bytesWritten, nullptr))
    {
        AddOutputLine("Failed to send data", true);
    }
#else
    ssize_t written = write(m_SerialFd, data.c_str(), data.size());
    if (written < 0)
    {
        AddOutputLine("Failed to send data", true);
    }
#endif
}

void SerialMonitorPanel::ReadThreadFunc()
{
    char buffer[512];

    while (m_Running)
    {
#ifdef _WIN32
        DWORD bytesRead = 0;
        if (ReadFile(m_SerialHandle, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0)
        {
            buffer[bytesRead] = '\0';

            // Process received data
            std::string data(buffer, bytesRead);

            // Handle partial lines and complete lines
            for (char c : data)
            {
                if (c == '\n' || c == '\r')
                {
                    if (!m_PartialLine.empty())
                    {
                        AddOutputLine(m_PartialLine);
                        m_PartialLine.clear();
                    }
                }
                else
                {
                    m_PartialLine += c;
                }
            }
        }
#else
        ssize_t bytesRead = read(m_SerialFd, buffer, sizeof(buffer) - 1);
        if (bytesRead > 0)
        {
            buffer[bytesRead] = '\0';
            std::string data(buffer, bytesRead);

            for (char c : data)
            {
                if (c == '\n' || c == '\r')
                {
                    if (!m_PartialLine.empty())
                    {
                        AddOutputLine(m_PartialLine);
                        m_PartialLine.clear();
                    }
                }
                else
                {
                    m_PartialLine += c;
                }
            }
        }
#endif

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Flush any remaining partial line
    if (!m_PartialLine.empty())
    {
        AddOutputLine(m_PartialLine);
        m_PartialLine.clear();
    }
}

ImVec4 SerialMonitorPanel::AnsiCodeToColor(int code)
{
    switch (code)
    {
        case 0: return ImVec4(0.9f, 0.9f, 0.9f, 1.0f);   // Reset / default
        case 30: return ImVec4(0.2f, 0.2f, 0.2f, 1.0f);  // Black
        case 31: return DekiEditor::Palette::Red;        // Red
        case 32: return ImVec4(0.3f, 0.9f, 0.3f, 1.0f);  // Green
        case 33: return ImVec4(1.0f, 0.9f, 0.3f, 1.0f);  // Yellow
        case 34: return ImVec4(0.4f, 0.5f, 1.0f, 1.0f);  // Blue
        case 35: return ImVec4(0.9f, 0.4f, 0.9f, 1.0f);  // Magenta
        case 36: return ImVec4(0.4f, 0.9f, 0.9f, 1.0f);  // Cyan
        case 37: return ImVec4(0.9f, 0.9f, 0.9f, 1.0f);  // White
        default: return ImVec4(0.9f, 0.9f, 0.9f, 1.0f);  // Default
    }
}

std::vector<AnsiSegment> SerialMonitorPanel::ParseAnsi(const std::string& text)
{
    std::vector<AnsiSegment> segments;
    ImVec4 currentColor = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
    std::string current;

    size_t i = 0;
    while (i < text.size())
    {
        // Check for ESC character (0x1B)
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[')
        {
            // Flush current text
            if (!current.empty())
            {
                segments.push_back({ current, currentColor });
                current.clear();
            }

            // Parse CSI sequence: ESC [ (params) m
            i += 2;  // skip ESC [
            std::string paramStr;
            while (i < text.size() && text[i] != 'm' && text[i] >= 0x20 && text[i] <= 0x3F)
            {
                paramStr += text[i];
                i++;
            }

            if (i < text.size() && text[i] == 'm')
            {
                i++;  // skip 'm'

                // Parse semicolon-separated codes
                std::istringstream ss(paramStr);
                std::string token;
                while (std::getline(ss, token, ';'))
                {
                    if (!token.empty())
                    {
                        int code = std::atoi(token.c_str());
                        if (code == 0)
                            currentColor = AnsiCodeToColor(0);
                        else if (code >= 30 && code <= 37)
                            currentColor = AnsiCodeToColor(code);
                        // Ignore bold (1), dim (2), etc. — just use color
                    }
                }
            }
        }
        else
        {
            current += text[i];
            i++;
        }
    }

    if (!current.empty())
    {
        segments.push_back({ current, currentColor });
    }

    // If no segments, add the whole text as default
    if (segments.empty())
    {
        segments.push_back({ text, ImVec4(0.9f, 0.9f, 0.9f, 1.0f) });
    }

    return segments;
}

void SerialMonitorPanel::AddOutputLine(const std::string& text, bool isError)
{
    std::lock_guard<std::mutex> lock(m_OutputMutex);

    SerialOutputLine line;
    line.segments = ParseAnsi(text);

    // Store plain text for filtering
    line.text.clear();
    for (const auto& seg : line.segments)
        line.text += seg.text;

    line.timestamp = std::chrono::system_clock::now();
    line.isError = isError;

    m_OutputLines.push_back(line);

    // Limit buffer size
    const size_t maxLines = 10000;
    if (m_OutputLines.size() > maxLines)
    {
        m_OutputLines.erase(m_OutputLines.begin(),
                            m_OutputLines.begin() + (m_OutputLines.size() - maxLines));
    }
}

}  // namespace DekiEditor
