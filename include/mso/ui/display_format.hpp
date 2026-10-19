#pragma once
/// @file
/// Conversions between core value types (UTF-8 strings, paths, musical facts) and display text.
#include <QCoreApplication>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "mso/catalog/types.hpp"
#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"
#include "mso/search/search_service.hpp"

namespace mso::ui {

/// Stateless formatting helpers shared by the table, the inspector and the dialogs. All
/// user-visible words go through tr() so they can be externalized later.
class DisplayFormat {
    Q_DECLARE_TR_FUNCTIONS(DisplayFormat)

public:
    /// UTF-8 from the core to a QString (Hebrew and other scripts survive untouched).
    static QString from_utf8(std::string_view text);
    /// QString to the UTF-8 the core expects.
    static std::string to_utf8(const QString& text);
    /// Native path for display (backslashes on Windows) from a filesystem path.
    static QString from_path(const std::filesystem::path& path);
    /// Filesystem path from user-chosen text, without lossy code-page conversion.
    static std::filesystem::path to_path(const QString& text);

    /// Canonical text of a UUID (for item data and persisted UI state).
    static QString from_uuid(const uuids::uuid& id);
    /// Parses canonical UUID text; nullopt for anything else.
    static std::optional<uuids::uuid> to_uuid(const QString& text);

    /// "Unknown" placeholder used wherever a value is known to be missing (not loading).
    static QString unknown();
    /// Compact table marker for an unknown value ("—"); the tooltip spells out unknown().
    static QString unknown_marker();
    /// "1.25 s" / "1:02"; estimated durations are prefixed with "≈".
    static QString duration(std::optional<std::int64_t> microseconds, bool exact);
    static QString sample_rate(std::optional<std::int64_t> hertz);
    static QString channels(std::optional<std::int64_t> count);
    /// Bit depth, or empty for compressed formats where it is not meaningful.
    static QString bit_depth(std::optional<std::int64_t> bits);
    static QString byte_size(std::optional<std::int64_t> bytes);
    static QString bpm(std::optional<std::int64_t> milli_bpm);
    static QString key(const std::optional<MusicalKey>& key);
    /// Short label of a metadata source: "tag", "filename", "analysis", "user"...
    static QString source_label(std::string_view source);
    /// "filename, 80% confidence" style provenance line for BPM/key tooltips and the inspector.
    static QString provenance(std::string_view source, std::optional<double> confidence);
    /// "★★★☆☆" for 0..5; 0 renders as empty stars.
    static QString rating_stars(int rating);
    /// Spoken form for screen readers: "Rated 3 of 5" / "Unrated".
    static QString rating_accessible(int rating);
    static QString availability(search::RowAvailability availability);
    static QString root_availability(catalog::Availability availability);
    static QString location_state(catalog::LocationState state);
    /// Names of the analysis stages recorded in SampleRow::analysis_flags, or "None".
    static QString analysis_flags(std::int64_t flags);
    /// Local date and time of a UTC Unix-milliseconds timestamp.
    static QString timestamp(std::int64_t unix_milliseconds);
    /// User-facing text of a core error (safe detail only).
    static QString error(const Error& error);
    /// Thousands-separated integer in the user's locale.
    static QString count(std::int64_t value);
};

}  // namespace mso::ui
