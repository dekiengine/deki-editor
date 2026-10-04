#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <memory>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

/// OS file watcher that feeds AssetPipeline::Tick().
///
/// Wraps an efsw::FileWatcher (ReadDirectoryChangesW / inotify / FSEvents)
/// on its own worker thread. Raw events go into a thread-safe map keyed by
/// project-relative path. AssetPipeline::Tick() calls DrainReadyEvents() once
/// per frame to take the merged, debounced events on the main thread, which
/// is required because onAssetChanged subscribers touch GPU resources.
///
/// When a new event arrives for a path that already has one pending:
///   pending=Added,    new=Modified  -> keep Added
///   pending=Added,    new=Deleted   -> drop entry (created+deleted before drain)
///   pending=Modified, new=Modified  -> keep Modified, refresh timestamp
///   pending=Modified, new=Deleted   -> switch to Deleted
///   pending=Deleted,  new=Added     -> switch to Modified (replaced)
///   pending=Deleted,  new=Modified  -> switch to Modified
///   pending=Deleted,  new=Deleted   -> keep Deleted
///
/// A rename (efsw's Moved action) becomes Deleted(oldPath) + Added(newPath).
///
/// An entry is returned only once it has been quiet for kDebounceMs. This
/// turns the usual atomic save (write temp, delete original, rename temp)
/// into one event per change.
class DEKI_EDITOR_API AssetFileWatcher
{
public:
    enum class EventKind
    {
        Added,
        Modified,
        Deleted,
    };

    struct Event
    {
        EventKind kind;
        std::string relativePath;  // project-relative, forward slashes
    };

    AssetFileWatcher();
    ~AssetFileWatcher();

    AssetFileWatcher(const AssetFileWatcher&) = delete;
    AssetFileWatcher& operator=(const AssetFileWatcher&) = delete;

    /// Starts watching `watchPath` (absolute) and everything under it.
    /// Usually project/assets; HotReloadService also uses it for project/src
    /// and project/packages. Event paths are made relative to `projectPath`.
    void Start(const std::string& watchPath, const std::string& projectPath);

    /// Stops the watcher thread and clears pending events.
    void Stop();

    /// Returns the events that have been quiet for kDebounceMs. Main thread
    /// only. Newer entries stay pending for a later call.
    std::vector<Event> DrainReadyEvents();

    static constexpr int kDebounceMs = 100;

private:
    // Lives in the .cpp so this header does not pull in efsw.
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

}  // namespace DekiEditor
