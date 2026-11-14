#include "lcdui/Graphics.h"

#include <string>

#include "font_data.h"
#include "gfx.h"
#include "surface.h"

// lcdui's Graphics on top of platform/gfx: the calls the game makes, the anchoring
// rules lcdui specified, and nothing else. Upstream this file was 271 lines of SDL
// -- RenderCopy with a clip rect for the sprite cutting, TTF_RenderText_Blended
// for the text, SDL_RenderDrawPoint for the arcs.

Graphics::Graphics(gfx_target* target)
    : target(target)
{
}

void Graphics::drawString(const std::string& s, int x, int y, int anchor)
{
    if (!font) {
        return;
    }
    const gd_face* f = font->face();
    if (!f) {
        return;
    }

    int width = font->stringWidth(s);
    int height = f->line_height;
    x = getAnchorX(x, width, anchor);
    y = getAnchorY(y, height, anchor);

    // y is the top of the line; the glyphs are placed from the baseline.
    int baseline = y + f->ascent;
    int pen = x;
    for (char c : s) {
        if (c < 32 || c > 126) {
            continue;
        }
        const gd_glyph_t& g = f->glyphs[c - 32];
        if (g.w && g.h) {
            gfx_glyph(target, f->coverage + g.off, g.w, g.h, pen + g.left, baseline + g.top);
        }
        pen += g.advance;
    }
}

void Graphics::setColor(int r, int g, int b)
{
    gfx_set_color(target, r, g, b);
}

void Graphics::setFont(std::shared_ptr<Font> font)
{
    this->font = font;
}

std::shared_ptr<Font> Graphics::getFont() const
{
    return font;
}

void Graphics::setClip(int x, int y, int w, int h)
{
    gfx_set_clip(target, x, y, w, h);
}

void Graphics::drawChar(char c, int x, int y, int anchor)
{
    drawString(std::string(1, c), x, y, anchor);
}

void Graphics::fillRect(int x, int y, int w, int h)
{
    gfx_fill_rect(target, x, y, w, h);
}

void Graphics::fillArc(int x, int y, int w, int h, int startAngle, int arcAngle)
{
    gfx_fill_arc(target, x, y, w, h, startAngle, arcAngle);
}

void Graphics::drawArc(int x, int y, int w, int h, int startAngle, int arcAngle)
{
    gfx_draw_arc(target, x, y, w, h, startAngle, arcAngle);
}

void Graphics::drawLine(int x1, int y1, int x2, int y2)
{
    gfx_draw_line(target, x1, y1, x2, y2);
}

void Graphics::drawImage(Image* const image, int x, int y, int anchor)
{
    if (!image || !image->pixels()) {
        return;
    }
    x = getAnchorX(x, image->getWidth(), anchor);
    y = getAnchorY(y, image->getHeight(), anchor);
    gfx_blit(target, image->pixels(), image->alpha(), image->getWidth(), image->getHeight(), x, y);
}

int Graphics::getAnchorX(int x, int size, int anchor)
{
    if ((anchor & LEFT) != 0) {
        return x;
    }
    if ((anchor & RIGHT) != 0) {
        return x - size;
    }
    if ((anchor & HCENTER) != 0) {
        return x - size / 2;
    }
    // Upstream threw here. The game passes anchors from its own tables, so a
    // missing one is a bug upstream would have shown as a crash on the first
    // frame; drawing at x keeps the screen usable and the log short.
    return x;
}

int Graphics::getAnchorY(int y, int size, int anchor)
{
    if ((anchor & TOP) != 0) {
        return y;
    }
    if ((anchor & BOTTOM) != 0) {
        return y - size;
    }
    if ((anchor & VCENTER) != 0) {
        return y - size / 2;
    }
    return y;
}
