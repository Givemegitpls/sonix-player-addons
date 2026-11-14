#pragma once

#include <string>

struct gfx_target;

// The window, in lcdui's vocabulary.
//
// Upstream this owned an SDL window and the renderer the game drew into, polled
// SDL's event queue for keys, and presented with SDL_RenderPresent. There is no
// window on the card: this owns the frame the game draws into, turns the touch
// panel into key presses and releases, and hands the finished frame to the
// player's interface thread (platform/canvas.cpp).
//
// The size is the panel's, held sideways: 480x320. The game reads it through
// Canvas::getWidth()/getHeight() and lays itself out around it, which is how the
// PSP port ran it at 480x272.

class Canvas;

class CanvasImpl {
private:
    Canvas* canvas;

public:
    CanvasImpl(Canvas* canvas);
    ~CanvasImpl();

    void repaint();
    int getWidth();
    int getHeight();
    void processEvents();
    void setWindowTitle(const std::string& title);
    gfx_target* getTarget();
};
