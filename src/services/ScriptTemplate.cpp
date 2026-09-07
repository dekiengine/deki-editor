#include "ScriptTemplate.h"

#include <filesystem>
#include <fstream>

namespace DekiEditor
{
namespace ScriptTemplate
{

bool CreateBehaviourScript(const std::string& directory, const std::string& name, std::string& error)
{
    namespace fs = std::filesystem;

    const std::string headerPath = (fs::path(directory) / (name + ".h")).string();
    const std::string sourcePath = (fs::path(directory) / (name + ".cpp")).string();

    // Never overwrite
    std::error_code ec;
    if (fs::exists(headerPath, ec) || fs::exists(sourcePath, ec))
    {
        error = "'" + name + ".h' or '" + name + ".cpp' already exists in " + directory;
        return false;
    }

    // Header: a Deki::Behaviour. <deki/Engine.h> provides every engine type
    // (Deki::Behaviour, DEKI_LOG, ...). Deriving publicly from Deki::Behaviour
    // (or Deki::Component for data-only pieces) is all it takes: the reflection
    // codegen finds the class by its base and identifies it by its name. The
    // markers above the class are optional editor metadata.
    const std::string headerContent =
        "#pragma once\n"
        "\n"
        "#include <deki/Engine.h>\n"
        "\n"
        "DEKI_CATEGORY(\"Game\")\n"
        "DEKI_DESCRIPTION(\"What " +
        name + " does, in one line.\")\n"
               "class " +
        name + " : public Deki::Behaviour\n"
               "{\n"
               "public:\n"
               "    // Exported properties (visible in Inspector)\n"
               "    DEKI_EXPORT\n"
               "    float speed = 1.0f;\n"
               "\n"
               "    void Awake() override;\n"
               "    void Start() override;\n"
               "    void Update() override;\n"
               "};\n";

    const std::string sourceContent =
        "#include \"" + name + ".h\"\n"
                               "\n"
                               "void " +
        name + "::Awake()\n"
               "{\n"
               "}\n"
               "\n"
               "void " +
        name + "::Start()\n"
               "{\n"
               "}\n"
               "\n"
               "void " +
        name + "::Update()\n"
               "{\n"
               "}\n";

    std::ofstream headerFile(headerPath);
    if (!headerFile.is_open())
    {
        error = "cannot write " + headerPath;
        return false;
    }
    headerFile << headerContent;
    headerFile.close();

    std::ofstream sourceFile(sourcePath);
    if (!sourceFile.is_open())
    {
        error = "cannot write " + sourcePath;
        return false;
    }
    sourceFile << sourceContent;
    return true;
}

}  // namespace ScriptTemplate
}  // namespace DekiEditor
