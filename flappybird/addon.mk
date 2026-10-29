# Flappy Bird as an addon.
#
# The sources are under src/ so that the include paths inside them ("src/gui/
# flappybird/flappysound.h", the shell's "src/gui/shell/...") keep working as
# they were written: the addon's own folder is on the include path, and from
# there the tree looks like the player's.
flappybird_CFLAGS = -I$(CURDIR)/flappybird -O3

# A game draws a 480x720 frame four times the pixel size every 16 ms: the same
# reason the player builds its own game code at -O3 (Makefile, TARGET_HOT_OPT).
flappybird_CXXFLAGS = $(flappybird_CFLAGS)
