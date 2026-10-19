#pragma once
/// @file
/// Relay of catalog::OpenOptions::on_progress to LibraryOpenProgressEvent (internal to the app module).
#include <filesystem>
#include <functional>

#include "mso/app/application.hpp"

namespace mso::app::detail {

/// A catalog::OpenOptions::on_progress callback that emits LibraryOpenProgressEvent through `emit`,
/// throttled to one event per Application::kOpenProgressInterval within a step; the first report of
/// every step and the report that completes it always go out. Call it from one thread (the opening
/// thread, as catalog::Library::open does).
std::function<void(const catalog::OpenProgress&)> open_progress_relay(std::function<void(const ApplicationEvent&)> emit,
                                                                      uuids::uuid library,
                                                                      std::filesystem::path directory);

}  // namespace mso::app::detail
