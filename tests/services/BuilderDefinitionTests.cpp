// The toolchain definition a build backend supplies as JSON.

#include <gtest/gtest.h>

#include <deki-editor/build/BuilderDefinition.h>

using namespace DekiEditor;

// A component's PATH entries are read per platform, in order: ESP-IDF's
// install.bat runs `python` by name and must find the toolchain's own copy.
TEST(BuilderDefinition, PathEntriesAreReadPerPlatform)
{
    const char* json = R"json({
      "id": "espidf",
      "name": "ESP-IDF",
      "components": [
        {
          "id": "python",
          "installPath": "espressif/python",
          "detection": { "windows": "{installPath}/tools/python.exe" },
          "pathEntries": {
            "windows": ["{installPath}/tools", "{installPath}/tools/Scripts"],
            "unix": ["{installPath}/bin"]
          }
        },
        { "id": "esptool", "installPath": "espressif/esptool" }
      ]
    })json";

    BuilderDefinition def;
    std::string error;
    ASSERT_TRUE(ParseBuilderDefinition(json, def, error)) << error;
    ASSERT_EQ(def.components.size(), 2u);

    const ComponentDef& python = def.components[0];
    ASSERT_EQ(python.pathEntries.count("windows"), 1u);
    EXPECT_EQ(python.pathEntries.at("windows"),
              (std::vector<std::string>{ "{installPath}/tools", "{installPath}/tools/Scripts" }));
    EXPECT_EQ(python.pathEntries.at("unix"), (std::vector<std::string>{ "{installPath}/bin" }));
    EXPECT_TRUE(def.components[1].pathEntries.empty()) << "a component without pathEntries adds none";
}
