#include "UnsavedChangesPrompt.h"

namespace DekiEditor
{

bool UnsavedChangesPrompt::Request(PendingSceneAction action, const std::string& path, bool sceneIsDirty)
{
    if (m_Suppressed)
    {
        return false;
    }

    if (!sceneIsDirty)
    {
        return false;  // nothing to lose; the caller proceeds immediately
    }

    m_DialogVisible = true;
    m_PendingAction = action;
    m_PendingPath = path;
    return true;
}

PendingSceneAction UnsavedChangesPrompt::TakePending(std::string& outPath)
{
    const PendingSceneAction action = m_PendingAction;
    outPath = m_PendingPath;

    m_PendingAction = PendingSceneAction::None;
    m_PendingPath.clear();
    return action;
}

void UnsavedChangesPrompt::Cancel()
{
    m_DialogVisible = false;
    m_PendingAction = PendingSceneAction::None;
    m_PendingPath.clear();
}

DialogAnswer UnsavedChangesPrompt::TakeAnswer()
{
    const DialogAnswer answer = m_PendingAnswer;
    m_PendingAnswer = DialogAnswer::None;
    return answer;
}

}  // namespace DekiEditor
