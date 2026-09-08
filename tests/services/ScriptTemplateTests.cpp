// The Create Script template must produce a class the reflection codegen
// recognises as-is: no macro inside the class, no generated include, the
// metadata markers above it. (The 0.10 template wrote a three-argument
// DEKI_COMPONENT that never compiled.)

#include <gtest/gtest.h>

#include "services/ScriptTemplate.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using DekiEditor::ScriptTemplate::CreateBehaviourScript;

namespace
{
std::string ReadAll(const fs::path& p)
{
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

class ScriptTemplateTest : public ::testing::Test
{
protected:
    fs::path dir;
    void SetUp() override
    {
        dir = fs::temp_directory_path() / "deki_script_template_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

TEST_F(ScriptTemplateTest, WritesAMacroFreeBehaviour)
{
    std::string error;
    ASSERT_TRUE(CreateBehaviourScript(dir.string(), "Mover", error)) << error;

    const std::string header = ReadAll(dir / "Mover.h");
    EXPECT_NE(header.find("class Mover : public Deki::Component"), std::string::npos);
    EXPECT_NE(header.find("DEKI_CATEGORY("), std::string::npos);
    EXPECT_NE(header.find("DEKI_DESCRIPTION("), std::string::npos);
    EXPECT_NE(header.find("DEKI_EXPORT"), std::string::npos);
    EXPECT_EQ(header.find("DEKI_COMPONENT("), std::string::npos);
    EXPECT_EQ(header.find(".gen.h"), std::string::npos);
    // The markers sit above the class, not inside it
    EXPECT_LT(header.find("DEKI_CATEGORY("), header.find("class Mover"));
    EXPECT_LT(header.find("DEKI_DESCRIPTION("), header.find("class Mover"));

    const std::string source = ReadAll(dir / "Mover.cpp");
    EXPECT_NE(source.find("#include \"Mover.h\""), std::string::npos);
    EXPECT_NE(source.find("void Mover::Update()"), std::string::npos);
}

TEST_F(ScriptTemplateTest, RefusesToOverwrite)
{
    std::string error;
    ASSERT_TRUE(CreateBehaviourScript(dir.string(), "Mover", error)) << error;
    EXPECT_FALSE(CreateBehaviourScript(dir.string(), "Mover", error));
    EXPECT_NE(error.find("already exists"), std::string::npos);
}
