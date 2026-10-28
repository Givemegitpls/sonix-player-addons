# Shared addon code

Every `.c` in this folder is compiled into **every** addon.

That is deliberate. An addon is a self-contained package on a card: a shared
`libcommon.so` would have to be found, versioned and kept in step with each of
them, and a card that lost it would take out every addon at once. The pieces here
are small, and each library is linked with `-ffunction-sections
-Wl,--gc-sections` and `RTLD_LOCAL`, so an addon pays only for what it calls and
two addons' copies never meet.

What is here:

- `gunzip.{h,c}` — a small inflate for the `Content-Encoding: gzip` a proxy in
  front of a server may add whatever the request asked for.
- `store.{h,c}` — an addon's own `key = value` settings file on the card, written
  atomically. Every addon that remembers anything (a session, a token, a quality)
  uses this rather than the player's `device_config.ini`: that file belongs to
  the player, it is rewritten whole on every save, and saving it from an addon's
  worker thread races the interface.

The next piece will be a cover-art loader keyed by URL, for the services whose
covers are an endpoint rather than a file.
