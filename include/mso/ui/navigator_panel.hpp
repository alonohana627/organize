#pragma once
/// @file
/// NavigatorPanel: libraries → roots → folders, tags, categories and collections. Selecting a node
/// sets the navigation scope of the sample table (R14, R15).
#include <QString>
#include <QWidget>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "mso/catalog/edit_command.hpp"
#include "mso/catalog/types.hpp"
#include "mso/ui/navigation_scope.hpp"

class QTreeWidget;
class QTreeWidgetItem;

namespace mso::ui {

class ApplicationSession;

/// Navigation tree. Its contents are read on a worker pool and rebuilt on demand; folders of a
/// root are loaded only when that root is expanded. Expansion and the current node survive
/// rebuilds. Structural edits (rename, merge, collections) are requested from the main window,
/// which runs them and reports the outcome.
class NavigatorPanel final : public QWidget {
    Q_OBJECT

public:
    explicit NavigatorPanel(ApplicationSession& session, QWidget* parent = nullptr);

    /// Re-reads every open library's roots, tags, categories and collections.
    void reload();
    /// Library of the current node, if any (used for Add folder, Close library, undo routing).
    std::optional<uuids::uuid> current_library() const;
    /// Root of the current node (a root or one of its folders), if any.
    std::optional<std::int64_t> current_root() const;
    /// Updates one root's availability label without a full reload.
    void set_root_availability(const uuids::uuid& library, std::int64_t root_id, catalog::Availability availability);
    /// The scope of the current node (all libraries when nothing is selected).
    NavigationScope current_scope() const;

signals:
    void scopeSelected(const mso::ui::NavigationScope& scope);
    void taxonomyCommandRequested(const uuids::uuid& library, const mso::catalog::TaxonomyCommand& command);
    /// "New smart collection from the current search" (the main window knows the query).
    void smartCollectionRequested(const uuids::uuid& library, const QString& name);
    void addFolderRequested(const uuids::uuid& library);
    void rescanRequested(const uuids::uuid& library, std::int64_t root_id);
    void rebindRequested(const uuids::uuid& library, std::int64_t root_id);
    void closeLibraryRequested(const uuids::uuid& library);
    void backupRequested(const uuids::uuid& library);
    /// One-line feedback for the status bar (e.g. a folder that is offline).
    void statusMessage(const QString& message);

private:
    /// Everything the tree shows about one library, read on a worker.
    struct LibraryOutline {
        uuids::uuid library;
        QString name;
        bool read_only = false;
        std::vector<catalog::RootRecord> roots;
        std::vector<catalog::TaxonomyNode> tags;
        std::vector<catalog::TaxonomyNode> categories;
        std::vector<catalog::CollectionRecord> collections;
        std::map<std::int64_t, std::vector<catalog::FolderRecord>> folders;  ///< by root id
        QString error;
    };

    void populate(const std::vector<LibraryOutline>& outlines);
    void add_taxonomy(QTreeWidgetItem* parent, const LibraryOutline& outline,
                      const std::vector<catalog::TaxonomyNode>& nodes, int node_kind);
    /// Builds the folder subtree of one root under `root_item` (the root folder is the item itself).
    void add_folders(QTreeWidgetItem* root_item, const uuids::uuid& library, std::int64_t root_id,
                     const std::vector<catalog::FolderRecord>& folders);
    /// Resolves a root or folder to its local path off the UI thread and opens it in the file manager.
    void show_folder_in_file_manager(const uuids::uuid& library, std::int64_t root_id, const QString& relative_path);
    void show_context_menu(const QPoint& position);
    void emit_current_scope();
    /// Stable text identity of a node, used to restore expansion and the current node.
    static QString identity_of(const QTreeWidgetItem* item);
    NavigationScope scope_of(const QTreeWidgetItem* item) const;

    ApplicationSession& session_;
    QTreeWidget* tree_ = nullptr;
    std::uint64_t reload_generation_ = 0;
    std::set<QString> expanded_;
    QString current_identity_;
    bool populating_ = false;
};

}  // namespace mso::ui
