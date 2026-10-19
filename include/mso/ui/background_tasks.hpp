#pragma once
/// @file
/// Runs blocking work on a thread pool and delivers its result on a QObject's thread.
///
/// This is the only way UI code reaches blocking APIs (docs/02_ARCHITECTURE.md §3): the work runs
/// on the pool, and the delivery runs later, queued, on the thread that owns `context` (the UI
/// thread for widgets and models). When `context` is destroyed first, the delivery is skipped,
/// so a closed panel never receives a result.
#include <QFuture>
#include <QObject>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <type_traits>
#include <utility>

namespace mso::ui {

/// Runs `work()` on `pool` and then `deliver(result)` on `context`'s thread. For work returning
/// void, `deliver()` takes no argument. Never delivers synchronously, even if the work is trivial.
template <class Work, class Deliver>
void run_in_background(QThreadPool& pool, QObject* context, Work&& work, Deliver&& deliver) {
    using WorkResult = std::invoke_result_t<Work&>;
    QFuture<WorkResult> future = QtConcurrent::run(&pool, std::forward<Work>(work));
    if constexpr (std::is_void_v<WorkResult>) {
        future.then(context, [deliver = std::forward<Deliver>(deliver)]() mutable { deliver(); });
    } else {
        future.then(context, [deliver = std::forward<Deliver>(deliver)](QFuture<WorkResult> finished) mutable {
            deliver(finished.takeResult());
        });
    }
}

/// Runs `work()` on `pool` without delivering anything back (fire-and-forget persistence).
template <class Work>
void run_detached(QThreadPool& pool, Work&& work) {
    pool.start(std::forward<Work>(work));
}

}  // namespace mso::ui
