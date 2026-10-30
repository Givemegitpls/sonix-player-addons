# Yandex Music, as a service addon.
#
# The sources keep the paths they have in the player they came from
# (src/system/streaming/..., src/gui/streaming/...), so no #include inside them
# was rewritten: the addon's own folder is on the include path and from there
# the tree looks like the player's.
#
# No -DSONIX_YANDEX: unlike Zvuk, the fork never wrapped the Yandex sources in
# "#ifdef SONIX_YANDEX" -- they were part of the player unconditionally -- so
# there is no define to turn them on with. (addons/zvuk/addon.mk has that one
# for Zvuk, whose sources are wrapped.)
#
# -ldl: the addon carries its own copy of the fork's crypto.c (encraw files are
# AES-256-CTR, and this player has no AES at all), which opens libcrypto with
# dlopen(). The player links libdl, so the symbol would resolve anyway; naming
# the library is what makes the addon say what it needs.
yandex_CFLAGS = -I$(CURDIR)/yandex -O3
yandex_CXXFLAGS = $(yandex_CFLAGS)
yandex_LIBS = -ldl
