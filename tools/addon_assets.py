#!/usr/bin/env python3
"""Build the cassette addon's two sprites from the owner's drawing.

The page is a stack, bottom to top:

    the shell's colour   -- one flat rectangle the addon paints, its colour
                            comes from the sleeve on screen;
    the hub              -- hub.png, the reel the tape is wound on, turned
                            every frame;
    the overlay          -- overlay.png, 320x480, everything else: the shape
                            of the shell with its bevel and shadows, the recess
                            the reels sit in, the window, the screws, the label,
                            the reflections. Its alpha is where the colour and
                            the hubs show through, and it is drawn *over* the
                            hubs, so it also frames them;
    the light            -- shadow.png and light.png, both optional, both
                            320x480 and drawn over the lot: the shadow's dark
                            pixels multiply, the light's white pixels add, and
                            their alpha is the strength. This is the room the
                            cassette stands in -- a light that stays put when
                            the sleeve changes.

All files are written already turned to the page: the drawing is 480x320, the
panel is that rotated ninety degrees clockwise, so the addon places them by the
coordinates the drawing gives.

Sources, any one of:

  --static PNG                  one static layer for the whole page, as the
                                drawing (480x320) or already as the page;
  --pieces DIR                  sticker.png, reflection.png, window.png and
                                plate.png -- the four pre-rotated pieces the
                                first version of this tool wrote, so the
                                committed overlay can be rebuilt;
  --blender-dir DIR --render PNG  the Blender project itself: those four
                                pieces are cut out of the render and the .svg
                                files, then composited.

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
PAGE_W, PAGE_H = 320, 480

# The drawing's numbers, on the page, after the turn: lrect(lx, ly, lw, lh)
# in the addon (page x = 320 - (ly + lh), y = lx; width = lh, height = lw).
MOCK_H = 320


def place(lx, ly, lw, lh):
    return (MOCK_H - (ly + lh), lx, lh, lw)


SHELL = place(10, 20, 459, 279)          # the shell, radius SC(20) = 13
SHELL_RADIUS = 13
REFLECTION = place(94, 224, 291, 67)
STICKER = place(32, 47, 415, 170)
LABEL = (218, 89, 54, 300)               # the paper the writing sits on
INSERT = place(107, 103, 265, 71)        # the strip of shell the reels sit in
WINDOW = place(195, 114, 90, 50)
PLATE = place(88, 216, 304, 84)
SCREWS = [place(28 - 7, 38 - 7, 14, 14), place(452 - 7, 38 - 7, 14, 14),
          place(28 - 7, 282 - 7, 14, 14), place(452 - 7, 282 - 7, 14, 14)]
REELS = [(place(143 - 30, 139 - 30, 60, 60), 30), (place(336 - 30, 139 - 30, 60, 60), 30)]

# Fixed colours, the same ones cpal.c hands the page.
PAPER = (0xC5, 0xC2, 0xB1)
DISC = (0x0F, 0x0D, 0x0B)
SCREW = (0x4A, 0x4A, 0x4A)
HUB_TINT = "#c5c5c5"

# Supersampling for the two shapes LVGL used to draw for us: the shell's
# rounded corners and the round things on it.
AA = 4


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


def over(dst, src, at):
    """Alpha-composite an RGBA array onto another, at (x, y), clipping."""
    x, y = at
    h, w = src.shape[:2]
    x0, y0 = max(x, 0), max(y, 0)
    x1, y1 = min(x + w, dst.shape[1]), min(y + h, dst.shape[0])
    if x1 <= x0 or y1 <= y0:
        return
    s = src[y0 - y:y1 - y, x0 - x:x1 - x].astype(float) / 255.0
    d = dst[y0:y1, x0:x1].astype(float) / 255.0
    sa = s[..., 3:4]
    out_a = sa + d[..., 3:4] * (1.0 - sa)
    rgb = np.where(out_a > 0, (s[..., :3] * sa + d[..., :3] * d[..., 3:4] * (1.0 - sa)) / np.maximum(out_a, 1e-6), 0)
    dst[y0:y1, x0:x1, :3] = (rgb * 255.0 + 0.5).astype(np.uint8)
    dst[y0:y1, x0:x1, 3] = (out_a[..., 0] * 255.0 + 0.5).astype(np.uint8)


def rounded_mask(w, h, radius):
    """Alpha of a w x h rectangle with rounded corners, anti-aliased."""
    big_w, big_h, big_r = w * AA, h * AA, radius * AA
    yy, xx = np.mgrid[0:big_h, 0:big_w]
    px = xx + 0.5 - big_w / 2.0
    py = yy + 0.5 - big_h / 2.0
    qx = np.abs(px) - (big_w / 2.0 - big_r)
    qy = np.abs(py) - (big_h / 2.0 - big_r)
    qx = np.maximum(qx, 0)
    qy = np.maximum(qy, 0)
    d = np.sqrt(qx * qx + qy * qy) - big_r
    inside = (d <= 0).astype(np.uint8) * 255
    return np.asarray(Image.fromarray(inside, "L").resize((w, h), Image.LANCZOS))


def disc_mask(diameter):
    yy, xx = np.mgrid[0:diameter * AA, 0:diameter * AA]
    cx = cy = diameter * AA / 2.0
    r = diameter * AA / 2.0
    d = np.sqrt((xx + 0.5 - cx) ** 2 + (yy + 0.5 - cy) ** 2)
    inside = (d <= r).astype(np.uint8) * 255
    return np.asarray(Image.fromarray(inside, "L").resize((diameter, diameter), Image.LANCZOS))


def fill(dst, box, rgba):
    x, y, w, h = box
    dst[y:y + h, x:x + w] = rgba


def fill_masked(dst, box, rgba, mask):
    x, y, w, h = box
    region = dst[y:y + h, x:x + w]
    a = mask.astype(float) / 255.0
    for c in range(3):
        region[..., c] = (rgba[c] * a + region[..., c] * (1.0 - a) * (region[..., 3] / 255.0)) \
            / np.maximum(a + region[..., 3] / 255.0 * (1.0 - a), 1e-6)
    region[..., 3] = np.maximum(region[..., 3], (mask * 1.0).astype(np.uint8))


def clear(dst, box):
    x, y, w, h = box
    dst[y:y + h, x:x + w] = 0


def compose(pieces):
    """The overlay: everything that does not turn and is not the shell colour.

    `pieces` are the drawing's static parts, already turned to the page:
    reflection, sticker, window, plate.
    """
    ov = np.zeros((PAGE_H, PAGE_W, 4), np.uint8)
    # The shell's shape, and the page around it: the colour shows where this is
    # transparent, the black page is here rather than an object of its own.
    sx, sy, sw, sh = SHELL
    ov[..., 0:3] = 0
    ov[..., 3] = 255
    inside = rounded_mask(sw, sh, SHELL_RADIUS)
    hole = ov[sy:sy + sh, sx:sx + sw]
    hole[..., 3] = 255 - inside

    over(ov, pieces["reflection"], REFLECTION[:2])
    over(ov, pieces["sticker"], STICKER[:2])
    # The paper the writing sits on, as opaque as the label itself.
    fill(ov, LABEL, (*PAPER, 255))
    # The strip of shell the reels sit in is the shell's colour: this is the
    # colour layer showing through, not a drawing of its own.
    clear(ov, INSERT)
    over(ov, pieces["window"], WINDOW[:2])
    over(ov, pieces["plate"], PLATE[:2])
    for box in SCREWS:
        fill_masked(ov, box, SCREW, disc_mask(box[2]))
    return Image.fromarray(ov, "RGBA")


def hub_png(art):
    """The reel: the hub drawing on the black disc it used to be drawn over.

    The addon puts this one under the overlay and turns it, so the disc turns
    with the tape wound on it and the overlay frames it.
    """
    d = 2 * REELS[0][1]
    canvas = np.zeros((d, d, 4), np.uint8)
    fill_masked(canvas, (0, 0, d, d), DISC, disc_mask(d))
    over(canvas, np.asarray(art), (0, 0))
    return Image.fromarray(canvas, "RGBA")


def read_png(path):
    return np.asarray(Image.open(path).convert("RGBA"))


def pieces_from_blender(blender_dir, render_path):
    render = Image.open(render_path).convert("RGBA")
    scale = render.width / 480.0     # the render is 3x the drawing

    # The window: black with the two bands of tape the head sees.
    win = crop(render, (int(195 * scale), int(114 * scale), int(285 * scale), int(164 * scale)), scale)

    # The plate: its holes and the capstan, on the shell's colour cut away.
    plate = np.asarray(crop(render, (int(88 * scale), int(216 * scale), int(392 * scale), int(300 * scale)),
                            scale)).astype(int).copy()
    r, g, b = plate[..., 0], plate[..., 1], plate[..., 2]
    lum = (r * 299 + g * 587 + b * 114) // 1000
    keep = (lum < 130) & ((b - r) <= 18)
    plate[..., 3] = np.where(keep, plate[..., 3], 0)
    plate = np.asarray(Image.fromarray(plate.astype("uint8"), "RGBA"))

    sticker = read_png(os.path.join(blender_dir, "sticker.png"))
    sticker = np.asarray(rotate(Image.fromarray(sticker).crop((32, 47, 447, 217))))

    refl = read_png(os.path.join(blender_dir, "reflection.png")).astype(int).copy()
    refl[..., 3] = (refl[..., 3] * 0.55).astype(int)      # light, not a part
    refl = Image.fromarray(refl.astype("uint8"), "RGBA")
    ys, xs = np.where(np.asarray(refl)[..., 3] > 0)
    refl = refl.crop((xs.min(), ys.min(), xs.max() + 1, ys.max() + 1))
    refl = np.asarray(refl.rotate(PAGE_ROTATE, expand=True))

    art = rasterise(os.path.join(blender_dir, "tape_roller.svg"), 59, HUB_TINT)
    return {"reflection": refl, "sticker": sticker, "window": np.asarray(win), "plate": plate}, art


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--static", help="one static layer for the whole page, as drawn or as the page")
    src.add_argument("--pieces", help="a directory with the four pre-rotated pieces")
    src.add_argument("--blender-dir", help="the Blender project (with --render)")
    ap.add_argument("--render", help="the flat render of the whole cassette, 3x")
    ap.add_argument("--hub", help="use this hub drawing instead of the project's .svg")
    ap.add_argument("--shadow", help="the shadow texture (dark pixels multiply); also looked for in --static/--pieces/blender dirs")
    ap.add_argument("--light", help="the light texture (white pixels add); also looked for in --static/--pieces/blender dirs")
    args = ap.parse_args()

    if args.blender_dir and not args.render:
        ap.error("--blender-dir needs --render")
    os.makedirs(args.out, exist_ok=True)

    def extra(name, flag):
        """An optional panel-sized picture: told by a flag, or found by name."""
        if flag:
            return flag
        for d in (args.pieces, args.blender_dir,
                  args.static and (os.path.dirname(os.path.abspath(args.static)) or ".")):
            p = os.path.join(d, name + ".png") if d else None
            if p and os.path.exists(p):
                return p
        return None

    for name, path in (("shadow", extra("shadow", args.shadow)), ("light", extra("light", args.light))):
        if path is None:
            print("no %s.png: the page will have no %s layer" % (name, name))
            continue
        im = Image.open(path).convert("RGBA")
        if im.size == (480, 320):
            im = rotate(im)
        if im.size != (PAGE_W, PAGE_H):
            ap.error("%s.png has to be 480x320 or 320x480, not %dx%d" % ((name,) + im.size))
        im.save(os.path.join(args.out, name + ".png"))

    art = None
    if args.static:
        im = Image.open(args.static).convert("RGBA")
        if im.size == (480, 320):
            im = rotate(im)
        if im.size != (PAGE_W, PAGE_H):
            ap.error("--static has to be 480x320 or 320x480, not %dx%d" % im.size)
        overlay = im
    else:
        if args.pieces:
            pieces = {n: read_png(os.path.join(args.pieces, n + ".png"))
                      for n in ("reflection", "sticker", "window", "plate")}
        else:
            pieces, art = pieces_from_blender(args.blender_dir, args.render)
        overlay = compose(pieces)

    if args.hub:
        art = Image.open(args.hub).convert("RGBA")
    elif art is None:
        ap.error("no hub drawing: pass --static with --hub, or the Blender project")

    overlay.save(os.path.join(args.out, "overlay.png"))
    hub_png(art).save(os.path.join(args.out, "hub.png"))

    for name in ("overlay", "hub", "shadow", "light"):
        p = os.path.join(args.out, name + ".png")
        if os.path.exists(p):
            print(f"{p}: {Image.open(p).size}, {os.path.getsize(p)} bytes")


if __name__ == "__main__":
    sys.exit(main())
