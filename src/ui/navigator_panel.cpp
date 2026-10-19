#include "mso/ui/navigator_panel.hpp"

#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QScrollBar>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <system_error>
#include <vector>

#include "mso/ui/application_session.hpp"
#include "mso/ui/detail/parent_cycles.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/file_manager.hpp"

namespace mso::ui {

namespace {

/// What a tree item stands for.
enum NodeKind {
    AllLibrariesNode = 1,
    LibraryNode,
    RootsGroupNode,
    RootNode,
    FolderNode,
    TagsGroupNode,
    TagNode,
    CategoriesGroupNode,
    CategoryNode,
    UncategorizedNode,
    CollectionsGroupNode,
    CollectionNode,
    PlaceholderNode,
};

enum ItemRole {
    KindRole = Qt::UserRole + 1,
    LibraryRole,   ///< QString UUID
    IdRole,        ///< qlonglong: tag/category/collection/folder id
    RootRole,      ///< qlonglong root id (roots and folders)
    PathRole,      ///< QString: folder relative path, or tag/category display path
    RootNameRole,  ///< QString: a root's name without the availability suffix
};

int kind_of(const QTreeWidgetItem* item) {
    return item ? item->data(0, KindRole).toInt() : 0;
}

QTreeWidgetItem* make_item(QTreeWidgetItem* parent, const QString& text, int kind, const uuids::uuid& library) {
    auto* item = new QTreeWidgetItem(parent, QStringList{text});
    item->setData(0, KindRole, kind);
    item->setData(0, LibraryRole, DisplayFormat::from_uuid(library));
    return item;
}

/// Root label: its name, plus its availability when it is not online.
QString root_text(const QString& name, catalog::Availability availability) {
    if (availability == catalog::Availability::Online) return name;
    return NavigatorPanel::tr("%1 — %2").arg(name, DisplayFormat::root_availability(availability));
}

/// True when `candidate` is `ancestor` or lies below it.
bool is_same_or_descendant(const QTreeWidgetItem* candidate, const QTreeWidgetItem* ancestor) {
    for (const QTreeWidgetItem* node = candidate; node; node = node->parent())
        if (node == ancestor) return true;
    return false;
}

}  // namespace

NavigatorPanel::NavigatorPanel(ApplicationSession& session, QWidget* parent) : QWidget(parent), session_(session) {
    tree_ = new QTreeWidget(this);
    tree_->setHeaderHidden(true);
    tree_->setAccessibleName(tr("Library navigator"));
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setUniformRowHeights(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tree_);

    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* current) {
        current_identity_ = identity_of(current);
        if (!populating_) emit_current_scope();
    });
    connect(tree_, &QTreeWidget::itemExpanded, this,
            [this](QTreeWidgetItem* item) { expanded_.insert(identity_of(item)); });
    connect(tree_, &QTreeWidget::itemCollapsed, this,
            [this](QTreeWidgetItem* item) { expanded_.erase(identity_of(item)); });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, &NavigatorPanel::show_context_menu);
    library_reload_timer_.setSingleShot(true);
    connect(&library_reload_timer_, &QTimer::timeout, this, &NavigatorPanel::start_due_library_reloads);
}

NavigatorPanel::LibraryOutline NavigatorPanel::read_outline(app::Application& application,
                                                            const app::OpenLibraryInfo& info) {
    LibraryOutline outline;
    outline.library = info.uuid;
    outline.name = DisplayFormat::from_utf8(info.name);
    outline.read_only = info.read_only;
    auto read = application.with_library(info.uuid, [&outline](catalog::Library& library) -> Result<void> {
        auto roots = library.list_roots();
        if (!roots) return std::unexpected(roots.error());
        // The whole folder tree is read here, so expanding a root or folder never waits.
        for (const auto& root : *roots) {
            auto folders = library.list_folders(root.id);
            if (folders)
                outline.folders.emplace(root.id, std::move(*folders));
            else
                outline.error = DisplayFormat::error(folders.error());
        }
        outline.roots = std::move(*roots);
        auto tags = library.list_tags();
        auto categories = library.list_categories();
        auto collections = library.list_collections();
        if (tags) outline.tags = std::move(*tags);
        if (categories) outline.categories = std::move(*categories);
        if (collections) outline.collections = std::move(*collections);
        for (const Error* error : {tags ? nullptr : &tags.error(), categories ? nullptr : &categories.error(),
                                   collections ? nullptr : &collections.error()})
            if (error) outline.error = DisplayFormat::error(*error);
        return {};
    });
    if (!read) outline.error = DisplayFormat::error(read.error());
    return outline;
}

void NavigatorPanel::reload() {
    if (!session_.is_ready()) return;
    const std::uint64_t generation = ++reload_generation_;
    const std::uint64_t order = ++read_order_;
    ++outline_reads_started_;
    // A full read covers every pending per-library change; reads still in flight are superseded.
    for (auto& [library, state] : library_reloads_)
        state.dirty = false;
    session_.run(
        this,
        [](app::Application& application) {
            std::vector<LibraryOutline> outlines;
            for (const auto& info : application.libraries())
                outlines.push_back(read_outline(application, info));
            return outlines;
        },
        [this, generation, order](std::vector<LibraryOutline> outlines) {
            if (generation != reload_generation_) return;
            populate(outlines);
            std::set<uuids::uuid> open;
            for (const LibraryOutline& outline : outlines)
                open.insert(outline.library);
            // Closed libraries need no coalescing state any more.
            std::erase_if(library_reloads_, [&open](const auto& entry) {
                return !open.contains(entry.first) && !entry.second.in_flight;
            });
            // A per-library read that started after this full read and has already been shown was
            // just replaced by this (older) outline: read that library again.
            for (auto& [library, state] : library_reloads_)
                if (state.started_order > order && !state.in_flight) state.dirty = true;
            start_due_library_reloads();
        });
}

void NavigatorPanel::reload_library(const uuids::uuid& library) {
    if (!session_.is_ready()) return;
    library_reloads_[library].dirty = true;
    start_due_library_reloads();
}

void NavigatorPanel::start_due_library_reloads() {
    std::optional<qint64> next_due;
    for (auto& [library, state] : library_reloads_) {
        if (!state.dirty || state.in_flight) continue;  // a running read reschedules when it ends
        const qint64 waited =
            state.last_started.isValid() ? state.last_started.elapsed() : qint64{kLibraryReloadIntervalMilliseconds};
        if (waited >= kLibraryReloadIntervalMilliseconds) {
            start_library_read(library);
            continue;
        }
        const qint64 remaining = kLibraryReloadIntervalMilliseconds - waited;
        next_due = next_due ? std::min(*next_due, remaining) : remaining;
    }
    // One timer serves every library: it is moved earlier when another library is due sooner.
    if (next_due && (!library_reload_timer_.isActive() || library_reload_timer_.remainingTime() > *next_due))
        library_reload_timer_.start(static_cast<int>(*next_due));
}

void NavigatorPanel::start_library_read(const uuids::uuid& library) {
    LibraryReloadState& state = library_reloads_[library];
    state.dirty = false;
    state.in_flight = true;
    state.last_started.start();
    state.started_order = ++read_order_;
    ++outline_reads_started_;
    const std::uint64_t generation = reload_generation_;  // a full reload supersedes this one
    session_.run(
        this,
        [library](app::Application& application) -> std::optional<LibraryOutline> {
            for (const auto& info : application.libraries())
                if (info.uuid == library) return read_outline(application, info);
            return std::nullopt;
        },
        [this, generation, library](std::optional<LibraryOutline> outline) {
            library_reloads_[library].in_flight = false;
            // One read per library is in flight, so no older outline can arrive after this one.
            if (generation == reload_generation_ && outline) replace_library(*outline);
            start_due_library_reloads();  // a change that arrived meanwhile is read now (or soon)
        });
}

void NavigatorPanel::populate(const std::vector<LibraryOutline>& outlines) {
    populating_ = true;
    const QString previous_identity = current_identity_;
    const int scroll_position = tree_->verticalScrollBar()->value();
    // Rebuilt in one pass with painting suspended, so a reload never flickers.
    tree_->setUpdatesEnabled(false);
    tree_->clear();

    auto* all = new QTreeWidgetItem(tree_, QStringList{tr("All libraries")});
    all->setData(0, KindRole, AllLibrariesNode);
    for (const LibraryOutline& outline : outlines) {
        auto* library_item = new QTreeWidgetItem(tree_);
        fill_library(library_item, outline);
        if (expanded_.empty()) library_item->setExpanded(true);  // first run: show the library's groups
    }
    restore_tree_state(previous_identity, scroll_position, all);
}

void NavigatorPanel::replace_library(const LibraryOutline& outline) {
    QTreeWidgetItem* library_item = nullptr;
    for (int index = 0; index < tree_->topLevelItemCount() && !library_item; ++index) {
        QTreeWidgetItem* candidate = tree_->topLevelItem(index);
        if (kind_of(candidate) == LibraryNode &&
            candidate->data(0, LibraryRole).toString() == DisplayFormat::from_uuid(outline.library))
            library_item = candidate;
    }
    if (!library_item) return;  // closed meanwhile; the library list change reloads everything
    populating_ = true;
    const QString previous_identity = current_identity_;
    const int scroll_position = tree_->verticalScrollBar()->value();
    tree_->setUpdatesEnabled(false);
    qDeleteAll(library_item->takeChildren());
    fill_library(library_item, outline);
    restore_tree_state(previous_identity, scroll_position, tree_->topLevelItem(0));
}

void NavigatorPanel::fill_library(QTreeWidgetItem* library_item, const LibraryOutline& outline) {
    library_item->setText(0, outline.read_only ? tr("%1 (read-only)").arg(outline.name) : outline.name);
    library_item->setData(0, KindRole, LibraryNode);
    library_item->setData(0, LibraryRole, DisplayFormat::from_uuid(outline.library));
    library_item->setToolTip(0, outline.error.isEmpty()
                                    ? QString()
                                    : DisplayFormat::plain_tooltip(tr("Partly unreadable: %1").arg(outline.error)));

    auto* roots_group = make_item(library_item, tr("Folders"), RootsGroupNode, outline.library);
    for (const auto& root : outline.roots) {
        auto* root_item = make_item(roots_group, root_text(DisplayFormat::from_utf8(root.name), root.availability),
                                    RootNode, outline.library);
        root_item->setData(0, RootRole, static_cast<qlonglong>(root.id));
        root_item->setData(0, RootNameRole, DisplayFormat::from_utf8(root.name));
        root_item->setData(0, PathRole, QString());
        root_item->setToolTip(0, tr("%n sample(s)", nullptr, static_cast<int>(root.sample_count)));
        if (const auto folders = outline.folders.find(root.id); folders != outline.folders.end())
            add_folders(root_item, outline.library, root.id, folders->second);
    }
    add_taxonomy(make_item(library_item, tr("Tags"), TagsGroupNode, outline.library), outline, outline.tags, TagNode);
    auto* categories_group = make_item(library_item, tr("Categories"), CategoriesGroupNode, outline.library);
    add_taxonomy(categories_group, outline, outline.categories, CategoryNode);
    make_item(categories_group, tr("Uncategorized"), UncategorizedNode, outline.library);

    auto* collections_group = make_item(library_item, tr("Collections"), CollectionsGroupNode, outline.library);
    for (const auto& collection : outline.collections) {
        const QString name = DisplayFormat::from_utf8(collection.name);
        const QString text = collection.smart ? tr("%1 (smart)").arg(name)
                                              : tr("%1 (%2)").arg(name, DisplayFormat::count(collection.sample_count));
        auto* item = make_item(collections_group, text, CollectionNode, outline.library);
        item->setData(0, IdRole, static_cast<qlonglong>(collection.id));
        item->setData(0, PathRole, name);
        if (collection.query)
            item->setToolTip(
                0, DisplayFormat::plain_tooltip(tr("Query: %1").arg(DisplayFormat::from_utf8(*collection.query))));
    }
}

void NavigatorPanel::restore_tree_state(const QString& previous_identity, int scroll_position,
                                        QTreeWidgetItem* fallback) {
    QTreeWidgetItem* restored_current = nullptr;
    for (QTreeWidgetItemIterator iterator(tree_); *iterator; ++iterator) {
        const QString identity = identity_of(*iterator);
        if (expanded_.contains(identity)) (*iterator)->setExpanded(true);
        if (identity == previous_identity) restored_current = *iterator;
    }
    tree_->setCurrentItem(restored_current ? restored_current : fallback);
    tree_->verticalScrollBar()->setValue(scroll_position);
    tree_->setUpdatesEnabled(true);
    populating_ = false;
    // The scope only changes when the previous node disappeared (e.g. its library closed).
    if (!restored_current) emit_current_scope();
}

void NavigatorPanel::add_taxonomy(QTreeWidgetItem* parent, const LibraryOutline& outline,
                                  const std::vector<catalog::TaxonomyNode>& nodes, int node_kind) {
    std::map<std::int64_t, QTreeWidgetItem*> items;
    std::map<std::int64_t, std::optional<std::int64_t>> parent_of;
    for (const auto& node : nodes) {
        if (node.alias_of) continue;  // merged tags live on as redirects, not as tree nodes
        parent_of.emplace(node.id, node.parent_id);
        auto* item = new QTreeWidgetItem(QStringList{
            tr("%1 (%2)").arg(DisplayFormat::from_utf8(node.name), DisplayFormat::count(node.sample_count))});
        item->setData(0, KindRole, node_kind);
        item->setData(0, LibraryRole, DisplayFormat::from_uuid(outline.library));
        item->setData(0, IdRole, static_cast<qlonglong>(node.id));
        item->setData(0, PathRole, DisplayFormat::from_utf8(node.path));
        items.emplace(node.id, item);
    }
    // Members of a parent cycle (a damaged catalog) hang under the group: an item attached only to
    // the other items of its cycle would never be shown, nor freed with the tree.
    const std::set<std::int64_t> on_cycle = detail::nodes_on_parent_cycles(parent_of);
    for (const auto& node : nodes) {
        const auto found = items.find(node.id);
        if (found == items.end()) continue;
        const auto parent_item =
            node.parent_id && !on_cycle.contains(node.id) ? items.find(*node.parent_id) : items.end();
        (parent_item != items.end() ? parent_item->second : parent)->addChild(found->second);
    }
}

void NavigatorPanel::add_folders(QTreeWidgetItem* root_item, const uuids::uuid& library, std::int64_t root_id,
                                 const std::vector<catalog::FolderRecord>& folders) {
    std::map<std::int64_t, QTreeWidgetItem*> items;
    std::map<std::int64_t, std::optional<std::int64_t>> parent_of;
    for (const auto& folder : folders) {
        if (folder.relative_path.empty()) {
            items.emplace(folder.id, root_item);  // the root folder itself
            continue;
        }
        parent_of.emplace(folder.id, folder.parent_id);
        auto* item = new QTreeWidgetItem(QStringList{DisplayFormat::from_utf8(folder.name)});
        item->setData(0, KindRole, FolderNode);
        item->setData(0, LibraryRole, DisplayFormat::from_uuid(library));
        item->setData(0, IdRole, static_cast<qlonglong>(folder.id));
        item->setData(0, RootRole, static_cast<qlonglong>(root_id));
        item->setData(0, PathRole, DisplayFormat::from_utf8(folder.relative_path));
        item->setToolTip(0, DisplayFormat::plain_tooltip(DisplayFormat::from_utf8(folder.relative_path)));
        items.emplace(folder.id, item);
    }
    // Members of a parent cycle (a damaged catalog) hang under the group: an item attached only to
    // the other items of its cycle would never be shown, nor freed with the tree.
    const std::set<std::int64_t> on_cycle = detail::nodes_on_parent_cycles(parent_of);
    for (const auto& folder : folders) {
        if (folder.relative_path.empty()) continue;
        const auto parent =
            folder.parent_id && !on_cycle.contains(folder.id) ? items.find(*folder.parent_id) : items.end();
        (parent != items.end() ? parent->second : root_item)->addChild(items.at(folder.id));
    }
}

void NavigatorPanel::show_folder_in_file_manager(const uuids::uuid& library, std::int64_t root_id,
                                                 const QString& relative_path) {
    session_.run(
        this,
        [library, root_id, relative = DisplayFormat::to_utf8(relative_path)](
            app::Application& application) -> Result<std::filesystem::path> {
            auto roots =
                application.with_library(library, [](catalog::Library& opened) { return opened.list_roots(); });
            if (!roots) return std::unexpected(roots.error());
            for (const auto& root : *roots) {
                if (root.id != root_id) continue;
                const auto base = application.root_path(library, root);
                if (!base) return fail(ErrorCode::SourceOffline, "the folder is not linked on this computer");
                const std::filesystem::path folder = relative.empty() ? *base : *base / path_from_utf8(relative);
                std::error_code error;
                if (!std::filesystem::is_directory(folder, error))
                    return fail(ErrorCode::SourceOffline, "the folder is not reachable right now");
                return folder;
            }
            return fail(ErrorCode::NotFound, "the folder is no longer part of the library");
        },
        [this](Result<std::filesystem::path> folder) {
            if (!folder) {
                emit statusMessage(tr("Cannot show the folder: %1").arg(DisplayFormat::error(folder.error())));
                return;
            }
            if (!FileManager::open_folder(*folder))
                emit statusMessage(tr("Could not open %1").arg(DisplayFormat::from_path(*folder)));
        });
}

std::optional<uuids::uuid> NavigatorPanel::current_library() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    if (!item) return std::nullopt;
    return DisplayFormat::to_uuid(item->data(0, LibraryRole).toString());
}

std::optional<std::int64_t> NavigatorPanel::current_root() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    const int kind = kind_of(item);
    if (kind != RootNode && kind != FolderNode) return std::nullopt;
    return item->data(0, RootRole).toLongLong();
}

void NavigatorPanel::set_root_availability(const uuids::uuid& library, std::int64_t root_id,
                                           catalog::Availability availability) {
    const QString library_text = DisplayFormat::from_uuid(library);
    for (QTreeWidgetItemIterator iterator(tree_); *iterator; ++iterator) {
        QTreeWidgetItem* item = *iterator;
        if (kind_of(item) != RootNode || item->data(0, LibraryRole).toString() != library_text ||
            item->data(0, RootRole).toLongLong() != root_id)
            continue;
        item->setText(0, root_text(item->data(0, RootNameRole).toString(), availability));
    }
}

NavigationScope NavigatorPanel::current_scope() const {
    return scope_of(tree_->currentItem());
}

NavigationScope NavigatorPanel::scope_of(const QTreeWidgetItem* item) const {
    NavigationScope scope;
    const int kind = kind_of(item);
    if (!item || kind == AllLibrariesNode || kind == 0) {
        scope.label = tr("All libraries");
        return scope;
    }
    scope.library = DisplayFormat::to_uuid(item->data(0, LibraryRole).toString());
    const QString library_name = scope.library ? session_.library_name(*scope.library) : QString();
    const auto id = static_cast<std::int64_t>(item->data(0, IdRole).toLongLong());
    const QString path = item->data(0, PathRole).toString();
    switch (kind) {
        case RootNode:
            scope.scope.root_id = item->data(0, RootRole).toLongLong();
            // The root's name without the " — Offline" suffix the tree shows.
            scope.label = tr("%1 in %2").arg(item->data(0, RootNameRole).toString(), library_name);
            break;
        case FolderNode:
            scope.scope.root_id = item->data(0, RootRole).toLongLong();
            scope.scope.folder = DisplayFormat::to_utf8(path);
            scope.label = tr("Folder %1 in %2").arg(path, library_name);
            break;
        case TagNode:
            scope.scope.tag_id = id;
            scope.label = tr("Tag %1 in %2").arg(path, library_name);
            break;
        case CategoryNode:
            scope.scope.category_id = id;
            scope.label = tr("Category %1 in %2").arg(path, library_name);
            break;
        case UncategorizedNode:
            scope.scope.uncategorized = true;
            scope.label = tr("Uncategorized in %1").arg(library_name);
            break;
        case CollectionNode:
            scope.scope.collection_id = id;
            scope.label = tr("Collection %1 in %2").arg(path, library_name);
            break;
        default:
            scope.label = library_name;
            break;
    }
    return scope;
}

void NavigatorPanel::emit_current_scope() {
    emit scopeSelected(scope_of(tree_->currentItem()));
}

QString NavigatorPanel::identity_of(const QTreeWidgetItem* item) {
    if (!item) return {};
    const int kind = kind_of(item);
    // Tags, categories and collections are identified by id alone, so a rename keeps the node's
    // expansion and selection; folders by path (their ids are scan-local).
    const bool identified_by_id = kind == TagNode || kind == CategoryNode || kind == CollectionNode;
    return QStringLiteral("%1|%2|%3|%4|%5")
        .arg(kind)
        .arg(item->data(0, LibraryRole).toString())
        .arg(item->data(0, IdRole).toLongLong())
        .arg(item->data(0, RootRole).toLongLong())
        .arg(identified_by_id ? QString() : item->data(0, PathRole).toString());
}

NavigatorPanel::NodeInfo NavigatorPanel::node_info(const QTreeWidgetItem* item) {
    NodeInfo node;
    node.kind = kind_of(item);
    if (!item) return node;
    if (auto library = DisplayFormat::to_uuid(item->data(0, LibraryRole).toString())) node.library = *library;
    node.id = static_cast<std::int64_t>(item->data(0, IdRole).toLongLong());
    node.root_id = static_cast<std::int64_t>(item->data(0, RootRole).toLongLong());
    node.path = item->data(0, PathRole).toString();
    return node;
}

void NavigatorPanel::show_context_menu(const QPoint& position) {
    const QTreeWidgetItem* item = tree_->itemAt(position);
    if (!item || !DisplayFormat::to_uuid(item->data(0, LibraryRole).toString())) return;
    // Values only from here on: menu.exec() runs a nested event loop in which an outline delivery
    // can rebuild the tree and delete the item.
    const NodeInfo node = node_info(item);
    QMenu menu(this);
    switch (node.kind) {
        case LibraryNode:
        case RootsGroupNode:
            add_library_actions(menu, node);
            break;
        case RootNode:
        case FolderNode:
            add_folder_actions(menu, node);
            break;
        case TagNode:
            add_tag_actions(menu, node);
            break;
        case CategoryNode:
            add_category_actions(menu, node);
            break;
        case CollectionsGroupNode:
        case CollectionNode:
            add_collection_actions(menu, node);
            break;
        default:
            return;
    }
    menu.exec(tree_->viewport()->mapToGlobal(position));
}

void NavigatorPanel::add_library_actions(QMenu& menu, const NodeInfo& node) {
    const uuids::uuid library = node.library;
    menu.addAction(tr("Add folder…"), this, [this, library] { emit addFolderRequested(library); });
    if (node.kind != LibraryNode) return;
    menu.addAction(tr("Backup library"), this, [this, library] { emit backupRequested(library); });
    menu.addSeparator();
    menu.addAction(tr("Close library"), this, [this, library] { emit closeLibraryRequested(library); });
}

void NavigatorPanel::add_folder_actions(QMenu& menu, const NodeInfo& node) {
    const uuids::uuid library = node.library;
    const std::int64_t root_id = node.root_id;
    if (node.kind == RootNode) {
        menu.addAction(tr("Rescan"), this, [this, library, root_id] { emit rescanRequested(library, root_id); });
        menu.addAction(FileManager::reveal_action_text(), this,
                       [this, library, root_id] { show_folder_in_file_manager(library, root_id, {}); });
        menu.addAction(tr("Locate moved folder…"), this,
                       [this, library, root_id] { emit rebindRequested(library, root_id); });
        return;
    }
    menu.addAction(FileManager::reveal_action_text(), this,
                   [this, library, root_id, path = node.path] { show_folder_in_file_manager(library, root_id, path); });
}

void NavigatorPanel::add_tag_actions(QMenu& menu, const NodeInfo& node) {
    menu.addAction(tr("Rename tag…"), this, [this, node] {
        if (auto name = ask_name(tr("Rename tag"), node.path.section(QLatin1Char('/'), -1)))
            emit taxonomyCommandRequested(node.library, {.kind = catalog::TaxonomyCommand::Kind::RenameTag,
                                                         .target_id = node.id,
                                                         .new_name = DisplayFormat::to_utf8(*name)});
    });
    menu.addAction(tr("Move tag…"), this, [this, node] {
        // The targets are values read from the tree as it is now; the dialog's event loop may
        // rebuild the tree, and only these values are used afterwards.
        const auto targets = tag_targets(node.library, node.id);
        QStringList choices{tr("(top level)")};
        for (const auto& [tag_path, tag_id] : targets)
            choices << tag_path;
        bool accepted = false;
        const QString choice = QInputDialog::getItem(this, tr("Move tag"), tr("New parent of %1:").arg(node.path),
                                                     choices, 0, false, &accepted);
        if (!accepted) return;
        const auto index = choices.indexOf(choice);
        catalog::TaxonomyCommand command{.kind = catalog::TaxonomyCommand::Kind::MoveTag, .target_id = node.id};
        if (index > 0) command.new_parent_id = targets[static_cast<std::size_t>(index - 1)].second;
        emit taxonomyCommandRequested(node.library, command);
    });
    menu.addAction(tr("Merge into…"), this, [this, node] {
        const auto targets = tag_targets(node.library, node.id);
        if (targets.empty()) {
            emit statusMessage(tr("There is no other tag to merge %1 into.").arg(node.path));
            return;
        }
        QStringList choices;
        for (const auto& [tag_path, tag_id] : targets)
            choices << tag_path;
        bool accepted = false;
        const QString choice = QInputDialog::getItem(
            this, tr("Merge tag"), tr("Merge %1 into (it becomes an alias, undoable):").arg(node.path), choices, 0,
            false, &accepted);
        if (!accepted) return;
        const auto index = choices.indexOf(choice);
        if (index < 0) return;
        emit taxonomyCommandRequested(node.library, {.kind = catalog::TaxonomyCommand::Kind::MergeTag,
                                                     .target_id = node.id,
                                                     .other_id = targets[static_cast<std::size_t>(index)].second});
    });
}

void NavigatorPanel::add_category_actions(QMenu& menu, const NodeInfo& node) {
    menu.addAction(tr("Rename category…"), this, [this, node] {
        if (auto name = ask_name(tr("Rename category"), node.path.section(QLatin1Char('/'), -1)))
            emit taxonomyCommandRequested(node.library, {.kind = catalog::TaxonomyCommand::Kind::RenameCategory,
                                                         .target_id = node.id,
                                                         .new_name = DisplayFormat::to_utf8(*name)});
    });
}

void NavigatorPanel::add_collection_actions(QMenu& menu, const NodeInfo& node) {
    menu.addAction(tr("New collection…"), this, [this, node] {
        if (auto name = ask_name(tr("New collection"), {}))
            emit taxonomyCommandRequested(node.library, {.kind = catalog::TaxonomyCommand::Kind::CreateCollection,
                                                         .new_name = DisplayFormat::to_utf8(*name)});
    });
    menu.addAction(tr("New smart collection from current search…"), this, [this, node] {
        if (auto name = ask_name(tr("New smart collection"), {})) emit smartCollectionRequested(node.library, *name);
    });
    if (node.kind != CollectionNode) return;
    menu.addSeparator();
    menu.addAction(tr("Rename collection…"), this, [this, node] {
        if (auto name = ask_name(tr("Rename collection"), node.path))
            emit taxonomyCommandRequested(node.library, {.kind = catalog::TaxonomyCommand::Kind::RenameCollection,
                                                         .target_id = node.id,
                                                         .new_name = DisplayFormat::to_utf8(*name)});
    });
    menu.addAction(tr("Delete collection"), this, [this, node] {
        if (QMessageBox::question(this, tr("Delete collection"),
                                  tr("Delete the collection “%1”? Its samples stay in the library, and "
                                     "you can undo this.")
                                      .arg(node.path)) == QMessageBox::Yes)
            emit taxonomyCommandRequested(
                node.library, {.kind = catalog::TaxonomyCommand::Kind::DeleteCollection, .target_id = node.id});
    });
}

std::optional<QString> NavigatorPanel::ask_name(const QString& title, const QString& current) {
    bool accepted = false;
    const QString name =
        QInputDialog::getText(this, title, tr("Name:"), QLineEdit::Normal, current, &accepted).trimmed();
    if (!accepted || name.isEmpty()) return std::nullopt;
    return name;
}

std::vector<std::pair<QString, std::int64_t>> NavigatorPanel::tag_targets(const uuids::uuid& library,
                                                                          std::int64_t tag_id) const {
    std::vector<std::pair<QString, std::int64_t>> targets;
    const QString library_text = DisplayFormat::from_uuid(library);
    QTreeWidgetItem* group = nullptr;
    for (QTreeWidgetItemIterator iterator(tree_); *iterator && !group; ++iterator)
        if (kind_of(*iterator) == TagsGroupNode && (*iterator)->data(0, LibraryRole).toString() == library_text)
            group = *iterator;
    if (!group) return targets;
    // The tags subtree of the group: everything after it up to the next sibling of the group.
    std::vector<QTreeWidgetItem*> tags;
    for (QTreeWidgetItemIterator iterator(group); *iterator; ++iterator) {
        if (*iterator == group) continue;
        if ((*iterator)->parent() == group->parent()) break;  // left the tags subtree
        if (kind_of(*iterator) == TagNode) tags.push_back(*iterator);
    }
    const auto moved = std::ranges::find_if(
        tags, [tag_id](const QTreeWidgetItem* tag) { return tag->data(0, IdRole).toLongLong() == tag_id; });
    for (const QTreeWidgetItem* tag : tags) {
        if (moved != tags.end() && is_same_or_descendant(tag, *moved)) continue;
        targets.emplace_back(tag->data(0, PathRole).toString(),
                             static_cast<std::int64_t>(tag->data(0, IdRole).toLongLong()));
    }
    return targets;
}

}  // namespace mso::ui
