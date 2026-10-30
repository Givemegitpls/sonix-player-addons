# Spotify, as a service addon with a protocol half of its own.
#
# The C side (addon.c) is an ordinary addon: the flags below only add the
# engine's header directory. The Rust side (engine/) is a cdylib the addon
# opens with dlopen() from the card, and this file is what builds it and puts
# it where the packer will find it:
#
#   <card>/Addons/spotify/assets/spotify_engine.so
#
# `assets/` on purpose: addons/Makefile's pack rule copies an addon's `*.png`,
# `*.txt`, `*.ini` and its whole `assets/` folder, and nothing else, so this is
# the one place a second `.so` can sit without that rule having to grow a case
# for one addon.
#
# The rule never breaks `make addons`. Three things can be true of the machine
# running it, and each has its answer:
#
#   no cargo            the addon is built without an engine; the line in the
#                       build output says so, and the addon's Engine row says
#                       "engine not built" on the device. Nothing fails.
#   cargo, no MIPS std  the same, with the reason named (`rustup target add`).
#   cargo and the std   still not built by default -- see below.
#
# The last one is not a bug. Rust's mipsel-unknown-linux-gnu standard library
# is built against glibc 2.17 and the device has 2.16, so a library linked from
# it asks for symbols that are not there: `make addons-checks` (check-abi)
# would catch it, and on the device it would be a dlopen() that fails with a
# version in the message. The engine has to be built against a target whose std
# is old enough -- a custom target JSON and `-Z build-std`, the way the rekkod
# tree's spotify-engine did it -- and that needs nightly. Until then the target
# build is opt-in, and this is the switch:
#
#   make addons SPOTIFY_ENGINE_ALLOW_TARGET=1
#
# The host build has none of that trouble and is what the simulator uses:
#
#   make addons-host                     builds build/host/spotify_engine.so
#   make -C addons spotify-engine-test   cargo test, on the machine
spotify_ENGINE_DIR := $(CURDIR)/spotify/engine
spotify_ENGINE_SO := $(CURDIR)/spotify/assets/spotify_engine.so
spotify_ENGINE_HOST_SO := $(BUILD_DIR)/host/spotify_engine.so
spotify_CARGO ?= $(firstword $(wildcard /usr/bin/cargo /usr/local/bin/cargo) $(shell command -v cargo 2>/dev/null))
spotify_ENGINE_TARGET ?= mipsel-unknown-linux-gnu
spotify_ENGINE_ALLOW_TARGET ?= 0

spotify_CFLAGS = -I$(CURDIR)/spotify
# dlopen()/dlsym() are libdl on this device's glibc 2.16. The player links it
# too, so the symbols would resolve from there, but an addon that names its own
# dependencies is the rule here (-lpthread -lm -lrt come with ADDON_LIBS for
# the same reason).
spotify_LIBS = -ldl

# The addon's own prerequisites: the two libraries that are not made by a C
# compiler. Written as prerequisite lines rather than by touching ADDON_RULE --
# make merges them into the recipe rule below, wherever it is declared -- and
# order-only, because ADDON_RULE links `$^`: a phony prerequisite in there
# would be handed to the linker as an input file.
$(BUILD_DIR)/target/spotify.so: | spotify-engine-target
$(BUILD_DIR)/host/spotify.so: | spotify-engine-host

.PHONY: spotify-engine spotify-engine-host spotify-engine-target spotify-engine-test

# Both halves, for a hand-run `make -C addons spotify-engine`.
spotify-engine: spotify-engine-host spotify-engine-target

spotify-engine-host:
	@if [ -z "$(spotify_CARGO)" ]; then \
		echo "spotify: engine not built for the host: cargo is not installed (the addon runs without it)"; \
		exit 0; \
	fi; \
	mkdir -p $(BUILD_DIR)/host; \
	cd $(spotify_ENGINE_DIR) && $(spotify_CARGO) build --release --offline --quiet \
		|| { echo "spotify: cargo build failed; the addon is built without an engine"; exit 0; }; \
	cp $(spotify_ENGINE_DIR)/target/release/libspotify_engine.so $(spotify_ENGINE_HOST_SO); \
	echo "spotify: engine [host]: $(spotify_ENGINE_HOST_SO)"

spotify-engine-target:
	@if [ -z "$(spotify_CARGO)" ]; then \
		echo "spotify: engine not built: cargo is not installed (the addon runs without it)"; \
		exit 0; \
	fi; \
	if [ "$(spotify_ENGINE_ALLOW_TARGET)" != "1" ]; then \
		echo "spotify: engine not built for the device: Rust's $(spotify_ENGINE_TARGET) std needs glibc 2.17 and the device has 2.16."; \
		echo "spotify:   the addon is built without it and says \"engine not built\" on the screen."; \
		echo "spotify:   see addons/spotify/engine/README.md (SPOTIFY_ENGINE_ALLOW_TARGET=1 overrides)"; \
		exit 0; \
	fi; \
	libdir=$$(rustc --print target-libdir --target $(spotify_ENGINE_TARGET) 2>/dev/null); \
	if [ ! -d "$$libdir" ]; then \
		echo "spotify: engine not built: no $(spotify_ENGINE_TARGET) standard library (rustup target add $(spotify_ENGINE_TARGET))"; \
		exit 0; \
	fi; \
	mkdir -p $(dir $(spotify_ENGINE_SO)); \
	cd $(spotify_ENGINE_DIR) && \
		CARGO_TARGET_MIPSEL_UNKNOWN_LINUX_GNU_LINKER=$(CROSS_CC) \
		$(spotify_CARGO) build --release --offline --target $(spotify_ENGINE_TARGET) \
		|| { echo "spotify: cargo build --target $(spotify_ENGINE_TARGET) failed; the addon is built without an engine"; exit 0; }; \
	cp $(spotify_ENGINE_DIR)/target/$(spotify_ENGINE_TARGET)/release/libspotify_engine.so $(spotify_ENGINE_SO); \
	echo "spotify: engine [target]: $(spotify_ENGINE_SO) (run make addons-checks: this one may ask for a newer glibc)"

# The engine's own tests: 27 of them, no network, no device. They are not part
# of `make addons` because they are Rust's business and not the card's.
spotify-engine-test:
	@if [ -z "$(spotify_CARGO)" ]; then echo "spotify: no cargo, no tests"; exit 0; fi
	cd $(spotify_ENGINE_DIR) && $(spotify_CARGO) test --offline
