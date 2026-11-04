#!/bin/sh
#
# Re-copies librespot-core from the cargo registry and applies
# vendor/librespot-core.patch.
#
# Why this exists: librespot ends the whole process for an account that is not
# Premium (Session::check_catalogue, "TODO: logout instead of exiting"), and in
# this engine the process *is* the session -- a free login would answer
# `signed_in` and then take the engine with it. The patch keeps the session, so
# a free account can browse the catalogue; playback is still refused by Spotify
# at the audio key. Upstream's own PR for the same change is
# librespot-org/librespot#1855.
#
# Run this after bumping the librespot-core version in Cargo.toml: the version is
# read from there, the crate is taken from the registry cache, and the patch is
# applied on top. The result is committed under vendor/librespot-core; the patch
# file is the part that a human reviews.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
version=$(sed -n 's/.*librespot-core = { version = "=\([0-9][0-9.]*\)".*/\1/p' "$here/Cargo.toml")
if [ -z "$version" ]; then
	echo "$0: no librespot-core version pinned in $here/Cargo.toml" >&2
	exit 1
fi

cache=${CARGO_HOME:-$HOME/.cargo}/registry/src
src=$(ls -d "$cache"/*/librespot-core-"$version" 2>/dev/null | head -1)
if [ -z "$src" ]; then
	echo "$0: librespot-core-$version is not in $cache; run 'cargo fetch' first" >&2
	exit 1
fi

rm -rf "$here/vendor/librespot-core"
cp -r "$src" "$here/vendor/librespot-core"
chmod -R u+w "$here/vendor/librespot-core"
# The checksum file describes the published crate, not this copy: leaving it
# behind makes a directory source complain about the edited file.
rm -f "$here/vendor/librespot-core/.cargo-checksum.json"
(cd "$here/vendor/librespot-core" && patch -p1 --quiet < "$here/vendor/librespot-core.patch")

echo "vendored librespot-core $version with the session patch"
