---
name: sonix-icons
description: Use when drawing, replacing or reviewing a tile icon for an addon or a compiled-in page of the Sonix Player on the TempoTec V3 Analog — a PNG tile in the player's own style (an upright rounded card with a white glyph over a rotated saturated one). Covers `sonix-player/sonix-player/tools/make_icon.py`, the brand glyph sources in `assets/icons/brand/`, the loader's limits (SC(111) = 74 px, alpha kept, 4 MB, 4096²), the difference between an addon's runtime tile and a compiled-in menu icon, and pushing the result to the card. Trigger keywords: иконка аддона, плитка, icon.png, tile icon, make_icon, brand glyph, potrace, логотип сервиса, spotify icon, yandex music icon, zvuk icon, dino icon, иконка в стиле меню, ADDON_ICON_BOX, SC(111), icons_small, svg_to_lvgl.
---

# Tile icons

A tile icon is a 140x140 PNG that reads at 74 px. The player never scales it
up, so the size it is judged at is the size it has to work at.

Read `docs/ADDONS.md`, "Making the tile icon", for the reference text; this
skill is the recipe and the traps.

## The look, in one sentence

An upright rounded square carrying a white monochrome glyph, the same square
again rotated ~13° and saturated behind it, both a diagonal gradient of one
hue. `tidal.png`, `qobuz.png`, `subsonic/icon.png` and `flappybird/icon.png` are
the reference tiles; match those, not the ones that look odd (`dino`'s old
pixel-art square, `spotify`'s green circle).

Do not reach for a diffusion model for this: the style is parametric, and no
image model reproduces a brand mark or a legible glyph at 74 px.

## Making one

```sh
cd sonix-player/sonix-player
python3 tools/make_icon.py --glyph assets/icons/brand/zvuk.svg --hue 100 \
    --out ../zvuk/icon.png --preview /tmp/p.png
```

- `--hue` degrees for a coloured tile, `--mono` for the dark grey of Tidal,
  `--glyph-colour` for a coloured glyph (Spotify is `--mono --glyph-colour
  '#1ED760'`), `--rim 0.08` to tame the light edge on a dark tile.
- `--glyph` takes any SVG or PNG: Lucide (a stroke drawing), simple-icons (one
  filled path), a potrace of a logo, or a PNG. The size is fitted from the
  measured ink, so sources with different margins come out equal.
- `--preview` writes the tile at 140 px beside itself at 74 px. Always look at
  the 74 px one before shipping: a star with thin rays or a 2 px stroke does not
  survive it.
- `--size 512` for a card-sized copy; the loader scales it down itself.
- `--svg-only` prints the tile's SVG, which is the way to nudge the geometry by
  hand without touching the 140 px PNG.

Rasterising uses whichever of cairosvg, rsvg-convert or ImageMagick is
installed. The host has rsvg-convert and ImageMagick, not cairosvg.

## Brand glyphs

`assets/icons/brand/` holds sources only — no icon set compiles them.

- `spotify.svg` — the simple-icons path, CC0.
- `yandexmusic.svg`, `zvuk.svg` — potrace of the services' own app icons: the
  star from the 180 px `apple-touch-icon.png` of music.yandex.ru, the Zvuk
  squircle from its favicon (only 75 px; upscaled 10x before tracing). Trace
  with the mark **black** in the PBM — potrace fills the dark of the image, so a
  mask written mark-white traces the background and the glyph arrives as a black
  square with the logo knocked out of it.
- `dino.svg` — Dino's own pixel art turned into one rectangle per horizontal
  run, so it stays crisp at any size.

New services: try `https://cdn.simpleicons.org/<slug>` first; when it 404s,
take the service's touch icon and trace it, or draw the glyph by hand. These are
trademarks of their owners, used to label the service the tile opens.

## Where the tile goes

- **An addon's tile is a runtime file**, `<card>/Addons/<id>/icon.png`, named by
  `addon.json`'s `icon`. Nothing is compiled, nothing is rebuilt, no image set
  changes: `adb push` the PNG and the player picks it up on the next card scan.
- **A compiled-in menu tile is the other road**: an entry in `ICONS` or
  `COLOR_ICONS` in `tools/svg_to_lvgl.py`, then `--all` to regenerate
  `icons.c` and `icons_small.c`. That rewrites 24 MB of checked-in source and
  changes the icons of every model. An addon's tile never needs it.

## The loader's limits

`icon_load()` in `src/addon/addon.c`: 4 MB file, 4096x4096 pixels, four
channels, alpha kept (`icon_pack()` averages by alpha so glyph edges do not
darken), never upscaled, drawn in a box of `SC(111)` — 111 units at 480x720,
74 px at 320x480. A source smaller than 74 px shows up soft; transparent
corners take the tile's colour, opaque ones show as a square.

## Pushing to the player

```sh
adb push zvuk/icon.png /mnt/sd_0/Addons/zvuk/icon.png
```

The card is `/mnt/sd_0` on this board. `adb` here is USB only — no
`adb connect`. Replacing a PNG does not need the addon reload dance that
`addon.so` does (no live mapping is involved); the tile is re-read when the card
is scanned. Verify with `md5sum` on both sides rather than trusting the push.

Forcing that scan by `kill`ing `sonix_player` takes the whole board down with
it: adb drops ("error: closed"), the board reboots, and about half a minute
later the same serial is back with `uptime` at 0 and the player running again.
The new tiles are read on that boot. Do not read the reboot as a broken push —
check `md5sum` after adb returns.
