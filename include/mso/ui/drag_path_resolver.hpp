#pragma once
/// @file
/// DragPathResolver: checks in the background whether the selected samples' files are really
/// there, so drags and copy actions can refuse unavailable files without asking the disk on the
/// UI thread.
#include <QObject>
#include <QThreadPool>
#include <QTimer>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <vector>

#include "mso/domain/error.hpp"
#include "mso/ui/sample_key.hpp"

namespace mso::ui {

class ApplicationSession;

/// Keeps Application::resolve_playable_path() results for the selection.
///
/// Selection changes are debounced (kDebounceMilliseconds) and run on a dedicated one-thread pool,
/// so file checks never delay searches or edits on the session's pool. A newer selection cancels
/// the running check between two files (stop token) and its result is dropped.
///
/// The check itself is a PathLookup: Application::resolve_playable_path through the session, or
/// any thread-safe function (tests check the debounce, cancellation and bookkeeping with one).
class DragPathResolver final : public QObject {
    Q_OBJECT

public:
    /// Larger selections are not checked; dragging them is refused with an explanation.
    static constexpr std::size_t kMaxDragSamples = 1000;
    /// Quiet time after the last selection change before the check starts.
    static constexpr int kDebounceMilliseconds = 150;

    /// Resolves one sample's file (called on the resolver's own pool thread, one key at a time).
    using PathLookup = std::function<Result<std::filesystem::path>(const SampleKey& key)>;

    /// Resolves through `session`, which must outlive the resolver.
    explicit DragPathResolver(ApplicationSession& session, QObject* parent = nullptr);
    /// Resolves through `lookup`, which must be safe to call from another thread.
    explicit DragPathResolver(PathLookup lookup, QObject* parent = nullptr);
    /// Cancels the running check and waits for it.
    ~DragPathResolver() override;

    /// Schedules a check of `keys` (replacing any earlier request).
    void resolve(std::vector<SampleKey> keys);
    /// True when the last resolve() for exactly `keys` has finished.
    bool is_ready_for(const std::vector<SampleKey>& keys) const;
    /// Resolved paths of available samples, in selection order.
    std::vector<std::filesystem::path> available_paths() const;
    /// Keys whose resolution failed (offline root, missing file, access denied).
    std::vector<SampleKey> unavailable_keys() const;
    /// Keys that resolved to a playable path.
    std::vector<SampleKey> available_keys() const;
    /// Path of one resolved key, if it resolved and is available.
    std::optional<std::filesystem::path> path_of(const SampleKey& key) const;

signals:
    /// The check for the current selection finished.
    void resolved();

private:
    /// Starts the debounced check on the resolver's own pool.
    void start_check();
    /// Stores a finished check of generation `generation` (dropped when superseded or cancelled).
    void finish_check(std::uint64_t generation, std::vector<Result<std::filesystem::path>> results);
    /// Resolves `keys` one after another with `lookup` until `stop` is requested; a stopped check
    /// returns fewer results than keys.
    static std::vector<Result<std::filesystem::path>> check_keys(const std::vector<SampleKey>& keys,
                                                                 const PathLookup& lookup, const std::stop_token& stop);

    /// Resolves through the Application (null when a PathLookup was given).
    ApplicationSession* session_ = nullptr;
    /// The lookup given instead of a session (empty with a session).
    PathLookup lookup_;
    QThreadPool pool_;              ///< one thread, separate from the session's pool
    QTimer debounce_;               ///< waits kDebounceMilliseconds after the last selection change
    std::stop_source stop_;         ///< cancels the running check
    std::uint64_t generation_ = 0;  ///< drops deliveries of superseded checks
    /// results_ belong to keys_ and are complete.
    bool ready_ = false;
    bool checking_ = false;                               ///< a check of keys_ is running on the pool
    std::vector<SampleKey> keys_;                         ///< selection being (or last) checked
    std::vector<Result<std::filesystem::path>> results_;  ///< parallel to keys_
    std::map<SampleKey, std::size_t> index_of_key_;       ///< position in keys_ (path_of() per drag row)
};

}  // namespace mso::ui
