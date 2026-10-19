#pragma once
/// @file
/// Runs blocking work on a thread pool and delivers its result on a QObject's thread.
///
/// This is the only way UI code reaches blocking APIs (docs/02_ARCHITECTURE.md §3): the work runs
/// on the pool, and the delivery runs later, queued, on the thread that owns `context` (the UI
/// thread for widgets and models). When `context` is destroyed first, the delivery is skipped,
/// so a closed panel never receives a result.
///
/// Exceptions never cross the thread boundary: the work runs inside a try/catch barrier that turns
/// an escaping exception into an INTERNAL error (for Result-returning work) or a logged default
/// value, so QFuture::takeResult() cannot rethrow on the UI thread.
#include <QFuture>
#include <QObject>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <QtGlobal>

#include <exception>
#include <expected>
#include <string>
#include <type_traits>
#include <utility>

#include "mso/domain/error.hpp"

namespace mso::ui {

namespace detail {

/// True for mso::Result<T> (std::expected<T, Error>).
template <class Type>
struct IsResult : std::false_type {};
template <class Value>
struct IsResult<std::expected<Value, Error>> : std::true_type {};

/// What a failed (thrown) piece of work hands to its delivery instead of a value.
template <class WorkResult>
WorkResult value_for_exception(const char* what) {
    if constexpr (IsResult<WorkResult>::value) {
        return fail(ErrorCode::Internal, std::string("unexpected exception: ") + what);
    } else {
        qCritical("Background task failed with an exception: %s", what);
        return WorkResult{};
    }
}

/// Wraps `work` in the exception barrier described in the file comment.
template <class Work>
auto guarded(Work&& work) {
    return [work = std::forward<Work>(work)]() mutable -> std::invoke_result_t<Work&> {
        using WorkResult = std::invoke_result_t<Work&>;
        try {
            return work();
        } catch (const std::exception& error) {
            if constexpr (std::is_void_v<WorkResult>)
                qCritical("Background task failed with an exception: %s", error.what());
            else
                return value_for_exception<WorkResult>(error.what());
        } catch (...) {
            if constexpr (std::is_void_v<WorkResult>)
                qCritical("Background task failed with an unknown exception");
            else
                return value_for_exception<WorkResult>("unknown exception");
        }
    };
}

}  // namespace detail

/// Runs `work()` on `pool` and then `deliver(result)` on `context`'s thread. For work returning
/// void, `deliver()` takes no argument. Never delivers synchronously, even if the work is trivial.
template <class Work, class Deliver>
void run_in_background(QThreadPool& pool, QObject* context, Work&& work, Deliver&& deliver) {
    using WorkResult = std::invoke_result_t<Work&>;
    QFuture<WorkResult> future = QtConcurrent::run(&pool, detail::guarded(std::forward<Work>(work)));
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
    pool.start(detail::guarded(std::forward<Work>(work)));
}

}  // namespace mso::ui
