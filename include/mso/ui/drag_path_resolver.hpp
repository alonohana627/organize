#pragma once
/// @file
/// DragPathResolver: resolves the playable local paths of the current selection in the
/// background, so a drag to a DAW can start instantly without file-system calls on the UI thread.
#include <QObject>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "mso/domain/error.hpp"
#include "mso/ui/sample_key.hpp"

namespace mso::ui {

class ApplicationSession;

/// Keeps Application::resolve_playable_path() results for the selected samples.
///
/// Every selection change starts a new resolution generation; results of older generations are
/// dropped. Paths are re-checked against the file system at selection time (the resolver stats
/// each file), which is the moment closest to the drag that does not block the UI thread.
class DragPathResolver final : public QObject {
    Q_OBJECT

public:
    /// Larger selections are not resolved; dragging them is refused with an explanation.
    static constexpr std::size_t kMaxDragSamples = 1000;

    explicit DragPathResolver(ApplicationSession& session, QObject* parent = nullptr);

    /// Starts resolving `keys` (replacing any earlier request).
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
    /// The resolution for the current selection finished.
    void resolved();

private:
    ApplicationSession& session_;
    std::uint64_t generation_ = 0;
    bool ready_ = false;
    std::vector<SampleKey> keys_;
    std::vector<Result<std::filesystem::path>> results_;  ///< parallel to keys_
};

}  // namespace mso::ui
