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

/**
 * @brief OS-level file watcher that feeds AssetPipeline::Tick().
 *
 * Wraps an efsw::FileWatcher (ReadDirectoryChangesW / inotify / FSEvents) running
 * on its own worker thread. The listener pushes raw events into a thread-safe
 * map keyed by project-relative path. AssetPipeline::Tick() calls
 * DrainReadyEvents() once per frame to receive coalesced, debounced events on
 * the main thread - required because downstream onAssetChanged subscribers
 * touch GPU resources.
 *
 * Coalescing rules per path (when a new event arrives for a path already pending):
 *   pending=Added,    new=Modified  -> keep Added
 *   pending=Added,    new=Deleted   -> drop entry (created+deleted before drain)
 *   pending=Modified, new=Modified  -> keep Modified, refresh timestamp
 *   pending=Modified, new=Deleted   -> switch to Deleted
 *   pending=Deleted,  new=Added     -> switch to Modified (replaced)
 *   pending=Deleted,  new=Modified  -> switch to Modified
 *   pending=Deleted,  new=Deleted   -> keep Deleted
 *
 * Renames arrive from efsw as a Moved action with both old and new filenames.
 * The watcher emits two pending entries: Deleted(oldPath) + Added(newPath).
 *
 * Debounce: an entry is only returned by DrainReadyEvents() once it has been
 * quiet for >= kDebounceMs (default 100ms). This collapses the typical
 * write-temp / delete-original / rename-temp atomic-save sequence into one
 * dispatched event per logical change.
 */
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

    /**
     * @brief Start watching a directory recursively.
     * @param watchPath Absolute path to the directory to watch (recursive).
     *                  Typically project/assets, but the class is generic - also
     *                  used by HotReloadService for project/src and project/packages.
     * @param projectPath Absolute path to the project root, used to compute
     *                    project-relative paths for emitted events.
     */
    void Start(const std::string& watchPath, const std::string& projectPath);

    /**
     * @brief Stop the watcher thread and clear pending events.
     */
    void Stop();

    /**
     * @brief Drain coalesced events that have been quiet for >= kDebounceMs.
     *
     * Must be called on the main thread. Entries still inside the debounce
     * window remain pending and may be returned by a subsequent call.
     */
    std::vector<Event> DrainReadyEvents();

    static constexpr int kDebounceMs = 100;

private:
    // Listener implementation lives in the .cpp file so this header doesn't
    // pull in efsw publicly. Pimpl-style.
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

}  // namespace DekiEditor
