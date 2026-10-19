#pragma once
/// @file
/// The workspace preference keys the desktop UI reads and writes, in one place.
///
/// UI settings live in the machine-local workspace (docs/02_ARCHITECTURE.md §4) rather than in
/// QSettings: one versioned file holds libraries, root bindings and preferences together, and a
/// `--workspace` profile (tests, separate setups) isolates all of them at once, which QSettings'
/// registry/ini location would not.
#include <array>
#include <string_view>

namespace mso::ui {

/// Names of the preferences owned by the UI.
struct UiPreferenceKeys {
    /// Customized keyboard shortcuts (ShortcutCatalog JSON).
    static constexpr std::string_view kShortcuts = "ui.shortcuts";
    /// Layout active when the application last closed (LayoutStore JSON).
    static constexpr std::string_view kCurrentLayout = "ui.layout.current";
    /// Named layout presets: {name: layout}.
    static constexpr std::string_view kLayoutPresets = "ui.layout.presets";
    /// Whether selecting a sample starts playing it.
    static constexpr std::string_view kAuditionOnSelection = "preview.audition_on_selection";
    /// Preview volume in decibels.
    static constexpr std::string_view kPreviewVolume = "preview.volume_db";

    /// Every key above, preloaded into ApplicationSession's cache at start-up.
    static constexpr std::array<std::string_view, 5> kAll = {kShortcuts, kCurrentLayout, kLayoutPresets,
                                                             kAuditionOnSelection, kPreviewVolume};
};

}  // namespace mso::ui
