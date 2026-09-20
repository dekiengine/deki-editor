#pragma once

#include <deki/providers/IFileSystem.h>
#include <string>
#include <functional>

namespace DekiEditor
{

class Project;
class AssetPipeline;

/**
 * @brief Filesystem implementation for editor play mode
 *
 * This filesystem works with absolute paths and also resolves
 * relative paths (like font atlas references) correctly.
 * It uses standard C file operations and works with the asset pipeline.
 */
class EditorPlayModeFileSystem : public Deki::IFileSystem
{
public:
    EditorPlayModeFileSystem();
    ~EditorPlayModeFileSystem() override;

    // Deki::IFileSystem interface
    bool Initialize() override;
    void Shutdown() override;
    FileHandle OpenFile(const char* path, OpenMode mode) override;
    void CloseFile(FileHandle handle) override;
    size_t ReadFile(FileHandle handle, void* buffer, size_t size) override;
    size_t WriteFile(FileHandle handle, const void* buffer, size_t size) override;
    long SeekFile(FileHandle handle, long offset, SeekOrigin origin) override;
    long TellFile(FileHandle handle) override;
    long GetFileSize(FileHandle handle) override;
    bool FileExists(const char* path) override;
    bool ConvertPath(const char* virtualPath, char* outBuffer, size_t bufferSize) override;

    /**
     * @brief Path resolver callback type
     *
     * Takes an asset-relative path (e.g., "scenes/Demo") and returns the
     * absolute path to the compiled/cached version of that asset.
     */
    using PathResolverCallback = std::function<std::string(const std::string&)>;

    /**
     * @brief Set the path resolver for asset-relative paths
     * @param resolver Callback that resolves asset paths to absolute cache paths
     */
    void SetPathResolver(PathResolverCallback resolver) { m_PathResolver = resolver; }

    /**
     * @brief Set the directory that the virtual "S:/" mount maps to.
     *
     * Must be set before any S:/ path is opened - UserData and other runtime
     * state writes through this mount. In playmode the SD card doesn't exist,
     * so EditorProjectManager points this at %APPDATA%/DekiEditor/<project>/
     * to keep runtime files out of the project tree (storage/ is for compiled
     * build artifacts only).
     */
    void SetStorageBasePath(const std::string& path) { m_StorageBasePath = path; }

private:
    std::string ResolvePath(const char* path);

    bool m_Initialized;
    PathResolverCallback m_PathResolver;
    std::string m_StorageBasePath;
};

}  // namespace DekiEditor
