#pragma once

#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <nlohmann/json.hpp>

namespace httplib
{
class Server;
struct Request;
struct Response;
}  // namespace httplib

namespace DekiEditor
{

class EditorApp;
class McpToolRegistry;

// Thread-safe queue for passing messages between HTTP thread and main thread
template <typename T>
class ThreadSafeQueue
{
public:
    void Push(T item)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Queue.push(std::move(item));
        m_Condition.notify_one();
    }

    bool TryPop(T& item)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Queue.empty())
        {
            return false;
        }
        item = std::move(m_Queue.front());
        m_Queue.pop();
        return true;
    }

    // Blocking pop with timeout (returns false if timed out)
    bool WaitPop(T& item, int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        if (!m_Condition.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !m_Queue.empty(); }))
        {
            return false;
        }
        item = std::move(m_Queue.front());
        m_Queue.pop();
        return true;
    }

private:
    std::queue<T> m_Queue;
    mutable std::mutex m_Mutex;
    std::condition_variable m_Condition;
};

// Pending SSE response: pairs a response JSON string with its request ID
// so the SSE stream can route responses back to the correct client session
struct SseResponse
{
    std::string jsonStr;
};

class McpServer
{
public:
    McpServer(EditorApp* editor);
    ~McpServer();

    // Binds before returning, so a port already in use is a false here rather
    // than a server that never answers. Port 0 takes any free one; GetPort()
    // says which.
    bool Start(int port = 0);
    void Stop();
    bool IsRunning() const { return m_Running.load(); }
    int GetPort() const { return m_Port; }

    // Per-session bearer token, minted on Start(). Clients must send it as
    // `Authorization: Bearer <token>` (or `?token=` on the SSE stream).
    // Surfaced so the Activity panel and the startup banner can show the URL a
    // client needs.
    const std::string& GetToken() const { return m_Token; }
    std::string GetSseUrl() const;

    // Called from main thread each frame to process queued requests
    void ProcessMessages();

private:
    void ServerThread();

    // True when the request carries the session token and does not look like a
    // cross-origin browser request. Writes a 401/403 into `res` when it fails.
    bool Authorize(const httplib::Request& req, httplib::Response& res) const;

    // JSON-RPC dispatch
    nlohmann::json HandleMessage(const nlohmann::json& msg);
    nlohmann::json HandleInitialize(const nlohmann::json& params);
    nlohmann::json HandleToolsList();
    nlohmann::json HandleToolsCall(const nlohmann::json& params);
    nlohmann::json HandleResourcesList();
    nlohmann::json HandleResourcesRead(const nlohmann::json& params);

    // JSON-RPC helpers
    static nlohmann::json MakeResponse(const nlohmann::json& id, const nlohmann::json& result);
    static nlohmann::json MakeError(const nlohmann::json& id, int code, const std::string& message);

    EditorApp* m_Editor;
    int m_Port = 3000;
    std::string m_Token;
    std::atomic<bool> m_Running{ false };

    std::unique_ptr<httplib::Server> m_HttpServer;
    std::thread m_ServerThread;

    // Request queue: HTTP thread pushes, main thread pops
    struct PendingRequest
    {
        nlohmann::json message;
        // Promise to deliver the response back to the HTTP handler
        std::shared_ptr<std::promise<std::string>> responsePromise;
    };
    ThreadSafeQueue<PendingRequest> m_IncomingRequests;

    // SSE response queue: main thread pushes, SSE stream sends
    ThreadSafeQueue<SseResponse> m_SseResponses;
    // Open /sse streams. Responses are queued for them only while one is
    // open: the CLI talks to a live editor with plain POSTs, and every reply
    // used to be kept for a stream that never connected, for the whole session.
    std::atomic<int> m_SseClients{ 0 };

    std::unique_ptr<McpToolRegistry> m_ToolRegistry;
};

}  // namespace DekiEditor
