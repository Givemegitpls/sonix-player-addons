#ifndef ZVUKSERVICE_H
#define ZVUKSERVICE_H

#include "src/addon/addon_service.h"

// Zvuk as the rest of the player sees it.
//
// The player asks two kinds of question about a streaming service, and this is
// the second one (docs/ADDONS.md, "A service rather than an app"):
//
//   * before the library is opened -- "is this path Zvuk's, may the device
//     sleep, is a download running" -- answered from the addon's manifest
//     (owns_paths/cache_path) and, once it is open, from this table;
//   * about the account -- the star, "add to playlist", "show album" -- which
//     only the service can answer.
//
// The table is the shape the fork called stream_provider_t (provider.h there);
// here it is sonix_addon_service_t, the interface every service addon fills in,
// and addons/qobuz/src/system/streaming/qobuzservice.c is the one to read beside
// this. Exported through sonix_addon_service_entry(), which the loader calls as
// soon as the library is open.
//
// Threads, as the header says: the cache hooks are asked from any thread and
// never touch LVGL; the account hooks are called on the UI thread and their
// `done` arrives on zvuksync's worker.

const sonix_addon_service_t *zvuk_service_table(void);

// The two pieces only the page can supply: the mark drawn over the artwork
// while a Zvuk track plays, and "show album". Called from the page's init(),
// long after the table was registered.
void zvuk_service_attach_gui(const void *badge, void *(*open_album)(const char *album_id, const char *title));

#endif /* ZVUKSERVICE_H */
