#!/bin/sh
#
# A new addon, ready to compile.
#
#   ./new.sh <id> [streaming|more]
#
# Writes <id>/addon.json and <id>/<id>.c from the hello example,
# with the id and the tile's caption filled in, and says what to do next. The
# code it writes is the smallest addon there is: one screen, one row per host
# fact. Read docs/ADDONS.md before going further.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
id=${1:-}
section=${2:-more}

if [ -z "$id" ]; then
	echo "usage: $0 <id> [streaming|more]" >&2
	exit 2
fi

case "$id" in
*[!a-z0-9_]* | "")
	echo "$0: the id is the folder name and the tile's log line: lower case, digits, underscore" >&2
	exit 2
	;;
esac

case "$section" in
streaming | more) ;;
*)
	echo "$0: the section is streaming or more" >&2
	exit 2
	;;
esac

if [ -e "$here/$id" ]; then
	echo "$0: $id is already there" >&2
	exit 1
fi

mkdir -p "$here/$id"

# The caption: the id with a capital letter, which is what most of them are.
title=$(printf '%s' "$id" | cut -c1 | tr '[:lower:]' '[:upper:]')$(printf '%s' "$id" | cut -c2-)

cat >"$here/$id/addon.json" <<EOF
{
	"api": 1,
	"title": "$title",
	"section": "$section",
	"version": "0.1"
}
EOF

sed -e "s/\"Hello\"/\"$title\"/g" -e "s/hello/$id/g" "$here/hello/hello.c" >"$here/$id/$id.c"

cat <<EOF

$id/ written.

  1. the screen and the rows are in $id/$id.c
  2. make; make host             # target and simulator libraries
  3. make pack                    # build/Addons/ -- copy to <card>/Addons
  4. make checks                  # after the player is built: ABI and exports

The icon is optional: drop an icon.png next to addon.json and the tile uses it,
otherwise the addon gets the menu's "more" glyph.

Read docs/ADDONS.md for the interface, the thread rules and the traps.
EOF
