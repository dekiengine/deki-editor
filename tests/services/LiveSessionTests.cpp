// LiveSession: how the command line finds and drives a running editor.
//
// Covers the session files (what an editor writes, what the finder keeps and
// what it throws away) and runs the client against a stand-in for the
// editor's /message endpoint that answers the way McpServer does.

#include <gtest/gtest.h>

#include "services/LiveSession.h"

#include <httplib.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace LS = DekiEditor::LiveSession;

namespace
{
long long OwnPid()
{
#ifdef _WIN32
    return static_cast<long long>(GetCurrentProcessId());
#else
    return static_cast<long long>(getpid());
#endif
}

struct TempDir
{
    fs::path path;
    TempDir()
    {
        path =
            fs::temp_directory_path() / ("deki-livesession-test-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void Write(const std::string& name, const LS::Info& info) const { std::ofstream(path / name) << LS::ToJson(info); }
};

LS::Info MakeInfo(long long pid, const std::string& project)
{
    LS::Info info;
    info.pid = pid;
    info.port = 4321;
    info.token = "0123456789abcdef0123456789abcdef";
    info.project = project;
    return info;
}

// The editor's /message endpoint, reduced to what the client relies on.
struct FakeEditor
{
    httplib::Server server;
    std::thread thread;
    int port = 0;
    std::string token = "fedcba9876543210fedcba9876543210";

    FakeEditor()
    {
        server.Post(
            "/message",
            [this](const httplib::Request& req, httplib::Response& res)
            {
                if (req.get_header_value("Authorization") != "Bearer " + token)
                {
                    res.status = 401;
                    return;
                }
                const nlohmann::json msg = nlohmann::json::parse(req.body);
                nlohmann::json result;
                if (msg["method"] == "tools/list")
                {
                    result = { { "tools", nlohmann::json::array({ { { "name", "echo" } } }) } };
                }
                else
                {
                    const std::string name = msg["params"]["name"];
                    const nlohmann::json args = msg["params"]["arguments"];
                    if (name == "echo")
                    {
                        result = { { "content",
                                     nlohmann::json::array({ { { "type", "text" }, { "text", args.dump() } } }) } };
                    }
                    else
                    {
                        result = { { "content",
                                     nlohmann::json::array({ { { "type", "text" }, { "text", "Error: it broke" } } }) },
                                   { "isError", true } };
                    }
                }
                res.set_content(
                    nlohmann::json{ { "jsonrpc", "2.0" }, { "id", msg["id"] }, { "result", result } }.dump(),
                    "application/json");
            });
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~FakeEditor()
    {
        server.stop();
        thread.join();
    }
    LS::Info Session() const
    {
        LS::Info info = MakeInfo(OwnPid(), "C:/p");
        info.port = port;
        info.token = token;
        return info;
    }
};
}  // namespace

TEST(LiveSession, SessionFileRoundTrips)
{
    const LS::Info in = MakeInfo(1234, "C:/dki/d18");
    LS::Info out;
    ASSERT_TRUE(LS::FromJson(LS::ToJson(in), out));
    EXPECT_EQ(out.pid, in.pid);
    EXPECT_EQ(out.port, in.port);
    EXPECT_EQ(out.token, in.token);
    EXPECT_EQ(out.project, in.project);
}

TEST(LiveSession, RejectsIncompleteFiles)
{
    LS::Info out;
    EXPECT_FALSE(LS::FromJson("not json", out));
    EXPECT_FALSE(LS::FromJson(R"({"pid": 1, "port": 2})", out));
    EXPECT_FALSE(LS::FromJson(R"({"pid": 1, "port": 0, "token": "x"})", out));
}

TEST(LiveSession, SameProjectIgnoresSpelling)
{
    TempDir dir;
    const std::string p = dir.path.string();
    EXPECT_TRUE(LS::SameProject(p, p + "/"));
    EXPECT_TRUE(LS::SameProject(p, (dir.path / "sub" / "..").string()));
    EXPECT_FALSE(LS::SameProject(p, (dir.path / "other").string()));
    EXPECT_FALSE(LS::SameProject("", p));  // the project list is no project
}

TEST(LiveSession, FindsTheEditorWithTheProjectOpen)
{
    TempDir sessions, project, other;
    sessions.Write("a.json", MakeInfo(OwnPid(), project.path.string()));
    sessions.Write("b.json", MakeInfo(OwnPid(), other.path.string()));

    const auto found = LS::FindForProject(sessions.path, project.path.string());
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].pid, OwnPid());
}

TEST(LiveSession, ForgetsEditorsThatExited)
{
    TempDir sessions, project;
    // No process has this id: pids are small and never this large.
    sessions.Write("gone.json", MakeInfo(2000000000LL, project.path.string()));

    EXPECT_TRUE(LS::FindForProject(sessions.path, project.path.string()).empty());
    EXPECT_FALSE(fs::exists(sessions.path / "gone.json"));
}

TEST(LiveSession, CallsAToolAndReturnsItsJson)
{
    FakeEditor editor;
    nlohmann::json result;
    std::string error;
    ASSERT_TRUE(LS::CallTool(editor.Session(), "echo", { { "x", 1 } }, result, error)) << error;
    EXPECT_EQ(result, nlohmann::json({ { "x", 1 } }));
}

TEST(LiveSession, ReportsAToolsErrorWithoutItsPrefix)
{
    FakeEditor editor;
    nlohmann::json result;
    std::string error;
    EXPECT_FALSE(LS::CallTool(editor.Session(), "boom", nlohmann::json::object(), result, error));
    EXPECT_EQ(error, "it broke");
}

TEST(LiveSession, ListsTools)
{
    FakeEditor editor;
    nlohmann::json tools;
    std::string error;
    ASSERT_TRUE(LS::ListTools(editor.Session(), tools, error)) << error;
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_EQ(tools[0]["name"], "echo");
}

TEST(LiveSession, AWrongTokenIsRefused)
{
    FakeEditor editor;
    LS::Info session = editor.Session();
    session.token = "not-the-token-not-the-token-1234";
    nlohmann::json result;
    std::string error;
    EXPECT_FALSE(LS::CallTool(session, "echo", nlohmann::json::object(), result, error));
    EXPECT_NE(error.find("401"), std::string::npos) << error;
}

TEST(LiveSession, AnEditorThatDoesNotAnswerIsAnError)
{
    LS::Info session;
    {
        FakeEditor editor;
        session = editor.Session();
    }  // stopped: nothing listens on its port now
    nlohmann::json result;
    std::string error;
    EXPECT_FALSE(LS::CallTool(session, "echo", nlohmann::json::object(), result, error));
    EXPECT_NE(error.find("did not answer"), std::string::npos) << error;
}
