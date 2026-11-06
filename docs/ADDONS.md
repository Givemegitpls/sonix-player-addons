# Addons: apps that arrive on the card

The Streaming and More pages are tiled sections, and every tile in them used to
be a module compiled into `sonix_player`. This document is about the other kind:
applications that live in `Addons/` on the microSD card, appear as tiles in
those two sections, and are loaded by the shell the first time their tile is
tapped.

The reference implementation is `subsonic/` – a client for the user's own
OpenSubsonic server (Navidrome, Gonic, Airsonic, Ampache). `hello/` is the
smallest addon there is and the one to copy when starting another.

Three of the tiles that used to be compiled in are addons now: **Gearboy** and
**Flappy Bird** live in `gearboy/` and `flappybird/`, and the
player no longer contains a line of either (see "The addons in this tree"). They
are also what put C++ and the `order` key in the manifest — the two things this
document grew when they moved.

**Dino** (`dino/`) came the other way: it never was in this tree. It is
the page from the TempoTec V1 port's PR #56, which its own repository reverted
the same day, and it arrived on a card like any other addon — which is the point
of the whole arrangement.

## Why libraries and not a config file

A declarative "addon format" (a manifest describing HTTP endpoints and fields)
was considered and rejected. The services people actually ask for – Yandex
Music, Zvuk, Subsonic – are not catalogs over a fixed JSON shape: they have
token exchange, their own paging, retries, caches, downloads, and a UI that is
nothing like a list of URLs. Those are programs, and the honest way to let one
arrive on a card is to hand it the same headers and the same widgets the
compiled-in services use.

So an addon is a `.so` built by the player's cross compiler, opened with
`dlopen()`.

## What is on the card

```
Addons/
  subsonic/
    addon.json        what it is and where its tile goes
    addon.so          MIPS32 little-endian, built by `make`
    icon.png          optional; 140x140 or any size, scaled down
    README.txt        the addon's own instructions, packed as it lies there
  flappybird/
    addon.json
    addon.so
    icon.png
    README.txt
    assets/           whatever else the addon brought: 63 PNGs and 5 OGGs here
```

`addon.so` is the fixed name; the loader does not read it from the manifest.
`README.txt` is not read by anything either: `make pack` copies an addon's
`*.txt`, `*.png` and `*.ini` along, so the instructions ride in the release zip
(and on the card beside the code) without the loader knowing about them.
`assets/` is not a keyword either — Flappy Bird asks `host->dir` for it because
that is where `make pack` put it; an addon may call its folder anything.

The user's side of this — unzipping a release into `<card>/Addons/<id>/`, and the
sign-in of each service — is [`SETUP.md`](SETUP.md), kept apart so that this file
can stay about the interface.

### addon.json

```json
{
  "api": 1,
  "title": "Subsonic",
  "titles": { "Russian": "Subsonic", "Italiano": "Subsonic" },
  "section": "streaming",
  "order": 3,
  "version": "0.1",
  "icon": "icon.png"
}
```

| key | required | meaning |
|---|---|---|
| `api` | yes | the ABI the library was built against; must be `1` today |
| `title` | yes | the tile's caption |
| `titles` | no | per-language captions, keyed by the language file's name (`Russian`, `English`, …) |
| `section` | yes | `streaming` or `more` – which page the tile lands on |
| `order` | no | the tile's place on that page, 1-based; without it the tile goes after the built-in ones |
| `version` | no | logged at scan time; not interpreted |
| `icon` | no | picture in the addon's folder; `icon.png` is used when absent |
| `service` | no | `true` when the addon is a streaming service: it also exports `sonix_addon_service_entry()`, and the player asks it about the account (src/addon/addon_service.h) |
| `owns_paths` | no | the card paths that are the addon's, relative to the card, answered from the manifest alone — before the library is opened (USB export, the sleep timer, a factory reset) |
| `cache_path` | no | the directory a factory reset empties, relative to the card |

`order` is what makes an addon a replacement rather than an addition: a page
moved out of the binary asks for the place its tile used to have, and the page
looks the same as before (Gearboy and Flappy Bird, "The addons in this tree").
It is read into `addon_info_t.order` and handed to the grid as
`grid_entry_t.order`; `gridpage_set_entries()` inserts the tile there and
shifts the rest down (src/gui/shell/gridpage.c). Two addons asking for the same
place are ordered by name, and a number past the end of the page means the last
place.

The directory name is the addon's id and is what `host->id` reports. A folder
with no `addon.so`, or a manifest that does not parse, is skipped with a line in
the log – not an error the user sees, because there would be no tile to tap.

The folder is read at start-up **and again whenever the card comes or goes**: the
storage layer posts `addon_card_changed()` (src/addon/addon_notify.h), the
registry re-reads `Addons/`, and the Streaming and More pages rebuild their
tiles (`streaming_refresh()`, `morepage_refresh()`). A library that is already
open stays open across that: the slot moves, the handle and the screens do not.

A scan never clears the registry when it cannot read the folder. The directory is
opened before anything is overwritten, so a card that is out, not mounted yet, or
simply has no `Addons/` leaves the tiles that are already there alone and writes
one line to the log (`addon: cannot read <path>: <reason>`). An insert
re-scans in every case, even when the player believed the card had never left:
`card_databases_attach()` posts the scan before its "already attached" early
return. A card that goes away is the one case a scan cannot express -- at that
moment the mount may still be half alive, and whether `Addons/` reads is a matter
of the scheduler -- so the removal path drops the tiles explicitly instead:
`addon_card_gone()` (src/addon/addon_notify.h) empties the registry without
touching the filesystem at all, from `card_databases_detach()`.

## The interface

`sonix-player/sonix-player/src/addon/addon.h`, and only that.

```c
const sonix_addon_t *sonix_addon_entry(uint32_t api);
```

The library exports that one function and nothing else is required of it. The
shell calls it with `SONIX_ADDON_API`, and an addon built against another number
returns `NULL` and gets a sentence instead of a crash:

```c
static const sonix_addon_t hello = {
    .api = SONIX_ADDON_API,
    .title = "Hello",
    .init = hello_init,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
    return api == SONIX_ADDON_API ? &hello : NULL;
}
```

`init(host)` runs once, on the UI thread, the first time the tile is tapped, and
returns the screen to show – or `NULL` if it cannot start. Everything the addon
creates (its screens, its workers) is created there.

`host` carries:

| field | what it is |
|---|---|
| `id` | the folder name, `"subsonic"` |
| `dir` | the addon's own folder on the card, absolute |
| `sd_root` | the card root, as the rest of the player knows it |
| `cfg` | `gui_config_t`: `screen_width`, `screen_height`, `padding`, `top_bar_height` |

### The stop half: unloading, and what the card made necessary

An addon used to live as long as the process, and this page said so: "there is no
`deinit`". The card made that untenable. A `dlopen()`ed `.so` is a mapping of a
file on the card, a mapping keeps the filesystem alive after the card is pulled,
and the player then lives with a dead superblock that blocks every read that
still lands on it. So every addon exports a *second* entry symbol, and a library
without one is refused at the door with a sentence rather than loaded and never
unloadable:

```c
const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api);
```

`stop()` runs on the GUI thread, and TRUE means "the library is safe to unmap".
After it returns the player deletes the addon's screens and `dlclose()`s the
library, so:

- every thread it started has exited or been joined;
- every `lv_timer_t` it created is deleted (and, if it was registered with
  `power_pause_in_standby()`/`power_slow_in_standby()`, dropped with
  `power_forget_timer()` first);
- every callback it registered with the player is withdrawn:
  `theme_unregister_refresh()`, `device_state_remove_prepare_cb()`,
  `streamturn_unregister_abandon()`, its back guards;
- its files, sockets and child processes are closed;
- every static pointer into a screen is cleared and per-run state reset, so
  `init()` works a second time in the same process;
- its own screens are registered with `sonix_addon_own_screen(host->id, screen)`
  as they are created, so the player can take the display off them and delete
  them. The screen `init()` returns is registered automatically.

Return FALSE when a worker could not be stopped -- typically one parked inside an
HTTP request, which the player cannot interrupt. The player then keeps the
mapping, logs it, and refuses to open that addon again for the rest of the run: a
leak, deliberately, instead of a jump into unmapped code.

Two places unload an addon: the card leaving (synchronously, before the unmount:
`addon_card_gone()` waits on the GUI thread and the unmount follows it) and a
library whose files changed on the card — an addon copied over adb, downloaded
over the network or carried in on a card is reloaded the next time its tile is
tapped, because `addon_open()` compares the file identity it recorded with what
is on the card now.

`gui_post()` calls an addon makes are safe: the shell drops queued callbacks
whose function lies inside the library's mapping before it unmaps it.

What is still anchored when the library goes: `keyboard.c` and `settingsrow.c`
unlink their objects when the tray or slider is deleted, `listrow` needs
`listrow_stop()` because its poll timer is not a child of any widget.

### When the ABI has to change

Bump `SONIX_ADDON_API` whenever either structure changes shape or meaning, and
rebuild the addons (`make`) as part of the change. Old libraries on
somebody's card then fail with "the addon %s did not start" rather than with a
`SIGSEGV` in a field that moved.

## What an addon may call

Everything the player exports. Which is to say: the shell's own widgets
(`gridpage`, `settingsrow`, `keyboard`, `switcher`, `theme`, `fonts`), `http`,
`json`, `md5`, `config`, `cover`, `radio`, LVGL itself, and libc. The addon
includes the player's headers as the player does:

```c
#include "src/gui/shell/settingsrow.h"
#include "src/system/net/http.h"
```

**How the symbols get there.** A `dlopen()`ed library resolves what it does not
define against the objects already loaded, but only through the *dynamic* symbol
table – and the player exports almost nothing by itself. So the player's link
takes `-Wl,--dynamic-list=src/addon/exports.list`: a checked-in list, in the
player's repository, of every symbol of the modules this document tells an addon
to use (the shell widgets and fonts, `http`, `json`, `config`, `md5`, `cover`,
`radio`, the device and library helpers) **and of LVGL's public widgets**. It is
generated by `sonix-player/sonix-player/tools/addon_exports.sh` from the player's own objects
and regenerated only when those modules change.

The list lives with the player and not here, and that is the whole point: the
player links with it whether or not this repository is checked out, so a symbol
that is on the list is exported for every card, and a card-only addon written
against the documented headers needs no new firmware image – not even one that
uses an LVGL widget no other addon had touched. This repository cannot extend it
from here; `make checks` is what says an addon has outgrown it (`check-exports`,
"Building"), and growing it is a change in the player's repository.

It costs 397 KB of binary, because an exported symbol is a root for
`--gc-sections` and the widgets nothing calls stay in; that was measured against
a list built from the addons alone (7.74 MB → 8.14 MB). The rootfs volume had
about 4 MB free, and the packer refuses a tree that does not fit, so the price is
paid once and checked on every image.

The alternative – `-rdynamic` – was measured too and is not the same thing:
it exports every symbol the player has (six thousand) *and* keeps all dead code,
7.3 MB → 8.9 MB, with no way to tell an interface from an internal.

## Threads

The interface is not thread-safe: LVGL runs without `LV_USE_OS` here, so
touching it from another thread is a real `SIGSEGV`. The rules are the player's
own, and they are not optional:

- `init()` and every event callback run on the UI thread;
- anything that touches the network or the card **must** run on a thread of its
  own – `http_get()` blocks for as long as the server takes;
- when that thread is done, hand the work back with `gui_post(cb, user)`, which
  runs `cb(user)` on the UI thread within a few tens of milliseconds. Never
  `lv_async_call()`, and never `lv_*` from the worker.

`subsonic/page.c` shows the shape: one detached worker per request fills
a shared array under a mutex, posts its generation number, and the UI thread
drops an answer whose generation is no longer the newest one.

## Audio

`radio_play(&station)` is the public way into the audio path, and it is what the
addons use: `radio.c` owns the transport, the codec detection, the buffering and
the now-playing line, and an addon is not going to grow a second one.

```c
radio_station_t station = {0};
snprintf(station.name, sizeof(station.name), "%s - %s", track->artist, track->title);
subsonic_stream_url(track->id, station.url, sizeof(station.url));
if (!radio_station_problem(&station)) {
    radio_play(&station);
}
```

What that costs, and it is worth knowing before designing an addon around it:

- what plays is a *radio station*: the player sheet shows a station name with no
  position, no length and no seeking;
- there is no queue: tapping a track replaces the previous one;
- leave `uuid` empty. A non-empty one is reported to radio-browser as a listen
  (`radio.c`, `report_click`), and a station that did not come from its
  directory has no business being counted there.

An addon that needs a real queue and seeking is a change to the player (a public
"play this URL as a track" path), not a change to the addon.

### Sound the player does not own

A game is the other kind: it has to sound a button press within a frame, which
the transport's buffering is wrong for. `audio_external_begin_latency()` takes
the DAC out of the player's hands, `audio_external_write()` pushes interleaved
frames into it and `audio_external_end()` gives it back — this is what the
metronome, the Bluetooth receiver and both games use (see
`gearboy/src/system/gearboy/gearboy.c` and `flappybird/.../flappysound.c`).

Two things to know before using it:

- while an addon holds the output, the player's own playback has been stopped
  (`audio_stop()`), and the radio's stream with it — that is what the games do,
  on purpose: a game is not a soundtrack;
- the sleep timer and the power manager ask `audio_external_active()`, and an
  owner of the DAC counts as "something is playing, there is nothing here to
  pause". This is how the games tell the player they are running now that they
  are not part of it: the flag used to be `gearboy_running()`, a call into the
  emulator from `gui/nowplaying/player.c`, and the DAC is what the two halves
  can see of each other.

## The interface it draws

- The screen comes from `lv_obj_create(NULL)`; put it in the history with
  `switch_screen(screen)` and the chevron and swipe-back work as everywhere.
  **`init()` itself must not call `switch_screen()` for the screen it returns**:
  the shell does that, and a second switch to the screen already up is a no-op
  inside LVGL (`lv_display.c`, `lv_screen_load_anim` returns before loading), so
  anything a page does from its `SCREEN_LOADED` event — the ones here hide the
  status bar — is done once and then undone by the second switch. That is how
  Flappy Bird came back from the move with a status bar over it.
- The page heading is `settingsrow_title(screen, cfg, text)`; rows are
  `settingsrow_add()` / `settingsrow_action()`. A list that outgrows the panel
  needs a container that scrolls – `settingsrow_page()` does not, and its
  comment says why (a settings page is meant to fit).
- `switcher_set_back_guard(screen, handler)` lets a page keep the back chevron
  for itself. The Subsonic list screen uses it to walk its own stack of levels
  (artists → albums → tracks) before it lets the addon close.
- **Every pixel size goes through `SC()`** (`src/gui/shell/scale.h`): the
  interface is written at 480x720 and compiled at 320x480, and an addon is
  compiled with the same board defines (`-DSONIX_LAYOUT_SCALE=2`,
  `-DSONIX_ICON_SET_SMALL=1`) for exactly this reason.
- The tile icon is drawn at the size the compiled-in menu icons are (111 units
  through `SC()`); the loader scales whatever PNG the addon ships down to it.
- An icon keeps its **alpha**: it is decoded here as ARGB8888 and blended over
  the card, not converted to RGB565. The player's cover loader asks stb_image
  for three channels, and a logo on transparency comes out of that with black
  behind it – a black square on the tile. `icon_pack()` in the player's `src/addon/addon.c`
  does four channels, an area average weighted by alpha (so the edges of a glyph
  do not darken), and a never-upscale scale.
- A long row name and a right-hand value do not always fit together.
  `settingsrow_add()` steps a name down to fit, but that runs from layout
  events, and a value filled in right after the row is made keeps the whole
  width for the name and draws over the value. Clip long names in the addon
  (`subsonic/page.c`, `clip()`).

### Making the tile icon

The tile's picture is `icon.png` in the addon's folder, named by the manifest.
Nothing is compiled for it and nothing has to be rebuilt: the player reads it
when it scans the card. What constrains it is the other end —

- It is drawn in a box of `SC(111)`, which is **74 px** on the 320x480 panel,
  and it is never scaled up. Draw for that size; a 140x140 or 512x512 source is
  plenty, a 48x48 one is not.
- Alpha is kept, so the addon does not have to draw a card of its own: whatever
  is transparent takes the tile's colour. Opaque corners show as a square.
- 4 MB and 4096x4096 are the loader's limits (`icon_load()`); past either it
  logs and falls back to the "more" tile.

The drawing the compiled-in tiles use, and therefore the one an addon should
match, is: an upright rounded square carrying a white glyph, the same square
again a few degrees off and saturated behind it, both on a diagonal gradient of
one hue. `tools/make_icon.py` composes exactly that, so that a new addon's tile
is a command rather than a drawing session:

```sh
python3 tools/make_icon.py --glyph sonix-player/sonix-player/assets/icons/brand/spotify.svg --mono \
    --glyph-colour '#1ED760' --out spotify/icon.png --preview /tmp/p.png
python3 tools/make_icon.py --glyph sonix-player/sonix-player/assets/icons/brand/yandexmusic.svg --hue 54 \
    --out yandex/icon.png
```

- `--hue` is the tile's colour in degrees; `--mono` is the dark grey of the
  Tidal tile instead of a hue.
- `--glyph` takes any SVG (a Lucide line icon, a simple-icons brand mark,
  something traced) or a PNG. It is recoloured to `--glyph-colour` and fitted to
  the tile by its measured ink, so a traced logo and a 24x24 Lucide glyph come
  out the same size.
- `--preview` writes the result at 140 px beside the same tile at 74 px, which
  is the size to judge it at. `--svg-only` prints the tile's SVG instead.
- `--size 512` is for the card-sized copy; the player scales it down itself.

Rasterising needs one of cairosvg, rsvg-convert or ImageMagick, on whichever
machine makes the icon. None of them is needed to build or to run the player.

The brand marks in the player's `assets/icons/brand/` are sources, not shipped artwork, and
are not in any icon set: `spotify.svg` is the simple-icons path; `yandexmusic.svg`
and `zvuk.svg` are tracings of the services' own app icons (the star from the
180 px touch icon, the Zvuk squircle from its favicon) potraced into vector
curves; `dino.svg` is Dino's own pixel art turned into rectangles, so it stays
crisp. These are trademarks of their owners, used here to label the service
they open, exactly as `tidal.png` and `qobuz.png` already were.

An icon wanted as a *compiled-in* menu tile is a different road: an entry in
`ICONS` or `COLOR_ICONS` in `tools/svg_to_lvgl.py`, then both icon sets
regenerated, which rewrites 24 MB of checked-in source. An addon's tile never
needs that.

## Languages

An addon's own strings are not in `/usr/resource/sonix/language`: that tree is
part of the rootfs and cannot know about a program that arrived on a card. The
manifest's `titles` covers the tile. Everything else is written in whatever
language the addon is written in – Subsonic's pages are English – or the addon
brings its own small table.

## Building

This repository is the addons on their own: the player is a git submodule at
`sonix-player/` — the player's repository, whose own source tree is one level
down, at `sonix-player/sonix-player/` (the repository also carries its docs,
patches and packer). That tree supplies the headers, the ABI (`src/addon/addon.h`),
the cross toolchain's own build (`sonix-player/sonix-player/rockboxdev/rockboxdev.sh`),
the docker image and LVGL. Nothing else is needed.

```sh
git clone https://github.com/Givemegitpls/sonix-player-addons
cd sonix-player-addons
git submodule update --init        # the player, at the revision this builds against
make                               # lvgl (cloned once), then build/target/<id>.so
make host                          # build/host/<id>.so, for the SDL simulator
make checks                        # ABI and export checks; run after the player is built
make pack                          # build/Addons/<id>/ -- copy to <card>/Addons
```

`make` needs the cross toolchain in `sonix-player/sonix-player/rockbox-toolchain`; with
nothing but docker on the machine, the script builds it and runs the build in the
player's image:

```sh
docker/build.sh                       # make
docker/build.sh pack                  # the folder that goes on a card
docker/build.sh ADDON_IDS="dino qobuz"   # only those
```

There is no separate image: an addon is compiled by the same cross toolchain as
the player, against the same headers, and the player's image
(`sonix-player/sonix-player/docker/Dockerfile`, Ubuntu 24.04 pinned by digest from the public
registry) carries make and python3 along with it. `ROCKBOX_TOOLCHAIN_DIR` is
passed through as it is for the player. `host` is not in there: the simulator's
addons link against the machine's own libraries, so `make host` runs on
the machine.

The one exception is the Spotify engine, which is Rust and needs a compiler the
player's image deliberately does not carry: it has an image of its own,
`spotify/engine-mips/docker/Dockerfile` (the same Ubuntu 24.04, plus the pinned
nightly and the musl cross compiler), built by `make -C spotify/engine-mips`
when it is not there. The player's image has no Rust in it at all.

`make` compiles every directory that has an `addon.json`, plus
everything in `common/` (compiled into each addon on purpose: an addon is
a self-contained package). `ADDON_IDS="…"` narrows it to the named ones, which
is what the CI does; a name that has no directory stops the build instead of
being ignored.

**The version of the player is the version of the interface.** The submodule's
revision is what `addon.h` and the headers came from, so an addon builds against
one ABI and no other: bump the submodule, rebuild, and re-run `make checks`. The
player's repository carries a pointer to this document, not a copy of it – one
document, in one place, so that the two cannot disagree.

The **sources are everything under the addon's folder**, not just its top level,
so an addon that carries a program keeps the tree it came from: Gearboy's core is
`gearboy/src/gb/core/`, its APU a directory under that. Both of them keep
the paths they had in the player — `<id>/src/gui/gearboy/gearboyplay.c`
still says `#include "src/system/gearboy/gbdb.h"` — because the addon's own
folder is on the include path and from there the tree looks like the player's.
Nothing in a moved file had to be rewritten.

**C++ is allowed.** `.cpp` is compiled by the cross `g++` into a `.opp`, and an
addon that has any is linked by `g++` with `-static-libstdc++ -static-libgcc`: the
device has `libstdc++.so.6.0.21`, that is GLIBCXX_3.4.21 (the GCC 5 ABI), and the
toolchain is GCC 9, which emits references up to 3.4.26 — the same reason the
player used to link that way. On the host there are no static flags (most
distributions do not ship a static libstdc++); the simulator uses the machine's.

That is what Gearboy costs: its `addon.so` is 2.5 MB, of which 1.3 MB is code and
the rest is the parts of libstdc++ its core touches (the iostreams and locale
facets a `std::ostream` brings along are the bulk). Linked against the device's
own libstdc++ the library is 690 KB, but that needs GLIBCXX_3.4.21 to exist
there and is a bet this tree does not make.

**An addon's own flags live in `<id>/addon.mk`**, included before any
addon is declared, with the addon's name as the prefix: `gearboy_CFLAGS`,
`gearboy_CXXFLAGS`, `gearboy_LIBS`, and `gearboy_LD_target` /
`gearboy_LD_host` when the linker is not the C driver. That is where Gearboy's
`-DGEARBOY_DISABLE_DISASSEMBLER -fno-exceptions -fno-rtti -Wno-attributes` and
its `-I` into the core went when the emulator left the player's Makefile.

The flags mirror the player's, and three of them matter:

- `-march=mips32r2 -mno-madd4` – the SoC's core, and no fused multiply-add;
- `-include src/system/core/glibc_compat.h` – the toolchain's glibc is 2.27 and
  the device's is 2.16; five maths functions have to be asked for by their old
  names or the library does not load;
- `-lpthread -lm -lrt` – an addon says which libraries it links. `clock_gettime`
  is in librt on this device, and the `.symver` above only resolves against the
  library that exports the name.

The checks:

- `check-abi` walks each `.so`'s dynamic symbols and refuses anything above
  `GLIBC_2.16` – the same check the player's own link runs, for the same reason
  (on this device a missing symbol is a boot loop, not an error message);
- `check-exports` compares the addons' undefined symbols with what the player
  defines and exports, and names anything that `--gc-sections` collected.

The addons on the card must be rebuilt whenever the player they run on is
rebuilt: the ABI is the submodule's headers, not a frozen interface, which is
why the submodule's revision is what a release pins.

## Where an addon keeps its settings

Not in the player's configuration. `device_config.ini` belongs to the player: it
is rewritten whole on every save, and an addon writing to it from a worker thread
races the interface for it. An addon's settings are also its own business, and
they belong next to the addon — on the card it arrived on.

`common/store.{h,c}` is the small shared piece for that (compiled into
every addon, like the rest of `common/`): a flat `key = value` file with
`store_load()`, `store_get()`, `store_set()` and `store_save()`. `store_save()`
writes `<path>.tmp`, flushes it to the card and renames it over the old file, so
a card pulled mid-write or a battery that goes at that moment leaves the previous
file whole rather than half of a new one. An empty value removes the key.

The path comes from `host->dir`, the addon's own folder under `<card>/Addons`,
which `init()` is given:

```c
snprintf(path, sizeof(path), "%s/session.ini", host->dir);
```

Every addon that keeps anything does it this way: `subsonic/session.ini`,
`gearboy/settings.ini`, `flappybird/state.ini`, `zvuk/settings.ini` and
`zvuk/zvuk_session.ini`, `yandex/settings.ini` and `yandex/yandex_session.ini`,
`qobuz/settings.ini` and `qobuz/session.ini`, `tidal/settings.ini` and
`tidal/session.ini`, `spotify/credentials.ini`. What a game produces goes on the
card as well, but not through the store — Gearboy's saves are files next to the
player's ROMs (`Games/Saves`), because they belong to the game cartridge rather
than to the emulator's settings, and other tools read them.

### Caches and indexes: `<addon>/cache`, and nothing in `.local`

Settings are not the whole of an addon's state. A service has a download cache
and an index of what it saved; an emulator has a ROM index. Those used to live in
the player's shared tree (`<card>/.local/<service>-cache/`, `.local/<id>-saved.tsv`,
`.local/gearboy-cache/`), and that was wrong in three ways: four addons claimed
one directory the player also uses (`.local/listart/`), a factory reset had to
guess which parts of `.local` were whose, and the user could not attribute a
directory on their card to anything.

The rule now: **an addon writes under `<card>/Addons/<id>/` and nowhere else** —
settings beside `addon.so`, caches and indexes in `<card>/Addons/<id>/cache/`.
`common/addonfs.{h,c}` does the two things that makes easy:

```c
char cache[512];
addon_migrate_path(old_path, cache);           // one-shot move from the old place
addon_cache_dir(host, cache, sizeof cache);    // <addon>/cache, created
```

Migration is rename-only and conservative: the new path is only taken when it
does not exist yet, and a failed rename leaves both paths alone for the next
start. **Order matters** — migrate *before* creating the cache directory, since
an existing destination is what "already migrated" means. Covers stay the
player's (`listart` in `.local/listart`), because every page draws from them and
they are not anybody's save; no addon may delete that directory.

Each addon then declares what it owns so the player can answer "is this file a
service's", export it over USB correctly and wipe only caches on a factory reset:

```json
"owns_paths": ["Addons/yandex/cache/", "Addons/yandex/saved.tsv"],
"cache_path": "Addons/yandex/cache/"
```

Paths are relative to the card root, no leading slash. `cache_path` is what a
factory reset empties; settings are never in it.

### The card is the slowest thing in the player

An addon that downloads a track and hands the player its path is writing tens of
megabytes to an SD card that this device's driver cannot always keep up with, on
a machine with about a megabyte of free RAM. That combination is not a
performance detail: it stalls the whole player, and it was measured on the
hardware (2026-11-06).

What the kernel says when it happens, from `dmesg`:

```
jzmmc_v1.2: timeout 3000ms op:25 w sz:37888   ... blks:19/74
jzmmc_v1.2: timeout 3000ms op:25 w sz:524288  ... blks:19/1024
mmcblk0: unknown error -5 sending read/write command, card status 0x900
end_request: I/O error, dev mmcblk0, sector 21762
FAT-fs (mmcblk0p1): FAT read failed (blocknr 19714)
```

and what the process looks like at the same moment, from
`/proc/<pid>/task/*/stack`:

- the **interface thread** is in `do_fsync → fat_file_fsync → filemap_write_and_wait_range`
  (it commits the library database, journal included);
- **`logwriter`** is in the same `fsync` (the log is on the card);
- **`playback`** is in `generic_file_aio_read → __lock_page_killable`, waiting for
  a page that is being written back;
- the **download thread** is in `balance_dirty_pages_ratelimited` — the kernel
  itself throttling the writer because too many pages are dirty.

`MemFree` sits around 1 MB and `Writeback` around 3 MB. Everything waits behind
the card, the interface included, and it reads as a freeze; on this device it
can last minutes and then recover on its own once the backlog drains, which is
why the same bug has been reported as "hangs after a few tracks" and as "the
player died with the panel dark".

What limits it is the board's path to the card (`jzmmc`), not the card: the same
card through a good USB reader is several times faster, and the player's own USB
export is slower still. So measure the ceiling **on the device** (`dd` to
`/mnt/sd_0` with `conv=fsync`) before choosing a pacing rate — a host card
reader's number says nothing about what an addon can write here.

The rule that follows:

- **Pace the writes.** A lossless track is 25–45 MB; arriving at network speed
  that is 2–4 MB/s of dirty pages, and that is what kills the card. 1 MB/s is
  enough: the download is a track ahead of playback, so a 40 s download costs
  nothing anybody hears. The reference implementation is `common/cardpace.h` /
  `common/cardpace.c` — `cardpace_reset()` before a download, then
  `cardpace_note(&d->pace, bytes_written_so_far)` after every write of track
  audio (never for covers). In `yandex/src/system/streaming/yandexcache.c` it is
  called from `note_written()` so every write path (raw, unwrapped and detached
  saves) is paced; qobuz, tidal, zvuk and subsonic call it from their own write
  choke points.
- **Never delete tens of megabytes in one go while something plays.** That is
  why the periodic "clear the whole cache every N tracks" sweep was removed
  (`YANDEXCACHE_CLEAR_EVERY 0`): the ceiling's `prune()` deletes one track's
  worth when the ceiling is actually reached, which is the same bytes spread
  over the tracks that make them. A user-requested clear or the shutdown clear
  is fine — nothing is playing then.
- **Do not add fsync loops.** One `fsync` per file at the end of a write is
  what the addons' stores do; an fsync per chunk, or a synchronous file write
  per second, is the same problem from the other side.

As of 2026-11-07 all six download caches pace: `yandexcache.c`,
`qobuzcache.c`, `tidalcache.c` (both the FILE and the DASH shape),
`zvukcache.c`, `subsoniccache.c` and piped's `pplay.c` (a whole muxed MP4, so
the largest writes in the tree). `spotify` fetches through its own Rust engine,
which is a separate job.

The rate is only half of it. A `dd` matrix on the device (2026-11-07, the table
is in `.opencode/NOTES.md`) shows the card does not stall on writes at all: 4
MB/s of unpaced writes with playback-rate reads produced no `jzmmc` timeout,
while a writer racing an *unthrottled* reader that reads in 512 KB requests
produced five. So pacing bounds the addon's share of the queue, and the rest of
the stall comes from the read side — decode readahead, thumbnails, a library
scan — which no cache can pace. Do not answer a freeze by lowering the rate
further without evidence: measure with `dmesg | grep -c "timeout 3000ms"`
before and after, the way that matrix did.

### Keys and sessions

A service's credentials are the addon's, and they live next to it on the card.
The player keeps none of them. `src/system/streaming/streamkeys.{c,h}` is down
to the two accounts the binary itself signs requests with — Podcast Index and
Last.fm, whose consumers (`podcast.c`, `lastfm.c`) are still part of the player
and have no other way in. Qobuz, Tidal, Zvuk, Yandex Music and Spotify are not
in that file, no getter for any of them is exported (`sonix-player/sonix-player/src/addon/exports.list` lost
the `streamkeys_qobuz_*`, `streamkeys_tidal_*` and `save_*` names when they
moved), and nothing about them is compiled in.

What a service addon keeps, then:

| file | what is in it |
|---|---|
| `<card>/Addons/<id>/settings.ini` | the application keys, the address of the API, the quality ceiling — through `common/store` |
| `<card>/Addons/<id>/session.ini` | the account: the token, the refresh token, its expiry, the display name |

The session has the store's shape and the store's write (tmp, flush, rename),
with two exceptions and a naming quirk, all of them in the addon's own header:

- **Tidal writes its own.** A Tidal token is an eight-to-nine-hundred character
  JWT and `store_t` holds `STORE_VALUE_MAX` (256) per value; widening that for
  one service would widen it in every addon's copy of the store. `tidalconf.c`
  writes the same `key = value` lines with the same tmp-then-rename step.
- **Spotify splits the token off when it does not fit**: a token under
  `STORE_VALUE_MAX` stays in `session.ini`, a longer one goes to
  `<card>/Addons/spotify/token.txt`, beside it.
- **Zvuk and Yandex keep the names the fork used** — `zvuk_session.ini`,
  `yandex_session.ini`, not `session.ini`. There they sat beside
  `device_config.ini`, and a card written before the move is easier to read when
  the file is called what it was called.

**The migration, and it runs once.** The owner had Qobuz and Tidal signed in
before the move, and the files that carried those sessions are still on the
card. On its first run an addon reads, in this order:

1. `<card>/sonix/streaming.json` — the file the user wrote, which may name the
   application pair as well;
2. `<card>/sonix/streaming-session.json` — the file the player wrote, and the
   newer of the two, so where both carry an account the player's wins;
3. `streamkeys_legacy_get(section, key, …)` — only for a pair `streaming.json`
   does not name. It is the one function `streamkeys.h` keeps for this, and it
   reads the `[qobuz]`/`[tidal]` section of `streaming-keys.ini` or the sealed
   `streaming-keys.bin` (which an addon cannot open by itself, and which is why
   the function is in the player rather than in the addon). After it, Qobuz
   falls back to the pair it derived from the web player the last time (the
   session file carries it) and Tidal to the pair built into `tidal.c`.

Then it writes its own two files and `migrated = 1` into `settings.ini`. The
marker is written even when nothing was found, and that is the point: a
sign-out must not be undone by the old files still sitting on the card. Nothing
is deleted from `sonix/` — a card that goes back to an older firmware, or to a
build of the player that still knows the old place, keeps working.

`streamkeys_legacy_get()` is temporary. It is exported, it has two callers
(`qobuzconf.c`, `tidalconf.c`), and it comes out — with them — one release after
the migration has shipped.

## Trust

An addon is native code from the card, and the player runs it with the player's
own privileges: it can read the card, the database, the settings, the
`streaming-keys.bin`, and it can talk to any host on the network. There is no
sandbox, no signature and no permission model, and there could not be one
without an interpreter and a process boundary the device does not have.

That is a property of the design, not an oversight: a card that carries an
addon is a card you trust, exactly as a card that carries a firmware image is.

## What is built, and what is not

The CI builds a whitelist, not everything that has an `addon.json`, and there is
one reason for it: an addon that has never run on a device can be broken in ways
no ABI check sees. The list is `ADDON_IDS` in `.github/workflows/addons.yml`, and
this is the state behind it:

| addon | built in CI | why |
|---|---|---|
| `subsonic`, `yandex`, `qobuz` | yes | used on a device; the pages, the sign-in and the caches were checked there |
| `gearboy`, `flappybird`, `dino` | yes | games, checked on a device |
| `hello` | yes | the example: it is the cheap compile that proves the SDK still compiles |
| `zvuk` | no | ported, never checked on a device — a state screen stands in for its page |
| `tidal` | no | the device-code sign-in and playback were never checked on a device; `tidal/README.md` says what is unverified |
| `spotify` | no | the C half builds, but the addon means nothing without its Rust engine, and the engine's session was never checked on a device |
| `piped` | no | checked in the simulator and on a device against a live Piped instance; what it depends on is a third party (see `### piped`) |

A plain `make` still builds all of them, and that is deliberate: a developer's
machine is where the unverified ones are supposed to be tried. What the whitelist
keeps out is the artifact — the folder `make pack` produces for somebody's card.

## The addons in this tree

### subsonic

`subsonic/` – the user's own library on their own server.

A port of the client from `Rekkod-Tree/sonix-player-rekkod-mod` (GPLv3, as this
tree), with the parts that other tree's player provided removed: the provider
registry, the download cache, and the favourites sync.

**Several servers.** Every account is one file, `<card>/Addons/subsonic/servers/
<slug>.ini` (server, user, salt, token, password, quality, label), and
`servers/active` names the one in use; the addon's home screen has a `Server`
row that opens a server list — switch, add, remove — and a `Quality` row that
changes the active server's format. A server's token is never touched by a
switch or by signing out of another. On the first start of this version an older
`session.ini` is renamed to `servers/default.ini`, so nothing is typed twice.
The slug is the server's host, sanitised and made unique; a sign-in that matches
an existing server and user reuses that file. On a card that refuses writes the
session still works for the run and the page says it will not be kept.

What works: token sign-in (`t = md5(password + salt)`, with the plain-password
fallback for servers that refuse tokens), artists → albums → tracks, newest,
random, playlists, search. Lists are capped at 100 entries and are not paged.

**Playback goes through the queue, not `radio_play`.** A tap on a track starts
the whole visible list: `device_state_play_list(paths, n, tapped)`, so shuffle,
repeat, the queue page, `Play next` and `Add to queue` are the player's own.
Each track is fetched into `<card>/Addons/subsonic/cache/<key>.<suffix>` by
`subsoniccache.c`, which wraps the download in `growfile_announce/progress/
finish`, so the decoder opens the file long before it is complete (measured:
opened at 512 KB of an 800 KB track, 11 s before the last byte), takes the
shared `streamturn` turn and registers an abandon hook. `prepare_track`, handed
to `device_state_add_prepare_cb`, is where the download for the *next* track
starts — the player calls it with the path it is about to open. The
`sonix_addon_service_entry` table (`subsonicservice.c`) is what makes the star,
the sleep timer, the USB export and a factory reset know those files: `cache_dir`,
`owns_path`, `track_id`, `downloading`, `network_wanted`, `clear_on_exit`,
`wipe`, `fav_*`, `album_id`, `open_album`.

What does not: cover art in the list rows (the sheet's cover comes from the
cached file), scrobbling (the client has the calls; the pages do not offer them
yet), a badge over the artwork (this player's icon set has no Subsonic mark),
and "Add to playlist" (no `playlists_*` in the service table).

### hello

The example. A screen with three rows saying which folder it was loaded from and
what screen size it was given. Delete it from a card that does not need it;
`make pack` leaves it out by default.

### gearboy

`gearboy/` – the Game Boy and Game Boy Color emulator, from `src/gb`
(Gearboy's core and its blargg APU) to the cartridges the card carries.

It used to be a page of the binary, under `-DGB_CORE` so that a build without it
left the code out, and it was the only C++ in the tree: the player linked its own
copy of libstdc++ for it. Now `src/` has no C++ at all and the player's link goes
through the C driver (`Makefile`, `APP_CXX_SRCS`); the emulator's runtime travels
inside the addon.

What the addon had to take over from the player:

- `gbdb_set_root()` – where `Games/GB` and `Games/GBC` are. `main.c` and
  `system.c` used to call it; now `init()` gets the same path from `host->sd_root`
  and the cartridge database rescans every time the list is opened, so a card
  that came or went needs nothing more;
- the three screens (`gearboypage_screen`, `gearboyplay_screen`,
  `gearboysettings_screen`) are created in `init()` instead of at start-up, which
  is the whole point: a player who never opens a game no longer pays for the core,
  the database and the emulator's interface;
- `panel_touch_device()` / `panel_touch_enable()` / `display_get_rotated()` – it
  takes the touch panel over for multitouch while a game runs (PATCHES.md). They
  live in the player's `main.c` and are exported like any other symbol an addon
  asks for;
- the player's sleep timer and power manager used to ask `gearboy_running()`; both
  ask `audio_external_active()` now (see "Sound the player does not own");

Everything the emulator remembers is on the card, and none of it is in the
player's configuration:

- `Games/GB`, `Games/GBC` — the ROMs, the user's own;
- `Games/Saves` — battery-backed cartridge RAM (`.sav`) and save states
  (`.state1`), beside the ROMs rather than inside their folders, so they can be
  copied off in one go;
- `Games/Bios` — the optional boot ROMs (`dmg_boot.bin`, `cgb_boot.bin`);
- `<card>/Addons/gearboy/settings.ini` — palette, shader, colour correction and
  the boot ROM switch (`gbconf.h`, through `common/store`). They were `[gearboy]`
  keys in `device_config.ini` while the emulator was compiled in; an addon's
  settings belong next to the addon, and a card taken to another player takes its
  palette with it. A key the file does not have yet is read from the old section
  once, so a card written before the move keeps its choice.

### dino

`dino/` – the Chrome offline game: a tap to jump, a finger held down to
duck, the further you get the faster it goes, and every hundred points the sky
changes hands (day is paper, night is blue-black, with stars and a moon).

It is not from this tree. It comes from `mr-f0xx/sonix-player-tempotec-v1`
(GPL-3.0), PR #56 "Three games for the More menu", and that repository reverted
it a few hours later — "the V1 freezes when the theme is changed, on the builds
from the games work onward" — so it is only reachable through the merge commit
`ef2da1a`. It is a page of that tree, compiled in; here it is a card addon, which
is how a feature that was withdrawn upstream can still exist.

What the move changed, and it is all in one block at the top of
`src/gui/games/games.c`:

- `bp_pick(v1, hi)` → `SC(hi)`. Their tree had two boards (240x320 and 480x720)
  and picked a length for each; this interface is written at 480x720 and
  compiled at the panel's own, so the second number is the one that matters.
  `bp_is_tempotec_v1()` is 0, their fonts are our `font_ui_*`, their card radius
  is `SC(12)`;
- the best score moved out of `device_config.ini` (`[dino] best`) into
  `<card>/Addons/dino/settings.ini` (`gstate.h`), with the old section read once
  so a high score survives;
- the score line became one row: their stacked caption-over-number needs the
  480-pixel panel's 32-pixel band, and at 26 pixels on this one the caption was
  cut in half.
- `tr()` → literals. The keys (`dino`, `dino_hint`, `game_over`, …) are in the
  V1 port's language files, not in ours, and an addon's strings are its own
  (see "Languages").

The game brings no artwork and no sound: every sprite is a set of coloured
rectangles moved by a 16 ms timer, and `games.h` says why a game here must not
touch the audio — a run leaves the music playing. The tile's icon is their
`game-dino.svg` with `currentColor` rendered as the accent blue, 989 bytes.

**The freeze they reverted it for was looked for and does not happen here**:
theme switched dark → light → dark with the page already built, several times,
in the simulator, and the page recolours through `theme_register_refresh` without
hanging. Whoever changes `games.c` should repeat that before believing anything
else.

### flappybird

`flappybird/` – the game from the More page.

It is a page, not a service: no card, no network, a timer and a frame buffer.
The move was mechanical, with two things worth knowing:

- `init()` runs **once**, so the game cannot be loaded there and forgotten. The
  artwork, the frame and the sounds are freed when the screen unloads
  (`screen_unloaded_cb`) and loaded again on the next `SCREEN_LOADED`
  (`game_begin()`); `flappybird_open()` is that plus the `switch_screen()`, and
  the addon's `init()` uses it to tell the shell whether the artwork is there at
  all;
- the best score and the sound switch are `<card>/Addons/flappybird/state.ini`
  (`fstate.h`, through `common/store`), for the same reason as Gearboy's
  settings: they used to be `[flappybird]` keys in the player's configuration;
- the artwork is the addon's own, in `flappybird/assets` (332 KB of PNG
  and OGG, identical for every board — they used to be three copies under
  `sonix-packer/assets/<board>/usr/resource/sonix/gui/flappybird`, and are one
  folder in the addon now). `make pack` copies the folder along, and the code
  reads it through `flappybird_assets_dir()`, set from `host->dir` in `init()`;
  the resource tree in the image is only the fallback for a card that carries
  `addon.so` and nothing else.

### zvuk

`zvuk/` – Zvuk (zvuk.com), the first of the two services the rekkod-mod
tree carried, and — like Qobuz and Tidal — a **service** in the manifest's sense
as well as an app: the manifest says `"service": true` with `owns_paths` and
`cache_path`, and the library exports two entry points instead of one:

```c
const sonix_addon_t *sonix_addon_entry(uint32_t api);          // the page
const sonix_addon_service_t *sonix_addon_service_entry(uint32_t api);  // the account
```

The first is the page, and `init()` returns its screen as everywhere else. The
second is the table the player asks about files it did not create
(`src/addon/addon_service.h`): whose path this is, which track of the service
it is, is a download running, may the device sleep, what the star writes to,
which album "Show album" opens. It is filled in by `zvukservice.c`, one line per
hook over the modules below, and registered as soon as the library is open.

Six modules of the fork's binary came along, keeping their paths
(`<id>/src/...`, "Building"), so not one `#include` was rewritten:
`zvuk.c` (the API, GraphQL behind the web player's headers, and the session),
`zvukcache.c` (the download cache the player plays from), `zvukdl.c` (saving
into the library), `zvukplay.c` (the queue and the fetch-ahead: a queued track
is downloaded when playback gets near it), `zvuklist.c` (a long list's ids and
their pages) and `zvuksync.c` (the writes). The page is `zvukpage.c`, the same
page ported off Graphite 2 onto this player's widgets ("Porting a service
page").

What it keeps on the card, and none of it is the player's:

- `<card>/Addons/zvuk/settings.ini` — `base_url`, `user_agent`, `quality`,
  `check_track` (`zvukconf.c`, through `common/store`). They were `[zvuk]` keys
  of `device_config.ini` in the fork; a key the addon's file does not have yet
  is read from there once, and nothing is ever written back.
- `<card>/Addons/zvuk/zvuk_session.ini` — the token, the account's name, the
  plan and its expiry (`zvuk.c`).
- `Addons/zvuk/token.txt` — the user's token, in the addon's own folder, read by
  the addon itself. Zvuk has no sign-in an outside application can do: the site
  signs in with a phone number and an SMS behind an anti-bot page, there is no
  device flow and no refresh token, so what every open client uses is the web
  session's own token, copied out of `zvuk.com/api/tiny/profile`. There are no
  application keys at all — nothing of Zvuk's is compiled in, and nothing comes
  out of `streamkeys`.
- `Addons/zvuk/cache/` — the cache it downloads into (the manifest's
  `cache_path`, so a factory reset empties it and the card watcher knows whose
  it is), `Addons/zvuk/saved.tsv` — the index of what was saved into the
  library. Covers are the player's shared `.local/listart/`, not the addon's.

**Without a token the page is the sign-in screen, and nothing on it makes a
network request.** The home says where the file goes and whether it is already
there, and offers one row — "Sign in from the card" — which starts a worker and
reads the file; the session in `zvuk_session.ini` is what makes the difference.
Signing in is one detached worker over `zvuk_sign_in()`, whose answer comes back
through `gui_post()` ("Threads").

Playback is not `radio_play()`: a track is fetched into `Addons/zvuk/cache/` by
`zvukcache.c` and the queue is handed to the player as a list of files
(`device_state_play_list()`), which is what a service with a cache needs and what
the fork did. `radio_play()` is for a stream an addon hands over; a cache is for
what it downloaded first, and it is the addon's own way into the audio path.

What is **not** here, compared with the fork, and it is worth knowing before
believing a screenshot:

- **no ranged requests.** `http_stream_open_range()` and `http_stream_resume()`
  do not exist in this player's `http.h`, so the cache has no ranged windows and
  no resume after a dropped connection: the file is fetched again from the start
  and `src_seek()` refuses to jump ("the source" in `zvukcache.c`);
- **saving books and podcasts is a stub.** The fork's book and podcast importers
  (`bookdownload.c`, `podcastimport.c`, 665 lines and a card-pin protocol) are
  not carried; `zvukdl_book()`, `zvukdl_episode()` and the spoken-content branch
  answer "not available" and say why in the log. Tracks and albums are saved;
- **no queue actions.** "Play next" and "add to the end" belong to the fork's
  player (`svcui_queue`, `playlist_queue_where_t`); this player has no public
  "add these tracks to the queue" path for a service, so a tap on a row plays it
  and the menu holds like and save-to-the-card.
- **no `playbackplace`**: there is no such module here, so a list is not
  reopened where a track left off;
- **no badge on the artwork**: the service table has the hook, but this player's
  icon set has no Zvuk badge, so it is filled with `NULL` and the sleeve stays
  as the page drew it;
- **no `cardimport`** (the fork's scanner for files at the card's root): the
  token file is read by the page, and there is nothing else to import;
- **no `streamerr`**: a failure is one sentence in the list's empty label and a
  toast.

The tile is `order 3`; the manifest's `order` is what puts it there.

### yandex

`yandex/` – Yandex Music, the second service from that tree, built the
same way: `order 4`, `"service": true`, `owns_paths` for
`Addons/yandex/cache/` and `Addons/yandex/saved.tsv`,
`cache_path` for the cache, and the two entry points.

Eight modules came along (`yandex.c`, `yandexcache.c`, `yandexdl.c`,
`yandexplay.c`, `yandexlist.c`, `yandexsaved.c`, `yandexsync.c`, `yandexwave.c`)
plus two files that are not the fork's streaming code at all and are the reason
this addon links `-ldl`:

- `src/system/net/crypto.c` and `src/system/net/openssl_runtime.c` — the fork's
  AES. Yandex's "encraw" files are AES-256-CTR and this player has no AES
  anywhere, so the addon carries its own: libcrypto is `dlopen()`ed (the cross
  toolchain has no headers for it), both key sizes are checked against the NIST
  SP 800-38A vectors before they are trusted, and a rootfs whose libcrypto
  cannot do it reports itself unavailable instead of playing noise. Neither file
  is in the player's tree, and neither needs to be.

What it keeps on the card:

- `<card>/Addons/yandex/settings.ini` — the four keys (`client_id`,
  `client_secret`, `sign_key`, `client_header`), `base_url`, `auth_url` and
  `quality` (`yandexconf.c`, through `common/store`). In the fork the settings
  were `[yandex]` keys of `device_config.ini` and the four keys were sealed into
  `streaming-keys.bin` with a key compiled into the binary — an addon can open
  neither, so both live here. The values are the owner's; nothing is compiled in
  and nothing is guessed. A file that does not carry all four is **"no keys"**,
  which is a state the screen shows rather than a failure: `yandex_configured()`
  is false, the service is off, and the sign-in row says why.
- `<card>/Addons/yandex/yandex_session.ini` — `access_token`, `refresh_token`,
  `expires_at`, `uid`, `user` (`yandex.c`).
- `Addons/yandex/token.txt` — the token the user pastes, in the addon's folder,
  card, read by the addon. A whole `Authorization: OAuth …` line is fine;
  `yandex_token_clean()` takes the token out of it.
- `Addons/yandex/cache/`, `Addons/yandex/saved.tsv` — the
  same three as Zvuk's, named for this service.

**Where the four keys come from.** The Music API has no developer program: they
are the credentials of the official client, which is why the fork sealed them.
Nothing is compiled in and the tree carries only `yandex/settings.ini.example`
— an example, deliberately named so `make pack`'s `*.ini` glob does not put a
keyless `settings.ini` on a card. The published place to read the values is
`MarshalX/yandex-music-api`, which talks to the same API: `client_id` and
`client_secret` are the Android application's defaults in
`yandex_music/_client/device_auth.py`, and `sign_key` with `client_header` are
`DESKTOP_SIGN_KEY` and `DESKTOP_CLIENT` in `yandex_music/utils/sign_request.py`
(`DEFAULT_SIGN_KEY` there is the Android one). Two rules:

- **`sign_key` and `client_header` are a pair.** The library says so in as many
  words: the signature of `get-file-info` is checked together with the
  `X-Yandex-Music-Client` header, and a key from one client beside the other
  client's header is refused. That library signs `get-file-info` with the
  desktop pair (`_client/tracks.py`), and the addon's signature is the same
  construction (`yandex.c`, `file_info_once()`: `ts + trackId + quality + the
  codecs without their commas + transport`, HMAC-SHA256, base64 without the
  trailing `=`), so the desktop pair is the one that fits.
- **`client_header` carries a version and it ages.** The header is sent with
  every request and the pair is what the service knows; when playback starts
  being refused, the version in it is the first thing to bump, not the key.

`client_id`/`client_secret` play no part in playback. They are asked for two
grants and nothing else: the device code's (`grant_type=device_code`, which is
how the screen signs in) and the refresh (`grant_type=refresh_token`, once the
session has a refresh token and comes within 30 days of its expiry). A session
imported from the card's token file has neither — `expires_at = 0`, no refresh
token — so for that one the pair only has to be non-empty for
`yandex_configured()`.

**The page is a state screen, and it signs in, and it plays.**
`src/gui/streaming/yandexstate.c` shows the keys (present / no keys), the
account, whether the token file is on the card, and the quality. The Quality row
cycles through Lossless, High and Normal; seven actions sit under it — play My
Wave, play the liked tracks, sign in with a code, stop waiting, sign in from the
card, read again from the card, sign out. Opening it touches no network, and
every request is a detached worker posted back with `gui_post()`.

The two playback actions are the smallest caller of `yandexplay.c` there is, and
they stand in for the unported page rather than beside it:

- **My Wave** — the worker calls `yandex_wave_start()`, the batch becomes a
  `yandexlist_t` (`yandexlist_add_track()` each), and `yandexplay_list(list, 0,
  true)` makes it the queue. The wave's own machinery then takes over inside
  `play_queue_from()`: `yandexwave_begin()` with `yandexplay.c`'s batch callback,
  and every later batch is appended by `yandexwave.c` on its own thread.
- **Liked tracks** — `yandex_liked_ids()` with a callback that adds each id to
  the same kind of list, then `yandexplay_list(list, 0, false)`: ids only, so a
  589-track queue is 8 bytes a track and the details are fetched when playback
  reaches them.

Both build the list **on the worker** (`yandexlist_*` is thread-safe) and start
it on the GUI thread, because `yandexplay_list()` is a GUI-thread call. One
`lv_timer` at 1 s runs `yandexplay_watch()` — fetching ahead, the cache's
ceiling, the wave's next batch and its "track started" feedback. It is created
with the page and never deleted with it: leaving the screen does not stop the
music, and the wave would run dry without it. `yandexplay_signed_out()` on
sign-out ends the wave and forgets the queue.

Without Plus the station still answers and My Wave plays: every track comes back
`aac-mp4 192 kbps` and the log says `account has Plus: no (lossless needs it)`.
Lossless over `encraw` is the addon's own AES path through `dlopen(libcrypto)`.

Two ways in, and the first one is the one to use:

- **a code (OAuth device flow).** `yandex_login_begin()` asks `oauth.yandex.ru`
  for a device code, the screen shows the eight characters and `ya.ru/device`,
  the user types them on a phone, and `yandex_login_poll()` is asked every
  `interval_secs` (five) until the account answers — `take_tokens()` saves the
  session and `fetch_account()` proves it, so a sign-in leaves the card with a
  real session and a refresh token. The polls are paced by an LVGL timer on the
  GUI thread, one request in flight at a time (`login_polling`), and they stop
  at `expires_secs`, on "stop waiting", on sign-out and on
  `LV_EVENT_SCREEN_UNLOADED` — a page left open would otherwise poll a code
  nobody is looking at, and eventually sign in behind the user's back. A poll
  that answers OK after the page stopped waiting still counts: the tokens are
  already written and the account is signed in.
- **a token from the card** (`Addons/yandex/token.txt`), which is what the stage
  had before the screen could drive the flow. `yandex_import_token()` spends one
  `/account/status` and keeps nothing if the token is refused. This path stores
  the token with `expires_at = 0` and no refresh token, so it never renews; the
  device flow's session does.

The full page — `yandexpage.c`, 2 693 lines on Graphite 2 — is **not** ported,
and the service table's two GUI hooks (the badge over the artwork, "show album")
are `NULL` until it is. What that costs today is the *browsing*: there are no
playlist rows, no album pages and no track menus, so the two actions above are
the only ways into the music — but they are real ones, the same `yandexplay.c`
the page would have used.

The limitations are Zvuk's, and one more of its own: the fork's player had a
handful of functions this one does not, and `yandexcompat.{h,c}` writes every
one of them out with what the substitution costs — `playlist_replace_path()`
(the queue is rebuilt through `playlist_load_paths()`, which drops its appended
bookkeeping and deals a shuffle again), `device_state_note_metadata_available()`
(tags appear at the next track), `sleeptimer_expiration_serial()` (no serial),
`storage_pin_card()` (an fd on the card root instead of a mount pin),
`http_set_thread_timeout_cap()`, `gui_post_or_free()`, and three `mp4flac`
helpers. The ranged requests are missing here too, so there is no resume.

### spotify

`spotify/` – `order 9`, `"service": true`, and the only addon in this
tree whose other half is a **separate process**:

```
<card>/Addons/spotify/
  addon.so                          the interface: spotify/addon.c, C + LVGL
  assets/spotify-engine-mips        the engine: spotify/engine-mips/, Rust
  credentials.ini                   the account: an access token (see below)
  state/                            librespot's cache, so the token is needed once
```

The engine is **librespot**, tokio and rustls built as a static
`mipsel-unknown-linux-musl` binary and spoken to over a Unix socket: a `cdylib`
cannot be static, and Rust's `mipsel-unknown-linux-gnu` std wants glibc 2.17
while the device has 2.16 — a musl child process sidesteps the device's libc
entirely. `engine-mips/README.md` is the build recipe (the pinned nightly, the
`vergen` pin, `-soft-float`) and the reasons behind each flag.

One request line per connection, one response line: `probe`, `status`, `login`,
`logout`, `track`, `playlists`, `playlist`, `album`, `liked`, `search`, `stream`
(audio goes back over the same socket; Sonix keeps owning decoding, the queue,
DSP and the DAC). The addon spawns the engine, watches it and prints everything
it says as `spotify: <line>`; no `lv_*` is ever reached from the engine's thread.

**There is no password form, on purpose.** Spotify has withdrawn the
username/password flow for the client librespot imitates, so `Session::connect`
with a pair answers `LoginFailed` on a real account (`engine-mips/src/session.rs`
says so at the top). The account is therefore an **OAuth access token**:

```
<card>/Addons/spotify/credentials.ini
  # access_token = BQC...
```

The addon never reads that file — it writes the template once and points the
engine at it (`--credentials`) — and after the first successful login librespot
caches the session in `state/credentials.json`, so the token is needed once.
`Sign in from the card` sends `logout` (which drops the cache) and then `login`,
so replacing the token in the file and tapping it again is the whole update
procedure. The page shows `Account`, `Premium` and the token file's path.

**An engine restart does not sign the user out.** The session lives in the
engine process, and the addon stops that process when the page is left (about
40 MB of RSS is not something to keep on this device). The first `status` a new
engine answers is therefore followed by one `login`, which reads
`state/credentials.json`; when the cached token has gone stale — an hour is the
usual life — the addon drops the cache and signs in from `credentials.ini`
instead. A catalogue request answered `not_signed_in` (a session that died
under it) is retried once behind the same login, rather than shown to a user who
is signed in.

**A free account browses, and cannot play.** librespot 0.8 refuses a non-Premium
account by ending the whole process (`Session::check_catalogue`, "TODO: logout
instead of exiting"), and in this engine the process *is* the session: the login
answered `signed_in` and a second later every request answered `not_signed_in`.
`engine-mips/vendor/librespot-core` is that one function patched (the diff is
`vendor/librespot-core.patch`, `tools/vendor-librespot-core.sh` re-creates the
copy after a version bump; upstream's own PR for it is
librespot-org/librespot#1855), so the session survives: the page reports
`Premium: no`, the catalog browses, and a tap on a track says `Spotify Premium is
required to play` without starting a stream. Playback is not something the client
can grant: Spotify refuses the **audio key**, so the file stays ciphertext.
`StderrLogger` in `engine-mips/src/main.rs` is what makes librespot's own
sentences reach the log at all.

**The catalog is Spotify's Web API, not librespot's metadata.** librespot 0.8
serves metadata through `spclient`, which asks `login5` for a bearer token, and
`login5` only accepts a *stored credential* -- the blob an old
username/password login produced. The access token a user copies out of the web
player answers `Login request was denied: INVALID_CREDENTIALS` there, for a free
account and a Premium one alike (measured 2026-11-03; the same token is accepted
by `api.spotify.com`, which is the 401-for-bogus / 429-for-valid distinction).
So `engine-mips/src/catalog.rs` speaks `/v1/search`, `/v1/me/playlists`,
`/v1/playlists/{id}/tracks`, `/v1/albums/{id}/tracks` and `/v1/tracks/{id}`, with
librespot's own `HttpClient` and the session's token. Two consequences the page
knows about: a browser token lasts about an hour, after which every list says
`the access_token has expired` (paste a new one and tap the sign-in row), and
`SONIX_SPOTIFY_API` can point the engine at a local mock for testing.

The engine also answers its own sentences for a credentials file: a bare token
line is accepted as a token, `no access_token in credentials.ini` when there is
none, `Spotify no longer accepts a username and password: put an access_token
in credentials.ini` for the withdrawn pair, and `the access_token was refused
(it may have expired)` for librespot's `Bad credentials`.

**Signing in: the user's own Spotify application.** A token copied out of the web
player is a *first-party* client's token, and since 2025-12-15 Spotify answers
`429 API rate limit exceeded` to those on `api.spotify.com` — for a free account
and a Premium one alike (measured 2026-11-04: 401 for a bogus token, 429 for a
valid one, on two networks, `retry-after` ~40 s and no recovery). The way that
works is an application the user registers:

1. <https://developer.spotify.com/dashboard> → *Create app*; its *Redirect URI*
   must be exactly `http://127.0.0.1:5588/login` (Spotify allows plain `http`
   only for loopback). The Web API is enabled by default.
2. `client_id = ...` in `<card>/Addons/spotify/credentials.ini`; the client
   secret is not used — the flow is PKCE.
3. `Sign in with Spotify` on the page: the engine makes a verifier, a SHA-256
   challenge and a state, and writes the authorization URL to
   `<card>/Addons/spotify/oauth-url.txt` (a URL of that length cannot be shown
   on, let alone typed into, a 320-pixel page).
4. Opening it anywhere and authorizing leaves the browser on
   `http://127.0.0.1:5588/login?code=...` — which reaches no player, so the whole
   redirect URL (or just the code) goes into
   `<card>/Addons/spotify/oauth-code.txt`. The engine picks the file up within
   five seconds of the tick, exchanges the code, and `Finish sign-in` is there for
   when it does not.

The exchange stores `{"access","refresh","expires_at"}` in
`<card>/Addons/spotify/state/oauth.json` (0600). The access token is refreshed a
minute before it expires through `accounts.spotify.com/api/token`; the refresh
token is what keeps the card signed in, and a refresh the service refuses clears
the file and says so rather than falling back to another account. Authorizing
also deletes `state/credentials.json`: that is librespot's cache of whoever
signed in last, and an authorization names the account this card is for — the
same reason the pasted `access_token` is ignored once an authorization exists.
`Sign out` clears both.

The engine also answers its own sentences for a credentials file: a bare token
line is accepted as a token, `no access_token in credentials.ini` when there is
none, `Spotify no longer accepts a username and password: put an access_token
in credentials.ini` for the withdrawn pair, and `the access_token was refused
(it may have expired)` for librespot's `Bad credentials`. The OAuth half answers
`no_client_id`, `no_code_file`, `no_code_in_file`, `no_login_in_progress` and
`oauth_refused`, and the page turns each into a sentence.

### piped

`piped/` – audio from YouTube, as a client of a Piped instance.

The tile keeps the YouTube mark, and that is deliberate: what the addon plays is
YouTube's catalogue, and the tile should say what will be heard rather than which
relay carries it.

**Why an instance and not YouTube.** Measured on 2026-11-05 from the player's
own network: `www.youtube.com`, `music.youtube.com` and `youtubei.googleapis.com`
complete the TCP connection and then say nothing for fifteen seconds, and every
`googlevideo.com` host — the media itself — does the same. So there is nothing
to extract on the device: a Piped instance ([TeamPiped](https://github.com/TeamPiped/Piped))
reaches YouTube from its own address and hands back a media URL on its own
`/videoplayback` proxy, which is reachable. Two instances answered that day:
`api.piped.private.coffee` and `pipedapi.ducks.party`. The other fifteen from the
projects' public lists were dead, unresolvable or 502.

**What the instances serve today.** `/search?q=&filter=music_songs` is complete:
title, id, duration, uploader, proxied thumbnail. `/suggestions` and `/config`
work. `/streams/{id}` works and carries `relatedStreams` (20) — but
`audioStreams` is empty on both, and only **itag 18** (muxed MP4 with an AAC
track, `moov` ahead of `mdat`) is offered; the player has no WebM demuxer, so
Opus-only formats would be useless anyway. `/playlists/{id}` and `/channel/{id}`
answer 200 with `nextpage: null` and an empty `relatedStreams`: the extractor
behind them is broken, so the addon does not offer playlists or channels at all
rather than open empty pages.

**The client** (`src/system/gw/pgw.c`) keeps a built-in list and an optional
pinned instance in `Addons/piped/settings.ini` (`common/store`), probes with
`/config`, fails over on every call, and remembers 404 and 500 separately: "the
video is gone", "YouTube blocked the instance" and "nothing answered" are three
different sentences.

**Playback** goes through the queue, like Zvuk's: a track is one more `/streams`
call and a download into `<card>/Addons/piped/cache/<videoId>.mp4`, announced
with `growfile_announce/progress/finish` so the decoder opens it while it is
still arriving, guarded by a `<videoId>.mp4.part` marker so a download cut by a
card pull is never mistaken for a complete track. `<videoId>.mp4.tags` carries
the title and the artist (`metadata.c` reads that sidecar), and `<videoId>.jpg`
the cover — only when the instance's image proxy hands back JPEG, PNG or GIF,
because it answers WebP and the player decodes neither WebP nor AVIF.
`device_state_add_prepare_cb` fetches the next track, and the
`sonix_addon_service_entry` table makes the star, the sleep timer and a factory
reset know the cache.

**What it does not do:** video, playlists, channels, history, an account, or
lifting the instance's IP block. That last one is not a bug here: the instances
answer 500 with
`SignInConfirmNotBotException: YouTube probably temporarily blocked anonymous access from this IP`
for every video they have not cached, and the addon says "The instance could not
get this from YouTube". It passes when YouTube lets them through again.

## Covers on the rows of a list

A service's list is a column of rows with a small square sleeve on each. The
sleeves are URLs, and downloading one is the player's job: `listart` keeps a
pool of twelve slots, one owner at a time, a worker that downloads and decodes,
and a cache on the card named by the MD5 of the URL. What `listart` does not
know is what a *row* is.

`common/listrow.{h,c}` is that piece, and it is what a ported page should
use instead of carrying its own copy of the bookkeeping (`tidalpage.c` and
`qobuzpage.c` each have one, written before this existed). `zvukpage.c` is the
first page on it, and the sample below is its shape; Yandex's page is next when
it is ported. A page's side of it is all of this:

```c
static const char zvuk_art_owner;        // the page's token; only its address matters

listrow_bind(&zvuk_art_owner, row_image, index, cover_url);
listrow_visible(&zvuk_art_owner, first, last);   // after filling, and on every scroll
listrow_reset(&zvuk_art_owner);                  // a new page, or a reload
```

- **The row is an `lv_image` the page created and laid out.** The module sets
  its source when the cover arrives, clears it when the picture goes, and
  touches nothing else: size, position, radius and the placeholder before the
  first cover are the page's. The decode size comes from the object's width.
- **Only the visible range is requested.** `listrow_visible()` is the scroll
  hook; a row that leaves the range gives its slot back at once so the slot goes
  to a row on screen, and its decoded picture is freed once it is more than a
  few rows away. Coming back is a read of the card's cache, not a download.
- **A row that is reused for another URL cannot receive the old picture.** Every
  row carries a generation; the slot remembers the generation it was asked for,
  and an answer from a superseded request is thrown away rather than drawn.
- **A deleted row gives its slot back by itself**: `listrow_bind()` installs the
  `LV_EVENT_DELETE` handler. A slot left behind by a dead row is otherwise never
  collected and starves the rows that are visible.
- **Everything runs on the UI thread**, including the collecting timer (LVGL,
  120 ms, paused when nothing is in flight). The download happens on `listart`'s
  worker, so no `lv_*` is ever called from another thread.
- **`[streaming] list_art`, on unless the card sets it to 0.** There is no page
  for it in the interface, and while it defaulted to off no addon's list ever
  had a cover: the rows showed their placeholders and nothing was ever asked
  for. Off, `listrow_bind()` records nothing, no timer is created and nothing is
  allocated, so a page can still call it unconditionally.

`listart` and `cover` were not in `sonix-player/sonix-player/src/addon/exports.list` when this was written:
`exports-base.sh` did not walk `gui/streaming` and `gui/nowplaying`, so they were
exported only because the qobuz addon called them (the second half of the export
list is built from the addons' own undefined symbols). That made a card-only
addon built against a player without those addons fail to open. Both directories
are in the script now: 151 more names, and the image grew by **4 KB** (7 061 460 →
7 065 556 bytes) — nearly all of it was already reachable, so the usual "an
exported symbol is a root for --gc-sections" price did not apply here.

## Porting a service page (Yandex Music, Zvuk)

The rekkod-mod tree carried both, and both arrived this way. What it took, from
reading it:

| piece | size there | what happened here |
|---|---|---|
| `system/streaming/yandex.c`, `zvuk.c` | 85 KB, 69 KB | the API client came as it was: mostly self-contained (http, json, its own token refresh) |
| `yandexplay.c`, `zvukplay.c` | 34 KB, 23 KB | playback goes through their **download cache** on the card (`Addons/<service>/cache/`) and then `audio_play(file)`; that is a second way into the audio path, not `radio_play`, and it stayed inside the addon |
| `yandexdl.c`, `zvukdl.c`, `*cache.c` | ~120 KB | the download manager, the cache index and the card watcher came along too — they are the addon's own now, and the player only asks the service table about them |
| `yandexpage.c`, `zvukpage.c` | 97 KB, 72 KB | Zvuk's page is ported onto this tree's rows; Yandex's is not yet (a state screen stands in). That tree's `svclist`/`svcui`/`streamerr` helpers were replaced or dropped |
| `streamkeys` | – | that tree's application keys are sealed with a key compiled into the player; an addon cannot open it, so each addon keeps its own keys in its own file, and the account is a token the user puts on the card |

The order that turned out to work: the API client first, against a mock
(`tools/zvuk_mock.py`); then the page on this tree's rows; the download cache
inside the addon, because this player's `http.h` has no ranged request and a
public "cache this URL and play the file" path would have been a change to the
player for one service's sake.

### The services, at a glance

Five tiles on the Streaming page are services now, and every one of them keeps
its own keys — none of these names is in `streamkeys` and none of them is
compiled in:

| addon | order | what it keeps on the card | its own keys |
|---|---|---|---|
| `tidal` | 1 | `Addons/tidal/settings.ini`, `Addons/tidal/session.ini` (its own writer, token > 256), `Addons/tidal/cache/` | `client_id`, `client_secret`; the pair built into `tidal.c` is the fallback |
| `qobuz` | 2 | `Addons/qobuz/settings.ini`, `Addons/qobuz/session.ini`, `Addons/qobuz/token.txt`, `Addons/qobuz/cache/` | `app_id`, `app_secret` — named, from the old keys file, or derived from the web player; a token alone is not enough |
| `zvuk` | 3 | `Addons/zvuk/settings.ini`, `Addons/zvuk/zvuk_session.ini`, `Addons/zvuk/cache/`, `Addons/zvuk/saved.tsv` | none at all: `Addons/zvuk/token.txt` is the whole of it |
| `yandex` | 4 | `Addons/yandex/settings.ini`, `Addons/yandex/yandex_session.ini`, `Addons/yandex/cache/`, `Addons/yandex/saved.tsv` | `client_id`, `client_secret`, `sign_key`, `client_header`; all four or the service is off |
| `spotify` | 9 | `Addons/spotify/credentials.ini`, `Addons/spotify/state/`, `assets/spotify-engine-mips` | none: the account is an OAuth access token, and the engine is librespot as a static musl process |

The order numbers are the tiles' places on the page, which is what `order` in
the manifest means; they are not alphabetical and not the order the addons
arrived in. `subsonic` (no `order`) lands after the built-in tiles, and the
games are on the More page.

**Qobuz signs in with a token, not a password.** The account is
`<card>/Addons/qobuz/token.txt` — one line, the `user_auth_token` a web session
already carries; the addon reads it, checks it against `/user/get`, and only then
writes the session. Signing out records `signed_out = 1` in `session.ini` rather
than deleting the token, so a restart does not quietly sign the user back in, and
"Sign in from the card" is one tap away. The app keys are still what every
request needs: without a derivable `app_id`/`app_secret` the page says a token
alone is not enough, rather than blaming the token.

## "The server answered 200"

The one failure that does not explain itself. The player's HTTP client asks for
`Accept-Encoding: identity`, and a reverse proxy in front of a server is free to
compress anyway; what arrives is then gzip and `json_parse()` rejects it, which
is indistinguishable from any other unparseable answer. Two things deal with it:

- `common/gunzip.c` — a gzip body is inflated before parsing, whatever
  the server said about encoding. It lives in the **addon** side on purpose: the
  first version of it sat in the player, which meant a new firmware image for a
  quirk of one service's proxy — exactly what addons exist to avoid. miniz's
  inflate is in the tree already (PNG uses it), and including it into the `.so`
  costs 45 KB and no image;
- the addon logs what it got instead: `subsonic: getArtists answered 200, 1234
  bytes: 1f 8b 08 ...` followed by a printable preview and either `complete` or
  `TRUNCATED`. A body starting with `<` is XML — a server too old to answer
  `f=json` — and the message says so, with the first characters on screen so the
  user does not have to find the log to report what happened;
- a request whose answer does not parse is retried once. The player's client
  speaks HTTP/1.0 with `Connection: close`, so a server that sends no
  `Content-Length` frames its body by closing the socket, and there is no way to
  tell a whole body from one cut short at a chunk boundary. A second connection
  usually delivers the whole document.

## Debugging

The simulator is the cheap loop (`docs/UPDATING.md`, "the simulator"):

```sh
make host
SDL_VIDEODRIVER=offscreen SONIX_SD_ROOT=/tmp/sd \
  SONIX_LANG_DIR=$PWD/../sonix-packer/assets/BLAZE/usr/resource/sonix/language \
  ./sonix_player_host
```

with `Addons/<id>/addon.so` being the **host** build of the library. Touches go
in through `/tmp/sonix/tap` (`printf 'x y' > /tmp/sonix/tap`), screenshots come
out through `/tmp/sonix/shot`.

`subsonic` was driven this way against a mock server. The player logs the
lifecycle, and those lines are the first place to look:

```
addon: subsonic  'Subsonic' (streaming)     ← found at scan time, and after every card insert
addon: subsonic opened                      ← dlopen + init + switch_screen
addon: dlopen …/addon.so: undefined symbol: lv_obj_create
                                            ← the player does not export it
addon: subsonic init() gave no screen       ← init() returned NULL
```

### When the player freezes on the device

Before touching the power button, take the state — it survives only while the
process does:

```sh
adb shell 'dmesg | tail -40'
adb shell 'PID=$(pidof sonix_player); for t in /proc/$PID/task/*/; do \
  echo "$(basename $t) $(cut -d" " -f3 $t/stat) $(cat $t/comm) $(cat $t/wchan)"; done'
adb shell 'pidof sonix_player' | while read p; do cat /proc/$p/task/*/stack; done
adb shell 'grep -E "^(MemFree|Dirty|Writeback|Cached)" /proc/meminfo'
```

A freeze that is not a crash shows as threads in **`D`** state, and the answer
is in `dmesg`: `jzmmc … timeout 3000ms` and `end_request: I/O error, dev
mmcblk0` mean the card, not the code (see "The card is the slowest thing in the
player", above). A hang that recovers by itself after a minute or two is the
same thing: the backlog drains.

Reading the log **while** the card is stalled will hang the shell too — the log
lives on the card. Take `dmesg` and the stacks first, and pull the log after
the reboot.
