#!/bin/bash
#
# Builds the addons in docker, with nothing installed on the machine but docker.
#
#   sonix-player/addons/docker/build.sh              make addons
#   sonix-player/addons/docker/build.sh addons-pack  the folder that goes on a card
#
# There is no image of its own. An addon is compiled by the same cross toolchain
# as the player, against the same headers, and the player's image
# (sonix-player/docker/Dockerfile, Ubuntu 24.04 from the public registry) already
# carries it along with make and python3 -- everything `make addons` asks for.
# This script exists so that "build the addons" is one command rather than a
# remembered incantation, and it hands every argument to that build unchanged.
#
# The toolchain lives outside the repository when the tree has not built one
# itself; set ROCKBOX_TOOLCHAIN_DIR, exactly as the player's script takes it, and
# the directory is mounted into the container.
#
# `host` is not here: the SDL simulator's addons link against the machine's own
# libraries, so `make addons-host` on the machine is the way -- see
# docs/ADDONS.md.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
PLAYER_DIR="$(dirname "$(dirname "$HERE")")"

if [ $# -eq 0 ]; then
	set -- addons
fi

exec "$PLAYER_DIR/docker/build.sh" "$@"
