#include "mso/ui/display_format.hpp"

#include <QDateTime>
#include <QDir>
#include <QLocale>
#include <QStringList>

#include <cmath>

namespace mso::ui {

namespace {

/// Bits of SampleRow::analysis_flags (see catalog/library_analysis.cpp).
constexpr std::int64_t kAnalyzedTempo = 1;
constexpr std::int64_t kAnalyzedKey = 2;
constexpr std::int64_t kAnalyzedWaveform = 4;
constexpr std::int64_t kAnalyzedHash = 8;
constexpr std::int64_t kAnalyzedLoudness = 16;

}  // namespace

QString DisplayFormat::from_utf8(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string DisplayFormat::to_utf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString DisplayFormat::from_path(const std::filesystem::path& path) {
    return QDir::toNativeSeparators(QString::fromStdU16String(path.generic_u16string()));
}

std::filesystem::path DisplayFormat::to_path(const QString& text) {
    return std::filesystem::path(QDir::fromNativeSeparators(text).toStdU16String());
}

QString DisplayFormat::from_uuid(const uuids::uuid& id) {
    return from_utf8(uuids::to_string(id));
}

std::optional<uuids::uuid> DisplayFormat::to_uuid(const QString& text) {
    return uuids::uuid::from_string(to_utf8(text));
}

QString DisplayFormat::unknown() {
    return tr("Unknown");
}

QString DisplayFormat::unknown_marker() {
    return QString(QChar(0x2014));
}

QString DisplayFormat::duration(std::optional<std::int64_t> microseconds, bool exact) {
    if (!microseconds) return {};
    const QString text = from_utf8(format_duration(*microseconds));
    return exact ? text : tr("≈%1").arg(text);
}

QString DisplayFormat::sample_rate(std::optional<std::int64_t> hertz) {
    if (!hertz) return {};
    if (*hertz % 1000 == 0) return tr("%1 kHz").arg(*hertz / 1000);
    return tr("%1 kHz").arg(static_cast<double>(*hertz) / 1000.0, 0, 'f', 1);
}

QString DisplayFormat::channels(std::optional<std::int64_t> count) {
    if (!count) return {};
    if (*count == 1) return tr("Mono");
    if (*count == 2) return tr("Stereo");
    return tr("%1 ch").arg(*count);
}

QString DisplayFormat::bit_depth(std::optional<std::int64_t> bits) {
    if (!bits || *bits <= 0) return {};
    return tr("%1-bit").arg(*bits);
}

QString DisplayFormat::byte_size(std::optional<std::int64_t> bytes) {
    if (!bytes) return {};
    return from_utf8(format_byte_size(*bytes));
}

QString DisplayFormat::bpm(std::optional<std::int64_t> milli_bpm) {
    if (!milli_bpm) return {};
    return from_utf8(format_bpm(*milli_bpm));
}

QString DisplayFormat::key(const std::optional<MusicalKey>& musical_key) {
    if (!musical_key) return {};
    return from_utf8(format_key(*musical_key));
}

QString DisplayFormat::source_label(std::string_view source) {
    if (source == "user") return tr("user");
    if (source == "user_unknown") return tr("set to unknown");
    if (source == "container") return tr("file tag");
    if (source == "filename") return tr("filename");
    if (source == "analysis") return tr("analysis");
    if (source == "accepted_suggestion") return tr("accepted suggestion");
    if (source.empty()) return tr("no source");
    return from_utf8(source);
}

QString DisplayFormat::provenance(std::string_view source, std::optional<double> confidence) {
    QString text = source_label(source);
    if (confidence && std::isfinite(*confidence))
        text = tr("%1, %2% confidence").arg(text).arg(std::lround(*confidence * 100.0));
    return text;
}

QString DisplayFormat::rating_stars(int rating) {
    QString stars;
    for (int star = 1; star <= 5; ++star)
        stars += star <= rating ? QChar(0x2605) : QChar(0x2606);
    return stars;
}

QString DisplayFormat::rating_accessible(int rating) {
    return rating <= 0 ? tr("Unrated") : tr("Rated %1 of 5").arg(rating);
}

QString DisplayFormat::availability(search::RowAvailability value) {
    switch (value) {
        case search::RowAvailability::Available:
            return tr("Available");
        case search::RowAvailability::SourceOffline:
            return tr("Source offline");
        case search::RowAvailability::Missing:
            return tr("Missing");
        case search::RowAvailability::Unknown:
            return tr("Unknown");
    }
    return tr("Unknown");
}

QString DisplayFormat::root_availability(catalog::Availability value) {
    switch (value) {
        case catalog::Availability::Online:
            return tr("Online");
        case catalog::Availability::Offline:
            return tr("Offline");
        case catalog::Availability::Unknown:
            return tr("Unknown");
        case catalog::Availability::Denied:
            return tr("Access denied");
    }
    return tr("Unknown");
}

QString DisplayFormat::location_state(catalog::LocationState state) {
    switch (state) {
        case catalog::LocationState::Present:
            return tr("Present");
        case catalog::LocationState::Missing:
            return tr("Missing");
        case catalog::LocationState::Unknown:
            return tr("Unknown");
        case catalog::LocationState::Denied:
            return tr("Access denied");
        case catalog::LocationState::Changed:
            return tr("Changed");
        case catalog::LocationState::Retired:
            return tr("Retired");
    }
    return tr("Unknown");
}

QString DisplayFormat::analysis_flags(std::int64_t flags) {
    QStringList stages;
    if (flags & kAnalyzedWaveform) stages << tr("waveform");
    if (flags & kAnalyzedTempo) stages << tr("tempo");
    if (flags & kAnalyzedKey) stages << tr("key");
    if (flags & kAnalyzedLoudness) stages << tr("loudness");
    if (flags & kAnalyzedHash) stages << tr("hash");
    return stages.isEmpty() ? tr("None") : stages.join(QStringLiteral(", "));
}

QString DisplayFormat::timestamp(std::int64_t unix_milliseconds) {
    if (unix_milliseconds <= 0) return {};
    const QDateTime local = QDateTime::fromMSecsSinceEpoch(unix_milliseconds).toLocalTime();
    return QLocale().toString(local, QLocale::ShortFormat);
}

QString DisplayFormat::error(const Error& failure) {
    return from_utf8(failure.message());
}

QString DisplayFormat::count(std::int64_t value) {
    return QLocale().toString(static_cast<qlonglong>(value));
}

}  // namespace mso::ui
