#include <deki-editor/build/BuildActivitySink.h>

namespace DekiEditor
{

namespace
{
IBuildActivitySink* s_Sink = nullptr;
}

void SetBuildActivitySink(IBuildActivitySink* sink)
{
    s_Sink = sink;
}

IBuildActivitySink* GetBuildActivitySink()
{
    return s_Sink;
}

void BuildActivityLog(const std::string& process, const std::string& message)
{
    if (s_Sink != nullptr)
        s_Sink->Log(process, message);
}

void BuildActivityComplete(const std::string& process)
{
    if (s_Sink != nullptr)
        s_Sink->Complete(process);
}

void BuildActivityFail(const std::string& process, const std::string& error)
{
    if (s_Sink != nullptr)
        s_Sink->Fail(process, error);
}

}  // namespace DekiEditor
