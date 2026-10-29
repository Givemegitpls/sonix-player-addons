# Dino, from the TempoTec V1 port.
#
# The sources keep the paths they had there (src/gui/games/...), so nothing in
# them had to be rewritten for the include paths: the addon's own folder is on
# the include path and from there the tree looks like the player's.
dino_CFLAGS = -I$(CURDIR)/dino -O3
dino_CXXFLAGS = $(dino_CFLAGS)
