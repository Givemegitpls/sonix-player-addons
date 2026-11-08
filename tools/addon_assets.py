#!/usr/bin/env python3
"""Build the cassette addon's sprite files from the owner's drawing.

The page is the drawing turned ninety degrees clockwise to fill the 320x480
panel, so every sprite here is written already turned: the addon just places
them by the coordinates the drawing gives.

Sources, all from the Blender project:
  * tape_roller.svg -- the hub, rasterised at the size it is drawn at;
  * sticker.png     -- the label, its chamfers and corners as drawn;
  * reflection.png  -- the light rim of the plate;
  * the flat render  -- the window with the tape, and the plate's holes.

    /usr/bin/python3 tools/addon_assets.py --blender-dir "/path/to/cassete" \
        --render /tmp/tape.png --out cassette/assets
"""

import argparse
import os
import subprocess
import sys

from PIL import Image
import numpy as np

# The drawing is 480x320; the page is that turned clockwise.
PAGE_ROTATE = -90


def rotate(im):
    return im.rotate(PAGE_ROTATE, expand=True)


def crop(im, box, scale=1.0):
    """A crop of the drawing, brought down to 1x (the render is 3x)."""
    x0, y0, x1, y1 = box
    part = im.crop((x0, y0, x1, y1))
    w, h = int(round((x1 - x0) / scale)), int(round((y1 - y0) / scale))
    return rotate(part.resize((w, h), Image.LANCZOS))


def rasterise(svg, size, tint):
    tmp = "/tmp/addon_asset_%d.png" % os.getpid()
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "-o", tmp, svg], check=True)
    im = Image.open(tmp).convert("RGBA")
    os.unlink(tmp)
    a = np.asarray(im).astype(int).copy()
    t = tint.lstrip("#")
    a[..., 0], a[..., 1], a[..., 2] = int(t[0:2], 16), int(t[2:4], 16), int(t[4:6], 16)
    return rotate(Image.fromarray(a.astype("uint8"), "RGBA"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--blender-dir", required=True)
    ap.add_argument("--render", required=True, help="the flat render of the whole cassette, 3x")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    render = Image.open(args.render).convert("RGBA")
    scale = render.width / 480.0     # the render is 3x the drawing

    def at(x0, y0, x1, y1):
        return (int(x0 * scale), int(y0 * scale), int(x1 * scale), int(y1 * scale))

    # The window: black with the two bands of tape the head sees.
    win = crop(render, (int(195 * scale), int(114 * scale), int(285 * scale), int(164 * scale)), scale)
    win.save(os.path.join(args.out, "window.png"))

    # The plate: its holes and the capstan, on the shell's colour cut away.
    plate = np.asarray(crop(render, (int(88 * scale), int(216 * scale), int(392 * scale), int(300 * scale)),
                            scale)).astype(int).copy()
    r, g, b = plate[..., 0], plate[..., 1], plate[..., 2]
    lum = (r * 299 + g * 587 + b * 114) // 1000
    keep = (lum < 130) & ((b - r) <= 18)
    plate[..., 3] = np.where(keep, plate[..., 3], 0)
    Image.fromarray(plate.astype("uint8"), "RGBA").save(os.path.join(args.out, "plate.png"))

    # The label, and the rim of light: both already carry their own alpha.
    sticker = Image.open(os.path.join(args.blender_dir, "sticker.png")).convert("RGBA")
    rotate(sticker.crop((32, 47, 447, 217))).save(os.path.join(args.out, "sticker.png"))

    refl = Image.open(os.path.join(args.blender_dir, "reflection.png")).convert("RGBA")
    a = np.asarray(refl).astype(int).copy()
    a[..., 3] = (a[..., 3] * 0.55).astype(int)          # light, not a part
    refl = Image.fromarray(a.astype("uint8"), "RGBA")
    ys, xs = np.where(np.asarray(refl)[..., 3] > 0)
    box = (xs.min(), ys.min(), xs.max() + 1, ys.max() + 1)
    refl.crop(box).rotate(PAGE_ROTATE, expand=True).save(os.path.join(args.out, "reflection.png"))
    print("reflection placed at", box, "-> the addon uses", (94, 224, 385, 290))

    hub = rasterise(os.path.join(args.blender_dir, "tape_roller.svg"), 59, "#c5c5c5")
    hub.save(os.path.join(args.out, "hub.png"))

    for name in ("hub", "window", "plate", "sticker", "reflection"):
        p = os.path.join(args.out, name + ".png")
        print(f"{p}: {Image.open(p).size}, {os.path.getsize(p)} bytes")


if __name__ == "__main__":
    sys.exit(main())
