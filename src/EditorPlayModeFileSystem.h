#pragma once

#include <deki/providers/IFileSystem.h>
#include <string>
#include <functional>

namespace DekiEditor
{

class Project;
class AssetPipeline;

/// The filesystem for editor play mode. It takes absolute paths and also
/// resolves relative ones (such as font atlas references) through the asset
/// pipeline, using standard C file operations.
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

    /// Takes an asset-relative path (such as "scenes/Demo") and returns the
    /// absolute path to the compiled, cached version of that asset.
    using PathResolverCallback = std::function<std::string(const std::string&)>;

    /// Sets the callback that resolves asset-relative paths to absolute cache
    /// paths.
    void SetPathResolver(PathResolverCallback resolver) { m_PathResolver = resolver; }

    /// Sets the directory that the virtual "S:/" mount maps to. Must be set
    /// before any S:/ path is opened: UserData and other runtime state write
    /// through this mount. Play mode has no SD card, so EditorProjectManager
    /// points this at %APPDATA%/DekiEditor/<project>/ to keep runtime files
    /// out of the project tree (storage/ holds compiled build output only).
    void SetStorageBasePath(const std::string& path) { m_StorageBasePath = path; }

private:
    std::string ResolvePath(const char* path);

    bool m_Initialized;
    PathResolverCallback m_PathResolver;
    std::string m_StorageBasePath;
};

}  // namespace DekiEditor
