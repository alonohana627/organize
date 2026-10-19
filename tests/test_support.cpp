#include "test_support.hpp"

#include <doctest.h>

#include <cmath>
#include <fstream>
#include <numbers>
#include <random>

namespace mso::test {

ScratchDirectory::ScratchDirectory() {
    std::random_device random;
    path_ = std::filesystem::temp_directory_path() / std::format("mso-test-{:08x}{:08x}", random(), random());
    std::filesystem::create_directories(path_);
}

ScratchDirectory::~ScratchDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}

namespace {
void write_little_endian(std::ofstream& out, std::uint32_t value, int bytes) {
    for (int index = 0; index < bytes; ++index)
        out.put(static_cast<char>((value >> (8 * index)) & 0xFF));
}
}  // namespace

void write_sine_wav(const std::filesystem::path& file, double seconds, double frequency, int sample_rate) {
    std::filesystem::create_directories(file.parent_path());
    const auto frames = static_cast<std::uint32_t>(seconds * sample_rate);
    std::ofstream out(file, std::ios::binary);
    out.write("RIFF", 4);
    write_little_endian(out, 36 + frames * 2, 4);
    out.write("WAVEfmt ", 8);
    write_little_endian(out, 16, 4);
    write_little_endian(out, 1, 2);  // PCM
    write_little_endian(out, 1, 2);  // mono
    write_little_endian(out, static_cast<std::uint32_t>(sample_rate), 4);
    write_little_endian(out, static_cast<std::uint32_t>(sample_rate * 2), 4);
    write_little_endian(out, 2, 2);
    write_little_endian(out, 16, 2);
    out.write("data", 4);
    write_little_endian(out, frames * 2, 4);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const double value = 0.5 * std::sin(2 * std::numbers::pi * frequency * frame / sample_rate);
        write_little_endian(out, static_cast<std::uint16_t>(static_cast<std::int16_t>(value * 32767)), 2);
    }
}

catalog::ScannedFile synthetic_file(std::string relative_path, std::int64_t size, std::int64_t duration_us,
                                    std::string format) {
    catalog::ScannedFile file;
    file.relative_path = std::move(relative_path);
    file.byte_size = size;
    file.mtime_ns = 1'700'000'000'000'000'000;
    catalog::ObservedMetadata metadata;
    metadata.format = std::move(format);
    metadata.codec = "pcm_s16";
    metadata.sample_rate = 44100;
    metadata.channels = 2;
    metadata.bit_depth = 16;
    metadata.duration_us = duration_us;
    metadata.duration_exact = true;
    file.metadata = metadata;
    return file;
}

PopulatedLibrary make_library(const std::filesystem::path& directory, const std::vector<catalog::ScannedFile>& files,
                              std::string_view name) {
    PopulatedLibrary populated;
    auto library = catalog::Library::create(directory, name);
    REQUIRE(library.has_value());
    populated.library = std::move(*library);
    auto root = populated.library->add_root("Samples", catalog::RootMode::Referenced);
    REQUIRE(root.has_value());
    populated.root_id = root->id;
    auto session = populated.library->begin_scan(root->id);
    REQUIRE(session.has_value());
    for (std::size_t offset = 0; offset < files.size(); offset += 500) {
        const std::span<const catalog::ScannedFile> batch(files.data() + offset,
                                                          std::min<std::size_t>(500, files.size() - offset));
        auto committed =
            populated.library->commit_scan_batch(*session, batch, {}, [](std::string_view) { return false; });
        REQUIRE(committed.has_value());
    }
    REQUIRE(populated.library->finish_scan(*session, false).has_value());
    return populated;
}

std::int64_t sample_id_named(catalog::Library& library, std::string_view display_name) {
    auto reader = library.acquire_reader();
    REQUIRE(reader.has_value());
    auto statement = reader->connection().prepare("SELECT id FROM samples WHERE display_name=?1");
    REQUIRE(statement.has_value());
    statement->bind(1, display_name);
    auto has_row = statement->next_row();
    REQUIRE(has_row.has_value());
    REQUIRE(*has_row);
    return statement->column_int64(0);
}

}  // namespace mso::test
