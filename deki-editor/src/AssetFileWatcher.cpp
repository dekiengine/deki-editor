#include <deki-editor/AssetFileWatcher.h>

#include <efsw/efsw.hpp>

#include <algorithm>
#include <filesystem>
#include <system_error>

#ifdef __APPLE__
#include <limits.h>
#include <sys/resource.h>
#endif

#include <deki/LogSystem.h>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{
bool IsSidecarExt(const std::string& ext)
{
    return ext == ".dekiguid" || ext == ".data";
}

#ifdef __APPLE__
// macOS starts a process with at most 256 open files, and efsw's kqueue backend
// keeps 500 of them spare, so it always fell back to polling, which missed
// renames. kqueue takes a descriptor per watched file: raise the soft limit as
// far as the system allows (OPEN_MAX) before efsw first reads it.
void RaiseOpenFileLimit()
{
    rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < OPEN_MAX)
    {
        limit.rlim_cur = std::min<rlim_t>(OPEN_MAX, limit.rlim_max);
        setrlimit(RLIMIT_NOFILE, &limit);
    }
}
#endif
}  // namespace

struct AssetFileWatcher::Impl : public efsw::FileWatchListener
{
    std::unique_ptr<efsw::FileWatcher> watcher;
    efsw::WatchID watchId = 0;
    bool watching = false;

    std::string watchPath;
    std::string projectPath;

    struct Pending
    {
        EventKind kind;
        std::chrono::steady_clock::time_point lastUpdated;
    };

    std::mutex mutex;
    std::unordered_map<std::string, Pending> pending;

    // Called by the efsw worker thread.
    void handleFileAction(efsw::WatchID /*watchid*/, const std::string& dir, const std::string& filename,
                          efsw::Action action, std::string oldFilename) override
    {
        const auto now = std::chrono::steady_clock::now();

        switch (action)
        {
            case efsw::Actions::Add: Submit(dir, filename, EventKind::Added, now); break;
            case efsw::Actions::Modified: Submit(dir, filename, EventKind::Modified, now); break;
            case efsw::Actions::Delete: Submit(dir, filename, EventKind::Deleted, now); break;
            case efsw::Actions::Moved:
                Submit(dir, oldFilename, EventKind::Deleted, now);
                Submit(dir, filename, EventKind::Added, now);
                break;
        }
    }

    void Submit(const std::string& dir, const std::string& filename, EventKind kind,
                std::chrono::steady_clock::time_point now)
    {
        std::string ext = fs::path(filename).extension().string();
        if (IsSidecarExt(ext))
        {
            return;
        }

        std::string relPath;
        if (!ToProjectRelative(dir, filename, relPath))
        {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex);
        Upsert(relPath, kind, now);
    }

    bool ToProjectRelative(const std::string& dir, const std::string& filename, std::string& outRel) const
    {
        std::error_code ec;
        fs::path abs = fs::path(dir) / filename;
        fs::path canonical = fs::weakly_canonical(abs, ec);
        if (ec)
        {
            canonical = abs;
        }

        fs::path rel = fs::relative(canonical, projectPath, ec);
        if (ec)
        {
            return false;
        }

        std::string relStr = rel.string();
        if (relStr.empty() || relStr.rfind("..", 0) == 0)
        {
            return false;
        }

        for (char& c : relStr)
        {
            if (c == '\\')
            {
                c = '/';
            }
        }
        outRel = std::move(relStr);
        return true;
    }

    // Caller holds `mutex`.
    void Upsert(const std::string& path, EventKind kind, std::chrono::steady_clock::time_point now)
    {
        auto it = pending.find(path);
        if (it == pending.end())
        {
            pending.emplace(path, Pending{ kind, now });
            return;
        }

        Pending& p = it->second;
        p.lastUpdated = now;

        switch (p.kind)
        {
            case EventKind::Added:
                if (kind == EventKind::Deleted)
                {
                    pending.erase(it);
                    return;
                }
                break;
            case EventKind::Modified:
                if (kind == EventKind::Deleted)
                {
                    p.kind = EventKind::Deleted;
                }
                break;
            case EventKind::Deleted:
                if (kind == EventKind::Added || kind == EventKind::Modified)
                {
                    p.kind = EventKind::Modified;
                }
                break;
        }
    }
};

AssetFileWatcher::AssetFileWatcher()
    : m_Impl(std::make_unique<Impl>())
{
}

AssetFileWatcher::~AssetFileWatcher()
{
    Stop();
}

void AssetFileWatcher::Start(const std::string& watchPath, const std::string& projectPath)
{
    Stop();

    Impl& impl = *m_Impl;
    impl.watchPath = watchPath;
    impl.projectPath = projectPath;
#ifdef __APPLE__
    RaiseOpenFileLimit();
#endif
    impl.watcher = std::make_unique<efsw::FileWatcher>();

    impl.watchId = impl.watcher->addWatch(watchPath, &impl, /*recursive*/ true);
    if (impl.watchId < 0)
    {
        DEKI_LOG_ERROR("AssetFileWatcher: addWatch failed for %s (efsw error %d)", watchPath.c_str(),
                       (int)impl.watchId);
        impl.watcher.reset();
        return;
    }

    impl.watcher->watch();
    impl.watching = true;
    DEKI_LOG_DEBUG("AssetFileWatcher: watching %s (id=%d)", watchPath.c_str(), (int)impl.watchId);
}

void AssetFileWatcher::Stop()
{
    Impl& impl = *m_Impl;
    if (!impl.watching)
    {
        impl.watcher.reset();
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.pending.clear();
        return;
    }

    if (impl.watcher && impl.watchId >= 0)
    {
        impl.watcher->removeWatch(impl.watchId);
    }

    // Destroying the FileWatcher joins its worker thread.
    impl.watcher.reset();
    impl.watching = false;
    impl.watchId = 0;

    std::lock_guard<std::mutex> lock(impl.mutex);
    impl.pending.clear();
}

std::vector<AssetFileWatcher::Event> AssetFileWatcher::DrainReadyEvents()
{
    std::vector<Event> out;
    Impl& impl = *m_Impl;
    if (!impl.watching)
    {
        return out;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto threshold = std::chrono::milliseconds(kDebounceMs);

    std::lock_guard<std::mutex> lock(impl.mutex);
    for (auto it = impl.pending.begin(); it != impl.pending.end();)
    {
        if (now - it->second.lastUpdated >= threshold)
        {
            out.push_back({ it->second.kind, it->first });
            it = impl.pending.erase(it);
        }
        else
        {
            ++it;
        }
    }
    return out;
}

}  // namespace DekiEditor
