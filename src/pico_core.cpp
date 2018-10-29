#include "pico_core.h"
#include "pico_memory.h"

#include <assert.h>
#include <iostream>

static pico_api::colour_t backbuffer_store[128 * 256];
static pico_api::colour_t* backbuffer = &backbuffer_store[128 * 64];
static pico_api::colour_t* backbuffer_guard1 = &backbuffer_store[0];
static pico_api::colour_t* backbuffer_guard2 = &backbuffer_store[128 * 192];

static int buffer_size_x = 128;
static int buffer_size_y = 128;

struct InputState {
	uint8_t old = 0;
	uint8_t current = 0;
	uint8_t repcnt = 0;

	void set(int state) {
		old = current;
		current = state;

		if (justPressed()) {
			repcnt = 16;
		}

		repcnt--;
		if (repcnt == 0)
			repcnt = 4;
	}

	bool isPressed(uint8_t key) {
		return ((current >> key) & 1) == 1;
	}

	uint8_t isPressed() {
		return current;
	}

	bool justPressed(uint8_t key) {
		return (justPressed() >> key) & 1;
	}

	uint8_t justPressed() {
		return ~old & current;
	}

	bool justPressedRpt(uint8_t key) {
		return justPressed(key) || (isPressed(key) && (repcnt == 1));
	}

	uint8_t justReleased() {
		return old & ~current;
	}

	bool justReleased(uint8_t key) {
		return (justReleased() >> key) & 1;
	}
};

static InputState inputState[4];
static MouseState mouseState;

struct GraphicsState {
	pico_api::colour_t fg = 7;
	pico_api::colour_t bg = 0;
	uint16_t pattern = 0;
	int text_x = 0;
	int text_y = 0;
	int clip_x1 = 0;
	int clip_y1 = 0;
	int clip_x2 = 128;
	int clip_y2 = 128;
	int camera_x = 0;
	int camera_y = 0;
	pico_api::colour_t palette_map[16];
	bool transparent[16];
};

static GraphicsState graphicsState;
static GraphicsState* currentGraphicsState = &graphicsState;

struct SpriteSheet {
	pico_api::colour_t sprite_data[128 * 128];
	uint8_t flags[256];
};

static SpriteSheet fontSheet;
static SpriteSheet spriteSheet;
static SpriteSheet* currentSprData = &spriteSheet;

struct MapSheet {
	uint8_t map_data[128 * 64];
};

static uint8_t cart_data[pico_ram::MEM_CART_DATA_SIZE] = {0};
static uint8_t scratch_data[pico_ram::MEM_SCRATCH_SIZE] = {0};

static MapSheet mapSheet;
static MapSheet* currentMapData = &mapSheet;

static pico_ram::RAM ram;
static pico_ram::SplitNibbleMemoryArea mem_gfx(spriteSheet.sprite_data,
                                               pico_ram::MEM_GFX_ADDR,
                                               pico_ram::MEM_GFX_SIZE);
static pico_ram::SplitNibbleMemoryArea mem_gfx2(spriteSheet.sprite_data + 128 * 64,
                                                pico_ram::MEM_GFX2_MAP2_ADDR,
                                                pico_ram::MEM_GFX2_MAP2_SIZE);
static pico_ram::LinearMemoryArea mem_map2(mapSheet.map_data + 128 * 32,
                                           pico_ram::MEM_GFX2_MAP2_ADDR,
                                           pico_ram::MEM_GFX2_MAP2_SIZE);
static pico_ram::DualMemoryArea mem_gfx2_map2(&mem_map2,
                                              &mem_gfx2);  // shared memory between gfx2 & map2
static pico_ram::LinearMemoryArea mem_map(mapSheet.map_data,
                                          pico_ram::MEM_MAP_ADDR,
                                          pico_ram::MEM_MAP_SIZE);
static pico_ram::LinearMemoryArea mem_flags(spriteSheet.flags,
                                            pico_ram::MEM_GFX_PROPS_ADDR,
                                            pico_ram::MEM_GFX_PROPS_SIZE);
static pico_ram::SplitNibbleMemoryArea mem_screen(backbuffer,
                                                  pico_ram::MEM_SCREEN_ADDR,
                                                  pico_ram::MEM_SCREEN_SIZE);

static pico_ram::SplitNibbleMemoryArea mem_font(fontSheet.sprite_data, 0x8000, 0x2000);

static pico_ram::LinearMemoryAreaDF mem_cart_data(cart_data,
                                                  pico_ram::MEM_CART_DATA_ADDR,
                                                  pico_ram::MEM_CART_DATA_SIZE);

static pico_ram::LinearMemoryArea mem_scratch_data(scratch_data,
                                                   pico_ram::MEM_SCRATCH_ADDR,
                                                   pico_ram::MEM_SCRATCH_SIZE);

namespace pico_private {
	using namespace pico_api;

	void init_guards() {
		for (size_t i = 0; i < 128 * 64; i++) {
			backbuffer_guard1[i] = i;
			backbuffer_guard2[i] = i;
		}
	}

	void check_guards() {
		for (size_t i = 0; i < 128 * 64; i++) {
			assert(backbuffer_guard1[i] == (i & 0xff));
			assert(backbuffer_guard2[i] == (i & 0xff));
		}
	}

	static void restore_palette() {
		for (colour_t n = 0; n < 16; n++) {
			currentGraphicsState->palette_map[n] = n;
		}
	}

	static void restore_transparency() {
		for (size_t n = 0; n < 16; n++) {
			currentGraphicsState->transparent[n] = false;
		}
		currentGraphicsState->transparent[0] = true;
	}

	static void clip_axis(int& dest_pos, int& src_pos, int& len, int min, int max) {
		if (dest_pos < min) {
			len = len - (min - dest_pos);
			src_pos += (min - dest_pos);
			dest_pos = min;
		}

		if ((dest_pos + len) >= max) {
			len = len - (dest_pos + len - max);
		}

		if (len < 0) {
			len = 0;
		}
	}

	// test if rectangle is within cliping rectangle
	static bool is_visible(int x, int y, int w, int h) {
		if (x >= currentGraphicsState->clip_x2)
			return false;
		if (y >= currentGraphicsState->clip_y2)
			return false;
		if (x + w <= currentGraphicsState->clip_x1)
			return false;
		if (y + h <= currentGraphicsState->clip_y1)
			return false;

		return true;
	}

	static void blitter(SpriteSheet& sprites,
	                    int scr_x,
	                    int scr_y,
	                    int spr_x,
	                    int spr_y,
	                    int w,
	                    int h,
	                    bool flip_x = false,
	                    bool flip_y = false) {
		if (!is_visible(scr_x, scr_y, w, h))
			return;

		clip_axis(scr_x, spr_x, w, currentGraphicsState->clip_x1, currentGraphicsState->clip_x2);
		clip_axis(scr_y, spr_y, h, currentGraphicsState->clip_y1, currentGraphicsState->clip_y2);

		colour_t* spr;
		int spr_dy;
		if (!flip_y) {
			spr = sprites.sprite_data + spr_y * 128 + spr_x;
			spr_dy = 128;
		} else {
			spr = sprites.sprite_data + (spr_y + h - 1) * 128 + spr_x;
			spr_dy = -128;
		}

		colour_t* pix = backbuffer + scr_y * buffer_size_x + scr_x;
		for (int y = 0; y < h; y++) {
			if (!flip_x) {
				for (int x = 0; x < w; x++) {
					colour_t c = spr[x];
					if (!currentGraphicsState->transparent[c]) {
						pix[x] = currentGraphicsState->palette_map[c];
					}
				}
			} else {
				for (int x = 0; x < w; x++) {
					colour_t c = spr[h - x - 1];
					if (!currentGraphicsState->transparent[c]) {
						pix[x] = currentGraphicsState->palette_map[c];
					}
				}
			}

			pix += buffer_size_x;
			spr += spr_dy;
		}
	}

	static void stretch_blitter(SpriteSheet& sprites,
	                            int spr_x,
	                            int spr_y,
	                            int spr_w,
	                            int spr_h,
	                            int scr_x,
	                            int scr_y,
	                            int scr_w,
	                            int scr_h,
	                            bool flip_x = false,
	                            bool flip_y = false) {
		if (!is_visible(scr_x, scr_y, scr_w, scr_h))
			return;

		int dx = (spr_w << 16) / scr_w;
		int dy = (spr_h << 16) / scr_h;

		spr_x = spr_x << 16;
		spr_y = spr_y << 16;

		// left clip
		if (scr_x < currentGraphicsState->clip_x1) {
			int nclip = currentGraphicsState->clip_x1 - scr_x;
			scr_x = currentGraphicsState->clip_x1;
			scr_w -= nclip;
			spr_x += nclip * dx;
		}

		// right clip
		if (scr_x + scr_w > currentGraphicsState->clip_x2) {
			int nclip = (scr_x + scr_w) - currentGraphicsState->clip_x2;
			scr_w -= nclip;
		}

		// top clip
		if (scr_y < currentGraphicsState->clip_y1) {
			int nclip = currentGraphicsState->clip_y1 - scr_y;
			scr_y = currentGraphicsState->clip_y1;
			scr_h -= nclip;
			spr_y += nclip * dy;
		}

		// bottom clip
		if (scr_y + scr_h > currentGraphicsState->clip_y2) {
			int nclip = (scr_y + scr_h) - currentGraphicsState->clip_y2;
			scr_h -= nclip;
		}

		colour_t* pix = backbuffer + scr_y * buffer_size_x + scr_x;

		for (int y = 0; y < scr_h; y++) {
			colour_t* spr = sprites.sprite_data + ((spr_y + y * dy) >> 16) * 128;

			for (int x = 0; x < scr_w; x++) {
				colour_t c = spr[(spr_x + x * dx) >> 16];
				if (!currentGraphicsState->transparent[c]) {
					pix[x] = currentGraphicsState->palette_map[c];
				}
			}
			pix += buffer_size_x;
		}
	}

	static int clip_rect(int& x0, int& y0, int& x1, int& y1) {
		int flags = 0;

		if (x0 < currentGraphicsState->clip_x1) {
			x0 = currentGraphicsState->clip_x1;
			flags |= 1;
		}
		if (y0 < currentGraphicsState->clip_y1) {
			y0 = currentGraphicsState->clip_y1;
			flags |= 2;
		}
		if (x1 >= currentGraphicsState->clip_x2) {
			x1 = currentGraphicsState->clip_x2 - 1;
			flags |= 4;
		}
		if (y1 >= currentGraphicsState->clip_y2) {
			y1 = currentGraphicsState->clip_y2 - 1;
			flags |= 8;
		}
		return 0;
	}

	template <typename T>
	T limit(T n, T min, T max) {
		if (n < min)
			return min;
		if (n > max)
			return max;
		return n;
	}

	inline void normalise_coords(int& c0, int& c1) {
		if (c0 > c1)
			std::swap(c0, c1);
	}

	void hline(int x0, int x1, int y) {
		normalise_coords(x0, x1);
		x1++;
		if (y < currentGraphicsState->clip_y1 || y >= currentGraphicsState->clip_y2) {
			return;
		}
		x0 = limit(x0, currentGraphicsState->clip_x1, currentGraphicsState->clip_x2);
		x1 = limit(x1, currentGraphicsState->clip_x1, currentGraphicsState->clip_x2);

		colour_t fg = currentGraphicsState->palette_map[currentGraphicsState->fg];
		colour_t bg = currentGraphicsState->palette_map[currentGraphicsState->bg];

		colour_t* pix = backbuffer + y * buffer_size_x;
		uint16_t pat = currentGraphicsState->pattern;

		if (pat == 0) {
			memset(pix + x0, fg, x1 - x0);
		} else if (pat == 0xffff) {
			memset(pix + x0, bg, x1 - x0);
		} else {
			for (int x = x0; x < x1; x++) {
				pix[x] = ((pat >> ((3 - (x & 0x3)) + (3 - (y & 0x3)) * 4)) & 1) ? bg : fg;
			}
		}
	}

	void vline(int y0, int y1, int x) {
		if (x < currentGraphicsState->clip_x1 || x >= currentGraphicsState->clip_x2) {
			return;
		}

		y0 = limit(y0, currentGraphicsState->clip_y1, currentGraphicsState->clip_y2);
		y1 = limit(y1, currentGraphicsState->clip_y1, currentGraphicsState->clip_y2);

		colour_t* pix = backbuffer + y0 * buffer_size_x;

		colour_t fg = currentGraphicsState->palette_map[currentGraphicsState->fg];
		colour_t bg = currentGraphicsState->palette_map[currentGraphicsState->bg];
		uint16_t pat = currentGraphicsState->pattern;

		for (int y = y0; y < y1; y++) {
			pix[x] = ((pat >> ((3 - (x & 0x3)) + (3 - (y & 0x3)) * 4)) & 1) ? bg : fg;
			pix += buffer_size_x;
		}
	}

	void pset(int x, int y) {
		if (x < currentGraphicsState->clip_x1 || x >= currentGraphicsState->clip_x2 ||
		    y < currentGraphicsState->clip_y1 || y >= currentGraphicsState->clip_y2) {
			return;
		}

		colour_t* pix = backbuffer + y * buffer_size_x + x;
		uint16_t pat = currentGraphicsState->pattern;
		colour_t fg = currentGraphicsState->palette_map[currentGraphicsState->fg];
		colour_t bg = currentGraphicsState->palette_map[currentGraphicsState->bg];

		if (pat == 0) {
			*pix = fg;
		} else if (pat == 0xffff) {
			*pix = bg;
		} else {
			*pix = ((pat >> ((3 - (x & 0x3)) + (3 - (y & 0x3)) * 4)) & 1) ? bg : fg;
		}
	}

	void apply_camera(int& x, int& y) {
		x = x - currentGraphicsState->camera_x;
		y = y - currentGraphicsState->camera_y;
	}

}  // namespace pico_private

namespace pico_control {
	void init(int x, int y) {
		buffer_size_x = x;
		buffer_size_y = y;

		pico_private::restore_palette();
		pico_private::restore_transparency();

		ram.addMemoryArea(&mem_gfx);
		ram.addMemoryArea(&mem_gfx2_map2);
		ram.addMemoryArea(&mem_map);
		ram.addMemoryArea(&mem_flags);
		ram.addMemoryArea(&mem_screen);
		ram.addMemoryArea(&mem_font);
		ram.addMemoryArea(&mem_cart_data);
		ram.addMemoryArea(&mem_scratch_data);

		pico_private::init_guards();
	}

	pico_api::colour_t* get_buffer(int& width, int& height) {
		width = buffer_size_x;
		height = buffer_size_y;
		pico_private::check_guards();
		return backbuffer;
	}

	void copy_data_to_ram(uint16_t addr, const std::string& data, bool gfx = true) {
		for (size_t n = 0; n < data.length(); n++) {
			char buf[3] = {0};

			if (data[n] > ' ') {
				if (gfx) {
					buf[1] = data[n++];
					buf[0] = data[n];
				} else {
					buf[0] = data[n++];
					buf[1] = data[n];
				}
				pico_api::poke(addr++, strtol(buf, nullptr, 16));
			}
		}
	}

	void set_sprite_data(std::string data, std::string flags) {
		copy_data_to_ram(pico_ram::MEM_GFX_ADDR, data);
		copy_data_to_ram(pico_ram::MEM_GFX_PROPS_ADDR, flags, false);
	}

	void set_font_data(std::string data) {
		copy_data_to_ram(0x8000, data);
	}

	void set_map_data(std::string data) {
		copy_data_to_ram(pico_ram::MEM_MAP_ADDR, data, false);
		// ram.dump(0x0000, 0x4000);
	}

	void set_input_state(int state, int player) {
		inputState[player].set(state);
	}

	void set_mouse_state(const MouseState& ms) {
		mouseState = ms;
	}

	void test_integrity() {
		pico_private::check_guards();
	}
}  // namespace pico_control

namespace pico_api {

	void color(uint8_t c) {
		currentGraphicsState->fg = c & 0xf;
		currentGraphicsState->bg = c >> 4;
	}

	void cls(colour_t c) {
		colour_t p = currentGraphicsState->palette_map[c];
		memset(backbuffer, p, buffer_size_x * buffer_size_y);

		currentGraphicsState->text_x = 0;
		currentGraphicsState->text_y = 0;
	}

	uint8_t peek(uint16_t a) {
		return ram.peek(a);
	}

	uint32_t peek4(uint16_t a) {
		uint32_t v = peek(a);
		v |= uint32_t(peek(a + 1)) << 8;
		v |= uint32_t(peek(a + 2)) << 16;
		v |= uint32_t(peek(a + 3)) << 24;
		return v;
	}

	void poke(uint16_t a, uint8_t v) {
		ram.poke(a, v);
	}

	void poke4(uint16_t a, uint32_t v) {
		ram.poke(a, v);
		ram.poke(a + 1, v >> 8);
		ram.poke(a + 2, v >> 16);
		ram.poke(a + 3, v >> 24);
	}

	uint32_t dget(uint16_t a) {
		return peek4(pico_ram::MEM_CART_DATA_ADDR + ((a * 4) & 0xff));
	}

	void dset(uint16_t a, uint32_t v) {
		poke4(pico_ram::MEM_CART_DATA_ADDR + ((a * 4) & 0xff), v);
	}

	void cls() {
		cls(0);
	}

	uint8_t fget(int n) {
		return currentSprData->flags[n & 0xff];
	}

	bool fget(int n, int bit) {
		return (fget(n) >> bit) & 1;
	}

	void fset(int n, uint8_t val) {
		currentSprData->flags[n & 0xff] = val;
	}

	void fset(int n, int bit, bool val) {
		if (val)
			fset(n, fget(n) | (1 << bit));
		else
			fset(n, fget(n) & ~(1 << bit));
	}

	void spr(int n, int x, int y) {
		spr(n, x, y, 1, 1, false, false);
	}

	void spr(int n, int x, int y, int w, int h) {
		spr(n, x, y, w, h, false, false);
	}

	void spr(int n, int x, int y, int w, int h, bool flip_x, bool flip_y) {
		pico_private::apply_camera(x, y);

		int spr_x = (n % 16) * 8;
		int spr_y = (n / 16) * 8;
		pico_private::blitter(*currentSprData, x, y, spr_x, spr_y, w * 8, h * 8, flip_x, flip_y);
	}

	void sspr(int sx, int sy, int sw, int sh, int dx, int dy) {
		pico_private::apply_camera(dx, dy);
		pico_private::blitter(*currentSprData, dx, dy, sx, sy, sw, sh);
	}

	void sspr(int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh) {
		pico_private::apply_camera(dx, dy);
		pico_private::stretch_blitter(*currentSprData, sx, sy, sw, sh, dx, dy, dw, dh);
	}

	void
	sspr(int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, bool flip_x, bool flip_y) {
		pico_private::apply_camera(dx, dy);
		pico_private::stretch_blitter(*currentSprData, sx, sy, sw, sh, dx, dy, dw, dh, flip_x,
		                              flip_y);
	}

	colour_t sget(int x, int y) {
		y &= 0x7f;
		x &= 0x7f;
		return currentSprData->sprite_data[y * 128 + x];
	}

	void sset(int x, int y) {
		sset(x, y, currentGraphicsState->fg);
	}

	void sset(int x, int y, colour_t c) {
		y &= 0x7f;
		x &= 0x7f;
		currentSprData->sprite_data[y * 128 + x] = c;
	}

	void pset(int x, int y) {
		pset(x, y, currentGraphicsState->fg);
	}

	void pset(int x, int y, colour_t c) {
		color(c);
		pico_private::apply_camera(x, y);
		pico_private::pset(x, y);
	}

	void rect(int x0, int y0, int x1, int y1) {
		rect(x0, y0, x1, y1, currentGraphicsState->fg);
	}

	void rect(int x0, int y0, int x1, int y1, colour_t c) {
		pico_private::apply_camera(x0, y0);
		pico_private::apply_camera(x1, y1);
		color(c);
		pico_private::normalise_coords(x0, x1);
		pico_private::normalise_coords(y0, y1);
		pico_private::hline(x0, x1, y0);
		pico_private::hline(x0, x1, y1);
		pico_private::vline(y0, y1, x0);
		pico_private::vline(y0, y1, x1);
	}

	void rectfill(int x0, int y0, int x1, int y1) {
		rectfill(x0, y0, x1, y1, currentGraphicsState->fg);
	}

	void rectfill(int x0, int y0, int x1, int y1, colour_t c) {
		pico_private::apply_camera(x0, y0);
		pico_private::apply_camera(x1, y1);
		color(c);
		pico_private::normalise_coords(x0, x1);
		pico_private::normalise_coords(y0, y1);

		pico_private::clip_rect(x0, y0, x1, y1);
		colour_t* pix = backbuffer + y0 * buffer_size_x;
		colour_t p1 = currentGraphicsState->palette_map[c & 0x0f];
		colour_t p2 = currentGraphicsState->palette_map[(c >> 4) & 0x0f];

		uint16_t pat = currentGraphicsState->pattern;

		for (int y = y0; y <= y1; y++) {
			for (int x = x0; x <= x1; x++) {
				pix[x] = ((pat >> ((3 - (x & 0x3)) + (3 - (y & 0x3)) * 4)) & 1) ? p2 : p1;
			}
			pix += buffer_size_x;
		}
	}

	void circ(int x, int y, int r) {
		circ(x, y, r, currentGraphicsState->fg);
	}

	void circ(int xm, int ym, int r, colour_t c) {
		pico_private::apply_camera(xm, ym);
		color(c);
		if (r >= 0) {
			int x = -r, y = 0, err = 2 - 2 * r; /* II. Quadrant */
			do {
				pico_private::pset(xm - x, ym + y); /*   I. Quadrant */
				pico_private::pset(xm - y, ym - x); /*  II. Quadrant */
				pico_private::pset(xm + x, ym - y); /* III. Quadrant */
				pico_private::pset(xm + y, ym + x); /*  IV. Quadrant */
				r = err;
				if (r > x)
					err += ++x * 2 + 1; /* e_xy+e_x > 0 */
				if (r <= y)
					err += ++y * 2 + 1; /* e_xy+e_y < 0 */
			} while (x < 0);
		}
	}

	void circfill(int x, int y, int r) {
		circfill(x, y, r, currentGraphicsState->fg);
	}

	void circfill(int xm, int ym, int r, colour_t c) {
		pico_private::apply_camera(xm, ym);
		color(c);
		if (r == 0) {
			pico_private::pset(xm, ym);
		} else if (r == 1) {
			pico_private::pset(xm, ym - 1);
			pico_private::hline(xm - 1, xm + 1, ym);
			pico_private::pset(xm, ym + 1);
		} else if (r > 0) {
			int x = -r, y = 0, err = 2 - 2 * r;
			do {
				pico_private::hline(xm - x, xm + x, ym + y);
				pico_private::hline(xm - x, xm + x, ym - y);
				r = err;
				if (r > x)
					err += ++x * 2 + 1;
				if (r <= y)
					err += ++y * 2 + 1;
			} while (x < 0);
		}
	}

	void line(int x0, int y0, int x1, int y1) {
		line(x0, y0, x1, y1, currentGraphicsState->fg);
	}

	void line(int x0, int y0, int x1, int y1, colour_t c) {
		pico_private::apply_camera(x0, y0);
		pico_private::apply_camera(x1, y1);
		color(c);
		int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
		int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
		int err = dx + dy, e2; /* error value e_xy */

		for (;;) { /* loop */
			pico_private::pset(x0, y0);
			if (x0 == x1 && y0 == y1)
				break;
			e2 = 2 * err;
			if (e2 >= dy) {
				err += dy;
				x0 += sx;
			} /* e_xy+e_x > 0 */
			if (e2 <= dx) {
				err += dx;
				y0 += sy;
			} /* e_xy+e_y < 0 */
		}
	}

	void map(int cell_x, int cell_y) {
		map(cell_x, cell_y, 0, 0);
	}

	void map(int cell_x, int cell_y, int scr_x, int scr_y) {
		map(cell_x, cell_y, scr_x, scr_y, 16, 16);
	}

	void map(int cell_x, int cell_y, int scr_x, int scr_y, int cell_w, int cell_h) {
		map(cell_x, cell_y, scr_x, scr_y, cell_w, cell_h, 0);
	}

	void map(int cell_x, int cell_y, int scr_x, int scr_y, int cell_w, int cell_h, uint8_t layer) {
		for (int y = 0; y < cell_h; y++) {
			for (int x = 0; x < cell_w; x++) {
				uint8_t cell = mget(cell_x + x, cell_y + y);
				if (cell && ((layer == 0) || ((fget(cell) & layer) == layer))) {
					spr(cell, scr_x + x * 8, scr_y + y * 8);
				}
			}
		}
	}

	uint8_t mget(int x, int y) {
		return currentMapData->map_data[y * 128 + x];
	}

	void mset(int x, int y, uint8_t v) {
		currentMapData->map_data[y * 128 + x] = v;
	}

	void pal(colour_t c0, colour_t c1) {
		currentGraphicsState->palette_map[c0 & 0xf] = c1 & 0xf;
	}

	void pal() {
		pico_private::restore_palette();
		pico_private::restore_transparency();
	}

	void palt(colour_t col, bool t) {
		currentGraphicsState->transparent[col] = t;
	}

	void palt() {
		pico_private::restore_transparency();
	}

	void print(std::string str) {
		print(str, currentGraphicsState->text_x, currentGraphicsState->text_y);
	}

	void print(std::string str, int x, int y) {
		print(str, x, y, currentGraphicsState->fg);
	}

	void print(std::string str, int x, int y, colour_t c) {
		pico_private::apply_camera(x, y);
		color(c);

		colour_t old = currentGraphicsState->palette_map[7];
		bool oldt = currentGraphicsState->transparent[0];

		currentGraphicsState->palette_map[7] = c & 0xf;
		currentGraphicsState->transparent[0] = true;

		for (size_t n = 0; n < str.length(); n++) {
			uint8_t ch = str[n];
			if (ch >= 0x20 && ch < 0x80) {
				int index = ch - 32;
				pico_private::blitter(fontSheet, x, y, (index % 32) * 4, (index / 32) * 6, 4, 5);
				x += 4;
			} else if (ch >= 0x80 && ch <= 0x99) {
				int index = ch - 0x80;
				pico_private::blitter(fontSheet, x, y, (index % 16) * 8, (index / 16) * 6 + 18, 8,
				                      5);
				x += 8;
			} else if (ch == '\n') {
				x = 0;
				y += 6;
			}
		}

		currentGraphicsState->text_x = 0;
		currentGraphicsState->text_y = y + 6;

		currentGraphicsState->palette_map[7] = old;
		currentGraphicsState->transparent[0] = oldt;

		currentGraphicsState->fg = c & 0xf;
	}

	int btn() {
		return inputState[0].current;
	}

	int btn(int n, int player) {
		return inputState[player].isPressed(n);
	}

	int btnp() {
		return inputState[0].justPressed();  // TODO: impl repeat on this
	}

	int btnp(int n, int player) {
		return inputState[player].justPressedRpt(n);
	}

	void clip(int x, int y, int w, int h) {
		currentGraphicsState->clip_x1 = x;
		currentGraphicsState->clip_y1 = y;
		currentGraphicsState->clip_x2 = x + w;
		currentGraphicsState->clip_y2 = y + h;
	}

	void clip() {
		clip(0, 0, 128, 128);
	}

	void camera() {
		camera(0, 0);
	}

	void camera(int x, int y) {
		currentGraphicsState->camera_x = x;
		currentGraphicsState->camera_y = y;
	}

	void fillp() {
		fillp(0);
	}

	void fillp(int pattern) {
		currentGraphicsState->pattern = pattern;
	}

	int stat(int key, std::string& sval, int& ival) {
		switch (key) {
			case 32:
				ival = mouseState.x;
				return 2;
			case 33:
				ival = mouseState.y;
				return 2;
			case 34:
				ival = mouseState.buttons;
				return 2;
			case 36:
				ival = mouseState.wheel;
				return 2;
		}

		ival = 0;
		return 2;
	}

}  // namespace pico_api
