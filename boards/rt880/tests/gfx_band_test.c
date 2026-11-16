// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Band-rendering regression: the same pattern drawn through gfx must reconstruct identically whether the frame buffer
// holds the whole screen (GFX_BAND_ROWS=0) or one band at a time (GFX_BAND_ROWS>0). This file is compiled twice (see
// the Makefile's test target) and the two hex dumps are diffed; any difference means the banded path drifted.
#include "gfx.h"
#include "font.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define W 120
#define H 160
#define STRIDE 60
static uint16_t cap[H][W];	// the reconstructed full frame

static void flush_indexed(const void *fb, const uint16_t *pal, uint16_t y0, uint16_t y1)
{
	const uint8_t *base = fb;	// absolute-indexed, base + y*STRIDE (banded mode offsets base below its buffer)
	for (uint16_t y = y0; y < y1; y++) {
		const uint8_t *row = base + (size_t)y * STRIDE;
		for (int x = 0; x < W; x++)
			cap[y][x] = pal[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 0xF];
	}
}
static const hal_display_t disp = { .width = W, .height = H, .bpp = 4, .flush_indexed = flush_indexed };

// gfx draws text/icons through font_glyph; this test paints only fills and pixels, so a null glyph is enough to link.
const uint8_t *font_glyph(const font_t *f, uint8_t c, int *top, int *rows)
{
	(void)f; (void)c;
	if (top) *top = 0;
	if (rows) *rows = 0;
	return 0;
}

// Rectangles that cross the 40-row band boundaries, several palette colours, overlap, and a per-row diagonal.
static void draw(void)
{
	gfx_clear(0x0000);
	gfx_fill(0,  10, 120, 60, 0xF800);	// red, rows 10..70 (crosses 40)
	gfx_fill(10, 35, 100, 50, 0x07E0);	// green, 35..85 (crosses 40, 80)
	gfx_fill(20, 70, 80,  60, 0x001F);	// blue, 70..130 (crosses 80, 120)
	gfx_fill(40, 115, 40, 30, 0xFFE0);	// yellow, 115..145 (crosses 120)
	for (int i = 0; i < 160; i++)
		gfx_pixel(i % 120, i, 0xFFFF);	// white diagonal, one pixel per row
}

int main(void)
{
	if (!gfx_init(&disp)) {
		fprintf(stderr, "gfx_init failed\n");
		return 2;
	}
	memset(cap, 0xAB, sizeof cap);
	gfx_render(draw);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			printf("%04x", cap[y][x]);
	printf("\n");
	return 0;
}
