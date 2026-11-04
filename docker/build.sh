#!/bin/bash
#
# Builds the addons in docker, with nothing installed on the machine but docker.
#
#   docker/build.sh                          make            (target libraries)
#   docker/build.sh ADDON_IDS="dino qobuz"   only those
#   docker/build.sh pack                     build/Addons/, the folder for a card
#   docker/build.sh checks                   ABI and export checks
#
# There is no image of its own. An addon is compiled by the player's cross
# toolchain, against the player's headers, and the player's image
# (sonix-player/sonix-player/docker/Dockerfile, in the submodule) carries that toolchain's
# sources, make and python3 -- everything `make` here asks for. The image is
# built from the submodule, so the same Dockerfile, the same Ubuntu pin and the
# same Rust nightly as the player's own builds.
#
# `host` is not here: the SDL simulator's addons link against the machine's own
# libraries (SDL2, FreeType, opusfile, wavpack, ALSA), which the image does not
# carry, so `make host` runs on the machine -- docs/ADDONS.md, "Building".
#
# The toolchain lives inside the submodule when it has been built there
# (sonix-player/sonix-player/rockbox-toolchain) and is built by this script on the first run:
# 20-60 minutes, once. A toolchain that already exists elsewhere is reused
# rather than rebuilt: set ROCKBOX_TOOLCHAIN_DIR and it is mounted read-only.
set -euo pipefail

IMAGE="${IMAGE:-tempotec-player-build}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO="$(dirname "$HERE")"
PLAYER_DIR="${PLAYER_DIR:-$REPO/sonix-player/sonix-player}"
TOOLCHAIN_TARGET="rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc"

[ -f "$PLAYER_DIR/src/addon/addon.h" ] || {
	echo "build.sh: $PLAYER_DIR is not there or is empty -- git submodule update --init" >&2
	exit 1
}

command -v docker >/dev/null 2>&1 || {
	echo "build.sh: docker is not installed, and this is what builds the addons" >&2
	exit 1
}

# Asked every run, not only when the image is missing: docker reuses the layers
# of an unchanged Dockerfile in a second, and an edit to it would otherwise be
# silently left out.
echo "build.sh: the image $IMAGE from $PLAYER_DIR/docker (cached layers)"
docker build -t "$IMAGE" "$PLAYER_DIR/docker"

RUN_EXTRA=()
MAKE_VARS=()
if [ -n "${ROCKBOX_TOOLCHAIN_DIR:-}" ]; then
	[ -x "$ROCKBOX_TOOLCHAIN_DIR/bin/mipsel-rockbox-linux-gnu-gcc" ] || {
		echo "build.sh: ROCKBOX_TOOLCHAIN_DIR=$ROCKBOX_TOOLCHAIN_DIR has no mipsel-rockbox-linux-gnu-gcc" >&2
		exit 1
	}
	RUN_EXTRA+=(-v "$ROCKBOX_TOOLCHAIN_DIR:$ROCKBOX_TOOLCHAIN_DIR:ro")
	MAKE_VARS+=(
		"CROSS_CC=$ROCKBOX_TOOLCHAIN_DIR/bin/mipsel-rockbox-linux-gnu-gcc"
		"CROSS_CXX=$ROCKBOX_TOOLCHAIN_DIR/bin/mipsel-rockbox-linux-gnu-g++"
		"CROSS_READELF=$ROCKBOX_TOOLCHAIN_DIR/bin/mipsel-rockbox-linux-gnu-readelf"
		"CROSS_STRIP=$ROCKBOX_TOOLCHAIN_DIR/bin/mipsel-rockbox-linux-gnu-strip"
	)
fi

# The same docker invocation for every step: the tree at its host path (the .o
# files carry paths, and the compiler's messages name them), as the calling
# user, with a HOME the image can write to.
run_docker() {
	docker run --rm \
		-v "$REPO:$REPO" \
		-w "$REPO" \
		--user "$(id -u):$(id -g)" \
		-e HOME=/tmp \
		"${RUN_EXTRA[@]}" \
		"$IMAGE" \
		"$@"
}

if [ -z "${ROCKBOX_TOOLCHAIN_DIR:-}" ] && [ ! -x "$PLAYER_DIR/$TOOLCHAIN_TARGET" ]; then
	echo "build.sh: the cross toolchain is not built yet; building it inside $PLAYER_DIR"
	echo "build.sh:   (20-60 minutes, once -- the player's rockboxdev.sh and its patches)"
	run_docker make -C "$PLAYER_DIR" "$TOOLCHAIN_TARGET"
fi

if [ $# -eq 0 ]; then
	set -- target
fi
run_docker make "${MAKE_VARS[@]}" "$@"
