#include <deki-editor/EditorHttpUtils.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <array>
#include <vector>
#include <string>
#include <system_error>
#include <filesystem>

#ifdef _WIN32
// The helpers below call MultiByteToWideChar, so windows.h has to be visible
// here as well as at the WinHTTP block further down (the include is guarded).
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace
{

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4).
//
// Written out here rather than pulled in: the editor has no crypto dependency,
// and adding OpenSSL (or splitting between BCrypt and CommonCrypto) to hash a
// downloaded archive is a lot of surface for one function. This is only used to
// compare a download against a pinned digest, never for secrets.
// ---------------------------------------------------------------------------
class Sha256
{
public:
    void Update(const uint8_t* data, size_t len)
    {
        m_BitLen += static_cast<uint64_t>(len) * 8;
        while (len > 0)
        {
            const size_t take = std::min(len, size_t{ 64 } - m_BufferLen);
            std::memcpy(m_Buffer + m_BufferLen, data, take);
            m_BufferLen += take;
            data += take;
            len -= take;

            if (m_BufferLen == 64)
            {
                Transform(m_Buffer);
                m_BufferLen = 0;
            }
        }
    }

    std::string HexDigest()
    {
        // Pad: 0x80, zeros, then the message length as a big-endian uint64.
        uint8_t pad[72] = { 0x80 };
        const size_t padLen = (m_BufferLen < 56) ? (56 - m_BufferLen) : (120 - m_BufferLen);
        const uint64_t bitLen = m_BitLen;

        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i)
            lenBytes[i] = static_cast<uint8_t>(bitLen >> (56 - i * 8));

        Update2(pad, padLen);
        Update2(lenBytes, 8);

        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (uint32_t h : m_State)
            out << std::setw(8) << h;
        return out.str();
    }

private:
    // Update without touching m_BitLen — used by the padding step, whose bytes
    // must not count toward the message length.
    void Update2(const uint8_t* data, size_t len)
    {
        while (len > 0)
        {
            const size_t take = std::min(len, size_t{ 64 } - m_BufferLen);
            std::memcpy(m_Buffer + m_BufferLen, data, take);
            m_BufferLen += take;
            data += take;
            len -= take;
            if (m_BufferLen == 64)
            {
                Transform(m_Buffer);
                m_BufferLen = 0;
            }
        }
    }

    static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void Transform(const uint8_t block[64])
    {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
        {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(block[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = m_State[0], b = m_State[1], c = m_State[2], d = m_State[3];
        uint32_t e = m_State[4], f = m_State[5], g = m_State[6], h = m_State[7];

        for (int i = 0; i < 64; ++i)
        {
            const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + ch + k[i] + w[i];
            const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        m_State[0] += a;
        m_State[1] += b;
        m_State[2] += c;
        m_State[3] += d;
        m_State[4] += e;
        m_State[5] += f;
        m_State[6] += g;
        m_State[7] += h;
    }

    uint32_t m_State[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t m_Buffer[64] = {};
    size_t m_BufferLen = 0;
    uint64_t m_BitLen = 0;
};

std::string ToLowerAscii(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

#ifdef _WIN32
/// Escape a value for use inside a PowerShell single-quoted string, where the
/// only special character is the apostrophe and it is escaped by doubling.
std::string PowerShellSingleQuote(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (char c : value)
    {
        out.push_back(c);
        if (c == '\'')
            out.push_back('\'');
    }
    return out;
}

/// Encode a script for `powershell -EncodedCommand`: UTF-16LE, then base64.
/// The result is base64 alphabet only, so it cannot carry a metacharacter
/// through the process command line.
std::string EncodeCommandUtf16Base64(const std::string& utf8Script)
{
    // UTF-8 -> UTF-16LE
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8Script.c_str(),
                                            static_cast<int>(utf8Script.size()),
                                            nullptr, 0);
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    if (wideLen > 0)
    {
        MultiByteToWideChar(CP_UTF8, 0, utf8Script.c_str(),
                            static_cast<int>(utf8Script.size()),
                            wide.data(), wideLen);
    }

    const auto* bytes = reinterpret_cast<const uint8_t*>(wide.data());
    const size_t byteLen = wide.size() * sizeof(wchar_t);

    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((byteLen + 2) / 3) * 4);
    for (size_t i = 0; i < byteLen; i += 3)
    {
        const uint32_t b0 = bytes[i];
        const uint32_t b1 = (i + 1 < byteLen) ? bytes[i + 1] : 0;
        const uint32_t b2 = (i + 2 < byteLen) ? bytes[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

        out.push_back(kTable[(triple >> 18) & 0x3F]);
        out.push_back(kTable[(triple >> 12) & 0x3F]);
        out.push_back((i + 1 < byteLen) ? kTable[(triple >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < byteLen) ? kTable[triple & 0x3F] : '=');
    }
    return out;
}
#endif

#ifndef _WIN32
// Spawn a process from an argument vector. Nothing goes through /bin/sh, so a
// quote or semicolon in a path is just a character in a filename.
int RunProcess(const std::vector<std::string>& args)
{
    if (args.empty()) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0)
    {
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// Same, capturing the child's stdout. Used for `curl` fetches and `unzip -Z1`
// listings, which used to go through popen() and therefore /bin/sh.
int RunProcessCapture(const std::vector<std::string>& args, std::string& out)
{
    out.clear();
    if (args.empty()) return -1;

    int fds[2];
    if (pipe(fds) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if (pid == 0)
    {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(fds[1]);
    char buffer[4096];
    for (;;)
    {
        const ssize_t n = read(fds[0], buffer, sizeof(buffer));
        if (n <= 0) break;
        out.append(buffer, static_cast<size_t>(n));
    }
    close(fds[0]);

    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
#endif

#ifdef _WIN32
// Quote one argument the way CommandLineToArgvW un-quotes it, so a path with a
// space or a quote stays one argument.
std::string QuoteWindowsArgument(const std::string& arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos)
        return arg;
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg)
    {
        if (c == '\\')
        {
            ++backslashes;
            continue;
        }
        if (c == '"')
        {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, '\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

// Run an argument vector without a window, capture stdout, kill it after
// `timeoutMs`. Returns the exit code, or -1 when it could not start or timed
// out. No shell is involved: the command line is assembled with
// QuoteWindowsArgument and read back by the child's C runtime.
int RunProcessCaptureWindows(const std::vector<std::string>& args, std::string& out, DWORD timeoutMs)
{
    out.clear();
    if (args.empty()) return -1;

    std::string command;
    for (const auto& a : args)
    {
        if (!command.empty()) command += ' ';
        command += QuoteWindowsArgument(a);
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    HANDLE readPipe = NULL, writePipe = NULL;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return -1;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = NULL;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    std::vector<char> cmdBuf(command.begin(), command.end());
    cmdBuf.push_back('\0');
    const BOOL started = CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(writePipe);
    if (!started)
    {
        CloseHandle(readPipe);
        return -1;
    }

    // Drain the pipe while waiting, or a chatty child blocks on a full pipe.
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    bool timedOut = false;
    char buffer[4096];
    for (;;)
    {
        DWORD avail = 0;
        if (PeekNamedPipe(readPipe, NULL, 0, NULL, &avail, NULL) && avail > 0)
        {
            DWORD n = 0;
            if (ReadFile(readPipe, buffer, sizeof(buffer), &n, NULL) && n > 0)
                out.append(buffer, n);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 50) != WAIT_TIMEOUT)
        {
            // Child exited: take whatever is left.
            DWORD n = 0;
            while (PeekNamedPipe(readPipe, NULL, 0, NULL, &avail, NULL) && avail > 0 &&
                   ReadFile(readPipe, buffer, sizeof(buffer), &n, NULL) && n > 0)
                out.append(buffer, n);
            break;
        }
        if (GetTickCount64() >= deadline)
        {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            timedOut = true;
            break;
        }
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(readPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return timedOut ? -1 : static_cast<int>(exitCode);
}

std::string WindowsSystemTool(const char* exe)
{
    char sysDir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryA(sysDir, MAX_PATH);
    std::string dir = (n > 0 && n < MAX_PATH) ? std::string(sysDir) : std::string("C:\\Windows\\System32");
    return dir + "\\" + exe;
}
#endif

}  // namespace

#ifdef _WIN32
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

// Helper to convert wide string to narrow string (UTF-8)
static std::string WideToNarrow(const std::wstring& wide)
{
    if (wide.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &result[0], size, nullptr, nullptr);
    return result;
}

static std::string FormatBytes(DWORD bytes)
{
    if (bytes >= 1024 * 1024)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
        return buf;
    }
    else if (bytes >= 1024)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.1f KB", (double)bytes / 1024.0);
        return buf;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%u bytes", bytes);
    return buf;
}
#endif

namespace DekiEditor
{

bool EditorHttpUtils::IsAllowedUrl(const std::string& url, std::string& reason)
{
    if (url.empty())
    {
        reason = "empty URL";
        return false;
    }
    if (url.size() > 2048)
    {
        reason = "URL is longer than 2048 characters";
        return false;
    }
    // https only, at hop 0 as well as on redirects: everything fetched here is
    // either parsed as an index of what to install next or executed as a
    // toolchain, so a plain-http first hop hands substitution rights to the path.
    if (url.rfind("https://", 0) != 0)
    {
        reason = "URL must start with https://";
        return false;
    }
    for (unsigned char c : url)
    {
        // Nothing here reaches a shell any more; the metacharacters are refused
        // anyway so the intent stays explicit and a future caller cannot
        // reintroduce the problem.
        if (c < 0x21 || c > 0x7e || c == '"' || c == '\'' || c == '`' || c == '\\' ||
            c == '|' || c == '<' || c == '>' || c == '^' || c == '$' || c == '(' ||
            c == ')' || c == '{' || c == '}' || c == ';')
        {
            reason = "URL contains an invalid character";
            return false;
        }
    }
    return true;
}

std::string EditorHttpUtils::FetchUrl(const std::string& url)
{
    {
        std::string reason;
        if (!IsAllowedUrl(url, reason)) return "";
    }

#ifdef _WIN32
    std::string result;

    // Convert URL to wide string
    std::wstring wUrl(url.begin(), url.end());

    // Parse URL
    URL_COMPONENTS urlComp;
    ZeroMemory(&urlComp, sizeof(urlComp));
    urlComp.dwStructSize = sizeof(urlComp);

    wchar_t hostName[256] = { 0 };
    wchar_t urlPath[2048] = { 0 };

    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = sizeof(hostName) / sizeof(wchar_t);
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = sizeof(urlPath) / sizeof(wchar_t);

    if (!WinHttpCrackUrl(wUrl.c_str(), 0, 0, &urlComp))
    {
        return "";
    }

    HINTERNET hSession = WinHttpOpen(L"DekiEditor/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession) return "";

    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath,
                                            NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // GitHub API requires User-Agent header
    WinHttpAddRequestHeaders(hRequest, L"User-Agent: DekiEditor/1.0", -1, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    if (!WinHttpReceiveResponse(hRequest, NULL))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Treat a non-2xx as failure rather than returning the error body. Callers
    // test the result for emptiness to decide whether to fall back to a mirror,
    // and a 404's HTML page is not empty — so without this check a dead endpoint
    // silently wins over a working fallback and parses to nothing.
    {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (WinHttpQueryHeaders(hRequest,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                                WINHTTP_NO_HEADER_INDEX) &&
            (statusCode < 200 || statusCode >= 300))
        {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return "";
        }
    }

    // Read response
    char buffer[4096];
    DWORD bytesRead = 0;
    while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0)
    {
        buffer[bytesRead] = '\0';
        result += buffer;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return result;
#else
    // Linux/macOS: use curl.
    //
    // -f so HTTP errors fail with no body, matching the Windows status check
    // above: callers see "" and can fall back to a mirror, rather than handing an
    // error page to a JSON parser.
    //
    // -L because the registry's addresses redirect between each other — while a
    // Pages custom domain is configured, the github.io URL 301s to it.
    //
    // Bounded, because "unreachable" does not always mean "refused". When a host
    // is null-routed rather than rejecting — as happens to Cloudflare IP ranges
    // during the ISP-level blocks used against football piracy — the connect
    // simply hangs. Without a cap the caller waits out curl's ~2 minute default,
    // which is long enough that a registry fallback times out before it is ever
    // tried, and long enough to look like the editor has frozen.
    // An argument vector, not a shell string: the URL used to be interpolated
    // into a popen() command line, where `$(...)` in a registry id ran.
    // --proto '=https' --proto-redir '=https' keep every hop on TLS.
    std::string result;
    const int rc = RunProcessCapture({ "curl", "-sfL", "--connect-timeout", "5", "--max-time", "30",
                                       "--proto", "=https", "--proto-redir", "=https",
                                       "-H", "User-Agent: DekiEditor", "--", url },
                                     result);
    if (rc != 0) return "";
    return result;
#endif
}

std::string EditorHttpUtils::Sha256File(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return "";

    Sha256 hash;
    std::vector<char> buffer(64 * 1024);
    while (file)
    {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = file.gcount();
        if (got > 0)
            hash.Update(reinterpret_cast<const uint8_t*>(buffer.data()),
                        static_cast<size_t>(got));
    }
    if (file.bad())
        return "";

    return hash.HexDigest();
}

bool EditorHttpUtils::DownloadFileVerified(const std::string& url,
                                           const std::string& localPath,
                                           const std::string& expectedSha256,
                                           std::atomic<bool>* cancelFlag,
                                           HttpStatusCallback status,
                                           bool allowUnpinned)
{
    if (expectedSha256.empty() && !allowUnpinned)
    {
        // An unpinned component is a gap in the chain that makes every other
        // pin decorative. The built-in toolchain definitions carry a hash for
        // every archive; only a user-added source may opt out, loudly.
        if (status)
            status("Refusing to download " + url + ": no SHA-256 is pinned for it");
        return false;
    }

    if (!DownloadFile(url, localPath, cancelFlag, status))
        return false;

    if (expectedSha256.empty())
    {
        if (status)
            status("WARNING: no SHA-256 pinned for " + url + " — contents not verified");
        return true;
    }

    if (status) status("Verifying SHA-256...");

    const std::string actual = Sha256File(localPath);
    if (actual.empty())
    {
        if (status) status("Could not hash downloaded file: " + localPath);
        std::error_code ec;
        std::filesystem::remove(localPath, ec);
        return false;
    }

    if (actual != ToLowerAscii(expectedSha256))
    {
        // The file is about to be extracted and executed as a build toolchain,
        // so a mismatch is not something to warn about and continue past.
        if (status)
        {
            status("SHA-256 mismatch for " + url +
                   "\n  expected " + ToLowerAscii(expectedSha256) +
                   "\n  actual   " + actual +
                   "\nDiscarding the download.");
        }
        std::error_code ec;
        std::filesystem::remove(localPath, ec);
        return false;
    }

    if (status) status("SHA-256 verified");
    return true;
}

bool EditorHttpUtils::ParseGitHubRelease(const std::string& json, const std::string& assetPattern,
                                         const std::string& repo,
                                         std::string& outVersion, std::string& outDownloadUrl)
{
    // A real JSON parse (this used to be substring scanning that took the
    // first quoted string after "browser_download_url" wherever it sat), and
    // the asset must live under the repository that was asked about: a
    // release body can name any URL, and what it names gets executed as a
    // toolchain.
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_object()) return false;

    const std::string tag = j.value("tag_name", "");
    if (tag.empty()) return false;

    const std::string prefix = repo.empty() ? std::string() : "https://github.com/" + repo + "/releases/download/";

    const auto assets = j.find("assets");
    if (assets == j.end() || !assets->is_array()) return false;

    for (const auto& asset : *assets)
    {
        if (!asset.is_object()) continue;
        const std::string downloadUrl = asset.value("browser_download_url", "");
        if (downloadUrl.empty() || downloadUrl.find(assetPattern) == std::string::npos) continue;

        // For the ESP32 release pattern (esp32-) skip the -libs and .tar variants:
        // the main package is esp32-X.X.X.zip.
        if (assetPattern == "esp32-")
        {
            if (downloadUrl.find("-libs") != std::string::npos) continue;
            if (downloadUrl.find(".tar.") != std::string::npos) continue;
            if (downloadUrl.size() < 4 || downloadUrl.compare(downloadUrl.size() - 4, 4, ".zip") != 0) continue;
        }

        if (!prefix.empty() && downloadUrl.rfind(prefix, 0) != 0) return false;

        std::string reason;
        if (!IsAllowedUrl(downloadUrl, reason)) return false;

        outVersion = tag;
        outDownloadUrl = downloadUrl;
        return true;
    }

    return false;
}

// Archive checks: shared by both platforms below.
bool EditorHttpUtils::IsSafeArchiveEntry(const std::string& entry)
{
    if (entry.empty()) return false;
    std::string e = entry;
    for (char& c : e)
        if (c == '\\') c = '/';
    if (e.front() == '/') return false;                                   // absolute
    if (e.size() >= 2 && e[1] == ':') return false;                       // drive letter
    if (e.rfind("//", 0) == 0) return false;                              // UNC
    for (unsigned char c : e)
        if (c < 0x20) return false;                                       // control characters
    // Any ".." path segment, wherever it sits.
    size_t start = 0;
    while (start <= e.size())
    {
        const size_t end = e.find('/', start);
        const std::string seg = e.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (seg == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

bool EditorHttpUtils::ZipEntriesAreSafe(const std::string& zipPath, std::string& reason)
{
    std::string listing;
#ifdef _WIN32
    const int rc = RunProcessCaptureWindows({ WindowsSystemTool("tar.exe"), "-tf", zipPath }, listing, 60u * 1000u);
#else
    const int rc = RunProcessCapture({ "unzip", "-Z1", "--", zipPath }, listing);
#endif
    if (rc != 0)
    {
        reason = "could not list the archive";
        return false;
    }
    std::istringstream lines(listing);
    std::string line;
    while (std::getline(lines, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (!IsSafeArchiveEntry(line))
        {
            reason = "archive entry escapes the destination: " + line;
            return false;
        }
    }
    return true;
}

#ifdef _WIN32

bool EditorHttpUtils::DownloadFile(const std::string& url, const std::string& localPath,
                                   std::atomic<bool>* cancelFlag, HttpStatusCallback status)
{
    {
        std::string reason;
        if (!IsAllowedUrl(url, reason))
        {
            if (status) status("Refusing to download " + url + ": " + reason);
            return false;
        }
    }

    // Redirects are followed with a loop and a hop budget. The previous version
    // recursed into DownloadFile for every hop with no cap, so a server pointing
    // Location: at itself walked the stack until the process died — each frame
    // carries an 8 KB read buffer plus two 4 KB wide-char arrays, so roughly
    // sixty hops was enough.
    std::string currentUrl = url;
    bool startedSecure = false;

    for (int hop = 0; hop <= kMaxRedirects; ++hop)
    {
        std::string host;

        // Convert URL to wide string
        std::wstring wUrl(currentUrl.begin(), currentUrl.end());

        // Parse URL using WinHTTP's URL_COMPONENTS (wide string version)
        URL_COMPONENTS urlComp;
        ZeroMemory(&urlComp, sizeof(urlComp));
        urlComp.dwStructSize = sizeof(urlComp);

        wchar_t hostName[256] = { 0 };
        wchar_t urlPath[2048] = { 0 };

        urlComp.lpszHostName = hostName;
        urlComp.dwHostNameLength = sizeof(hostName) / sizeof(wchar_t);
        urlComp.lpszUrlPath = urlPath;
        urlComp.dwUrlPathLength = sizeof(urlPath) / sizeof(wchar_t);

        if (!WinHttpCrackUrl(wUrl.c_str(), 0, 0, &urlComp))
        {
            if (status) status("Failed to parse URL: " + currentUrl);
            return false;
        }

        const bool isSecure = (urlComp.nScheme == INTERNET_SCHEME_HTTPS);
        if (hop == 0)
        {
            startedSecure = isSecure;
        }
        else if (startedSecure && !isSecure)
        {
            // Refusing the downgrade is the whole point: the payload is a
            // toolchain the build will execute, so allowing a redirect to drop
            // TLS hands substitution rights to anyone on the path.
            if (status) status("Refusing redirect from HTTPS to plain HTTP: " + currentUrl);
            return false;
        }

        host = WideToNarrow(hostName);
        if (status) status("Connecting to " + host + "...");

        // Open session
        HINTERNET hSession = WinHttpOpen(L"DekiEditor/1.0",
                                         WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                         WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0);

        if (!hSession)
        {
            if (status) status("Failed to open HTTP session");
            return false;
        }

        // Connect
        HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);

        if (!hConnect)
        {
            if (status) status("Failed to connect to " + host);
            WinHttpCloseHandle(hSession);
            return false;
        }

        // Open request
        DWORD flags = isSecure ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath,
                                                NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

        if (!hRequest)
        {
            if (status) status("Failed to open request to " + host);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        // Send request
        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        {
            if (status) status("Failed to send request to " + host);
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        // Closes the three handles for this hop. Declared here so both the
        // redirect path and the error paths use exactly one cleanup.
        auto closeAll = [&]
        {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
        };

        // Reads Location and, if present, arms the next loop iteration.
        auto takeRedirect = [&](DWORD code) -> bool
        {
            wchar_t redirectUrl[2048] = { 0 };
            DWORD redirectUrlSize = sizeof(redirectUrl);
            if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                                     WINHTTP_HEADER_NAME_BY_INDEX, redirectUrl, &redirectUrlSize,
                                     WINHTTP_NO_HEADER_INDEX))
                return false;

            currentUrl = WideToNarrow(redirectUrl);
            if (status)
                status("Redirecting (HTTP " + std::to_string(code) + ") to " + currentUrl);
            return true;
        };

        // Receive response
        if (!WinHttpReceiveResponse(hRequest, NULL))
        {
            DWORD statusCode = 0;
            DWORD statusCodeSize = sizeof(statusCode);
            WinHttpQueryHeaders(hRequest,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);

            if (statusCode >= 300 && statusCode < 400 && takeRedirect(statusCode))
            {
                closeAll();
                continue;
            }

            if (status) status("Failed to receive response from " + host);
            closeAll();
            return false;
        }

        // Check status code
        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);

        // Handle redirect
        if (statusCode >= 300 && statusCode < 400 && takeRedirect(statusCode))
        {
            closeAll();
            continue;
        }

        if (statusCode != 200)
        {
            if (status) status("HTTP error " + std::to_string(statusCode) + " from " + host);
            closeAll();
            return false;
        }

        // Get content length
        DWORD contentLength = 0;
        DWORD contentLengthSize = sizeof(contentLength);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &contentLengthSize, WINHTTP_NO_HEADER_INDEX);

        if (status)
        {
            if (contentLength > 0)
                status("HTTP 200 OK, downloading " + FormatBytes(contentLength) + "...");
            else
                status("HTTP 200 OK, downloading (unknown size)...");
        }

        // Ensure parent directory exists
        try
        {
            std::filesystem::create_directories(std::filesystem::path(localPath).parent_path());
        }
        catch (...)
        { /* best effort: the write that follows reports its own failure */
        }

        // Open output file
        std::ofstream outFile(localPath, std::ios::binary);
        if (!outFile.is_open())
        {
            if (status) status("Failed to open output file: " + localPath);
            closeAll();
            return false;
        }

        // Read data
        char buffer[8192];
        DWORD bytesRead = 0;
        DWORD totalRead = 0;
        DWORD lastReportedMB = 0;

        while (WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0)
        {
            if (cancelFlag && cancelFlag->load())
            {
                if (status) status("Download cancelled");
                outFile.close();
                // Don't leave a truncated file behind for a caller to mistake
                // for a complete download.
                std::error_code ec;
                std::filesystem::remove(localPath, ec);
                closeAll();
                return false;
            }

            outFile.write(buffer, bytesRead);
            totalRead += bytesRead;

            // Log every 5 MB
            DWORD currentMB = totalRead / (5 * 1024 * 1024);
            if (status && contentLength > 0 && currentMB > lastReportedMB)
            {
                lastReportedMB = currentMB;
                status("Downloaded " + FormatBytes(totalRead) + " / " + FormatBytes(contentLength));
            }
        }

        outFile.close();
        closeAll();

        if (status) status("Download complete (" + FormatBytes(totalRead) + ")");

        return true;
    }

    if (status)
        status("Too many redirects (" + std::to_string(kMaxRedirects) + ") downloading " + url);
    return false;
}

bool EditorHttpUtils::ExtractZip(const std::string& zipPath, const std::string& destDir, HttpStatusCallback status)
{
    if (status) status("Extracting archive to " + destDir + "...");

    // Two layers against zip-slip. The listing check refuses any entry with a
    // ".." segment, an absolute path or a drive letter before anything is
    // written; then bsdtar (tar.exe, in every Windows since 1803) extracts,
    // and it refuses such entries on its own. PowerShell's Expand-Archive,
    // used before, has no such protection in the version Windows ships.
    {
        std::string reason;
        if (!ZipEntriesAreSafe(zipPath, reason))
        {
            if (status) status("Refusing to extract " + zipPath + ": " + reason);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);

    std::string output;
    const DWORD kUnzipTimeoutMs = 10u * 60u * 1000u;
    const int rc = RunProcessCaptureWindows({ WindowsSystemTool("tar.exe"), "-xf", zipPath, "-C", destDir }, output, kUnzipTimeoutMs);

    if (status)
    {
        if (rc == 0)
            status("Extraction complete");
        else if (rc < 0)
            status("Extraction failed to start or timed out after 10 minutes");
        else
            status("Extraction failed (exit code " + std::to_string(rc) + "): " + output);
    }
    return rc == 0;
}

#else
// Linux/macOS implementations

bool EditorHttpUtils::DownloadFile(const std::string& url, const std::string& localPath,
                                   std::atomic<bool>* cancelFlag, HttpStatusCallback status)
{
    (void)cancelFlag;
    {
        std::string reason;
        if (!IsAllowedUrl(url, reason))
        {
            if (status) status("Refusing to download " + url + ": " + reason);
            return false;
        }
    }
    if (status) status("Downloading with curl: " + url);

    // --proto '=https' --proto-redir '=https' keep both the initial request and
    // every redirect on TLS, matching the Windows path's refusal to downgrade.
    // --max-redirs bounds the hop count the same way.
    std::vector<std::string> args = {
        "curl", "-fL", "--proto", "=https", "--proto-redir", "=https",
        "--max-redirs", std::to_string(kMaxRedirects),
        "-o", localPath, "--", url
    };
    bool ok = RunProcess(args) == 0;
    if (!ok)
    {
        std::error_code ec;
        std::filesystem::remove(localPath, ec);
    }
    if (status) status(ok ? "Download complete" : "Download failed");
    return ok;
}

bool EditorHttpUtils::ExtractZip(const std::string& zipPath, const std::string& destDir, HttpStatusCallback status)
{
    if (status) status("Extracting archive to " + destDir + "...");
    {
        std::string reason;
        if (!ZipEntriesAreSafe(zipPath, reason))
        {
            if (status) status("Refusing to extract " + zipPath + ": " + reason);
            return false;
        }
    }
    // Argument vector, not a shell string: a quote in either path used to end
    // the quoting and hand the rest to /bin/sh. unzip strips "../" itself as
    // well; the listing check above is what refuses instead of silently
    // rewriting.
    bool ok = RunProcess({ "unzip", "-o", "--", zipPath, "-d", destDir }) == 0;
    if (status) status(ok ? "Extraction complete" : "Extraction failed");
    return ok;
}
#endif

}  // namespace DekiEditor
