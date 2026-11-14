#pragma once

#include <string>

// A face the game draws with.
//
// Upstream this was an SDL_ttf font opened from FontSansSerif.ttf, and the game
// asked it for the width of a string so that menus could be laid out around it.
// There is no FreeType in an addon, so the faces the game actually uses are baked
// at 12, 16 and 32 pixels and kept as coverage masks (gravity/tools/bake_font.py,
// platform/font_data.c). `stringWidth` is the sum of the baked advances, which is
// what makes the layouts keep their shape.
//
// FontStorage caches one of these per (style, size) pair, so the object is cheap
// and owns nothing but a pointer into the table.

struct gd_face;

class Font {
public:
    enum FontSize {
        SIZE_SMALL = 8,
        SIZE_MEDIUM = 0,
        SIZE_LARGE = 16
    };

    enum FontStyle {
        STYLE_PLAIN = 0,
        STYLE_BOLD = 1,
        STYLE_ITALIC = 2
    };

    enum FontFace {
        FACE_SYSTEM = 0
    };

    Font(FontStyle style, FontSize pointSize);
    ~Font();

    int getBaselinePosition() const;
    int getHeight() const;
    int charWidth(char c);
    int stringWidth(const std::string& s);
    int substringWidth(const std::string& string, int offset, int len);
    const gd_face* face() const;

private:
    const gd_face* f;
    int height;

    static int getRealFontSize(FontSize size);
};
