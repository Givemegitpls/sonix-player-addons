#ifndef STREAM_QOBUZSERVICE_H
#define STREAM_QOBUZSERVICE_H

#include "src/addon/addon_service.h"

// Qobuz, seen through the service registry (src/system/streaming/service.h).
//
// While the service is still part of the binary this adapter is all there is:
// the table is filled from qobuzcache_*/qobuzsync_*, so the rest of the player
// stops naming Qobuz and starts asking the registry. When the service moves to
// the card (docs/ADDONS.md), this file becomes the addon's
// sonix_addon_service_entry and the player keeps the registry.
//
// The two interface objects -- the mark over the artwork and "show album" --
// arrive with the page (qobuz_service_attach_gui), because this file must stay
// free of LVGL: the power manager and USB include it.

// The table itself, for the addon's sonix_addon_service_entry(). Registering it
// is the loader's job (src/addon/addon.c), not the addon's.
const sonix_addon_service_t *qobuz_service_table(void);

// Called from qobuzpage_init(): `badge` is an lv_image_dsc_t, `open_album` is
// qobuzpage_open_album. Both may be NULL.
void qobuz_service_attach_gui(const void *badge, void *(*open_album)(const char *album_id, const char *title));

#endif /* STREAM_QOBUZSERVICE_H */
