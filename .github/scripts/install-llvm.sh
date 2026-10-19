#!/usr/bin/env bash
# Installs clang, libc++ and libc++abi of one LLVM major version from apt.llvm.org on Ubuntu 24.04,
# adding the repository explicitly (no piped install script run as root). The signing key is checked
# against its pinned fingerprint before apt trusts it.
#
#   .github/scripts/install-llvm.sh <llvm-major-version>
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <llvm-major-version>" >&2
  exit 2
fi
llvm_version=$1
codename=noble
key_fingerprint=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421  # LLVM apt repository signing key
keyring=/etc/apt/keyrings/apt.llvm.org.gpg

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT
curl --fail --silent --show-error --location --retry 3 --output "$workdir/llvm.asc" https://apt.llvm.org/llvm-snapshot.gpg.key
actual=$(gpg --show-keys --with-colons "$workdir/llvm.asc" | awk -F: '$1 == "fpr" { print $10; exit }')
if [[ "$actual" != "$key_fingerprint" ]]; then
  echo "apt.llvm.org key fingerprint is $actual, expected $key_fingerprint" >&2
  exit 1
fi
sudo install -d -m 0755 /etc/apt/keyrings
gpg --dearmor < "$workdir/llvm.asc" | sudo tee "$keyring" > /dev/null
echo "deb [signed-by=$keyring] https://apt.llvm.org/$codename/ llvm-toolchain-$codename-$llvm_version main" \
  | sudo tee "/etc/apt/sources.list.d/llvm-$llvm_version.list" > /dev/null
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  "clang-$llvm_version" "lld-$llvm_version" "libc++-$llvm_version-dev" "libc++abi-$llvm_version-dev" \
  "libclang-rt-$llvm_version-dev"
"clang++-$llvm_version" --version
