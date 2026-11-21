# Rules for agents working in this repository

The addons for the Sonix Player (TempoTec V3 Analog, Ingenic X1000, 320x480 SLCD,
AK4493). The player is the `sonix-player/` submodule: the repository, whose own
source tree (headers, ABI, toolchain, image) sits at `sonix-player/sonix-player/`.
The documentation lives in
`docs/ADDONS.md` — one copy, here, and the player's repository only points at it.

## Build and verify

- `make` builds every addon; `make ADDON_IDS="dino qobuz"` builds some. The
  toolchain is `sonix-player/rockbox-toolchain`, `ROCKBOX_TOOLCHAIN_DIR` reuses
  one from outside the tree.
- `make release` writes `build/release/<id>.zip` and `<id>.json`: exactly what a
  release carries, made the same way the CI makes it.
- `docker/build.sh` is the same build with nothing installed but docker; it
  builds the toolchain on the first run.
- `make host` needs SDL2, FreeType, opusfile, wavpack and ALSA on the machine.
- `make check-abi` after a build; `make check-exports` additionally needs the
  player binary (`sonix-player/sonix-player/sonix_player`), so it runs on a machine that has
  built the player, not on a fresh clone.
- Every addon links against symbols the player exports from
  `sonix-player/sonix-player/src/addon/exports.list`. A new symbol is a change in the player's
  repository (`sonix-player/sonix-player/tools/addon_exports.sh`), never a change here.

## What the CI builds, and what it releases

Every addon compiles in CI when something it depends on changes
(`tools/changed_addons.py`): a change in `common/`, the player submodule or the
`Makefile` means every addon, anything else is the addon folders the diff names.
A docs-only push builds nothing.

`RELEASE_ADDONS` in `.github/workflows/addons.yml` is the gate for packing and
releasing: `2048 addonstore cassette daily dino flappybird gearboy gravity
qobuz subsonic tictactoe yandex`. Everything else — `piped`, `zvuk`, `tidal`,
`spotify` — still compiles, but is never packed into the CI artifact and cannot
be released: a tag `<id>-v<version>` naming an addon outside the list fails the
`changes` job with that reason. Do not add an addon to the list without a device
run to point at — `docs/ADDONS.md`, "What is built, and what is released".

Within the list, only what a commit touches is compiled.

## Releasing

One tag per addon, `<id>-v<version>` (`dino-v0.2.0`). The version in the tag must
be the one in `<id>/addon.json` — `release_zip.py --expect-version` fails the job
before anything is written — so a release is "bump the manifest, tag the same
number". It builds and publishes that addon alone: the release carries `<id>.zip`
and `<id>.json` (title, section, version, zip size/`sha256`, `addon.so`
`sha256`), both written by `tools/release_zip.py` — the same thing `make release`
writes locally (`make release ADDON_IDS=<id> VERSION=<ver>` checks the match too).
After the release, `tools/publish_index.py` builds `index.json` and
`tools/rolling_release.py` publishes it — together with byte copies of every
current archive — in one release tagged `latest`, which is the catalogue the
store fetches directly:

`https://github.com/<owner>/<name>/releases/download/latest/index.json`. The old
all-in-one tags still build and publish every releasable addon. `docs/ADDONS.md`,
"Releasing an addon".

The `latest` release is the copy older store builds read as "the newest
release", and its tag is put on a fresh commit of the `releases` branch: the
release list is ordered by the date of the tagged commit, and releases dated the
same second are ordered by tag name, descending, so a bundle sharing a commit
with an addon release loses to it (`yandex-v0.2.0` sorts above `latest`; the old
`store` release climbed above everything for that reason). The files are
byte copies, so no client is offered an update for an addon that did not change.
`ROLLING_RELEASE=off` in the repository variables turns that job off.

## Rules that keep biting

- An addon is **unloaded** when the card leaves, its files change on the card, or
  — for an app addon whose manifest says `"unload": "leave"` — when the user
  leaves its page (docs/ADDONS.md, "Leaving a page"). So every one exports
  `sonix_addon_stop_entry` (`sonix_addon_stop_t.stop`,
  `bool`): true means the library may be `dlclose()`d, false means a worker is
  still inside it and the mapping is kept instead. A library without the symbol
  is refused at load. `stop()` joins threads, deletes its `lv_timer`s (with
  `power_forget_timer()`), withdraws `theme`/`device_state`/`streamturn`
  registrations, and registers every screen it creates with
  `sonix_addon_own_screen()`. A `stop()` that can block in a socket has no
  business with the `leave` key until it can interrupt itself
  (`http_stream_wake()`; `as_cancel()` in `addonstore/asnet.c`).
- The interface is written at 480x720 and compiled at 320x480: every pixel size
  or position goes through `SC()` (`sonix-player/sonix-player/src/gui/shell/scale.h`).
  `tools/scale_gui.py` in the player's repository finds bare literals in layout
  calls; the `#define`d sizes at the top of a page have to be wrapped by hand.
- `lv_*` only on the UI thread; network and card work on a worker, then
  `gui_post()`. Never `lv_async_call()`.
- An addon's settings live next to it on the card (`common/store`), never in the
  player's `device_config.ini`.
- The tile icon is `icon.png` next to `addon.json`, 74 px box (SC(111)) on this
  panel; `tools/make_icon.py` draws one in the player's style.
- **A download cache paces its writes to the card.** 25–45 MB written at network
  speed (2–4 MB/s) makes this device's card time out (`jzmmc … 3000ms`, then
  `end_request: I/O error`), and the whole player — interface, log, library
  journal — goes into `D` state behind it; that is what the "hangs after a few
  tracks" reports are. `common/cardpace.h` is the reference, 1 MB/s: a
  `cardpace_t` per download, `cardpace_reset()` before it, `cardpace_note()`
  after every write of track audio (never for covers). Do not add a sweep that
  deletes the cache in bursts, and do not add fsync loops. `docs/ADDONS.md`,
  "The card is the slowest thing in the player".
- `spotify/engine-mips/` is a Rust crate whose device build needs its own docker
  image (`spotify/engine-mips/docker/`: musl cross compiler, pinned nightly), not
  the player's; its `target/` is not in git
  and `addons/spotify/addon.mk` copies the built binary into `spotify/assets/`.
  The addon without that binary says "engine not built" — never commit a stale
  one.
- **A push carrying more than three tags creates no `push` events at all**, so
  ten tags pushed together release nothing and the store catalogue is not
  rebuilt (`latest` needs a tag or an explicit `index`). Push tags one at a
  time; a batch already on the remote must be deleted and pushed again in
  threes — `docs/ADDONS.md`, "Releasing an addon".
- `git commit` is denied to agents by the global opencode config: stage, write
  the message, hand the command to the owner.
