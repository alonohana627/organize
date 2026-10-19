#pragma once
/// @file
/// Test-only access to a Library's private writer connection, for fault injection (simulating a
/// concurrent tool, corrupt rows or an injected failure). Production code never includes this: all
/// writes go through Library methods so the writer mutex, revision and outbox stay atomic.
#include <mutex>

#include "mso/catalog/library.hpp"

namespace mso::catalog {

/// Friend of Library used by tests/catalog_tests.cpp and tests/sqlite_tests.cpp.
struct LibraryTestAccess {
    /// The serialized writer connection (hold writer_mutex while using it from several threads).
    static sqlite::Connection& writer(Library& library) { return library.writer_; }
    /// The mutex that serializes writers.
    static std::mutex& writer_mutex(Library& library) { return library.writer_mutex_; }
};

}  // namespace mso::catalog
