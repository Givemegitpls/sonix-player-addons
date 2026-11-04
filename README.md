# sonix-player-addons

The addons for the Sonix Player on the TempoTec V3 Analog — every app that sits
on the card under `<card>/Addons/` and appears as a tile in Streaming or More.
Each one is a MIPS32 shared library the player opens with `dlopen()` when its
tile is tapped.

The player itself is a separate repository, and here it is a submodule:

- player: <https://github.com/Givemegitpls/sonix-player-tempotec-v3-analog>
- documentation: [`docs/ADDONS.md`](docs/ADDONS.md) — the interface, the thread
  rules, the card layout, the audio paths, the build, and one section per addon.
  This is the only copy; the player's repository points here instead of keeping
  its own.

## Layout

```
sonix-player/       the player's repository, as a submodule; its source tree
                    (headers, ABI, toolchain, image) is sonix-player/sonix-player/
common/             compiled into every addon (store, addonfs, listrow, gunzip)
dino/               the Chrome offline game
flappybird/         the game from the More page
gearboy/            Game Boy / Game Boy Color, from src/gb
hello/              the example addon; not packed on a card
qobuz/              Qobuz, as a service
spotify/            Spotify: the C interface and engine-mips/, its Rust half
subsonic/           the user's own library on their own server
tidal/              Tidal, as a service
yandex/             Yandex Music, as a service
zvuk/               Zvuk, as a service
tools/              addon_port.py (porting a page), make_icon.py (tile icons)
docs/ADDONS.md      the documentation
```

## Building

```sh
git clone https://github.com/Givemegitpls/sonix-player-addons
cd sonix-player-addons
git submodule update --init      # the player: headers, ABI, toolchain, image
make                             # build/target/<id>.so, all of them
make host                        # build/host/<id>.so, for the SDL simulator
make pack                        # build/Addons/<id>/ -- copy to <card>/Addons
make checks                      # ABI and export checks: needs a built player
                                 # (sonix-player/sonix-player/sonix_player) for the second one
```

With nothing but docker installed, `docker/build.sh` builds the cross toolchain
(20-60 minutes the first time, inside the submodule) and runs the same make in
the player's image. `docker/build.sh ADDON_IDS="dino qobuz"` narrows the build.

`make host` needs the machine's own SDL2, FreeType, opusfile, wavpack and ALSA:
it is the simulator's build, not the device's.

## What is built, and what is not

The CI builds a whitelist and packs only that: `dino flappybird gearboy hello
qobuz subsonic yandex`. `zvuk`, `tidal` and `spotify` are in the tree, compile,
and are not built by the pipeline — none of the three has been verified on a
device, and `spotify` additionally means nothing without the engine its Rust half
builds. `docs/ADDONS.md`, "What is built, and what is not", has the per-addon
reason. A plain `make` builds all of them.

## Licence

GPL-3.0-or-later, the same as the player (see `LICENSE`). The parts that came
from elsewhere — Gearboy's core and its APU, the rekkod-mod pages, librespot —
carry their own notices next to them.
