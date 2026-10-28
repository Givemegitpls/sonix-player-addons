#!/bin/sh
#
# Regenerates addons/exports.base: the symbols the player publishes for addons,
# whether or not an addon in this tree happens to call them.
#
# Why a checked-in list and not a rule the build runs: the player's link needs
# the list, and the list is derived from the player's objects, so generating it
# during the build would be a cycle. This script is the deliberate step -- run it
# after `make target` when the interface modules change, look at the size, and
# commit the result. docs/ADDONS.md, "What an addon may call", says what it may
# and may not contain.
#
# The modules are the ones an addon is documented to use, plus LVGL's public
# widgets: an addon draws its page with lv_obj_create and friends, and a list
# that left those out would only move the problem to the first addon that needed
# a widget the last one did not.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
player=$(cd "$here/.." && pwd)
build="$player/build_target"
out="$here/exports.base"

NM=$(command -v mipsel-rockbox-linux-gnu-nm 2>/dev/null || true)
if [ -z "$NM" ]; then
	NM="$player/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-nm"
fi
if [ ! -x "$NM" ]; then
	echo "$0: no mipsel nm found; set up the toolchain or PATH" >&2
	exit 1
fi
if [ ! -d "$build" ]; then
	echo "$0: $build does not exist -- run 'make target' first" >&2
	exit 1
fi

player_objects="
	$build/gui/shell
	$build/gui/fonts
	$build/system/core
	$build/system/net
	$build/system/image
	$build/system/streaming
	$build/system/audio
	$build/system/library
	$build/system/playback
	$build/system/device
"

lvgl_objects="
	$build/lvgl/src/core
	$build/lvgl/src/widgets
	$build/lvgl/src/misc
	$build/lvgl/src/display
	$build/lvgl/src/indev
	$build/lvgl/src/draw
	$build/lvgl/src/layouts
	$build/lvgl/src/font
	$build/lvgl/src/stdlib
	$build/lvgl/src/tick
"

files=""
for dir in $player_objects $lvgl_objects; do
	if [ -d "$dir" ]; then
		files="$files $(find "$dir" -name '*.o' | tr '\n' ' ')"
	fi
done

tmp="$out.tmp"
# shellcheck disable=SC2086
$NM -g --defined-only $files 2>/dev/null |
	awk 'NF == 3 && $3 !~ /^_/ && $2 ~ /^[TDBRWV]$/ { print $3 }' |
	sed 's/@.*//' |
	sort -u >"$tmp"

count=$(wc -l <"$tmp")
mv "$tmp" "$out"
echo "exports.base: $count symbols from $(echo $files | wc -w) objects"
echo "rebuild the player ('make target') so the new list takes effect"
