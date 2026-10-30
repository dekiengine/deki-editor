#include "EditorRenderer.h"

#include <deki-editor/EditorTexture.h>

#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "imgui_impl_sdlgpu3_shaders.h"  // its vertex shader, for the text pipelines
#include "misc/freetype/imgui_freetype.h"

#include "TextShaders.h"

#include <deki/LogSystem.h>

#include <algorithm>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace DekiEditor::EditorRenderer
{

namespace
{

SDL_GPUDevice* s_Device = nullptr;
SDL_Window* s_Window = nullptr;
SDL_GPUTextureFormat s_Format = SDL_GPU_TEXTUREFORMAT_INVALID;
bool s_ImGui = false;
bool s_Vsync = true;

// The frame, drawn here and then copied to the window (see the header).
SDL_GPUTexture* s_Frame = nullptr;
uint32_t s_FrameWidth = 0;
uint32_t s_FrameHeight = 0;
bool s_HaveFrame = false;  // s_Frame holds the last frame drawn

SDL_GPUTransferBuffer* s_Readback = nullptr;
uint32_t s_ReadbackSize = 0;

// Subpixel text's passes, one per colour channel (see shaders/text.frag),
// handed to ImGui's backend.
SDL_GPUGraphicsPipeline* s_TextPasses[3] = {};

// ---- Textures (EditorTexture) ----

// A texture's id is its slot's index + 1, so 0 stays "none".
struct Slot
{
    SDL_GPUTexture* texture = nullptr;
    SDL_GPUTransferBuffer* upload = nullptr;  // kept for its next update
    uint32_t uploadSize = 0;
};
std::vector<Slot> s_Slots;
std::vector<uint32_t> s_FreeSlots;

// RGB565 to RGBA8, as four bytes R, G, B, A in a word. Each channel is widened
// by repeating its top bits, as drivers do.
const uint32_t* Rgb565ToRgba()
{
    static const std::vector<uint32_t> kTable = []
    {
        std::vector<uint32_t> t(65536);
        for (uint32_t v = 0; v < 65536; ++v)
        {
            const uint32_t r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
            const uint32_t r = (r5 << 3) | (r5 >> 2);
            const uint32_t g = (g6 << 2) | (g6 >> 4);
            const uint32_t b = (b5 << 3) | (b5 >> 2);
            t[v] = r | (g << 8) | (b << 16) | 0xFF000000u;
        }
        return t;
    }();
    return kTable.data();
}

Slot* FindSlot(uint32_t texture)
{
    if (texture == 0 || texture > s_Slots.size() || !s_Slots[texture - 1].texture)
    {
        return nullptr;
    }
    return &s_Slots[texture - 1];
}

uint32_t CreateTexture(int width, int height)
{
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<Uint32>(width);
    info.height = static_cast<Uint32>(height);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(s_Device, &info);
    if (!texture)
    {
        DEKI_LOG_ERROR("EditorTexture: %dx%d texture: %s", width, height, SDL_GetError());
        return 0;
    }

    uint32_t index;
    if (!s_FreeSlots.empty())
    {
        index = s_FreeSlots.back();
        s_FreeSlots.pop_back();
    }
    else
    {
        index = static_cast<uint32_t>(s_Slots.size());
        s_Slots.emplace_back();
    }
    s_Slots[index].texture = texture;
    return index + 1;
}

void UpdateTexture(uint32_t texture, int x, int y, int width, int height, EditorTexture::Format format,
                   const void* pixels, int rowPixels)
{
    Slot* slot = FindSlot(texture);
    if (!slot)
    {
        return;
    }

    const uint32_t bytes = static_cast<uint32_t>(width) * height * 4;
    if (bytes > slot->uploadSize)
    {
        if (slot->upload)
        {
            SDL_ReleaseGPUTransferBuffer(s_Device, slot->upload);
        }
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = bytes;
        slot->upload = SDL_CreateGPUTransferBuffer(s_Device, &info);
        slot->uploadSize = slot->upload ? bytes : 0;
        if (!slot->upload)
        {
            return;
        }
    }

    // Cycled: an upload still in flight keeps its own copy.
    auto* dst = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(s_Device, slot->upload, true));
    if (!dst)
    {
        return;
    }
    if (format == EditorTexture::Format::Rgb565)
    {
        const uint32_t* toRgba = Rgb565ToRgba();
        for (int row = 0; row < height; ++row)
        {
            const uint16_t* in = static_cast<const uint16_t*>(pixels) + static_cast<size_t>(row) * rowPixels;
            uint32_t* out = reinterpret_cast<uint32_t*>(dst) + static_cast<size_t>(row) * width;
            for (int i = 0; i < width; ++i)
            {
                out[i] = toRgba[in[i]];
            }
        }
    }
    else
    {
        for (int row = 0; row < height; ++row)
        {
            std::memcpy(dst + static_cast<size_t>(row) * width * 4,
                        static_cast<const uint8_t*>(pixels) + static_cast<size_t>(row) * rowPixels * 4,
                        static_cast<size_t>(width) * 4);
        }
    }
    SDL_UnmapGPUTransferBuffer(s_Device, slot->upload);

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(s_Device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = slot->upload;
    source.pixels_per_row = static_cast<Uint32>(width);
    source.rows_per_layer = static_cast<Uint32>(height);
    SDL_GPUTextureRegion region{};
    region.texture = slot->texture;
    region.x = static_cast<Uint32>(x);
    region.y = static_cast<Uint32>(y);
    region.w = static_cast<Uint32>(width);
    region.h = static_cast<Uint32>(height);
    region.d = 1;
    SDL_UploadToGPUTexture(copy, &source, &region, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(commands);
}

void DestroyTexture(uint32_t texture)
{
    Slot* slot = FindSlot(texture);
    if (!slot)
    {
        return;
    }
    // Released once the GPU is done with it.
    SDL_ReleaseGPUTexture(s_Device, slot->texture);
    if (slot->upload)
    {
        SDL_ReleaseGPUTransferBuffer(s_Device, slot->upload);
    }
    *slot = Slot{};
    s_FreeSlots.push_back(texture - 1);
}

ImTextureID TextureToImGui(uint32_t texture)
{
    const Slot* slot = FindSlot(texture);
    return slot ? (ImTextureID)(intptr_t)slot->texture : ImTextureID_Invalid;
}

const EditorTexture::Backend kTextures = { CreateTexture, UpdateTexture, DestroyTexture, TextureToImGui };

// ---- Frames ----

bool EnsureFrame(uint32_t width, uint32_t height)
{
    if (s_Frame && s_FrameWidth == width && s_FrameHeight == height)
    {
        return true;
    }
    if (s_Frame)
    {
        SDL_ReleaseGPUTexture(s_Device, s_Frame);
        s_Frame = nullptr;
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = s_Format;
    // A colour target for ImGui, and sampled when copied to the window.
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    s_Frame = SDL_CreateGPUTexture(s_Device, &info);
    s_FrameWidth = s_Frame ? width : 0;
    s_FrameHeight = s_Frame ? height : 0;
    s_HaveFrame = false;
    return s_Frame != nullptr;
}

SDL_GPUPresentMode PresentMode(bool vsync)
{
    if (vsync)
    {
        return SDL_GPU_PRESENTMODE_VSYNC;
    }
    if (SDL_WindowSupportsGPUPresentMode(s_Device, s_Window, SDL_GPU_PRESENTMODE_IMMEDIATE))
    {
        return SDL_GPU_PRESENTMODE_IMMEDIATE;
    }
    if (SDL_WindowSupportsGPUPresentMode(s_Device, s_Window, SDL_GPU_PRESENTMODE_MAILBOX))
    {
        return SDL_GPU_PRESENTMODE_MAILBOX;
    }
    return SDL_GPU_PRESENTMODE_VSYNC;
}

}  // namespace

bool IsMinimized(SDL_Window* window)
{
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
    {
        return true;
    }
#ifdef _WIN32
    HWND hwnd =
        (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    return hwnd && IsIconic(hwnd);
#else
    return false;
#endif
}

SDL_GPUDevice* CreateDevice()
{
    // The shader formats ImGui's SDL GPU backend ships: SPIR-V for Vulkan,
    // DXBC for Direct3D 12, Metal's for Metal.
    const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXBC |
                                        SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB;
#ifdef _WIN32
    // SDL would pick Vulkan first. Windows' own API is the one every driver
    // there is tested with; NVIDIA's Vulkan driver crashed asked for the
    // image of a window SDL did not know was minimized.
    if (SDL_GPUDevice* device = SDL_CreateGPUDevice(formats, false, "direct3d12"))
    {
        return device;
    }
#endif
    return SDL_CreateGPUDevice(formats, false, nullptr);
}

bool Attach(SDL_Window* window, std::string& failedStep)
{
    s_Device = CreateDevice();
    if (!s_Device)
    {
        failedStep = "SDL_CreateGPUDevice";
        return false;
    }
    if (!SDL_ClaimWindowForGPUDevice(s_Device, window))
    {
        failedStep = "SDL_ClaimWindowForGPUDevice";
        SDL_DestroyGPUDevice(s_Device);
        s_Device = nullptr;
        return false;
    }
    s_Window = window;
    s_Format = SDL_GetGPUSwapchainTextureFormat(s_Device, window);
    DEKI_LOG_EDITOR("Drawing with SDL's GPU API on %s", DriverName());
    EditorTexture::SetBackend(&kTextures);
    return true;
}

bool InitImGui(SDL_Window* window)
{
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo info;
    info.Device = s_Device;
    info.ColorTargetFormat = s_Format;
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    s_ImGui = ImGui_ImplSDLGPU3_Init(&info);
    return s_ImGui;
}

namespace
{
// One pass of subpixel text: ImGui's vertex shader and vertex layout, a text
// fragment shader, and an alpha blend that writes only `channel`. Null when
// the device refuses it.
SDL_GPUGraphicsPipeline* CreateTextPipeline(SDL_GPUShaderFormat format, const uint8_t* vertexCode, size_t vertexSize,
                                            const uint8_t* fragmentCode, size_t fragmentSize,
                                            SDL_GPUColorComponentFlags channel)
{
    SDL_GPUShaderCreateInfo vertexInfo{};
    vertexInfo.code = vertexCode;
    vertexInfo.code_size = vertexSize;
    vertexInfo.entrypoint = "main";
    vertexInfo.format = format;
    vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vertexInfo.num_uniform_buffers = 1;
    SDL_GPUShaderCreateInfo fragmentInfo{};
    fragmentInfo.code = fragmentCode;
    fragmentInfo.code_size = fragmentSize;
    fragmentInfo.entrypoint = "main";
    fragmentInfo.format = format;
    fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fragmentInfo.num_samplers = 1;
    SDL_GPUShader* vertex = SDL_CreateGPUShader(s_Device, &vertexInfo);
    SDL_GPUShader* fragment = SDL_CreateGPUShader(s_Device, &fragmentInfo);

    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    if (vertex && fragment)
    {
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        buffer.pitch = sizeof(ImDrawVert);
        SDL_GPUVertexAttribute attributes[3]{};
        attributes[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(ImDrawVert, pos) };
        attributes[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(ImDrawVert, uv) };
        attributes[2] = { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(ImDrawVert, col) };

        // The shader's alpha is this channel's coverage.
        SDL_GPUColorTargetDescription target{};
        target.format = s_Format;
        target.blend_state.enable_blend = true;
        target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.enable_color_write_mask = true;
        target.blend_state.color_write_mask = channel;

        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = 3;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = 1;
        pipeline = SDL_CreateGPUGraphicsPipeline(s_Device, &info);
    }
    if (vertex)
    {
        SDL_ReleaseGPUShader(s_Device, vertex);
    }
    if (fragment)
    {
        SDL_ReleaseGPUShader(s_Device, fragment);
    }
    return pipeline;
}

void ReleaseTextPipelines()
{
    ImGui_ImplSDLGPU3_DekiSetTextPipelines(nullptr, 0);
    for (SDL_GPUGraphicsPipeline*& pass : s_TextPasses)
    {
        if (pass)
        {
            SDL_ReleaseGPUGraphicsPipeline(s_Device, pass);
            pass = nullptr;
        }
    }
}
}  // namespace

int SetSubpixelText(int mode)
{
    ReleaseTextPipelines();
    const std::string driver = s_ImGui ? DriverName() : "";
    const bool vulkan = driver == "vulkan";
    if (mode == 0 || (!vulkan && driver != "direct3d12"))
    {
        ImGuiFreeType::DekiSetLcdMode(0);
        return 0;
    }

    struct Shader
    {
        const uint8_t* code;
        size_t size;
    };
    const Shader passes[3] = {
        vulkan ? Shader{ kText_channel0_spirv, sizeof(kText_channel0_spirv) }
               : Shader{ kText_channel0_dxbc, sizeof(kText_channel0_dxbc) },
        vulkan ? Shader{ kText_channel1_spirv, sizeof(kText_channel1_spirv) }
               : Shader{ kText_channel1_dxbc, sizeof(kText_channel1_dxbc) },
        vulkan ? Shader{ kText_channel2_spirv, sizeof(kText_channel2_spirv) }
               : Shader{ kText_channel2_dxbc, sizeof(kText_channel2_dxbc) },
    };
    const SDL_GPUColorComponentFlags channels[3] = { SDL_GPU_COLORCOMPONENT_R, SDL_GPU_COLORCOMPONENT_G,
                                                     SDL_GPU_COLORCOMPONENT_B };
    const SDL_GPUShaderFormat format = vulkan ? SDL_GPU_SHADERFORMAT_SPIRV : SDL_GPU_SHADERFORMAT_DXBC;
    const Shader vertex =
        vulkan ? Shader{ spirv_vertex, sizeof(spirv_vertex) } : Shader{ dxbc_vertex, sizeof(dxbc_vertex) };
    for (int i = 0; i < 3; ++i)
    {
        s_TextPasses[i] =
            CreateTextPipeline(format, vertex.code, vertex.size, passes[i].code, passes[i].size, channels[i]);
        if (!s_TextPasses[i])
        {
            DEKI_LOG_WARNING("Subpixel text is off: %s", SDL_GetError());
            ReleaseTextPipelines();
            ImGuiFreeType::DekiSetLcdMode(0);
            return 0;
        }
    }
    ImGui_ImplSDLGPU3_DekiSetTextPipelines(s_TextPasses, 3);
    ImGuiFreeType::DekiSetLcdMode(mode);
    return mode;
}

void Shutdown()
{
    if (!s_Device)
    {
        return;
    }
    SDL_WaitForGPUIdle(s_Device);
    EditorTexture::SetBackend(nullptr);
    ReleaseTextPipelines();
    if (s_ImGui)
    {
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        s_ImGui = false;
    }
    for (Slot& slot : s_Slots)
    {
        if (slot.texture)
        {
            SDL_ReleaseGPUTexture(s_Device, slot.texture);
        }
        if (slot.upload)
        {
            SDL_ReleaseGPUTransferBuffer(s_Device, slot.upload);
        }
    }
    s_Slots.clear();
    s_FreeSlots.clear();
    if (s_Frame)
    {
        SDL_ReleaseGPUTexture(s_Device, s_Frame);
        s_Frame = nullptr;
    }
    if (s_Readback)
    {
        SDL_ReleaseGPUTransferBuffer(s_Device, s_Readback);
        s_Readback = nullptr;
    }
    SDL_ReleaseWindowFromGPUDevice(s_Device, s_Window);
    SDL_DestroyGPUDevice(s_Device);
    s_Device = nullptr;
    s_Window = nullptr;
}

const char* DriverName()
{
    return s_Device ? SDL_GetGPUDeviceDriver(s_Device) : "none";
}

void NewFrame()
{
    ImGui_ImplSDLGPU3_NewFrame();
}

bool Render(ImDrawData* drawData, const ImVec4& clear, bool waitForVsync)
{
    const uint32_t width = static_cast<uint32_t>(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const uint32_t height = static_cast<uint32_t>(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (width == 0 || height == 0 || !EnsureFrame(width, height))
    {
        return false;
    }

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(s_Device);
    ImGui_ImplSDLGPU3_PrepareDrawData(drawData, commands);

    SDL_GPUColorTargetInfo target{};
    target.texture = s_Frame;
    target.clear_color = SDL_FColor{ clear.x, clear.y, clear.z, clear.w };
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);
    ImGui_ImplSDLGPU3_RenderDrawData(drawData, commands, pass);
    SDL_EndGPURenderPass(pass);
    s_HaveFrame = true;

    // Minimized, the frame is drawn but not shown. The window's image is not
    // even asked for: SDL may not know it is minimized, and a driver asked
    // for an image of a window with no size can crash (NVIDIA's Vulkan did).
    SDL_GPUTexture* window = nullptr;
    Uint32 windowWidth = 0, windowHeight = 0;
    const bool minimized = IsMinimized(s_Window);
    if (!minimized && waitForVsync)
    {
        SDL_WaitAndAcquireGPUSwapchainTexture(commands, s_Window, &window, &windowWidth, &windowHeight);
    }
    else if (!minimized)
    {
        SDL_AcquireGPUSwapchainTexture(commands, s_Window, &window, &windowWidth, &windowHeight);
    }
    if (window)
    {
        SDL_GPUBlitInfo blit{};
        blit.source.texture = s_Frame;
        blit.source.w = std::min(width, windowWidth);
        blit.source.h = std::min(height, windowHeight);
        blit.destination.texture = window;
        blit.destination.w = blit.source.w;
        blit.destination.h = blit.source.h;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(commands, &blit);
    }
    SDL_SubmitGPUCommandBuffer(commands);
    return window != nullptr;
}

bool ReadPixels(int x, int y, int width, int height, uint8_t* rgba)
{
    if (!s_HaveFrame || width <= 0 || height <= 0 || x < 0 || y < 0 ||
        static_cast<uint32_t>(x + width) > s_FrameWidth || static_cast<uint32_t>(y + height) > s_FrameHeight)
    {
        return false;
    }

    const uint32_t bytes = static_cast<uint32_t>(width) * height * 4;
    if (bytes > s_ReadbackSize)
    {
        if (s_Readback)
        {
            SDL_ReleaseGPUTransferBuffer(s_Device, s_Readback);
        }
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = bytes;
        s_Readback = SDL_CreateGPUTransferBuffer(s_Device, &info);
        s_ReadbackSize = s_Readback ? bytes : 0;
        if (!s_Readback)
        {
            return false;
        }
    }

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(s_Device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureRegion region{};
    region.texture = s_Frame;
    region.x = static_cast<Uint32>(x);
    region.y = static_cast<Uint32>(y);
    region.w = static_cast<Uint32>(width);
    region.h = static_cast<Uint32>(height);
    region.d = 1;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = s_Readback;
    destination.pixels_per_row = static_cast<Uint32>(width);
    destination.rows_per_layer = static_cast<Uint32>(height);
    SDL_DownloadFromGPUTexture(copy, &region, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence)
    {
        return false;
    }
    SDL_WaitForGPUFences(s_Device, true, &fence, 1);
    SDL_ReleaseGPUFence(s_Device, fence);

    const auto* src = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(s_Device, s_Readback, false));
    if (!src)
    {
        return false;
    }
    std::memcpy(rgba, src, bytes);
    SDL_UnmapGPUTransferBuffer(s_Device, s_Readback);

    // Window textures are usually BGRA.
    if (s_Format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || s_Format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB)
    {
        for (uint32_t i = 0; i < bytes; i += 4)
        {
            std::swap(rgba[i], rgba[i + 2]);
        }
    }
    return true;
}

void SetVsync(bool on)
{
    if (s_Device && on != s_Vsync)
    {
        SDL_SetGPUSwapchainParameters(s_Device, s_Window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, PresentMode(on));
        s_Vsync = on;
    }
}

}  // namespace DekiEditor::EditorRenderer
