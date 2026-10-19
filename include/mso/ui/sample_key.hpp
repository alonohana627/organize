#pragma once
/// @file
/// SampleKey: the UI's stable identity of one sample across every open library.
#include <cstdint>

#include "mso/domain/core.hpp"

namespace mso::ui {

/// Identifies one sample across all open libraries: the library UUID plus the library-local
/// sample row id. Selection, drag and undo routing use keys, never table row numbers, so they
/// survive re-sorting, refreshes and paging.
struct SampleKey {
    uuids::uuid library;
    std::int64_t sample_id = 0;

    bool operator==(const SampleKey& other) const { return sample_id == other.sample_id && library == other.library; }
    /// Strict weak order so keys can live in std::set / std::map.
    bool operator<(const SampleKey& other) const {
        if (library == other.library) return sample_id < other.sample_id;
        return library < other.library;
    }
};

}  // namespace mso::ui
