// Change feed: committed outbox envelopes served to consumers, consumer cursors and outbox pruning.
#include <algorithm>
#include <format>

#include "mso/catalog/detail/catalog_internal.hpp"
#include "mso/catalog/library.hpp"

namespace mso::catalog {

namespace {

constexpr std::size_t kMaximumConsumerNameCodePoints = 128;

/// Merges one stored envelope into the change set; false when it cannot be applied incrementally
/// (older envelope version, malformed payload, or too many samples listed).
bool merge_envelope(std::int64_t envelope_version, const std::string& payload_text, ChangeSet& changes,
                    std::vector<std::int64_t>& sample_ids) {
    if (envelope_version != detail::kOutboxEnvelopeVersion) return false;
    const auto payload = json_fields::parse_json(payload_text);
    if (json_fields::boolean_field(payload, "all_samples").value_or(false)) return false;
    const auto samples = json_fields::integer_array_field(payload, "samples");
    const auto structure = json_fields::boolean_field(payload, "structure");
    if (!samples || !structure) return false;
    sample_ids.insert(sample_ids.end(), samples->begin(), samples->end());
    changes.structure_changed = changes.structure_changed || *structure;
    return true;
}

}  // namespace

Result<ChangeSet> Library::changes_since(std::int64_t after_revision, std::size_t limit) {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto& connection = reader->connection();
    sqlite::ReadTransaction snapshot(connection);  // revision and envelopes from one snapshot
    if (auto began = snapshot.begin(); !began) return std::unexpected(began.error());
    auto current = connection.query_int64("SELECT catalog_revision FROM library_state WHERE singleton=1");
    if (!current) return std::unexpected(current.error());

    ChangeSet changes;
    changes.revision = *current;
    auto reload_everything = [&] {
        changes.full_reload = true;
        changes.structure_changed = true;
        changes.sample_ids.clear();
        changes.revision = *current;
        return changes;
    };
    if (after_revision == *current) return changes;
    if (after_revision > *current || after_revision < 0 || limit == 0) return reload_everything();

    auto statement = connection.prepare(
        "SELECT revision, envelope_version, payload_json FROM catalog_outbox WHERE revision>?1 "
        "ORDER BY revision LIMIT ?2");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, after_revision).bind(2, static_cast<std::int64_t>(std::min<std::size_t>(limit, 1'000'000)));
    std::vector<std::int64_t> sample_ids;
    changes.revision = after_revision;  // advanced envelope by envelope; must reach `current`
    std::int64_t expected_revision = after_revision + 1;
    bool incremental = true;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        if (!incremental) return {};
        const auto revision = row.column_int64(0);
        // A gap means envelopes were pruned (or never written): the consumer must reload.
        if (revision != expected_revision ||
            !merge_envelope(row.column_int64(1), row.column_text(2), changes, sample_ids)) {
            incremental = false;
            return {};
        }
        changes.revision = revision;
        ++expected_revision;
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    // Either a gap/unsupported envelope, or more envelopes remain than `limit`.
    if (!incremental || changes.revision != *current) return reload_everything();

    std::ranges::sort(sample_ids);
    const auto [duplicates_begin, duplicates_end] = std::ranges::unique(sample_ids);
    sample_ids.erase(duplicates_begin, duplicates_end);
    changes.sample_ids = std::move(sample_ids);
    return changes;
}

Result<void> Library::acknowledge_changes(std::string_view consumer, std::int64_t revision) {
    if (consumer.empty() || !is_valid_utf8(consumer) || count_code_points(consumer) > kMaximumConsumerNameCodePoints)
        return fail(ErrorCode::InvalidArgument, "a change consumer needs a name of 1–128 characters");
    if (revision < 0) return fail(ErrorCode::InvalidArgument, "a revision cannot be negative");
    if (read_only_) return {};  // nothing is written to the outbox of a read-only library
    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    auto upsert = writer_.prepare(
        "INSERT INTO consumer_cursors(name,revision) "
        "VALUES(?1,min(?2,(SELECT catalog_revision FROM library_state WHERE singleton=1))) "
        "ON CONFLICT(name) DO UPDATE SET revision=excluded.revision");
    if (!upsert) return std::unexpected(upsert.error());
    upsert->bind(1, consumer).bind(2, revision);
    if (auto stored = upsert->run(); !stored) return stored;
    // Every consumer has applied everything at or below the minimum cursor: those envelopes go.
    if (auto pruned = writer_.execute_script(
            "DELETE FROM catalog_outbox WHERE revision<=(SELECT min(revision) FROM consumer_cursors)");
        !pruned)
        return pruned;
    return transaction.commit();
}

}  // namespace mso::catalog
