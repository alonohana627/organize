#include "mso/ui/undo_router.hpp"

#include <algorithm>

namespace mso::ui {

std::optional<uuids::uuid> UndoRouter::choose(const UndoContext& context) {
    const auto is_open = [&context](const uuids::uuid& library) {
        return std::ranges::find(context.open_libraries, library) != context.open_libraries.end();
    };
    for (const auto& candidate : {context.current_row_library, context.history_library, context.navigator_library,
                                  context.last_edited_library}) {
        if (candidate && is_open(*candidate)) return candidate;
    }
    if (context.open_libraries.size() == 1) return context.open_libraries.front();
    return std::nullopt;
}

}  // namespace mso::ui
