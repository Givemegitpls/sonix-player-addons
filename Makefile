# The addons on the card.
#
# Each directory here is one addon: <id>/addon.json says what it is and where
# its tile goes, <id>/*.c is its code, and `make` here produces <id>.so -- a
# MIPS32 shared library the player opens with dlopen() when the tile is tapped.
# The interface between the two halves is sonix-player/src/addon/addon.h.
#
# Run it through the player's Makefile, which hands over the toolchain:
#
#   make addons            # the target libraries, into build/target/
#   make addons host       # the same addons for the SDL simulator, build/host/
#   make addons pack       # build/Addons/, the folder to copy to a card
#
# The host build is what makes an addon testable without the device: the same
# sources, the same headers, the same dlopen(), on x86.
#
# EVERYTHING in `common/` is compiled into every addon. That is deliberate: an
# addon is a self-contained package on a card, and a shared libcommon.so would
# have to be found, versioned and kept in step with each of them. The pieces are
# small, -ffunction-sections and --gc-sections drop the ones a given addon does
# not use, and RTLD_LOCAL keeps two addons' copies from meeting.

PLAYER_DIR ?= $(abspath $(CURDIR)/..)
# Defaults so that `make` here works on its own, without the player's Makefile
# handing the toolchain over: the in-tree toolchain first, the one symlinked
# from outside the tree second.
CROSS_CC ?= $(firstword $(wildcard $(PLAYER_DIR)/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc \
			  $(PLAYER_DIR)/rockbox-toolchain/rockbox-dev/bin/mipsel-rockbox-linux-gnu-gcc))
CROSS_READELF ?= $(dir $(CROSS_CC))mipsel-rockbox-linux-gnu-readelf
CROSS_STRIP ?= $(dir $(CROSS_CC))mipsel-rockbox-linux-gnu-strip
LVGL_DIR ?= $(PLAYER_DIR)/lvgl
BOARD ?= tempotec_v3

BUILD_DIR = $(CURDIR)/build

# ---------------------------------------------------------------------------
# What the addons are compiled against
#
# The same board defines the player itself is built with (sonix-player/Makefile,
# "The board this binary is built for"): SC() in src/gui/shell/scale.h turns
# every size in the interface into panel pixels at compile time, so an addon that
# did not get BOARD would lay itself out for another screen.
# ---------------------------------------------------------------------------
ifeq ($(BOARD),tempotec_v3)
ADDON_BOARD_CFLAGS = -DSONIX_BOARD_TEMPOTEC_V3=1 -DSONIX_LAYOUT_SCALE=2 -DSONIX_ICON_SET_SMALL=1
else
ADDON_BOARD_CFLAGS = -DSONIX_LAYOUT_SCALE=3
endif

# -include glibc_compat.h, like every translation unit of the player: the
# toolchain's glibc is 2.27 and the device's is 2.16, and five single-precision
# maths functions need to be asked for by their old names or the library will
# not load. The file guards on __mips__, so the host build ignores it.
TARGET_CFLAGS = -O2 -g -Wall -MMD -MP -fPIC -march=mips32r2 -mno-madd4 \
				-ffunction-sections -fdata-sections \
				-D_FILE_OFFSET_BITS=64 -I$(CURDIR) -I$(PLAYER_DIR) -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE=1 \
				$(ADDON_BOARD_CFLAGS) -include $(PLAYER_DIR)/src/system/core/glibc_compat.h

HOST_CC ?= cc

HOST_CFLAGS = -O2 -g -Wall -MMD -MP -fPIC -DHOST_BUILD=1 \
			  $(shell pkg-config --cflags freetype2) \
			  -D_FILE_OFFSET_BITS=64 -I$(CURDIR) -I$(PLAYER_DIR) -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE=1 \
			  $(ADDON_BOARD_CFLAGS)

# Undefined symbols are the point: lv_obj_create, http_get and the rest are
# resolved by the player at dlopen() time. --gc-sections drops what an addon
# does not call, which is most of what common/ holds for the other ones.
#
# -lrt, -lpthread and -lm are named rather than taken from the player: an addon
# is a shared object and has to say which libraries it links, and clock_gettime
# is in librt on this device (glibc_compat.h explains why it is asked for by its
# old name, and the .symver only resolves against the library that exports it).
ADDON_LIBS = -lpthread -lm -lrt
TARGET_LDFLAGS = -shared -Wl,--gc-sections $(ADDON_LIBS)
HOST_LDFLAGS = -shared -Wl,--gc-sections $(ADDON_LIBS)

# ---------------------------------------------------------------------------
# The addons themselves
# ---------------------------------------------------------------------------
ADDONS := $(sort $(notdir $(patsubst %/addon.json,%,$(wildcard $(CURDIR)/*/addon.json))))
# hello is the example, not a product: it is built with the rest (it is the cheap
# compile that proves the SDK still compiles) but it is not put in the folder
# that goes on a card. `make addons-pack PACK_EXCLUDE=` packs it too.
PACK_EXCLUDE ?= hello
PACK_ADDONS := $(filter-out $(PACK_EXCLUDE),$(ADDONS))
COMMON_SRCS := $(wildcard $(CURDIR)/common/*.c)

target: $(addprefix $(BUILD_DIR)/target/,$(addsuffix .so,$(ADDONS)))
host: $(addprefix $(BUILD_DIR)/host/,$(addsuffix .so,$(ADDONS)))

# One rule shape for both halves. The toolchain, the flags and the output
# directory are chosen by target-specific variables on the mode's directory --
# not by $(if) inside the recipe, whose commas would eat -Wl,--gc-sections.
$(BUILD_DIR)/target/%.o $(BUILD_DIR)/target/%.so: ADDON_CC = $(CROSS_CC)
$(BUILD_DIR)/target/%.o: ADDON_CFLAGS = $(TARGET_CFLAGS)
$(BUILD_DIR)/target/%.so: ADDON_LDFLAGS = $(TARGET_LDFLAGS)

$(BUILD_DIR)/host/%.o $(BUILD_DIR)/host/%.so: ADDON_CC = $(HOST_CC)
$(BUILD_DIR)/host/%.o: ADDON_CFLAGS = $(HOST_CFLAGS)
$(BUILD_DIR)/host/%.so: ADDON_LDFLAGS = $(HOST_LDFLAGS)

# $(1) is the mode (target or host), $(2) the addon id.
define ADDON_RULE
$(BUILD_DIR)/$(1)/$(2).so: $$(patsubst $(CURDIR)/%.c,$(BUILD_DIR)/$(1)/%.o,$$(wildcard $(CURDIR)/$(2)/*.c) $(COMMON_SRCS))
	@mkdir -p $$(dir $$@)
	$$(ADDON_CC) $$(ADDON_LDFLAGS) -o $$@ $$^
	@echo "addon $(2) [$(1)]: $$@ ($$$$(stat -c%s $$@) bytes)"

$(BUILD_DIR)/$(1)/%.o: $(CURDIR)/%.c
	@mkdir -p $$(dir $$@)
	$$(ADDON_CC) $$(ADDON_CFLAGS) -c $$< -o $$@
endef

$(foreach mode,target host,$(foreach id,$(ADDONS),$(eval $(call ADDON_RULE,$(mode),$(id)))))

# ---------------------------------------------------------------------------
# exports: what the player has to publish for these addons
#
# A dlopen()ed library resolves its undefined symbols against the objects
# already loaded, the executable included -- but only through the dynamic symbol
# table, and the player exports almost nothing by itself (libgcc and libstdc++).
# The player's link therefore takes --dynamic-list with this file.
#
# The list has two halves:
#
#   exports.base       every symbol of the modules an addon is meant to use, and
#                      of LVGL's public widgets (exports-base.sh writes it). This
#                      is what makes an addon a card-only affair: a new one,
#                      written against the documented headers, finds what it
#                      calls already exported and needs no new firmware image.
#   the addons' UND    whatever this tree's addons call on top of that, so an
#                      addon may reach a symbol the base left out without the
#                      base having to grow for it.
#
# The base costs 365 KB of binary (measured against a list built from the addons
# alone): exported symbols are roots for --gc-sections, so the widgets nothing
# calls stay. That is the price of not touching the image again.
#
# A superset is harmless: a name the player does not define is ignored, and the
# libc names an addon imports are in the list too and do nothing.
#
# Rewritten only when its contents change: it is a prerequisite of the player's
# link, and touching it on every build would relink the player on every build.
# ---------------------------------------------------------------------------
EXPORTS = $(BUILD_DIR)/exports.list

BASE_EXPORTS = $(CURDIR)/exports.base

# Regenerating the base needs the player's objects, so it is a step of its own
# and not part of `exports`: see exports-base.sh.
.PHONY: exports-base
exports-base:
	@$(CURDIR)/exports-base.sh

.PHONY: exports
exports: $(BASE_EXPORTS) target
	@mkdir -p $(BUILD_DIR)
	@libs="$(wildcard $(BUILD_DIR)/target/*.so)"; \
	tmp=$(EXPORTS).tmp; \
	{ \
		echo "/* Generated by addons/Makefile: exports.base plus what these addons call. */"; \
		echo "{"; \
		sed 's/^/  /; s/$$/;/' $(BASE_EXPORTS); \
		for lib in $$libs; do \
			$(CROSS_READELF) --dyn-syms -W $$lib 2>/dev/null; \
		done | awk '$$7 == "UND" { sub(/@.*/, "", $$8); print $$8 }' | sort -u \
			| grep -Ev '^$$' | sed 's/^/  /; s/$$/;/'; \
		echo "};"; \
	} > $$tmp; \
	if cmp -s $$tmp $(EXPORTS); then rm -f $$tmp; else mv $$tmp $(EXPORTS); echo "exports: $$(grep -c ';' $(EXPORTS)) names for the player to publish"; fi

# ---------------------------------------------------------------------------
# The checks
#
# check-abi repeats, for every target library, what the player's own check-abi
# does: the toolchain builds against glibc 2.27 and the device has 2.16, and a
# symbol from a newer version means the library does not load -- with a sentence
# from dlerror that names the missing version, on a device whose user has no
# console in front of them.
#
# check-exports is the other half of -rdynamic plus --gc-sections: an undefined
# symbol the player does not export is a dlopen() that fails with "undefined
# symbol", and the likeliest cause is a helper that only an addon called and the
# linker therefore collected. Run it after building the player.
# ---------------------------------------------------------------------------
.PHONY: check-abi check-exports checks
checks: check-abi check-exports

check-abi:
	@if [ ! -x "$(CROSS_READELF)" ]; then \
		echo "check-abi: $(CROSS_READELF) is not there, check skipped"; \
		exit 0; \
	fi; \
	bad_all=""; \
	for lib in $(wildcard $(BUILD_DIR)/target/*.so); do \
		bad=$$($(CROSS_READELF) --dyn-syms -W $$lib 2>/dev/null \
			| awk '{ for (i = 1; i <= NF; i++) if (match($$i, /@GLIBC_[0-9.]+$$/)) { \
				ver = substr($$i, RSTART + 7); n = split(ver, v, "."); \
				if (v[1] > $(DEVICE_GLIBC_MAJOR) || (v[1] == $(DEVICE_GLIBC_MAJOR) && v[2] > $(DEVICE_GLIBC_MINOR))) print "  " $$i; \
			} }' | sort -u); \
		if [ -n "$$bad" ]; then \
			echo ""; \
			echo "*** $$lib asks for symbols the device does not have:"; \
			echo "$$bad"; \
			bad_all="$$bad_all $$lib"; \
		fi; \
	done; \
	if [ -n "$$bad_all" ]; then \
		echo ""; \
		echo "*** The device has glibc $(DEVICE_GLIBC_MAJOR).$(DEVICE_GLIBC_MINOR). These addons would not load."; \
		echo "*** Add a .symver in src/system/core/glibc_compat.h, which explains how."; \
		rm -f $$bad_all; \
		exit 1; \
	fi; \
	echo "check-abi: no addon asks for anything beyond glibc $(DEVICE_GLIBC_MAJOR).$(DEVICE_GLIBC_MINOR)."

PLAYER_BIN ?= $(PLAYER_DIR)/sonix_player
# The unstripped copy: the check needs the player's own symbol table, which the
# shipped binary does not carry.
PLAYER_DEBUG_BIN ?= $(PLAYER_DIR)/sonix_player_debug

check-exports:
	@if [ ! -x "$(CROSS_READELF)" ] || [ ! -f "$(PLAYER_BIN)" ]; then \
		echo "check-exports: no $(PLAYER_BIN) or no readelf, check skipped"; \
		exit 0; \
	fi; \
	defined=$$(mktemp); exported=$$(mktemp); undef=$$(mktemp); missing_file=$$(mktemp); \
	src=$(PLAYER_DEBUG_BIN); [ -f "$$src" ] || src=$(PLAYER_BIN); \
	$(CROSS_READELF) --syms -W $$src 2>/dev/null \
		| awk '$$7 != "UND" && ($$4 == "FUNC" || $$4 == "OBJECT") { sub(/@.*/, "", $$8); print $$8 }' \
		| sort -u > $$defined; \
	$(CROSS_READELF) --dyn-syms -W $(PLAYER_BIN) 2>/dev/null \
		| awk '$$7 != "UND" { sub(/@.*/, "", $$8); print $$8 }' | sort -u > $$exported; \
	bad_all=""; \
	for lib in $(wildcard $(BUILD_DIR)/target/*.so); do \
		$(CROSS_READELF) --dyn-syms -W $$lib 2>/dev/null \
			| awk '$$7 == "UND" { sub(/@.*/, "", $$8); print $$8 }' | sort -u > $$undef; \
		comm -12 $$undef $$defined > $$missing_file; \
		comm -23 $$missing_file $$exported > $$undef.missing; \
		if [ -s $$undef.missing ]; then \
			echo "*** $$lib needs symbols the player defines but does not export:"; \
			sed 's/^/  /' $$undef.missing; \
			bad_all="$$bad_all $$undef.missing"; \
		fi; \
		rm -f $$undef.missing; \
	done; \
	rm -f $$defined $$exported $$undef $$missing_file; \
	if [ -n "$$bad_all" ]; then \
		echo ""; \
		echo "*** These are collected by --gc-sections or were never public:"; \
		echo "*** the addons would fail to open. Make the player reference them."; \
		exit 1; \
	fi; \
	echo "check-exports: every symbol an addon takes from the player is exported."

# ---------------------------------------------------------------------------
# pack: what actually goes on the card
#
#   build/Addons/<id>/addon.json
#   build/Addons/<id>/addon.so
#   build/Addons/<id>/icon.png      (when the addon ships one)
#   build/Addons/<id>/...           (anything else the addon needs)
#
# Copy that folder to <card>/Addons and the tiles appear at the next start.
# ---------------------------------------------------------------------------
.PHONY: pack
pack: target
	@rm -rf $(BUILD_DIR)/Addons
	@for id in $(PACK_ADDONS); do \
		mkdir -p "$(BUILD_DIR)/Addons/$$id"; \
		cp "$(CURDIR)/$$id/addon.json" "$(BUILD_DIR)/Addons/$$id/"; \
		cp "$(BUILD_DIR)/target/$$id.so" "$(BUILD_DIR)/Addons/$$id/addon.so"; \
		for extra in "$(CURDIR)/$$id"/*.png "$(CURDIR)/$$id"/*.txt "$(CURDIR)/$$id"/*.ini; do \
			if [ -f "$$extra" ]; then cp "$$extra" "$(BUILD_DIR)/Addons/$$id/"; fi; \
		done; \
	done
	@echo "pack: $(BUILD_DIR)/Addons -- copy it to <card>/Addons"

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
