#include "LiveSession.h"

#include <deki-editor/EditorSettings.h>
#include <deki-editor/FileIO.h>

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace DekiEditor::LiveSession
{

namespace
{
long long CurrentPid()
{
#ifdef _WIN32
    return static_cast<long long>(GetCurrentProcessId());
#else
    return static_cast<long long>(getpid());
#endif
}

bool IsAlive(long long pid)
{
    if (pid <= 0)
    {
        return false;
    }
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h)
    {
        return GetLastError() == ERROR_ACCESS_DENIED;  // exists, someone else's
    }
    DWORD code = 0;
    const bool running = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return running;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

std::string Normalized(const std::string& path)
{
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(path), ec);
    if (ec)
    {
        p = fs::path(path).lexically_normal();
    }
    std::string s = p.generic_string();
    while (s.size() > 1 && s.back() == '/')
    {
        s.pop_back();
    }
#ifdef _WIN32
    // Windows paths are case-insensitive: C:/Dki and c:/dki are one folder.
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return s;
}

// One JSON-RPC call to the editor's /message endpoint. The server answers on
// the same request, so no event stream is needed.
bool Rpc(const Info& session, const std::string& method, const nlohmann::json& params, nlohmann::json& result,
         std::string& error)
{
    httplib::Client client("127.0.0.1", session.port);
    client.set_connection_timeout(2, 0);
    // A tool can be a full rebuild; the server's own cap is 15 minutes.
    client.set_read_timeout(900, 0);
    client.set_write_timeout(30, 0);

    const nlohmann::json request = { { "jsonrpc", "2.0" }, { "id", 1 }, { "method", method }, { "params", params } };
    const httplib::Headers headers = { { "Authorization", "Bearer " + session.token } };
    const auto res = client.Post("/message", headers, request.dump(), "application/json");
    if (!res)
    {
        error = "the editor (process " + std::to_string(session.pid) + ") did not answer on port " +
                std::to_string(session.port);
        return false;
    }
    if (res->status != 200)
    {
        error = "the editor refused the request (HTTP " + std::to_string(res->status) + ")";
        return false;
    }

    const nlohmann::json reply = nlohmann::json::parse(res->body, nullptr, false);
    if (reply.is_discarded())
    {
        error = "the editor's answer is not JSON";
        return false;
    }
    if (reply.contains("error"))
    {
        error = reply["error"].value("message", std::string("unknown error"));
        return false;
    }
    result = reply.value("result", nlohmann::json::object());
    return true;
}
}  // namespace

fs::path Directory()
{
    return fs::path(EditorSettings::GetSettingsDir()) / "sessions";
}

std::string ToJson(const Info& info)
{
    return nlohmann::json{
        { "pid", info.pid }, { "port", info.port }, { "token", info.token }, { "project", info.project }
    }.dump(2);
}

bool FromJson(const std::string& text, Info& out)
{
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("pid") || !j.contains("port") || !j.contains("token"))
    {
        return false;
    }
    try
    {
        out.pid = j.at("pid").get<long long>();
        out.port = j.at("port").get<int>();
        out.token = j.at("token").get<std::string>();
        out.project = j.value("project", std::string());
    }
    catch (const std::exception&)
    {
        return false;
    }
    return out.pid > 0 && out.port > 0 && !out.token.empty();
}

bool SameProject(const std::string& a, const std::string& b)
{
    return !a.empty() && !b.empty() && Normalized(a) == Normalized(b);
}

void Publish(int port, const std::string& token, const std::string& projectPath)
{
    std::error_code ec;
    const fs::path dir = Directory();
    fs::create_directories(dir, ec);
#ifndef _WIN32
    // The file holds the MCP server's token, which is what lets anything drive
    // this editor. Written with the default umask it was readable by every
    // user on the machine. The folder is owner-only, so nothing in it can be
    // reached by others, whatever mode a file gets. (On Windows the per-user
    // profile's ACL already does this.)
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
#endif

    Info info;
    info.pid = CurrentPid();
    info.port = port;
    info.token = token;
    info.project = projectPath.empty() ? std::string() : Normalized(projectPath);

    std::string err;
    const fs::path file = dir / (std::to_string(info.pid) + ".json");
    AtomicWriteFile(file.string(), ToJson(info), err);
#ifndef _WIN32
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
#endif
}

void Withdraw()
{
    std::error_code ec;
    fs::remove(Directory() / (std::to_string(CurrentPid()) + ".json"), ec);
}

std::vector<Info> FindForProject(const std::string& projectPath)
{
    return FindForProject(Directory(), projectPath);
}

std::vector<Info> FindForProject(const fs::path& dir, const std::string& projectPath)
{
    std::vector<Info> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        if (entry.path().extension() != ".json")
        {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        Info info;
        if (!FromJson(text, info))
        {
            continue;
        }
        if (!IsAlive(info.pid))
        {
            std::error_code rec;
            fs::remove(entry.path(), rec);  // an editor that crashed
            continue;
        }
        if (SameProject(info.project, projectPath))
        {
            found.push_back(info);
        }
    }
    return found;
}

bool ListTools(const Info& session, nlohmann::json& tools, std::string& error)
{
    nlohmann::json result;
    if (!Rpc(session, "tools/list", nlohmann::json::object(), result, error))
    {
        return false;
    }
    tools = result.value("tools", nlohmann::json::array());
    return true;
}

bool CallTool(const Info& session, const std::string& name, const nlohmann::json& args, nlohmann::json& result,
              std::string& error)
{
    nlohmann::json reply;
    if (!Rpc(session, "tools/call", { { "name", name }, { "arguments", args } }, reply, error))
    {
        return false;
    }

    // MCP wraps the tool's answer as text content; the editor puts the tool's
    // JSON there, or "Error: ..." when it threw.
    std::string text;
    if (reply.contains("content") && reply["content"].is_array() && !reply["content"].empty())
    {
        text = reply["content"][0].value("text", std::string());
    }

    if (reply.value("isError", false))
    {
        const std::string prefix = "Error: ";
        error = text.rfind(prefix, 0) == 0 ? text.substr(prefix.size()) : text;
        return false;
    }

    result = nlohmann::json::parse(text, nullptr, false);
    if (result.is_discarded())
    {
        result = text;
    }
    return true;
}

}  // namespace DekiEditor::LiveSession
