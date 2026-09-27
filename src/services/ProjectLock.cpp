#include "ProjectLock.h"

#include <deki-editor/build/ProjectPaths.h>

#include <fstream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{
fs::path Normalized(const fs::path& p)
{
    std::error_code ec;
    fs::path out = fs::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : out;
}

// What the holder wrote into the file: its process id, for the message.
std::string ReadHolder(const fs::path& lockFile)
{
    std::ifstream in(lockFile);
    std::string pid;
    std::getline(in, pid);
    return pid;
}

std::string HeldMessage(const fs::path& lockFile)
{
    const std::string pid = ReadHolder(lockFile);
    std::string msg = "This project is already open in another Deki Editor";
    if (!pid.empty())
        msg += " (process " + pid + ")";
    return msg + ". Close it there first.";
}
}  // namespace

fs::path ProjectLock::LockFilePath(const fs::path& projectRoot)
{
    return ProjectPaths::Generated(projectRoot.string()) / "editor.lock";
}

ProjectLock::~ProjectLock()
{
    Release();
}

ProjectLock::ProjectLock(ProjectLock&& other) noexcept
{
    *this = std::move(other);
}

ProjectLock& ProjectLock::operator=(ProjectLock&& other) noexcept
{
    if (this != &other)
    {
        Release();
#ifdef _WIN32
        m_Handle = std::exchange(other.m_Handle, nullptr);
#else
        m_Fd = std::exchange(other.m_Fd, -1);
#endif
        m_Root = std::move(other.m_Root);
        other.m_Root.clear();
    }
    return *this;
}

bool ProjectLock::IsHeld() const
{
#ifdef _WIN32
    return m_Handle != nullptr;
#else
    return m_Fd >= 0;
#endif
}

bool ProjectLock::IsFor(const fs::path& projectRoot) const
{
    return IsHeld() && m_Root == Normalized(projectRoot);
}

bool ProjectLock::Acquire(const fs::path& projectRoot, std::string& error)
{
    Release();

    const fs::path file = LockFilePath(projectRoot);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);

#ifdef _WIN32
    // No FILE_SHARE_WRITE: a second editor's open fails with a sharing
    // violation for as long as this handle lives. FILE_SHARE_READ lets it read
    // who holds the lock.
    HANDLE h = CreateFileW(file.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION)
            error = HeldMessage(file);
        else
            error = "Could not create " + file.string() + " (error " + std::to_string(err) + ")";
        return false;
    }
    SetEndOfFile(h);
    const std::string pid = std::to_string(GetCurrentProcessId()) + "\n";
    DWORD written = 0;
    WriteFile(h, pid.data(), static_cast<DWORD>(pid.size()), &written, nullptr);
    FlushFileBuffers(h);
    m_Handle = h;
#else
    // flock locks belong to the open file description, so a second open of the
    // same file conflicts even inside one process, and the kernel drops the
    // lock with the last descriptor, crash included.
    const int fd = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        error = "Could not create " + file.string() + " (errno " + std::to_string(errno) + ")";
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        const int err = errno;
        close(fd);
        if (err == EWOULDBLOCK)
            error = HeldMessage(file);
        else
            error = "Could not lock " + file.string() + " (errno " + std::to_string(err) + ")";
        return false;
    }
    if (ftruncate(fd, 0) == 0)
    {
        const std::string pid = std::to_string(getpid()) + "\n";
        const ssize_t n = write(fd, pid.data(), pid.size());
        (void)n;
    }
    m_Fd = fd;
#endif

    m_Root = Normalized(projectRoot);
    return true;
}

void ProjectLock::Release()
{
#ifdef _WIN32
    if (m_Handle)
    {
        CloseHandle(static_cast<HANDLE>(m_Handle));
        m_Handle = nullptr;
    }
#else
    if (m_Fd >= 0)
    {
        flock(m_Fd, LOCK_UN);
        close(m_Fd);
        m_Fd = -1;
    }
#endif
    m_Root.clear();
}

}  // namespace DekiEditor
