// Import and reconciliation against a real folder: probing, renames, changes, missing files,
// cancellation with resume, small batches, probe failures, catalog failures mid-batch, gaps for
// unreadable directories, link policy, cycles and duplicate links, skipped entries that must not
// become missing (links, cloud-only files), stored stats across batches, long paths and the depth
// limit, unsafe paths, file identity and invariant 11.
#include <doctest.h>

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "mso/files/detail/file_identity.hpp"
#include "mso/files/importer.hpp"
#include "mso/media/native_file.hpp"
#include "mso/search/search_service.hpp"
#include "test_support.hpp"

#ifdef _WIN32
#include <sddl.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace mso;
using mso::test::ScratchDirectory;

namespace {

namespace fs = std::filesystem;

/// A source folder with three audio files and one text file, plus a library with one root bound to it.
struct ImportFixture {
    ScratchDirectory scratch{"import"};
    fs::path source;
    std::unique_ptr<catalog::Library> library;
    std::int64_t root_id = 0;

    ImportFixture() : source(scratch / "samples") {
        test::write_sine_wav(source / "Drums/Kick 01.wav", 0.3, 60);
        test::write_sine_wav(source / "Loops/Groove_124bpm_Am.wav", 1.0, 220);
        test::write_sine_wav(source / path_from_utf8("FX/שלום.wav"), 0.2, 880);
        test::write_file(source / "FX/readme.txt", std::string_view("not audio"));
        auto created = catalog::Library::create(scratch / "lib", "Imports");
        REQUIRE(created.has_value());
        library = std::move(*created);
        auto root = library->add_root("Samples", catalog::RootMode::Referenced);
        REQUIRE(root.has_value());
        root_id = root->id;
    }

    files::ImportSummary import(const files::ImportOptions& options = {}, std::stop_token stop = {}) {
        auto summary = files::import_root(*library, root_id, source, stop, {}, options);
        REQUIRE_MESSAGE(summary.has_value(), (summary ? std::string() : summary.error().message()));
        return *summary;
    }

    /// Samples the catalog reports as missing.
    std::size_t missing_count() {
        catalog::Library* scope[] = {library.get()};
        auto missing = search::SearchService().search(scope, {.text = "available:false"});
        REQUIRE(missing.has_value());
        return missing->rows.size();
    }
};

#ifdef _WIN32
/// Replaces a path's DACL with the given SDDL string; true on success.
bool set_access_control(const fs::path& path, const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr))
        return false;
    const BOOL applied = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    return applied != FALSE;
}
#endif

/// Makes a directory unlistable for this process while it lives (and listable again afterwards).
class ListingDenied {
public:
    explicit ListingDenied(fs::path directory) : directory_(std::move(directory)) {
#ifdef _WIN32
        // Protected DACL: Everyone may do everything except list this directory.
        applied_ = set_access_control(directory_, L"D:P(D;;0x1;;;WD)(A;OICI;FA;;;WD)");
#else
        applied_ = ::chmod(directory_.c_str(), 0) == 0 && ::geteuid() != 0;  // root ignores permissions
#endif
        // Some accounts bypass the restriction anyway (root, or an elevated Windows CI runner with
        // backup privileges), so check that listing really fails, the same way the importer lists.
        if (applied_) {
            std::error_code error;
            const fs::directory_iterator listing(directory_, error);
            applied_ = static_cast<bool>(error);
        }
    }
    ListingDenied(const ListingDenied&) = delete;
    ListingDenied& operator=(const ListingDenied&) = delete;
    ~ListingDenied() {
#ifdef _WIN32
        set_access_control(directory_, L"D:P(A;OICI;FA;;;WD)");
#else
        ::chmod(directory_.c_str(), 0755);
#endif
    }
    /// False when the platform refused (or, as root, would ignore) the restriction.
    bool applied() const { return applied_; }

private:
    fs::path directory_;
    bool applied_ = false;
};

/// True when this process can make a directory unlistable (false as root, or for an account that
/// bypasses the ACL, such as an elevated runner with backup privileges). Decided once, before the
/// tests run, so the case that needs it is reported as skipped instead of passing silently.
bool listing_restriction_unavailable() {
    static const bool unavailable = [] {
        std::error_code error;
        const fs::path probe =
            fs::temp_directory_path(error) /
            ("mso-listing-probe-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (error || !fs::create_directories(probe, error)) return true;
        bool applied = false;
        {
            ListingDenied denied(probe);
            applied = denied.applied();
        }
        fs::remove_all(probe, error);
        return !applied;
    }();
    return unavailable;
}

/// A directory link (junction on Windows, symbolic link elsewhere) removed again on destruction,
/// before the scratch directory is deleted, so cleanup never walks through it.
class DirectoryLink {
public:
    DirectoryLink(fs::path link, const fs::path& target) : link_(std::move(link)) {
#ifdef _WIN32
        // Junctions need no privileges, unlike symbolic links. _wsystem passes the UTF-16 paths
        // through unchanged (std::system would read UTF-8 in the ANSI code page).
        const std::wstring command = L"mklink /J \"" + link_.lexically_normal().make_preferred().native() + L"\" \"" +
                                     fs::path(target).lexically_normal().make_preferred().native() + L"\" >NUL";
        created_ = _wsystem(command.c_str()) == 0;
#else
        std::error_code error;
        fs::create_directory_symlink(target, link_, error);
        created_ = !error;
#endif
    }
    DirectoryLink(const DirectoryLink&) = delete;
    DirectoryLink& operator=(const DirectoryLink&) = delete;
    ~DirectoryLink() {
#ifdef _WIN32
        RemoveDirectoryW(link_.c_str());  // removes the junction itself, never its target
#else
        std::error_code ignored;
        fs::remove(link_, ignored);
#endif
    }
    /// True when the link exists.
    bool created() const { return created_; }

private:
    fs::path link_;
    bool created_ = false;
};

/// Bytes and last write time of a file, for the invariant-11 check.
struct FileSnapshot {
    std::string bytes;
    fs::file_time_type last_write;
    bool operator==(const FileSnapshot&) const = default;
};

FileSnapshot snapshot(const fs::path& file) {
    return {test::read_file(file), fs::last_write_time(file)};
}

}  // namespace

TEST_SUITE("import") {
    TEST_CASE("import catalogs audio files with probed metadata and filename hints, skipping non-audio") {
        ImportFixture fixture;
        const auto summary = fixture.import();
        CHECK(summary.progress.added == 3);
        CHECK(summary.progress.files_probed == 3);
        CHECK(summary.progress.probe_failures == 0);
        CHECK(summary.progress.finished);
        CHECK_FALSE(summary.progress.cancelled);
        CHECK(summary.finish.complete_coverage);
        auto groove = fixture.library->sample_detail(test::sample_id_named(*fixture.library, "Groove_124bpm_Am"));
        REQUIRE(groove.has_value());
        CHECK(groove->format == "wav");
        CHECK(groove->sample_rate == 44100);
        CHECK(groove->duration_exact);
        CHECK(groove->bpm_milli == 124'000);
        CHECK(groove->bpm_source == "filename");
        REQUIRE(groove->key.has_value());
        CHECK(format_key(*groove->key) == "Am");
        CHECK(test::sample_id_named(*fixture.library, "שלום") > 0);
    }

    TEST_CASE("a rescan keeps annotations through renames and reports changed and missing files") {
        ImportFixture fixture;
        fixture.import();
        auto& library = *fixture.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        catalog::EditCommand favorite;
        favorite.sample_ids = {kick};
        favorite.favorite = true;
        favorite.tags_add = {"Mine"};
        REQUIRE(library.apply_edit(favorite).has_value());

        fs::rename(fixture.source / "Drums/Kick 01.wav", fixture.source / "Drums/Kick Renamed.wav");
        fs::remove(fixture.source / path_from_utf8("FX/שלום.wav"));
        test::write_sine_wav(fixture.source / "Loops/Groove_124bpm_Am.wav", 2.0, 110);  // content changed in place

        const auto summary = fixture.import();
        CHECK(summary.progress.relinked == 1);
        CHECK(summary.progress.updated == 1);
        CHECK(summary.progress.added == 0);
        CHECK(summary.finish.marked_missing == 1);

        auto detail = library.sample_detail(kick);
        REQUIRE(detail.has_value());
        CHECK(detail->favorite);
        CHECK(detail->locations.front().relative_path == "Drums/Kick Renamed.wav");
        auto groove = library.sample_detail(test::sample_id_named(library, "Groove_124bpm_Am"));
        CHECK(groove->content_revision == 2);
        CHECK(groove->duration_us == 2'000'000);

        catalog::Library* scope[] = {&library};
        auto missing = search::SearchService().search(scope, {.text = "available:false"});
        REQUIRE(missing->rows.size() == 1);
        CHECK(missing->rows.front().availability == search::RowAvailability::Missing);
    }

    TEST_CASE("unchanged files are not probed again") {
        ImportFixture fixture;
        fixture.import();
        const auto second = fixture.import();
        CHECK(second.progress.files_committed == 3);
        CHECK(second.progress.files_probed == 0);
        CHECK(second.progress.added == 0);
    }

    TEST_CASE("one-file batches and a single probe thread give the same result") {
        ImportFixture fixture;
        files::ImportOptions options;
        options.batch_size = 1;
        options.probe_threads = 1;
        const auto summary = fixture.import(options);
        CHECK(summary.progress.added == 3);
        CHECK(summary.progress.files_committed == 3);
        CHECK(summary.progress.directories_scanned == 4);  // root, Drums, FX, Loops
        CHECK(fixture.library->sample_count().value_or(0) == 3);
    }

    TEST_CASE("a file that fails to probe is still catalogued by name and counted") {
        ImportFixture fixture;
        test::write_file(fixture.source / "Drums/Broken.wav", std::string_view("RIFF....WAVEnot really a wave file"));
        const auto summary = fixture.import();
        CHECK(summary.progress.probe_failures == 1);
        CHECK(summary.progress.added == 4);
        CHECK(test::sample_id_named(*fixture.library, "Broken") > 0);
    }

    TEST_CASE("probe limits from ImportOptions apply to every file") {
        ImportFixture fixture;
        files::ImportOptions options;
        options.probe_limits = media::ProbeLimits{40, std::chrono::milliseconds(2000)};
        fixture.import(options);
        auto groove = fixture.library->sample_detail(test::sample_id_named(*fixture.library, "Groove_124bpm_Am"));
        REQUIRE(groove.has_value());
        CHECK(groove->format == "wav");                // identified from the first 40 bytes
        CHECK_FALSE(groove->sample_rate.has_value());  // the fmt chunk lay beyond the budget
    }

    TEST_CASE("a cancelled import commits nothing new and the next scan completes") {
        ImportFixture fixture;
        std::stop_source stop;
        stop.request_stop();
        auto cancelled = files::import_root(*fixture.library, fixture.root_id, fixture.source, stop.get_token());
        REQUIRE_FALSE(cancelled.has_value());
        CHECK(cancelled.error().code == ErrorCode::Cancelled);
        CHECK(fixture.library->sample_count().value_or(-1) == 0);
        const auto resumed = fixture.import();
        CHECK(resumed.progress.added == 3);
    }

    TEST_CASE("cancelling mid-scan keeps committed work, reports a final cancelled progress, and resumes") {
        ImportFixture fixture;
        for (int index = 0; index < 12; ++index)
            test::write_sine_wav(
                fixture.source / ("Bulk/Part " + std::to_string(index % 3) + "/Tone " + std::to_string(index) + ".wav"),
                0.05, 440);
        std::stop_source stop;
        files::ImportProgress last;
        files::ImportOptions options;
        options.batch_size = 1;
        // The first report follows the first committed one-file batch.
        const files::ImportProgressCallback cancel_after_first = [&](const files::ImportProgress& progress) {
            last = progress;
            if (progress.files_committed >= 1) stop.request_stop();
        };
        auto cancelled = files::import_root(*fixture.library, fixture.root_id, fixture.source, stop.get_token(),
                                            cancel_after_first, options);
        REQUIRE_FALSE(cancelled.has_value());
        CHECK(cancelled.error().code == ErrorCode::Cancelled);
        CHECK(last.finished);
        CHECK(last.cancelled);
        const auto committed = fixture.library->sample_count().value_or(-1);
        CHECK(committed >= 1);
        CHECK(committed < 15);

        const auto resumed = fixture.import(options);
        CHECK(resumed.finish.complete_coverage);
        CHECK(fixture.library->sample_count().value_or(-1) == 15);  // nothing lost, nothing duplicated
    }

    TEST_CASE("a catalog error mid-import fails the scan cleanly while probes are in flight") {
        ImportFixture fixture;
        for (int index = 0; index < 8; ++index)
            test::write_sine_wav(fixture.source / ("Extra/Dir " + std::to_string(index) + "/Tone.wav"), 0.05, 330);
        sqlite3* connection = nullptr;
        REQUIRE(sqlite3_open_v2(path_to_utf8(fixture.scratch / "lib/catalog.sqlite").c_str(), &connection,
                                SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
        sqlite3_busy_timeout(connection, 10'000);
        bool broken = false;
        files::ImportOptions options;
        options.batch_size = 1;
        const files::ImportProgressCallback break_catalog = [&](const files::ImportProgress& progress) {
            // After the first committed batch, make the next stored-stats read fail.
            if (broken || progress.files_committed < 1) return;
            broken = sqlite3_exec(connection, "ALTER TABLE locations RENAME TO locations_moved", nullptr, nullptr,
                                  nullptr) == SQLITE_OK;
        };
        auto result = files::import_root(*fixture.library, fixture.root_id, fixture.source, {}, break_catalog, options);
        CHECK(broken);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code != ErrorCode::Cancelled);
        sqlite3_close(connection);
    }

    TEST_CASE("an unlistable directory becomes a gap: its files are not marked missing" *
              doctest::skip(listing_restriction_unavailable())) {
        ImportFixture fixture;
        fixture.import();
        ListingDenied denied(fixture.source / "Loops");
        REQUIRE(denied.applied());
        const auto summary = fixture.import();
        CHECK(summary.progress.inaccessible_directories == 1);
        CHECK_FALSE(summary.finish.complete_coverage);
        CHECK(summary.finish.marked_missing == 0);
        CHECK(fixture.library->sample_count().value_or(0) == 3);
    }

    TEST_CASE("links are skipped by default, followed on request, and link cycles end the descent") {
        ImportFixture fixture;
        test::write_sine_wav(fixture.scratch / "outside/Outside Tone.wav", 0.1, 500);
        DirectoryLink outside(fixture.source / "Linked", fixture.scratch / "outside");
        REQUIRE(outside.created());
        DirectoryLink cycle(fixture.source / "Loops/Back", fixture.source);
        REQUIRE(cycle.created());
        CHECK(files::detail::classify_link(fixture.source / "Linked") != files::detail::LinkKind::None);
        CHECK(files::detail::classify_link(fixture.source / "Drums") == files::detail::LinkKind::None);

        const auto skipped = fixture.import();
        CHECK(skipped.progress.links_skipped == 2);
        CHECK(skipped.progress.added == 3);

        files::ImportOptions follow;
        follow.follow_reparse_points = true;
        const auto followed = fixture.import(follow);
        CHECK(followed.progress.links_skipped == 0);
        CHECK(followed.progress.link_cycles_skipped == 1);
        CHECK(followed.progress.added == 1);  // Linked/Outside Tone.wav
        CHECK(test::sample_id_named(*fixture.library, "Outside Tone") > 0);
    }

    TEST_CASE("audio files whose path the catalog cannot store are skipped and counted") {
        ImportFixture fixture;
#ifdef _WIN32
        // "CON" is a reserved device name; only an extended-length path can create such a file.
        const std::wstring reserved =
            LR"(\\?\)" + fs::absolute(fixture.source / "Drums").make_preferred().native() + LR"(\CON.wav)";
        const HANDLE created = CreateFileW(reserved.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        REQUIRE(created != INVALID_HANDLE_VALUE);
        CloseHandle(created);
        struct RemoveReserved {
            const std::wstring& path;
            ~RemoveReserved() { DeleteFileW(path.c_str()); }
        } remove_reserved{reserved};
#else
        test::write_file(fixture.source / std::string("Drums/bad\xFF.wav"), std::string_view("not decoded"));
#endif
        const auto summary = fixture.import();
        CHECK(summary.progress.unsafe_paths_skipped == 1);
        CHECK(summary.progress.added == 3);
    }

    TEST_CASE("read_file_identity reports size and time and keeps its id across a rename") {
        ScratchDirectory scratch("import");
        const fs::path file = scratch / "a.wav";
        test::write_sine_wav(file, 0.1);
        auto identity = files::detail::read_file_identity(file);
        REQUIRE(identity.has_value());
        CHECK(identity->byte_size == static_cast<std::int64_t>(fs::file_size(file)));
        const auto expected_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::file_clock::to_sys(fs::last_write_time(file)).time_since_epoch())
                                     .count();
        CHECK(std::llabs(identity->last_write_time_ns - expected_ns) < 1000);
        REQUIRE(identity->volume_id.has_value());
        CHECK(identity->volume_id->size() == 16);
        REQUIRE(identity->file_id.has_value());

        fs::rename(file, scratch / "b.wav");
        auto renamed = files::detail::read_file_identity(scratch / "b.wav");
        REQUIRE(renamed.has_value());
        CHECK(renamed->file_id == identity->file_id);
        CHECK(renamed->volume_id == identity->volume_id);

        test::write_sine_wav(scratch / "c.wav", 0.1);
        auto other = files::detail::read_file_identity(scratch / "c.wav");
        REQUIRE(other.has_value());
        CHECK(other->file_id != identity->file_id);

        CHECK(files::detail::read_file_identity(scratch / "missing.wav").error().code == ErrorCode::FileMissing);
        auto directory = files::detail::read_file_identity(scratch.path());
        REQUIRE(directory.has_value());
        CHECK(directory->file_id.has_value());
        CHECK(files::detail::classify_link(scratch / "b.wav") == files::detail::LinkKind::None);
    }

    TEST_CASE("an import leaves source bytes and modification times untouched (invariant 11)") {
        ImportFixture fixture;
        const std::vector<fs::path> files = {fixture.source / "Drums/Kick 01.wav",
                                             fixture.source / "Loops/Groove_124bpm_Am.wav",
                                             fixture.source / path_from_utf8("FX/שלום.wav")};
        std::vector<FileSnapshot> before;
        for (const fs::path& file : files)
            before.push_back(snapshot(file));
        fixture.import();
        for (std::size_t index = 0; index < files.size(); ++index) {
            CAPTURE(path_to_utf8(files[index]));
            CHECK(snapshot(files[index]) == before[index]);
        }
    }

    TEST_CASE("an unreachable root is reported offline and keeps its samples") {
        ImportFixture fixture;
        fixture.import();
        auto offline = files::import_root(*fixture.library, fixture.root_id, fixture.scratch / "gone", {});
        REQUIRE_FALSE(offline.has_value());
        CHECK(offline.error().code == ErrorCode::SourceOffline);
        CHECK(fixture.library->sample_count().value_or(0) == 3);
        auto roots = fixture.library->list_roots();
        CHECK(roots->front().availability == catalog::Availability::Offline);
    }

    TEST_CASE("a directory link skipped on a rescan keeps the samples imported through it") {
        ImportFixture fixture;
        test::write_sine_wav(fixture.scratch / "outside/Outside Tone.wav", 0.1, 500);
        DirectoryLink outside(fixture.source / "Linked", fixture.scratch / "outside");
        REQUIRE(outside.created());
        files::ImportOptions follow;
        follow.follow_reparse_points = true;
        CHECK(fixture.import(follow).progress.added == 4);

        const auto rescan = fixture.import();  // links are not followed now
        CHECK(rescan.progress.links_skipped == 1);
        CHECK(rescan.finish.marked_missing == 0);
        CHECK_FALSE(rescan.finish.complete_coverage);  // the link's subtree was not looked at
        CHECK(fixture.missing_count() == 0);
        CHECK(fixture.library->sample_count().value_or(0) == 4);
    }

    TEST_CASE("a file skipped on a rescan (cloud-only, or a link) is not marked missing") {
        ImportFixture fixture;
        fixture.import();
        const fs::path kick = fixture.source / "Drums/Kick 01.wav";
#ifdef _WIN32
        // OneDrive "Free up space" leaves the entry with its data elsewhere; FILE_ATTRIBUTE_OFFLINE
        // is what such a file reports without a reparse point.
        const DWORD attributes = GetFileAttributesW(kick.c_str());
        REQUIRE(attributes != INVALID_FILE_ATTRIBUTES);
        REQUIRE(SetFileAttributesW(kick.c_str(), attributes | FILE_ATTRIBUTE_OFFLINE));
        struct RestoreAttributes {
            const fs::path& file;
            DWORD attributes;
            ~RestoreAttributes() { SetFileAttributesW(file.c_str(), attributes); }
        } restore{kick, attributes};
        CHECK(files::detail::classify_link(kick) == files::detail::LinkKind::CloudOnly);
#else
        // The file moves away and a link takes its place: links are skipped by default.
        fs::rename(kick, fixture.scratch / "Kick 01.wav");
        fs::create_symlink(fixture.scratch / "Kick 01.wav", kick);
#endif
        const auto rescan = fixture.import();
#ifdef _WIN32
        CHECK(rescan.progress.cloud_only_skipped == 1);
#else
        CHECK(rescan.progress.links_skipped == 1);
#endif
        CHECK(rescan.finish.marked_missing == 0);
        CHECK(rescan.finish.complete_coverage);  // a skipped file is seen, not a gap
        CHECK(fixture.missing_count() == 0);
    }

    TEST_CASE("two links to one folder import it once") {
        ImportFixture fixture;
        test::write_sine_wav(fixture.scratch / "outside/Outside Tone.wav", 0.1, 500);
        DirectoryLink first(fixture.source / "Linked", fixture.scratch / "outside");
        DirectoryLink second(fixture.source / "Linked Again", fixture.scratch / "outside");
        REQUIRE(first.created());
        REQUIRE(second.created());
        files::ImportOptions follow;
        follow.follow_reparse_points = true;
        const auto summary = fixture.import(follow);
        CHECK(summary.progress.added == 4);
        CHECK(summary.progress.link_cycles_skipped == 1);
        CHECK(fixture.library->sample_count().value_or(0) == 4);
    }

    TEST_CASE("a rescan of a flat folder larger than a batch probes nothing again") {
        ImportFixture fixture;
        for (int index = 0; index < 40; ++index)
            test::write_sine_wav(fixture.source / ("Flat/Hit " + std::to_string(index) + ".wav"), 0.02, 300);
        files::ImportOptions options;
        options.batch_size = 7;  // stored stats must survive across batches of one directory
        CHECK(fixture.import(options).progress.added == 43);
        const auto rescan = fixture.import(options);
        CHECK(rescan.progress.files_committed == 43);
        CHECK(rescan.progress.files_probed == 0);
        CHECK(rescan.progress.updated == 0);
        CHECK(rescan.finish.marked_missing == 0);
    }

    TEST_CASE("files under paths longer than MAX_PATH are imported, and the depth limit leaves a gap") {
        ImportFixture fixture;
        // 30 levels of 12 characters: about 400 characters below the scratch folder.
        fs::path long_directory = fixture.source / "Long";
        for (int level = 0; level < 30; ++level)
            long_directory /= "level-" + std::to_string(100000 + level);
        // Past the depth limit: 260 nested single-letter folders.
        fs::path deep_directory = fixture.source / "Deep";
        for (int level = 0; level < 260; ++level)
            deep_directory /= "d";
        std::error_code error;
        fs::create_directories(media::native_path(long_directory), error);
        REQUIRE_FALSE(error);
        fs::create_directories(media::native_path(deep_directory), error);
        REQUIRE_FALSE(error);
        // The test helpers write through short paths; the files are then moved into place.
        test::write_sine_wav(fixture.scratch / "long.wav", 0.1, 440);
        test::write_sine_wav(fixture.scratch / "deep.wav", 0.1, 440);
        fs::rename(media::native_path(fixture.scratch / "long.wav"),
                   media::native_path(long_directory / "Long Tone.wav"));
        fs::rename(media::native_path(fixture.scratch / "deep.wav"),
                   media::native_path(deep_directory / "Deep Tone.wav"));
        REQUIRE(path_to_utf8(long_directory / "Long Tone.wav").size() > 300);

        const auto summary = fixture.import();
        CHECK(summary.progress.added == 4);  // the three fixture files and Long Tone
        CHECK(summary.progress.probe_failures == 0);
        CHECK(summary.progress.inaccessible_directories == 1);  // the first folder below the limit
        CHECK_FALSE(summary.finish.complete_coverage);
        auto long_tone = fixture.library->sample_detail(test::sample_id_named(*fixture.library, "Long Tone"));
        REQUIRE(long_tone.has_value());
        CHECK(long_tone->format == "wav");  // probed through the extended-length path
        CHECK(long_tone->duration_exact);

        // ScratchDirectory removes the rest; these trees need extended-length paths.
        fs::remove_all(media::native_path(fixture.source / "Long"), error);
        fs::remove_all(media::native_path(fixture.source / "Deep"), error);
    }

#ifdef _WIN32
    TEST_CASE("a file name with an unpaired surrogate is skipped as a path the catalog cannot store") {
        ImportFixture fixture;
        // U+D800 alone is a valid NTFS name but not valid UTF-16, so it has no UTF-8 spelling.
        const std::wstring lone_surrogate =
            LR"(\\?\)" + fs::absolute(fixture.source / "Drums").make_preferred().native() + L"\\\xD800 tone.wav";
        const HANDLE created = CreateFileW(lone_surrogate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        REQUIRE(created != INVALID_HANDLE_VALUE);
        CloseHandle(created);
        struct RemoveFile {
            const std::wstring& path;
            ~RemoveFile() { DeleteFileW(path.c_str()); }
        } remove_file{lone_surrogate};
        const auto summary = fixture.import();
        CHECK(summary.progress.unsafe_paths_skipped == 1);
        CHECK(summary.progress.added == 3);
    }
#endif

}  // TEST_SUITE("import")
