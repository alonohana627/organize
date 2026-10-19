#include "mso/ui/edit_controller.hpp"

#include <QStringList>

#include <map>
#include <memory>
#include <set>

#include "mso/ui/display_format.hpp"
#include "mso/ui/edit_gateway.hpp"
#include "mso/ui/optimistic_edit.hpp"
#include "mso/ui/sample_table_model.hpp"

namespace mso::ui {

namespace {

/// Accumulates the per-library outcomes of one edit that spans several libraries.
struct MultiLibraryOutcome {
    std::size_t remaining = 0;
    std::int64_t applied = 0;
    std::int64_t conflicted = 0;
    QStringList failures;
    std::set<uuids::uuid> failed_libraries;  ///< libraries whose write failed (their rows are reverted)
};

/// Groups keys by library, preserving each library's selection order.
std::map<uuids::uuid, std::vector<std::int64_t>> group_by_library(const std::vector<SampleKey>& keys) {
    std::map<uuids::uuid, std::vector<std::int64_t>> groups;
    for (const SampleKey& key : keys)
        groups[key.library].push_back(key.sample_id);
    return groups;
}

}  // namespace

EditController::EditController(EditGateway& gateway, SampleTableModel& model, QObject* parent)
    : QObject(parent), gateway_(gateway), model_(model) {}

void EditController::apply(const std::vector<SampleKey>& keys, catalog::EditCommand command,
                           const QString& done_message) {
    if (keys.empty()) return;
    const bool removes = OptimisticEdit::removes_rows(command);
    // 1. The user sees the result now: loaded rows at once, evicted rows when they are read again.
    //    The rows as they were are kept, so a failed write can be undone on screen.
    auto previous = std::make_shared<std::vector<search::SampleRow>>();
    if (removes) {
        model_.remove_rows(keys);
    } else {
        const auto patch = [command](search::SampleRow& row) { OptimisticEdit::apply(command, row); };
        *previous = model_.patch_rows(keys, patch);
        model_.begin_pending_edit(keys, patch);
    }

    // 2. SQLite catches up in the background, one independent operation per library, each behind
    //    the library's earlier writes.
    const auto groups = group_by_library(keys);
    auto outcome = std::make_shared<MultiLibraryOutcome>();
    outcome->remaining = groups.size();
    for (const auto& [library, sample_ids] : groups) {
        catalog::EditCommand per_library = command;
        per_library.operation = new_uuid();  // idempotency key of this library's write
        per_library.sample_ids = sample_ids;
        last_edited_library_ = library;
        auto deliver = [this, outcome, library, keys, removes, previous,
                        done_message](Result<catalog::MutationReceipt> receipt) {
            if (receipt) {
                outcome->applied += receipt->applied;
                outcome->conflicted += receipt->conflicted;
            } else {
                outcome->failures << tr("%1: %2").arg(gateway_.library_name(library),
                                                      DisplayFormat::error(receipt.error()));
                outcome->failed_libraries.insert(library);
            }
            emit historyChanged(library);
            if (--outcome->remaining > 0) return;

            // 3. Reconcile with the index: shows catalog-computed values, or reverts a failure.
            if (removes) {
                if (!outcome->failures.isEmpty()) emit reloadRequested();
            } else {
                model_.end_pending_edit(keys);
                // A failure is undone on screen now, not only once the read below answers (it can
                // fail too); a success shows the catalog's own values when the read answers.
                if (!outcome->failed_libraries.empty()) {
                    std::vector<SampleKey> failed_keys;
                    for (const SampleKey& key : keys)
                        if (outcome->failed_libraries.contains(key.library)) failed_keys.push_back(key);
                    model_.revert_rows(failed_keys, *previous);
                }
                static_cast<void>(model_.refresh_rows(keys));  // reconciles; the count is for change events
            }
            if (!outcome->failures.isEmpty()) {
                emit failed(tr("The change was not saved"),
                            tr("%1 was undone on screen because saving failed:\n%2")
                                .arg(done_message, outcome->failures.join(QLatin1Char('\n'))));
                return;
            }
            QString message = tr("%1 (%n sample(s))", nullptr, static_cast<int>(outcome->applied)).arg(done_message);
            if (outcome->conflicted > 0)
                message = tr("%1; %n skipped because of conflicts", nullptr, static_cast<int>(outcome->conflicted))
                              .arg(message);
            emit succeeded(message);
        };
        enqueue_write(library, [this, library, per_library = std::move(per_library),
                                deliver = std::move(deliver)](std::function<void()> settled) mutable {
            gateway_.request_edit(library, per_library, this,
                                  [deliver = std::move(deliver),
                                   settled = std::move(settled)](Result<catalog::MutationReceipt> receipt) mutable {
                                      deliver(std::move(receipt));
                                      settled();
                                  });
        });
    }
}

void EditController::apply_taxonomy(const uuids::uuid& library, const catalog::TaxonomyCommand& command) {
    last_edited_library_ = library;
    enqueue_write(library, [this, library, command](std::function<void()> settled) {
        gateway_.request_taxonomy(
            library, command, this,
            [this, library, settled = std::move(settled)](Result<catalog::MutationReceipt> receipt) {
                if (!receipt) {
                    emit failed(tr("The change was not applied"), DisplayFormat::error(receipt.error()));
                } else {
                    emit succeeded(tr("Saved"));
                    emit historyChanged(library);
                    emit structureChanged(library);
                }
                settled();
            });
    });
}

void EditController::undo(const uuids::uuid& library) {
    undo_or_redo(library, false);
}

void EditController::redo(const uuids::uuid& library) {
    undo_or_redo(library, true);
}

void EditController::undo_or_redo(const uuids::uuid& library, bool redo) {
    enqueue_write(library, [this, library, redo](std::function<void()> settled) {
        auto deliver = [this, library, redo, settled = std::move(settled)](Result<catalog::MutationReceipt> receipt) {
            emit historyChanged(library);
            if (!receipt) {
                const QString title = redo ? tr("Redo failed") : tr("Undo failed");
                emit failed(title, receipt.error().code == ErrorCode::HistoryConflict
                                       ? tr("A later edit changed the same values, so nothing was overwritten. "
                                            "The conflict was recorded for review.")
                                       : DisplayFormat::error(receipt.error()));
            } else {
                emit succeeded(redo ? tr("Redone") : tr("Undone"));
            }
            settled();
        };
        if (redo)
            gateway_.request_redo(library, this, std::move(deliver));
        else
            gateway_.request_undo(library, this, std::move(deliver));
    });
}

std::size_t EditController::queued_writes(const uuids::uuid& library) const {
    const auto found = queued_writes_.find(library);
    return found == queued_writes_.end() ? 0 : found->second.size();
}

void EditController::enqueue_write(const uuids::uuid& library, Write write) {
    queued_writes_[library].push_back(std::move(write));
    if (!writing_libraries_.contains(library)) start_next_write(library);
}

void EditController::start_next_write(const uuids::uuid& library) {
    const auto queue = queued_writes_.find(library);
    if (queue == queued_writes_.end() || queue->second.empty()) {
        writing_libraries_.erase(library);
        if (queue != queued_writes_.end()) queued_writes_.erase(queue);
        return;
    }
    Write write = std::move(queue->second.front());
    queue->second.pop_front();
    writing_libraries_.insert(library);
    // Deliveries never run before the request call returns, so `settled` cannot re-enter here.
    write([this, library] { start_next_write(library); });
}

}  // namespace mso::ui
