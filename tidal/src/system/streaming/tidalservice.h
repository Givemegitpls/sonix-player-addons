#ifndef STREAM_TIDALSERVICE_H
#define STREAM_TIDALSERVICE_H

#include "src/addon/addon_service.h"

// Tidal, seen through the service registry (src/system/streaming/service.h).
//
// While the service is still part of the binary this adapter is all there is:
// the table is filled from tidalcache_*/tidalsync_*, so the rest of the player
// stops naming Tidal and starts asking the registry. When the service moves to
// the card (docs/ADDONS.md), this file becomes the addon's
// sonix_addon_service_entry and the player keeps the registry.
//
// The two interface objects -- the mark over the artwork and "show album" --
// arrive with the page (tidal_service_attach_gui), because this file must stay
// free of LVGL: the power manager and USB include it.

// The table itself, for the addon's sonix_addon_service_entry(). Registering it
// is the loader's job (src/addon/addon.c), not the addon's.
const sonix_addon_service_t *tidal_service_table(void);

// Called from tidalpage_init(): `badge` is an lv_image_dsc_t, `open_album` is
// tidalpage_open_album. Both may be NULL.
void tidal_service_attach_gui(const void *badge, void *(*open_album)(const char *album_id, const char *title));

#endif /* STREAM_TIDALSERVICE_H */
