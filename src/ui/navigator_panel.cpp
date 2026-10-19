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

#include <filesystem>
#include <map>
#include <system_error>

#include "mso/ui/application_session.hpp"
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
    LibraryRole,  ///< QString UUID
    IdRole,       ///< qlonglong: tag/category/collection/folder id
    RootRole,     ///< qlonglong root id (roots and folders)
    PathRole,     ///< QString: folder relative path, or tag/category display path
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

QString root_text(const catalog::RootRecord& root) {
    QString text = DisplayFormat::from_utf8(root.name);
    if (root.availability != catalog::Availability::Online)
        text += NavigatorPanel::tr(" — %1").arg(DisplayFormat::root_availability(root.availability));
    return text;
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
}

void NavigatorPanel::reload() {
    if (!session_.is_ready()) return;
    const std::uint64_t generation = ++reload_generation_;
    session_.run(
        this,
        [](app::Application& application) {
            std::vector<LibraryOutline> outlines;
            for (const auto& info : application.libraries()) {
                LibraryOutline outline;
                outline.library = info.uuid;
                outline.name = DisplayFormat::from_utf8(info.name);
                outline.read_only = info.read_only;
                catalog::Library* library = application.library(info.uuid);
                if (!library) continue;
                auto roots = library->list_roots();
                // The whole folder tree is read here, so expanding a root or folder never waits.
                if (roots) {
                    for (const auto& root : *roots) {
                        auto folders = library->list_folders(root.id);
                        if (folders)
                            outline.folders.emplace(root.id, std::move(*folders));
                        else
                            outline.error = DisplayFormat::error(folders.error());
                    }
                }
                auto tags = library->list_tags();
                auto categories = library->list_categories();
                auto collections = library->list_collections();
                if (roots) outline.roots = std::move(*roots);
                if (tags) outline.tags = std::move(*tags);
                if (categories) outline.categories = std::move(*categories);
                if (collections) outline.collections = std::move(*collections);
                for (const Error* error :
                     {roots ? nullptr : &roots.error(), tags ? nullptr : &tags.error(),
                      categories ? nullptr : &categories.error(), collections ? nullptr : &collections.error()})
                    if (error) outline.error = DisplayFormat::error(*error);
                outlines.push_back(std::move(outline));
            }
            return outlines;
        },
        [this, generation](std::vector<LibraryOutline> outlines) {
            if (generation != reload_generation_) return;
            populate(outlines);
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
        QString name = outline.name;
        if (outline.read_only) name += tr(" (read-only)");
        auto* library_item = new QTreeWidgetItem(tree_, QStringList{name});
        library_item->setData(0, KindRole, LibraryNode);
        library_item->setData(0, LibraryRole, DisplayFormat::from_uuid(outline.library));
        if (!outline.error.isEmpty()) library_item->setToolTip(0, tr("Partly unreadable: %1").arg(outline.error));

        auto* roots_group = make_item(library_item, tr("Folders"), RootsGroupNode, outline.library);
        for (const auto& root : outline.roots) {
            auto* root_item = make_item(roots_group, root_text(root), RootNode, outline.library);
            root_item->setData(0, RootRole, static_cast<qlonglong>(root.id));
            root_item->setData(0, PathRole, QString());
            root_item->setToolTip(0, tr("%1 samples").arg(DisplayFormat::count(root.sample_count)));
            if (const auto folders = outline.folders.find(root.id); folders != outline.folders.end())
                add_folders(root_item, outline.library, root.id, folders->second);
        }
        add_taxonomy(make_item(library_item, tr("Tags"), TagsGroupNode, outline.library), outline, outline.tags,
                     TagNode);
        auto* categories_group = make_item(library_item, tr("Categories"), CategoriesGroupNode, outline.library);
        add_taxonomy(categories_group, outline, outline.categories, CategoryNode);
        make_item(categories_group, tr("Uncategorized"), UncategorizedNode, outline.library);

        auto* collections_group = make_item(library_item, tr("Collections"), CollectionsGroupNode, outline.library);
        for (const auto& collection : outline.collections) {
            const QString text = collection.smart ? tr("%1 (smart)").arg(DisplayFormat::from_utf8(collection.name))
                                                  : tr("%1 (%2)").arg(DisplayFormat::from_utf8(collection.name),
                                                                      DisplayFormat::count(collection.sample_count));
            auto* item = make_item(collections_group, text, CollectionNode, outline.library);
            item->setData(0, IdRole, static_cast<qlonglong>(collection.id));
            item->setData(0, PathRole, DisplayFormat::from_utf8(collection.name));
            if (collection.query) item->setToolTip(0, tr("Query: %1").arg(DisplayFormat::from_utf8(*collection.query)));
        }
        if (expanded_.empty()) library_item->setExpanded(true);  // first run: show the library's groups
    }

    QTreeWidgetItem* restored_current = nullptr;
    for (QTreeWidgetItemIterator iterator(tree_); *iterator; ++iterator) {
        const QString identity = identity_of(*iterator);
        if (expanded_.contains(identity)) (*iterator)->setExpanded(true);
        if (identity == previous_identity) restored_current = *iterator;
    }
    tree_->setCurrentItem(restored_current ? restored_current : all);
    tree_->verticalScrollBar()->setValue(scroll_position);
    tree_->setUpdatesEnabled(true);
    populating_ = false;
    // The scope only changes when the previous node disappeared (e.g. its library closed).
    if (!restored_current) emit_current_scope();
}

void NavigatorPanel::add_taxonomy(QTreeWidgetItem* parent, const LibraryOutline& outline,
                                  const std::vector<catalog::TaxonomyNode>& nodes, int node_kind) {
    std::map<std::int64_t, QTreeWidgetItem*> items;
    for (const auto& node : nodes) {
        if (node.alias_of) continue;  // merged tags live on as redirects, not as tree nodes
        auto* item = new QTreeWidgetItem(QStringList{
            tr("%1 (%2)").arg(DisplayFormat::from_utf8(node.name), DisplayFormat::count(node.sample_count))});
        item->setData(0, KindRole, node_kind);
        item->setData(0, LibraryRole, DisplayFormat::from_uuid(outline.library));
        item->setData(0, IdRole, static_cast<qlonglong>(node.id));
        item->setData(0, PathRole, DisplayFormat::from_utf8(node.path));
        items.emplace(node.id, item);
    }
    for (const auto& node : nodes) {
        const auto found = items.find(node.id);
        if (found == items.end()) continue;
        const auto parent_item = node.parent_id ? items.find(*node.parent_id) : items.end();
        (parent_item != items.end() ? parent_item->second : parent)->addChild(found->second);
    }
}

void NavigatorPanel::add_folders(QTreeWidgetItem* root_item, const uuids::uuid& library, std::int64_t root_id,
                                 const std::vector<catalog::FolderRecord>& folders) {
    std::map<std::int64_t, QTreeWidgetItem*> items;
    for (const auto& folder : folders) {
        if (folder.relative_path.empty()) {
            items.emplace(folder.id, root_item);  // the root folder itself
            continue;
        }
        auto* item = new QTreeWidgetItem(QStringList{DisplayFormat::from_utf8(folder.name)});
        item->setData(0, KindRole, FolderNode);
        item->setData(0, LibraryRole, DisplayFormat::from_uuid(library));
        item->setData(0, IdRole, static_cast<qlonglong>(folder.id));
        item->setData(0, RootRole, static_cast<qlonglong>(root_id));
        item->setData(0, PathRole, DisplayFormat::from_utf8(folder.relative_path));
        item->setToolTip(0, DisplayFormat::from_utf8(folder.relative_path));
        items.emplace(folder.id, item);
    }
    for (const auto& folder : folders) {
        if (folder.relative_path.empty()) continue;
        const auto parent = folder.parent_id ? items.find(*folder.parent_id) : items.end();
        (parent != items.end() ? parent->second : root_item)->addChild(items.at(folder.id));
    }
}

void NavigatorPanel::show_folder_in_file_manager(const uuids::uuid& library, std::int64_t root_id,
                                                 const QString& relative_path) {
    session_.run(
        this,
        [library, root_id, relative = DisplayFormat::to_utf8(relative_path)](
            app::Application& application) -> Result<std::filesystem::path> {
            catalog::Library* opened = application.library(library);
            if (!opened) return fail(ErrorCode::NotFound, "the library is no longer open");
            auto roots = opened->list_roots();
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
        QString name = item->text(0).section(QStringLiteral(" — "), 0, 0);
        if (availability != catalog::Availability::Online)
            name += tr(" — %1").arg(DisplayFormat::root_availability(availability));
        item->setText(0, name);
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
            scope.label = tr("%1 in %2").arg(item->text(0), library_name);
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
    return QStringLiteral("%1|%2|%3|%4|%5")
        .arg(item->data(0, KindRole).toInt())
        .arg(item->data(0, LibraryRole).toString())
        .arg(item->data(0, IdRole).toLongLong())
        .arg(item->data(0, RootRole).toLongLong())
        .arg(item->data(0, PathRole).toString());
}

void NavigatorPanel::show_context_menu(const QPoint& position) {
    QTreeWidgetItem* item = tree_->itemAt(position);
    const int kind = kind_of(item);
    const auto library = item ? DisplayFormat::to_uuid(item->data(0, LibraryRole).toString()) : std::nullopt;
    if (!library) return;
    const auto id = static_cast<std::int64_t>(item->data(0, IdRole).toLongLong());
    const QString path = item->data(0, PathRole).toString();
    QMenu menu(this);

    const auto request = [this, library = *library](catalog::TaxonomyCommand command) {
        emit taxonomyCommandRequested(library, command);
    };
    const auto ask_name = [this](const QString& title, const QString& current) -> std::optional<QString> {
        bool accepted = false;
        const QString name =
            QInputDialog::getText(this, title, tr("Name:"), QLineEdit::Normal, current, &accepted).trimmed();
        if (!accepted || name.isEmpty()) return std::nullopt;
        return name;
    };
    /// Display paths of the library's tags other than `excluded` (for merge and move targets).
    const auto other_tags = [item, id] {
        std::vector<std::pair<QString, std::int64_t>> tags;
        QTreeWidgetItem* group = item;
        while (group && kind_of(group) != TagsGroupNode)
            group = group->parent();
        if (!group) return tags;
        for (QTreeWidgetItemIterator iterator(group); *iterator; ++iterator) {
            if (*iterator == group) continue;
            if ((*iterator)->parent() == group->parent()) break;  // left the tags subtree
            if (kind_of(*iterator) != TagNode) continue;
            const auto tag_id = static_cast<std::int64_t>((*iterator)->data(0, IdRole).toLongLong());
            if (tag_id != id) tags.emplace_back((*iterator)->data(0, PathRole).toString(), tag_id);
        }
        return tags;
    };

    switch (kind) {
        case LibraryNode:
            menu.addAction(tr("Add folder…"), this, [this, library = *library] { emit addFolderRequested(library); });
            menu.addAction(tr("Backup library"), this, [this, library = *library] { emit backupRequested(library); });
            menu.addSeparator();
            menu.addAction(tr("Close library"), this,
                           [this, library = *library] { emit closeLibraryRequested(library); });
            break;
        case RootsGroupNode:
            menu.addAction(tr("Add folder…"), this, [this, library = *library] { emit addFolderRequested(library); });
            break;
        case RootNode: {
            const std::int64_t root_id = item->data(0, RootRole).toLongLong();
            menu.addAction(tr("Rescan"), this,
                           [this, library = *library, root_id] { emit rescanRequested(library, root_id); });
            menu.addAction(tr("Show in Explorer"), this,
                           [this, library = *library, root_id] { show_folder_in_file_manager(library, root_id, {}); });
            menu.addAction(tr("Locate moved folder…"), this,
                           [this, library = *library, root_id] { emit rebindRequested(library, root_id); });
            break;
        }
        case FolderNode: {
            const std::int64_t root_id = item->data(0, RootRole).toLongLong();
            menu.addAction(tr("Show in Explorer"), this, [this, library = *library, root_id, path] {
                show_folder_in_file_manager(library, root_id, path);
            });
            break;
        }
        case TagNode:
            menu.addAction(tr("Rename tag…"), this, [=] {
                if (auto name = ask_name(tr("Rename tag"), path.section(QLatin1Char('/'), -1)))
                    request({.kind = catalog::TaxonomyCommand::Kind::RenameTag,
                             .target_id = id,
                             .new_name = DisplayFormat::to_utf8(*name)});
            });
            menu.addAction(tr("Move tag…"), this, [=, this] {
                QStringList choices{tr("(top level)")};
                const auto tags = other_tags();
                for (const auto& [tag_path, tag_id] : tags)
                    choices << tag_path;
                bool accepted = false;
                const QString choice = QInputDialog::getItem(this, tr("Move tag"), tr("New parent of %1:").arg(path),
                                                             choices, 0, false, &accepted);
                if (!accepted) return;
                const auto index = choices.indexOf(choice);
                catalog::TaxonomyCommand command{.kind = catalog::TaxonomyCommand::Kind::MoveTag, .target_id = id};
                if (index > 0) command.new_parent_id = tags[static_cast<std::size_t>(index - 1)].second;
                request(command);
            });
            menu.addAction(tr("Merge into…"), this, [=, this] {
                const auto tags = other_tags();
                if (tags.empty()) return;
                QStringList choices;
                for (const auto& [tag_path, tag_id] : tags)
                    choices << tag_path;
                bool accepted = false;
                const QString choice = QInputDialog::getItem(
                    this, tr("Merge tag"), tr("Merge %1 into (it becomes an alias, undoable):").arg(path), choices, 0,
                    false, &accepted);
                if (!accepted) return;
                const auto index = static_cast<std::size_t>(choices.indexOf(choice));
                request({.kind = catalog::TaxonomyCommand::Kind::MergeTag,
                         .target_id = id,
                         .other_id = tags[index].second});
            });
            break;
        case CategoryNode:
            menu.addAction(tr("Rename category…"), this, [=] {
                if (auto name = ask_name(tr("Rename category"), path.section(QLatin1Char('/'), -1)))
                    request({.kind = catalog::TaxonomyCommand::Kind::RenameCategory,
                             .target_id = id,
                             .new_name = DisplayFormat::to_utf8(*name)});
            });
            break;
        case CollectionsGroupNode:
        case CollectionNode:
            menu.addAction(tr("New collection…"), this, [=] {
                if (auto name = ask_name(tr("New collection"), {}))
                    request({.kind = catalog::TaxonomyCommand::Kind::CreateCollection,
                             .new_name = DisplayFormat::to_utf8(*name)});
            });
            menu.addAction(tr("New smart collection from current search…"), this, [=, this] {
                if (auto name = ask_name(tr("New smart collection"), {}))
                    emit smartCollectionRequested(*library, *name);
            });
            if (kind == CollectionNode) {
                menu.addSeparator();
                menu.addAction(tr("Rename collection…"), this, [=] {
                    if (auto name = ask_name(tr("Rename collection"), path))
                        request({.kind = catalog::TaxonomyCommand::Kind::RenameCollection,
                                 .target_id = id,
                                 .new_name = DisplayFormat::to_utf8(*name)});
                });
                menu.addAction(tr("Delete collection"), this, [=, this] {
                    if (QMessageBox::question(this, tr("Delete collection"),
                                              tr("Delete the collection “%1”? Its samples stay in the library, and "
                                                 "you can undo this.")
                                                  .arg(path)) == QMessageBox::Yes)
                        request({.kind = catalog::TaxonomyCommand::Kind::DeleteCollection, .target_id = id});
                });
            }
            break;
        default:
            return;
    }
    menu.exec(tree_->viewport()->mapToGlobal(position));
}

}  // namespace mso::ui
