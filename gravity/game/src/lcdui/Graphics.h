#pragma once

#include <memory>
#include <stdexcept>

#include "Image.h"
#include "Font.h"

// The drawing surface the game was written against.
//
// Upstream this was a hand around an SDL_Renderer: every call went to SDL and
// `renderer` was the window's. Here the same calls land in platform/gfx, which
// writes RGB565 pixels into the addon's one frame. The class shape is upstream's
// -- the game calls these methods, and platform/graphics.cpp answers them -- with
// the SDL types swapped for the target structure; see NOTICE.md for the list.
//
// An anchor is a bitmask of the Anchors below, exactly as lcdui specified them:
// drawImage(image, x, y, Graphics::HCENTER | Graphics::TOP) puts the image's top
// and horizontal centre at (x, y).

struct gfx_target;

class Graphics {
private:
    gfx_target* target;
    std::shared_ptr<Font> font;

public:
    enum Anchors {
        HCENTER = 1,
        VCENTER = 2,
        LEFT = 4,
        RIGHT = 8,
        TOP = 16,
        BOTTOM = 32,
        BASELINE = 64
    };

    explicit Graphics(gfx_target* target);
    void drawString(const std::string& s, int x, int y, int anchor);
    void setColor(int r, int g, int b);
    void setFont(std::shared_ptr<Font> font);
    std::shared_ptr<Font> getFont() const;
    void setClip(int x, int y, int w, int h);
    void drawChar(char c, int x, int y, int anchor);
    void fillRect(int x, int y, int w, int h);
    void fillArc(int x, int y, int w, int h, int startAngle, int arcAngle);
    void drawArc(int x, int y, int w, int h, int startAngle, int arcAngle);
    void drawLine(int x1, int y1, int x2, int y2);
    void drawImage(Image* const image, int x, int y, int anchor);
    static int getAnchorX(int x, int size, int anchor);
    static int getAnchorY(int y, int size, int anchor);
};
