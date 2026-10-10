#include "McpServer.h"
#include "McpCompletion.h"
#include "McpToolRegistry.h"
#include "McpTools.h"
#include "McpResources.h"
#include "../EditorApp.h"
#include "../core/Project.h"
#include "../services/AgentCursor.h"

#include <deki/Object.h>
#include <deki/Scene.h>

#include <httplib.h>
#include <filesystem>
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

// How long a tool's deferred work (McpCompletion) may take before its call
// fails rather than hangs. A screenshot or a click takes a few frames; a
// minute only fires when something is stuck (a hidden window does not draw).
constexpr auto kDeferredTimeout = std::chrono::seconds(60);

nlohmann::json ToolResult(const nlohmann::json& result)
{
    return { { "content", nlohmann::json::array({ { { "type", "text" }, { "text", result.dump() } } }) } };
}

nlohmann::json ToolError(const std::string& message)
{
    return { { "content", nlohmann::json::array({ { { "type", "text" }, { "text", "Error: " + message } } }) },
             { "isError", true } };
}

// Loopback only. MCP reaches the whole editor (install_package clones and
// rebuild compiles), so reaching it means running code on this machine; on
// every interface, anyone on the same network could. A client that needs
// remote access should forward the port on purpose (SSH, etc.).
constexpr const char* kBindAddress = "127.0.0.1";

// 256 bits of CSPRNG output, hex encoded.
//
// DEKI_MCP_TOKEN overrides it. A per-session token cannot go into a client
// configuration that outlives the session, so a developer who wants a stable
// one sets the variable for both the editor and the client (SETUP.md section
// 8). Anything shorter than 16 characters is refused: a weak fixed token on a
// surface that can run code is worse than the per-session one.
std::string GenerateSessionToken()
{
    if (const char* fromEnv = std::getenv("DEKI_MCP_TOKEN"))
    {
        const std::string token(fromEnv);
        if (token.size() >= 16)
        {
            return token;
        }
        DEKI_LOG_WARNING("MCP: ignoring DEKI_MCP_TOKEN (needs at least 16 characters); "
                         "generating a session token instead");
    }

    std::random_device rd;
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < 8; ++i)
    {
        out << std::setw(8) << rd();
    }
    return out.str();
}

// Constant-time compare so a caller can't recover the token by timing guesses.
bool SecureEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}
}  // namespace

McpServer::McpServer(EditorApp* editor)
    : m_Editor(editor),
      m_ToolRegistry(std::make_unique<McpToolRegistry>())
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
        const bool ok = host == "127.0.0.1:" + port || host == "localhost:" + port || host == "[::1]:" + port ||
                        host == "127.0.0.1" || host == "localhost";
        if (!ok)
        {
            res.status = 403;
            res.set_content(MakeError(nullptr, -32001, "Host header is not the loopback address").dump(),
                            "application/json");
            return false;
        }
    }

    // Any page the user visits can make the browser POST to localhost, so
    // anything carrying an Origin is rejected: a real MCP client is not a web
    // page and never sends one. This stops a visited site from driving the
    // editor while the server is running.
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
        {
            presented = auth.substr(std::strlen(kPrefix));
        }
    }
    // The SSE stream is opened by an EventSource-style client that cannot set
    // headers, so the token is also accepted as a query parameter there.
    if (presented.empty() && req.has_param("token"))
    {
        presented = req.get_param_value("token");
    }

    if (!SecureEquals(presented, m_Token))
    {
        res.status = 401;
        res.set_header("WWW-Authenticate", "Bearer");
        res.set_content(MakeError(nullptr, -32002, "Missing or invalid session token").dump(), "application/json");
        return false;
    }

    return true;
}

bool McpServer::Start(int port)
{
    if (m_Running.load())
    {
        return true;
    }

    m_Token = GenerateSessionToken();
    m_HttpServer = std::make_unique<httplib::Server>();

    // httplib's default 5s socket timeout would drop the connection during a
    // long tool call (package install, rebuild) before the handler can write
    // a response.
    m_HttpServer->set_read_timeout(kRequestTimeoutSeconds, 0);
    m_HttpServer->set_write_timeout(kRequestTimeoutSeconds, 0);

    // POST /message: JSON-RPC requests from the MCP client.
    m_HttpServer->Post(
        "/message",
        [this](const httplib::Request& req, httplib::Response& res)
        {
            if (!Authorize(req, res))
            {
                return;
            }

            try
            {
                auto msg = nlohmann::json::parse(req.body);

                // A notification (no id) gets no response, but is still queued
                // for the main thread.
                if (!msg.contains("id"))
                {
                    PendingRequest pending;
                    pending.message = std::move(msg);
                    m_IncomingRequests.Push(std::move(pending));
                    res.status = 202;
                    return;
                }

                // The main thread fulfils the promise with the response.
                auto promise = std::make_shared<std::promise<std::string>>();
                auto future = promise->get_future();

                PendingRequest pending;
                pending.message = std::move(msg);
                pending.responsePromise = promise;
                m_IncomingRequests.Push(std::move(pending));

                // Wait for the main thread. Handlers run there, and some take long:
                // a package install is a git clone, `rebuild` is a full CMake build.
                // So the cap here only guards against a hang; clients that want to
                // give up sooner should use their own request timeout.
                //
                // Waited in short steps so a stopping server lets go at once:
                // Stop() joins this thread, and a request that arrived as the
                // editor was closing would otherwise hold the quit for 15 minutes.
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kRequestTimeoutSeconds);
                std::future_status status = std::future_status::timeout;
                while (m_Running.load() && std::chrono::steady_clock::now() < deadline)
                {
                    status = future.wait_for(std::chrono::milliseconds(250));
                    if (status == std::future_status::ready)
                    {
                        break;
                    }
                }
                if (status != std::future_status::ready && !m_Running.load() &&
                    future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                {
                    status = std::future_status::ready;  // answered by Stop()
                }
                if (status == std::future_status::ready)
                {
                    std::string responseStr = future.get();
                    res.set_content(responseStr, "application/json");

                    // Also push to the SSE stream, if one is listening.
                    if (m_SseClients.load() > 0)
                    {
                        SseResponse sseResp;
                        sseResp.jsonStr = responseStr;
                        m_SseResponses.Push(std::move(sseResp));
                    }
                }
                else
                {
                    auto errJson =
                        MakeError(nullptr, -32603, m_Running.load() ? "Request timed out" : "The editor is closing");
                    res.set_content(errJson.dump(), "application/json");
                }
            }
            catch (const std::exception& e)
            {
                auto errJson = MakeError(nullptr, -32700, std::string("Parse error: ") + e.what());
                res.set_content(errJson.dump(), "application/json");
            }
        });

    // GET /sse: Server-Sent Events stream for the MCP client.
    m_HttpServer->Get("/sse",
                      [this](const httplib::Request& req, httplib::Response& res)
                      {
                          if (!Authorize(req, res))
                          {
                              return;
                          }

                          res.set_header("Content-Type", "text/event-stream");
                          res.set_header("Cache-Control", "no-cache");
                          res.set_header("Connection", "keep-alive");
                          // No Access-Control-Allow-Origin: browsers are not clients of this
                          // server, and a wildcard would invite the cross-origin access
                          // Authorize() rejects.

                          res.set_content_provider(
                              "text/event-stream",
                              [this](size_t /*offset*/, httplib::DataSink& sink) -> bool
                              {
                                  struct Listening
                                  {
                                      std::atomic<int>& n;
                                      explicit Listening(std::atomic<int>& c)
                                          : n(c)
                                      {
                                          ++n;
                                      }
                                      ~Listening() { --n; }
                                  } listening(m_SseClients);
                                  // The endpoint event comes first.
                                  std::string endpointEvent = "event: endpoint\ndata: /message\n\n";
                                  sink.write(endpointEvent.c_str(), endpointEvent.size());

                                  while (m_Running.load())
                                  {
                                      SseResponse resp;
                                      if (m_SseResponses.WaitPop(resp, 1000))
                                      {
                                          std::string sseEvent = "event: message\ndata: " + resp.jsonStr + "\n\n";
                                          if (!sink.write(sseEvent.c_str(), sseEvent.size()))
                                          {
                                              return false;  // Client disconnected
                                          }
                                      }
                                      else
                                      {
                                          // A keepalive comment after each idle second.
                                          std::string keepalive = ": keepalive\n\n";
                                          if (!sink.write(keepalive.c_str(), keepalive.size()))
                                          {
                                              return false;
                                          }
                                      }
                                  }
                                  return false;  // Server stopping
                              },
                              [](bool) {}  // Content provider done callback
                          );
                      });

    // Bind here, on the caller's thread: listen() on the server thread fails
    // silently when the port is taken, leaving a server that says it is running
    // and never answers. Every windowed editor starts one, so they take any
    // free port unless one is asked for.
    if (port == 0)
    {
        m_Port = m_HttpServer->bind_to_any_port(kBindAddress);
    }
    else
    {
        m_Port = port;
        if (!m_HttpServer->bind_to_port(kBindAddress, port))
        {
            m_Port = -1;
        }
    }
    if (m_Port <= 0)
    {
        DEKI_LOG_ERROR("MCP: could not listen on %s:%d (in use?)", kBindAddress, port);
        m_HttpServer.reset();
        m_Token.clear();
        m_Port = 0;
        return false;
    }

    m_Running.store(true);
    m_ServerThread = std::thread(&McpServer::ServerThread, this);
    return true;
}

void McpServer::Stop()
{
    if (!m_Running.load())
    {
        return;
    }

    m_Running.store(false);

    // Answer what is still queued: the main thread will not get to it now, and
    // each one has a server thread waiting on it (see the /message handler).
    PendingRequest req;
    if (m_Parked)
    {
        req = std::move(*m_Parked);
        m_Parked.reset();
        if (req.responsePromise)
        {
            req.responsePromise->set_value(
                MakeError(req.message.value("id", nlohmann::json()), -32603, "The editor is closing").dump());
        }
    }
    while (m_IncomingRequests.TryPop(req))
    {
        if (req.responsePromise)
        {
            req.responsePromise->set_value(
                MakeError(req.message.value("id", nlohmann::json()), -32603, "The editor is closing").dump());
        }
    }
    for (DeferredCall& call : m_Deferred)
    {
        call.responsePromise->set_value(MakeError(call.id, -32603, "The editor is closing").dump());
    }
    m_Deferred.clear();

    if (m_HttpServer)
    {
        m_HttpServer->stop();
    }

    if (m_ServerThread.joinable())
    {
        m_ServerThread.join();
    }

    m_HttpServer.reset();
    // Never reuse a token across sessions.
    m_Token.clear();
}

void McpServer::ServerThread()
{
    m_HttpServer->listen_after_bind();
}

void McpServer::ProcessMessages()
{
    // Calls whose work has finished since the last frame are answered first.
    for (size_t i = 0; i < m_Deferred.size();)
    {
        DeferredCall& call = m_Deferred[i];
        nlohmann::json result;
        bool done = true;
        try
        {
            if (std::optional<nlohmann::json> finished = call.poll())
            {
                result = ToolResult(*finished);
            }
            else if (std::chrono::steady_clock::now() - call.started > kDeferredTimeout)
            {
                result = ToolError("not finished after " + std::to_string(kDeferredTimeout.count()) + " s");
            }
            else
            {
                done = false;
            }
        }
        catch (const std::exception& e)
        {
            result = ToolError(e.what());
        }
        if (!done)
        {
            ++i;
            continue;
        }
        call.responsePromise->set_value(MakeResponse(call.id, result).dump());
        m_Deferred.erase(m_Deferred.begin() + static_cast<std::ptrdiff_t>(i));
    }

    // At most 10 messages per frame, so the UI keeps running.
    int processed = 0;
    // The count must be checked before TryPop: otherwise the 11th message is
    // popped and dropped on loop exit, its promise never set, and the client
    // waits out the full request timeout.
    while (processed < 10)
    {
        PendingRequest req;
        if (m_Parked)
        {
            // Held for the agent pointer: it runs once the pointer is there, and
            // until then nothing queued behind it does.
            if (!AgentCursor::Get().Arrived())
            {
                return;
            }
            req = std::move(*m_Parked);
            m_Parked.reset();
        }
        else
        {
            if (!m_IncomingRequests.TryPop(req))
            {
                return;
            }
            if (ShowOnAgentPointer(req.message))
            {
                m_Parked = std::move(req);
                continue;  // runs on a later frame, or now if the pointer has nowhere to go
            }
        }

        auto response = HandleMessage(req.message);

        // A tool that finishes on a later frame is answered once it has
        // (above, on a later call); its queued reply is not sent.
        std::optional<McpCompletion::Poll> poll = std::move(m_LastCallDeferral);
        m_LastCallDeferral.reset();
        if (poll && req.responsePromise)
        {
            m_Deferred.push_back({ req.message.value("id", nlohmann::json()), req.responsePromise, std::move(*poll),
                                   std::chrono::steady_clock::now() });
        }
        else if (req.responsePromise)
        {
            req.responsePromise->set_value(response.dump());
        }

        ++processed;
    }
}

bool McpServer::ShowOnAgentPointer(const nlohmann::json& msg)
{
    if (!AgentCursor::IsEnabled() || msg.value("method", "") != "tools/call")
    {
        return false;
    }
    const nlohmann::json params = msg.value("params", nlohmann::json::object());
    const nlohmann::json args = params.value("arguments", nlohmann::json::object());
    AgentIntent intent = IntentForTool(params.value("name", ""), args.is_object() ? args : nlohmann::json::object());
    if (!intent.Shows())
    {
        return false;
    }

    // What the mapping cannot know without the editor: the object's name for
    // the label, and the asset's absolute path, which the Asset Browser's rows
    // hold.
    if (const size_t at = intent.label.find("{object}"); at != std::string::npos)
    {
        std::string name = "object " + std::to_string(intent.objectId);
        Deki::Scene* scene = m_Editor ? m_Editor->GetCurrentScene() : nullptr;
        if (Deki::Object* obj = (scene && intent.hasObject) ? scene->FindDekiObject(intent.objectId) : nullptr)
        {
            name = "'" + obj->GetName() + "'";
        }
        intent.label.replace(at, 8, name);
    }
    if (!intent.assetPath.empty())
    {
        Project* project = m_Editor ? m_Editor->GetProject() : nullptr;
        intent.assetPath = project ? (std::filesystem::path(project->GetPath()) / intent.assetPath).string() : "";
        if (intent.assetPath.empty())
        {
            intent.target = AgentIntent::Target::Panel;
        }
    }

    // Input into a running game is shown but not held: a pause before each tap
    // would change what is being tested (timing, double taps, a held drag).
    const bool playing = m_Editor && !m_Editor->IsStopped();
    const bool wait = intent.Travels() && !(playing && intent.target == AgentIntent::Target::Point);
    AgentCursor::Get().Show(intent, wait);
    return wait;
}

nlohmann::json McpServer::HandleMessage(const nlohmann::json& msg)
{
    if (!msg.contains("method") || !msg["method"].is_string())
    {
        return MakeError(msg.value("id", nlohmann::json()), -32600, "Invalid request: missing method");
    }

    std::string method = msg["method"];
    auto id = msg.value("id", nlohmann::json());
    auto params = msg.value("params", nlohmann::json::object());

    if (method == "initialize")
    {
        return MakeResponse(id, HandleInitialize(params));
    }
    else if (method == "notifications/initialized")
    {
        return nlohmann::json();  // No response for notifications
    }
    else if (method == "tools/list")
    {
        return MakeResponse(id, HandleToolsList());
    }
    else if (method == "tools/call")
    {
        return MakeResponse(id, HandleToolsCall(params));
    }
    else if (method == "resources/list")
    {
        return MakeResponse(id, HandleResourcesList());
    }
    else if (method == "resources/read")
    {
        return MakeResponse(id, HandleResourcesRead(params));
    }
    else if (method == "ping")
    {
        return MakeResponse(id, nlohmann::json::object());
    }
    else
    {
        return MakeError(id, -32601, "Method not found: " + method);
    }
}

nlohmann::json McpServer::HandleInitialize(const nlohmann::json& /*params*/)
{
    return { { "protocolVersion", "2024-11-05" },
             { "capabilities", { { "tools", nlohmann::json::object() }, { "resources", nlohmann::json::object() } } },
             { "serverInfo", { { "name", "deki-editor" }, { "version", "1.1.0" } } } };
}

nlohmann::json McpServer::HandleToolsList()
{
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& tool : m_ToolRegistry->GetTools())
    {
        tools.push_back(
            { { "name", tool.name }, { "description", tool.description }, { "inputSchema", tool.inputSchema } });
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
        return { { "content",
                   nlohmann::json::array({ { { "type", "text" }, { "text", "Unknown tool: " + toolName } } }) },
                 { "isError", true } };
    }

    if (const std::string missing = MissingRequiredArguments(*tool, arguments); !missing.empty())
    {
        return ToolError(toolName + " needs " + missing);
    }

    McpCompletion::Take();  // a deferral left by a handler run elsewhere is not this call's
    try
    {
        nlohmann::json result = tool->handler(arguments);
        m_LastCallDeferral = McpCompletion::Take();
        return ToolResult(result);
    }
    catch (const std::exception& e)
    {
        McpCompletion::Take();
        return ToolError(e.what());
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
    return { { "contents",
               nlohmann::json::array(
                   { { { "uri", uri }, { "mimeType", "application/json" }, { "text", result.dump() } } }) } };
}

nlohmann::json McpServer::MakeResponse(const nlohmann::json& id, const nlohmann::json& result)
{
    return { { "jsonrpc", "2.0" }, { "id", id }, { "result", result } };
}

nlohmann::json McpServer::MakeError(const nlohmann::json& id, int code, const std::string& message)
{
    return { { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", code }, { "message", message } } } };
}

}  // namespace DekiEditor
