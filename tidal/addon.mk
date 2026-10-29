# Tidal, as a service addon.
#
# The sources keep the paths they have in the player (src/system/streaming/...,
# src/gui/streaming/...), so no #include inside them was rewritten: the addon's
# own folder is on the include path and from there the tree looks like the
# player's.
tidal_CFLAGS = -I$(CURDIR)/tidal -O3
tidal_CXXFLAGS = $(tidal_CFLAGS)
