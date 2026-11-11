# Shared addon code

Every `.c` in this folder is compiled into **every** addon.

That is deliberate. An addon is a self-contained package on a card: a shared
`libcommon.so` would have to be found, versioned and kept in step with each of
them, and a card that lost it would take out every addon at once. The pieces here
are small, and each library is linked with `-ffunction-sections
-Wl,--gc-sections` and `RTLD_LOCAL`, so an addon pays only for what it calls and
two addons' copies never meet.

What is here:

- `cardpace.{h,c}` — the rate a download may fill the card at (1 MB/s). Writing a
  track at network speed makes this device's card time out and stalls the whole
  player in `D` state; a `cardpace_t` per download plus `cardpace_note()` after
  every write of track audio is what keeps the backlog short. The long story is
  `docs/ADDONS.md`, "The card is the slowest thing in the player".
- `gunzip.{h,c}` — a small inflate for the `Content-Encoding: gzip` a proxy in
  front of a server may add whatever the request asked for.
- `oneline.{h,c}` — a label kept to one line, cut with dots: LVGL's
  `LV_LABEL_LONG_MODE_DOTS` with the height pinned to the face's own line, which
  is what makes it write `...` instead of wrapping. No character count: the
  faces are proportional (300 px of the cassette's Caveat at 22 px holds 38
  lower-case letters and 26 capitals). The label needs a fixed width first.
- `store.{h,c}` — an addon's own `key = value` settings file on the card, written
  atomically. Every addon that remembers anything (a session, a token, a quality)
  uses this rather than the player's `device_config.ini`: that file belongs to
  the player, it is rewritten whole on every save, and saving it from an addon's
  worker thread races the interface.
- `listrow.{h,c}` — covers for the rows of a service's list: a row is bound to a
  URL, the picture appears in it when it arrives, and the twelve download slots
  the player has are shared out among the rows that are actually on screen. The
  downloading itself is the player's (`listart`); this is the row bookkeeping
  the pages would otherwise each write out by hand. On unless the card sets
  `[streaming] list_art = 0`; there is no page for it in the interface.
