#include "StartupProgress.h"
#include "ProgressModal.h"

namespace DekiEditor
{

bool StartupProgress::s_Complete = false;

void StartupProgress::SetWindow(SDL_Window* window)
{
    ProgressModal::SetWindow(window);
    ProgressModal::Reset();
    s_Complete = false;
}

void StartupProgress::Begin()
{
    ProgressModal::Reset();
    s_Complete = false;
}

void StartupProgress::Complete()
{
    s_Complete = true;
}

bool StartupProgress::IsComplete()
{
    return s_Complete;
}

void StartupProgress::Update(const char* phase, const char* detail)
{
    if (s_Complete)
    {
        return;
    }

    ProgressModal::Show("Deki Editor", phase, detail);
}

}  // namespace DekiEditor
