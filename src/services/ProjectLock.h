#pragma once

#include <filesystem>
#include <string>

namespace DekiEditor
{

// Keeps a project open in one editor at a time. Any number of editors can run;
// two on the same project would rebuild the same package DLLs, write the same
// scenes and stash each other's package edits.
//
// The lock is an operating-system lock on <project>/generated/editor.lock, not
// the file's existence, so the system drops it when the editor exits or
// crashes and a stale file never keeps a project shut.
class ProjectLock
{
public:
    ProjectLock() = default;
    ~ProjectLock();

    ProjectLock(ProjectLock&& other) noexcept;
    ProjectLock& operator=(ProjectLock&& other) noexcept;
    ProjectLock(const ProjectLock&) = delete;
    ProjectLock& operator=(const ProjectLock&) = delete;

    // Takes the lock for the project at `projectRoot`. False when another
    // editor holds it, with `error` saying so in words for the user.
    bool Acquire(const std::filesystem::path& projectRoot, std::string& error);
    void Release();

    bool IsHeld() const;
    // Whether this is the lock of the project at `projectRoot`.
    bool IsFor(const std::filesystem::path& projectRoot) const;

    static std::filesystem::path LockFilePath(const std::filesystem::path& projectRoot);

private:
#ifdef _WIN32
    void* m_Handle = nullptr;
#else
    int m_Fd = -1;
#endif
    std::filesystem::path m_Root;
};

}  // namespace DekiEditor
