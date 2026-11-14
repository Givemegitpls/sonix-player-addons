#include "lcdui/Image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdexcept>
#include <string>

#include "assets.h"

// stb_image, which the player exports (Flappy Bird loads its artwork the same
// way). The addon does not carry its own copy: `-Wl,--dynamic-list` on the
// player's link is what makes these names resolvable at dlopen() time.
extern "C" {
unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *channels_in_file,
                                     int desired_channels);
void stbi_image_free(void *retval_from_stbi_load);
}

// Where the game's pictures are: <addon>/assets, set by gravity_run() before the
// game thread starts. The game asks for "fender.png", which is the name upstream
// embedded, so the files keep it.
static std::string assets_dir;

void image_set_assets_dir(const char *dir)
{
    assets_dir = dir ? dir : "";
}

static uint16_t pack565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static uint8_t *read_file(const std::string &path, long *size_out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return nullptr;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return nullptr;
    }
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4 * 1024 * 1024) {
        fclose(f);
        return nullptr;
    }

    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fclose(f);
        return nullptr;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return nullptr;
    }
    fclose(f);
    *size_out = size;
    return data;
}

Image::Image(int width, int height)
    : w(width)
    , h(height)
{
    if (w <= 0 || h <= 0) {
        w = h = 0;
        return;
    }
    px = (uint16_t *)calloc((size_t)w * (size_t)h, sizeof(uint16_t));
}

Image::Image(const std::string &embeddedPath)
{
    std::string path = assets_dir + "/" + embeddedPath;
    long size = 0;
    uint8_t *file = read_file(path, &size);
    if (!file) {
        throw std::runtime_error("Image: " + path + ": cannot be read");
    }

    int iw = 0, ih = 0, channels = 0;
    unsigned char *rgba = stbi_load_from_memory(file, (int)size, &iw, &ih, &channels, 4);
    free(file);
    if (!rgba || iw <= 0 || ih <= 0) {
        stbi_image_free(rgba);
        throw std::runtime_error("Image: " + path + ": not a picture stb_image can decode");
    }

    w = iw;
    h = ih;
    size_t n = (size_t)w * (size_t)h;
    px = (uint16_t *)malloc(n * sizeof(uint16_t));
    uint8_t *holes = (uint8_t *)malloc(n);
    if (!px || !holes) {
        stbi_image_free(rgba);
        free(px);
        free(holes);
        px = nullptr;
        w = h = 0;
        return;
    }

    bool any_hole = false;
    for (size_t i = 0; i < n; i++) {
        const unsigned char *s = rgba + i * 4;
        px[i] = pack565(s[0], s[1], s[2]);
        uint8_t keep = s[3] >= 128 ? 1 : 0;
        holes[i] = keep;
        any_hole = any_hole || !keep;
    }
    stbi_image_free(rgba);

    if (any_hole) {
        mask = holes;
    } else {
        free(holes);
    }
}

Image::~Image()
{
    free(px);
    free(mask);
}

int Image::getWidth() const { return w; }
int Image::getHeight() const { return h; }
const uint16_t *Image::pixels() const { return px; }
const uint8_t *Image::alpha() const { return mask; }
