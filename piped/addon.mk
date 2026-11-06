# YouTube, as a service addon.
#
# The sources keep the paths an addon inside the player's tree would have
# (src/system/gw/..., src/gui/...), so no #include inside them is rewritten:
# the addon's own folder is on the include path and from there the tree looks
# like the player's.
piped_CFLAGS = -I$(CURDIR)/piped -O3
piped_CXXFLAGS = $(piped_CFLAGS)
