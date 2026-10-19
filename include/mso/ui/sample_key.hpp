#pragma once
/// @file
/// SampleKey: the UI's stable identity of one sample across every open library.
#include "mso/app/application.hpp"

namespace mso::ui {

/// Identifies one sample across all open libraries: the library UUID plus the library-local sample
/// row id. The UI uses the Application's key type (contract "app → ui") so keys pass to
/// Application::rows() unchanged. Selection, drag, edits and undo routing use keys, never table
/// row numbers, so they survive re-sorting, refreshes and paging.
using SampleKey = app::SampleKey;

}  // namespace mso::ui
