#pragma once
/// @file
/// RootLocations: the machine-local base path of every root of every open library, so file URLs
/// can be built at the moment a drag starts, without touching the disk.
#include <QObject>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <utility>

#include "mso/domain/core.hpp"
#include "mso/search/search_service.hpp"

namespace mso::ui {

class ApplicationSession;

/// Cache of (library, root id) → absolute root path, read in the background with
/// Application::root_path(). Reload it when libraries open or close, roots change, or a root's
/// binding may have moved.
class RootLocations final : public QObject {
    Q_OBJECT

public:
    /// Locations of `session`'s libraries; the session must outlive this object.
    explicit RootLocations(ApplicationSession& session, QObject* parent = nullptr);

    /// Re-reads every root of every open library (off the UI thread).
    void reload();
    /// Absolute path of a sample's first live location, or nullopt when its root is not bound on
    /// this computer (or not read yet).
    std::optional<std::filesystem::path> path_of(const search::SampleRow& row) const;

signals:
    /// A reload finished.
    void reloaded();

private:
    using RootKey = std::pair<uuids::uuid, std::int64_t>;

    /// Reads roots through the Application.
    ApplicationSession& session_;
    /// Absolute path of every bound root.
    std::map<RootKey, std::filesystem::path> roots_;
    /// Drops deliveries a newer reload superseded.
    std::uint64_t generation_ = 0;
};

}  // namespace mso::ui
