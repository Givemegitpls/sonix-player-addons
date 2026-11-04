# The environment the Spotify addon's Rust half is built in.
#
# The engine is a Rust crate compiled for a Tier 3 target, and nothing the player
# itself needs can produce it: the device's glibc is 2.16, so the binary is a
# static mipsel-musl one, which means a musl cross compiler, a nightly rustc with
# `rust-src`, and std compiled from source. This image carries exactly those
# three things and nothing of the player's toolchain -- the player's image
# (sonix-player/sonix-player/docker/Dockerfile, in the submodule) has no Rust in
# it, and the C half of this addon does not need any.
#
#   docker build -t tempotec-spotify-engine spotify/engine-mips/docker
#   make -C spotify/engine-mips            # builds that image, then the engine
#
# The base is pinned by digest and it is the same Ubuntu 24.04 the player's image
# uses, so an engine built today is the engine a clone builds later.
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3

ENV DEBIAN_FRONTEND=noninteractive

# What each one is for, so that none of them looks like an accident:
#
#   build-essential   `cc` for the crates that build C (ring's assembly and
#                     curve25519) and for `make test`, which compiles the unit
#                     tests for the machine running them rather than for MIPS
#   binutils          readelf, which `make check` runs on the binary
#   file              `file` on the result: static, MIPS32, no interpreter
#   git               vergen reads the commit the binary was built from
#   wget              rustup-init and the musl.cc tarball; the base is pinned but
#                     this keeps the image from needing curl as well
#   ca-certificates   https for all of the above
RUN apt-get update && apt-get install -y --no-install-recommends \
		build-essential binutils file git wget ca-certificates \
	&& rm -rf /var/lib/apt/lists/*

# ---------------------------------------------------------------------------
# Rust
# ---------------------------------------------------------------------------
#
# `mipsel-unknown-linux-musl` is a Tier 3 target: rustup has no prebuilt standard
# library for it, so the only way to a binary is to compile std from source for
# the target -- `-Z build-std`, nightly only, and it reads the source from the
# `rust-src` component. That is the entire reason rustup is here.
#
# The nightly is pinned by date rather than by channel: `nightly` moves every
# night, and an image built in a year should produce the binary this image
# produces. nightly-2026-10-01 is rustc 1.101.0-nightly (21b707e3f 2026-09-30)
# and does publish `rust-src`; moving the pin means checking that it still does,
# and re-running `make check` on the engine.
#
# rustup-init itself is pinned and fetched by wget, which the image already has,
# rather than through sh.rustup.rs: one fewer network hop and one fewer script
# that can change under a pinned base image.
#
# RUSTUP_HOME and CARGO_HOME are moved out of $HOME on purpose. The Makefile
# runs the build as the calling user with HOME=/tmp, so a toolchain installed
# into /root/.cargo would be invisible to it. They are made world-writable for
# the same reason: that user has to be able to write the registry cache, the
# download cache and rustup's own settings.
ENV RUSTUP_HOME=/opt/rustup \
	CARGO_HOME=/opt/cargo
RUN wget -q -t 5 -T 60 -O /tmp/rustup-init \
		https://static.rust-lang.org/rustup/archive/1.28.2/x86_64-unknown-linux-gnu/rustup-init \
	&& chmod +x /tmp/rustup-init \
	&& /tmp/rustup-init -y --no-modify-path --profile minimal \
		--default-toolchain nightly-2026-10-01 \
		--component rust-src \
	&& rm -f /tmp/rustup-init \
	&& chmod -R a+rwX "$RUSTUP_HOME" "$CARGO_HOME"
ENV PATH=/opt/cargo/bin:$PATH

# ---------------------------------------------------------------------------
# A C cross-compiler for mipsel-linux-musl
# ---------------------------------------------------------------------------
#
# Why it is here rather than "rustc can do MIPS on its own": rustc's LLVM does
# emit MIPS32r2, but the static link still needs a C library and the four crt
# objects, and for a distributed target those come from rustc's own
# `self-contained` directory -- a directory that does not exist for a Tier 3
# target that was just compiled with `-Z build-std`. musl.cc publishes a
# complete mipsel-linux-musl gcc (musl 1.2, built for i686 hosts and runnable on
# x86-64), unpacked where its tarball expects to be, so
# `-C linker=mipsel-linux-musl-gcc` links against its `libc.a` and `crt1.o`. It
# is also the `cc` for any C dependency the engine grows later, and the
# toolchain `cc`-using build scripts look for. `make client` builds the probe
# client with it too.
#
# The last line of that RUN is the one thing that is not the tarball's own
# layout. rustc adds `-lunwind` to every musl link unconditionally, on the
# assumption that the target is distributed and ships LLVM's libunwind in its
# `self-contained` directory -- which a Tier 3 target built here does not. The
# name is all that is missing: libgcc_eh.a is the same Itanium unwinder ABI
# (`_Unwind_RaiseException`, `_Unwind_Resume`, ...) compiled for the same
# target, and it is in the tarball already. So it is put where `-lunwind` looks,
# under the name rustc asks for. Nothing unwinds in the engine (panic = "abort",
# no backtrace printing), but the linker is not told that and the symbol must
# resolve.
#
# The download is resumed rather than restarted: the tarball is 97 MB, musl.cc
# is slow from a data centre, and a dropped connection in the middle of it is
# the way this layer used to fail (`wget: unable to resolve host address` or a
# plain reset, exit 4). `-c` continues a partial file, `-t 5` tries five times,
# and the size is checked because a resume that stopped early leaves a file that
# untars into nothing useful. The tarball is not checksummed: musl.cc publishes
# no manifest, and the file has not changed since 2021 (Last-Modified: Tue, 23
# Nov 2021); the size is the pin this download can have.
ENV MUSL_CROSS_SIZE=101686600
ENV PATH=/opt/mipsel-linux-musl-cross/bin:$PATH
RUN wget -c -q -t 5 -T 60 --waitretry=10 -O /tmp/musl-cross.tgz \
		https://musl.cc/mipsel-linux-musl-cross.tgz \
	&& [ "$(stat -c%s /tmp/musl-cross.tgz)" = "$MUSL_CROSS_SIZE" ] \
	&& tar -xzf /tmp/musl-cross.tgz -C /opt \
	&& rm -f /tmp/musl-cross.tgz \
	&& cp /opt/mipsel-linux-musl-cross/lib/gcc/mipsel-linux-musl/*/libgcc_eh.a \
		/opt/mipsel-linux-musl-cross/mipsel-linux-musl/lib/libunwind.a \
	&& mipsel-linux-musl-gcc --version | head -1

# Nothing is built into the image. The tree is mounted at its own absolute path
# and everything the build produces -- target/ -- stays in it, so the runs after
# the first only compile what changed.
WORKDIR /work
