#!/usr/bin/env bash
# Prepares the patched copy of the wgpu-hal crate that ALVR is built against, and points
# ALVR/Cargo.toml at it with a path relative to the ALVR folder (so it works on any machine).
#
# Why: ALVR's Rust graphics code would otherwise create its own EGL context that cannot share
# textures with the app's OpenGL code. The patch (patches/wgpu-hal-25.0.2-share-egl-context.patch)
# makes it share the app's context, and also turns off GL debug output, which crashes the PowerVR
# driver of some phones. See notes/MIGRATION_NOTES.md.
#
# Run from anywhere; called by prepare-alvr-deps.sh before it builds ALVR. Safe to run again.
set -euo pipefail
cd "$(dirname "$0")"

VERSION="25.0.2"
DIR="wgpu-hal-${VERSION}"
PATCH="patches/wgpu-hal-${VERSION}-share-egl-context.patch"
CARGO_TOML="ALVR/Cargo.toml"
# Relative to the folder holding ALVR/Cargo.toml, i.e. ALVR/, so ../ is this folder.
PATCH_LINE="wgpu-hal = { path = \"../${DIR}\" }"

[ -f "${CARGO_TOML}" ] || { echo "ERROR: ${CARGO_TOML} not found. Clone ALVR into ./ALVR first."; exit 1; }
[ -f "${PATCH}" ] || { echo "ERROR: ${PATCH} not found."; exit 1; }

# 1. Fresh copy of the crate from crates.io, then apply our patch.
rm -rf "${DIR}"
tmp="$(mktemp -d)"
curl -fsSL "https://static.crates.io/crates/wgpu-hal/${DIR}.crate" -o "${tmp}/crate.tar.gz"
tar -xzf "${tmp}/crate.tar.gz" -C .
rm -rf "${tmp}"
( cd "${DIR}" && patch -p1 --no-backup-if-mismatch < "../${PATCH}" )

# 2. Point ALVR at it (relative path), whatever state Cargo.toml is in.
if grep -q '^wgpu-hal *=' "${CARGO_TOML}"; then
    # Already has a wgpu-hal line (e.g. an old absolute path): replace it.
    sed -i "s|^wgpu-hal *=.*|${PATCH_LINE}|" "${CARGO_TOML}"
elif grep -q '^\[patch.crates-io\]' "${CARGO_TOML}"; then
    # Section exists without wgpu-hal: add the line under the header.
    sed -i "/^\[patch.crates-io\]/a ${PATCH_LINE}" "${CARGO_TOML}"
else
    printf '\n[patch.crates-io]\n%s\n' "${PATCH_LINE}" >> "${CARGO_TOML}"
fi

echo "wgpu-hal patched in ./${DIR}; ${CARGO_TOML} now has:"
grep -n -B1 '^wgpu-hal *=' "${CARGO_TOML}"
