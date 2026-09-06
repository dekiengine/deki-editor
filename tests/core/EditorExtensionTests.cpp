#include <gtest/gtest.h>
#include <deki-editor/EditorExtension.h>
#include <deki-editor/EditorRegistry.h>
#include <memory>

using namespace DekiEditor;
using namespace DekiEditorInternal;

// ============================================================================
// Mock Editor Classes for Testing
// ============================================================================

// Mock AssetTypeEditor for testing
class MockAssetTypeEditor : public AssetTypeEditor
{
public:
    MockAssetTypeEditor(const std::string& typeName = "MockAsset")
        : m_TypeName(typeName) {}

    const char* GetTypeName() const override { return m_TypeName.c_str(); }
    const char* GetDisplayName() const override { return "Mock Asset Display"; }
    const char* GetExtension() const override { return ".mockasset"; }
    const char* GetDefaultContent() const override { return "{}"; }
    int GetCompileTarget() const override { return 0; } // Texture

    bool Compile(const std::string& jsonData,
                 std::vector<uint8_t>& rgba,
                 int& width, int& height) override
    {
        m_CompileCalled = true;
        width = 32;
        height = 32;
        rgba.resize(32 * 32 * 4, 128);
        return true;
    }

    bool CompileToFile(const std::string& jsonData,
                       const std::string& cachePath) override
    {
        m_CompileToFileCalled = true;
        m_LastCachePath = cachePath;
        return true;
    }

    bool OnInspectorGUI(std::string& jsonData,
                        const std::string& assetPath,
                        const std::string& assetGuid) override
    {
        m_InspectorCalled = true;
        return false;
    }

    // Test state
    bool m_CompileCalled = false;
    bool m_CompileToFileCalled = false;
    bool m_InspectorCalled = false;
    std::string m_LastCachePath;

private:
    std::string m_TypeName;
};

// Mock AssetTypeEditor that returns Data compile target
class MockDataAssetEditor : public AssetTypeEditor
{
public:
    const char* GetTypeName() const override { return "MockDataAsset"; }
    const char* GetDisplayName() const override { return "Mock Data Asset"; }
    const char* GetExtension() const override { return ".mockdata"; }
    const char* GetDefaultContent() const override { return R"({"data": true})"; }
    int GetCompileTarget() const override { return 2; } // Data

    bool Compile(const std::string& jsonData,
                 std::vector<uint8_t>& rgba,
                 int& width, int& height) override
    {
        return false; // Data assets don't use Compile
    }

    bool CompileToFile(const std::string& jsonData,
                       const std::string& cachePath) override
    {
        m_CompileToFileCalled = true;
        return true;
    }

    bool OnInspectorGUI(std::string& jsonData,
                        const std::string& assetPath,
                        const std::string& assetGuid) override
    {
        return false;
    }

    bool m_CompileToFileCalled = false;
};

// ============================================================================
// AssetTypeEditor Tests
// ============================================================================

class AssetTypeEditorTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(AssetTypeEditorTest, GetEditorType_ReturnsAssetTypeEditor)
{
    MockAssetTypeEditor editor;
    EXPECT_EQ(editor.GetEditorType(), EditorType::AssetTypeEditor);
}

TEST_F(AssetTypeEditorTest, GetName_ReturnsTypeName)
{
    MockAssetTypeEditor editor("CustomTypeName");
    EXPECT_STREQ(editor.GetName(), "CustomTypeName");
}

TEST_F(AssetTypeEditorTest, Compile_TextureTarget_ReturnsRGBA)
{
    MockAssetTypeEditor editor;

    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    bool success = editor.Compile("{}", rgba, width, height);

    EXPECT_TRUE(success);
    EXPECT_TRUE(editor.m_CompileCalled);
    EXPECT_EQ(width, 32);
    EXPECT_EQ(height, 32);
    EXPECT_EQ(rgba.size(), 32u * 32u * 4u);
}

TEST_F(AssetTypeEditorTest, CompileToFile_DefaultImplementation_ReturnsFalse)
{
    // Create a minimal editor that doesn't override CompileToFile
    class MinimalEditor : public AssetTypeEditor
    {
    public:
        const char* GetTypeName() const override { return "Minimal"; }
        const char* GetDisplayName() const override { return "Minimal"; }
        const char* GetExtension() const override { return ".min"; }
        const char* GetDefaultContent() const override { return "{}"; }
        int GetCompileTarget() const override { return 0; }

        bool Compile(const std::string&, std::vector<uint8_t>&, int&, int&) override
        {
            return true;
        }

        bool OnInspectorGUI(std::string&, const std::string&, const std::string&) override
        {
            return false;
        }
    };

    MinimalEditor editor;
    bool success = editor.CompileToFile("{}", "/path/to/cache");

    // Default implementation should return false
    EXPECT_FALSE(success);
}

TEST_F(AssetTypeEditorTest, CompileToFile_Overridden_Works)
{
    MockAssetTypeEditor editor;
    bool success = editor.CompileToFile("{}", "/path/to/cache");

    EXPECT_TRUE(success);
    EXPECT_TRUE(editor.m_CompileToFileCalled);
    EXPECT_EQ(editor.m_LastCachePath, "/path/to/cache");
}

TEST_F(AssetTypeEditorTest, GetCompileTarget_Texture_ReturnsZero)
{
    MockAssetTypeEditor editor;
    EXPECT_EQ(editor.GetCompileTarget(), 0);
}

TEST_F(AssetTypeEditorTest, GetCompileTarget_Data_ReturnsTwo)
{
    MockDataAssetEditor editor;
    EXPECT_EQ(editor.GetCompileTarget(), 2);
}

// ============================================================================
// AssetTypeEditorRegistry Tests
// ============================================================================

class AssetTypeEditorRegistryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Note: Registry is a singleton, so tests may affect each other
        // Create fresh editors for each test
    }

    void TearDown() override {}
};

TEST_F(AssetTypeEditorRegistryTest, Instance_ReturnsSameInstance)
{
    auto& instance1 = AssetTypeEditorRegistry::Instance();
    auto& instance2 = AssetTypeEditorRegistry::Instance();

    EXPECT_EQ(&instance1, &instance2);
}

TEST_F(AssetTypeEditorRegistryTest, Register_AddsEditor)
{
    auto& registry = AssetTypeEditorRegistry::Instance();
    size_t initialCount = registry.GetAllEditors().size();

    MockAssetTypeEditor editor("RegistryTestEditor");
    registry.Register(&editor);

    EXPECT_EQ(registry.GetAllEditors().size(), initialCount + 1);
}

TEST_F(AssetTypeEditorRegistryTest, GetEditor_ExistingType_ReturnsEditor)
{
    auto& registry = AssetTypeEditorRegistry::Instance();

    MockAssetTypeEditor editor("UniqueEditorName");
    registry.Register(&editor);

    AssetTypeEditor* found = registry.GetEditor("UniqueEditorName");

    ASSERT_NE(found, nullptr);
    EXPECT_STREQ(found->GetTypeName(), "UniqueEditorName");
}

TEST_F(AssetTypeEditorRegistryTest, GetEditor_NonExisting_ReturnsNull)
{
    auto& registry = AssetTypeEditorRegistry::Instance();

    AssetTypeEditor* found = registry.GetEditor("NonExistentEditor12345");

    EXPECT_EQ(found, nullptr);
}

// ============================================================================
// EditorRegistry (Engine-side) Tests
// ============================================================================

class EditorRegistryTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(EditorRegistryTest, Instance_ReturnsSameInstance)
{
    auto& instance1 = EditorRegistry::Instance();
    auto& instance2 = EditorRegistry::Instance();

    EXPECT_EQ(&instance1, &instance2);
}

TEST_F(EditorRegistryTest, AddFactory_CreatesFactoryInfo)
{
    auto& registry = EditorRegistry::Instance();
    size_t initialCount = registry.GetAllFactories().size();

    static MockAssetTypeEditor prototype;
    registry.AddFactory(&prototype, []() -> std::unique_ptr<EditorExtension> {
        return std::make_unique<MockAssetTypeEditor>();
    });

    EXPECT_EQ(registry.GetAllFactories().size(), initialCount + 1);
}

TEST_F(EditorRegistryTest, Factory_CreatesFreshInstances)
{
    auto& registry = EditorRegistry::Instance();

    static MockAssetTypeEditor prototype("FactoryTestEditor");
    registry.AddFactory(&prototype, []() -> std::unique_ptr<EditorExtension> {
        return std::make_unique<MockAssetTypeEditor>("FactoryTestEditor");
    });

    // Find the factory
    const EditorFactoryInfo* factoryInfo = nullptr;
    for (const auto& info : registry.GetAllFactories())
    {
        if (info.prototype && std::string(info.prototype->GetName()) == "FactoryTestEditor")
        {
            factoryInfo = &info;
            break;
        }
    }

    ASSERT_NE(factoryInfo, nullptr);
    ASSERT_NE(factoryInfo->factory, nullptr);

    // Create two instances
    auto instance1 = factoryInfo->factory();
    auto instance2 = factoryInfo->factory();

    ASSERT_NE(instance1, nullptr);
    ASSERT_NE(instance2, nullptr);

    // Should be different instances
    EXPECT_NE(instance1.get(), instance2.get());

    // But same type
    EXPECT_STREQ(instance1->GetName(), "FactoryTestEditor");
    EXPECT_STREQ(instance2->GetName(), "FactoryTestEditor");
}

TEST_F(EditorRegistryTest, GetTotalCount_IncludesFactories)
{
    auto& registry = EditorRegistry::Instance();
    size_t initialCount = registry.GetTotalCount();

    // Add factory
    static MockAssetTypeEditor factoryProto("CountTestFactory");
    registry.AddFactory(&factoryProto, []() -> std::unique_ptr<EditorExtension> {
        return std::make_unique<MockAssetTypeEditor>("CountTestFactory");
    });

    EXPECT_EQ(registry.GetTotalCount(), initialCount + 1);
}

// ============================================================================
// EditorExtension Type Checks
// ============================================================================

TEST_F(AssetTypeEditorTest, EditorType_IsCorrect)
{
    MockAssetTypeEditor editor;

    EXPECT_EQ(editor.GetEditorType(), EditorType::AssetTypeEditor);
}

TEST_F(AssetTypeEditorTest, Polymorphism_WorksCorrectly)
{
    std::unique_ptr<EditorExtension> extension = std::make_unique<MockAssetTypeEditor>("PolyTest");

    EXPECT_EQ(extension->GetEditorType(), EditorType::AssetTypeEditor);
    EXPECT_STREQ(extension->GetName(), "PolyTest");

    // Safe downcast
    auto* assetEditor = dynamic_cast<AssetTypeEditor*>(extension.get());
    ASSERT_NE(assetEditor, nullptr);
    EXPECT_STREQ(assetEditor->GetTypeName(), "PolyTest");
}

// ============================================================================
// Default Content Tests
// ============================================================================

TEST_F(AssetTypeEditorTest, DefaultContent_IsValidJson)
{
    MockAssetTypeEditor editor;
    const char* content = editor.GetDefaultContent();

    EXPECT_NE(content, nullptr);
    EXPECT_STRNE(content, "");

    // Should be parseable as JSON (just check it's not empty for now)
    EXPECT_EQ(content[0], '{');
}

TEST_F(AssetTypeEditorTest, DataAsset_HasProperDefaultContent)
{
    MockDataAssetEditor editor;
    const char* content = editor.GetDefaultContent();

    EXPECT_NE(content, nullptr);
    EXPECT_TRUE(std::string(content).find("data") != std::string::npos);
}
