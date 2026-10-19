#pragma once
/// @file
/// NavigatorPanel: libraries → roots → folders, tags, categories and collections. Selecting a node
/// sets the navigation scope of the sample table (R14, R15).
#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "mso/app/application.hpp"
#include "mso/catalog/edit_command.hpp"
#include "mso/catalog/types.hpp"
#include "mso/ui/navigation_scope.hpp"

class QMenu;
class QTreeWidget;
class QTreeWidgetItem;

namespace mso::ui {

class ApplicationSession;

/// Navigation tree. The whole tree of every library — roots with all their folders, tags,
/// categories and collections — is read on a worker pool and held in memory, so expanding and
/// clicking never wait. It is rebuilt only when the structure changes (a library opens or closes:
/// reload(); one library's roots, folders or taxonomy change: reload_library()), never on a timer.
/// Per library, at most one outline read is in flight and reads start at most once per
/// kLibraryReloadIntervalMilliseconds (an import changes the structure every 150 ms); a change
/// arriving meanwhile marks the library dirty and is read afterwards, so the newest outline always
/// wins. Expansion, the current node and the scroll position survive rebuilds. Structural edits
/// (rename, merge, collections) are requested from the main window, which runs them and reports
/// the outcome.
///
/// Menus and dialogs run nested event loops in which a rebuild can delete any tree item, so no
/// QTreeWidgetItem pointer is kept across them: menu actions capture the node's values (NodeInfo)
/// and look items up again when they run.
///
/// A QTreeWidget is enough here: the tree is rebuilt only on structure changes, and its size is the
/// number of folders and tags, not samples, so a custom QAbstractItemModel would add code without a
/// measurable gain.
class NavigatorPanel final : public QWidget {
    Q_OBJECT

public:
    /// Outline reads of one library start at most this often (structure changes are coalesced).
    static constexpr int kLibraryReloadIntervalMilliseconds = 1000;

    /// A navigator over `session`'s libraries; the session must outlive the panel.
    explicit NavigatorPanel(ApplicationSession& session, QWidget* parent = nullptr);

    /// Re-reads every open library's roots, folders, tags, categories and collections.
    void reload();
    /// Re-reads one library (its structure changed) and replaces only its subtree. Coalesced:
    /// see the class comment.
    void reload_library(const uuids::uuid& library);
    /// Number of outline reads started since construction (tests check the coalescing).
    std::uint64_t outline_reads_started() const { return outline_reads_started_; }
    /// Library of the current node, if any (used for Add folder, Close library, undo routing).
    std::optional<uuids::uuid> current_library() const;
    /// Root of the current node (a root or one of its folders), if any.
    std::optional<std::int64_t> current_root() const;
    /// Updates one root's availability label without a full reload.
    void set_root_availability(const uuids::uuid& library, std::int64_t root_id, catalog::Availability availability);
    /// The scope of the current node (all libraries when nothing is selected).
    NavigationScope current_scope() const;

signals:
    /// The current node changed: search in its scope.
    void scopeSelected(const mso::ui::NavigationScope& scope);
    /// Rename, move or merge a tag, rename a category, create, rename or delete a collection.
    void taxonomyCommandRequested(const uuids::uuid& library, const mso::catalog::TaxonomyCommand& command);
    /// "New smart collection from the current search" (the main window knows the query).
    void smartCollectionRequested(const uuids::uuid& library, const QString& name);
    /// Add a sample folder to `library`.
    void addFolderRequested(const uuids::uuid& library);
    /// Rescan a root.
    void rescanRequested(const uuids::uuid& library, std::int64_t root_id);
    /// Locate a moved root.
    void rebindRequested(const uuids::uuid& library, std::int64_t root_id);
    /// Close a library.
    void closeLibraryRequested(const uuids::uuid& library);
    /// Back up a library.
    void backupRequested(const uuids::uuid& library);
    /// One-line feedback for the status bar (e.g. a folder that is offline).
    void statusMessage(const QString& message);

private:
    /// Everything the tree shows about one library, read on a worker.
    struct LibraryOutline {
        /// Library identity.
        uuids::uuid library;
        /// Display name.
        QString name;
        /// Opened read-only.
        bool read_only = false;
        /// Sample folders.
        std::vector<catalog::RootRecord> roots;
        /// Tag tree (flat, with parent ids).
        std::vector<catalog::TaxonomyNode> tags;
        /// Category tree (flat, with parent ids).
        std::vector<catalog::TaxonomyNode> categories;
        /// Manual and smart collections.
        std::vector<catalog::CollectionRecord> collections;
        std::map<std::int64_t, std::vector<catalog::FolderRecord>> folders;  ///< by root id
        QString error;  ///< why part of the outline could not be read (empty when complete)
    };

    /// The values of one tree node a menu action needs. Captured by value before a menu or dialog
    /// opens, so nothing refers to a tree item a rebuild may delete meanwhile.
    struct NodeInfo {
        int kind = 0;              ///< NodeKind of the node
        uuids::uuid library;       ///< library of the node
        std::int64_t id = 0;       ///< tag, category, collection or folder id
        std::int64_t root_id = 0;  ///< root of a root or folder node
        QString path;              ///< folder relative path, or tag/category display path
    };

    /// Coalescing state of one library's outline reads.
    struct LibraryReloadState {
        bool in_flight = false;           ///< a read is running
        bool dirty = false;               ///< the structure changed since the last read started
        QElapsedTimer last_started;       ///< when the last read started (invalid before the first)
        std::uint64_t started_order = 0;  ///< read_order_ value when the last read started
    };

    /// Reads one library's outline (worker thread).
    static LibraryOutline read_outline(app::Application& application, const app::OpenLibraryInfo& info);
    /// Rebuilds the whole tree from `outlines`.
    void populate(const std::vector<LibraryOutline>& outlines);
    /// Replaces one library's subtree.
    void replace_library(const LibraryOutline& outline);
    /// Fills a library item's subtree (groups, roots, folders, taxonomy, collections).
    void fill_library(QTreeWidgetItem* library_item, const LibraryOutline& outline);
    /// Re-applies expansion, the current node and the scroll position after a rebuild; selects
    /// `fallback` (and announces its scope) when the previous current node disappeared.
    void restore_tree_state(const QString& previous_identity, int scroll_position, QTreeWidgetItem* fallback);
    /// Builds a tag or category subtree under `parent` (merged tags are skipped).
    void add_taxonomy(QTreeWidgetItem* parent, const LibraryOutline& outline,
                      const std::vector<catalog::TaxonomyNode>& nodes, int node_kind);
    /// Builds the folder subtree of one root under `root_item` (the root folder is the item itself).
    void add_folders(QTreeWidgetItem* root_item, const uuids::uuid& library, std::int64_t root_id,
                     const std::vector<catalog::FolderRecord>& folders);
    /// Resolves a root or folder to its local path off the UI thread and opens it in the file manager.
    void show_folder_in_file_manager(const uuids::uuid& library, std::int64_t root_id, const QString& relative_path);
    /// Starts the reads of dirty libraries whose interval has passed; schedules the others.
    void start_due_library_reloads();
    /// Starts one library's outline read now.
    void start_library_read(const uuids::uuid& library);
    /// Menu of the node under `position` (rename, rescan, show in folder …).
    void show_context_menu(const QPoint& position);
    /// Menu entries of a library or its Folders group.
    void add_library_actions(QMenu& menu, const NodeInfo& node);
    /// Menu entries of a root or folder.
    void add_folder_actions(QMenu& menu, const NodeInfo& node);
    /// Menu entries of a tag.
    void add_tag_actions(QMenu& menu, const NodeInfo& node);
    /// Menu entries of a category.
    void add_category_actions(QMenu& menu, const NodeInfo& node);
    /// Menu entries of the Collections group or a collection.
    void add_collection_actions(QMenu& menu, const NodeInfo& node);
    /// Asks for a name (nullopt when cancelled or empty).
    std::optional<QString> ask_name(const QString& title, const QString& current);
    /// Display paths and ids of the tags of `library` that can receive the tag `tag_id` (merge or
    /// move target): not the tag itself and not one of its descendants (that would create a
    /// cycle). Reads the tree as it is now.
    std::vector<std::pair<QString, std::int64_t>> tag_targets(const uuids::uuid& library, std::int64_t tag_id) const;
    /// The values of a tree node.
    static NodeInfo node_info(const QTreeWidgetItem* item);
    /// Announces the current node's scope.
    void emit_current_scope();
    /// Stable text identity of a node, used to restore expansion and the current node.
    static QString identity_of(const QTreeWidgetItem* item);
    /// Search scope and label of a node.
    NavigationScope scope_of(const QTreeWidgetItem* item) const;

    /// Reads the outlines through the Application.
    ApplicationSession& session_;
    /// The tree widget.
    QTreeWidget* tree_ = nullptr;
    /// Drops outline deliveries a newer full reload superseded.
    std::uint64_t reload_generation_ = 0;
    /// Counts read starts (full and per library), so a full read can tell which per-library reads
    /// started after it: their newer outline must not be replaced by the full read's older one.
    std::uint64_t read_order_ = 0;
    /// Coalescing state of reload_library() per library.
    std::map<uuids::uuid, LibraryReloadState> library_reloads_;
    /// Starts deferred library reads once their interval has passed.
    QTimer library_reload_timer_;
    /// Outline reads started (full and per library).
    std::uint64_t outline_reads_started_ = 0;
    /// Identities of expanded nodes (survive rebuilds).
    std::set<QString> expanded_;
    /// Identity of the current node (survives rebuilds).
    QString current_identity_;
    /// Suppresses scope announcements while the tree is rebuilt.
    bool populating_ = false;
};

}  // namespace mso::ui
