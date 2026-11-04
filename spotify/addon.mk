# Spotify: the C half of this addon, and the Rust engine it spawns.
#
# The addon (addon.c) is an ordinary shared library. The protocol half is a
# program of its own -- engine-mips/, librespot + tokio + rustls built as a
# static mipsel-musl binary -- which addon.c starts with fork()/execv() and
# speaks to over a unix socket. `docs/ADDONS.md` has the why; the short version
# is that the device's glibc is 2.16 and nothing on this side can link a modern
# Rust library, while a static binary has no libc to ask.
#
# What this file does, and what it deliberately does not:
#
#   * `make addons` copies the already-built device binary into
#     spotify/assets/, which is the one place addons/Makefile's pack rule looks
#     for a second file. It does NOT build it: that needs docker and this
#     addon's own image (docker/ in engine-mips/: the musl cross compiler and the
#     pinned Rust nightly), and `make addons` runs on machines that have neither.
#     To build the engine:
#
#         make -C addons/spotify/engine-mips        (builds its image first)
#
#     With no binary there, the addon is built without one and says "engine
#     not built" on the screen. Nothing fails, and no stale binary is left in
#     the folder for the packer to pick up.
#
#   * `make addons-host` builds the same crate for this machine (cargo without
#     the MIPS flags) and puts the binary where the simulator's addon folder
#     expects it. The host engine is the same program with the same protocol;
#     only the librespot session needs an account to say anything interesting.
#
#   * `make -C addons spotify-engine-test` runs the engine's unit tests.
#
# Nothing here links anything new into the addon: libc's pthread and the socket
# calls come with ADDON_LIBS and the player's own libc, and the engine is a file
# on the card, not a library.
spotify_ENGINE_DIR := $(CURDIR)/spotify/engine-mips
spotify_ENGINE_MIPS := $(spotify_ENGINE_DIR)/target/mipsel-unknown-linux-musl/release/spotify-engine-mips
spotify_ENGINE_HOST_BIN := $(spotify_ENGINE_DIR)/target/release/spotify-engine-mips
spotify_ENGINE_ASSET := $(CURDIR)/spotify/assets/spotify-engine-mips
spotify_ENGINE_HOST := $(BUILD_DIR)/host/spotify-engine-host
spotify_ENGINE_CARGO ?= $(shell command -v cargo 2>/dev/null)

# Where the addon keeps nothing of the build's: the header of addon.c.
spotify_CFLAGS = -I$(CURDIR)/spotify

# The two libraries that are not made by a C compiler. Written as prerequisite
# lines rather than by touching ADDON_RULE -- make merges them into the recipe
# rule wherever it is declared -- and order-only, because ADDON_RULE links `$^`
# and a phony prerequisite in there would be handed to the linker as an input.
$(BUILD_DIR)/target/spotify.so: | spotify-engine-pack
$(BUILD_DIR)/host/spotify.so: | spotify-engine-host

.PHONY: spotify-engine spotify-engine-pack spotify-engine-host spotify-engine-test

# Both halves, for a hand-run `make -C addons spotify-engine`.
spotify-engine: spotify-engine-pack spotify-engine-host

# The device binary, into the folder the packer copies onto the card. Absent
# source, absent file: a card that used to carry an engine and no longer has one
# must say "engine not built", not run yesterday's.
spotify-engine-pack:
	@if [ -f "$(spotify_ENGINE_MIPS)" ]; then \
		mkdir -p "$(dir $(spotify_ENGINE_ASSET))"; \
		cp "$(spotify_ENGINE_MIPS)" "$(spotify_ENGINE_ASSET)"; \
		chmod 0755 "$(spotify_ENGINE_ASSET)"; \
		echo "spotify: engine [device]: $(spotify_ENGINE_ASSET) ($$(stat -c%s "$(spotify_ENGINE_ASSET)") bytes)"; \
	else \
		rm -f "$(spotify_ENGINE_ASSET)"; \
		echo "spotify: engine not built for the device: $(spotify_ENGINE_MIPS) is not there"; \
		echo "spotify:   build it with: make -C addons/spotify/engine-mips   (docker; its own image)"; \
		echo "spotify:   the addon is built without it and says \"engine not built\" on the screen"; \
	fi

# The same crate for this machine. `--offline` first: a machine that has built
# it once has every crate in the registry cache, and a machine with no network
# should not wait for a fetch to time out.
spotify-engine-host:
	@if [ -z "$(spotify_ENGINE_CARGO)" ]; then \
		echo "spotify: engine not built for the simulator: cargo is not installed (the addon shows \"engine not built\")"; \
		rm -f "$(spotify_ENGINE_HOST)"; \
		exit 0; \
	fi; \
	mkdir -p "$(dir $(spotify_ENGINE_HOST))"; \
	if ! (cd "$(spotify_ENGINE_DIR)" && $(spotify_ENGINE_CARGO) build --release --offline >/dev/null 2>&1); then \
		(cd "$(spotify_ENGINE_DIR)" && $(spotify_ENGINE_CARGO) build --release --quiet) \
			|| { echo "spotify: engine not built for the simulator: cargo build failed (the addon shows \"engine not built\")"; \
				rm -f "$(spotify_ENGINE_HOST)"; exit 0; }; \
	fi; \
	cp "$(spotify_ENGINE_HOST_BIN)" "$(spotify_ENGINE_HOST)"; \
	chmod 0755 "$(spotify_ENGINE_HOST)"; \
	echo "spotify: engine [host]: $(spotify_ENGINE_HOST)"

# The engine's own tests: no network, no device, no account.
spotify-engine-test:
	@if [ -z "$(spotify_ENGINE_CARGO)" ]; then echo "spotify: no cargo, no tests"; exit 0; fi
	cd "$(spotify_ENGINE_DIR)" && $(spotify_ENGINE_CARGO) test
