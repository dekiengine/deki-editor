#pragma once

#include <deki-editor/EditorExport.h>

// HLSL in, the GLSL an OpenGL device runs out.
//
// Shaders are written in HLSL. Every device Deki draws on with a GPU runs
// OpenGL ES 3.0 or desktop OpenGL 3.3, which take GLSL, so the editor
// translates: Slang (Khronos's shader compiler, which takes HLSL) makes
// SPIR-V, and SPIRV-Cross writes it as GLSL ES 3.00 and GLSL 3.30. The game
// ships the GLSL; no compiler goes to a device.
//
// The GLSL follows fixed names, so the code that draws with it can find
// things without asking:
//
//   uniforms   one struct, `u`: HLSL's `float uTime;` is "u.uTime"
//   textures   a Texture2D and the samplers it is used with become one
//              sampler2D named as the Texture2D
//   varyings   vary<location>, the same in both stages
//   output     fragColor
//   matrices   row-major: mul(M, v) with M uploaded transposed
//
// Slang is slang-compiler (.dll, .so, .dylib) beside the editor, with
// slang-glslang, which it optimises SPIR-V with. It is loaded on first use;
// without it nothing compiles, and the error says why.

#include <string>
#include <vector>

namespace DekiEditor
{

enum class ShaderStage
{
    Vertex,
    Fragment,
};

struct ShaderCompileRequest
{
    std::string source;      // the whole HLSL, with #line directives for the user's file
    std::string fileName;    // the user's file, for errors that have no line
    ShaderStage stage = ShaderStage::Fragment;
    std::string entry;       // the entry point, e.g. "FragmentMain"
    std::vector<std::string> defines;  // NAME or NAME=VALUE
};

/// One stage, in both dialects.
struct ShaderStageText
{
    std::string es;  // GLSL ES 3.00, for OpenGL ES 3.0 and WebGL 2
    std::string gl;  // GLSL 3.30, for desktop OpenGL 3.3
};

class DEKI_EDITOR_API ShaderCompiler
{
public:
    /// Whether Slang is here and loads. False with `why` when not.
    static bool IsAvailable(std::string* why = nullptr);

    /// Compiles one stage. False with `errors` (Slang's messages, each
    /// naming the file, line and column) when Slang rejects it, or when it
    /// uses something OpenGL ES 3.0 cannot run. What Slang warns about when it
    /// compiles goes to `warnings`.
    static bool Compile(const ShaderCompileRequest& request, ShaderStageText& out, std::string& errors,
                        std::string* warnings = nullptr);
};

}  // namespace DekiEditor
