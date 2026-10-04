#include "EditorPlayModeFileSystem.h"
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace DekiEditor
{

EditorPlayModeFileSystem::EditorPlayModeFileSystem()
    : m_Initialized(false)
{
}

EditorPlayModeFileSystem::~EditorPlayModeFileSystem()
{
    Shutdown();
}

bool EditorPlayModeFileSystem::Initialize()
{
    if (m_Initialized)
    {
        return true;
    }

    m_Initialized = true;
    return true;
}

void EditorPlayModeFileSystem::Shutdown()
{
    m_Initialized = false;
    m_PathResolver = nullptr;
}

std::string EditorPlayModeFileSystem::ResolvePath(const char* path)
{
    if (!path)
    {
        return "";
    }

    std::string pathStr(path);

    // The virtual S:/ mount must be checked before is_absolute(): on Windows
    // "S:/..." counts as an absolute drive path, and fopen would then fail
    // with "no such drive".
    if (pathStr.length() >= 3 && pathStr.compare(0, 3, "S:/") == 0)
    {
        return m_StorageBasePath + pathStr.substr(3);
    }

    // Absolute paths are used as they are.
    fs::path fsPath(pathStr);
    if (fsPath.is_absolute())
    {
        return pathStr;
    }

    // Asset-relative paths go through the asset pipeline's resolver.
    if (m_PathResolver)
    {
        return m_PathResolver(pathStr);
    }

    return pathStr;
}

Deki::IFileSystem::FileHandle EditorPlayModeFileSystem::OpenFile(const char* path, OpenMode mode)
{
    if (!m_Initialized || !path)
    {
        return nullptr;
    }

    std::string realPath = ResolvePath(path);
    const char* modeStr = "";

    switch (mode)
    {
        case OpenMode::ReadBinary: modeStr = "rb"; break;
        case OpenMode::WriteBinary: modeStr = "wb"; break;
        case OpenMode::ReadText: modeStr = "r"; break;
        case OpenMode::WriteText: modeStr = "w"; break;
    }

    FILE* file = fopen(realPath.c_str(), modeStr);
    return static_cast<FileHandle>(file);
}

void EditorPlayModeFileSystem::CloseFile(FileHandle handle)
{
    if (handle)
    {
        fclose(static_cast<FILE*>(handle));
    }
}

size_t EditorPlayModeFileSystem::ReadFile(FileHandle handle, void* buffer, size_t size)
{
    if (!handle || !buffer)
    {
        return 0;
    }
    return fread(buffer, 1, size, static_cast<FILE*>(handle));
}

size_t EditorPlayModeFileSystem::WriteFile(FileHandle handle, const void* buffer, size_t size)
{
    if (!handle || !buffer)
    {
        return 0;
    }
    return fwrite(buffer, 1, size, static_cast<FILE*>(handle));
}

long EditorPlayModeFileSystem::SeekFile(FileHandle handle, long offset, SeekOrigin origin)
{
    if (!handle)
    {
        return -1;
    }

    int whence = SEEK_SET;
    switch (origin)
    {
        case SeekOrigin::Begin: whence = SEEK_SET; break;
        case SeekOrigin::Current: whence = SEEK_CUR; break;
        case SeekOrigin::End: whence = SEEK_END; break;
    }

    if (fseek(static_cast<FILE*>(handle), offset, whence) == 0)
    {
        return ftell(static_cast<FILE*>(handle));
    }
    return -1;
}

long EditorPlayModeFileSystem::TellFile(FileHandle handle)
{
    if (!handle)
    {
        return -1;
    }
    return ftell(static_cast<FILE*>(handle));
}

long EditorPlayModeFileSystem::GetFileSize(FileHandle handle)
{
    if (!handle)
    {
        return -1;
    }

    FILE* file = static_cast<FILE*>(handle);
    long currentPos = ftell(file);

    if (fseek(file, 0, SEEK_END) != 0)
    {
        return -1;
    }

    long size = ftell(file);

    fseek(file, currentPos, SEEK_SET);

    return size;
}

bool EditorPlayModeFileSystem::FileExists(const char* path)
{
    if (!m_Initialized || !path)
    {
        return false;
    }

    std::string realPath = ResolvePath(path);
    return fs::exists(realPath);
}

bool EditorPlayModeFileSystem::ConvertPath(const char* virtualPath, char* outBuffer, size_t bufferSize)
{
    if (!virtualPath || !outBuffer || bufferSize == 0)
    {
        return false;
    }

    std::string converted = ResolvePath(virtualPath);
    if (converted.length() >= bufferSize)
    {
        return false;
    }

    strcpy(outBuffer, converted.c_str());
    return true;
}

}  // namespace DekiEditor
