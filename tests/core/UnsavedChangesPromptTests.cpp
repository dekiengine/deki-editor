// The "you have unsaved changes" state machine. These properties decide
// whether a user's work survives an interrupted New/Open/Close.

#include <gtest/gtest.h>

#include "core/UnsavedChangesPrompt.h"

using DekiEditor::UnsavedChangesPrompt;
using DekiEditor::PendingSceneAction;
using DekiEditor::DialogAnswer;

namespace
{

TEST(UnsavedChangesPrompt, CleanSceneProceedsWithoutInterrupting)
{
    UnsavedChangesPrompt p;
    EXPECT_FALSE(p.Request(PendingSceneAction::NewScene, "", /*dirty*/ false));
    EXPECT_FALSE(p.IsOpen());
    EXPECT_EQ(p.PendingAction(), PendingSceneAction::None);
}

TEST(UnsavedChangesPrompt, DirtySceneRaisesThePromptAndDefersTheAction)
{
    UnsavedChangesPrompt p;
    EXPECT_TRUE(p.Request(PendingSceneAction::OpenScene, "scenes/level2.scene", /*dirty*/ true));
    EXPECT_TRUE(p.IsOpen());
    EXPECT_TRUE(p.DialogVisible());
    EXPECT_EQ(p.PendingAction(), PendingSceneAction::OpenScene);
    EXPECT_EQ(p.PendingPath(), "scenes/level2.scene");
}

// IsOpen must stay true between the modal closing and the deferred action
// running. A scripted caller that trusts a false here would issue commands that
// appear to succeed while the editor is still on the guarded scene.
TEST(UnsavedChangesPrompt, StaysOpenWhileAnActionIsStillPending)
{
    UnsavedChangesPrompt p;
    ASSERT_TRUE(p.Request(PendingSceneAction::CloseProject, "", true));

    p.SetDialogVisible(false);  // the modal drew and closed itself
    EXPECT_TRUE(p.IsOpen()) << "the deferred action has not run yet";

    std::string path;
    EXPECT_EQ(p.TakePending(path), PendingSceneAction::CloseProject);
    EXPECT_FALSE(p.IsOpen()) << "now it really is done";
}

TEST(UnsavedChangesPrompt, TakePendingClearsUpFrontSoReentryCannotSeeAStaleAction)
{
    UnsavedChangesPrompt p;
    ASSERT_TRUE(p.Request(PendingSceneAction::OpenScene, "a.scene", true));

    std::string path;
    EXPECT_EQ(p.TakePending(path), PendingSceneAction::OpenScene);
    EXPECT_EQ(path, "a.scene");

    // A re-entrant call - e.g. the open failed and raised its own dialog.
    std::string second;
    EXPECT_EQ(p.TakePending(second), PendingSceneAction::None);
    EXPECT_TRUE(second.empty());
}

TEST(UnsavedChangesPrompt, CancelAbandonsTheDeferredAction)
{
    UnsavedChangesPrompt p;
    ASSERT_TRUE(p.Request(PendingSceneAction::NewScene, "", true));

    p.Cancel();
    EXPECT_FALSE(p.IsOpen());
    EXPECT_EQ(p.PendingAction(), PendingSceneAction::None);
    EXPECT_TRUE(p.PendingPath().empty());
}

// Suppression is the re-entrancy guard: while the caller replays the user's
// answer through the real NewScene/OpenScene, asking again would loop straight
// back into the dialog.
TEST(UnsavedChangesPrompt, SuppressedRequestsDoNotReopenTheDialog)
{
    UnsavedChangesPrompt p;
    p.SetSuppressed(true);
    EXPECT_FALSE(p.Request(PendingSceneAction::NewScene, "", /*dirty*/ true))
        << "a dirty scene must still not re-prompt while replaying an answer";
    EXPECT_FALSE(p.IsOpen());

    p.SetSuppressed(false);
    EXPECT_TRUE(p.Request(PendingSceneAction::NewScene, "", true));
}

TEST(UnsavedChangesPrompt, ProgrammaticAnswerIsDeliveredExactlyOnce)
{
    UnsavedChangesPrompt p;
    EXPECT_EQ(p.TakeAnswer(), DialogAnswer::None);

    p.Answer(DialogAnswer::Discard);
    EXPECT_EQ(p.TakeAnswer(), DialogAnswer::Discard);
    EXPECT_EQ(p.TakeAnswer(), DialogAnswer::None) << "an answer must not apply again on the next frame";
}

TEST(UnsavedChangesPrompt, LatestProgrammaticAnswerWins)
{
    UnsavedChangesPrompt p;
    p.Answer(DialogAnswer::Save);
    p.Answer(DialogAnswer::Cancel);
    EXPECT_EQ(p.TakeAnswer(), DialogAnswer::Cancel);
}

// The full path a deferred open takes, in order.
TEST(UnsavedChangesPrompt, InterruptedOpenRoundTrips)
{
    UnsavedChangesPrompt p;

    ASSERT_TRUE(p.Request(PendingSceneAction::OpenScene, "b.scene", true));
    p.Answer(DialogAnswer::Discard);

    // The modal draws, consumes the answer, and closes.
    p.SetDialogVisible(false);
    ASSERT_EQ(p.TakeAnswer(), DialogAnswer::Discard);

    // The caller replays it with re-asking suppressed.
    std::string path;
    ASSERT_EQ(p.TakePending(path), PendingSceneAction::OpenScene);
    ASSERT_EQ(path, "b.scene");

    p.SetSuppressed(true);
    EXPECT_FALSE(p.Request(PendingSceneAction::OpenScene, "b.scene", true));
    p.SetSuppressed(false);

    EXPECT_FALSE(p.IsOpen());
}

// Quit and Open Project go through the prompt too: the close button, Alt+F4,
// File > Exit and File > Open Project must not discard a dirty scene without
// asking.
TEST(UnsavedChangesPrompt, QuitIsDeferredWhileDirty)
{
    UnsavedChangesPrompt p;
    EXPECT_TRUE(p.Request(PendingSceneAction::Quit, "", /*dirty*/ true));
    EXPECT_TRUE(p.IsOpen());

    p.SetDialogVisible(false);  // the modal drew, was answered, and closed
    std::string path;
    EXPECT_EQ(p.TakePending(path), PendingSceneAction::Quit);
    EXPECT_FALSE(p.IsOpen());
}

TEST(UnsavedChangesPrompt, QuitProceedsImmediatelyWhenClean)
{
    UnsavedChangesPrompt p;
    EXPECT_FALSE(p.Request(PendingSceneAction::Quit, "", /*dirty*/ false));
    EXPECT_FALSE(p.IsOpen());
}

TEST(UnsavedChangesPrompt, OpenProjectCarriesTheProjectPath)
{
    UnsavedChangesPrompt p;
    ASSERT_TRUE(p.Request(PendingSceneAction::OpenProject, "C:/projects/other", true));

    p.SetDialogVisible(false);
    std::string path;
    EXPECT_EQ(p.TakePending(path), PendingSceneAction::OpenProject);
    EXPECT_EQ(path, "C:/projects/other");
}

}  // namespace
