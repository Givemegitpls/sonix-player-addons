# Where this addon's game comes from, and what was done to it

## The game

**Gravity Defied** was a 2004 J2ME moto-trial game by Codebrew Software (Tors
Björn Henrik Johansson, Set Elis Norman, Per David Jacobsson). The name, the
logotype and the artwork are Codebrew's; nothing here is associated with them.

The code this addon carries is **rgimad/gravity_defied_cpp**, a rewrite of the
game from Java into C++ and SDL2 that includes all the features of the original:

    https://github.com/rgimad/gravity_defied_cpp

taken at commit `91bd283959b96a7ea07e1c4c0040460334c85458 (2025-05-12)`, from its `src/` and `assets/` directories,
under the **GNU General Public License, version 2** (`LICENSE` in this directory
is that licence, unmodified). The copyright of that code stays with its authors:
rgimad, AntonEvmenenko and Max Logaev, on top of Codebrew's work.

The PSP ports by **antim0118** and **den4iklovelinux** were read while doing this
and are the model for how the platform layer was attacked -- antim0118's
`src/lcdui/Graphics.cpp` replaces the same five SDL files with 309 lines against
gLib2D at 480x272, which is what made the estimate honest. None of their code is
in this addon.

### Licensing, plainly

This repository is GPL-3.0 and the player with it; upstream is GPL-2.0, and the
two are not compatible. The game is kept in this one directory, with its own
`LICENSE` file and this notice, and built as a separate library that the player
opens at run time -- so the boundary is visible in the tree and in the shipped
`.so`. That is a marking of the problem, not a solution to it: the clean answer
is upstream saying "version 2 or any later version" -- one sentence in its
licence material -- and until that exists this directory is the boundary.

## What is upstream's, and untouched

Everything in `game/src/` except the files listed below. That is the game: the
physics in 16.16 fixed point (`GamePhysics.cpp`, `MathF16.cpp`), the level format
(`LevelLoader.cpp`), the menu and its layout (`MenuManager.cpp`, `GameMenu.cpp`),
`GameCanvas.cpp`, the record store (`rms/`). Its include paths are the ones it was
written with, so an upstream change can still be read as a diff.

`assets/` is upstream's: the ten PNGs, `levels.mrg` (5 KB, every level), and
`FontSansSerif.ttf`. The PNGs are all either fully opaque or fully transparent
per pixel, which is why an image needs one byte of mask per pixel and not more.

## What was removed

| file | why |
|---|---|
| `src/main.cpp` | it has a `main()`; an addon is a library |
| `src/lcdui/Graphics.cpp`, `Image.cpp`, `Font.cpp`, `CanvasImpl.cpp` | they are SDL; `platform/` implements the same classes |
| `src/utils/Time.cpp` | `SDL_Delay` and the wall clock; rewritten in place against `clock_gettime` |
| `src/utils/EmbedFileStream.*` | cmrc, the CMake step that embedded `levels.mrg` into the binary; the levels are a file on the card now |
| `src/tests/` | they test the SDL layer and have a `main()` |

## What was changed in place

Small, and each one is a comment where it happens:

| file | change |
|---|---|
| `src/lcdui/{Graphics,Image,Font,CanvasImpl}.h` | the SDL types swapped for the platform's (`gfx_target*`, RGB565 pixels, a baked face, a thread's frame). The class shapes the game calls are unchanged. |
| `src/lcdui/Canvas.cpp` | `make_unique<Graphics>(impl->getRenderer())` → `getTarget()`: there is no renderer |
| `src/LevelLoader.cpp` | the cmrc fallback became an exception; the addon passes `<addon>/assets/levels.mrg` |
| `src/rms/RecordStore.cpp` | `setRecordStoreDir()` took `$HOME` and wrote `~/.GravityDefied`; it now takes the directory it is given -- `<addon>/saves` on the card |
| `src/utils/Time.cpp` | `CLOCK_MONOTONIC` milliseconds and `nanosleep`; every comparison in the game is an interval |

## What is ours

`platform/` -- the layer upstream's SDL files occupied:

- `gfx.{h,cpp}`: an RGB565 frame, clip, rect, line, arc, blit, glyph. The arc
  arithmetic is upstream's, doubles and all, because the wheels are drawn with it
  and changing the picture was not the job.
- `graphics.cpp`, `image.cpp`, `font.cpp`: the lcdui classes on top of it. Images
  are decoded with stb_image, which the player exports (Flappy Bird does the
  same); the font is baked into `font_data.c` by `tools/bake_font.py`, because an
  addon has no FreeType.
- `surface.{h,cpp}`, `canvas.cpp`: the frame buffer, the hand-over to the
  interface thread, and the thread the game's own loop runs on.
- `addon.c` (one directory up): the screen, the picture of the frame, the tile.

## The one behavioural change

The game's loop is upstream's: `Micro::run()` paces itself with `Time::sleep` and
never returns until it is told to stop. That is why the game has a thread of its
own, and why `Micro::field_249` -- the game's own "keep going" -- is the handle
the addon's `stop()` uses. Nothing in the game was restructured into a step
function to avoid this.

## Licence of this directory's own files

`platform/`, `addon.c`, `tools/` and the build files are written for this player
and are under the repository's GPL-3.0; the game they carry (`game/`, `assets/`)
is GPL-2.0. See "Licensing, plainly" above.
