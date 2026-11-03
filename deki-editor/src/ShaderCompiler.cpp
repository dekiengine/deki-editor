#include "deki-editor/ShaderCompiler.h"

#include "deki-editor/EditorPaths.h"

#include <slang.h>
#include <spirv_glsl.hpp>

#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace DekiEditor
{
namespace
{

#if defined(_WIN32)
constexpr const char* kLibraryName = "slang-compiler.dll";
#elif defined(__APPLE__)
constexpr const char* kLibraryName = "libslang-compiler.dylib";
#else
constexpr const char* kLibraryName = "libslang-compiler.so";
#endif

using CreateGlobalSessionFn = SlangResult (*)(const SlangGlobalSessionDesc* desc,
                                              slang::IGlobalSession** outGlobalSession);

/// Releases a Slang object when it goes out of scope.
template <typename T>
struct Ref
{
    T* p = nullptr;
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref()
    {
        if (p)
        {
            p->release();
        }
    }
};

std::string BlobText(slang::IBlob* blob)
{
    if (!blob || blob->getBufferSize() == 0)
    {
        return {};
    }
    const std::string all(static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize());
    // Without the lines Slang ends a failed compile with ("import failed",
    // "compilation ceased"), which only repeat that it failed.
    std::string text;
    size_t start = 0;
    while (start < all.size())
    {
        size_t end = all.find('\n', start);
        end = end == std::string::npos ? all.size() : end + 1;
        const std::string line = all.substr(start, end - start);
        if (line.find("[E39999]") == std::string::npos && line.find("[E40003]") == std::string::npos)
        {
            text += line;
        }
        start = end;
    }
    while (!text.empty() && (text.back() == '\0' || text.back() == '\n' || text.back() == '\r'))
    {
        text.pop_back();
    }
    return text;
}

/// Slang, loaded once: the library beside the editor, never one PATH finds,
/// and one global session, which loads Slang's core module (slow, so once).
struct Slang
{
    slang::IGlobalSession* global = nullptr;
    std::string why;  // why `global` is null

    Slang()
    {
        const std::string path = EditorPaths::GetExecutableDir() + "/" + kLibraryName;
        CreateGlobalSessionFn create = nullptr;
#ifdef _WIN32
        // Its own folder first for what it loads in turn (slang-glslang).
        HMODULE library = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (library)
        {
            create = reinterpret_cast<CreateGlobalSessionFn>(
                reinterpret_cast<void*>(GetProcAddress(library, "slang_createGlobalSession2")));
        }
#else
        void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (library)
        {
            create = reinterpret_cast<CreateGlobalSessionFn>(dlsym(library, "slang_createGlobalSession2"));
        }
#endif
        if (!library)
        {
            why = "the HLSL compiler (Slang) is missing: " + path + " does not load";
            return;
        }
        if (!create)
        {
            why = "the HLSL compiler (Slang) at " + path + " has no slang_createGlobalSession2";
            return;
        }
        SlangGlobalSessionDesc desc;
        if (SLANG_FAILED(create(&desc, &global)) || !global)
        {
            global = nullptr;
            why = "the HLSL compiler (Slang) did not start";
            return;
        }
    }
};

Slang& GetSlang()
{
    static Slang s_Slang;
    return s_Slang;
}

// Slang and SPIRV-Cross are used from whichever thread compiles an asset; one
// compile at a time keeps them simple.
std::mutex s_CompileMutex;

/// HLSL to SPIR-V. False with Slang's messages in `errors`; its warnings, if
/// it compiled, in `warnings`.
bool ToSpirv(const ShaderCompileRequest& request, std::vector<uint32_t>& spirv, std::string& errors,
             std::string& warnings)
{
    Slang& slangLib = GetSlang();
    if (!slangLib.global)
    {
        errors = request.fileName + ": " + slangLib.why;
        return false;
    }

    // NAME or NAME=VALUE.
    std::vector<std::string> names, values;
    for (const std::string& define : request.defines)
    {
        const size_t eq = define.find('=');
        names.push_back(define.substr(0, eq));
        values.push_back(eq == std::string::npos ? std::string() : define.substr(eq + 1));
    }
    std::vector<slang::PreprocessorMacroDesc> macros;
    for (size_t i = 0; i < names.size(); ++i)
    {
        macros.push_back({ names[i].c_str(), values[i].c_str() });
    }

    slang::TargetDesc target;
    target.format = SLANG_SPIRV;
    // No SPIR-V version: Slang picks its own (and warns about one it finds old);
    // SPIRV-Cross reads any.
    slang::SessionDesc desc;
    desc.targets = &target;
    desc.targetCount = 1;
    desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_ROW_MAJOR;  // see the header
    desc.preprocessorMacros = macros.data();
    desc.preprocessorMacroCount = static_cast<SlangInt>(macros.size());
    Ref<slang::ISession> session;
    if (SLANG_FAILED(slangLib.global->createSession(desc, &session.p)) || !session.p)
    {
        errors = request.fileName + ": the HLSL compiler (Slang) did not start a session";
        return false;
    }

    // Each step may say something; what fails says why in its diagnostics.
    std::string said;
    auto collect = [&](slang::IBlob* blob)
    {
        const std::string text = BlobText(blob);
        if (!text.empty())
        {
            said += (said.empty() ? "" : "\n") + text;
        }
    };
    auto fail = [&](const char* fallback)
    {
        errors = said.empty() ? request.fileName + ": " + fallback : said;
        return false;
    };

    Ref<slang::IBlob> diagnostics;
    // The path's extension makes it HLSL; #line in the source names the file.
    slang::IModule* module =
        session.p->loadModuleFromSourceString("shader", "shader.hlsl", request.source.c_str(), &diagnostics.p);
    collect(diagnostics.p);
    if (!module)
    {
        return fail("the HLSL compiler (Slang) rejected it without saying why");
    }

    Ref<slang::IEntryPoint> entry;
    Ref<slang::IBlob> entryDiagnostics;
    const SlangStage stage = request.stage == ShaderStage::Vertex ? SLANG_STAGE_VERTEX : SLANG_STAGE_FRAGMENT;
    if (SLANG_FAILED(module->findAndCheckEntryPoint(request.entry.c_str(), stage, &entry.p, &entryDiagnostics.p)) ||
        !entry.p)
    {
        collect(entryDiagnostics.p);
        return fail("has no entry point the HLSL compiler (Slang) could use");
    }
    collect(entryDiagnostics.p);

    slang::IComponentType* parts[] = { module, entry.p };
    Ref<slang::IComponentType> program;
    Ref<slang::IBlob> composeDiagnostics;
    if (SLANG_FAILED(session.p->createCompositeComponentType(parts, 2, &program.p, &composeDiagnostics.p)))
    {
        collect(composeDiagnostics.p);
        return fail("could not be put together by the HLSL compiler (Slang)");
    }
    Ref<slang::IComponentType> linked;
    Ref<slang::IBlob> linkDiagnostics;
    if (SLANG_FAILED(program.p->link(&linked.p, &linkDiagnostics.p)) || !linked.p)
    {
        collect(linkDiagnostics.p);
        return fail("could not be linked by the HLSL compiler (Slang)");
    }
    Ref<slang::IBlob> code;
    Ref<slang::IBlob> codeDiagnostics;
    if (SLANG_FAILED(linked.p->getEntryPointCode(0, 0, &code.p, &codeDiagnostics.p)) || !code.p ||
        code.p->getBufferSize() < 4)
    {
        collect(codeDiagnostics.p);
        return fail("made no code in the HLSL compiler (Slang)");
    }
    collect(codeDiagnostics.p);
    warnings = said;

    spirv.resize(code.p->getBufferSize() / 4);
    std::memcpy(spirv.data(), code.p->getBufferPointer(), spirv.size() * 4);
    return true;
}

/// SPIR-V to one GLSL dialect, with the names the header lists.
std::string ToGlsl(const std::vector<uint32_t>& spirv, ShaderStage stage, bool es)
{
    spirv_cross::CompilerGLSL glsl(spirv);
    spirv_cross::CompilerGLSL::Options options;
    options.es = es;
    options.version = es ? 300 : 330;
    options.enable_420pack_extension = false;
    // GL draws set uniforms one by one; ES 3.0 has blocks, but not every
    // uniform a draw sets would be in one.
    options.emit_uniform_buffer_as_plain_uniforms = true;
    // As the hand-written shaders had it: a fragment shader's floats highp,
    // so a value passed between the stages keeps its precision.
    options.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Highp;
    options.fragment.default_int_precision = spirv_cross::CompilerGLSL::Options::Highp;
    glsl.set_common_options(options);

    const spirv_cross::ShaderResources resources = glsl.get_shader_resources();
    auto byLocation = [&](const spirv_cross::Resource& r)
    { glsl.set_name(r.id, "vary" + std::to_string(glsl.get_decoration(r.id, spv::DecorationLocation))); };
    if (stage == ShaderStage::Vertex)
    {
        for (const auto& out : resources.stage_outputs)
        {
            byLocation(out);
        }
    }
    else
    {
        for (const auto& in : resources.stage_inputs)
        {
            byLocation(in);
        }
        for (const auto& out : resources.stage_outputs)
        {
            glsl.set_name(out.id, "fragColor");
        }
    }
    for (const auto& block : resources.uniform_buffers)
    {
        glsl.set_name(block.id, "u");
    }
    glsl.build_combined_image_samplers();
    for (const auto& combined : glsl.get_combined_image_samplers())
    {
        glsl.set_name(combined.combined_id, glsl.get_name(combined.image_id));
    }
    return glsl.compile();
}

/// The first #extension line of `glsl`, or empty: something the base
/// version cannot do, which a device would refuse.
std::string FirstExtension(const std::string& glsl)
{
    const size_t at = glsl.find("#extension");
    if (at == std::string::npos)
    {
        return {};
    }
    const size_t end = glsl.find('\n', at);
    return glsl.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

}  // namespace

bool ShaderCompiler::IsAvailable(std::string* why)
{
    std::lock_guard<std::mutex> lock(s_CompileMutex);
    Slang& slangLib = GetSlang();
    if (why)
    {
        *why = slangLib.why;
    }
    return slangLib.global != nullptr;
}

bool ShaderCompiler::Compile(const ShaderCompileRequest& request, ShaderStageText& out, std::string& errors,
                             std::string* warnings)
{
    std::lock_guard<std::mutex> lock(s_CompileMutex);
    errors.clear();
    std::vector<uint32_t> spirv;
    std::string said;
    if (!ToSpirv(request, spirv, errors, said))
    {
        return false;
    }
    if (warnings)
    {
        *warnings = said;
    }
    try
    {
        out.es = ToGlsl(spirv, request.stage, true);
        out.gl = ToGlsl(spirv, request.stage, false);
    }
    catch (const std::exception& e)
    {
        errors = request.fileName + ": cannot be written for OpenGL ES 3.0: " + e.what();
        return false;
    }
    const std::string extension = FirstExtension(out.es);
    if (!extension.empty())
    {
        errors = request.fileName + ": uses something OpenGL ES 3.0 does not have (" + extension + ")";
        return false;
    }
    return true;
}

}  // namespace DekiEditor
