#!/bin/sh
#
# Builds and runs the store's host checks.
#
#   addonstore/tools/run_store_test.sh /path/to/dino.zip
#   addonstore/tools/run_store_test.sh /path/to/dino.zip /path/to/releases.json
#
# The first form uses a canned release list and the real zip; the second parses
# a response captured from api.github.com as well. Nothing is downloaded and
# nothing from the player is built: the network is stubbed
# (tools/stub_net.c) and only the player's json, sha256 and cardpace are
# compiled in.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
player=$(cd "$here/../sonix-player/sonix-player" && pwd)
out=${TMPDIR:-/tmp}/addonstore-test
zip=${1:?usage: $0 ZIP [RELEASES_JSON]}
json=${2:-}

cd "$here"
cc -O1 -Wall -I. -I"$player" -I.. -o "$out" \
	tools/test_store.c asdata.c asnet.c asinst.c aszip.c tools/stub_net.c \
	../common/cardpace.c ../common/gunzip.c \
	"$player/src/system/core/json.c" "$player/src/system/core/sha256.c"

# A second build of the same release: same name inside, addon.so and README.txt
# with different bytes. That is what an update has to replace, while the
# manifest and everything else in the folder stays.
v2=${TMPDIR:-/tmp}/addonstore-v2
rm -rf "$v2"
mkdir -p "$v2"
python3 - "$zip" "$v2/dino.zip" <<'PY_PY'
import sys, zipfile

src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        data = zin.read(item.filename)
        if item.filename == 'addon.so':
            data += b'V2'
        elif item.filename == 'README.txt':
            data += b'\nv2\n'
        zout.writestr(item, data)
    # A release that ships its own settings.ini, and one that brings a file the
    # card does not have: the first must not overwrite, the second must arrive.
    zout.writestr('settings.ini', b'server = from-release\n')
    zout.writestr('newfile.txt', b'added by the release\n')
PY_PY

# Two releases that must never be installed: a zip whose addon.so is not an
# ELF for this board, and one whose manifest asks for another ABI.
python3 - "$zip" "$v2/dino.zip" "$v2/bad-so.zip" "$v2/bad-api.zip" <<'PY_PY'
import sys, zipfile

src, ok, bad_so, bad_api = sys.argv[1:5]
with zipfile.ZipFile(src) as zin:
    entries = [(item, zin.read(item.filename)) for item in zin.infolist()]

def write(path, mutate):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as out:
        for item, data in entries:
            out.writestr(item, mutate(item.filename, data))

write(bad_so, lambda name, data: b'not an elf at all' if name == 'addon.so' else data)
write(bad_api, lambda name, data: b'{"api": 2, "title": "X", "version": "9"}' if name == 'addon.json' else data)
PY_PY

"$out" "$out.card" "$zip" "$v2/dino.zip" "$v2/bad-so.zip" "$v2/bad-api.zip"

if [ -n "$json" ]; then
	printf '\n-- a captured release list --\n'
	AS_TEST_JSON=$json "$out" "$out.card-json" "$zip"
fi
