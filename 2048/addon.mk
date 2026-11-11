# 2048. Sources keep the games/ paths they share with Dino, so the addon's own
# folder is on the include path and the tree looks like the player's.
2048_CFLAGS = -I$(CURDIR)/2048 -O3
2048_CXXFLAGS = $(2048_CFLAGS)
