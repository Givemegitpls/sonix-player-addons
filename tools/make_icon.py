#!/usr/bin/env python3
"""Composes a tile icon in the player's own style, for an addon that has none
of its own (or one that does not fit).

The tile is the drawing every compiled-in menu tile is: an upright rounded
square carrying a white glyph, and behind it, on top, the same square a few
degrees off and translucent, so the two read as one stacked object. The colour
is a diagonal gradient of one hue; the glyph is monochrome and comes from any
SVG (Lucide, simple-icons, a traced logo) or from a PNG.

    python3 tools/make_icon.py --glyph sonix-player/sonix-player/assets/icons/brand/spotify.svg \
        --hue 141 --out spotify/icon.png

Nothing here runs during a build: an addon's tile is a 140x140 PNG on the card,
loaded at scan time by icon_load() in the player's src/addon/addon.c and scaled into
SC(111) -- 74 px on the 320x480 panel. That box is the number that matters, so
--preview draws the result at both sizes, beside tiles that already exist.

Rasterising goes through whichever of cairosvg, rsvg-convert or ImageMagick is
present; none of them is needed to build the player.
"""

import argparse
import base64
import io
import math
import os
import re
import shutil
import subprocess
import sys

# Geometry of the tile, as fractions of the side. Measured off the tiles that
# are already in the player's icon set (tidal, gearboy, flappy-bird): the upright card
# carries the glyph and is the translucent of the two, the rotated one lies
# over it, opaque, which is what makes the seam read.
CARD = 0.60  # side of one card
RADIUS = 0.25  # corner radius, as a fraction of the card's side
FRONT = (0.475, 0.607)  # nominal centre of the upright card, the one with the glyph
BACK_DX = 0.14  # how far the rotated card sits to the side -- either side, see below
BACK_DY = 0.170  # and how far above the upright one
BACK_ROT = 13.0  # its rotation, degrees
FRONT_ALPHA = 0.78  # translucent, so the rotated card reads through it
GLYPH_BOX = 0.64  # the glyph's longest side, as a fraction of the card
GLYPH_OPACITY = 0.94  # the stock glyphs are off-white, not paper white
# The glyph is white, and stays white. The stock set does use light tints of the
# card's own hue on the coloured tiles (music's cream note, radio's pale rose),
# but at the size that matters -- 74 px, SC(111) -- a tint at lightness 0.92 is
# indistinguishable from white, and pushing it further costs the one thing the
# glyph is for: reading the tile in a row. --glyph-colour takes any colour for
# the tiles that want one, and --mono is the dark brand pairing (tidal, qobuz).

# Where a tile's cards actually sit: the nominal places above, moved by a stable
# per-tile offset. Two tiles drawn a day apart used to be pixel-identical in
# placement, which is what made a row of them look machine-made; a real set is
# not that tidy. The seed is the glyph's name, so the same command writes the
# same file (no diff noise in git) while different tiles differ.
#
# Which side the rotated card sits on is part of that: in the stock set it is
# up-left of the upright one in music and radio, up-right in settings, and a row
# of tiles that all lean one way reads as a template rather than a set. The
# rotation flips with the side, so the two cards stay one tilted object.
JITTER_XY = 0.022  # fraction of the canvas, each way (the x axis)
# Next to nothing on y: the top and bottom edges of a row of stock tiles line up
# (top 15 px, bottom 13 px on a 140 px file), and a few px of vertical drift is
# what makes a set look hand-placed in the wrong way. The side and the sideways
# offset carry the variety instead.
JITTER_Y = 0.006
JITTER_ROT = 2.2  # degrees, on the rotated card

# Gradient stops, light at the top left and dark at the bottom right. Two pairs:
# the upright card is the washed-out face, the rotated one the saturated face,
# which is the pairing the checked-in tiles use (see gearboy, ebook, flappy).
#
# The values are matched against the stock tiles (tidal.png, qobuz.png): both
# cards stay dark, the back one darker still and only its saturated edge reads,
# and the glyph is what carries the colour of a pale tile. The first version was
# a stop lighter on both cards, and a row of those looked like pastel stickers
# next to the menu.
LIGHT_TOP, LIGHT_BOTTOM = 0.62, 0.46
SAT_LIGHT, SAT_DARK = 0.62, 0.70
BACK_LIGHT_TOP, BACK_LIGHT_BOTTOM = 0.46, 0.26
BACK_SAT_TOP, BACK_SAT_BOTTOM = 0.95, 0.92


def placement(seed):
    """One tile's offsets: front x/y, back x/y, back rotation, and its side.

    The six are (jx, jy, jbx, jby, jrot, side); `side` is +1 when the rotated
    card sits to the right of the upright one and -1 when it sits to the left.
    Stable for a seed and different between seeds; see JITTER_XY.
    """
    import hashlib
    import random

    digest = hashlib.sha256(str(seed or "").encode("utf-8")).hexdigest()
    rnd = random.Random(int(digest[:8], 16))
    side = rnd.choice((-1, 1))
    jfx = rnd.uniform(-JITTER_XY, JITTER_XY)
    jbx = rnd.uniform(-JITTER_XY, JITTER_XY)
    jy = rnd.uniform(-JITTER_Y, JITTER_Y)
    jby = rnd.uniform(-JITTER_Y, JITTER_Y)
    return (jfx, jy, jbx, jby, rnd.uniform(-JITTER_ROT, JITTER_ROT), side)


def _clamp(value, low, high):
    return low if value < low else high if value > high else value


def hsl_to_rgb(h, s, lightness):
    """HSL -> '#rrggbb'. h in degrees, s and l in 0..1."""
    import colorsys

    r, g, b = colorsys.hls_to_rgb((h % 360) / 360.0, lightness, s)
    return "#%02x%02x%02x" % (round(r * 255), round(g * 255), round(b * 255))


# ---------------------------------------------------------------------------
# The glyph
# ---------------------------------------------------------------------------


def _root_attrs(svg):
    """The opening <svg ...> tag's attributes, minus the ones that place it."""
    m = re.search(r"<svg\b([^>]*)>", svg, re.S)
    if not m:
        sys.exit("not an SVG: no <svg> element")
    attrs = {}
    for name, value in re.findall(r'([\w:-]+)\s*=\s*"([^"]*)"', m.group(1)):
        attrs[name] = value
    return attrs


def _inner(svg):
    """Everything inside the root <svg> element."""
    start = svg.find(">", re.search(r"<svg\b", svg).start()) + 1
    end = svg.rfind("</svg>")
    body = svg[start:end]
    # A <title>/<desc> is metadata, and librsvg would draw nothing for it; drop
    # it so the wrapper's fill does not apply to a stray element.
    body = re.sub(r"<title\b.*?</title>", "", body, flags=re.S)
    body = re.sub(r"<desc\b.*?</desc>", "", body, flags=re.S)
    # potrace writes its one fill as an explicit black on an inner <g>, which
    # would win over the wrapper's colour and paint the glyph's box black.
    body = re.sub(r'(fill|stroke)="#000000"', r'\1="currentColor"', body)
    body = re.sub(r"(fill|stroke):\s*#000000", r"\1:currentColor", body)
    return body


def _viewbox(attrs, fallback=(0, 0, 24, 24)):
    vb = attrs.get("viewBox") or attrs.get("viewbox")
    if vb:
        parts = [float(p) for p in re.split(r"[\s,]+", vb.strip())]
        if len(parts) == 4:
            return tuple(parts)
    try:
        return (0.0, 0.0, float(attrs["width"]), float(attrs["height"]))
    except (KeyError, ValueError):
        return fallback


def _glyph_markup(path, colour, silhouette=False):
    """The glyph as a <g>, in its own units, centred on its viewBox.

    The SVG's own root attributes are carried onto the wrapper, which is what
    keeps a Lucide icon a stroke drawing (fill="none" stroke="currentColor"
    stroke-width="2") and a simple-icons one a filled path. Sizing is not done
    here: potrace output, a Lucide glyph and a hand-made one all leave a
    different amount of empty margin around the ink, so the scale comes from a
    measured bounding box (fit_glyph).
    """
    if path.lower().endswith(".png"):
        from PIL import Image  # only needed for PNG glyphs

        im = Image.open(path).convert("RGBA")
        if silhouette:
            # A coloured sprite used as a mark: its alpha becomes the shape and
            # the fill is the glyph colour, which is what makes a game's bird
            # read as white on a saturated tile instead of as its own palette.
            solid = Image.new("RGBA", im.size, colour)
            solid.putalpha(im.getchannel("A"))
            buf = io.BytesIO()
            solid.save(buf, format="PNG")
            img_data = base64.b64encode(buf.getvalue()).decode("ascii")
        else:
            img_data = base64.b64encode(open(path, "rb").read()).decode("ascii")
        w, h = im.size
        return (
            '<g transform="translate(%f %f)">'
            '<image x="0" y="0" width="%d" height="%d" href="data:image/png;base64,%s"'
            ' style="image-rendering:pixelated"/></g>'
        ) % (-w / 2.0, -h / 2.0, w, h, img_data)

    svg = open(path, "r", encoding="utf-8").read()
    attrs = _root_attrs(svg)
    vx, vy, vw, vh = _viewbox(attrs)

    carry = []
    for name in (
        "fill",
        "stroke",
        "stroke-width",
        "stroke-linecap",
        "stroke-linejoin",
        "stroke-miterlimit",
    ):
        if name in attrs:
            carry.append('%s="%s"' % (name, attrs[name]))
    # currentColor, and the case of an icon with no fill of its own (a filled
    # path, simple-icons' shape), both have to end up `colour`.
    carry.append('color="%s" style="color:%s"' % (colour, colour))
    if not any(c.startswith("fill=") for c in carry):
        carry.append('fill="%s"' % colour)

    return '<g transform="translate(%f %f)" %s>%s</g>' % (
        -vx - vw / 2.0,
        -vy - vh / 2.0,
        " ".join(carry),
        _inner(svg),
    )


def fit_glyph(path, box, colour, scale=1.0, silhouette=False):
    """The glyph, scaled so its ink fills `box` px, centred on (0,0).

    The ink's bounding box is measured, not assumed: the glyph is drawn once on
    a probe canvas and its alpha read back, which is the only way to make a
    traced logo and a Lucide icon come out the same size on the tile.
    """
    markup = _glyph_markup(path, colour, silhouette)
    probe_size = 512
    probe = (
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
        '<g transform="translate(%f %f)">%s</g></svg>'
        % (
            probe_size,
            probe_size,
            probe_size,
            probe_size,
            probe_size / 2.0,
            probe_size / 2.0,
            markup,
        )
    )
    png = rasterise(probe, probe_size)

    from PIL import Image

    alpha = Image.open(io.BytesIO(png)).convert("RGBA").getchannel("A")
    bbox = alpha.point(lambda v: 255 if v > 8 else 0).getbbox()
    if not bbox:
        sys.exit("the glyph draws nothing: %s" % path)

    x0, y0, x1, y1 = bbox
    mid = probe_size / 2.0
    cx, cy = (x0 + x1) / 2.0 - mid, (y0 + y1) / 2.0 - mid
    k = box * scale / max(x1 - x0, y1 - y0)
    return '<g transform="scale(%f) translate(%f %f)">%s</g>' % (k, -cx, -cy, markup)


# ---------------------------------------------------------------------------
# The tile
# ---------------------------------------------------------------------------


def tile_svg(
    size,
    hue,
    glyph,
    colour="#ffffff",
    mono=False,
    glyph_scale=1.0,
    rim=0.18,
    seed=None,
    silhouette=False,
):
    """The whole tile as one SVG document. `seed` places the two cards."""
    a = size * CARD
    r = a * RADIUS
    jfx, jfy, jbx, jby, jrot, side = placement(seed)
    # The rotated card reaches further than its side: half of it times
    # cos + sin of the angle. Neither card may leave the canvas -- a clipped
    # corner reads as a mistake, not as a tile -- so the centres are clamped.
    rad = math.radians(BACK_ROT)
    reach = a / 2.0 * (math.cos(rad) + math.sin(rad))
    # The pair is what is centred, not the upright card: with the rotated one on
    # the left the glyph card has to move right by half the offset, or the tile
    # visibly hangs to one side of its 140 px square.
    fcx = _clamp((FRONT[0] - side * BACK_DX / 2.0 + jfx) * size, a / 2.0, size - a / 2.0)
    fcy = _clamp((FRONT[1] + jfy) * size, a / 2.0, size - a / 2.0)
    bcx = _clamp(fcx + (side * BACK_DX + jbx) * size, reach, size - reach)
    bcy = _clamp(fcy - (BACK_DY + jby) * size, reach, size - reach)
    box = a * GLYPH_BOX

    if mono:
        top, bottom = "#4a4a52", "#232329"
        back_top, back_bottom = "#61616b", "#1b1b20"
    else:
        top = hsl_to_rgb(hue, SAT_LIGHT, LIGHT_TOP)
        bottom = hsl_to_rgb(hue, SAT_DARK, LIGHT_BOTTOM)
        back_top = hsl_to_rgb(hue, BACK_SAT_TOP, BACK_LIGHT_TOP)
        back_bottom = hsl_to_rgb(hue, BACK_SAT_BOTTOM, BACK_LIGHT_BOTTOM)

    def card(cx, cy, gradient, rot=0.0, opacity=1.0):
        return (
            '<g transform="translate(%f %f) rotate(%f)" opacity="%f">'
            '<rect x="%f" y="%f" width="%f" height="%f" rx="%f" fill="url(#%s)"/>'
            "</g>" % (cx, cy, rot, opacity, -a / 2, -a / 2, a, a, r, gradient)
        )

    return """<svg xmlns="http://www.w3.org/2000/svg" width="{s}" height="{s}" viewBox="0 0 {s} {s}">
<defs>
 <linearGradient id="front" x1="0" y1="0" x2="1" y2="1">
  <stop offset="0" stop-color="{top}"/><stop offset="1" stop-color="{bottom}"/>
 </linearGradient>
 <linearGradient id="backg" x1="0" y1="0" x2="1" y2="1">
  <stop offset="0" stop-color="{back_top}"/><stop offset="1" stop-color="{back_bottom}"/>
 </linearGradient>
 <filter id="shine" x="-20%" y="-20%" width="140%" height="140%">
  <feDropShadow dx="0" dy="{shadow}" stdDeviation="{blur}" flood-color="#000" flood-opacity="0.22"/>
 </filter>
</defs>
<g filter="url(#shine)">
 {back}
 {front}
 <rect x="{rim_x}" y="{rim_y}" width="{rim_w}" height="{rim_h}" rx="{r}" fill="none"
       stroke="#fff" stroke-opacity="{rim_opacity}" stroke-width="{rim_sw}"/>
</g>
<g transform="translate({gcx} {gcy})" opacity="{glyph_opacity}">{glyph}</g>
</svg>
""".format(
        s=size,
        top=top,
        bottom=bottom,
        back_top=back_top,
        back_bottom=back_bottom,
        shadow=size * 0.012,
        blur=size * 0.018,
        # The upright card first, translucent, then the rotated one over it:
        # the order the checked-in tiles use, and the reason the seam reads as
        # two faces of one object rather than as a shadow.
        front=card(fcx, fcy, "front", 0.0, FRONT_ALPHA),
        back=card(bcx, bcy, "backg", side * (BACK_ROT + jrot), 1.0),
        rim_x=fcx - a / 2,
        rim_y=fcy - a / 2,
        rim_w=a,
        rim_h=a,
        rim_sw=max(1.0, size * 0.011),
        rim_opacity=rim,
        r=r,
        gcx=fcx,
        gcy=fcy,
        glyph=fit_glyph(glyph, box, colour, glyph_scale, silhouette),
        glyph_opacity=GLYPH_OPACITY,
    )


def rasterise(svg_text, size):
    """SVG text -> PNG bytes, through whatever is installed."""
    try:
        import cairosvg
    except ImportError:
        pass
    else:
        return cairosvg.svg2png(
            bytestring=svg_text.encode("utf-8"), output_width=size, output_height=size
        )

    if shutil.which("rsvg-convert"):
        return subprocess.run(
            ["rsvg-convert", "-w", str(size), "-h", str(size)],
            input=svg_text.encode("utf-8"),
            stdout=subprocess.PIPE,
            check=True,
        ).stdout

    magick = shutil.which("magick") or shutil.which("convert")
    if magick:
        cmd = [
            magick,
            "-background",
            "none",
            "-density",
            "96",
            "svg:-",
            "-resize",
            "%dx%d" % (size, size),
            "png:-",
        ]
        return subprocess.run(
            cmd, input=svg_text.encode("utf-8"), stdout=subprocess.PIPE, check=True
        ).stdout

    sys.exit("needs one of cairosvg, rsvg-convert or ImageMagick")


def preview(png, size, sheet):
    """Write the icon at the size it is drawn and at SC(111), in a strip."""
    from PIL import Image

    icon = Image.open(io.BytesIO(png)).convert("RGBA")
    small = round(size * 2 / 3)
    icon_small = icon.resize((small, small), Image.LANCZOS)
    pad = 8
    sheet_img = Image.new(
        "RGBA", (size + small + pad * 3, size + pad * 2), (32, 32, 36, 255)
    )
    sheet_img.alpha_composite(icon, (pad, pad))
    sheet_img.alpha_composite(icon_small, (size + pad * 2, pad + (size - small) // 2))
    sheet_img.save(sheet)
    return sheet


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--glyph", required=True, help="SVG or PNG to draw on the tile")
    ap.add_argument("--out", help="PNG to write; omitted with --svg-only")
    ap.add_argument(
        "--size",
        type=int,
        default=140,
        help="side in pixels (default 140, the size the check-in tiles are)",
    )
    ap.add_argument("--hue", type=float, default=205.0, help="hue of the tile, degrees")
    ap.add_argument("--mono", action="store_true", help="the dark grey tile (tidal's)")
    ap.add_argument("--glyph-colour", default="#ffffff", help="colour of the glyph")
    ap.add_argument(
        "--glyph-scale",
        type=float,
        default=1.0,
        help="multiply the glyph's fitted size (default 1.0)",
    )
    ap.add_argument(
        "--rim",
        type=float,
        default=0.18,
        help="brightness of the card's light edge, 0..1 (default 0.18)",
    )
    ap.add_argument(
        "--glyph-silhouette",
        action="store_true",
        help="a PNG glyph used as a mask: its alpha in --glyph-colour "
        "(a coloured sprite, e.g. a game's bird)",
    )
    ap.add_argument(
        "--seed",
        default=None,
        help="place the cards from this seed instead of the glyph's name "
        "(the default: the same command writes the same file)",
    )
    ap.add_argument(
        "--preview", help="also write this sheet: the tile at size and at SC(111)"
    )
    ap.add_argument(
        "--svg-only", action="store_true", help="print the tile's SVG, write nothing"
    )
    args = ap.parse_args()

    if not os.path.exists(args.glyph):
        sys.exit("no such glyph: %s" % args.glyph)

    svg_text = tile_svg(
        args.size,
        args.hue,
        args.glyph,
        colour=args.glyph_colour,
        mono=args.mono,
        glyph_scale=args.glyph_scale,
        rim=args.rim,
        seed=args.seed if args.seed is not None else os.path.basename(args.glyph),
        silhouette=args.glyph_silhouette,
    )

    if args.svg_only:
        print(svg_text)
        return

    png = rasterise(svg_text, args.size)
    if not args.out:
        sys.exit("--out is required unless --svg-only is given")
    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(png)
    print("wrote %s (%d bytes)" % (out, len(png)))

    if args.preview:
        print("preview: %s" % preview(png, args.size, args.preview))


if __name__ == "__main__":
    main()
