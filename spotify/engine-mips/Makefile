# The librespot half of the Spotify addon, built the way the player is built.
#
#   make            build the engine binary in the docker image
#   make client     build the unix-socket probe client (same cross compiler)
#   make check      file/readelf/size on the result: static, MIPS, no dynamic section
#   make probe      push both, start the engine on the device, ask it `probe`
#   make clean      remove target/
#
# Why these flags and not others is in README.md; the short version is that
# mipsel-unknown-linux-musl is a Tier 3 target, so std has to be compiled from
# source (-Z build-std, nightly, rust-src), and that the link needs the musl
# cross compiler from the image instead of rustc's own start files.
#
# The docker invocation mirrors sonix-player/docker/build.sh: the tree mounted
# at its own absolute path, the calling user, HOME=/tmp. Nothing is written
# outside the tree, and nothing in the tree ends up owned by root. The registry
# cache lives in the image's /opt/cargo and is lost with the container, so a
# fresh `docker run` downloads the crates again -- the compiled artifacts in
# target/ are what makes a rebuild cheap.
#
# Cargo.lock is committed and is part of the recipe: `vergen` is pinned to
# 9.0.6 there, because librespot-core 0.8.0's build script does not compile
# against vergen 9.1.0 + vergen-gitcl 1.0.8 (two vergen-lib majors in one graph,
# "the trait bound `&Build: CoerceUnsized<&dyn Add>` is not satisfied"). Do not
# run `cargo update` without re-running `make`.

IMAGE ?= tempotec-player-build
REPO := $(abspath $(CURDIR)/../../../..)

TOOLCHAIN := +nightly-2026-10-01
TARGET := mipsel-unknown-linux-musl
BIN := target/$(TARGET)/release/spotify-engine-mips
CLIENT := target/mipsel-linux-musl/probe-client

# -C target-feature=-soft-float is not optional: see "Why each piece" in
# README.md. Without it a f64 coming back from musl's libm arrives in the wrong
# registers and the result is silently wrong. It is also what makes ring's C
# half agree with musl.cc's libc about the o32 float ABI.
RUSTFLAGS_MIPS := -C linker=mipsel-linux-musl-gcc \
	-C link-self-contained=no \
	-C target-feature=-soft-float,+crt-static

CARGO := cargo $(TOOLCHAIN) build --release --target $(TARGET) -Z build-std=std,panic_abort

DOCKER_RUN := docker run --rm \
	-v "$(REPO):$(REPO)" \
	-w "$(CURDIR)" \
	--user "$$(id -u):$$(id -g)" \
	-e HOME=/tmp \
	-e RUSTFLAGS="$(RUSTFLAGS_MIPS)" \
	$(IMAGE)

# The same container without the MIPS flags: the unit tests are built for the
# machine that runs them (x86-64), where the linker is the ordinary one.
DOCKER_RUN_HOST := docker run --rm \
	-v "$(REPO):$(REPO)" \
	-w "$(CURDIR)" \
	--user "$$(id -u):$$(id -g)" \
	-e HOME=/tmp \
	$(IMAGE)

.PHONY: all build client check probe test clean

all: build

build:
	@command -v docker >/dev/null || { echo "engine-mips: docker is what builds this" >&2; exit 1; }
	$(DOCKER_RUN) $(CARGO)

# The unit tests of the parts that have no account in them: the INI parser, the
# content-range parser, the chunked body, and the decrypting side of the audio
# pipe against a locally encrypted stand-in. No network, no device.
test:
	@command -v docker >/dev/null || { echo "engine-mips: docker is what builds this" >&2; exit 1; }
	$(DOCKER_RUN_HOST) cargo $(TOOLCHAIN) test

client:
	@mkdir -p $(dir $(CLIENT))
	$(DOCKER_RUN) mipsel-linux-musl-gcc -Os -static -o $(CLIENT) tools/probe-client.c
	@file $(CLIENT)

check: build
	@file $(BIN)
	@ls -l $(BIN)
	@echo "--- readelf -d (want: no dynamic section) ---"
	@readelf -d $(BIN)
	@echo "--- readelf -A (want: MIPS32r2, hard float, CPR1 size 32) ---"
	@readelf -A $(BIN) | sed -n '1,10p'

probe: build client
	tools/probe.sh

clean:
	rm -rf target
