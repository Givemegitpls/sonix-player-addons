#pragma once

#include <cstdint>
#include <string>

// A picture the game draws.
//
// Upstream this was an SDL_Surface decoded from a PNG embedded in the binary by
// cmrc. On the card the PNGs are files in the addon's own assets/ and are decoded
// with stb_image, which the player exports: the pixels become RGB565 and, for an
// image that has holes in it, one byte of coverage per pixel. Every PNG the game
// ships is either fully opaque or fully transparent per pixel, so the coverage is
// 0 or 255 and `alpha()` is NULL for the ones with no hole -- the same shape
// Flappy Bird's sprites have.
//
// The names the game asks for are upstream's ("fender.png", "sprites.png"), which
// is why the files keep their names on the card.

class Image {
private:
    int w = 0;
    int h = 0;
    uint16_t* px = nullptr;
    uint8_t* mask = nullptr;

public:
    explicit Image(const std::string& embeddedPath);
    Image(int width, int height);
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    int getWidth() const;
    int getHeight() const;
    const uint16_t* pixels() const;
    const uint8_t* alpha() const;
};
