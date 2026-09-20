#include "McpServer.h"
#include "McpToolRegistry.h"
#include "McpTools.h"
#include "McpResources.h"

#include <httplib.h>
#include <future>
#include <random>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdlib>

#include <deki/LogSystem.h>

namespace DekiEditor
{

namespace
{
// See the wait_for() call in Start() for why this is minutes rather than seconds.
constexpr int kRequestTimeoutSeconds = 900;

// Loopback only. The MCP surface is the whole editor - install_package clones and
// rebuild compiles, so reaching it is equivalent to running code on this machine.
// Binding every interface put that behind nothing more than being on the same
// network. Clients that genuinely need remote access should forward the port
// deliberately (SSH, etc.) rather than have the editor publish it by default.
constexpr const char* kBindAddress = "127.0.0.1";

// 256 bits of CSPRNG output, hex encoded.
//
// DEKI_MCP_TOKEN overrides it. A per-session token cannot be written into a
// client configuration that outlives the session, so a developer who wants a
// stable one sets the env var for both the editor and the client instead
// (SETUP.md section 8). Anything shorter than 16
// characters is refused rather than silently accepted - a weak fixed token on a
// surface that can execute code is worse than the rotating one it replaces.
std::string GenerateSessionToken()
{
    if (const char* fromEnv = std::getenv("DEKI_MCP_TOKEN"))
    {
        const std::string token(fromEnv);
        if (token.size() >= 16)
            return token;
        DEKI_LOG_WARNING("MCP: ignoring DEKI_MCP_TOKEN (needs at least 16 characters); "
                         "generating a session token instead");
    }

    std::random_device rd;
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < 8; ++i)
        out << std::setw(8) << rd();
    return out.str();
}

// Constant-time compare so a caller can't recover the token by timing guesses.
bool SecureEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}
}  // namespace

McpServer::McpServer(EditorApp* editor)
    : m_Editor(editor), m_ToolRegistry(std::make_unique<McpToolRegistry>())
{
    RegisterAllTools(*m_ToolRegistry, m_Editor);
}

McpServer::~McpServer()
{
    Stop();
}

std::string McpServer::GetSseUrl() const
{
    // The one place the token appears in a URL: the SSE stream is opened by
    // EventSource-style clients that cannot set headers. Never log this
    // string; it is handed to the clipboard and printed once for --mcp.
    return "http://127.0.0.1:" + std::to_string(m_Port) + "/sse?token=" + m_Token;
}

bool McpServer::Authorize(const httplib::Request& req, httplib::Response& res) const
{
    // DNS rebinding: a page can resolve its own hostname to 127.0.0.1 and then
    // talk to this port as "attacker.example". The Host header names what the
    // client thinks it is talking to, and only the loopback spellings are
    // accepted.
    {
        const std::string host = req.get_header_value("Host");
        const std::string port = std::to_string(m_Port);
        const bool ok = host == "127.0.0.1:" + port || host == "localhost:" + port ||
                        host == "[::1]:" + port || host == "127.0.0.1" || host == "localhost";
        if (!ok)
        {
            res.status = 403;
            res.set_content(MakeError(nullptr, -32001, "Host header is not the loopback address").dump(),
                            "application/json");
            return false;
        }
    }

    // A browser can be made to POST to localhost by any page the user visits, so
    // reject anything carrying an Origin outright - a legitimate MCP client is
    // not a web page and never sends one. This is what stops a visited site from
    // driving the editor even while the user has the server running.
    if (req.has_header("Origin"))
    {
        res.status = 403;
        res.set_content(MakeError(nullptr, -32001, "Cross-origin requests are not accepted").dump(),
                        "application/json");
        return false;
    }

    std::string presented;
    if (req.has_header("Authorization"))
    {
        const std::string auth = req.get_header_value("Authorization");
        constexpr const char* kPrefix = "Bearer ";
        if (auth.rfind(kPrefix, 0) == 0)
            presented = auth.substr(std::strlen(kPrefix));
    }
    // The SSE stream is opened by an EventSource-style client that cannot set
    // headers, so the token is also accepted as a query parameter there.
    if (presented.empty() && req.has_param("token"))
        presented = req.get_param_value("token");

    if (!SecureEquals(presented, m_Token))
    {
        res.status = 401;
        res.set_header("WWW-Authenticate", "Bearer");
        res.set_content(MakeError(nullptr, -32002, "Missing or invalid session token").dump(),
                        "application/json");
        return false;
    }

    return true;
}

void McpServer::Start(int port)
{
    if (m_Running.load()) return;

    m_Port = port;
    m_Token = GenerateSessionToken();
    m_Running.store(true);

    m_HttpServer = std::make_unique<httplib::Server>();

    // httplib defaults to a 5s socket timeout, which would drop the connection
    // out from under a long tool call (package install, rebuild) before the
    // handler ever gets to write a response.
    m_HttpServer->set_read_timeout(kRequestTimeoutSeconds, 0);
    m_HttpServer->set_write_timeout(kRequestTimeoutSeconds, 0);

    // POST /message - receive JSON-RPC requests from MCP client
    m_HttpServer->Post("/message", [this](const httplib::Request& req, httplib::Response& res)
                       {
        if (!Authorize(req, res))
            return;

        try
        {
            auto msg = nlohmann::json::parse(req.body);

            // Check if this is a notification (no id field) - process directly
            if (!msg.contains("id"))
            {
                // Notifications don't need responses. Queue for main thread processing anyway.
                PendingRequest pending;
                pending.message = std::move(msg);
                m_IncomingRequests.Push(std::move(pending));
                res.status = 202;
                return;
            }

            // Create promise/future pair for synchronous response
            auto promise = std::make_shared<std::promise<std::string>>();
            auto future = promise->get_future();

            PendingRequest pending;
            pending.message = std::move(msg);
            pending.responsePromise = promise;
            m_IncomingRequests.Push(std::move(pending));

            // Wait for the main thread to process. Handlers run inline on the
            // main thread, and some of them are genuinely long: a package install
            // is a git clone, `rebuild` is a full CMake build. Those would trip
            // a short timeout even though the work completes fine, so the cap
            // here is a liveness backstop, not a latency budget. Clients that
            // want to bail sooner should use their own request timeout.
            if (future.wait_for(std::chrono::seconds(kRequestTimeoutSeconds)) == std::future_status::ready)
            {
                std::string responseStr = future.get();
                res.set_content(responseStr, "application/json");

                // Also push to SSE stream
                SseResponse sseResp;
                sseResp.jsonStr = responseStr;
                m_SseResponses.Push(std::move(sseResp));
            }
            else
            {
                auto errJson = MakeError(nullptr, -32603, "Request timed out");
                res.set_content(errJson.dump(), "application/json");
            }
        }
        catch (const std::exception& e)
        {
            auto errJson = MakeError(nullptr, -32700, std::string("Parse error: ") + e.what());
            res.set_content(errJson.dump(), "application/json");
        } });

    // GET /sse - Server-Sent Events stream for MCP client
    m_HttpServer->Get("/sse", [this](const httplib::Request& req, httplib::Response& res)
                      {
        if (!Authorize(req, res))
            return;

        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        // No Access-Control-Allow-Origin: browsers are not clients of this
        // server, and advertising a wildcard invited exactly the cross-origin
        // access Authorize() now rejects.

        res.set_content_provider(
            "text/event-stream",
            [this](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                // Send endpoint event first
                std::string endpointEvent = "event: endpoint\ndata: /message\n\n";
                sink.write(endpointEvent.c_str(), endpointEvent.size());

                // Keep streaming SSE events
                while (m_Running.load())
                {
                    SseResponse resp;
                    if (m_SseResponses.WaitPop(resp, 1000))
                    {
                        std::string sseEvent = "event: message\ndata: " + resp.jsonStr + "\n\n";
                        if (!sink.write(sseEvent.c_str(), sseEvent.size()))
                            return false;  // Client disconnected
                    }
                    else
                    {
                        // Send keepalive comment every second of inactivity
                        std::string keepalive = ": keepalive\n\n";
                        if (!sink.write(keepalive.c_str(), keepalive.size()))
                            return false;
                    }
                }
                return false;  // Server stopping
            },
            [](bool) {}  // Content provider done callback
        ); });

    // Start server thread
    m_ServerThread = std::thread(&McpServer::ServerThread, this);
}

void McpServer::Stop()
{
    if (!m_Running.load()) return;

    m_Running.store(false);

    if (m_HttpServer)
        m_HttpServer->stop();

    if (m_ServerThread.joinable())
        m_ServerThread.join();

    m_HttpServer.reset();
    // Never reuse a token across sessions.
    m_Token.clear();
}

void McpServer::ServerThread()
{
    m_HttpServer->listen(kBindAddress, m_Port);
}

void McpServer::ProcessMessages()
{
    PendingRequest req;
    // Process up to 10 messages per frame to avoid blocking UI
    int processed = 0;
    // Count check FIRST: with TryPop first, the 11th message was popped and
    // then dropped on the loop exit, its promise never set - the client waited
    // out the full request timeout.
    while (processed < 10 && m_IncomingRequests.TryPop(req))
    {
        auto response = HandleMessage(req.message);

        if (req.responsePromise)
        {
            req.responsePromise->set_value(response.dump());
        }

        ++processed;
    }
}

nlohmann::json McpServer::HandleMessage(const nlohmann::json& msg)
{
    if (!msg.contains("method") || !msg["method"].is_string())
        return MakeError(msg.value("id", nlohmann::json()), -32600, "Invalid request: missing method");

    std::string method = msg["method"];
    auto id = msg.value("id", nlohmann::json());
    auto params = msg.value("params", nlohmann::json::object());

    if (method == "initialize")
        return MakeResponse(id, HandleInitialize(params));
    else if (method == "notifications/initialized")
        return nlohmann::json();  // No response for notifications
    else if (method == "tools/list")
        return MakeResponse(id, HandleToolsList());
    else if (method == "tools/call")
        return MakeResponse(id, HandleToolsCall(params));
    else if (method == "resources/list")
        return MakeResponse(id, HandleResourcesList());
    else if (method == "resources/read")
        return MakeResponse(id, HandleResourcesRead(params));
    else if (method == "ping")
        return MakeResponse(id, nlohmann::json::object());
    else
        return MakeError(id, -32601, "Method not found: " + method);
}

nlohmann::json McpServer::HandleInitialize(const nlohmann::json& /*params*/)
{
    return {
        { "protocolVersion", "2024-11-05" },
        { "capabilities", { { "tools", nlohmann::json::object() }, { "resources", nlohmann::json::object() } } },
        { "serverInfo", { { "name", "deki-editor" }, { "version", "1.1.0" } } }
    };
}

nlohmann::json McpServer::HandleToolsList()
{
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& tool : m_ToolRegistry->GetTools())
    {
        tools.push_back({ { "name", tool.name },
                          { "description", tool.description },
                          { "inputSchema", tool.inputSchema } });
    }
    return { { "tools", tools } };
}

nlohmann::json McpServer::HandleToolsCall(const nlohmann::json& params)
{
    std::string toolName = params.value("name", "");
    auto arguments = params.value("arguments", nlohmann::json::object());

    const auto tool = m_ToolRegistry->FindTool(toolName);
    if (!tool)
    {
        return {
            { "content", nlohmann::json::array({ { { "type", "text" },
                                                   { "text", "Unknown tool: " + toolName } } }) },
            { "isError", true }
        };
    }

    try
    {
        nlohmann::json result = tool->handler(arguments);
        return {
            { "content", nlohmann::json::array({ { { "type", "text" },
                                                   { "text", result.dump() } } }) }
        };
    }
    catch (const std::exception& e)
    {
        return {
            { "content", nlohmann::json::array({ { { "type", "text" },
                                                   { "text", std::string("Error: ") + e.what() } } }) },
            { "isError", true }
        };
    }
}

nlohmann::json McpServer::HandleResourcesList()
{
    return { { "resources", ListResources() } };
}

nlohmann::json McpServer::HandleResourcesRead(const nlohmann::json& params)
{
    std::string uri = params.value("uri", "");
    auto result = HandleResourceRead(uri, m_Editor);
    return {
        { "contents", nlohmann::json::array({ { { "uri", uri },
                                                { "mimeType", "application/json" },
                                                { "text", result.dump() } } }) }
    };
}

nlohmann::json McpServer::MakeResponse(const nlohmann::json& id, const nlohmann::json& result)
{
    return {
        { "jsonrpc", "2.0" },
        { "id", id },
        { "result", result }
    };
}

nlohmann::json McpServer::MakeError(const nlohmann::json& id, int code, const std::string& message)
{
    return {
        { "jsonrpc", "2.0" },
        { "id", id },
        { "error", { { "code", code }, { "message", message } } }
    };
}

}  // namespace DekiEditor
