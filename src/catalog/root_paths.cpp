#include "mso/catalog/root_paths.hpp"

#include <format>
#include <optional>

#include "mso/domain/core.hpp"

namespace mso::catalog {

namespace {

/// Most directory entries examined before falling back to the directory's own name.
constexpr int kMaximumEntriesExamined = 256;

/// The file name with its first ASCII letter's case flipped, or nullopt when it has no ASCII letter.
std::optional<std::filesystem::path> case_variant(const std::filesystem::path& file_name) {
    auto text = file_name.native();
    for (auto& character : text) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<std::filesystem::path::value_type>(character - 'a' + 'A');
            return std::filesystem::path(text);
        }
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<std::filesystem::path::value_type>(character - 'A' + 'a');
            return std::filesystem::path(text);
        }
    }
    return std::nullopt;
}

/// Decides from one existing entry: its case variant missing, or a different file, means the
/// directory distinguishes case; the variant resolving to the same file means it does not.
std::optional<bool> case_sensitivity_from(const std::filesystem::path& existing) {
    const auto variant_name = case_variant(existing.filename());
    if (!variant_name) return std::nullopt;
    const auto variant = existing.parent_path() / *variant_name;
    std::error_code error;
    const bool variant_exists = std::filesystem::exists(variant, error);
    if (error) return std::nullopt;
    if (!variant_exists) return true;
    const bool same_file = std::filesystem::equivalent(existing, variant, error);
    if (error) return std::nullopt;
    return !same_file;
}

}  // namespace

bool default_case_sensitivity() noexcept {
#if defined(_WIN32) || defined(__APPLE__)
    return false;
#else
    return true;
#endif
}

Result<bool> detect_case_sensitivity(const std::filesystem::path& directory) {
    std::error_code error;
    const auto status = std::filesystem::status(directory, error);
    if (status.type() == std::filesystem::file_type::not_found)
        return fail(ErrorCode::NotFound, std::format("{} does not exist", path_to_utf8(directory)));
    if (error)
        return fail(ErrorCode::AccessDenied,
                    std::format("cannot examine {}: {}", path_to_utf8(directory), error.message()));
    if (status.type() != std::filesystem::file_type::directory)
        return fail(ErrorCode::NotFound, std::format("{} is not a folder", path_to_utf8(directory)));

    std::filesystem::directory_iterator entries(directory, std::filesystem::directory_options::skip_permission_denied,
                                                error);
    int examined = 0;
    for (; !error && entries != std::filesystem::directory_iterator() && examined < kMaximumEntriesExamined;
         entries.increment(error), ++examined) {
        if (const auto decided = case_sensitivity_from(entries->path())) return *decided;
    }
    const auto absolute = std::filesystem::absolute(directory, error);
    if (!error && absolute.has_parent_path() && absolute.has_filename())
        if (const auto decided = case_sensitivity_from(absolute)) return *decided;
    return default_case_sensitivity();
}

}  // namespace mso::catalog
