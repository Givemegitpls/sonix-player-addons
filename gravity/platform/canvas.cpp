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
#include "touch.h"
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

// One frame in the interface thread's hands at a time, and never a queue of them.
//
// The game draws thirty frames a second whether or not the interface has looked
// at the last one, and gui_post() has a finite queue: without this the queue
// fills ("gui: message queue full, dropping one") and the shell starts dropping
// the messages other parts of the player send. A frame is a level, not an event
// -- the newest one is the only one worth showing -- so the flag says "already
// posted" and the post is skipped while it is set.
static volatile bool frame_pending;

static void (*exit_cb)(void *);
static void *exit_user;

static void exit_on_ui(void *user)
{
    (void)user;
    if (exit_cb) {
        exit_cb(exit_user);
    }
}

static void frame_on_ui(void *user)
{
    (void)user;
    frame_pending = false;
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
    if (frame_pending) {
        return; // the interface has not taken the last one yet
    }
    frame_pending = true;
    if (!gui_post(frame_on_ui, NULL)) {
        frame_pending = false; // the queue was full; the next frame tries again
    }
}

// Upstream polled SDL's event queue here and turned keys into keyPressed and
// keyReleased. The zones are the same two calls: what is pressed, released or
// held is the difference between the state now and the last one applied.
//
// The centre strip is FIRE, and that is what selects: MenuManager::processKeyCode
// sends case 8 to processGameActionUpd(1), and a menu row reads "one" as the press
// that activates it -- SettingsStringRender::menuElemMethod calls processMenu()
// for 1 only when the row is one of the play menu's ("Start>", "Level:Easy"), so a
// tap with RIGHT there does nothing at all. While riding, FIRE is a key the game
// does not read.
static const uint16_t zone_bits[5] = { GD_KEY_UP, GD_KEY_DOWN, GD_KEY_LEFT, GD_KEY_RIGHT, GD_KEY_FIRE };
static int applied_key[5] = { -1, -1, -1, -1, -1 };

static int key_for_bit(int i)
{
	switch (i) {
	case 0:
		return Canvas::Keys::UP; // gas
	case 1:
		return Canvas::Keys::DOWN; // brake
	case 2:
		return Canvas::Keys::LEFT; // lean back
	case 3:
		return Canvas::Keys::RIGHT; // lean forward
	default:
		return Canvas::Keys::FIRE; // the centre strip: select in a menu
	}
}

static void apply_state(Canvas *c, uint16_t mask);

void CanvasImpl::processEvents()
{
	uint16_t states[GTOUCH_MAX_STATES];
	int count = gtouch_poll(states, GTOUCH_MAX_STATES);
	for (int s = 0; s < count; s++) {
		apply_state(canvas, states[s]);
	}
}

// One state of the glass: what the keys are now, against what they were.
static void apply_state(Canvas *c, uint16_t mask)
{

	for (int i = 0; i < 5; i++) {
		bool down = (mask & zone_bits[i]) != 0;
		int want = down ? key_for_bit(i) : -1;
		if (want == applied_key[i]) {
			continue;
		}
		if (applied_key[i] >= 0) {
			c->publicKeyReleased(applied_key[i]);
		}
		if (want >= 0) {
			c->publicKeyPressed(want);
		}
		applied_key[i] = want;
	}

	// The strip held down is the game's own menu command: upstream's Escape key,
	// which runs the BACK command the canvas registered (Canvas::pressedEsc).
	static bool menu_down;
	if ((mask & GD_KEY_MENU) && !menu_down) {
		c->pressedEsc();
	}
	menu_down = (mask & GD_KEY_MENU) != 0;
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

    // The game's own loop has ended. Whatever the addon wants to do about it has
    // to happen on the interface thread, which is the only place LVGL may be
    // touched.
    if (exit_cb) {
        gui_post(exit_on_ui, NULL);
    }
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

    // The panel becomes the gamepad. On the device that means LVGL's indev is
    // parked and the evdev node is read here (platform/touch.c); on the simulator
    // the page feeds the pointer in and this only says so. A failure is not fatal:
    // the game runs, with nothing to press.
    if (!gtouch_open(NULL)) {
        printf("gravity: the game has no controls\n");
    }

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
    gtouch_close();
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

void gravity_exit_cb_set(void (*cb)(void *), void *user)
{
    exit_cb = cb;
    exit_user = user;
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
