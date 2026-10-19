#pragma once
/// @file
/// UndoRouter: which library Undo and Redo act on.
#include <optional>
#include <vector>

#include "mso/domain/core.hpp"

namespace mso::ui {

/// What the window knows when the user presses Undo or Redo.
struct UndoContext {
    std::optional<uuids::uuid> current_row_library;  ///< library of the table's current row
    std::optional<uuids::uuid> history_library;      ///< library shown in the History dock
    std::optional<uuids::uuid> navigator_library;    ///< library of the navigator's current node
    std::optional<uuids::uuid> last_edited_library;  ///< library of the most recent edit
    std::vector<uuids::uuid> open_libraries;         ///< every open library
};

/// History is per library (docs/03 §9), so Undo needs one library. The rule, in order: the
/// current row's library, the History dock's, the navigator's, the last edited one if still open,
/// the only open library; otherwise none (the user is asked to select something).
class UndoRouter {
public:
    /// Library Undo/Redo should act on, or nullopt when it is ambiguous.
    static std::optional<uuids::uuid> choose(const UndoContext& context);
};

}  // namespace mso::ui
