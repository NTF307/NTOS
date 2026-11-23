/*
 * console.c - text console on the UEFI framebuffer.
 * Cells are 6x10 font pixels (5x9 glyph + spacing), scaled by an integer factor.
 * Once the PMM is up, a RAM shadow buffer makes scrolling fast (no VRAM reads).
 */
#include "console.h"
#include "theme.h"
#include "mm.h"
#include "lib.h"
#include "sched.h"

#define CELL_W 6
#define CELL_H 10

static uint8_t *fb;          /* visible framebuffer (HHDM virtual) */
static uint8_t *back;        /* shadow copy in RAM, or NULL */
static uint32_t width, height, pitch, format, scale;
static uint32_t cols, rows, cx, cy;
static uint32_t fg = THEME_FG, bg = THEME_BG;
static bool     cursor_visible, ready;
static mutex_t  lock;                     /* all public drawing entry points take it */

/* Mouse pointer: drawn on the visible framebuffer only, so the back buffer
 * always holds the real screen contents and erasing is a copy back. */
static int32_t  ptr_x, ptr_y;
static bool     ptr_visible;
static const char *const pointer_shape[] = {
    "X",
    "XX",
    "X.X",
    "X..X",
    "X...X",
    "X....X",
    "X.....X",
    "X......X",
    "X.......X",
    "X........X",
    "X.........X",
    "X......XXXXX",
    "X...X..X",
    "X..XX..X",
    "X.X  X..X",
    "XX   X..X",
    "X     X..X",
    "      X..X",
    "       XX",
};
#define PTR_W 12
#define PTR_H ((uint32_t)ARRAY_SIZE(pointer_shape))

static inline uint32_t to_pixel(uint32_t rgb)
{
    if (format == NTOS_FB_BGRX) return rgb;
    return ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}

static void put_span(uint32_t x, uint32_t y, const uint32_t *px, uint32_t n)
{
    uint64_t off = (uint64_t)y * pitch + (uint64_t)x * 4;
    memcpy(fb + off, px, n * 4);
    if (back) memcpy(back + off, px, n * 4);
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    uint32_t line[64], c = to_pixel(rgb);
    for (uint32_t i = 0; i < 64; i++) line[i] = c;
    for (uint32_t r = 0; r < h; r++)
        for (uint32_t done = 0; done < w; done += 64)
            put_span(x + done, y + r, line, MIN(64, w - done));
}

static void draw_cell(uint32_t col, uint32_t row, char ch)
{
    const uint8_t *g = font_glyph(ch);
    uint32_t f = to_pixel(fg), b = to_pixel(bg);
    uint32_t px = col * CELL_W * scale, py = row * CELL_H * scale;
    uint32_t line[CELL_W * 4];

    for (uint32_t cyy = 0; cyy < CELL_H; cyy++) {
        uint8_t bits = (cyy >= 1 && cyy <= FONT_GLYPH_H) ? g[cyy - 1] : 0;   /* row 0 is spacing */
        for (uint32_t cxx = 0; cxx < CELL_W; cxx++) {
            bool on = cxx < FONT_GLYPH_W && (bits >> (FONT_GLYPH_W - 1 - cxx)) & 1;
            for (uint32_t s = 0; s < scale; s++) line[cxx * scale + s] = on ? f : b;
        }
        for (uint32_t s = 0; s < scale; s++) put_span(px, py + cyy * scale + s, line, CELL_W * scale);
    }
}

static void draw_cursor(bool show)
{
    if (!ready) return;
    fill_rect(cx * CELL_W * scale, (cy * CELL_H + CELL_H - 1) * scale, CELL_W * scale, scale, show ? fg : bg);
}

static void scroll(void)
{
    uint64_t line_bytes = (uint64_t)CELL_H * scale * pitch;
    uint64_t text_bytes = line_bytes * rows;
    uint8_t *src = back ? back : fb;
    memmove(src, src + line_bytes, text_bytes - line_bytes);
    if (back) {
        /* Clear the last row in the shadow, then publish the whole text area once. */
        uint32_t c = to_pixel(bg);
        for (uint64_t y = 0; y < (uint64_t)CELL_H * scale; y++) {
            uint32_t *p = (uint32_t *)(back + text_bytes - line_bytes + y * pitch);
            for (uint32_t x = 0; x < cols * CELL_W * scale; x++) p[x] = c;
        }
        memcpy(fb, back, text_bytes);
    } else {
        fill_rect(0, (rows - 1) * CELL_H * scale, cols * CELL_W * scale, CELL_H * scale, bg);
    }
}

static void pointer_draw(void)
{
    uint32_t fill = to_pixel(THEME_POINTER_FILL), edge = to_pixel(THEME_POINTER_EDGE);
    for (uint32_t r = 0; r < PTR_H; r++) {
        const char *row = pointer_shape[r];
        for (uint32_t c = 0; row[c]; c++) {
            if (row[c] == ' ') continue;
            uint32_t px = row[c] == 'X' ? edge : fill;
            for (uint32_t sy = 0; sy < scale; sy++) {
                int64_t y = ptr_y + (int64_t)(r * scale + sy);
                if (y < 0 || y >= (int64_t)height) continue;
                uint32_t *line = (uint32_t *)(fb + (uint64_t)y * pitch);
                for (uint32_t sx = 0; sx < scale; sx++) {
                    int64_t x = ptr_x + (int64_t)(c * scale + sx);
                    if (x >= 0 && x < (int64_t)width) line[x] = px;
                }
            }
        }
    }
}

static void pointer_erase(void)
{
    int64_t x0 = MAX(ptr_x, 0), y0 = MAX(ptr_y, 0);
    int64_t x1 = MIN((int64_t)ptr_x + PTR_W * scale, (int64_t)width);
    int64_t y1 = MIN((int64_t)ptr_y + PTR_H * scale, (int64_t)height);
    for (int64_t y = y0; y < y1; y++) {
        uint64_t off = (uint64_t)y * pitch + (uint64_t)x0 * 4;
        memcpy(fb + off, back + off, (uint64_t)(x1 - x0) * 4);
    }
}

/* Text drawing may have painted over (or scrolled away) the pointer. */
static void pointer_refresh(void) { if (ptr_visible) pointer_draw(); }

bool console_pointer_set(int32_t x, int32_t y, bool visible)
{
    if (!back) return false;
    mutex_lock(&lock);
    if (ptr_visible) pointer_erase();
    ptr_x = x; ptr_y = y; ptr_visible = visible;
    if (visible) pointer_draw();
    mutex_unlock(&lock);
    return true;
}

void console_init(const ntos_framebuffer *info, uint64_t hhdm)
{
    font_init();
    fb     = (uint8_t *)(info->base + hhdm);
    width  = info->width;
    height = info->height;
    pitch  = info->pitch;
    format = info->format;
    scale  = width >= 2560 ? 3 : width >= 1024 ? 2 : 1;
    cols   = width / (CELL_W * scale);
    rows   = height / (CELL_H * scale);
    ready  = true;
    console_clear();
}

bool console_enable_backbuffer(void)
{
    if (!ready || back) return back != NULL;
    uint64_t size = (uint64_t)pitch * height;
    uint64_t phys = pmm_alloc_pages(ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE);
    if (!phys) return false;
    back = phys_to_virt(phys);
    memcpy(back, fb, size);
    return true;
}

static void putc_locked(char c);

void console_clear(void)
{
    if (!ready) return;
    mutex_lock(&lock);
    fill_rect(0, 0, width, height, bg);
    cx = cy = 0;
    draw_cursor(cursor_visible);
    pointer_refresh();
    mutex_unlock(&lock);
}

void console_putc(char c)
{
    if (!ready) return;
    mutex_lock(&lock);
    putc_locked(c);
    pointer_refresh();
    mutex_unlock(&lock);
}

static void putc_locked(char c)
{
    draw_cursor(false);
    switch (c) {
    case '\n': cx = 0; cy++; break;
    case '\r': cx = 0; break;
    case '\t': do { draw_cell(cx++, cy, ' '); } while (cx % 4 && cx < cols); break;
    case '\b':
        if (cx > 0) cx--;
        else if (cy > 0) { cy--; cx = cols - 1; }
        draw_cell(cx, cy, ' ');
        break;
    default:
        draw_cell(cx++, cy, c);
        break;
    }
    if (cx >= cols) { cx = 0; cy++; }
    if (cy >= rows) { scroll(); cy = rows - 1; }
    draw_cursor(cursor_visible);
}

void console_write(const char *s, size_t n)
{
    if (!ready) return;
    mutex_lock(&lock);
    while (n--) putc_locked(*s++);
    pointer_refresh();
    mutex_unlock(&lock);
}

void console_set_color(uint32_t f, uint32_t b) { fg = f; bg = b; }
void console_get_color(uint32_t *f, uint32_t *b) { *f = fg; *b = bg; }
void console_set_cursor(bool visible)
{
    mutex_lock(&lock);
    cursor_visible = visible;
    draw_cursor(visible);
    pointer_refresh();
    mutex_unlock(&lock);
}
void console_screen_size(uint32_t *w, uint32_t *h) { *w = width; *h = height; }
uint32_t console_cols(void) { return cols; }
uint32_t console_rows(void) { return rows; }
