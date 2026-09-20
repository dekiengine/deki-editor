#include <gtest/gtest.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace DekiEditor;
using json = nlohmann::json;

// ============================================================================
// AssetTypeRegistry Tests
// ============================================================================

class AssetTypeRegistryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create temp directory for test files
        m_TempDir = fs::temp_directory_path() / "deki_asset_type_test";
        fs::create_directories(m_TempDir);

        // Note: AssetTypeRegistry is a singleton, tests may affect each other
        // In production, you'd want isolation, but for now we test additive behavior
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
    }

    // Helper to create a test handler
    AssetTypeHandler CreateTestHandler(const std::string& typeName,
                                        const std::string& extension,
                                        AssetCompileTarget target = AssetCompileTarget::Texture)
    {
        AssetTypeHandler handler;
        handler.typeName = typeName;
        handler.displayName = typeName + " Display";
        handler.extensions = {extension};
        handler.defaultContent = R"({"type":")" + typeName + R"("})";
        handler.compilesTo = target;
        return handler;
    }

    fs::path m_TempDir;
};

// ============================================================================
// Registration
// ============================================================================

TEST_F(AssetTypeRegistryTest, Register_AddsHandler)
{
    auto& registry = AssetTypeRegistry::Instance();
    size_t initialCount = registry.GetTypes().size();

    AssetTypeHandler handler = CreateTestHandler("TestType1", ".test1");
    registry.Register(handler);

    EXPECT_EQ(registry.GetTypes().size(), initialCount + 1);
}

TEST_F(AssetTypeRegistryTest, Register_MultipleHandlers_AllAdded)
{
    auto& registry = AssetTypeRegistry::Instance();
    size_t initialCount = registry.GetTypes().size();

    registry.Register(CreateTestHandler("TestTypeA", ".testa"));
    registry.Register(CreateTestHandler("TestTypeB", ".testb"));
    registry.Register(CreateTestHandler("TestTypeC", ".testc"));

    EXPECT_EQ(registry.GetTypes().size(), initialCount + 3);
}

// ============================================================================
// GetHandler
// ============================================================================

TEST_F(AssetTypeRegistryTest, GetHandler_ExistingType_ReturnsHandler)
{
    auto& registry = AssetTypeRegistry::Instance();

    AssetTypeHandler handler = CreateTestHandler("UniqueTestType", ".unique");
    handler.displayName = "Unique Display Name";
    registry.Register(handler);

    const AssetTypeHandler* found = registry.GetHandler("UniqueTestType");

    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->typeName, "UniqueTestType");
    EXPECT_EQ(found->displayName, "Unique Display Name");
}

TEST_F(AssetTypeRegistryTest, GetHandler_NonExistingType_ReturnsNull)
{
    auto& registry = AssetTypeRegistry::Instance();

    const AssetTypeHandler* found = registry.GetHandler("NonExistentType12345");

    EXPECT_EQ(found, nullptr);
}

// ============================================================================
// GetHandlerForExtension
// ============================================================================

TEST_F(AssetTypeRegistryTest, GetHandlerForExtension_ExistingExtension_ReturnsHandler)
{
    auto& registry = AssetTypeRegistry::Instance();

    AssetTypeHandler handler = CreateTestHandler("ExtTest", ".extest");
    registry.Register(handler);

    const AssetTypeHandler* found = registry.GetHandlerForExtension(".extest");

    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->typeName, "ExtTest");
}

TEST_F(AssetTypeRegistryTest, GetHandlerForExtension_CaseInsensitive)
{
    auto& registry = AssetTypeRegistry::Instance();

    AssetTypeHandler handler = CreateTestHandler("CaseTest", ".casetest");
    registry.Register(handler);

    // Test various cases
    EXPECT_NE(registry.GetHandlerForExtension(".casetest"), nullptr);
    EXPECT_NE(registry.GetHandlerForExtension(".CASETEST"), nullptr);
    EXPECT_NE(registry.GetHandlerForExtension(".CaseTest"), nullptr);
}

TEST_F(AssetTypeRegistryTest, GetHandlerForExtension_NonExisting_ReturnsNull)
{
    auto& registry = AssetTypeRegistry::Instance();

    const AssetTypeHandler* found = registry.GetHandlerForExtension(".nonexistent12345");

    EXPECT_EQ(found, nullptr);
}

TEST_F(AssetTypeRegistryTest, HasHandlerForExtension_ReturnsCorrectBool)
{
    auto& registry = AssetTypeRegistry::Instance();

    AssetTypeHandler handler = CreateTestHandler("HasTest", ".hastest");
    registry.Register(handler);

    EXPECT_TRUE(registry.HasHandlerForExtension(".hastest"));
    EXPECT_FALSE(registry.HasHandlerForExtension(".doesnotexist"));
}

// ============================================================================
// Multiple Extensions
// ============================================================================

TEST_F(AssetTypeRegistryTest, MultipleExtensions_AllResolveToSameHandler)
{
    auto& registry = AssetTypeRegistry::Instance();

    AssetTypeHandler handler;
    handler.typeName = "MultiExtType";
    handler.displayName = "Multi Extension Type";
    handler.extensions = {".multi1", ".multi2", ".multi3"};
    handler.defaultContent = "{}";
    handler.compilesTo = AssetCompileTarget::Texture;
    registry.Register(handler);

    const AssetTypeHandler* found1 = registry.GetHandlerForExtension(".multi1");
    const AssetTypeHandler* found2 = registry.GetHandlerForExtension(".multi2");
    const AssetTypeHandler* found3 = registry.GetHandlerForExtension(".multi3");

    ASSERT_NE(found1, nullptr);
    ASSERT_NE(found2, nullptr);
    ASSERT_NE(found3, nullptr);

    EXPECT_EQ(found1->typeName, "MultiExtType");
    EXPECT_EQ(found2->typeName, "MultiExtType");
    EXPECT_EQ(found3->typeName, "MultiExtType");
}

// ============================================================================
// CreateAsset
// ============================================================================

TEST_F(AssetTypeRegistryTest, CreateAsset_ValidType_CreatesFileWithDefaultContent)
{
    auto& registry = AssetTypeRegistry::Instance();

    AssetTypeHandler handler;
    handler.typeName = "CreateTest";
    handler.displayName = "Create Test";
    handler.extensions = {".createtest"};
    handler.defaultContent = R"({"type":"CreateTest","value":42})";
    handler.compilesTo = AssetCompileTarget::Data;
    registry.Register(handler);

    std::string filePath = (m_TempDir / "test.createtest").string();
    bool success = registry.CreateAsset("CreateTest", filePath);

    EXPECT_TRUE(success);
    EXPECT_TRUE(fs::exists(filePath));

    // Verify content
    std::ifstream file(filePath);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, handler.defaultContent);
}

TEST_F(AssetTypeRegistryTest, CreateAsset_InvalidType_ReturnsFalse)
{
    auto& registry = AssetTypeRegistry::Instance();

    std::string filePath = (m_TempDir / "test.invalid").string();
    bool success = registry.CreateAsset("NonExistentType99999", filePath);

    EXPECT_FALSE(success);
    EXPECT_FALSE(fs::exists(filePath));
}

TEST_F(AssetTypeRegistryTest, CreateAsset_InvalidPath_ReturnsFalse)
{
    auto& registry = AssetTypeRegistry::Instance();

    AssetTypeHandler handler = CreateTestHandler("PathTest", ".pathtest");
    registry.Register(handler);

    // Try to create in non-existent deeply nested directory without creating parents
    std::string invalidPath = (m_TempDir / "nonexistent" / "deep" / "path" / "test.pathtest").string();
    bool success = registry.CreateAsset("PathTest", invalidPath);

    // Should fail because parent directories don't exist
    EXPECT_FALSE(success);
}

// ============================================================================
// CompileAsset
// ============================================================================

TEST_F(AssetTypeRegistryTest, CompileAsset_WithCompileFunc_CallsHandler)
{
    auto& registry = AssetTypeRegistry::Instance();

    bool compileCalled = false;
    int capturedWidth = 0;
    int capturedHeight = 0;

    AssetTypeHandler handler;
    handler.typeName = "CompileTest";
    handler.displayName = "Compile Test";
    handler.extensions = {".compiletest"};
    handler.defaultContent = "{}";
    handler.compilesTo = AssetCompileTarget::Texture;
    handler.compile = [&](const json& data, std::vector<uint8_t>& rgba, int& w, int& h) {
        compileCalled = true;
        w = 64;
        h = 32;
        rgba.resize(w * h * 4, 255);
        return true;
    };
    registry.Register(handler);

    json data = {{"type", "CompileTest"}};
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;

    bool success = registry.CompileAsset(data, rgba, width, height);

    EXPECT_TRUE(success);
    EXPECT_TRUE(compileCalled);
    EXPECT_EQ(width, 64);
    EXPECT_EQ(height, 32);
    EXPECT_EQ(rgba.size(), 64u * 32u * 4u);
}

TEST_F(AssetTypeRegistryTest, CompileAsset_NoCompileFunc_ReturnsFalse)
{
    auto& registry = AssetTypeRegistry::Instance();

    AssetTypeHandler handler;
    handler.typeName = "NoCompileTest";
    handler.displayName = "No Compile Test";
    handler.extensions = {".nocompile"};
    handler.defaultContent = "{}";
    handler.compilesTo = AssetCompileTarget::Texture;
    // compile function not set
    registry.Register(handler);

    json data = {{"type", "NoCompileTest"}};
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;

    bool success = registry.CompileAsset(data, rgba, width, height);

    EXPECT_FALSE(success);
}

TEST_F(AssetTypeRegistryTest, CompileAsset_UnknownType_ReturnsFalse)
{
    auto& registry = AssetTypeRegistry::Instance();

    json data = {{"type", "UnknownType99999"}};
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;

    bool success = registry.CompileAsset(data, rgba, width, height);

    EXPECT_FALSE(success);
}

// ============================================================================
// CompileAssetToFile
// ============================================================================

TEST_F(AssetTypeRegistryTest, CompileAssetToFile_WithCompileToFileFunc_CallsHandler)
{
    auto& registry = AssetTypeRegistry::Instance();

    bool compileToFileCalled = false;
    std::string capturedPath;

    AssetTypeHandler handler;
    handler.typeName = "CompileToFileTest";
    handler.displayName = "Compile To File Test";
    handler.extensions = {".compiletofile"};
    handler.defaultContent = "{}";
    handler.compilesTo = AssetCompileTarget::Data;
    handler.compileToFile = [&](const json& data, const std::string& path) {
        compileToFileCalled = true;
        capturedPath = path;

        // Create the output file
        std::ofstream file(path, std::ios::binary);
        file << "compiled data";
        return true;
    };
    registry.Register(handler);

    json data = {{"type", "CompileToFileTest"}};
    std::string outputPath = (m_TempDir / "output.bin").string();

    bool success = registry.CompileAssetToFile(data, outputPath);

    EXPECT_TRUE(success);
    EXPECT_TRUE(compileToFileCalled);
    EXPECT_EQ(capturedPath, outputPath);
}

TEST_F(AssetTypeRegistryTest, CompileAssetToFile_NoCompileToFileFunc_ReturnsFalse)
{
    auto& registry = AssetTypeRegistry::Instance();

    AssetTypeHandler handler;
    handler.typeName = "NoCompileToFileTest";
    handler.displayName = "No Compile To File Test";
    handler.extensions = {".nocompiletofile"};
    handler.defaultContent = "{}";
    handler.compilesTo = AssetCompileTarget::Data;
    // compileToFile function not set
    registry.Register(handler);

    json data = {{"type", "NoCompileToFileTest"}};
    std::string outputPath = (m_TempDir / "output.bin").string();

    bool success = registry.CompileAssetToFile(data, outputPath);

    EXPECT_FALSE(success);
}

// ============================================================================
// Compile Target Types
// ============================================================================

TEST_F(AssetTypeRegistryTest, CompileTarget_TextureType_HasCompileFunc)
{
    auto& registry = AssetTypeRegistry::Instance();

    AssetTypeHandler handler;
    handler.typeName = "TextureTarget";
    handler.displayName = "Texture Target";
    handler.extensions = {".texturetarget"};
    handler.defaultContent = "{}";
    handler.compilesTo = AssetCompileTarget::Texture;
    handler.compile = [](const json&, std::vector<uint8_t>&, int&, int&) { return true; };
    registry.Register(handler);

    const AssetTypeHandler* found = registry.GetHandler("TextureTarget");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->compilesTo, AssetCompileTarget::Texture);
    EXPECT_TRUE(found->compile != nullptr);
}

TEST_F(AssetTypeRegistryTest, CompileTarget_DataType_HasCompileToFileFunc)
{
    auto& registry = AssetTypeRegistry::Instance();

    AssetTypeHandler handler;
    handler.typeName = "DataTarget";
    handler.displayName = "Data Target";
    handler.extensions = {".datatarget"};
    handler.defaultContent = "{}";
    handler.compilesTo = AssetCompileTarget::Data;
    handler.compileToFile = [](const json&, const std::string&) { return true; };
    registry.Register(handler);

    const AssetTypeHandler* found = registry.GetHandler("DataTarget");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->compilesTo, AssetCompileTarget::Data);
    EXPECT_TRUE(found->compileToFile != nullptr);
}

// =============================================================================
// Invariant: every registered handler ships a defaultContent string that is
// itself valid JSON. CreateAsset writes this string verbatim to a new .asset
// file, and the subsequent OnInspectorGUI deserializes it - so a malformed
// default would crash the inspector the moment the user creates the type.
// =============================================================================

TEST_F(AssetTypeRegistryTest, DefaultContent_IsValidJson_ForAllRegisteredTypes)
{
    auto& registry = AssetTypeRegistry::Instance();

    for (const AssetTypeHandler& handler : registry.GetTypes())
    {
        if (handler.defaultContent.empty())
            continue;  // pass-through types may legitimately have no default

        try
        {
            json parsed = json::parse(handler.defaultContent);
            (void)parsed;
        }
        catch (const json::exception& e)
        {
            ADD_FAILURE() << "AssetTypeHandler '" << handler.typeName
                          << "' has malformed defaultContent: " << e.what();
        }
    }
}
