#ifndef ADDON_LISTROW_H
#define ADDON_LISTROW_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// Covers for the rows of a streaming-service list.
//
// A page builds a list of rows and knows the cover URL of each one. This module
// is the bookkeeping between that and src/gui/streaming/listart.h, which is the
// downloader: a pool of twelve slots with one owner at a time, a request per
// slot, and a poll that collects what came back.
//
// Why this is a layer and not part of listart
// -------------------------------------------
// listart knows URLs, slots and pixels, and nothing about rows. A page would
// otherwise have to carry, per row, the URL, the generation, whether the cover
// arrived, whether it failed, which slot it holds, and to give the slot back
// when the row scrolls away or is destroyed. tidalpage.c and qobuzpage.c each
// write that out in full -- a hundred lines, twice -- and it is the part that
// goes wrong quietly: a slot left behind by a deleted row starves the rows that
// are on screen, and a stale picture drawn into a recycled row shows the wrong
// album. That is state about a page's widgets, and the downloader is the wrong
// place for it: the same listart serves a page with one hero image
// (podcastpage.c) as well as one with a hundred rows, and it must not learn
// about anybody's lv_obj_t.
//
// So listart keeps the network, the cache on the card and the decode; this
// keeps the rows. A page includes this header and nothing else, and no page
// needs to know that the pictures come from listart at all.
//
// The model
// ---------
//   row   -- an lv_image the page created and laid out. The module sets its
//            source when the cover arrives and touches nothing else about it:
//            the page owns the object, its size, its place and its styles. The
//            decode size is taken from the object's width, so the picture is
//            not decoded at one size and resampled to another.
//   index -- the row's position in the page's list. Only the rows inside the
//            range given to listrow_visible() are ever requested.
//   owner -- the page, in the sense listart.h gives the word: a stable pointer
//            that differs between pages (the address of a static char in the
//            page's own translation unit). One page is on screen at a time.
//
// The setting: [streaming] list_art, on unless the card sets it to 0. There is
// no page for it in the interface, so "off by default" used to mean no cover
// appeared in any list of any addon. Off, listrow_bind() records nothing, no
// timer is created and no memory is allocated -- a page may call this
// unconditionally and pay nothing. The setting is read when a page resets
// (listrow_reset()), so a change made in the settings takes effect the next
// time a page opens.
//
// Threading: every function here runs on the GUI thread, and so does the timer
// inside. listart downloads on its own worker; this module only polls with an
// LVGL timer, exactly as the pages do by hand today.
//
// Lifetime of a picture: it is requested for a row inside the visible range,
// and it stays with the row while the row is within a small margin of that
// range. Further away -- or rebound, forgotten, reset -- the slot is given back
// and the decoded picture is freed, so memory follows what is on screen rather
// than how far the user has scrolled. Coming back is a read of the cache on the
// card, not a download.

// Binds `row` (an lv_image) to the cover at `url`, as the row at `index` of
// `owner`'s list. Returns immediately; the picture appears on a later pass of
// the timer.
//
// `url` NULL or "" unbinds the row: its slot is given back and its picture
// dropped. `owner` must not be NULL. Binding the same row to another URL makes
// anything in flight for the old one useless: it is abandoned and cannot be
// drawn into the row that now means something else.
void listrow_bind(void *owner, lv_obj_t *row, int index, const char *url);

// The rows on screen, as a range of indices. Called after the list is filled
// and on every scroll. Rows inside it are requested; a row that leaves it gives
// its slot back at once, so the slot goes to a row that is visible. Without a
// call to this, nothing is ever downloaded: the range starts empty.
void listrow_visible(void *owner, int first, int last);

// A new page, or the same one reopened: gives every slot back, drops every
// picture and forgets every row of `owner`. A URL that failed is allowed again.
void listrow_reset(void *owner);

// The row object is going away (LV_EVENT_DELETE, or just before the page
// destroys it): forget it. listrow_bind() installs this handler itself, so a
// page that deletes its rows normally need not call it; it is here for the
// pages that recycle widgets or want the slot back before the object dies.
void listrow_forget(lv_obj_t *row);

// The whole addon is being unloaded: delete the poll timer and forget every row
// and every owner. The timer is not a child of any widget, so deleting the page
// does not take it with it, and it is paused rather than stopped when idle --
// a paused timer whose callback is in a dlclose()d library is a crash on the
// next tick. Call from the addon's stop(), after listrow_reset().
void listrow_stop(void);

#endif /* ADDON_LISTROW_H */
