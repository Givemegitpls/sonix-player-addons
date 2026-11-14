#include "surface.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

// The panel's own pixels with the player held sideways: 320x480 of glass, so
// 480 across and 320 down. The game's drawing is 1:1 with the glass; the shell
// scales the frame up for LVGL's 480x720 layout, which the player then scales
// back down to the panel (see gravity/addon.c). Keeping the game's coordinates
// equal to the physical ones is what makes the sprites and the baked text land
// on pixel boundaries.
#define FRAME_W 480
#define FRAME_H 320

static gfx_target draw;
static uint16_t *draw_px;
static uint16_t *hand_px;
static pthread_mutex_t hand_lock = PTHREAD_MUTEX_INITIALIZER;

void surface_init(void) {
	size_t bytes = (size_t)FRAME_W * (size_t)FRAME_H * sizeof(uint16_t);
	if (!draw_px) {
		draw_px = (uint16_t *)malloc(bytes);
	}
	if (!hand_px) {
		hand_px = (uint16_t *)malloc(bytes);
	}
	if (!draw_px || !hand_px) {
		printf("gravity: no frame buffer (%zu bytes each)\n", bytes);
		exit(1);
	}
	memset(draw_px, 0, bytes);
	memset(hand_px, 0, bytes);
	gfx_init(&draw, draw_px, FRAME_W, FRAME_H);
}

void surface_finish(void) {
	free(draw_px);
	free(hand_px);
	draw_px = NULL;
	hand_px = NULL;
	draw.px = NULL;
}

gfx_target *surface(void) { return &draw; }
int surface_width(void) { return FRAME_W; }
int surface_height(void) { return FRAME_H; }

void surface_hand_over(void) {
	pthread_mutex_lock(&hand_lock);
	memcpy(hand_px, draw_px, (size_t)FRAME_W * (size_t)FRAME_H * sizeof(uint16_t));
	pthread_mutex_unlock(&hand_lock);
}

void surface_copy(uint16_t *dst) {
	pthread_mutex_lock(&hand_lock);
	memcpy(dst, hand_px, (size_t)FRAME_W * (size_t)FRAME_H * sizeof(uint16_t));
	pthread_mutex_unlock(&hand_lock);
}
