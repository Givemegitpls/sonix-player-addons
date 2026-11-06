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
- setting up: [`docs/SETUP.md`](docs/SETUP.md) — what each released addon needs
  on the card, and how to sign a service in (both languages, section by
  section).

## Installing an addon on the card

The releases page carries one zip per addon, named after its folder:
`qobuz.zip`, `subsonic.zip`, `yandex.zip`, `gearboy.zip`, `flappybird.zip`,
`dino.zip`. (`hello` is built as the compile check and is not packed, so there is
no `hello.zip`.) The files inside sit at the **root** of the archive — `addon.json`,
`addon.so`, `icon.png`, `README.txt`, and `assets/` for the addons that bring
artwork or sound.

```
<id>.zip
├── addon.json     what it is: title, section (Streaming / More), order, version
├── addon.so       the MIPS32 code, opened with dlopen() on the first tap
├── icon.png       the tile's picture
├── README.txt     installing and signing in, English and Russian
└── assets/        optional: only where the addon brings artwork or sound
```

What the player looks for is `<card>/Addons/<id>/addon.json`, and the id is the
folder's name. So:

1. Copy `<id>.zip` to a computer the card is visible from.
2. Create `<card>/Addons/<id>/` on the card — `Addons/qobuz`, `Addons/yandex`,
   and so on. The folder name is the addon's id; nothing else identifies it.
3. Unzip the archive **into that folder**, not into `Addons/` and not into a
   folder an archiver offers to create, so the result is exactly

   ```
   <card>/Addons/qobuz/addon.json
   <card>/Addons/qobuz/addon.so
   <card>/Addons/qobuz/icon.png
   <card>/Addons/qobuz/README.txt
   ```

   A second level (`Addons/qobuz/qobuz/addon.json`) or a bare
   `Addons/addon.json` is a card the player reads and silently skips.
4. Eject the card properly and put it in the player. The tile appears in
   **Streaming** for the services and in **More** for the games;
   `addon.json`'s `section` and `order` decide which page and where on it.
5. A card the player sees arrive while it runs is read again immediately — the
   registry rescans `Addons/` and the two pages are rebuilt — so a restart is
   normally not needed. Restart if a tile is missing anyway.

`addon.json`'s `"api"` must be the API the player's build speaks (currently `1`);
a newer addon on an older player is skipped and the log says why. Both the
player and the addon are from this project, so taking the pair from the same
release is the only combination that is expected to work.

**Updating.** Replace the addon's files with the new archive's, keeping the
folder. Its own settings and caches live in the same folder
(`<card>/Addons/<id>/*.ini`, `cache/`) and are not part of any archive, so they
survive. The player unloads the library when the card goes, so a replaced
`addon.so` is reopened on the next tap.

**Removing.** Delete `<card>/Addons/<id>/`. Nothing else refers to it; a cache
left behind is what a factory reset empties, and a sign-in is the only thing that
has to be repeated.

**A read-only card** still runs an addon, but a session cannot be written: the
service works for that run and its page says it will not be kept.

**What an addon writes on the card** — and what can be deleted safely — is the
table in [`docs/SETUP.md`](docs/SETUP.md), "What is on the card". The rule behind
it: an addon writes under `<card>/Addons/<id>/` and nowhere else; the ROMs and
saves in `Games/` are the user's, not an addon's.

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
docs/SETUP.md       installing an addon, and signing a service in (en/ru)
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

The CI builds a whitelist and packs all of it but `hello`: `dino flappybird
gearboy qobuz subsonic yandex` are the release zips, and `hello` is the compile
check that proves the SDK still builds. `zvuk`, `tidal` and `spotify` are in the
tree, compile, and are not built by the pipeline — none of the three has been
verified on a device, and `spotify` additionally means nothing without the engine
its Rust half builds. `docs/ADDONS.md`, "What is built, and what is not", has the
per-addon reason. A plain `make` builds all of them.

## Licence

GPL-3.0-or-later, the same as the player (see `LICENSE`). The parts that came
from elsewhere — Gearboy's core and its APU, the rekkod-mod pages, librespot —
carry their own notices next to them.
