# Gearboy as an addon.
#
# The emulator used to be part of the binary and was the only C++ in the tree:
# the player linked its own copy of libstdc++ for it. It is a separate package
# now -- core, cartridge database, input, pages and settings -- and this file
# is what that costs.
#
# The sources are laid out under src/ exactly as they were in the player, so
# every include inside them ("src/gb/gbcore.h", "src/system/gearboy/gbdb.h")
# still resolves: the addon's own folder is on the include path and from there
# the tree looks like the player's.
gearboy_CFLAGS = -I$(CURDIR)/gearboy -I$(CURDIR)/gearboy/src/gb/core -I$(CURDIR)/gearboy/src/gb/miniz \
				 -DGB_CORE=1 -DGEARBOY_DISABLE_DISASSEMBLER -Wno-attributes

# The flags Gearboy's own sources need, the same set the player's Makefile gives
# them (GB_CFLAGS):
#
#   -DGEARBOY_DISABLE_DISASSEMBLER  drops the disassembler and the breakpoints:
#       they are for Gearboy's own debugger, which is not here.
#   -fno-exceptions -fno-rtti       the core uses neither, and without them a
#       few tables and some dead code go away.
#   -Wno-attributes                 Gearboy declares InvalidOPCode() noinline
#       first and inline afterwards. Upstream code, left alone on purpose (see
#       src/gb/README.md).
gearboy_CXXFLAGS = $(gearboy_CFLAGS) -fno-exceptions -fno-rtti

# -O3 on the emulator, like the player gave it (TARGET_HOT_OPT): a Game Boy
# frame is a few hundred thousand cycles of interpreted code every 16 ms.
gearboy_CFLAGS += -O3
gearboy_CXXFLAGS += -O3

# A C++ addon is linked by the C++ driver, and on the target with the runtime
# inside it: the device has libstdc++.so.6.0.21, that is GLIBCXX_3.4.21 (the GCC
# 5 ABI), and the toolchain is GCC 9, which emits references up to 3.4.26.
# Linked dynamically the library would not load at all. The simulator uses the
# machine's own libstdc++, which is why the host line carries no static flags
# (most distributions do not ship a static libstdc++).
gearboy_LD_target = $(CROSS_CXX) -static-libstdc++ -static-libgcc
gearboy_LD_host = $(HOST_CXX)
