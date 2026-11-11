---
name: sonix-addon
description: Use when creating, building, debugging or porting an addon for the Sonix Player on the TempoTec V3 Analog — an app in Addons/ on the card that appears as a tile in Streaming or More. Covers where an addon keeps its files (its own folder, `<addon>/cache`, addon.json owns_paths/cache_path, migrations with common/addonfs.h), the stop hook and unloading, and the two audio paths (radio_play vs the download-cache queue). Trigger keywords: аддон, addon, Addons folder, addon.json, addon.so, dlopen, exports.list, dynamic-list, sonix_addon_entry, sonix_addon_stop_entry, addonfs, owns_paths, cache_path, saves, аддонные сейвы, unload, subsonic addon, Yandex Music addon, Zvuk addon.
---

# Addons on the card

An addon is a MIPS32 `.so` on the microSD card that the shell opens with
`dlopen()` when its tile is tapped. Read `docs/ADDONS.md` in the repository
first — it is the reference. This skill is the checklist.

## Facts that cost time to rediscover

- **Builds run in docker or on the host, both work.**
  `docker/build.sh` builds the addons in the player's image
  (Ubuntu 24.04, public registry) and needs nothing but docker: the prebuilt
  cross compiler is a host x86-64 binary that needs glibc ≥ 2.38, which Ubuntu
  24.04 has. `make` on the machine is the same build and is what to use
  while editing; `host` is host-only (`make host`).
- An addon keeps **its own settings on the card**, never in the player's
  `device_config.ini`: `<card>/Addons/<id>/<something>.ini` via
  `common/store.{h,c}`, path built from `host->dir`. `config_save()` from
  a worker thread races the interface, and the card is where an addon lives.
  Caches and indexes go in `<card>/Addons/<id>/cache/` — see "Where an addon
  keeps its files".
- `sonix-player/rockbox-toolchain` is built inside the submodule (20-60 minutes,
  once) or mounted from outside with `ROCKBOX_TOOLCHAIN_DIR`.
- The player publishes a **checked-in** list of symbols,
  `sonix-player/sonix-player/src/addon/exports.list`, linked with `-Wl,--dynamic-list`. It is
  not generated here: growing it is a change in the player's repository
  (`tools/addon_exports.sh`), and `make checks` says when an addon has outgrown it.
- `make checks` answers the two questions that otherwise show up as a
  `dlopen` failure on the device: a symbol the player collected
  (`check-exports`) and a symbol from a newer glibc (`check-abi`).
- The submodule's revision *is* the interface version: bump it, rebuild, re-run
  `make checks`. Two addons built against different submodule revisions are two
  different ABIs.

## Where an addon keeps its files

The rule, and it is not a preference: **an addon writes under
`<card>/Addons/<id>/` and nowhere else.** Settings beside `addon.so`, caches and
indexes in `<card>/Addons/<id>/cache/`, save files beside the settings. Nothing
in the card root, nothing in the player's `.local/` tree.

Why it is a rule and not a habit: several addons once shared `.local/` with the
player and with each other, so a factory reset had to guess which directory was
whose, one addon's "clear my covers on exit" deleted another's, the USB export
had to ask about files nobody could attribute, and the user saw directories on
their card that no app of theirs claimed.

```c
#include "common/addonfs.h"   // built into every addon

char cache[512];
// 1. MIGRATE FIRST: an existing destination is what "already migrated" means,
//    and addon_cache_dir() creates exactly that destination.
addon_migrate_path("<card>/.local/<svc>-cache", cache);   // old path -> new
addon_cache_dir(host, cache, sizeof cache);               // <addon>/cache, created
```

- `addon_migrate_path()` renames, never copies, and only when the destination
  does not exist yet; a failed rename leaves both paths alone and the next start
  repeats it. A read-only card must degrade to "no cache" with a sentence, not
  crash.
- Declare what you own, so the player can answer "is this file a service's",
  export it over USB and wipe only caches on a factory reset:
  ```json
  "service": true,
  "owns_paths": ["Addons/<id>/cache/", "Addons/<id>/saved.tsv"],
  "cache_path": "Addons/<id>/cache/"
  ```
  Paths are relative to the card root, no leading slash. `cache_path` is what a
  factory reset empties — never put settings there.
- **`listart` (`.local/listart/`) is the player's** cover cache, shared by every
  page. Use it, never delete it, never claim it in `owns_paths`.
- Saved *music* a service writes into the user's library (`<music>/<Service>/…`)
  is the user's, not an addon save: it stays where the library is. Say so in a
  comment where the path is built.
- A game's save files belong to the cartridge, not to the emulator: Gearboy
  keeps `.sav`/`.state1` next to the ROMs in `Games/`, and that is correct.


1. **Scaffold**
   ```sh
   new.sh <id> streaming|more
   ```
   Writes `<id>/addon.json` and `<id>/<id>.c` from the hello
   example.
2. **Manifest** — `api` 1, `title`, `section`, optional `titles` (keyed by the
   language file's name: `Russian`, `English`, `Italiano`…), optional `icon`
   (`icon.png` is used when the key is absent). A service also declares
   `"service": true`, `owns_paths` and `cache_path` (see "Where an addon keeps
   its files").
   The tile itself: draw it with `sonix-player/sonix-player/tools/make_icon.py` rather than
   by hand — see the `sonix-icons` skill. It reads at 74 px on this panel, and
   the loader keeps alpha, so the tile's own card shows through a transparent
   corner.
3. **Implement** `sonix_addon_entry()`, `init()` and `sonix_addon_stop_entry()`.
   `init` returns the screen, or `NULL`; the stop hook answers whether the
   library may be unloaded (see "Lifecycle").
4. **Build**
   ```sh
   make          # build/target/<id>.so
   make host     # build/host/<id>.so for the simulator
   make checks   # ABI + exports (needs the player built for check-exports)
   ```
   With only docker on the machine: `docker/build.sh` (and `docker/build.sh
   pack`). It runs in the player's image, Ubuntu 24.04 from
   the public registry; `host` stays host-only.
5. **Try it in the simulator** (see below), then `make pack` → copy
   `build/Addons/*` to `<card>/Addons/`.

## Interface rules

- Only `src/addon/addon.h` is ABI. Everything else is "this build's headers":
  rebuild the addon whenever the player is rebuilt.
- `host->cfg` gives `screen_width`, `screen_height`, `padding`,
  `top_bar_height`. **Every pixel size goes through `SC()`** — the addon is
  compiled with `-DSONIX_LAYOUT_SCALE=2` like the player.
- Reuse the shell: `settingsrow_title` / `settingsrow_add` / `settingsrow_page`,
  `keyboard_create`, `switch_screen`, `switcher_set_back_guard`,
  `theme_style_*`, `font_ui_24`. Do not hand-roll rows or a keyboard.
- `settingsrow_page()` does not scroll. A long list needs your own container
  with `lv_obj_set_scroll_dir(c, LV_DIR_VER)`.
- A row name longer than ~18 characters overlaps its right-hand value; clip it
  yourself (`clip()` in `subsonic/page.c`).
- A label that must stay on one line: `#include "common/oneline.h"`, give it a
  fixed width first (`lv_pct(100)` of a sized parent counts), then
  `oneline_label(label)` — it switches to `LV_LABEL_LONG_MODE_DOTS` and pins the
  height to the face's own line, which is what makes LVGL cut and write `...`
  instead of wrapping. No character count: the faces are proportional (300 px of
  Caveat at 22 px holds 38 lower-case letters and 26 capitals). Called before
  the font is set or before a later font change, the height is the wrong one —
  call it again.

## Threads

- UI thread: `init()`, every event callback, every `lv_*` call.
- Network and card I/O: a worker thread of your own, then `gui_post(cb, user)`
  back to the UI thread. Never `lv_async_call()`.
- Call **`thread_signal_stack()`** (exported) as the worker's first statement.
  An alternate signal stack is per thread and only the player's own thread
  helpers install one; without it a stack overflow in your worker kills the
  process with nothing in the log — the crash handler has no room to run, and
  "the device rebooted by itself" is all anyone sees. It costs 32 KB and does
  nothing when a stack is already there.
- Drop replies that are no longer wanted: keep a generation counter, compare it
  in the posted callback (`subsonic/page.c`).

## Audio

Two ways in, and they are not interchangeable:

- **`radio_play(&station)`** — a live stream. Fill `name` and `url`, leave
  **`uuid` empty** (a non-empty one is reported to radio-browser as a listen),
  check `radio_station_problem()` first. The sheet then shows a station: no
  length, no seek, **no queue, no shuffle, no repeat** — a stream has no end.
  `radio_set_list()` makes prev/next step through a list of stations, which is
  the most a streaming addon can get.
- **The queue** — a track the addon fetched to the card and plays with
  `audio_play(path)`. Shuffle, repeat, the queue page, "play next", the star and
  a real cover all work, because to the player it is an ordinary file. This is
  what Zvuk and Yandex Music do, and it is the path to take for any service that
  can download a track. The pieces, all exported:
  - fetch with `http_stream` into `<addon>/cache/<id>.<ext>` and wrap it with
    `growfile_announce/progress/finish` (`src/system/decode/growfile.h`) so
    playback starts on the growing file instead of after it;
  - take the shared download turn (`streamturn_acquire/release`, and register an
    abandon hook with `streamturn_register_abandon` — **unregister it in stop**);
  - hand the order to the player with `playlist_load_paths(..., start)` and the
    spot actions with `playlist_insert_next` / `playlist_enqueue`;
  - register `device_state_add_prepare_cb(cb)`: the player calls it with the path
    it is about to open, and that is where the download for the *next* track
    starts;
  - export a `sonix_addon_service_entry()` table (`src/addon/addon_service.h`)
    with `cache_dir`/`owns_path`/`track_id`/`downloading`/`network_wanted`/
    `clear_on_exit`/`wipe`/`fav_*`, or the star, the sleep timer and the factory
    reset will not know your files exist.

`zvuk` is the reference for the queue path (`zvukcache.c`, `zvukplay.c`,
`zvukservice.c`); `subsonic` started on `radio_play` and was moved to the
queue for exactly the reasons above.

## Lifecycle: an addon is unloaded

An addon is **not** loaded for the life of the process any more. The player
unloads it when the card leaves and reloads it when its files change on the card,
so every addon exports a second symbol:

```c
static bool x_stop(void);           // true = safe to dlclose()
static const sonix_addon_stop_t x_lifecycle = { .api = SONIX_ADDON_API, .stop = x_stop };
const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) { … }
```

`stop()` runs on the UI thread; after it returns the shell deletes the addon's
screens and `dlclose()`s the library. So it must:

- join or count-and-wait every thread it started (a detached worker needs a
  **per-module** counter — one shared count makes one module's stop wait for
  another's idle worker and the unload fails; see `yandex`'s
  `yandexlive` for the per-owner shape);
- delete every `lv_timer_t` it created (with `power_forget_timer()` if it went to
  `power_pause_in_standby`/`power_slow_in_standby`);
- withdraw every registration into the player: `theme_unregister_refresh`,
  `device_state_remove_prepare_cb`, `streamturn_unregister_abandon`,
  `listrow_stop()`, its back guards;
- close its files, sockets and child processes;
- clear every static pointer into a screen and reset per-run state, so `init()`
  works again in the same process;
- register its screens with `sonix_addon_own_screen(host->id, screen)` as they
  are created, so the shell can take the display off them and delete them. The
  screen `init()` returns is registered for you (appending, not overwriting).

Return **false** when a worker could not be stopped (one parked in an
`http_request` is the usual case — the player cannot interrupt it). The shell
then keeps the mapping, logs it and refuses to open that addon again this run: a
leak instead of a jump into unmapped code. Anything that would leave a callback
into the library is caught by the same reasoning — `gui_post` work still queued
is dropped by the loader when the library goes.

Updating a loaded addon is part of this: `addon_open()` compares the file
identity (`stat`: dev/ino/size/mtime) of `addon.so` and `addon.json` with what it
loaded, and reloads when they changed — no reboot needed. **Replace the file by
rename, never in place**: `cp` over a mapped `addon.so` truncates the inode the
live addon is mapped from and the next call crashes the player. On the device
that is `adb push … addon.so.new && adb shell mv addon.so.new addon.so`.

## Testing without the device

```sh
make host
SDL_VIDEODRIVER=offscreen SONIX_SD_ROOT=/tmp/sd \
  SONIX_LANG_DIR=$PWD/../sonix-packer/assets/BLAZE/usr/resource/sonix/language \
  ./sonix_player_host
```

- The simulator's settings live at `/tmp/sonix/data/device_config.ini`; seed
  `[ui] language`, `[clock] configured = true` or it stops on the welcome flow.
- Taps: `printf 'x y' > /tmp/sonix/tap`. Screenshots: `touch /tmp/sonix/shot`,
  the PNG lands in `$SONIX_SD_ROOT/Screenshots/`.
- **Write the tap file by rename, not in place**
  (`printf ... > /tmp/sonix/tap.tmp && mv /tmp/sonix/tap.tmp /tmp/sonix/tap`):
  the player polls every frame and `remove()`s the file after it reads it, so a
  write that lands inside that window is read as an empty line and every other
  tap is silently lost.
- `[power] screen_off_seconds = 0` in `/tmp/sonix/data/device_config.ini`, or
  the panel blanks after 30 s and every tap after it is ignored.
- Tile geometry on the 320x480 panel: 2x3 grid, columns at x≈92 and x≈246, rows
  at y≈107, 252, 399.
- For a service addon, a mock server is cheaper than a real account: a Python
  `http.server` answering `ping`, the list calls and `stream` is enough.

## Log lines

```
addon: <id>  '<title>' (streaming)          found at scan time
addon: <id> opened                          dlopen + init + switch_screen
addon: dlopen ...: undefined symbol: X      the player does not export X
addon: <id> init() gave no screen           init() returned NULL
addon: <id>/icon.png is not a picture ...   PNG could not be decoded; fallback glyph
```

## Porting a service (Yandex Music, Zvuk)

- Take the API client from `Rekkod-Tree/sonix-player-rekkod-mod` (GPLv3), strip
  its dependencies on that tree's player (`streamerr`, `provider`, `svclist`,
  `*cache`, `*dl`), and keep `subsonic_last_error()`-style reporting.
- Their playback goes through a download cache and `audio_play(file)`. That path
  **does** exist here now — `growfile` + the queue + `device_state_add_prepare_cb`
  + the service table (see "Audio"). Zvuk and Yandex Music are the ported
  examples; `radio_play` is for streams only.
- Their application keys are sealed with a compiled-in key; an addon cannot use
  it. Plan on token import by the user.

## Moving an existing page or module into an addon

A different job from writing one from scratch, and it has its own skill:
`sonix-addon-port` (inventory with `tools/addon_port.py`, the `src/`-mirroring
layout, `init()` running once, state that has to move to the card, and what the
player used to know about the module). Gearboy and Flappy Bird are the worked
examples, in `gearboy` and `flappybird`.

## Do not

- Do not add `-rdynamic` to the target link: it keeps everything `--gc-sections`
  drops, 7.3 MB → 8.9 MB, and the rootfs volume has ~2 MB of room.
- Do not put addon sources under `sonix-player/sonix-player/src/` — `APP_SRCS` is
  `find src -name '*.c'` and they would be compiled into the player.
- Do not translate addon strings with `tr()`: the language files are on the
  read-only rootfs and cannot know about a card.
- Do not store an addon's settings with `config_set()`/`config_save()`: that is
  the player's file, rewritten whole, and saving it from an addon's worker
  thread races the interface. `common/store.h` writes the addon's own
  file on the card instead.
- Do not write outside your own folder: no card-root files, no `.local/`
  directories, no deleting `.local/listart` (see "Where an addon keeps its
  files"). A path the player must know about belongs in `addon.json`'s
  `owns_paths`/`cache_path`.
- Do not expect an addon to live forever: it is unloaded on a card pull and
  reloaded when its files change, so a missing `sonix_addon_stop_entry` is a
  refused load (see "Lifecycle").
- Do not call `switch_screen()` in `init()` for the screen it returns: the shell
  switches, and a second switch to the screen already up is a no-op inside LVGL,
  so everything the page does from `SCREEN_LOADED` (hiding the status bar) is
  done once and then undone.
