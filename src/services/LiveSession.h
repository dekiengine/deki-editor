#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace DekiEditor
{

// A running editor's MCP server, as the command line finds it.
//
// Every windowed editor starts its MCP server on a free loopback port and
// publishes where it is in the user's settings folder, one file per process:
// <settings>/sessions/<pid>.json with the project it has open, the port and
// the session token. `DekiEditor --tool/--script/--list-tools <project>` looks
// there first and, when an editor has that project open, runs the tools in it
// rather than opening a second copy (which the project lock would refuse).
//
// The file sits in the per-user folder, never in the project: a project
// folder gets synced and shared, and the token is the whole of the server's
// authentication.
namespace LiveSession
{
struct Info
{
    long long pid = 0;
    int port = 0;
    std::string token;
    std::string project;  // empty while the editor shows the project list
};

std::filesystem::path Directory();

std::string ToJson(const Info& info);
bool FromJson(const std::string& text, Info& out);

// Whether two spellings name the same project folder.
bool SameProject(const std::string& a, const std::string& b);

// This process's own file. Publish again whenever the open project changes.
void Publish(int port, const std::string& token, const std::string& projectPath);
void Withdraw();

// Running editors that have `projectPath` open. Files of processes that have
// exited are removed along the way.
std::vector<Info> FindForProject(const std::string& projectPath);
std::vector<Info> FindForProject(const std::filesystem::path& dir, const std::string& projectPath);

// Talk to one. Both fail with `error` when the editor does not answer.
bool ListTools(const Info& session, nlohmann::json& tools, std::string& error);
bool CallTool(const Info& session, const std::string& name, const nlohmann::json& args,
              nlohmann::json& result, std::string& error);
}  // namespace LiveSession

}  // namespace DekiEditor
