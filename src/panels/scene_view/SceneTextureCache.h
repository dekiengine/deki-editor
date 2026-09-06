#pragma once

#include "SceneViewTypes.h"
#include <string>
#include <unordered_map>

namespace DekiEditor
{

class EditorApp;

class SceneTextureCache
{
public:
    SceneTextureCache(EditorApp* app);
    ~SceneTextureCache();

    // Get or load a texture by asset reference (GUID or path)
    CachedTexture* GetOrLoad(const std::string& assetRef);

    // Invalidate a specific texture from cache (call when asset is recompiled)
    void Invalidate(const std::string& assetRef);

    // Clean up all cached textures
    void Cleanup();

    // Byte budget for the CPU-side pixel copies (the GPU textures are freed with
    // them). Least-recently-used entries are evicted when a load exceeds it.
    void SetByteBudget(size_t bytes) { m_ByteBudget = bytes; }
    size_t GetByteBudget() const { return m_ByteBudget; }
    size_t GetTotalBytes() const { return m_TotalBytes; }

private:
    void EvictToBudget(const std::string& keep);

    EditorApp* m_App;
    std::unordered_map<std::string, CachedTexture> m_Cache;
    size_t m_TotalBytes = 0;
    size_t m_ByteBudget = 64u * 1024u * 1024u;
    uint64_t m_UseCounter = 0;
};

}  // namespace DekiEditor
