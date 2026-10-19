#!/usr/bin/env bash
# Builds Qt's qtbase (Core, Gui, Widgets, Concurrent and the offscreen platform plugin; xcb too when its
# development packages are installed) with clang + libc++ and installs it into <prefix>.
#
#   .github/scripts/build-qt-libcxx.sh <prefix> [extra Qt CMake arguments...]
#   environment: LLVM_VERSION (default 23), QT_BUILD_JOBS (default: nproc)
#
# Why: Linux distributions and the Qt online installer ship Qt built against libstdc++. This project
# is built with libc++, whose types live in the inline namespace std::__1, so they never match
# libstdc++'s at link time. The Qt API is almost entirely Qt types, but QtConcurrent::run (used by the
# UI) instantiates QUnhandledException(std::exception_ptr) and QFutureInterfaceBase::reportException(
# const std::exception_ptr&), exported from libQt6Core: against a libstdc++ Qt those symbols are
# undefined at link time. The GUI therefore needs a Qt built with the same standard library. Used by
# CI (cached) and documented in docs/BUILDING.md for Linux developers.
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <install-prefix> [extra Qt CMake arguments...]" >&2
  exit 2
fi
prefix=$1
shift

qt_version=6.11.2
qt_sha256=5b2e00eccaf5a4d8c14134ffa0ea8dfd0a35ae1ffc7f8d87fa4305a1ed23cf22
qt_url="https://download.qt.io/official_releases/qt/${qt_version%.*}/${qt_version}/submodules/qtbase-everywhere-src-${qt_version}.tar.xz"
llvm_version=${LLVM_VERSION:-23}
jobs=${QT_BUILD_JOBS:-$(nproc)}

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

curl --fail --silent --show-error --location --retry 3 --max-time 1800 --output "$workdir/qtbase.tar.xz" "$qt_url"
echo "$qt_sha256  $workdir/qtbase.tar.xz" | sha256sum --check --strict -
tar -xf "$workdir/qtbase.tar.xz" -C "$workdir"

cmake -S "$workdir/qtbase-everywhere-src-$qt_version" -B "$workdir/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_C_COMPILER="clang-$llvm_version" \
  -DCMAKE_CXX_COMPILER="clang++-$llvm_version" \
  -DCMAKE_CXX_FLAGS="-stdlib=libc++" \
  -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++" \
  -DCMAKE_SHARED_LINKER_FLAGS="-stdlib=libc++" \
  -DQT_BUILD_EXAMPLES=OFF \
  -DQT_BUILD_TESTS=OFF \
  -DFEATURE_sql=OFF \
  -DFEATURE_testlib=OFF \
  -DFEATURE_printsupport=OFF \
  -DFEATURE_opengl=OFF \
  -DFEATURE_dbus=OFF \
  "$@"
cmake --build "$workdir/build" --parallel "$jobs"
cmake --install "$workdir/build"
echo "Qt $qt_version (clang $llvm_version + libc++) installed in $prefix"
