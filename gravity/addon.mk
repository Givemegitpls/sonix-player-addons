# Gravity Defied as an addon.
#
# The game is C++ (7 700 lines of it) and upstream is C++17, which is the first
# thing this file has to say to the build: the addon's sources are compiled by the
# C++ driver and the library is linked by it too, because the runtime comes with
# it (the same arrangement Gearboy's addon.mk makes).
#
# The tree is kept as upstream laid it out -- game/src/*.cpp beside the game's own
# headers, the lcdui layer in game/src/lcdui -- so an upstream change can still be
# read as a diff. What was SDL is replaced, not rewritten around: the five files
# that talked to SDL are gone and platform/ implements the same classes. See
# NOTICE.md for the exact list.
gravity_CFLAGS = -I$(CURDIR)/gravity -I$(CURDIR)/gravity/game/src -O3

# -std=c++17: std::filesystem in Micro.h and inline static members.
#
# -fno-rtti: nothing in the game uses dynamic_cast or typeid, and without them
# libstdc++'s type_info tables go away.
#
# Exceptions stay on: the game throws (Image's constructor, RecordStore) and its
# own main loop catches, which is upstream's error handling and not ours to
# remove.
#
# -Wno-narrowing and -Wno-sign-compare: upstream is rebuilt from a decompiler's
# output in places and these are all over it. Warnings, not errors.
gravity_CXXFLAGS = $(gravity_CFLAGS) -std=c++17 -fno-rtti -Wno-narrowing -Wno-sign-compare -Wno-unused-variable

# The linker is the C++ driver for both halves: the library needs libstdc++, and
# on the target the runtime goes inside it. The device has libstdc++.so.6.0.21 --
# GCC 5's ABI, GLIBCXX_3.4.21 -- while the toolchain is GCC 9 and refers to symbols
# up to 3.4.26, so a dynamically linked library does not load at all ("cannot open
# shared object file: libstdc++.so.6", the message the shell puts up over the tile).
# Gearboy's addon.mk carries the same two flags for the same reason. The simulator
# keeps the machine's own libstdc++: distributions do not ship a static one.
gravity_LD_target = $(CROSS_CXX) -static-libstdc++ -static-libgcc
gravity_LD_host = $(HOST_CXX)
