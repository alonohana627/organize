#include "mso/ui/sample_context_menu.hpp"

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QMenu>
#include <QMimeData>
#include <QStringList>
#include <QUrl>
#include <QWidget>

#include "mso/ui/application_session.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/file_manager.hpp"

namespace mso::ui {

SampleContextMenu::SampleContextMenu(ApplicationSession& session, SampleMenuCommands commands, QWidget* parent_widget)
    : QObject(parent_widget), session_(session), commands_(commands), parent_widget_(parent_widget) {}

void SampleContextMenu::show(const QPoint& global_position, const std::vector<search::SampleRow>& selection,
                             const search::SampleRow& clicked) {
    if (selection.empty()) return;
    std::vector<SampleKey> keys;
    bool all_available = true;
    for (const auto& sample : selection) {
        keys.push_back({sample.library, sample.sample_id});
        all_available = all_available && sample.availability == search::RowAvailability::Available;
    }
    const SampleKey clicked_key{clicked.library, clicked.sample_id};
    const bool clicked_available = clicked.availability == search::RowAvailability::Available;
    const QString unavailable_reason = tr("Unavailable: the source folder is offline or the file is missing");
    const auto count = static_cast<int>(selection.size());

    QMenu menu(parent_widget_);
    menu.setToolTipsVisible(true);
    menu.addAction(commands_.play);
    menu.addSeparator();
    QAction* reveal_action =
        menu.addAction(FileManager::reveal_action_text(), this, [this, clicked_key] { reveal(clicked_key); });
    QAction* copy_path_action =
        menu.addAction(tr("Copy %n path(s)", nullptr, count), this, [this, keys] { copy_paths(keys); });
    QAction* copy_file_action =
        menu.addAction(tr("Copy %n file(s)", nullptr, count), this, [this, keys] { copy_files(keys); });
    for (QAction* action : {reveal_action, copy_path_action, copy_file_action}) {
        const bool enabled = action == reveal_action ? clicked_available : all_available;
        action->setEnabled(enabled);
        if (!enabled) action->setToolTip(unavailable_reason);
    }
    menu.addSeparator();
    QMenu* rating = menu.addMenu(tr("Rating"));
    for (QAction* action : commands_.ratings)
        rating->addAction(action);
    menu.addAction(commands_.toggle_favorite);
    QMenu* analysis = menu.addMenu(tr("Analyze"));
    analysis->addAction(commands_.analyze_waveforms);
    analysis->addAction(commands_.analyze_musical);
    analysis->addAction(commands_.analyze_duplicates);
    menu.addSeparator();
    menu.addAction(commands_.remove_from_library);
    menu.exec(global_position);
}

void SampleContextMenu::resolve_paths(const std::vector<SampleKey>& keys,
                                      std::function<void(std::vector<std::filesystem::path>)> use) {
    if (!session_.is_ready()) return;
    session_.run(
        this,
        [keys](app::Application& application) {
            std::vector<Result<std::filesystem::path>> results;
            results.reserve(keys.size());
            for (const SampleKey& key : keys)
                results.push_back(application.resolve_playable_path(key.library, key.sample_id));
            return results;
        },
        [this, use = std::move(use)](std::vector<Result<std::filesystem::path>> results) {
            std::vector<std::filesystem::path> paths;
            int unavailable = 0;
            const Error* first_error = nullptr;
            for (const auto& result : results) {
                if (result) {
                    paths.push_back(*result);
                } else {
                    ++unavailable;
                    if (!first_error) first_error = &result.error();
                }
            }
            // Never act on part of the request silently.
            if (first_error) {
                emit statusMessage(tr("Nothing done: %n sample(s) unavailable (%1).", nullptr, unavailable)
                                       .arg(DisplayFormat::error(*first_error)));
                return;
            }
            use(std::move(paths));
        });
}

void SampleContextMenu::reveal(const SampleKey& key) {
    resolve_paths({key}, [this](std::vector<std::filesystem::path> paths) {
        if (!paths.empty() && !FileManager::reveal_file(paths.front()))
            emit statusMessage(tr("Could not start the file manager."));
    });
}

void SampleContextMenu::copy_paths(const std::vector<SampleKey>& keys) {
    resolve_paths(keys, [this](std::vector<std::filesystem::path> paths) {
        QStringList lines;
        for (const auto& path : paths)
            lines << DisplayFormat::from_path(path);
        QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
        emit statusMessage(tr("Copied %n path(s)", nullptr, static_cast<int>(lines.size())));
    });
}

void SampleContextMenu::copy_files(const std::vector<SampleKey>& keys) {
    resolve_paths(keys, [this](std::vector<std::filesystem::path> paths) {
        QList<QUrl> urls;
        for (const auto& path : paths)
            urls << DisplayFormat::to_file_url(path);
        auto* data = new QMimeData;
        data->setUrls(urls);
        QGuiApplication::clipboard()->setMimeData(data);
        emit statusMessage(tr("Copied %n file(s)", nullptr, static_cast<int>(urls.size())));
    });
}

}  // namespace mso::ui
