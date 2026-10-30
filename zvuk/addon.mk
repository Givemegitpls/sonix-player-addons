# Zvuk, as a service addon.
#
# The sources keep the paths they have in the player they came from
# (src/system/streaming/..., src/gui/streaming/...), so no #include inside them
# was rewritten: the addon's own folder is on the include path and from there
# the tree looks like the player's.
#
# -DSONIX_ZVUK: in the fork every Zvuk source is wrapped in "#ifdef SONIX_ZVUK",
# where it was a build option of the player. An addon is either built or not,
# so the define is what turns the six modules on.
zvuk_CFLAGS = -I$(CURDIR)/zvuk -O3 -DSONIX_ZVUK=1
zvuk_CXXFLAGS = $(zvuk_CFLAGS)
