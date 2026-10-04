// UpdateCheckParse: the decisions behind the "a newer editor exists" toast,
// with the network and the settings out of the picture.

#include <gtest/gtest.h>

#include "services/UpdateCheckParse.h"

using namespace DekiEditor;

namespace
{
const char* const kRelease = R"({
  "url": "https://api.github.com/repos/dekiengine/deki-editor/releases/1",
  "html_url": "https://github.com/dekiengine/deki-editor/releases/tag/0.11.0",
  "tag_name": "0.11.0",
  "name": "Deki Editor 0.11.0",
  "draft": false,
  "prerelease": false,
  "assets": [{"name": "DekiEditor-0.11.0-windows-x86_64.zip"}]
})";
}

TEST(UpdateCheckParse, ReadsTagAndReleasePage)
{
    LatestRelease r;
    ASSERT_TRUE(ParseLatestRelease(kRelease, r));
    EXPECT_EQ(r.tag, "0.11.0");
    EXPECT_EQ(r.htmlUrl, "https://github.com/dekiengine/deki-editor/releases/tag/0.11.0");
}

TEST(UpdateCheckParse, StripsALeadingV)
{
    LatestRelease r;
    ASSERT_TRUE(ParseLatestRelease(
        R"({"tag_name": "v1.2.3", "html_url": "https://github.com/dekiengine/deki-editor/releases/tag/v1.2.3"})", r));
    EXPECT_EQ(r.tag, "1.2.3");
}

TEST(UpdateCheckParse, OnlyOurOwnReleasePageIsOpened)
{
    // html_url goes to SDL_OpenURL: a file:// or foreign-host URL in a spoofed
    // response is a reason to distrust the whole release.
    LatestRelease r;
    EXPECT_FALSE(
        ParseLatestRelease(R"({"tag_name": "0.11.0", "html_url": "file:///C:/Windows/System32/calc.exe"})", r));
    EXPECT_FALSE(
        ParseLatestRelease(R"({"tag_name": "0.11.0", "html_url": "https://evil.example/releases/tag/0.11.0"})", r));
    EXPECT_FALSE(ParseLatestRelease(
        R"({"tag_name": "0.11.0", "html_url": "http://github.com/dekiengine/deki-editor/releases/tag/0.11.0"})", r));
    EXPECT_FALSE(ParseLatestRelease(R"({"tag_name": "0.11.0"})", r));
    EXPECT_TRUE(ParseLatestRelease(
        R"({"tag_name": "0.11.0", "html_url": "https://github.com/dekiengine/deki-editor/releases/tag/0.11.0"})", r));
}

TEST(UpdateCheckParse, RejectsDraftsPrereleasesAndJunk)
{
    LatestRelease r;
    EXPECT_FALSE(ParseLatestRelease(R"({"tag_name": "0.11.0", "draft": true})", r));
    EXPECT_FALSE(ParseLatestRelease(R"({"tag_name": "0.11.0", "prerelease": true})", r));
    EXPECT_FALSE(ParseLatestRelease(R"({"message": "Not Found"})", r)) << "GitHub's 404 body";
    EXPECT_FALSE(ParseLatestRelease("<html>rate limited</html>", r));
    EXPECT_FALSE(ParseLatestRelease("", r));
}

TEST(UpdateCheckParse, NewerMeansStrictlyHigher)
{
    EXPECT_TRUE(IsNewerRelease("0.11.0", "0.10.0"));
    EXPECT_TRUE(IsNewerRelease("v0.10.1", "0.10.0"));
    EXPECT_FALSE(IsNewerRelease("0.10.0", "0.10.0"));
    EXPECT_FALSE(IsNewerRelease("0.9.0", "0.10.0"));
    EXPECT_FALSE(IsNewerRelease("1.0.0", "0.0.0-dev")) << "a dev build is never nagged";
}

TEST(UpdateCheckParse, DueOncePerInterval)
{
    const int64_t day = 24 * 60 * 60;
    EXPECT_TRUE(UpdateCheckIsDue(0, 1000, day)) << "never checked";
    EXPECT_FALSE(UpdateCheckIsDue(1000, 1000 + day - 1, day));
    EXPECT_TRUE(UpdateCheckIsDue(1000, 1000 + day, day));
    EXPECT_TRUE(UpdateCheckIsDue(5000, 1000, day)) << "clock went backwards: check rather than wait";
}
