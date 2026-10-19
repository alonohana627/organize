// libFuzzer target: the two paging-cursor decoders on malformed tokens (crash-proofing).
//
// A cursor is an opaque token the caller passes back for the next page: the desktop keeps it in
// memory, the CLI prints it and reads it back from the command line (`mso search --cursor`). Both
// decoders must turn any token into a page or STALE_CURSOR / INVALID_ARGUMENT, never a crash, an
// exception or an unbounded allocation:
//   - search::SearchService (the SQL path, src/search/search_service.cpp);
//   - app::search_indexes (the desktop's in-memory SampleIndex, src/app/sample_index.cpp).
//
// Input: the first byte chooses the decoder (bit 0), the sort field (bits 1-4) and the direction
// (bit 5); the rest is the token. A token that parses as a JSON object is applied as a JSON merge
// patch (RFC 7386) to a real cursor of the same request, so the fuzzer explores the body of valid
// tokens (positions, sort values, ids) instead of stopping at the fingerprint check; any other token is
// passed as it is. The catalog is a small library in a temporary directory, created once per process.
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "mso/app/sample_index.hpp"
#include "mso/catalog/library.hpp"
#include "mso/search/search_service.hpp"

namespace {

using mso::search::SortField;

/// Every sort field, indexed by bits 1-4 of the first input byte (modulo the count).
constexpr std::array kSortFields{SortField::Name,       SortField::Added,    SortField::Duration, SortField::Size,
                                 SortField::SampleRate, SortField::Bpm,      SortField::Key,      SortField::Rating,
                                 SortField::Format,     SortField::Relevance};

/// Samples in the fixture library: enough for several one-row pages in every sort order.
constexpr int kFixtureSamples = 12;

/// The fixture: one library with a root of synthetic samples, and its loaded SampleIndex. The
/// temporary directory is removed when the process exits normally.
class Fixture {
public:
    Fixture() {
        std::random_device entropy;
        directory_ = std::filesystem::temp_directory_path() /
                     ("mso-fuzz-cursor-" + std::to_string(entropy()) + "-" + std::to_string(entropy()));
        auto created = mso::catalog::Library::create(directory_ / "Library", "Fuzz");
        if (!created) __builtin_trap();
        library_ = std::move(*created);
        auto root = library_->add_root("Samples", mso::catalog::RootMode::Referenced);
        if (!root) __builtin_trap();
        auto session = library_->begin_scan(root->id);
        if (!session) __builtin_trap();
        std::vector<mso::catalog::ScannedFile> files;
        for (int index = 0; index < kFixtureSamples; ++index) {
            mso::catalog::ScannedFile file;
            file.relative_path = "Folder " + std::to_string(index % 3) + "/sample " + std::to_string(index) + ".wav";
            file.byte_size = 1000 + (index * 37 % 11) * 100;
            file.mtime_ns = 1'700'000'000'000'000'000 + index;
            mso::catalog::ObservedMetadata metadata;
            metadata.format = index % 2 == 0 ? "wav" : "flac";
            metadata.codec = index % 2 == 0 ? "pcm_s16" : "flac";
            metadata.sample_rate = index % 3 == 0 ? 48'000 : 44'100;
            metadata.channels = 2;
            metadata.bit_depth = 16;
            metadata.duration_us = 250'000 * (1 + index % 4);  // equal values: ties break on the id
            metadata.duration_exact = true;
            if (index % 4 != 0) metadata.embedded_bpm_milli = 90'000 + 10'000 * (index % 3);  // some unknown
            file.metadata = metadata;
            files.push_back(std::move(file));
        }
        if (!library_->commit_scan_batch(*session, files, {}, [](std::string_view) { return false; })) __builtin_trap();
        if (!library_->finish_scan(*session, false)) __builtin_trap();
        index_ = std::make_unique<mso::app::SampleIndex>(library_->uuid());
        if (!index_->load(*library_)) __builtin_trap();
    }

    ~Fixture() {
        index_.reset();
        library_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    /// One page through the chosen decoder; the result itself is not checked, only that it returns.
    mso::Result<mso::search::SearchPage> search(bool through_index, const mso::search::SearchRequest& request) {
        if (through_index) {
            const std::array libraries{mso::app::IndexedLibrary{index_.get(), library_.get(), "Fuzz"}};
            return mso::app::search_indexes(libraries, request);
        }
        const std::array libraries{library_.get()};
        return service_.search(libraries, request);
    }

    /// A real continuation token of `request` (its first one-row page), cached per request shape.
    const std::optional<nlohmann::json>& valid_cursor(bool through_index, const mso::search::SearchRequest& request) {
        const auto key = std::make_tuple(through_index, request.sort, request.descending);
        if (const auto found = valid_cursors_.find(key); found != valid_cursors_.end()) return found->second;
        std::optional<nlohmann::json> cursor;
        if (const auto page = search(through_index, request); page && page->next) {
            auto parsed = nlohmann::json::parse(page->next->token, nullptr, false);
            if (parsed.is_object()) cursor = std::move(parsed);
        }
        return valid_cursors_.emplace(key, std::move(cursor)).first->second;
    }

private:
    std::filesystem::path directory_;
    std::unique_ptr<mso::catalog::Library> library_;
    std::unique_ptr<mso::app::SampleIndex> index_;
    mso::search::SearchService service_;
    std::map<std::tuple<bool, SortField, bool>, std::optional<nlohmann::json>> valid_cursors_;
};

}  // namespace

/// libFuzzer entry point: decodes the token in `data` through the decoder the first byte selects.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    static Fixture fixture;
    const std::uint8_t selector = data[0];
    const bool through_index = (selector & 0x1U) != 0;
    mso::search::SearchRequest request;
    request.sort = kSortFields[static_cast<std::size_t>((selector >> 1U) & 0xFU) % kSortFields.size()];
    request.descending = (selector & 0x20U) != 0;
    request.page_size = 1;
    if (through_index && !mso::app::SampleIndex::serves(request)) return 0;

    const std::string_view token(reinterpret_cast<const char*>(data + 1), size - 1);
    const auto patch = nlohmann::json::parse(token, nullptr, false);
    const auto& valid = fixture.valid_cursor(through_index, request);
    if (patch.is_object() && valid) {
        nlohmann::json patched = *valid;
        patched.merge_patch(patch);
        request.cursor = mso::search::Cursor{patched.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)};
    } else {
        request.cursor = mso::search::Cursor{std::string(token)};
    }
    [[maybe_unused]] const auto page = fixture.search(through_index, request);
    return 0;
}
