// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"

static k_spinlock console_lock = K_SPINLOCK_INIT;
UINT32 *fb_base;
/* Console RAM is authoritative; scrolling never reads framebuffer memory. */
static char *con_cells, *con_shown, *con_history;
#define CON_HISTORY_ROWS 2048
static UINT32 history_head, history_count, history_view;
/* Overlay is restored from RAM; framebuffer reads are avoided. */
static struct {
    INT32 x, y;
    UINT32 width, height, pixels[32 * 32];
} fb_overlay;
static UINT32 con_dirty_first = ~0U, con_dirty_last;
BOOLEAN fb_pat_wc;
static volatile UINT32 console_emergency;
UINT32 fb_width, fb_height, fb_pitch;
static UINT8 red_shift, green_shift, blue_shift;

#define FONT_W 8
#define FONT_H 8
static UINT32 con_cols, con_rows;
static UINT32 cur_x, cur_y;
static UINT32 fg_color = 0x00E0E0E0;
static UINT32 bg_color = 0x00101018;

typedef struct {
    char ch;
    UINT8 rows[8];
} glyph_t;

static CONST glyph_t FONT[] = {
    {'@', {0x3c, 0x42, 0x5e, 0x52, 0x5e, 0x40, 0x3e, 0}},
    {'[', {0x3c, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3c, 0}},
    {']', {0x3c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x3c, 0}},
    {'#', {0x24, 0x24, 0x7e, 0x24, 0x7e, 0x24, 0x24, 0}},
    {'%', {0x62, 0x64, 0x08, 0x10, 0x26, 0x46, 0, 0}},
    {'|', {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'0', {0x00, 0x3C, 0x66, 0x6E, 0x76, 0x66, 0x3C, 0x00}},
    {'1', {0x00, 0x18, 0x38, 0x18, 0x18, 0x18, 0x3C, 0x00}},
    {'2', {0x00, 0x3C, 0x66, 0x0C, 0x18, 0x30, 0x7E, 0x00}},
    {'3', {0x00, 0x3C, 0x66, 0x1C, 0x06, 0x66, 0x3C, 0x00}},
    {'4', {0x00, 0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x00}},
    {'5', {0x00, 0x7E, 0x60, 0x7C, 0x06, 0x66, 0x3C, 0x00}},
    {'6', {0x00, 0x1C, 0x30, 0x7C, 0x66, 0x66, 0x3C, 0x00}},
    {'7', {0x00, 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x00}},
    {'8', {0x00, 0x3C, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00}},
    {'9', {0x00, 0x3C, 0x66, 0x66, 0x3E, 0x0C, 0x38, 0x00}},
    {'A', {0x00, 0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x00}},
    {'B', {0x00, 0x7C, 0x66, 0x66, 0x7C, 0x66, 0x7C, 0x00}},
    {'C', {0x00, 0x3C, 0x66, 0x60, 0x60, 0x66, 0x3C, 0x00}},
    {'D', {0x00, 0x78, 0x6C, 0x66, 0x66, 0x6C, 0x78, 0x00}},
    {'E', {0x00, 0x7E, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00}},
    {'F', {0x00, 0x7E, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00}},
    {'G', {0x00, 0x3C, 0x66, 0x60, 0x6E, 0x66, 0x3C, 0x00}},
    {'H', {0x00, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00}},
    {'I', {0x00, 0x3C, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00}},
    {'J', {0x00, 0x1E, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00}},
    {'K', {0x00, 0x66, 0x6C, 0x78, 0x78, 0x6C, 0x66, 0x00}},
    {'L', {0x00, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00}},
    {'M', {0x00, 0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x00}},
    {'N', {0x00, 0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x00}},
    {'O', {0x00, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}},
    {'P', {0x00, 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x00}},
    {'Q', {0x00, 0x3C, 0x66, 0x66, 0x66, 0x6C, 0x36, 0x00}},
    {'R', {0x00, 0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x00}},
    {'S', {0x00, 0x3C, 0x66, 0x3C, 0x06, 0x66, 0x3C, 0x00}},
    {'T', {0x00, 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}},
    {'U', {0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}},
    {'V', {0x00, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00}},
    {'W', {0x00, 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x63, 0x00}},
    {'X', {0x00, 0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x00}},
    {'Y', {0x00, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00}},
    {'Z', {0x00, 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x7E, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30}},
    {':', {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00}},
    {';', {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x30, 0x00}},
    {'!', {0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00}},
    {'?', {0x00, 0x3C, 0x66, 0x0C, 0x18, 0x00, 0x18, 0x00}},
    {'\'', {0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'"', {0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E}},
    {'(', {0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00}},
    {')', {0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00}},
    {'*', {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}},
    {'+', {0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00}},
    {'=', {0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00}},
    {'/', {0x06, 0x0C, 0x18, 0x18, 0x30, 0x60, 0x00, 0x00}},
    {'\\', {0x60, 0x30, 0x18, 0x18, 0x0C, 0x06, 0x00, 0x00}},
    {'<', {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}},
    {'>', {0x60, 0x30, 0x18, 0x0C, 0x18, 0x30, 0x60, 0x00}},
};
#define FONT_COUNT (sizeof(FONT) / sizeof(FONT[0]))

static CONST UINT8 FALLBACK_GLYPH[8] = {0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x00};

static CONST UINT8 *font_lookup(char c)
{
    UINTN i;
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 32);
    }
    for (i = 0; i < FONT_COUNT; i++) {
        if (FONT[i].ch == c) {
            return FONT[i].rows;
        }
    }
    return FALLBACK_GLYPH;
}

static UINT32 fb_pixel(UINT32 rgb)
{
    return (((rgb >> 16) & 255) << red_shift) | (((rgb >> 8) & 255) << green_shift) |
           ((rgb & 255) << blue_shift);
}
static void fb_fill(UINT32 rgb)
{
    volatile UINT32 *dst = fb_base;
    UINTN count = (UINTN)fb_pitch * fb_height;
    UINT32 pixel = fb_pixel(rgb);
    /* Write-only full-width stores; avoid uncached framebuffer RMW. */
    arch_fill32(dst, pixel, count);
    arch_write_fence();
}
static void fb_draw_glyph(char c, UINT32 x, UINT32 y)
{
    const UINT8 *rows = font_lookup(c);
    UINT32 fg = fb_pixel(fg_color), bg = fb_pixel(bg_color);
    for (UINT32 row = 0; row < FONT_H; ++row) {
        volatile UINT32 *dst = fb_base + (UINTN)(y * FONT_H + row) * fb_pitch + x * FONT_W;
        UINT8 bits = rows[row];
        for (UINT32 col = 0; col < FONT_W; ++col)
            dst[col] = (bits & (0x80 >> col)) ? fg : bg;
    }
}
static void console_dirty(UINT32 first, UINT32 last)
{
    if (first < con_dirty_first)
        con_dirty_first = first;
    if (last > con_dirty_last)
        con_dirty_last = last;
}
static void draw_glyph(char c, UINT32 x, UINT32 y)
{
    if (x >= con_cols || y >= con_rows)
        return;
    if (!con_cells) {
        fb_draw_glyph(c, x, y);
        return;
    }
    con_cells[(UINTN)y * con_cols + x] = c;
    if (y + history_view < con_rows)
        console_dirty(y + history_view, y + history_view);
}
static char console_visible_cell(UINT32 x, UINT32 y)
{
    UINT32 row = history_count + y - history_view;
    if (row < history_count)
        return con_history[(UINTN)((history_head + row) % CON_HISTORY_ROWS) * con_cols + x];
    return con_cells[(UINTN)(row - history_count) * con_cols + x];
}
static void overlay_restore(void)
{
    if (!con_shown)
        return;
    UINT32 fg = fb_pixel(fg_color), bg = fb_pixel(bg_color);
    for (UINT32 row = 0; row < fb_overlay.height; ++row) {
        UINT32 y = (UINT32)fb_overlay.y + row;
        if (y >= fb_height)
            break;
        for (UINT32 col = 0; col < fb_overlay.width; ++col) {
            UINT32 x = (UINT32)fb_overlay.x + col;
            if (x >= fb_width)
                break;
            UINT32 pixel = bg;
            if (x / FONT_W < con_cols && y / FONT_H < con_rows) {
                char c = con_shown[(UINTN)(y / FONT_H) * con_cols + x / FONT_W];
                const UINT8 *glyph = font_lookup(c);
                if (glyph[y % FONT_H] & (0x80 >> (x % FONT_W)))
                    pixel = fg;
            }
            ((volatile UINT32 *)fb_base)[(UINTN)y * fb_pitch + x] = pixel;
        }
    }
}
static void overlay_draw(void)
{
    for (UINT32 row = 0; row < fb_overlay.height; ++row) {
        UINT32 y = (UINT32)fb_overlay.y + row;
        if (y >= fb_height)
            break;
        for (UINT32 col = 0; col < fb_overlay.width; ++col) {
            UINT32 x = (UINT32)fb_overlay.x + col;
            if (x >= fb_width)
                break;
            UINT32 pixel = fb_overlay.pixels[row * fb_overlay.width + col];
            if (pixel >> 24)
                ((volatile UINT32 *)fb_base)[(UINTN)y * fb_pitch + x] = fb_pixel(pixel);
        }
    }
}
static void console_present(void)
{
    overlay_restore();
    if (con_cells && con_dirty_first != ~0U) {
        for (UINT32 y = con_dirty_first; y <= con_dirty_last; ++y)
            for (UINT32 x = 0; x < con_cols; ++x) {
                UINTN i = (UINTN)y * con_cols + x;
                char c = console_visible_cell(x, y);
                if (c != con_shown[i]) {
                    fb_draw_glyph(c, x, y);
                    con_shown[i] = c;
                }
            }
        con_dirty_first = ~0U;
        con_dirty_last = 0;
    }
    overlay_draw();
    arch_write_fence();
}
static void fb_scroll_one_line(void)
{
    if (!con_cells) {
        /* Early-boot/OOM fallback clears instead of reading device memory. */
        fb_fill(bg_color);
        cur_y = 1;
        return;
    }
    if (con_history) {
        UINT32 row = (history_head + history_count) % CON_HISTORY_ROWS;
        mem_copy(con_history + (UINTN)row * con_cols, con_cells, con_cols);
        if (history_count == CON_HISTORY_ROWS)
            history_head = (history_head + 1) % CON_HISTORY_ROWS;
        else
            ++history_count;
        if (history_view && history_view < history_count)
            ++history_view;
    }
    UINTN moved = (UINTN)con_cols * (con_rows - 1);
    memmove(con_cells, con_cells + con_cols, moved);
    memset(con_cells + moved, ' ', con_cols);
    console_dirty(0, con_rows - 1);
}
/* Console rendering keeps IRQs enabled but pins the current task while locked.
 * Fault output bypasses the normal lock. */
static BOOLEAN console_enter(void)
{
    if (__atomic_load_n(&console_emergency, __ATOMIC_RELAXED))
        return FALSE;
    k_preempt_disable();
    while (__atomic_exchange_n(&console_lock.value, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&console_lock.value, __ATOMIC_RELAXED))
            arch_pause();
    return TRUE;
}
static void console_leave(BOOLEAN locked)
{
    console_present();
    if (locked) {
        __atomic_store_n(&console_lock.value, 0, __ATOMIC_RELEASE);
        k_preempt_enable();
    }
}
void console_panic_mode(void) { __atomic_store_n(&console_emergency, 1, __ATOMIC_RELAXED); }
void console_buffer_init(void)
{
    UINT64 cells = (UINT64)con_cols * con_rows;
    if (!cells || cells > 16 * 1024 * 1024)
        return;
    UINT64 memory = k_pmm_alloc_pages((UINT32)((cells * 2 + 4095) / 4096), 0);
    if (!memory)
        return;
    con_cells = (char *)(UINTN)memory;
    con_shown = con_cells + cells;
    memset(con_cells, ' ', cells * 2);
    UINT64 history_bytes = (UINT64)con_cols * CON_HISTORY_ROWS;
    con_history = (void *)(UINTN)k_pmm_alloc_pages((UINT32)((history_bytes + 4095) / 4096), 0);
    fb_fill(bg_color);
    cur_x = cur_y = 0;
}
BOOLEAN kernel_console_buffered(void) { return con_cells != NULL; }
BOOLEAN kernel_fb_pat_wc(void) { return fb_pat_wc; }

void kernel_fb_init(UINT32 *fb_base_in, UINT32 width, UINT32 height, UINT32 pitch, UINT8 rshift,
                    UINT8 gshift, UINT8 bshift)
{
    fb_base = fb_base_in;
    fb_width = width;
    fb_height = height;
    fb_pitch = pitch;
    red_shift = rshift;
    green_shift = gshift;
    blue_shift = bshift;

    con_cols = fb_width / FONT_W;
    con_rows = fb_height / FONT_H;
    cur_x = 0;
    cur_y = 0;
}

UINT32 kernel_fb_width(void) { return fb_width; }
UINT32 kernel_fb_height(void) { return fb_height; }
UINT32 kernel_con_cols(void) { return con_cols; }
UINT32 kernel_con_rows(void) { return con_rows; }

void kernel_console_clear(void)
{
    BOOLEAN locked = console_enter();
    history_head = history_count = history_view = 0;
    if (fb_base)
        fb_fill(bg_color);
    if (con_cells)
        memset(con_cells, ' ', (UINTN)con_cols * con_rows * 2);
    con_dirty_first = ~0U;
    con_dirty_last = 0;
    cur_x = cur_y = 0;
    console_leave(locked);
}

UINT32 kernel_console_scroll(INT32 rows)
{
    BOOLEAN locked = console_enter();
    INT64 next = (INT64)history_view + rows;
    UINT32 view = next < 0 ? 0 : next > history_count ? history_count : (UINT32)next;
    if (view != history_view) {
        history_view = view;
        console_dirty(0, con_rows - 1);
    }
    console_leave(locked);
    return view;
}
int kernel_overlay_set(INT32 x, INT32 y, UINT32 width, UINT32 height, const UINT32 *pixels)
{
    if (width > 32 || height > 32 || (width && height && !pixels) || !con_shown)
        return K_EINVAL;
    BOOLEAN locked = console_enter();
    overlay_restore();
    fb_overlay.width = width;
    fb_overlay.height = height;
    fb_overlay.x = x < 0 ? 0 : x >= (INT32)fb_width ? (INT32)fb_width - 1 : x;
    fb_overlay.y = y < 0 ? 0 : y >= (INT32)fb_height ? (INT32)fb_height - 1 : y;
    if (width && height)
        mem_copy(fb_overlay.pixels, pixels, width * height * 4);
    console_leave(locked);
    return 0;
}
void kernel_overlay_move(INT32 x, INT32 y)
{
    BOOLEAN locked = console_enter();
    overlay_restore();
    fb_overlay.x = x < 0 ? 0 : x >= (INT32)fb_width ? (INT32)fb_width - 1 : x;
    fb_overlay.y = y < 0 ? 0 : y >= (INT32)fb_height ? (INT32)fb_height - 1 : y;
    console_leave(locked);
}

static void con_putc_raw(char c)
{
    if (!fb_base || !con_cols || !con_rows)
        return;
    if (c == '\n') {
        cur_x = 0;
        cur_y++;
    } else if (c == '\r') {
        cur_x = 0;
    } else if (c == '\b') {
        if (cur_x > 0) {
            cur_x--;
            draw_glyph(' ', cur_x, cur_y);
        }
    } else {
        draw_glyph(c, cur_x, cur_y);
        cur_x++;
        if (cur_x >= con_cols) {
            cur_x = 0;
            cur_y++;
        }
    }

    while (cur_y >= con_rows) {
        fb_scroll_one_line();
        cur_y--;
    }
}

void con_putc(char c)
{
    BOOLEAN locked = console_enter();
    con_putc_raw(c);
    console_leave(locked);
}
void con_print(CONST char *s)
{
    if (!s)
        return;
    BOOLEAN locked = console_enter();
    while (*s)
        con_putc_raw(*s++);
    console_leave(locked);
}
void con_print_hex(UINT64 v)
{
    static const char hex[] = "0123456789abcdef";
    char b[19];
    b[0] = '0';
    b[1] = 'x';
    for (int i = 0; i < 16; ++i)
        b[2 + i] = hex[(v >> (60 - 4 * i)) & 15];
    b[18] = 0;
    con_print(b);
}

void con_print_uint(UINT64 v)
{
    char buf[24];
    int i = 0, a, b;

    if (v == 0) {
        con_putc('0');
        return;
    }

    while (v > 0 && i < 23) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    buf[i] = 0;

    for (a = 0, b = i - 1; a < b; a++, b--) {
        char tmp = buf[a];
        buf[a] = buf[b];
        buf[b] = tmp;
    }

    con_print(buf);
}

void con_print_2digit(UINT16 v)
{
    char buf[3];
    buf[0] = (char)('0' + ((v / 10) % 10));
    buf[1] = (char)('0' + (v % 10));
    buf[2] = 0;
    con_print(buf);
}

void con_print_int(INT32 v)
{
    if (v < 0) {
        con_putc('-');
        con_print_uint((UINT64)(-(INT64)v));
    } else {
        con_print_uint((UINT64)v);
    }
}
