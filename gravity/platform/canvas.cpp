#include "lcdui/CanvasImpl.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "gravity.h"
#include "gfx.h"
#include "surface.h"

#include "lcdui/Canvas.h"

// The player's bridge to the interface thread (src/gui/shell/gui.h). Declared here
// rather than included: that header is C, its symbols are unmangled, and a C++
// include of it would make this file ask the player for a mangled gui_post that
// does not exist -- an "undefined symbol" at dlopen time.
extern "C" bool gui_post(void (*cb)(void *), void *user);

#include "assets.h"
#include "Micro.h"

#define ADDON_PATH_MAX_LOCAL 512

// The window, in lcdui's vocabulary, and the thread the game runs on.
//
// Upstream CanvasImpl made an SDL window, polled SDL's event queue and presented
// with SDL_RenderPresent. Here it owns nothing: the frame is platform/surface's,
// the keys come from the touch zones (stage two, see processEvents), and
// repaint() is the hand-over to the interface thread.
//
// The game is upstream's Micro: its startApp() sets up the canvas, loads the
// levels, runs the three-second loading sequence and then loops for ever, pacing
// itself with Time::sleep. That loop is why the game has a thread: the loop is
// not coroutine-shaped and rewriting it into a step function would be a
// divergence from upstream in the middle of the game's own logic.

static void (*frame_cb)(void *);
static void *frame_user;

static pthread_t game_thread_id;
static bool thread_started;
static volatile bool running;

static char saves_dir[ADDON_PATH_MAX_LOCAL];
static char levels_path[ADDON_PATH_MAX_LOCAL * 2];

static void frame_on_ui(void *user)
{
    (void)user;
    if (frame_cb) {
        frame_cb(frame_user);
    }
}

CanvasImpl::CanvasImpl(Canvas *canvas)
    : canvas(canvas)
{
}

CanvasImpl::~CanvasImpl() = default;

int CanvasImpl::getWidth()
{
    return surface_width();
}

int CanvasImpl::getHeight()
{
    return surface_height();
}

gfx_target *CanvasImpl::getTarget()
{
    return surface();
}

void CanvasImpl::setWindowTitle(const std::string &title)
{
    (void)title;
}

// Called by Canvas::repaint() on the game thread, once per finished frame.
void CanvasImpl::repaint()
{
    surface_hand_over();
    gui_post(frame_on_ui, NULL);
}

// Upstream polled SDL here. The touch panel's zones are stage two of the port;
// until they exist the game runs with no key ever pressed.
void CanvasImpl::processEvents()
{
}

static void *game_thread(void *arg)
{
    (void)arg;

    try {
        Micro micro;
        // Upstream's argv: [0] is where the records go (RecordStore's directory,
        // patched to read it from here), [1] the levels file. Both are the
        // addon's own files on the card.
        char *argv[3];
        argv[0] = saves_dir;
        argv[1] = levels_path;
        argv[2] = NULL;
        micro.startApp(2, argv);
    } catch (const std::exception &e) {
        printf("gravity: the game stopped: %s\n", e.what());
    } catch (...) {
        printf("gravity: the game stopped on an unknown exception\n");
    }

    running = false;
    return NULL;
}

bool gravity_run(const char *addon_dir)
{
    if (running) {
        return true;
    }
    if (!addon_dir || !addon_dir[0]) {
        return false;
    }

    snprintf(saves_dir, sizeof(saves_dir), "%s/saves", addon_dir);
    snprintf(levels_path, sizeof(levels_path), "%s/assets/levels.mrg", addon_dir);

    char assets[ADDON_PATH_MAX_LOCAL * 2];
    snprintf(assets, sizeof(assets), "%s/assets", addon_dir);
    image_set_assets_dir(assets);

    mkdir(saves_dir, 0755);
    surface_init();

    // Micro::field_249 is the game's own "keep going": startApp() sets it, run()
    // loops on it, and destroyApp() clears it. It is how the game is asked to
    // stop from outside, and gravity_quit() is the only other caller.
    Micro::field_249 = true;

    running = true;
    if (pthread_create(&game_thread_id, NULL, game_thread, NULL) != 0) {
        running = false;
        printf("gravity: no thread for the game\n");
        return false;
    }
    thread_started = true;
    return true;
}

void gravity_quit(void)
{
    if (thread_started) {
        Micro::field_249 = false;
        pthread_join(game_thread_id, NULL);
        thread_started = false;
    }
    running = false;
    surface_finish();
}

bool gravity_running(void)
{
    return running;
}

void gravity_frame_cb_set(void (*cb)(void *), void *user)
{
    frame_cb = cb;
    frame_user = user;
}

void gravity_frame_copy(uint16_t *dst)
{
    if (dst) {
        surface_copy(dst);
    }
}

int gravity_width(void)
{
    return surface_width();
}

int gravity_height(void)
{
    return surface_height();
}
