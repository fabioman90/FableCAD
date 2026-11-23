#!/bin/sh
set -eu

bridge_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
build_dir=${1:?"Usage: $0 BUILD_DIRECTORY"}
mkdir -p "$build_dir"
build_dir=$(cd -- "$build_dir" && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

version=0.6.8
archive="$scratch/acadrust.crate"
curl --fail --silent --show-error --location \
    "https://static.crates.io/crates/acadrust/acadrust-$version.crate" --output "$archive"
printf '%s  %s\n' eb201e11e1c75584e4deefc2b30afb83e2f34d16338733439d38664cb9599929 "$archive" |
    shasum -a 256 -c -
tar -xf "$archive" -C "$scratch"
crate="$scratch/acadrust-$version"
(
    cd "$crate"
    patch -p1 < "$bridge_dir/acadrust-$version.patch"
)
export CARGO_TARGET_DIR="$build_dir"
cargo --config "patch.crates-io.acadrust.path='$crate'" build --locked --release \
    --manifest-path "$bridge_dir/Cargo.toml"
