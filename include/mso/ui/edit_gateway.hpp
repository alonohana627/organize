#pragma once
/// @file
/// EditGateway: where user edits are written. ApplicationSession implements it with the
/// Application's edit API on a worker pool; tests use a fake that can succeed or fail on demand.
#include <QObject>
#include <QString>

#include <functional>

#include "mso/catalog/edit_command.hpp"
#include "mso/catalog/types.hpp"
#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"

namespace mso::ui {

/// Asynchronous metadata writes. Deliveries follow SamplePageSource's rules: later, on
/// `context`'s thread, skipped when `context` was destroyed.
class EditGateway {
public:
    /// Receives the outcome of one write.
    using ReceiptDelivery = std::function<void(Result<catalog::MutationReceipt>)>;

    virtual ~EditGateway() = default;

    /// Applies a per-sample edit to one library (Application::apply_edit, which also patches the
    /// in-memory index before the change event).
    virtual void request_edit(const uuids::uuid& library, const catalog::EditCommand& command, QObject* context,
                              ReceiptDelivery deliver) = 0;
    /// Applies a structural edit (tags, categories, collections) to one library.
    virtual void request_taxonomy(const uuids::uuid& library, const catalog::TaxonomyCommand& command, QObject* context,
                                  ReceiptDelivery deliver) = 0;
    /// Undoes the library's most recent action.
    virtual void request_undo(const uuids::uuid& library, QObject* context, ReceiptDelivery deliver) = 0;
    /// Redoes the library's most recently undone action.
    virtual void request_redo(const uuids::uuid& library, QObject* context, ReceiptDelivery deliver) = 0;
    /// Display name of a library, for messages.
    virtual QString library_name(const uuids::uuid& library) const = 0;

protected:
    EditGateway() = default;
    EditGateway(const EditGateway&) = default;
    EditGateway& operator=(const EditGateway&) = default;
};

}  // namespace mso::ui
