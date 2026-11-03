#include "deki-editor/ShaderCompiler.h"

#include "deki-editor/EditorPaths.h"

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
// Not in an anonymous namespace: GCC would then know no class in this file
// implements them and turn every call into a pure-virtual-call abort.
namespace DxcInterfaces
{

// ---------------------------------------------------------------------------
// DXC's interfaces, declared by hand. dxcapi.h needs MSVC on Windows (and
// DXC's WinAdapter elsewhere); these are the few methods used, in the order
// DXC's vtables have them. The layout is COM's on every platform DXC ships
// for: three IUnknown methods, then the interface's own.
// ---------------------------------------------------------------------------

#ifdef _WIN32
#define DEKI_DXC_CALL __stdcall
#else
#define DEKI_DXC_CALL
#endif

using HResult = int32_t;
inline bool Failed(HResult hr)
{
    return hr < 0;
}

struct DxcGuid
{
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

struct DxcUnknown
{
    virtual HResult DEKI_DXC_CALL QueryInterface(const DxcGuid* iid, void** object) = 0;
    virtual uint32_t DEKI_DXC_CALL AddRef() = 0;
    virtual uint32_t DEKI_DXC_CALL Release() = 0;
};

struct DxcBlob : DxcUnknown
{
    virtual void* DEKI_DXC_CALL GetBufferPointer() = 0;
    virtual size_t DEKI_DXC_CALL GetBufferSize() = 0;
};

struct DxcBlobEncoding : DxcBlob
{
    virtual HResult DEKI_DXC_CALL GetEncoding(int32_t* known, uint32_t* codePage) = 0;
};

struct DxcOperationResult : DxcUnknown
{
    virtual HResult DEKI_DXC_CALL GetStatus(HResult* status) = 0;
    virtual HResult DEKI_DXC_CALL GetResult(DxcBlob** result) = 0;
    virtual HResult DEKI_DXC_CALL GetErrorBuffer(DxcBlobEncoding** errors) = 0;
};

struct DxcBuffer
{
    const void* ptr;
    size_t size;
    uint32_t encoding;
};

struct DxcCompiler3 : DxcUnknown
{
    virtual HResult DEKI_DXC_CALL Compile(const DxcBuffer* source, const wchar_t** args, uint32_t argCount,
                                          DxcUnknown* includeHandler, const DxcGuid* iid, void** result) = 0;
};

using DxcCreateInstanceFn = HResult(DEKI_DXC_CALL*)(const DxcGuid* clsid, const DxcGuid* iid, void** object);

}  // namespace DxcInterfaces

namespace
{
using namespace DxcInterfaces;

constexpr DxcGuid kClsidDxcCompiler = { 0x73e22d93, 0xe6ce, 0x47f3, { 0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0 } };
constexpr DxcGuid kIidDxcCompiler3 = { 0x228b4687, 0x5a6a, 0x4730, { 0x90, 0x0c, 0x97, 0x02, 0xb2, 0x20, 0x3f, 0x54 } };
constexpr DxcGuid kIidDxcOperationResult = {
    0xcedb484a, 0xd4e9, 0x445a, { 0xb9, 0x91, 0xca, 0x21, 0xca, 0x15, 0x7d, 0xc2 }
};
constexpr uint32_t kUtf8 = 65001;

/// Releases a DXC object when it goes out of scope.
template <typename T>
struct Ref
{
    T* p = nullptr;
    ~Ref()
    {
        if (p)
        {
            p->Release();
        }
    }
    void** Out() { return reinterpret_cast<void**>(&p); }
};

#if defined(_WIN32)
constexpr const char* kLibraryName = "dxcompiler.dll";
#elif defined(__APPLE__)
constexpr const char* kLibraryName = "libdxcompiler.dylib";
#else
constexpr const char* kLibraryName = "libdxcompiler.so";
#endif

/// DXC, loaded once: the library beside the editor, never one PATH finds.
struct Dxc
{
    DxcCreateInstanceFn create = nullptr;
    std::string why;  // why `create` is null

    Dxc()
    {
        const std::string path = EditorPaths::GetExecutableDir() + "/" + kLibraryName;
#ifdef _WIN32
        HMODULE library = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (library)
        {
            create = reinterpret_cast<DxcCreateInstanceFn>(
                reinterpret_cast<void*>(GetProcAddress(library, "DxcCreateInstance")));
        }
#else
        void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (library)
        {
            create = reinterpret_cast<DxcCreateInstanceFn>(dlsym(library, "DxcCreateInstance"));
        }
#endif
        if (!library)
        {
#ifdef __APPLE__
            why = "the HLSL compiler (DXC) is not available on macOS yet, so shaders cannot be compiled here";
#else
            why = "the HLSL compiler (DXC) is missing: " + path + " does not load";
#endif
        }
        else if (!create)
        {
            why = "the HLSL compiler (DXC) at " + path + " has no DxcCreateInstance";
        }
    }
};

Dxc& GetDxc()
{
    static Dxc s_Dxc;
    return s_Dxc;
}

// DXC and SPIRV-Cross are used from whichever thread compiles an asset; one
// compile at a time keeps them simple.
std::mutex s_CompileMutex;

std::wstring Widen(const std::string& text)
{
    // Arguments are ASCII (entry points, defines, profile names); a file
    // name that is not only labels the source, and #line carries the real one.
    std::wstring wide;
    wide.reserve(text.size());
    for (unsigned char c : text)
    {
        wide.push_back(c < 0x80 ? static_cast<wchar_t>(c) : L'_');
    }
    return wide;
}

std::string BlobText(DxcBlob* blob)
{
    if (!blob || blob->GetBufferSize() == 0)
    {
        return {};
    }
    std::string text(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
    while (!text.empty() && (text.back() == '\0' || text.back() == '\n' || text.back() == '\r'))
    {
        text.pop_back();
    }
    return text;
}

/// HLSL to SPIR-V. False with DXC's messages in `errors`.
bool ToSpirv(const ShaderCompileRequest& request, std::vector<uint32_t>& spirv, std::string& errors)
{
    Dxc& dxc = GetDxc();
    if (!dxc.create)
    {
        errors = request.fileName + ": " + dxc.why;
        return false;
    }
    Ref<DxcCompiler3> compiler;
    if (Failed(dxc.create(&kClsidDxcCompiler, &kIidDxcCompiler3, compiler.Out())) || !compiler.p)
    {
        errors = request.fileName + ": the HLSL compiler (DXC) did not start";
        return false;
    }

    std::vector<std::wstring> owned = {
        Widen(request.fileName),
        L"-spirv",
        L"-T",
        request.stage == ShaderStage::Vertex ? L"vs_6_0" : L"ps_6_0",
        L"-E",
        Widen(request.entry),
        L"-fspv-target-env=vulkan1.0",
        L"-Zpr",  // matrices row-major (see the header)
        L"-O3",
    };
    for (const std::string& define : request.defines)
    {
        owned.push_back(L"-D");
        owned.push_back(Widen(define));
    }
    std::vector<const wchar_t*> args;
    for (const std::wstring& arg : owned)
    {
        args.push_back(arg.c_str());
    }

    const DxcBuffer source{ request.source.data(), request.source.size(), kUtf8 };
    Ref<DxcOperationResult> result;
    if (Failed(compiler.p->Compile(&source, args.data(), static_cast<uint32_t>(args.size()), nullptr,
                                   &kIidDxcOperationResult, result.Out())) ||
        !result.p)
    {
        errors = request.fileName + ": the HLSL compiler (DXC) failed to run";
        return false;
    }
    HResult status = 0;
    result.p->GetStatus(&status);
    if (Failed(status))
    {
        Ref<DxcBlobEncoding> messages;
        result.p->GetErrorBuffer(&messages.p);
        errors = BlobText(messages.p);
        if (errors.empty())
        {
            errors = request.fileName + ": the HLSL compiler (DXC) rejected it without saying why";
        }
        return false;
    }
    Ref<DxcBlob> code;
    result.p->GetResult(&code.p);
    if (!code.p || code.p->GetBufferSize() < 4)
    {
        errors = request.fileName + ": the HLSL compiler (DXC) made no code";
        return false;
    }
    spirv.resize(code.p->GetBufferSize() / 4);
    std::memcpy(spirv.data(), code.p->GetBufferPointer(), spirv.size() * 4);
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
    Dxc& dxc = GetDxc();
    if (why)
    {
        *why = dxc.why;
    }
    return dxc.create != nullptr;
}

bool ShaderCompiler::Compile(const ShaderCompileRequest& request, ShaderStageText& out, std::string& errors)
{
    std::lock_guard<std::mutex> lock(s_CompileMutex);
    errors.clear();
    std::vector<uint32_t> spirv;
    if (!ToSpirv(request, spirv, errors))
    {
        return false;
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
