# Rules for agents working in this repository

The addons for the Sonix Player (TempoTec V3 Analog, Ingenic X1000, 320x480 SLCD,
AK4493). The player is the `sonix-player/` submodule. The documentation lives in
`docs/ADDONS.md` — one copy, here, and the player's repository only points at it.

## Build and verify

- `make` builds every addon; `make ADDON_IDS="dino qobuz"` builds some. The
  toolchain is `sonix-player/rockbox-toolchain`, `ROCKBOX_TOOLCHAIN_DIR` reuses
  one from outside the tree.
- `docker/build.sh` is the same build with nothing installed but docker; it
  builds the toolchain on the first run.
- `make host` needs SDL2, FreeType, opusfile, wavpack and ALSA on the machine.
- `make check-abi` after a build; `make check-exports` additionally needs the
  player binary (`sonix-player/sonix_player`), so it runs on a machine that has
  built the player, not on a fresh clone.
- Every addon links against symbols the player exports from
  `sonix-player/src/addon/exports.list`. A new symbol is a change in the player's
  repository (`sonix-player/tools/addon_exports.sh`), never a change here.

## What the CI builds

`ADDON_IDS` in `.github/workflows/addons.yml` is the whitelist: `dino flappybird
gearboy hello qobuz subsonic yandex`. `zvuk`, `tidal` and `spotify` are excluded
because none has been verified on a device; `spotify` also because its Rust
engine must be built with its C half, and that pair has not been checked. Do not
add an addon to the whitelist without a device run to point at — `docs/ADDONS.md`,
"What is built, and what is not".

## Rules that keep biting

- An addon is **unloaded** when the card leaves or its files change on the card,
  so every one exports `sonix_addon_stop_entry` (`sonix_addon_stop_t.stop`,
  `bool`): true means the library may be `dlclose()`d, false means a worker is
  still inside it and the mapping is kept instead. A library without the symbol
  is refused at load. `stop()` joins threads, deletes its `lv_timer`s (with
  `power_forget_timer()`), withdraws `theme`/`device_state`/`streamturn`
  registrations, and registers every screen it creates with
  `sonix_addon_own_screen()`.
- The interface is written at 480x720 and compiled at 320x480: every pixel size
  or position goes through `SC()` (`sonix-player/src/gui/shell/scale.h`).
  `tools/scale_gui.py` in the player's repository finds bare literals in layout
  calls; the `#define`d sizes at the top of a page have to be wrapped by hand.
- `lv_*` only on the UI thread; network and card work on a worker, then
  `gui_post()`. Never `lv_async_call()`.
- An addon's settings live next to it on the card (`common/store`), never in the
  player's `device_config.ini`.
- The tile icon is `icon.png` next to `addon.json`, 74 px box (SC(111)) on this
  panel; `tools/make_icon.py` draws one in the player's style.
- `spotify/engine-mips/` is a Rust crate whose device build needs the player's
  docker image (musl cross compiler, pinned nightly); its `target/` is not in git
  and `addons/spotify/addon.mk` copies the built binary into `spotify/assets/`.
  The addon without that binary says "engine not built" — never commit a stale
  one.
- `git commit` is denied to agents by the global opencode config: stage, write
  the message, hand the command to the owner.
