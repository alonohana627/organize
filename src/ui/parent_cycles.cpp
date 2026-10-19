#include "mso/ui/detail/parent_cycles.hpp"

#include <algorithm>
#include <vector>

namespace mso::ui::detail {

std::set<std::int64_t> nodes_on_parent_cycles(const std::map<std::int64_t, std::optional<std::int64_t>>& parent_of) {
    enum class Visit { Walking, Done };
    std::map<std::int64_t, Visit> visits;
    std::set<std::int64_t> on_cycle;
    for (const auto& entry : parent_of) {
        std::vector<std::int64_t> path;
        std::optional<std::int64_t> node = entry.first;
        // Walk up until a top-level node, a missing parent or a node already settled.
        while (node && parent_of.contains(*node) && !visits.contains(*node)) {
            visits.emplace(*node, Visit::Walking);
            path.push_back(*node);
            node = parent_of.at(*node);
        }
        if (node && visits.contains(*node) && visits.at(*node) == Visit::Walking) {
            // The walk came back to a node of this very walk: from there on, the path is a cycle.
            for (auto member = std::ranges::find(path, *node); member != path.end(); ++member)
                on_cycle.insert(*member);
        }
        for (std::int64_t settled : path)
            visits[settled] = Visit::Done;
    }
    return on_cycle;
}

}  // namespace mso::ui::detail
