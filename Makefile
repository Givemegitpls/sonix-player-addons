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
CROSS_CXX ?= $(firstword $(wildcard $(PLAYER_DIR)/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-g++ \
			   $(PLAYER_DIR)/rockbox-toolchain/rockbox-dev/bin/mipsel-rockbox-linux-gnu-g++))
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
HOST_CXX ?= c++

HOST_CFLAGS = -O2 -g -Wall -MMD -MP -fPIC -DHOST_BUILD=1 \
			  $(shell pkg-config --cflags freetype2) \
			  -D_FILE_OFFSET_BITS=64 -I$(CURDIR) -I$(PLAYER_DIR) -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE=1 \
			  $(ADDON_BOARD_CFLAGS)

# What a C++ translation unit of an addon is compiled with. The player has one
# of these too (Makefile, TARGET_CXXFLAGS); here it is the C set, and an addon
# that needs Gearboy's flags adds them to $(id)_CXXFLAGS (see "Per-addon
# additions" below).
TARGET_CXXFLAGS = $(TARGET_CFLAGS)
HOST_CXXFLAGS = $(HOST_CFLAGS)

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
#
# .o is a C object and .opp a C++ one: an addon that brings C++ (Gearboy's core
# and its APU) is compiled by the C++ driver, and its final link names that
# driver too (see $(id)_LD_target below), because the runtime comes with it.
$(BUILD_DIR)/target/%.o: ADDON_CC = $(CROSS_CC)
$(BUILD_DIR)/target/%.opp: ADDON_CC = $(CROSS_CXX)
$(BUILD_DIR)/target/%.so: ADDON_CC = $(CROSS_CC)
$(BUILD_DIR)/target/%.o $(BUILD_DIR)/target/%.opp: ADDON_CFLAGS = $(TARGET_CFLAGS)
$(BUILD_DIR)/target/%.so: ADDON_LDFLAGS = $(TARGET_LDFLAGS)

$(BUILD_DIR)/host/%.o: ADDON_CC = $(HOST_CC)
$(BUILD_DIR)/host/%.opp: ADDON_CC = $(HOST_CXX)
$(BUILD_DIR)/host/%.so: ADDON_CC = $(HOST_CC)
$(BUILD_DIR)/host/%.o $(BUILD_DIR)/host/%.opp: ADDON_CFLAGS = $(HOST_CFLAGS)
$(BUILD_DIR)/host/%.so: ADDON_LDFLAGS = $(HOST_LDFLAGS)

# ---------------------------------------------------------------------------
# Per-addon additions
#
# A directory may carry addon.mk, included before any addon is declared. It is
# plain make, with the addon's name as the prefix, and it exists so that one
# addon's oddities stay out of the other ones' builds:
#
#   gearboy_CFLAGS        extra flags for its C sources
#   gearboy_CXXFLAGS      extra flags for its C++ sources
#   gearboy_LIBS          extra libraries to link
#   gearboy_LD_target     the linker to use, when it is not the C driver
#   gearboy_LD_host       the same for the simulator
#
# Everything here is expanded when a recipe runs, so a variable may name
# $(CROSS_CXX) without caring where it was defined.
# ---------------------------------------------------------------------------
-include $(wildcard $(CURDIR)/*/addon.mk)

# $(1) is the mode (target or host), $(2) the addon id.
#
# The sources are everything under the addon's folder, not just its top level:
# an addon that carries a program (Gearboy's core, its C++ APU a few directories
# down, miniz) keeps the tree it came from. common/ is compiled into every addon
# and gets the ordinary flags, which is why it has a rule of its own.
#
# Two directories are never sources: `target/`, a cargo build tree (Spotify
# carries a Rust program beside its C, and ring's own build script writes .c
# into it -- compiled into an addon they were "multiple definition of `main`"),
# and `tools/`, the host-side probes of such a program, which have a main() of
# their own.
define ADDON_RULE
$(2)_CSRCS := $$(sort $$(wildcard $(CURDIR)/$(2)/*.c) $$(shell find $(CURDIR)/$(2) -mindepth 2 -name '*.c' ! -path '*/target/*' ! -path '*/tools/*' 2>/dev/null))
$(2)_CXXSRCS := $$(sort $$(shell find $(CURDIR)/$(2) -name '*.cpp' ! -path '*/target/*' 2>/dev/null))
$(1)_$(2)_OBJS := $$(patsubst $(CURDIR)/%.c,$(BUILD_DIR)/$(1)/%.o,$$($(2)_CSRCS) $(COMMON_SRCS)) \
				  $$(patsubst $(CURDIR)/%.cpp,$(BUILD_DIR)/$(1)/%.opp,$$($(2)_CXXSRCS))

$(BUILD_DIR)/$(1)/$(2).so: $$($(1)_$(2)_OBJS)
	@mkdir -p $$(@D)
	$$(or $$($(2)_LD_$(1)),$$(ADDON_CC)) $$(ADDON_LDFLAGS) $$($(2)_LIBS) -o $$@ $$^
	@echo "addon $(2) [$(1)]: $$@ ($$$$(stat -c%s $$@) bytes)"

$$(BUILD_DIR)/$(1)/$(2)/%.o: $(CURDIR)/$(2)/%.c
	@mkdir -p $$(@D)
	$$(ADDON_CC) $$(ADDON_CFLAGS) $$($(2)_CFLAGS) -c $$< -o $$@

$$(BUILD_DIR)/$(1)/$(2)/%.opp: $(CURDIR)/$(2)/%.cpp
	@mkdir -p $$(@D)
	$$(ADDON_CC) $$(ADDON_CFLAGS) $$($(2)_CXXFLAGS) -c $$< -o $$@
endef

$(foreach mode,target host,$(foreach id,$(ADDONS),$(eval $(call ADDON_RULE,$(mode),$(id)))))

$(BUILD_DIR)/target/common/%.o: $(CURDIR)/common/%.c
	@mkdir -p $(@D)
	$(ADDON_CC) $(ADDON_CFLAGS) -c $< -o $@

$(BUILD_DIR)/host/common/%.o: $(CURDIR)/common/%.c
	@mkdir -p $(@D)
	$(ADDON_CC) $(ADDON_CFLAGS) -c $< -o $@

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
		$(CROSS_STRIP) --strip-debug "$(BUILD_DIR)/Addons/$$id/addon.so"; \
		for extra in "$(CURDIR)/$$id"/*.png "$(CURDIR)/$$id"/*.txt "$(CURDIR)/$$id"/*.ini; do \
			if [ -f "$$extra" ]; then cp "$$extra" "$(BUILD_DIR)/Addons/$$id/"; fi; \
		done; \
		if [ -d "$(CURDIR)/$$id/assets" ]; then cp -r "$(CURDIR)/$$id/assets" "$(BUILD_DIR)/Addons/$$id/"; fi; \
	done
	@echo "pack: $(BUILD_DIR)/Addons -- copy it to <card>/Addons"

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
