#include "lcdui/Font.h"

#include <stdio.h>

#include "font_data.h"

// Upstream this was SDL_ttf: an RWops over the TTF embedded by cmrc, a TTF_Font
// per (style, size), and TTF_SizeText for every width the menus asked for. What
// the game asks for is fixed -- 12, 16 and 32 pixels, plain and bold -- so the
// faces are baked (tools/bake_font.py) and this is a lookup: the advance of a
// string is the sum of the baked advances, which is what keeps the layouts
// shaped, and the ascent and descent are the ones FreeType reported when the
// masks were made.

Font::Font(FontStyle style, FontSize pointSize)
{
    int real = getRealFontSize(pointSize);
    f = gd_face_lookup(real, style == STYLE_BOLD ? 1 : 0);
    height = real;

    if (!f) {
        // A style the baker did not produce: italic halves are unused, so fall
        // back to the plain face rather than drawing nothing.
        f = gd_face_lookup(real, 0);
    }
    if (!f) {
        printf("gravity: no baked face for %d px\n", real);
    }
}

Font::~Font() = default;

int Font::getBaselinePosition() const
{
    return height;
}

int Font::getHeight() const
{
    return height;
}

const gd_face *Font::face() const
{
    return f;
}

int Font::charWidth(char c)
{
    return stringWidth(std::string(1, c));
}

int Font::stringWidth(const std::string &s)
{
    if (!f) {
        return 0;
    }
    int width = 0;
    for (char c : s) {
        if (c < 32 || c > 126) {
            continue;
        }
        width += f->glyphs[c - 32].advance;
    }
    return width;
}

int Font::substringWidth(const std::string &string, int offset, int len)
{
    return stringWidth(string.substr((size_t)offset, (size_t)len));
}

int Font::getRealFontSize(FontSize size)
{
    switch (size) {
    case SIZE_LARGE:
        return 32;
    case SIZE_MEDIUM:
        return 16;
    case SIZE_SMALL:
        return 12;
    default:
        return 16;
    }
}
