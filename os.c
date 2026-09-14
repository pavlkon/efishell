// SPDX-License-Identifier: GPL-2.0-only
#include "kernel.h"

#define CMD_BUF_SIZE 256
static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_RUNTIME_SERVICES *RT;
static UINT64 snap_total_mib, snap_free_mib, snap_entries;
static UINT8 efi_map_buffer[256 * 1024] __attribute__((aligned(16)));
static char cwd[K_PATH_MAX] = "/";
static UINT32 passes, failures;
static k_process *launched[K_MAX_PROCESSES];
static UINT32 ram_test_disk = 0xffffffff;

static UINTN strlen_(const char *s)
{
    UINTN n = 0;
    while (s[n])
        ++n;
    return n;
}
static BOOLEAN streq_(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}
static BOOLEAN starts_with_(const char *s, const char *p)
{
    while (*p) {
        if (*s++ != *p++)
            return FALSE;
    }
    return TRUE;
}
static void copy_(void *dst, const void *src, UINTN n)
{
    UINT8 *d = dst;
    const UINT8 *s = src;
    while (n--)
        *d++ = *s++;
}
static void zero_(void *dst, UINTN n)
{
    UINT8 *d = dst;
    while (n--)
        *d++ = 0;
}
static BOOLEAN equal_(const void *a, const void *b, UINTN n)
{
    const UINT8 *x = a, *y = b;
    while (n--)
        if (*x++ != *y++)
            return FALSE;
    return TRUE;
}
static void textcopy_(char *d, const char *s, UINTN cap)
{
    UINTN i = 0;
    while (i + 1 < cap && s[i]) {
        d[i] = s[i];
        ++i;
    }
    d[i] = 0;
}
static void put32_(void *p, UINT32 v)
{
    UINT8 *b = p;
    for (int i = 0; i < 4; ++i)
        b[i] = (UINT8)(v >> (8 * i));
}
static void put64_(void *p, UINT64 v)
{
    UINT8 *b = p;
    for (int i = 0; i < 8; ++i)
        b[i] = (UINT8)(v >> (8 * i));
}
static void report(const char *name, BOOLEAN ok)
{
    con_print(ok ? "PASS " : "FAIL ");
    con_print(name);
    con_print("\n");
    if (ok)
        ++passes;
    else
        ++failures;
}
static void error_(const char *op, int e)
{
    con_print(op);
    con_print(": ");
    con_print(k_strerror(e));
    con_print("\n");
}
static BOOLEAN number_(const char **p, UINT64 *out)
{
    while (**p == ' ')
        ++*p;
    UINT32 radix = 10;
    if ((*p)[0] == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
        radix = 16;
        *p += 2;
    }
    UINT64 v = 0;
    UINT32 digits = 0;
    while (**p && **p != ' ') {
        char c = *(*p)++;
        UINT32 d = c >= '0' && c <= '9'   ? (UINT32)(c - '0')
                   : c >= 'a' && c <= 'f' ? (UINT32)(c - 'a' + 10)
                   : c >= 'A' && c <= 'F' ? (UINT32)(c - 'A' + 10)
                                          : 99;
        if (d >= radix || v > (~0ULL - d) / radix)
            return FALSE;
        v = v * radix + d;
        ++digits;
    }
    *out = v;
    return digits != 0;
}
static BOOLEAN process_status(k_process *p, k_process_info *info)
{
    UINT32 id = k_process_id(p);
    for (UINT32 i = 0; i < K_MAX_PROCESSES; ++i)
        if (!k_process_get(i, info) && info->id == id)
            return TRUE;
    return FALSE;
}

static const char USB_HID_MAP[128] = {
    0,   0,   0,   0,   'a',  'b', 'c', 'd',  'e', 'f', 'g',  'h', 'i',  'j',  'k',
    'l', 'm', 'n', 'o', 'p',  'q', 'r', 's',  't', 'u', 'v',  'w', 'x',  'y',  'z',
    '1', '2', '3', '4', '5',  '6', '7', '8',  '9', '0', '\n', 0,   '\b', '\t', ' ',
    '-', '=', '[', ']', '\\', 0,   ';', '\'', '`', ',', '.',  '/'};

static const char USB_HID_MAP_SHIFT[128] = {
    0,   0,   0,   0,   'A', 'B', 'C', 'D', 'E', 'F', 'G',  'H', 'I',  'J',  'K',
    'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',  'W', 'X',  'Y',  'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '\n', 0,   '\b', '\t', ' ',
    '_', '+', '{', '}', '|', 0,   ':', '"', '~', '<', '>',  '?'};

static BOOLEAN ps2_extended = FALSE;
static BOOLEAN ps2_shift = FALSE;

static CONST char SC_UNSHIFTED[] = {0,   0,   '1', '2', '3',  '4', '5', '6',  '7', '8', '9', '0',
                                    '-', '=', 0,   0,   'q',  'w', 'e', 'r',  't', 'y', 'u', 'i',
                                    'o', 'p', '[', ']', 0,    0,   'a', 's',  'd', 'f', 'g', 'h',
                                    'j', 'k', 'l', ';', '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',
                                    'b', 'n', 'm', ',', '.',  '/', 0,   '*',  0,   ' '};

static CONST char SC_SHIFTED[] = {0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
                                  '_', '+', 0,   0,   'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
                                  'O', 'P', '{', '}', 0,   0,   'A', 'S', 'D', 'F', 'G', 'H',
                                  'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
                                  'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' '};
#define SC_TABLE_SIZE (sizeof(SC_UNSHIFTED) / sizeof(SC_UNSHIFTED[0]))

static char process_ps2_scancode(UINT8 code)
{
    if (code == 1)
        return 27;
    if (code == 0xE0) {
        ps2_extended = TRUE;
        return 0;
    }

    if (ps2_extended) {
        ps2_extended = FALSE;
        return 0;
    }

    if (code & 0x80) {
        UINT8 make = code & 0x7F;
        if (make == 0x2A || make == 0x36) {
            ps2_shift = FALSE;
        }
        return 0;
    }

    if (code == 0x2A || code == 0x36) {
        ps2_shift = TRUE;
        return 0;
    }

    if (code == 0x1C)
        return '\n';
    if (code == 0x0E)
        return '\b';

    if (code < SC_TABLE_SIZE) {
        return ps2_shift ? SC_SHIFTED[code] : SC_UNSHIFTED[code];
    }

    return 0;
}

/* Input decoding stays in os.c; the kernel exposes raw reports only. */
#define OS_HID_FIELDS 160
#define OS_MOUSE_EVENTS 128
enum { HID_NONE, HID_KEY, HID_KEYS, HID_BUTTON, HID_X, HID_Y, HID_WHEEL, HID_PAN };
typedef struct {
    UINT32 offset, width, count, usage, usage_end;
    INT64 minimum, maximum;
    UINT8 report, kind;
    BOOLEAN relative;
} os_hid_field;
typedef struct {
    os_hid_field fields[OS_HID_FIELDS];
    UINT32 field_count, report_bits[256];
    BOOLEAN ids, mouse, keyboard;
} os_hid_layout;
typedef struct {
    k_input_device transport;
    os_hid_layout layout;
    UINT8 keys[32];
    UINT32 buttons, lost;
    BOOLEAN active, boot_keys, boot_pointer;
} os_input_device;
typedef struct {
    UINT32 source, buttons, changed, flags;
    INT32 x, y, dx, dy, wheel_x, wheel_y;
    UINT64 tick;
} os_mouse_event;
typedef struct {
    INT32 x, y;
    UINT32 buttons;
    INT64 wheel_x, wheel_y;
    UINT64 reports, dropped;
} os_mouse_state;
static os_input_device os_devices[K_INPUT_MAX_DEVICES];
static k_spinlock os_input_lock = K_SPINLOCK_INIT;
static os_mouse_state mouse;
static os_mouse_event mouse_events[OS_MOUSE_EVENTS];
static UINT32 mouse_head, mouse_count, os_input_task;
static BOOLEAN mouse_draw_dirty;
static INT32 mouse_scroll_pending;
static UINT8 mouse_ps2_id, mouse_ps2_bytes[4], mouse_ps2_count;
static BOOLEAN mouse_ps2_active;
static UINT64 mouse_ps2_tick;
static char os_keys[256];
static UINT32 os_key_head, os_key_count;
static BOOLEAN os_caps;
static UINT32 repeat_device, repeat_usage;
static UINT64 repeat_at;
static void os_key_put(char c)
{
    if (!c)
        return;
    UINT64 f = k_spin_lock(&os_input_lock);
    if (os_key_count < sizeof(os_keys)) {
        os_keys[(os_key_head + os_key_count) % sizeof(os_keys)] = c;
        ++os_key_count;
    }
    k_spin_unlock(&os_input_lock, f);
}
static int os_pollchar(void)
{
    UINT64 f = k_spin_lock(&os_input_lock);
    int c = -1;
    if (os_key_count) {
        c = (UINT8)os_keys[os_key_head];
        os_key_head = (os_key_head + 1) % sizeof(os_keys);
        --os_key_count;
    }
    k_spin_unlock(&os_input_lock, f);
    return c;
}
static char os_getchar(void)
{
    for (;;) {
        int c = os_pollchar();
        if (c >= 0) {
            kernel_console_scroll(-2147483647);
            return (char)c;
        }
        k_sleep(1);
    }
}
static INT64 hid_signed(UINT32 value, UINT32 bits)
{
    if (!bits)
        return 0;
    UINT64 mask = 1ULL << (bits - 1);
    return (value & mask) ? (INT64)value - (INT64)(1ULL << bits) : value;
}
static UINT32 hid_usage(UINT32 value, UINT32 bytes, UINT32 page)
{
    return bytes == 4 ? value : ((page & 0xffff) << 16) | (value & 0xffff);
}
static int os_hid_parse(const UINT8 *bytes, UINT32 size, os_hid_layout *out)
{
    typedef struct {
        UINT32 page, width, count, id;
        INT64 minimum, maximum;
    } globals;
    globals g = {0}, saved[8];
    UINT32 globals_depth = 0, collection_depth = 0, usages[128], usage_count = 0;
    UINT32 usage_min = 0, usage_max = 0;
    BOOLEAN have_min = FALSE, have_max = FALSE;
    UINT8 collection[16], app = 0;
    zero_(out, sizeof(*out));
    for (UINT32 pos = 0; pos < size;) {
        UINT8 prefix = bytes[pos++];
        if (prefix == 0xfe) { /* HID long item has no standard input semantics. */
            if (size - pos < 2 || bytes[pos] > size - pos - 2)
                return K_EINVAL;
            pos += 2 + bytes[pos];
            continue;
        }
        UINT32 n = prefix & 3, type = (prefix >> 2) & 3, tag = prefix >> 4, value = 0;
        if (n == 3)
            n = 4;
        if (n > size - pos)
            return K_EINVAL;
        for (UINT32 i = 0; i < n; ++i)
            value |= (UINT32)bytes[pos++] << (i * 8);
        if (type == 1) {
            switch (tag) {
            case 0:
                g.page = value;
                break;
            case 1:
                g.minimum = hid_signed(value, n * 8);
                break;
            case 2:
                g.maximum = g.minimum < 0 ? hid_signed(value, n * 8) : (INT64)value;
                break;
            case 7:
                g.width = value;
                break;
            case 8:
                if (!value || value > 255)
                    return K_EINVAL;
                g.id = value;
                out->ids = TRUE;
                break;
            case 9:
                g.count = value;
                break;
            case 10:
                if (globals_depth == 8)
                    return K_E2BIG;
                saved[globals_depth++] = g;
                break;
            case 11:
                if (!globals_depth)
                    return K_EINVAL;
                g = saved[--globals_depth];
                break;
            default:
                break;
            }
        } else if (type == 2) {
            if (tag == 0) {
                if (usage_count == 128)
                    return K_E2BIG;
                usages[usage_count++] = hid_usage(value, n, g.page);
            } else if (tag == 1) {
                usage_min = hid_usage(value, n, g.page);
                have_min = TRUE;
            } else if (tag == 2) {
                usage_max = hid_usage(value, n, g.page);
                have_max = TRUE;
            } else if (tag == 10)
                return K_ENOTSUP; /* ambiguous delimiter alternatives */
        } else if (type == 0) {
            if (tag == 10) {
                if (collection_depth == 16)
                    return K_E2BIG;
                collection[collection_depth++] = app;
                UINT32 usage = usage_count ? usages[0] : have_min ? usage_min : 0;
                if (value == 1)
                    app = usage == 0x10002 ? 1 : usage == 0x10006 ? 2 : 0;
            } else if (tag == 12) {
                if (!collection_depth)
                    return K_EINVAL;
                app = collection[--collection_depth];
            } else if (tag == 8) { /* Output and Feature items have separate bit offsets. */
                UINT64 bits = (UINT64)g.width * g.count;
                UINT32 offset = out->report_bits[g.id];
                if (bits > K_INPUT_REPORT_MAX * 8 || offset > K_INPUT_REPORT_MAX * 8 - bits)
                    return K_E2BIG;
                out->report_bits[g.id] += (UINT32)bits;
                if (have_min != have_max ||
                    (have_min && (usage_max < usage_min || (usage_max >> 16) != (usage_min >> 16))))
                    return K_EINVAL;
                if (!(value & 1) && app) {
                    if (!g.width || g.width > 32 || !g.count || g.count > 1024)
                        return K_ENOTSUP;
                    if (g.minimum > g.maximum)
                        return K_EINVAL;
                    for (UINT32 i = 0; i < g.count; ++i) {
                        UINT32 usage =
                            i < usage_count ? usages[i]
                            : have_min ? usage_min +
                                             (i < usage_max - usage_min ? i : usage_max - usage_min)
                            : usage_count ? usages[usage_count - 1]
                                          : 0;
                        UINT32 page = usage >> 16, id = usage & 0xffff, kind = HID_NONE;
                        if (app == 2 && page == 7)
                            kind = (value & 2) ? HID_KEY : HID_KEYS;
                        if (app == 1 && (value & 2)) {
                            if (page == 9 && id >= 1 && id <= 32)
                                kind = HID_BUTTON;
                            if (page == 1)
                                kind = id == 0x30   ? HID_X
                                       : id == 0x31 ? HID_Y
                                       : id == 0x38 ? HID_WHEEL
                                                    : kind;
                            if (page == 12 && id == 0x238)
                                kind = HID_PAN;
                        }
                        if (kind) {
                            if (out->field_count == OS_HID_FIELDS)
                                return K_E2BIG;
                            os_hid_field *f = &out->fields[out->field_count++];
                            *f = (os_hid_field){offset + i * g.width,
                                                g.width,
                                                kind == HID_KEYS ? g.count : 1,
                                                id,
                                                have_max ? usage_max & 0xffff : id,
                                                g.minimum,
                                                g.maximum,
                                                (UINT8)g.id,
                                                (UINT8)kind,
                                                (value & 4) != 0};
                            if (kind == HID_KEYS) {
                                if (!have_min || usage_count)
                                    return K_ENOTSUP;
                                f->usage = usage_min & 0xffff;
                            }
                            if (kind == HID_KEY || kind == HID_KEYS)
                                out->keyboard = TRUE;
                            else
                                out->mouse = TRUE;
                            if (kind == HID_KEYS)
                                break;
                        }
                    }
                }
            }
            usage_count = 0;
            have_min = have_max = FALSE;
        }
    }
    if (globals_depth || collection_depth || (out->ids && out->report_bits[0]))
        return K_EINVAL;
    return out->field_count ? 0 : K_ENOTSUP;
}
static BOOLEAN hid_extract(const UINT8 *p, UINT32 n, UINT32 bit, UINT32 width, UINT32 *value)
{
    if (!width || width > 32 || bit > n * 8 || width > n * 8 - bit)
        return FALSE;
    UINT32 result = 0;
    for (UINT32 i = 0; i < width; ++i)
        result |= (UINT32)((p[(bit + i) / 8] >> ((bit + i) % 8)) & 1) << i;
    *value = result;
    return TRUE;
}
static BOOLEAN os_key_is_down(const UINT8 keys[32], UINT32 usage)
{
    return usage < 256 && (keys[usage / 8] & (1U << (usage % 8)));
}
static void os_key_bit(UINT8 keys[32], UINT32 usage, BOOLEAN down)
{
    if (usage >= 256)
        return;
    if (down)
        keys[usage / 8] |= (UINT8)(1U << (usage % 8));
    else
        keys[usage / 8] &= (UINT8) ~(1U << (usage % 8));
}

static char os_usb_character(UINT32 usage)
{
    BOOLEAN shift = FALSE;
    for (UINT32 i = 0; i < K_INPUT_MAX_DEVICES; ++i)
        shift |=
            os_key_is_down(os_devices[i].keys, 0xe1) || os_key_is_down(os_devices[i].keys, 0xe5);
    if (usage == 41)
        return 27;
    if (usage >= sizeof(USB_HID_MAP))
        return 0;
    if (usage >= 4 && usage <= 29)
        shift ^= os_caps;
    return shift ? USB_HID_MAP_SHIFT[usage] : USB_HID_MAP[usage];
}
static void os_keyboard_apply(os_input_device *d, const UINT8 keys[32])
{
    UINT8 old[32];
    copy_(old, d->keys, 32);
    copy_(d->keys, keys, 32);
    for (UINT32 usage = 0; usage < 256; ++usage) {
        BOOLEAN down = os_key_is_down(keys, usage), before = os_key_is_down(old, usage);
        if (!down && repeat_device == d->transport.id && repeat_usage == usage)
            repeat_device = 0;
        if (!down || before)
            continue;
        if (usage == 0x39) {
            os_caps = !os_caps;
            continue;
        }
        char c = os_usb_character(usage);
        os_key_put(c);
        if (c && c != 27) {
            repeat_device = d->transport.id;
            repeat_usage = usage;
            repeat_at = k_uptime_ms() + 500;
        }
    }
}
static INT32 os_clamp64(INT64 v, INT32 lo, INT32 hi)
{
    return v < lo ? lo : v > hi ? hi : (INT32)v;
}
static void os_mouse_emit(UINT32 source, INT64 dx, INT64 dy, INT64 wx, INT64 wy, BOOLEAN absolute_x,
                          BOOLEAN absolute_y, UINT32 flags)
{
    UINT64 irq = k_spin_lock(&os_input_lock);
    INT32 old_x = mouse.x, old_y = mouse.y;
    UINT32 previous = mouse.buttons, buttons = 0;
    for (UINT32 i = 0; i < K_INPUT_MAX_DEVICES; ++i)
        if (os_devices[i].active)
            buttons |= os_devices[i].buttons;
    mouse.x = os_clamp64(absolute_x ? dx : (INT64)mouse.x + dx, 0, (INT32)kernel_fb_width() - 1);
    mouse.y = os_clamp64(absolute_y ? dy : (INT64)mouse.y + dy, 0, (INT32)kernel_fb_height() - 1);
    mouse.buttons = buttons;
    wx = os_clamp64(wx, -2147483647, 2147483647);
    wy = os_clamp64(wy, -2147483647, 2147483647);
    mouse.wheel_x += wx;
    mouse.wheel_y += wy;
    ++mouse.reports;
    os_mouse_event event = {source,    buttons,   previous ^ buttons, flags,
                            mouse.x,   mouse.y,   mouse.x - old_x,    mouse.y - old_y,
                            (INT32)wx, (INT32)wy, k_ticks()};
    if (event.dx || event.dy || wx || wy || event.changed || flags) {
        if (mouse_count == OS_MOUSE_EVENTS) {
            mouse_head = (mouse_head + 1) % OS_MOUSE_EVENTS;
            --mouse_count;
            ++mouse.dropped;
        }
        mouse_events[(mouse_head + mouse_count) % OS_MOUSE_EVENTS] = event;
        ++mouse_count;
        mouse_draw_dirty = TRUE;
        mouse_scroll_pending = os_clamp64((INT64)mouse_scroll_pending + wy * 3, -100000, 100000);
    }
    k_spin_unlock(&os_input_lock, irq);
}
static BOOLEAN os_mouse_read(os_mouse_event *out)
{
    UINT64 f = k_spin_lock(&os_input_lock);
    BOOLEAN ready = mouse_count != 0;
    if (ready) {
        *out = mouse_events[mouse_head];
        mouse_head = (mouse_head + 1) % OS_MOUSE_EVENTS;
        --mouse_count;
    }
    k_spin_unlock(&os_input_lock, f);
    return ready;
}
static os_mouse_state os_mouse_snapshot(void)
{
    UINT64 f = k_spin_lock(&os_input_lock);
    os_mouse_state out = mouse;
    k_spin_unlock(&os_input_lock, f);
    return out;
}
static void os_hid_report(os_input_device *d, const UINT8 *bytes, UINT32 size)
{
    if (d->boot_keys) {
        if (size < 8)
            return;
        UINT8 keys[32] = {0};
        for (UINT32 i = 0; i < 8; ++i)
            os_key_bit(keys, 0xe0 + i, (bytes[0] & (1U << i)) != 0);
        for (UINT32 i = 2; i < 8; ++i) {
            if (bytes[i] && bytes[i] <= 3)
                return; /* rollover/error report is not a set of key releases */
            if (bytes[i])
                os_key_bit(keys, bytes[i], TRUE);
        }
        os_keyboard_apply(d, keys);
        return;
    }
    if (d->boot_pointer) {
        if (size < 3)
            return;
        d->buttons = bytes[0] & 7;
        os_mouse_emit(d->transport.id, (INT8)bytes[1], (INT8)bytes[2], 0, 0, FALSE, FALSE, 0);
        return; /* USB boot protocol defines no wheel; never guess extra bytes */
    }
    os_hid_layout *layout = &d->layout;
    UINT32 id = 0;
    if (layout->ids) {
        if (!size)
            return;
        id = *bytes++;
        --size;
    }
    if (!layout->report_bits[id] || layout->report_bits[id] > size * 8)
        return;
    UINT8 keys[32];
    copy_(keys, d->keys, 32);
    UINT32 buttons = d->buttons;
    INT64 x = 0, y = 0, wx = 0, wy = 0;
    BOOLEAN pointer = FALSE, keyboard = FALSE, ax = FALSE, ay = FALSE;
    for (UINT32 n = 0; n < layout->field_count; ++n) {
        const os_hid_field *f = &layout->fields[n];
        if (f->report != id)
            continue;
        UINT32 raw;
        if (!hid_extract(bytes, size, f->offset, f->width, &raw))
            return;
        INT64 value = f->minimum < 0 ? hid_signed(raw, f->width) : (INT64)raw;
        if (f->kind == HID_KEYS) {
            for (UINT32 usage = f->usage; usage <= f->usage_end && usage < 256; ++usage)
                os_key_bit(keys, usage, FALSE);
            for (UINT32 i = 0; i < f->count; ++i) {
                if (!hid_extract(bytes, size, f->offset + i * f->width, f->width, &raw))
                    return;
                INT64 v = f->minimum < 0 ? hid_signed(raw, f->width) : (INT64)raw;
                if (v < f->minimum || v > f->maximum)
                    continue;
                INT64 usage = v - f->minimum + f->usage;
                if (usage > 0 && usage <= 3)
                    return;
                if (usage > 0 && usage <= f->usage_end)
                    os_key_bit(keys, (UINT32)usage, TRUE);
            }
            keyboard = TRUE;
        } else if (f->kind == HID_KEY) {
            os_key_bit(keys, f->usage, value != 0);
            keyboard = TRUE;
        } else if (f->kind == HID_BUTTON) {
            if (value)
                buttons |= 1U << (f->usage - 1);
            else
                buttons &= ~(1U << (f->usage - 1));
            pointer = TRUE;
        } else {
            if (value < f->minimum || value > f->maximum)
                continue;
            if ((f->kind == HID_X || f->kind == HID_Y) && !f->relative) {
                INT64 span = f->maximum - f->minimum;
                UINT32 limit = f->kind == HID_X ? kernel_fb_width() : kernel_fb_height();
                if (span <= 0)
                    continue;
                value = (value - f->minimum) * (limit - 1) / span;
            }
            if (f->kind == HID_X) {
                x = value;
                ax = !f->relative;
            }
            if (f->kind == HID_Y) {
                y = value;
                ay = !f->relative;
            }
            if (f->kind == HID_WHEEL)
                wy += value;
            if (f->kind == HID_PAN)
                wx += value;
            pointer = TRUE;
        }
    }
    if (keyboard)
        os_keyboard_apply(d, keys);
    if (pointer) {
        d->buttons = buttons;
        os_mouse_emit(d->transport.id, x, y, wx, wy, ax, ay, 0);
    }
}
static int os_ps2_rate(UINT8 rate)
{
    int e = k_i8042_exchange(1, 0xf3, NULL, 0);
    return e ? e : k_i8042_exchange(1, rate, NULL, 0);
}
static int os_ps2_mouse_init(void)
{
    int e = k_i8042_enable(1);
    if (!e)
        e = k_i8042_exchange(1, 0xf5, NULL, 0);
    if (!e)
        e = k_i8042_exchange(1, 0xf6, NULL, 0);
    if (!e)
        e = os_ps2_rate(200);
    if (!e)
        e = os_ps2_rate(100);
    if (!e)
        e = os_ps2_rate(80);
    if (!e)
        e = k_i8042_exchange(1, 0xf2, &mouse_ps2_id, 1);
    if (e)
        return e;
    if (mouse_ps2_id == 3) {
        if (!e)
            e = os_ps2_rate(200);
        if (!e)
            e = os_ps2_rate(200);
        if (!e)
            e = os_ps2_rate(80);
        if (!e)
            e = k_i8042_exchange(1, 0xf2, &mouse_ps2_id, 1);
    }
    if (!e && mouse_ps2_id != 0 && mouse_ps2_id != 3 && mouse_ps2_id != 4)
        e = K_ENOTSUP;
    if (!e)
        e = os_ps2_rate(100);
    if (!e)
        e = k_i8042_exchange(1, 0xf4, NULL, 0);
    return e;
}
static BOOLEAN os_ps2_decode(const UINT8 p[4], UINT8 id, INT32 *dx, INT32 *dy, INT32 *wheel,
                             UINT32 *buttons)
{
    if (!(p[0] & 8) || (id != 0 && id != 3 && id != 4))
        return FALSE;
    *dx = p[1] - ((p[0] & 0x10) ? 256 : 0);
    *dy = -(p[2] - ((p[0] & 0x20) ? 256 : 0));
    *wheel = id == 3 ? -(INT32)(INT8)p[3] : id == 4 ? -(INT32)hid_signed(p[3] & 15, 4) : 0;
    *buttons = (p[0] & 7) | (id == 4 ? (p[3] & 0x30) >> 1 : 0);
    if (p[0] & 0xc0)
        *dx = *dy = 0;
    return TRUE;
}
static void os_ps2_mouse_byte(UINT8 byte, UINT64 tick)
{
    if (mouse_ps2_count && tick - mouse_ps2_tick > k_tick_hz() / 10)
        mouse_ps2_count = 0;
    mouse_ps2_tick = tick;
    if (!mouse_ps2_count && !(byte & 8))
        return;
    mouse_ps2_bytes[mouse_ps2_count++] = byte;
    if (mouse_ps2_count < (mouse_ps2_id ? 4 : 3))
        return;
    mouse_ps2_count = 0;
    INT32 dx, dy, wheel;
    UINT32 buttons;
    if (!os_ps2_decode(mouse_ps2_bytes, mouse_ps2_id, &dx, &dy, &wheel, &buttons))
        return;
    os_devices[1].buttons = buttons;
    os_mouse_emit(2, dx, dy, 0, wheel, FALSE, FALSE, 0);
}
static void os_input_reset(os_input_device *d, UINT32 flags)
{
    UINT8 none[32] = {0};
    os_keyboard_apply(d, none);
    d->buttons = 0;
    if (d->transport.id == 2)
        mouse_ps2_count = 0;
    if (d->transport.id == 1) {
        ps2_extended = FALSE;
        ps2_shift = FALSE;
    }
    if (flags & K_INPUT_OFFLINE)
        d->active = FALSE;
    if (d->layout.mouse || d->boot_pointer || d->transport.id == 2)
        os_mouse_emit(d->transport.id, 0, 0, 0, 0, FALSE, FALSE, flags);
}
static void os_input_report(const k_input_report *r)
{
    if (!r->device || r->device > K_INPUT_MAX_DEVICES)
        return;
    os_input_device *d = &os_devices[r->device - 1];
    if (!d->active)
        return;
    if (r->lost != d->lost || r->flags) {
        d->lost = r->lost;
        os_input_reset(d, r->flags | K_INPUT_ERROR);
        if (r->flags || !r->size)
            return;
    }
    if (d->transport.transport == K_INPUT_USB)
        os_hid_report(d, r->bytes, r->size);
    else if (r->device == 1)
        for (UINT32 i = 0; i < r->size; ++i)
            os_key_put(process_ps2_scancode(r->bytes[i]));
    else if (r->device == 2 && mouse_ps2_active)
        for (UINT32 i = 0; i < r->size; ++i)
            os_ps2_mouse_byte(r->bytes[i], r->tick);
}
static const char *const arrow_mask[20] = {
    "#               ", "##              ", "#.#             ", "#..#            ",
    "#...#           ", "#....#          ", "#.....#         ", "#......#        ",
    "#.......#       ", "#........#      ", "#.........#     ", "#..........#    ",
    "#......#####    ", "#...#..#        ", "#..# #..#       ", "#.#  #..#       ",
    "##    #..#      ", "#     #..#      ", "       ##       ", "                "};
static UINT32 arrow_pixels[16 * 20];
static void os_mouse_present(void)
{
    UINT64 f = k_spin_lock(&os_input_lock);
    BOOLEAN dirty = mouse_draw_dirty;
    INT32 x = mouse.x, y = mouse.y, scroll = mouse_scroll_pending;
    mouse_draw_dirty = FALSE;
    mouse_scroll_pending = 0;
    k_spin_unlock(&os_input_lock, f);
    if (scroll)
        kernel_console_scroll(scroll);
    if (dirty)
        kernel_overlay_move(x, y);
}
static void os_input_worker(void *arg)
{
    (void)arg;
    k_input_report r;
    BOOLEAN handed_off = FALSE;
    for (;;) {
        if (k_input_claimed()) {
            handed_off = TRUE;
            repeat_device = 0;
            k_sleep(10);
            continue;
        }
        if (handed_off) {
            for (UINT32 i = 0; i < K_INPUT_MAX_DEVICES; ++i)
                if (os_devices[i].active)
                    os_input_reset(&os_devices[i], K_INPUT_ERROR);
            handed_off = FALSE;
        }
        k_input_pump();
        for (UINT32 n = 0; n < 256 && !k_input_read(&r); ++n)
            os_input_report(&r);
        if (repeat_device && k_uptime_ms() >= repeat_at) {
            os_input_device *d = &os_devices[repeat_device - 1];
            if (d->active && os_key_is_down(d->keys, repeat_usage))
                os_key_put(os_usb_character(repeat_usage));
            else
                repeat_device = 0;
            repeat_at = k_uptime_ms() + 30;
        }
        os_mouse_present();
        k_sleep(1);
    }
}
static void os_input_init(void)
{
    mouse.x = (INT32)kernel_fb_width() / 2;
    mouse.y = (INT32)kernel_fb_height() / 2;
    UINT8 descriptor[K_INPUT_DESCRIPTOR_MAX];
    for (UINT32 i = 0; i < k_input_count(); ++i) {
        k_input_device info;
        if (k_input_get(i, &info) || !info.id || info.id > K_INPUT_MAX_DEVICES)
            continue;
        os_input_device *d = &os_devices[info.id - 1];
        d->transport = info;
        if (info.transport == K_INPUT_I8042) {
            if (!info.port)
                d->active = TRUE;
            else {
                int e = os_ps2_mouse_init();
                mouse_ps2_active = !e;
                d->active = !e;
                d->layout.mouse = !e;
                if (!e) {
                    con_print("PS/2 mouse: ID ");
                    con_print_uint(mouse_ps2_id);
                    con_print(mouse_ps2_id ? ", wheel enabled\n" : ", three-button packets\n");
                }
            }
            continue;
        }
        UINT32 n = 0;
        int parsed = k_usb_report_descriptor(info.id, descriptor, sizeof(descriptor), &n);
        if (!parsed)
            parsed = os_hid_parse(descriptor, n, &d->layout);
        if (parsed)
            zero_(&d->layout, sizeof(d->layout));
        int e = 0;
        if (info.interface_subclass == 1 && info.interface_protocol == 1) {
            e = k_usb_input_protocol(info.id, 0);
            d->boot_keys = !e;
        } else if (!parsed && (d->layout.mouse || d->layout.keyboard)) {
            if (info.interface_subclass == 1)
                e = k_usb_input_protocol(info.id, 1);
        } else if (info.interface_subclass == 1 && info.interface_protocol == 2) {
            e = k_usb_input_protocol(info.id, 0);
            d->boot_pointer = !e;
        } else
            e = parsed ? parsed : K_ENOTSUP;
        if (!e)
            e = k_usb_input_start(info.id);
        d->active = !e;
        if (d->active) {
            con_print("USB input ");
            con_print_uint(info.id);
            con_print(d->boot_keys || d->layout.keyboard ? ": keyboard" : ": mouse");
            if (d->layout.mouse)
                con_print(" with descriptor-decoded axes/buttons/scroll");
            if (d->boot_pointer)
                con_print(" (boot protocol: no wheel)");
            con_print("\n");
        }
    }
    /* Discard AUX bytes captured before the negotiated packet format began;
     * retain primary-port keystrokes that arrived during initialization. */
    k_input_report old;
    while (!k_input_read(&old))
        if (old.device != 2)
            os_input_report(&old);
    BOOLEAN pointer = FALSE;
    for (UINT32 i = 0; i < K_INPUT_MAX_DEVICES; ++i)
        pointer |=
            os_devices[i].active && (os_devices[i].layout.mouse || os_devices[i].boot_pointer);
    if (pointer) {
        for (UINT32 y = 0; y < 20; ++y)
            for (UINT32 x = 0; x < 16; ++x)
                arrow_pixels[y * 16 + x] = arrow_mask[y][x] == '#'   ? 0xff101010
                                           : arrow_mask[y][x] == '.' ? 0xffeeeeee
                                                                     : 0;
        (void)kernel_overlay_set(mouse.x, mouse.y, 16, 20, arrow_pixels);
    }
    int e = k_task_create("input", os_input_worker, NULL, 256, 0, &os_input_task);
    if (e)
        kernel_halt("cannot create OS input task");
}
static void cmd_inputdevices(void)
{
    for (UINT32 i = 0; i < K_INPUT_MAX_DEVICES; ++i) {
        os_input_device *d = &os_devices[i];
        if (!d->transport.id)
            continue;
        con_print("input ");
        con_print_uint(d->transport.id);
        con_print(d->transport.transport == K_INPUT_USB ? " USB" : " PS/2");
        con_print(d->active ? " active" : " unavailable/unsupported");
        con_print(" buttons/axes=");
        con_print_uint(d->layout.mouse || d->boot_pointer);
        con_print(" fields=");
        con_print_uint(d->layout.field_count);
        con_print(" report IDs=");
        con_print_uint(d->layout.ids);
        con_print(" dropped=");
        con_print_uint(d->lost);
        con_print("\n");
    }
}
static void cmd_mousetest(void)
{
    con_print("Mouse test: move, click all buttons, scroll vertically/horizontally. ESC exits.\n");
    os_mouse_event event;
    while (os_mouse_read(&event)) {
    }
    UINT64 next = 0;
    for (;;) {
        int c = os_pollchar();
        if (c == 27)
            break;
        BOOLEAN change = FALSE;
        UINT32 changed = 0;
        while (os_mouse_read(&event)) {
            change = TRUE;
            changed |= event.changed;
        }
        if (change && (changed || k_uptime_ms() >= next)) {
            os_mouse_state s = os_mouse_snapshot();
            con_print("x=");
            con_print_int(s.x);
            con_print(" y=");
            con_print_int(s.y);
            con_print(" buttons=");
            con_print_hex(s.buttons);
            con_print(" wheel V=");
            con_print_int(os_clamp64(s.wheel_y, -2147483647, 2147483647));
            con_print(" H=");
            con_print_int(os_clamp64(s.wheel_x, -2147483647, 2147483647));
            con_print(" changed=");
            con_print_hex(changed);
            con_print("\n");
            next = k_uptime_ms() + 100;
        }
        k_sleep(1);
    }
    kernel_console_scroll(-2147483647);
    con_print("Mouse test complete.\n");
}

static void cmd_inputtest(void)
{
    static const UINT8 descriptor[] = {
        0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x07, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
        0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x05, 0x81, 0x02, 0x75, 0x03, 0x95, 0x01,
        0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x00, 0xf8, 0x26, 0xff, 0x07,
        0x75, 0x0c, 0x95, 0x02, 0x81, 0x06, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08,
        0x95, 0x01, 0x81, 0x06, 0x05, 0x0c, 0x0a, 0x38, 0x02, 0x81, 0x06, 0xc0};
    static const UINT8 packet[] = {7, 0x15, 0xef, 0x1f, 0x10, 0xff, 2};
    os_hid_layout layout;
    int e = os_hid_parse(descriptor, sizeof(descriptor), &layout);
    UINT32 seen = 0, buttons = 0;
    INT64 x = 0, y = 0, wheel = 0, pan = 0;
    if (!e) {
        for (UINT32 i = 0; i < layout.field_count; ++i) {
            os_hid_field *f = &layout.fields[i];
            UINT32 raw;
            if (!hid_extract(packet + 1, sizeof(packet) - 1, f->offset, f->width, &raw)) {
                e = K_EINVAL;
                break;
            }
            INT64 value = f->minimum < 0 ? hid_signed(raw, f->width) : (INT64)raw;
            if (f->kind == HID_BUTTON && value)
                buttons |= 1U << (f->usage - 1);
            if (f->kind == HID_X) {
                x = value;
                seen |= 1;
            }
            if (f->kind == HID_Y) {
                y = value;
                seen |= 2;
            }
            if (f->kind == HID_WHEEL) {
                wheel = value;
                seen |= 4;
            }
            if (f->kind == HID_PAN) {
                pan = value;
                seen |= 8;
            }
        }
    }
    report("HID report ID, five buttons, signed 12-bit XY, vertical wheel and horizontal pan",
           !e && layout.ids && layout.mouse && !layout.keyboard && layout.report_bits[7] == 48 &&
               seen == 15 && buttons == 0x15 && x == -17 && y == 257 && wheel == -1 && pan == 2);
    static const UINT8 truncated[] = {0x77, 1}, bad_pop[] = {0xb4};
    UINT32 value = 0;
    report("HID parser rejects truncated items, unbalanced stacks and short reports",
           os_hid_parse(descriptor, sizeof(descriptor) - 1, &layout) == K_EINVAL &&
               os_hid_parse(truncated, sizeof(truncated), &layout) == K_EINVAL &&
               os_hid_parse(bad_pop, sizeof(bad_pop), &layout) == K_EINVAL &&
               !hid_extract(packet, 1, 7, 12, &value));
    UINT8 ps2[] = {0x1b, 0xfe, 3, 0x3f};
    INT32 dx, dy, scroll;
    UINT32 pressed;
    BOOLEAN ok = os_ps2_decode(ps2, 4, &dx, &dy, &scroll, &pressed) && dx == -2 && dy == -3 &&
                 scroll == 1 && pressed == 0x1b;
    ps2[3] = 0xff;
    ok = ok && os_ps2_decode(ps2, 3, &dx, &dy, &scroll, &pressed) && scroll == 1 && pressed == 3;
    ok = ok && os_ps2_decode(ps2, 0, &dx, &dy, &scroll, &pressed) && !scroll && pressed == 3;
    ps2[0] |= 0xc0;
    ok = ok && os_ps2_decode(ps2, 4, &dx, &dy, &scroll, &pressed) && !dx && !dy;
    ps2[0] = 0;
    ok = ok && !os_ps2_decode(ps2, 4, &dx, &dy, &scroll, &pressed);
    report("PS/2 standard, wheel and five-button packets; overflow and synchronization bit", ok);
}

static void cmd_cachetest(void)
{
    UINT64 p = pmm_alloc_page(), flags = 0, cr0;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    int e = p ? vmm_translate(kernel_get_pml4(), p, NULL, &flags) : K_ENOMEM;
    report("allocated RAM selects write-back PAT entry with CPU caches enabled",
           !e && !(flags & (PAGE_PCD | PAGE_PWT)) && !(cr0 & ((1ULL << 30) | (1ULL << 29))));
    if (p)
        pmm_free_page(p);
    report("console has RAM text buffer and no GPU readback", kernel_console_buffered());
    con_print(kernel_fb_pat_wc() ? "Framebuffer PAT requests write combining; MTRRs retained.\n"
                                 : "Framebuffer uses uncached fallback (no PAT).\n");
}
static void cmd_consoletest(void)
{
    con_print("Visual scroll check: ordered rows, ending with SCROLL COMPLETE.\n");
    UINT64 start = k_uptime_ms();
    for (UINT32 row = 0; row < kernel_con_rows() + 8; ++row) {
        con_print("scroll row ");
        con_print_uint(row);
        con_print(" | abcdefghijklmnopqrstuvwxyz 0123456789\n");
    }
    con_print("SCROLL COMPLETE; elapsed ms=");
    con_print_uint(k_uptime_ms() - start);
    con_print(". Type normally at the prompt to check input responsiveness.\n");
}

static void cmd_pmmtest(void)
{
    UINT64 before = k_pmm_free_count(), p = pmm_alloc_page();
    if (!p) {
        report("PMM allocation", FALSE);
        return;
    }
    volatile UINT64 *v = (void *)(UINTN)p;
    BOOLEAN zero = TRUE;
    for (UINT32 i = 0; i < 512; ++i)
        if (v[i])
            zero = FALSE;
    for (UINT32 i = 0; i < 512; ++i)
        v[i] = 0xa55aa55af00df00dULL ^ i;
    BOOLEAN data = TRUE;
    for (UINT32 i = 0; i < 512; ++i)
        if (v[i] != (0xa55aa55af00df00dULL ^ i))
            data = FALSE;
    pmm_free_page(p + 1);
    BOOLEAN aligned = k_pmm_free_count() == before - 1;
    pmm_free_page(0);
    pmm_free_page(p);
    pmm_free_page(p);
    report("PMM zero-fill, full-page pattern, invalid/double free",
           zero && data && aligned && k_pmm_free_count() == before);
}
static void cmd_astest(void)
{
    k_address_space *a = k_as_create(), *b = k_as_create();
    UINT64 va = 0, vb = 0;
    if (!a || !b) {
        report("address-space allocation", FALSE);
        goto done;
    }
    int x = k_as_alloc(a, 8192, 4096, 3, &va), y = k_as_alloc(b, 8192, 4096, 3, &vb);
    UINT64 p = 0x1111222233334444ULL, q = 0xaaaabbbbccccddddULL, ra = 0, rb = 0;
    BOOLEAN ok = !x && !y && va == vb;
    if (ok)
        ok = !k_copy_to_user(a, va + 4093, &p, 8) && !k_copy_to_user(b, vb + 4093, &q, 8) &&
             !k_copy_from_user(a, &ra, va + 4093, 8) && !k_copy_from_user(b, &rb, vb + 4093, 8) &&
             ra == p && rb == q;
    report("same VA, separate physical backing; cross-page copies", ok);
    if (!x) {
        report("user-range wraparound, guard page, supervisor pointer rejection",
               k_as_check(a, ~0ULL - 2, 8, FALSE) == K_EFAULT &&
                   k_as_check(a, va + 8191, 2, FALSE) == K_EFAULT &&
                   k_as_check(a, (UINT64)(UINTN)&ST, 8, FALSE) == K_EFAULT);
        UINT64 ro = 0;
        int e = k_as_alloc(a, 4096, 4096, 1, &ro);
        report("read-only user page rejects copy-to-user",
               !e && k_copy_to_user(a, ro, &p, 8) == K_EFAULT);
        report("W+X region rejected", k_as_alloc(a, 4096, 4096, 7, &ro) == K_EINVAL);
    }
done:
    if (a)
        k_as_destroy(a);
    if (b)
        k_as_destroy(b);
}
static void cmd_vmmtest(void)
{
    UINT64 p = pmm_alloc_page();
    if (!p) {
        report("VMM page allocation", FALSE);
        return;
    }
    int e = vmm_map_page(kernel_get_pml4(), K_TEST_VA, p, PAGE_RW | PAGE_NX);
    if (e == K_EPERM) {
        con_print("Kernel mappings fixed after SMP start; checking private address spaces.\n");
        pmm_free_page(p);
        cmd_astest();
        return;
    }
    if (e) {
        error_("vmm map", e);
        pmm_free_page(p);
        return;
    }
    volatile UINT64 *v = (void *)(UINTN)K_TEST_VA;
    *v = 0xcafebabef00dbabeULL;
    BOOLEAN ok = *(volatile UINT64 *)(UINTN)p == *v;
    UINT64 translated = 0;
    ok = ok && !vmm_translate(kernel_get_pml4(), K_TEST_VA, &translated, NULL) && translated == p;
    e = vmm_unmap_page(kernel_get_pml4(), K_TEST_VA);
    ok = ok && !e && vmm_translate(kernel_get_pml4(), K_TEST_VA, NULL, NULL) == K_EFAULT;
    if (!e)
        pmm_free_page(p);
    report("live VA alias, translation, unmap", ok);
}
static void cmd_pathtest(void)
{
    char path[K_PATH_MAX];
    k_file a, b;
    char data[32];
    UINT64 got = 0;
    const char *sample = "namespace works";
    int e = k_vfs_put("@user/probe.txt", sample, strlen_(sample));
    BOOLEAN ok = !e && !k_vfs_open("/users/pavlkon/probe.txt", &a) &&
                 !k_vfs_open("@user/probe.txt", &b) && a.kind == b.kind && a.object == b.object &&
                 !k_vfs_read(&a, 0, data, sizeof(data), &got) && got == strlen_(sample) &&
                 equal_(data, sample, got);
    report("RAM file via mounted and named-root paths", ok);
    report("path normalization and named-root confinement",
           !k_path_resolve("@system//bin/./hello.rief", "/", path) &&
               streq_(path, "/system/bin/hello.rief") &&
               k_path_resolve("@system/../devices", "/", path) == K_EPERM &&
               k_path_resolve("@missing/file", "/", path) == K_ENOENT);
}

static struct {
    volatile UINT32 done, go, stop;
    volatile UINT64 counters[2];
    UINT64 end_tick;
    UINT32 target;
    k_mutex mutex;
    k_semaphore sem;
    k_spinlock spin;
    volatile UINT64 protected_counter;
} live;
static UINT32 live_task_ids[2];
static BOOLEAN live_begin(void)
{
    for (UINT32 i = 0; i < 2; ++i) {
        if (!live_task_ids[i])
            continue;
        int e = k_task_reap(live_task_ids[i], NULL);
        if (e && e != K_ENOENT) {
            error_("previous live worker still active", e);
            return FALSE;
        }
        live_task_ids[i] = 0;
    }
    zero_(&live, sizeof(live));
    return TRUE;
}
static void yield_worker(void *arg)
{
    UINT32 i = (UINT32)(UINTN)arg;
    for (UINT32 n = 0; n < 1000 && !live.stop; ++n) {
        ++live.counters[i];
        k_yield();
    }
    __atomic_add_fetch(&live.done, 1, __ATOMIC_RELEASE);
}
static void hog_worker(void *arg)
{
    UINT32 i = (UINT32)(UINTN)arg;
    UINT64 loops = 0;
    while (!__atomic_load_n(&live.go, __ATOMIC_ACQUIRE))
        k_yield();
    /* No voluntary yield in the measured loop: only timer preemption can
     * let the other CPU-0 worker and the shell make progress. */
    while (k_ticks() < live.end_tick && !live.stop && loops < 300000000ULL) {
        ++live.counters[i];
        ++loops;
        __asm__ volatile("pause");
    }
    __atomic_add_fetch(&live.done, 1, __ATOMIC_RELEASE);
}
static void mutex_worker(void *arg)
{
    (void)arg;
    for (UINT32 i = 0; i < 200 && !live.stop; ++i) {
        if (k_mutex_lock(&live.mutex))
            break;
        UINT64 value = live.protected_counter;
        k_yield();
        live.protected_counter = value + 1;
        k_mutex_unlock(&live.mutex);
    }
    k_sem_post(&live.sem);
    __atomic_add_fetch(&live.done, 1, __ATOMIC_RELEASE);
}
static void ipc_worker(void *arg)
{
    UINT64 value = 0xfeed0000ULL + (UINTN)arg;
    int e = k_ipc_send(live.target, &value, sizeof(value));
    live.counters[(UINTN)arg] = e ? 0 : 1;
    __atomic_add_fetch(&live.done, 1, __ATOMIC_RELEASE);
}
static void semaphore_worker(void *arg)
{
    if (!arg) {
        live.counters[0] = k_sem_wait(&live.sem) ? 0 : 1;
    } else {
        k_sleep(30);
        k_sem_post(&live.sem);
    }
    __atomic_add_fetch(&live.done, 1, __ATOMIC_RELEASE);
}
static void worker_diagnostics(const UINT32 ids[2])
{
    con_print("Worker progress: ");
    con_print_uint(live.counters[0]);
    con_print(" / ");
    con_print_uint(live.counters[1]);
    con_print("; completed=");
    con_print_uint(__atomic_load_n(&live.done, __ATOMIC_ACQUIRE));
    con_print("; spinlock counter=");
    con_print_uint(live.protected_counter);
    con_print("\n");
    for (UINT32 n = 0; n < 2; ++n) {
        k_task_info info;
        con_print("task ");
        con_print_uint(ids[n]);
        int e = k_task_query(ids[n], &info);
        if (e == K_ENOENT) {
            con_print(" reaped\n");
            continue;
        }
        if (e) {
            error_(" query", e);
            continue;
        }
        con_print(" cpu=");
        con_print_uint(info.cpu);
        con_print(" state=");
        con_print_uint(info.state);
        con_print(" ticks=");
        con_print_uint(info.runtime_ticks);
        con_print(" switches=");
        con_print_uint(info.switches);
        con_print(" vruntime=");
        con_print_uint(info.virtual_runtime);
        con_print("\n");
    }
}
static BOOLEAN wait_workers(UINT32 ids[2], UINT64 ms, UINT64 runtime[2])
{
    UINT64 deadline = k_uptime_ms() + ms, progress_at = k_uptime_ms() + 1000;
    BOOLEAN reaped[2] = {FALSE, FALSE};
    while (k_uptime_ms() < deadline) {
        for (UINT32 n = 0; n < 2; ++n) {
            if (reaped[n])
                continue;
            k_task_info info;
            /* One lookup per worker, instead of acquiring the global scheduler
             * lock once for each of 128 slots on every shell polling tick. */
            if (!k_task_query(ids[n], &info) && info.state == K_TASK_ZOMBIE) {
                if (runtime)
                    runtime[n] = info.runtime_ticks;
                if (!k_task_reap(ids[n], NULL)) {
                    reaped[n] = TRUE;
                    live_task_ids[n] = 0;
                }
            }
        }
        if (reaped[0] && reaped[1])
            return __atomic_load_n(&live.done, __ATOMIC_ACQUIRE) == 2;
        if (k_uptime_ms() >= progress_at) {
            con_print("Waiting for workers; completed=");
            con_print_uint(__atomic_load_n(&live.done, __ATOMIC_ACQUIRE));
            con_print("\n");
            progress_at = k_uptime_ms() + 1000;
        }
        k_sleep(1);
    }
    live.stop = 1;
    con_print("Worker deadline expired. States: 1 ready, 2 running, 3 sleeping, "
              "4 blocked, 5 zombie.\n");
    worker_diagnostics(ids);
    return FALSE;
}
static BOOLEAN start_workers(k_task_entry entry, UINT32 w0, UINT32 w1, UINT32 ids[2])
{
    int a = k_task_create("live-a", entry, (void *)0, w0, 0, &ids[0]);
    if (a) {
        error_("worker a", a);
        return FALSE;
    }
    live_task_ids[0] = ids[0];
    int b = k_task_create("live-b", entry, (void *)1, w1, 0, &ids[1]);
    if (b) {
        error_("worker b", b);
        live.stop = 1;
        live.go = 1;
        return FALSE;
    }
    live_task_ids[1] = ids[1];
    return TRUE;
}
static void cmd_tasktest(void)
{
    if (!live_begin())
        return;
    UINT32 ids[2];
    if (!start_workers(yield_worker, 1024, 1024, ids))
        return;
    live.go = 1;
    BOOLEAN ok = wait_workers(ids, 10000, NULL);
    BOOLEAN passed = ok && live.counters[0] == 1000 && live.counters[1] == 1000;
    report("two kernel stacks, cooperative switches and task reaping", passed);
    if (ok && !passed)
        worker_diagnostics(ids);
}
static void cpu_share_test(BOOLEAN weighted)
{
    if (!live_begin())
        return;
    UINT32 ids[2];
    UINT64 runtime[2] = {0};
    if (!start_workers(hog_worker, weighted ? 512 : 1024, weighted ? 2048 : 1024, ids))
        return;
    live.end_tick = k_ticks() + k_tick_hz();
    __atomic_store_n(&live.go, 1, __ATOMIC_RELEASE);
    BOOLEAN done = wait_workers(ids, 5000, runtime);
    con_print("worker timer ticks: ");
    con_print_uint(runtime[0]);
    con_print(" / ");
    con_print_uint(runtime[1]);
    con_print("\n");
    report(weighted ? "weighted fair scheduling favors weight 2048 over 512"
                    : "timer preempts both non-yielding kernel tasks",
           done && live.counters[0] && live.counters[1] && runtime[0] && runtime[1] &&
               (!weighted || runtime[1] > runtime[0]));
}
static void cmd_timertest(void)
{
    UINT64 before = k_ticks();
    k_delay_us(100000);
    UINT64 after = k_ticks();
    con_print("ticks over roughly 100 ms: ");
    con_print_uint(after - before);
    con_print("\n");
    report("timer IRQ advances while shell is busy", after > before);
}
static void cmd_synctest(void)
{
    if (!live_begin())
        return;
    UINT32 ids[2];
    k_sem_init(&live.sem, 0);
    BOOLEAN empty = k_sem_trywait(&live.sem) == K_EAGAIN;
    if (!start_workers(mutex_worker, 1024, 1024, ids))
        return;
    live.go = 1;
    BOOLEAN done = wait_workers(ids, 10000, NULL);
    BOOLEAN sem = !k_sem_trywait(&live.sem) && !k_sem_trywait(&live.sem) &&
                  k_sem_trywait(&live.sem) == K_EAGAIN;
    report("mutex blocks across yield; counting semaphore",
           done && empty && sem && live.protected_counter == 400);
    if (!done)
        return;
    int a = k_mutex_lock(&live.mutex), b = k_mutex_trylock(&live.mutex);
    if (!a)
        k_mutex_unlock(&live.mutex);
    report("recursive mutex acquisition reports deadlock", !a && b == K_EDEADLK);
    if (!live_begin())
        return;
    k_sem_init(&live.sem, 0);
    if (!start_workers(semaphore_worker, 1024, 1024, ids))
        return;
    live.go = 1;
    done = wait_workers(ids, 5000, NULL);
    report("semaphore waiter blocks and wakes after a delayed post", done && live.counters[0] == 1);
}
static void cmd_ipctest(void)
{
    if (!live_begin())
        return;
    k_message msg;
    while (!k_ipc_receive(&msg)) {
    }
    UINT32 ids[2];
    live.target = k_task_id();
    if (!start_workers(ipc_worker, 1024, 1024, ids))
        return;
    live.go = 1;
    BOOLEAN done = wait_workers(ids, 5000, NULL), seen[2] = {FALSE, FALSE};
    while (!k_ipc_receive(&msg)) {
        UINT64 value = 0;
        if (msg.size == 8)
            copy_(&value, msg.bytes, 8);
        for (UINT32 i = 0; i < 2; ++i)
            if (msg.sender == ids[i] && value == 0xfeed0000ULL + i)
                seen[i] = TRUE;
    }
    report("IPC preserves sender, size and message bytes", done && seen[0] && seen[1]);
    UINT8 byte = 7;
    UINT32 n = 0;
    int e;
    while (n < 128 && !(e = k_ipc_send(k_task_id(), &byte, 1)))
        ++n;
    BOOLEAN full = e == K_EAGAIN && n > 0;
    while (!k_ipc_receive(&msg)) {
    }
    report("bounded mailbox reports backpressure", full);
}
static void smp_worker(void *arg)
{
    UINT32 index = (UINT32)(UINTN)arg;
    for (UINT32 i = 0; i < 20000 && !live.stop; ++i) {
        UINT64 f = k_spin_lock(&live.spin);
        ++live.protected_counter;
        k_spin_unlock(&live.spin, f);
        if (!(i & 127))
            k_yield();
    }
    live.counters[index] = k_cpu_id() + 1;
    __atomic_add_fetch(&live.done, 1, __ATOMIC_RELEASE);
}
static void cmd_smptest(void)
{
    if (k_platform()->online_cpus < 2) {
        con_print("Use smpstart first; at least two online CPUs are required.\n");
        return;
    }
    if (!live_begin())
        return;
    UINT32 ids[2];
    int a = k_task_create("smp-bsp", smp_worker, (void *)0, 1024, 0, &ids[0]);
    if (!a)
        live_task_ids[0] = ids[0];
    int b = a ? a : k_task_create("smp-ap", smp_worker, (void *)1, 1024, 1, &ids[1]);
    if (!b)
        live_task_ids[1] = ids[1];
    if (a || b) {
        error_("SMP worker", a ? a : b);
        live.stop = 1;
        return;
    }
    live.go = 1;
    BOOLEAN done = wait_workers(ids, 10000, NULL);
    BOOLEAN passed =
        done && live.protected_counter == 40000 && live.counters[0] == 1 && live.counters[1] == 2;
    report("BSP/AP execute tasks and share a spinlock", passed);
    if (done && !passed)
        worker_diagnostics(ids);
}

static UINT32 make_rief(UINT8 out[1024], const UINT8 *code, UINT32 code_size, const UINT8 *data,
                        UINT32 data_size, UINT32 data_flags, INT32 patch, UINT32 relocation_type)
{
    zero_(out, 1024);
    rief_header_t h = {0};
    h.magic = RIEF_MAGIC;
    h.version_major = 1;
    h.architecture = RIEF_ARCH_X86_64;
    h.header_size = sizeof(h);
    h.region_count = 2;
    h.region_table_offset = 96;
    h.reloc_count = patch >= 0 ? 1 : 0;
    h.reloc_table_offset = patch >= 0 ? 160 : 0;
    h.export_count = 1;
    h.export_table_offset = patch >= 0 ? 200 : 160;
    h.string_table_offset = h.export_table_offset + 16;
    h.string_table_size = 6;
    UINT64 code_offset = (h.string_table_offset + 6 + 15) & ~15ULL;
    rief_region_t regions[2] = {
        {RIEF_REGION_R | RIEF_REGION_X, 4096, code_offset, code_size, code_size},
        {data_flags, 4096, code_offset + code_size, data_size, 4096}};
    if (code_offset + code_size + data_size > 1024 || !code_size)
        return 0;
    copy_(out, &h, sizeof(h));
    copy_(out + 96, regions, sizeof(regions));
    if (patch >= 0) {
        rief_reloc_t reloc = {0, relocation_type, 1, 0, (UINT64)patch, 0, 0};
        copy_(out + 160, &reloc, sizeof(reloc));
    }
    rief_export_t ex = {0, 0, 0};
    copy_(out + h.export_table_offset, &ex, sizeof(ex));
    copy_(out + h.string_table_offset, "entry", 6);
    copy_(out + code_offset, code, code_size);
    if (data_size)
        copy_(out + code_offset + code_size, data, data_size);
    return (UINT32)(code_offset + code_size + data_size);
}
static const UINT8 hello_code[] = {
    0xb8, 1,  0, 0, 0, 0xbf, 1,    0,    0,  0, 0x48, 0xbe, 0,    0,    0,    0,    0,    0,   0, 0,
    0xba, 24, 0, 0, 0, 0x0f, 0x05, 0xbf, 42, 0, 0,    0,    0x31, 0xc0, 0x0f, 0x05, 0x0f, 0x0b};
static const UINT8 hello_data[] = "hello from ring 3 RIEF!\n";
static const UINT8 badptr_code[] = {0xb8, 1,    0,    0,    0,    0xbf, 1,    0,    0,    0,
                                    0xbe, 1,    0,    0,    0,    0xba, 1,    0,    0,    0,
                                    0x0f, 0x05, 0x48, 0x83, 0xf8, 0xfb, 0x0f, 0x95, 0xc0, 0x0f,
                                    0xb6, 0xf8, 0x31, 0xc0, 0x0f, 0x05, 0x0f, 0x0b};
static const UINT8 int80_code[] = {0xb8, 4,    0,    0,    0,    0xcd, 0x80, 0x48,
                                   0x85, 0xc0, 0x0f, 0x94, 0xc0, 0x0f, 0xb6, 0xf8,
                                   0x31, 0xc0, 0xcd, 0x80, 0x0f, 0x0b};
/* movq constant -> XMM0; sleep eight times (other contexts load their own
 * FPU state); compare XMM0 after resumption; exit 0 only if it survived. */
static const UINT8 sse_code[] = {
    0x48, 0xbb, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x66, 0x48, 0x0f, 0x6e, 0xc3,
    0xbd, 8,    0,    0,    0,    0xb8, 3,    0,    0,    0,    0xbf, 1,    0,    0,    0,
    0x0f, 0x05, 0xff, 0xcd, 0x75, 0xf0, 0x66, 0x48, 0x0f, 0x7e, 0xc7, 0x48, 0x39, 0xdf, 0x40,
    0x0f, 0x95, 0xc7, 0x40, 0x0f, 0xb6, 0xff, 0x31, 0xc0, 0x0f, 0x05, 0x0f, 0x0b};
static const UINT8 badrsp_code[] = {0x48, 0xbc, 0, 0, 0, 0,    0,    0,    0,
                                    0x80, /* noncanonical RSP */
                                    0xb8, 4,    0, 0, 0, 0x0f, 0x05, 0x0f, 0x0b};
static BOOLEAN run_fixture(const UINT8 *bytes, UINT32 size, INT32 expected, UINT32 vector)
{
    k_process *p = k_process_create("live-rief");
    if (!p)
        return FALSE;
    int e = k_rief_load(p, bytes, size, NULL, 0);
    UINT32 task;
    if (!e)
        e = k_process_start(p, &task);
    if (e) {
        error_("RIEF fixture", e);
        k_process_destroy(p);
        return FALSE;
    }
    UINT64 deadline = k_uptime_ms() + 5000;
    BOOLEAN done = FALSE;
    k_process_info info;
    while (k_uptime_ms() < deadline) {
        if (process_status(p, &info) && info.state == K_TASK_ZOMBIE) {
            done = TRUE;
            break;
        }
        k_sleep(1);
    }
    BOOLEAN ok = done && info.exit_code == expected && info.fault_vector == vector;
    if (done && !k_process_destroy(p))
        return ok;
    for (UINT32 i = 0; i < K_MAX_PROCESSES; ++i)
        if (!launched[i]) {
            launched[i] = p;
            break;
        }
    if (!done)
        con_print("Fixture timed out; process retained for inspection with ps.\n");
    return ok;
}
static void cmd_rieftest(void)
{
    UINT8 bytes[1024], bad[1024];
    UINT32 size = make_rief(bytes, hello_code, sizeof(hello_code), hello_data,
                            sizeof(hello_data) - 1, 1, 12, RIEF_RELOC_ABS64);
    k_process *p = k_process_create("loader-check");
    UINT64 entry = 0;
    BOOLEAN ok = p && !k_rief_load(p, bytes, size, NULL, 0) && !k_rief_export(p, "entry", &entry) &&
                 entry >= K_USER_BASE;
    report("RIEF regions, ABS64 relocation and export lookup", ok);
    if (p)
        k_process_destroy(p);
    for (UINT32 variant = 0; variant < 5; ++variant) {
        copy_(bad, bytes, sizeof(bad));
        if (variant == 0)
            bad[0] = 0;
        if (variant == 1)
            put64_(bad + 32, ~0ULL - 7);
        if (variant == 2)
            put32_(bad + 96, 7);
        if (variant == 3)
            put64_(bad + 160 + 16, ~0ULL);
        if (variant == 4)
            put64_(bad + 96 + 16, ~0ULL);
        p = k_process_create("reject-check");
        int e = p ? k_rief_load(p, bad, size, NULL, 0) : 0;
        report("malformed RIEF rejected transactionally", p && e < 0);
        if (p)
            k_process_destroy(p);
    }
}

static void cmd_fileiotest(void)
{
    static const UINT8 program[] = {
        0x48, 0xbb, 0,    0,    0,    0,    0,    0,    0,    0,    0xb8, 8,    0,    0,    0,
        0x48, 0x89, 0xdf, 0xbe, 7,    0,    0,    0,    0x0f, 0x05, 0x48, 0x85, 0xc0, 0x78, 0x2e,
        0x49, 0x89, 0xc4, 0x4c, 0x89, 0xe7, 0x48, 0x8d, 0x73, 18,   0xba, 19,   0,    0,    0,
        0xb8, 1,    0,    0,    0,    0x0f, 0x05, 0x48, 0x83, 0xf8, 19,   0x75, 0x12, 0x4c, 0x89,
        0xe7, 0xb8, 10,   0,    0,    0,    0x0f, 0x05, 0x31, 0xff, 0x31, 0xc0, 0x0f, 0x05, 0x0f,
        0x0b, 0xbf, 1,    0,    0,    0,    0x31, 0xc0, 0x0f, 0x05, 0x0f, 0x0b};
    static const UINT8 data[] = "@user/syscall.txt\0file syscall works\n";
    UINT8 bytes[1024];
    UINT32 n =
        make_rief(bytes, program, sizeof(program), data, sizeof(data) - 1, 1, 2, RIEF_RELOC_ABS64);
    BOOLEAN ok = run_fixture(bytes, n, 0, 0);
    k_file f;
    char contents[32];
    UINT64 got = 0;
    int e = k_vfs_open("@user/syscall.txt", &f);
    if (!e)
        e = k_vfs_read(&f, 0, contents, sizeof(contents), &got);
    report("ring-3 file OPEN/WRITE/CLOSE and content readback",
           ok && !e && got == 19 && equal_(contents, "file syscall works\n", 19));
}
static void cmd_usertest(void)
{
    UINT8 bytes[1024];
    UINT32 n;
    n = make_rief(bytes, hello_code, sizeof(hello_code), hello_data, sizeof(hello_data) - 1, 1, 12,
                  RIEF_RELOC_ABS64);
    report("RIEF enters ring 3, SYSCALL writes, exit returns 42", run_fixture(bytes, n, 42, 0));
    n = make_rief(bytes, badptr_code, sizeof(badptr_code), NULL, 0, 1, -1, 0);
    report("invalid user pointer returns EFAULT without killing shell",
           run_fixture(bytes, n, 0, 0));
    n = make_rief(bytes, int80_code, sizeof(int80_code), NULL, 0, 1, -1, 0);
    report("INT 0x80 syscall gate and process identity", run_fixture(bytes, n, 0, 0));
    n = make_rief(bytes, sse_code, sizeof(sse_code), NULL, 0, 1, -1, 0);
    report("SSE state survives blocking syscalls and context switches",
           run_fixture(bytes, n, 0, 0));
    n = make_rief(bytes, badrsp_code, sizeof(badrsp_code), NULL, 0, 1, -1, 0);
    report("invalid user RSP is contained before IRET", run_fixture(bytes, n, -141, 13));
    cmd_fileiotest();
}
static void cmd_isolationtest(void)
{
    UINT64 sentinel = pmm_alloc_page();
    if (!sentinel) {
        report("isolation sentinel allocation", FALSE);
        return;
    }
    *(volatile UINT64 *)(UINTN)sentinel = 0xfedcba9876543210ULL;
    UINT8 code[] = {0x48, 0xb8, 0, 0,    0,    0, 0, 0,    0,   0,
                    0x48, 0xc7, 0, 0x34, 0x12, 0, 0, 0x0f, 0x0b};
    put64_(code + 2, sentinel);
    UINT8 bytes[1024];
    UINT32 n = make_rief(bytes, code, sizeof(code), NULL, 0, 1, -1, 0);
    BOOLEAN fault = run_fixture(bytes, n, -142, 14);
    BOOLEAN unchanged = *(volatile UINT64 *)(UINTN)sentinel == 0xfedcba9876543210ULL;
    report("ring-3 write to kernel-owned RAM faults; sentinel unchanged", fault && unchanged);
    if (fault)
        pmm_free_page(sentinel);
    else
        con_print("Isolation sentinel retained after failure or timeout.\n");
    const UINT8 cli_code[] = {0xfa, 0x0f, 0x0b};
    n = make_rief(bytes, cli_code, sizeof(cli_code), NULL, 0, 1, -1, 0);
    report("ring 3 cannot disable interrupts", run_fixture(bytes, n, -141, 13));
    const UINT8 jump[] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    const UINT8 ud2[] = {0x0f, 0x0b};
    n = make_rief(bytes, jump, sizeof(jump), ud2, sizeof(ud2), 3, 2, RIEF_RELOC_ABS64);
    report("NX prevents execution from a writable data region", run_fixture(bytes, n, -142, 14));
}

static void cmd_disks(void)
{
    for (UINT32 i = 0; i < k_disk_count(); ++i) {
        k_disk_info d;
        if (k_disk_get(i, &d))
            continue;
        con_print(d.name);
        con_print("  ");
        con_print(d.driver);
        con_print("  ");
        con_print(d.model);
        con_print("  sectors=");
        con_print_uint(d.sectors);
        con_print("  bytes/sector=");
        con_print_uint(d.sector_size);
        con_print(d.read_only ? "  raw read-only (file writes: see mounts)"
                              : "  volatile writable");
        con_print(d.online ? "\n" : "  OFFLINE\n");
    }
    if (!k_disk_count())
        con_print("No supported native block devices found.\n");
}
static void cmd_disktest(void)
{
    UINT8 buf[4096];
    UINT32 physical = 0;
    for (UINT32 i = 0; i < k_disk_count(); ++i) {
        k_disk_info d;
        k_disk_get(i, &d);
        if (!d.read_only)
            continue;
        ++physical;
        con_print(d.name);
        con_print(": ");
        int a = k_disk_read(i, 0, 1, buf, sizeof(buf));
        int b = a ? a : k_disk_read(i, d.sectors - 1, 1, buf, sizeof(buf));
        int c = k_disk_write(i, 0, 1, buf, sizeof(buf));
        report("first/last LBA reads; hardware write API refused", !a && !b && c == K_EROFS);
        if (a || b)
            error_("disk read", a ? a : b);
        report("one-past-end LBA refused",
               k_disk_read(i, d.sectors, 1, buf, sizeof(buf)) == K_EINVAL);
    }
    if (!physical)
        con_print("SKIP physical disk checks: none discovered.\n");
}
static void cmd_ramdisktest(void)
{
    if (ram_test_disk == 0xffffffff) {
        int e = k_ramdisk_create(128, &ram_test_disk);
        if (e) {
            error_("RAM disk", e);
            return;
        }
    }
    UINT8 a[512], b[512];
    for (UINT32 i = 0; i < 512; ++i)
        a[i] = (UINT8)(i ^ 0xa5);
    report("RAM disk permits bounded writes and reads",
           !k_disk_write(ram_test_disk, 5, 1, a, sizeof(a)) &&
               !k_disk_read(ram_test_disk, 5, 1, b, sizeof(b)) && equal_(a, b, sizeof(a)));
}
static void cmd_diskread(const char *args)
{
    UINT64 id, lba, count = 1;
    if (!number_(&args, &id) || !number_(&args, &lba)) {
        con_print("diskread <disk number> <LBA> [sector count]\n");
        return;
    }
    while (*args == ' ')
        ++args;
    if (*args && !number_(&args, &count)) {
        error_("count", K_EINVAL);
        return;
    }
    while (*args == ' ')
        ++args;
    k_disk_info info;
    if (*args || id > 0xffffffff || k_disk_get((UINT32)id, &info) || !count ||
        count > 4096 / info.sector_size) {
        error_("disk range", K_EINVAL);
        return;
    }
    UINT8 bytes[4096];
    int e = k_disk_read((UINT32)id, lba, (UINT32)count, bytes, sizeof(bytes));
    if (e) {
        error_("diskread", e);
        return;
    }
    UINT32 n = (UINT32)count * info.sector_size;
    con_print("Read ");
    con_print_uint(n);
    con_print(" bytes; showing first 256:\n");
    static const char hex[] = "0123456789abcdef";
    for (UINT32 i = 0; i < n && i < 256; i += 16) {
        con_print_hex(lba * info.sector_size + i);
        con_print("  ");
        for (UINT32 j = 0; j < 16 && i + j < n; ++j) {
            UINT8 b = bytes[i + j];
            con_putc(hex[b >> 4]);
            con_putc(hex[b & 15]);
            con_putc(' ');
        }
        con_putc('\n');
    }
}

static void list_entry(const k_dirent *d, void *unused)
{
    (void)unused;
    con_print(d->directory ? "[dir] " : "      ");
    con_print(d->name);
    if (!d->directory) {
        con_print("  ");
        con_print_uint(d->size);
    }
    con_print("\n");
}
static void mapping_entry(const char *a, const char *b, void *unused)
{
    (void)unused;
    con_print(a);
    con_print(" -> ");
    con_print(b);
    con_print("\n");
}
static void cmd_ls(const char *path)
{
    char resolved[K_PATH_MAX];
    int e = k_path_resolve(*path ? path : cwd, cwd, resolved);
    if (!e)
        e = k_vfs_list(resolved, list_entry, NULL);
    if (e)
        error_("ls", e);
}
static void cmd_cat(const char *path)
{
    char resolved[K_PATH_MAX];
    k_file f;
    int e = k_path_resolve(path, cwd, resolved);
    if (!e)
        e = k_vfs_open(resolved, &f);
    if (e) {
        error_("cat", e);
        return;
    }
    UINT64 offset = 0, total = f.size < 16384 ? f.size : 16384;
    while (offset < total) {
        UINT8 data[256];
        UINT64 got = 0;
        e = k_vfs_read(&f, offset, data,
                       total - offset < sizeof(data) ? total - offset : sizeof(data), &got);
        if (e || !got) {
            if (e)
                error_("cat read", e);
            break;
        }
        for (UINT64 i = 0; i < got; ++i)
            con_putc(data[i] == '\n' || (data[i] >= 32 && data[i] < 127) ? (char)data[i] : '.');
        offset += got;
    }
    con_print("\n");
    if (f.size > total)
        con_print("(display limited to 16 KiB)\n");
}
/* File writes are explicit shell operations; testall never writes a physical
 * filesystem. Paths here are whitespace-delimited; API paths can contain spaces. */
static BOOLEAN path_argument(const char **args, char out[K_PATH_MAX])
{
    while (**args == ' ')
        ++*args;
    UINT32 n = 0;
    while (**args && **args != ' ') {
        if (n + 1 == K_PATH_MAX)
            return FALSE;
        out[n++] = *(*args)++;
    }
    out[n] = 0;
    while (**args == ' ')
        ++*args;
    return n != 0;
}
static void cmd_write(const char *args, BOOLEAN append)
{
    char path[K_PATH_MAX], resolved[K_PATH_MAX];
    if (!path_argument(&args, path)) {
        con_print("Usage: write/append <path> <text>\n");
        return;
    }
    int e = k_path_resolve(path, cwd, resolved);
    if (!e && !append)
        e = k_vfs_put(resolved, args, strlen_(args));
    else if (!e) {
        k_file f;
        e = k_vfs_open(resolved, &f);
        if (e == K_ENOENT)
            e = k_vfs_create(resolved, &f);
        UINT64 written = 0;
        if (!e)
            e = k_vfs_write(&f, ~0ULL, args, strlen_(args), &written);
    }
    if (e)
        error_(append ? "append" : "write", e);
}
static void cmd_touch(const char *path)
{
    char resolved[K_PATH_MAX];
    k_file f;
    int e = k_path_resolve(path, cwd, resolved);
    if (!e) {
        e = k_vfs_create(resolved, &f);
        if (e == K_EEXIST)
            e = 0;
    }
    if (e)
        error_("touch", e);
}
static void cmd_mkdir(const char *path)
{
    char resolved[K_PATH_MAX];
    int e = k_path_resolve(path, cwd, resolved);
    if (!e)
        e = k_vfs_mkdir(resolved);
    if (e)
        error_("mkdir", e);
}
static void cmd_cp(const char *args)
{
    char src[K_PATH_MAX], dst[K_PATH_MAX], a[K_PATH_MAX], b[K_PATH_MAX];
    if (!path_argument(&args, src) || !path_argument(&args, dst) || *args) {
        con_print("Usage: cp <source> <destination>\n");
        return;
    }
    int e = k_path_resolve(src, cwd, a);
    if (!e)
        e = k_path_resolve(dst, cwd, b);
    k_file f;
    if (!e)
        e = k_vfs_open(a, &f);
    if (e) {
        error_("cp", e);
        return;
    }
    if ((f.attributes & 0x10) || f.kind == 3 || f.size > 4 * 1024 * 1024) {
        error_("cp", K_E2BIG);
        return;
    }
    UINT32 pages = (UINT32)((f.size + 4095) / 4096);
    UINT8 *buf = pages ? (void *)(UINTN)k_pmm_alloc_pages(pages, 0) : NULL;
    if (pages && !buf) {
        error_("cp", K_ENOMEM);
        return;
    }
    UINT64 got = 0;
    e = k_vfs_read(&f, 0, buf, f.size, &got);
    if (!e && got != f.size)
        e = K_EIO;
    if (!e)
        e = k_vfs_put(b, buf, got);
    if (buf)
        k_pmm_free_pages((UINT64)(UINTN)buf, pages);
    if (e)
        error_("cp", e);
}
static void cmd_writetest(const char *directory)
{
    char dir[K_PATH_MAX], path[K_PATH_MAX];
    int e = k_path_resolve(directory, cwd, dir);
    if (e) {
        error_("writetest", e);
        return;
    }
    /* Always reserve a new name exclusively; never overwrite an existing file. */
    k_file file;
    BOOLEAN made = FALSE;
    for (UINT32 i = 0; i < 1000; ++i) {
        textcopy_(path, dir, sizeof(path));
        UINTN n = strlen_(path);
        if (n + 16 >= sizeof(path)) {
            error_("path", K_E2BIG);
            return;
        }
        if (n > 1)
            path[n++] = '/';
        const char *prefix = "probe";
        while (*prefix)
            path[n++] = *prefix++;
        path[n++] = (char)('0' + i / 100);
        path[n++] = (char)('0' + (i / 10) % 10);
        path[n++] = (char)('0' + i % 10);
        textcopy_(path + n, ".txt", sizeof(path) - n);
        e = k_vfs_create(path, &file);
        if (!e) {
            made = TRUE;
            break;
        }
        if (e != K_EEXIST)
            break;
    }
    if (!made) {
        error_("create probe", e ? e : K_E2BIG);
        return;
    }
    UINT8 original[1537], readback[1538];
    for (UINT32 i = 0; i < sizeof(original); ++i)
        original[i] = (UINT8)(i * 73 + 11);
    UINT64 written = 0, got = 0;
    e = k_vfs_write(&file, 0, original, sizeof(original), &written);
    k_file second;
    if (!e)
        e = k_vfs_open(path, &second);
    if (!e)
        e = k_vfs_read(&second, 0, readback, sizeof(readback), &got);
    report("create and write across sectors/clusters", !e && written == sizeof(original) &&
                                                           got == sizeof(original) &&
                                                           equal_(original, readback, got));
    UINT8 tail = 0x5a;
    if (!e)
        e = k_vfs_write(&file, ~0ULL, &tail, 1, &written);
    if (!e)
        e = k_vfs_read(&second, 0, readback, sizeof(readback), &got);
    report("append refreshes an already-open file handle",
           !e && got == sizeof(readback) && readback[1537] == tail &&
               equal_(original, readback, sizeof(original)));
    if (!e)
        e = k_vfs_put(path, "persistent file write\n", 22);
    if (!e)
        e = k_vfs_read(&second, 0, readback, sizeof(readback), &got);
    report("replace/truncate and explicit flush",
           !e && got == 22 && equal_(readback, "persistent file write\n", 22) && !k_vfs_sync());
    con_print("Probe retained at ");
    con_print(path);
    con_print(". Physical filesystem writes persist across reboot; RAM does not.\n");
}
static UINT32 fat_test_disk = 0xffffffff;
static void cmd_fatwritetest(void)
{
    /* Construct a tiny FAT16 image ONLY in a newly allocated volatile RAM disk. */
    int e = 0;
    if (fat_test_disk == 0xffffffff) {
        UINT32 id;
        e = k_ramdisk_create(8192, &id);
        if (e) {
            error_("RAM FAT disk", e);
            return;
        }
        UINT8 sector[512];
        zero_(sector, sizeof(sector));
        sector[0] = 0xeb;
        sector[1] = 0x3c;
        sector[2] = 0x90;
        copy_(sector + 3, "MINIOS  ", 8);
        sector[12] = 2;
        sector[13] = 1;
        sector[14] = 1;
        sector[16] = 2;
        sector[18] = 2;
        sector[20] = 0x20;
        sector[21] = 0xf8;
        sector[22] = 32;
        sector[24] = 63;
        sector[26] = 255;
        sector[36] = 0x80;
        sector[38] = 0x29;
        put32_(sector + 39, 0x52414d46);
        copy_(sector + 43, "RAM TEST   ", 11);
        copy_(sector + 54, "FAT16   ", 8);
        sector[510] = 0x55;
        sector[511] = 0xaa;
        e = k_disk_write(id, 0, 1, sector, sizeof(sector));
        zero_(sector, sizeof(sector));
        sector[0] = 0xf8;
        sector[1] = sector[2] = sector[3] = 0xff;
        if (!e)
            e = k_disk_write(id, 1, 1, sector, sizeof(sector));
        if (!e)
            e = k_disk_write(id, 33, 1, sector, sizeof(sector));
        if (!e)
            e = k_vfs_mount_ramfat(id);
        if (e) {
            error_("RAM FAT mount", e);
            return;
        }
        fat_test_disk = id;
    }
    char root[K_PATH_MAX] = "@disk", digits[12];
    UINT32 count = 0, n = fat_test_disk;
    do {
        digits[count++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    UINT32 at = 5;
    while (count)
        root[at++] = digits[--count];
    root[at] = 0;
    char dir[K_PATH_MAX];
    textcopy_(dir, root, sizeof(dir));
    textcopy_(dir + at, "/write-test", sizeof(dir) - at);
    e = k_vfs_mkdir(dir);
    report("FAT directory and long filename creation", !e || e == K_EEXIST);
    if (!e || e == K_EEXIST)
        cmd_writetest(dir);
    char app[K_PATH_MAX];
    textcopy_(app, dir, sizeof(app));
    textcopy_(app + strlen_(app), "/hello.rief", sizeof(app) - strlen_(app));
    UINT8 bytes[1024];
    UINT32 size = make_rief(bytes, hello_code, sizeof(hello_code), hello_data,
                            sizeof(hello_data) - 1, 1, 12, RIEF_RELOC_ABS64);
    e = k_vfs_put(app, bytes, size);
    k_file f;
    UINT64 got = 0;
    if (!e)
        e = k_vfs_open(app, &f);
    if (!e)
        e = k_vfs_read(&f, 0, bytes, sizeof(bytes), &got);
    report("RIEF stored in FAT and loaded into ring 3",
           !e && got == size && run_fixture(bytes, size, 42, 0));
    con_print("Try cd ");
    con_print(dir);
    con_print(" then ./hello.rief\n");
}

static void cmd_launch(const char *args, UINT32 cls, UINT32 cpu)
{
    char path[K_PATH_MAX], resolved[K_PATH_MAX];
    if (!path_argument(&args, path)) {
        con_print("exec/ring1/helper <path> [arguments]\n");
        return;
    }
    int e = k_path_resolve(path, cwd, resolved);
    UINT32 pid = 0;
    if (!e)
        e = k_process_launch(resolved, args, cls, cpu, 0, &pid);
    if (e) {
        error_("launch", e);
        return;
    }
    k_process_info p;
    e = k_process_query(pid, &p);
    con_print("PID ");
    con_print_uint(pid);
    if (!e) {
        con_print(" task=");
        con_print_uint(p.task_id);
        con_print(" cpu=");
        con_print_uint(p.cpu);
        con_print(" class=");
        con_print_uint(p.execution_class);
    }
    con_print("\n");
}
static void cmd_exec(const char *args) { cmd_launch(args, K_EXEC_RING3, K_CPU_AUTO); }
static void cmd_execpu(const char *args, UINT32 cls)
{
    UINT64 cpu;
    if (!number_(&args, &cpu) || cpu >= K_MAX_CPUS) {
        con_print("execpu/ring1cpu <cpu> <path> [arguments]\n");
        return;
    }
    cmd_launch(args, cls, (UINT32)cpu);
}
static void cmd_reap(void)
{
    for (UINT32 i = 0; i < K_MAX_PROCESSES; ++i) {
        k_process_info p;
        if (k_process_get(i, &p) || p.state != K_TASK_ZOMBIE)
            continue;
        for (UINT32 j = 0; j < K_MAX_PROCESSES; ++j)
            if (launched[j] && k_process_id(launched[j]) == p.id)
                launched[j] = NULL;
        int e = k_process_reap_pid(p.id, NULL);
        if (e) {
            error_("reap", e);
            continue;
        }
        con_print("reaped PID ");
        con_print_uint(p.id);
        con_print(" exit=");
        con_print_int(p.exit_code);
        if (p.fault_vector) {
            con_print(" fault=");
            con_print_uint(p.fault_vector);
            con_print(" RIP=");
            con_print_hex(p.fault_ip);
        }
        con_print("\n");
    }
}
static void cmd_pids(void)
{
    for (UINT32 i = 0; i < K_MAX_PROCESSES; ++i) {
        k_process_info p;
        if (k_process_get(i, &p))
            continue;
        con_print("PID ");
        con_print_uint(p.id);
        con_print(" parent=");
        con_print_uint(p.parent);
        con_print(" class=");
        con_print_uint(p.execution_class);
        con_print(" task=");
        con_print_uint(p.task_id);
        con_print(" cpu=");
        con_print_uint(p.cpu);
        con_print(" state=");
        con_print_uint(p.state);
        con_print(" exit=");
        con_print_int(p.exit_code);
        con_putc(' ');
        con_print(p.name);
        con_putc('\n');
    }
}
static void cmd_control(const char *args, UINT32 op)
{
    UINT64 pid;
    if (!number_(&args, &pid) || pid > 0xffffffff) {
        error_("PID", K_EINVAL);
        return;
    }
    int e = k_process_control((UINT32)pid, op);
    if (e)
        error_("process control", e);
}
static BOOLEAN wait_process(UINT32 pid, UINT64 timeout, k_process_info *info)
{
    UINT64 start = k_uptime_ms();
    while (k_uptime_ms() - start < timeout) {
        if (k_process_query(pid, info))
            return FALSE;
        if (info->state == K_TASK_ZOMBIE)
            return TRUE;
        k_sleep(10);
    }
    return FALSE;
}
static void cmd_wait(const char *args)
{
    UINT64 pid;
    if (!number_(&args, &pid) || pid > 0xffffffff) {
        error_("PID", K_EINVAL);
        return;
    }
    con_print("Waiting; ESC leaves the wait.\n");
    for (;;) {
        k_process_info p;
        int e = k_process_query((UINT32)pid, &p);
        if (e) {
            error_("wait", e);
            return;
        }
        if (p.state == K_TASK_ZOMBIE) {
            con_print("exit=");
            con_print_int(p.exit_code);
            con_putc('\n');
            return;
        }
        if (os_pollchar() == 27)
            return;
        k_sleep(10);
    }
}
static void cmd_service(const char *args)
{
    char name[K_PATH_MAX];
    if (!path_argument(&args, name)) {
        con_print("service <name> [message up to 128 bytes]\n");
        return;
    }
    UINTN n = strlen_(args);
    if (n > K_IPC_BYTES) {
        error_("message", K_E2BIG);
        return;
    }
    UINT32 tid;
    int e = k_service_lookup(name, &tid);
    if (!e)
        e = k_ipc_send(tid, args, (UINT32)n);
    if (e) {
        error_("service", e);
        return;
    }
    UINT64 start = k_uptime_ms();
    while (k_uptime_ms() - start < 3000) {
        k_message m;
        e = k_ipc_receive(&m);
        if (!e) {
            con_print("Reply from task ");
            con_print_uint(m.sender);
            con_print(": ");
            for (UINT32 i = 0; i < m.size; ++i) {
                if (i)
                    con_putc(' ');
                con_print_hex(m.bytes[i]);
            }
            con_putc('\n');
            return;
        }
        if (e != K_EAGAIN) {
            error_("reply", e);
            return;
        }
        k_sleep(1);
    }
    error_("service reply", K_ETIMEDOUT);
}
static void cmd_partitions(void)
{
    for (UINT32 i = 0;; ++i) {
        k_partition_info p;
        if (k_partition_get(i, &p))
            break;
        con_print("partition ");
        con_print_uint(p.id);
        con_print(" disk=");
        con_print_uint(p.disk);
        con_print(" start=");
        con_print_uint(p.start);
        con_print(" sectors=");
        con_print_uint(p.sectors);
        con_print(" mounted=");
        con_print_uint(p.mounted);
        con_print(" RO=");
        con_print_uint(p.read_only);
        con_print(" owner=");
        con_print_uint(p.owner_pid);
        con_putc('\n');
    }
}

static void reap_after_test(UINT32 pid)
{
    for (UINT32 i = 0; i < 100; ++i) {
        int e = k_process_reap_pid(pid, NULL);
        if (!e || e == K_ENOENT)
            return;
        if (e != K_EBUSY) {
            error_("test reap", e);
            return;
        }
        k_sleep(1);
    }
}
static void cmd_ring1test(void)
{
    for (UINT32 i = 0; i < 2; ++i) {
        UINT32 pid = 0;
        int e = k_process_launch("@system/bin/ring1check.rief", "", i ? K_EXEC_RING1 : K_EXEC_RING3,
                                 0, 0, &pid);
        k_process_info info;
        BOOLEAN done = !e && wait_process(pid, 3000, &info);
        report(i ? "Ring 1 UUID/range/W^X checks and unusual mapping"
                 : "Ring 3 cannot request Ring 1 mappings",
               done && info.exit_code == 0 && !info.fault_vector);
        if (!e) {
            if (!done)
                (void)k_process_control(pid, K_CTL_KILL);
            else
                reap_after_test(pid);
        } else
            error_("Ring 1 fixture", e);
    }
}
static void cmd_procsmp(BOOLEAN smp)
{
    UINT32 a = 0, b = 0, cpu = 0;
    if (smp) {
        for (UINT32 i = 1; i < k_platform()->discovered_cpus; ++i) {
            k_cpu_info c;
            if (!k_cpu_get(i, &c) && c.online) {
                cpu = i;
                break;
            }
        }
        if (!cpu) {
            con_print("SKIP: run smpstart before procsmp.\n");
            return;
        }
    }
    int e = k_process_launch("@system/bin/tlsproc.rief", "", K_EXEC_RING3, 0, 0, &a);
    int e2 = k_process_launch("@system/bin/tlsproc.rief", "", K_EXEC_RING3, cpu, 0, &b);
    k_process_info ai, bi;
    BOOLEAN alive = !e && !e2 && !k_process_query(a, &ai) && !k_process_query(b, &bi) && a != b &&
                    ai.task_id != bi.task_id && ai.cpu == 0 && bi.cpu == cpu;
    report(smp ? "PID table starts BSP and AP user processes"
               : "PID table starts two concurrent user processes",
           alive);
    BOOLEAN ad = !e && wait_process(a, 5000, &ai), bd = !e2 && wait_process(b, 5000, &bi);
    report("per-task TLS survives repeated syscalls and context switches",
           ad && bd && !ai.exit_code && !bi.exit_code && !ai.fault_vector && !bi.fault_vector);
    if (!e) {
        if (ad)
            reap_after_test(a);
        else
            (void)k_process_control(a, K_CTL_KILL);
    }
    if (!e2) {
        if (bd)
            reap_after_test(b);
        else
            (void)k_process_control(b, K_CTL_KILL);
    }
}
static void cmd_helpertest(void)
{
    UINT32 helper = 0, client = 0, tid = 0;
    int e =
        k_process_launch("@system/bin/hash-helper.rief", "", K_EXEC_RING1, K_CPU_AUTO, 0, &helper);
    UINT64 start = k_uptime_ms();
    if (!e) {
        do {
            e = k_service_lookup("hash", &tid);
            if (!e)
                break;
            k_sleep(1);
        } while (k_uptime_ms() - start < 3000);
    }
    if (!e) {
        k_process_info h;
        e = k_process_query(helper, &h);
        if (!e && h.task_id != tid)
            e = K_EEXIST;
    }
    report("isolated Ring 1 helper registers service endpoint", !e);
    if (!e)
        e = k_process_launch("@system/bin/hash-client.rief", "", K_EXEC_RING3, K_CPU_AUTO, 0,
                             &client);
    k_process_info c;
    BOOLEAN done = !e && wait_process(client, 3000, &c);
    report("ordinary RIEF app calls helper through authenticated IPC",
           done && !c.exit_code && !c.fault_vector);
    if (client) {
        if (done)
            reap_after_test(client);
        else
            (void)k_process_control(client, K_CTL_KILL);
    }
    if (helper) {
        (void)k_process_control(helper, K_CTL_KILL);
        if (wait_process(helper, 3000, &c))
            reap_after_test(helper);
    }
}
static void cmd_overwrite(const char *args)
{
    char path[K_PATH_MAX], resolved[K_PATH_MAX];
    UINT64 offset;
    if (!path_argument(&args, path) || !number_(&args, &offset)) {
        con_print("overwrite <file> <offset> <text>\n");
        return;
    }
    while (*args == ' ')
        ++args;
    int e = k_path_resolve(path, cwd, resolved);
    k_file f;
    UINT64 written = 0;
    if (!e)
        e = k_vfs_open(resolved, &f);
    if (!e)
        e = k_vfs_write(&f, offset, args, strlen_(args), &written);
    if (e)
        error_("overwrite", e);
    else {
        con_print_uint(written);
        con_print(" bytes written and flushed\n");
    }
}

static void cmd_ps(void)
{
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i) {
        k_task_info t;
        if (k_task_get(i, &t))
            continue;
        con_print("task ");
        con_print_uint(t.id);
        con_print(" cpu=");
        con_print_uint(t.cpu);
        con_print(" state=");
        con_print_uint(t.state);
        con_print(" weight=");
        con_print_uint(t.weight);
        con_print(" ticks=");
        con_print_uint(t.runtime_ticks);
        con_print(" switches=");
        con_print_uint(t.switches);
        con_print(" ");
        con_print(t.name);
        if (t.state == K_TASK_ZOMBIE) {
            con_print(" exit=");
            con_print_int(t.exit_code);
        }
        con_print("\n");
    }
    con_print("states: 1 ready, 2 running, 3 sleeping, 4 blocked, 5 exited, 6 stopped\n");
}
static void cmd_cpus(void)
{
    const k_platform_info *p = k_platform();
    con_print("CPUs online/discovered: ");
    con_print_uint(p->online_cpus);
    con_print("/");
    con_print_uint(p->discovered_cpus);
    con_print("\nclock: ");
    con_print(p->clock_source);
    con_print("  timer: ");
    con_print(p->timer_source);
    con_print("  Hz=");
    con_print_uint(k_tick_hz());
    con_print("\n");
    con_print("NX=");
    con_print_uint(p->nx);
    con_print(" SMEP=");
    con_print_uint(p->smep);
    con_print(" APIC=");
    con_print_uint(p->apic);
    con_print(" x2APIC=");
    con_print_uint(p->x2apic);
    con_print(" PS/2=");
    con_print_uint(p->ps2);
    con_print("\nDMA: ");
    con_print(p->dma_reason);
    con_print("\n");
    for (UINT32 i = 0; i < p->discovered_cpus; ++i) {
        k_cpu_info c;
        k_cpu_get(i, &c);
        con_print("cpu ");
        con_print_uint(i);
        con_print(" APIC ID=");
        con_print_uint(c.apic_id);
        con_print(c.online ? " online timer IRQs=" : " offline timer IRQs=");
        con_print_uint(c.ticks);
        con_print(" idle=");
        con_print_uint(c.idle_ticks);
        con_print(" work ticks=");
        con_print_uint(c.ticks >= c.idle_ticks ? c.ticks - c.idle_ticks : 0);
        con_print(" switches=");
        con_print_uint(c.switches);
        con_print("\n");
    }
}
static void cmd_time(void)
{
    EFI_TIME t;
    UINT64 flags = k_irq_save();
    EFI_STATUS status = RT->GetTime(&t, NULL);
    k_irq_restore(flags);
    if (EFI_ERROR(status)) {
        con_print("GetTime failed\n");
        return;
    }
    con_print_uint(t.Year);
    con_putc('-');
    con_print_2digit(t.Month);
    con_putc('-');
    con_print_2digit(t.Day);
    con_putc(' ');
    con_print_2digit(t.Hour);
    con_putc(':');
    con_print_2digit(t.Minute);
    con_putc(':');
    con_print_2digit(t.Second);
    con_print("\n");
}
static void cmd_pmm(void)
{
    con_print("PMM managed/free/used MiB: ");
    con_print_uint(kernel_pmm_managed_mib());
    con_putc('/');
    con_print_uint(kernel_pmm_free_mib());
    con_putc('/');
    con_print_uint(kernel_pmm_used_mib());
    con_print("\n");
}
static void cmd_help(void)
{
    con_print("inputdevices / inputtest / mousetest -- USB and PS/2 input; ESC leaves mousetest\n");
    con_print("Mouse wheel scrolls console history; typing returns to the prompt.\n");
    con_print("cachetest / consoletest -- RAM cache and visual scroll diagnostics\n");
    con_print("help clear echo <text> time meminfo pmm uptime reboot coldboot poweroff\n");
    con_print("ps reap cpus smpstart roots mounts pwd cd <path> ls [path] cat <path>\n");
    con_print("pids | tasks | wait <pid> | kill/stop/cont <pid> | partitions\n");
    con_print("execpu <cpu> <path> [args] | ring1 <path> [args] | ring1cpu <cpu> <path> [args]\n");
    con_print("helper <path> [args] | service <name> [message] | ring1test | procsmp | tlstest | "
              "helpertest\n");
    con_print("exec <RIEF path>      e.g. exec @system/bin/hello.rief\n");
    con_print("disks diskread <number> <LBA> [count]  (physical media reads only)\n");
    con_print("./file executes RIEF from the current directory (extension is optional).\n");
    con_print("write <path> <text> | append <path> <text> | touch <path> | mkdir <path>\n");
    con_print("overwrite <file> <offset> <text> | cp <source> <destination> | sync\n");
    con_print("fatwritetest (RAM only)\n");
    con_print("writetest <directory> creates a new persistent probe; excluded from testall.\n");
    con_print("Live diagnostic commands:\n");
    con_print("pmmtest vmmtest astest timertest tasktest preempttest schedtest\n");
    con_print("synctest ipctest pathtest rieftest usertest isolationtest\n");
    con_print("disktest ramdisktest smptest testall\n");
    con_print("Run testall before smpstart, then smptest. Processes use selected/automatic CPU "
              "affinity.\n");
}
static void cmd_testall(void)
{
    passes = failures = 0;
    cmd_inputtest();
    cmd_ring1test();
    cmd_procsmp(FALSE);
    cmd_helpertest();
    cmd_cachetest();
    cmd_pmmtest();
    cmd_vmmtest();
    cmd_astest();
    cmd_timertest();
    cmd_tasktest();
    cpu_share_test(FALSE);
    cpu_share_test(TRUE);
    cmd_synctest();
    cmd_ipctest();
    cmd_pathtest();
    cmd_rieftest();
    cmd_usertest();
    cmd_isolationtest();
    cmd_disktest();
    cmd_ramdisktest();
    cmd_fatwritetest();
    con_print("Live results: ");
    con_print_uint(passes);
    con_print(" passed, ");
    con_print_uint(failures);
    con_print(" failed.\n");
    con_print("SMP is separate: smpstart, then smptest.\n");
}
static UINTN kb_read_line(char *buf, UINTN capacity)
{
    UINTN n = 0;
    buf[0] = 0;
    for (;;) {
        char c = os_getchar();
        if (c == '\n') {
            con_print("\n");
            buf[n] = 0;
            return n;
        }
        if (c == '\b') {
            if (n) {
                --n;
                con_putc('\b');
            }
            continue;
        }
        if (c >= 32 && c < 127 && n + 1 < capacity) {
            buf[n++] = c;
            buf[n] = 0;
            con_putc(c);
        }
    }
}

static void boot_print(const CHAR16 *s) { ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)s); }
static BOOLEAN mask_shift(UINT32 mask, UINT8 *shift)
{
    if (!mask)
        return FALSE;
    UINT8 n = 0;
    while (!(mask & 1)) {
        ++n;
        mask >>= 1;
    }
    if (mask != 255 || n > 24)
        return FALSE;
    *shift = n;
    return TRUE;
}
static BOOLEAN setup_framebuffer(void)
{
    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    if (EFI_ERROR(BS->LocateProtocol(&guid, NULL, (VOID **)&gop)) || !gop || !gop->Mode ||
        !gop->Mode->Info)
        return FALSE;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *i = gop->Mode->Info;
    UINT8 r, g, b;
    if (i->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
        r = 0;
        g = 8;
        b = 16;
    } else if (i->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
        r = 16;
        g = 8;
        b = 0;
    } else if (i->PixelFormat == PixelBitMask) {
        if (!mask_shift(i->PixelInformation.RedMask, &r) ||
            !mask_shift(i->PixelInformation.GreenMask, &g) ||
            !mask_shift(i->PixelInformation.BlueMask, &b) ||
            (i->PixelInformation.RedMask & i->PixelInformation.GreenMask) ||
            (i->PixelInformation.RedMask & i->PixelInformation.BlueMask) ||
            (i->PixelInformation.GreenMask & i->PixelInformation.BlueMask))
            return FALSE;
    } else
        return FALSE;
    if (i->HorizontalResolution < 8 || i->VerticalResolution < 8 ||
        i->HorizontalResolution > 32768 || i->VerticalResolution > 32768 ||
        i->PixelsPerScanLine > 65536 || i->PixelsPerScanLine < i->HorizontalResolution ||
        (UINT64)i->PixelsPerScanLine * i->VerticalResolution * 4 > gop->Mode->FrameBufferSize ||
        !gop->Mode->FrameBufferBase)
        return FALSE;
    kernel_fb_init((void *)(UINTN)gop->Mode->FrameBufferBase, i->HorizontalResolution,
                   i->VerticalResolution, i->PixelsPerScanLine, r, g, b);
    return TRUE;
}
static UINT64 find_rsdp(void)
{
    const EFI_GUID acpi20 = {
        0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
    const EFI_GUID acpi10 = {
        0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
    UINT64 old = 0;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; ++i) {
        EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];
        if (equal_(&t->VendorGuid, &acpi20, sizeof(acpi20)))
            return (UINT64)(UINTN)t->VendorTable;
        if (equal_(&t->VendorGuid, &acpi10, sizeof(acpi10)))
            old = (UINT64)(UINTN)t->VendorTable;
    }
    return old;
}
static BOOLEAN exit_boot_services(EFI_HANDLE image)
{
    for (UINT32 attempt = 0; attempt < 8; ++attempt) {
        UINTN size = sizeof(efi_map_buffer), key = 0, stride = 0;
        UINT32 version = 0;
        EFI_STATUS s = BS->GetMemoryMap(&size, (void *)efi_map_buffer, &key, &stride, &version);
        if (EFI_ERROR(s) || stride < sizeof(EFI_MEMORY_DESCRIPTOR) || !stride || size % stride ||
            size > sizeof(efi_map_buffer))
            return FALSE;
        s = BS->ExitBootServices(image, key);
        if (!EFI_ERROR(s)) {
            __asm__ volatile("cli" ::: "memory");
            snap_entries = size / stride;
            UINT64 total = 0, free = 0;
            for (UINTN i = 0; i < snap_entries; ++i) {
                EFI_MEMORY_DESCRIPTOR *d = (void *)(efi_map_buffer + i * stride);
                total += d->NumberOfPages;
                if (d->Type == EfiConventionalMemory)
                    free += d->NumberOfPages;
            }
            snap_total_mib = total / 256;
            snap_free_mib = free / 256;
            pmm_seed_from_efi_map((void *)efi_map_buffer, stride, snap_entries);
            BS = NULL;
            return TRUE;
        }
        if (s != EFI_INVALID_PARAMETER)
            return FALSE;
        /* Same actual capacity each retry. No AllocatePool, console protocol,
         * or other firmware calls can invalidate the next map key. */
    }
    return FALSE;
}

static const UINT8 ring1_fixture[] = {
    0x52, 0x49, 0x45, 0x46, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xd0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0xd8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x6d, 0x61, 0x69, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xec, 0x08, 0xe8, 0x09, 0x00, 0x00,
    0x00, 0x48, 0x89, 0xc7, 0x31, 0xc0, 0x0f, 0x05, 0x0f, 0x0b, 0x55, 0x48, 0x89, 0xe5, 0x48, 0x81,
    0xec, 0xb0, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x85, 0xc0, 0xff, 0xff, 0xff, 0x50, 0x48, 0x8b, 0xbc,
    0x24, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48,
    0x81, 0xc4, 0x08, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x9c, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85,
    0xc0, 0x0f, 0x84, 0x14, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xe9, 0x98, 0x06, 0x00, 0x00, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x85, 0x60, 0xff,
    0xff, 0xff, 0x50, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48,
    0x8d, 0x85, 0x58, 0xff, 0xff, 0xff, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x58, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00,
    0x50, 0x48, 0xb8, 0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x39, 0xc2,
    0x0f, 0x9c, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x55, 0x00, 0x00, 0x00, 0x48,
    0x8d, 0x85, 0x60, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x50, 0x48, 0x8d, 0x85, 0x58, 0xff, 0xff,
    0xff, 0x48, 0x8b, 0x00, 0x41, 0x5a, 0x48, 0x69, 0xc0, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x01, 0xd0,
    0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x0f, 0xb6, 0xc0,
    0x41, 0x88, 0x02, 0x48, 0x8d, 0x85, 0x58, 0xff, 0xff, 0xff, 0x50, 0x48, 0x8b, 0x00, 0x49, 0x89,
    0xc3, 0x48, 0x05, 0x01, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x4c, 0x89, 0xd8, 0xe9,
    0x82, 0xff, 0xff, 0xff, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x05, 0x00, 0x00, 0x00,
    0x00, 0x50, 0x48, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x89, 0xc0,
    0x41, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x05, 0x04, 0x00, 0x00, 0x00,
    0x50, 0x48, 0xb8, 0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x89, 0xc0, 0x41,
    0x89, 0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x05, 0x08, 0x00, 0x00, 0x00, 0x50,
    0x48, 0xb8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x89, 0xc0, 0x41, 0x89,
    0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x05, 0x10, 0x00, 0x00, 0x00, 0x50, 0x48,
    0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x48, 0x69, 0xc0, 0x08, 0x00,
    0x00, 0x00, 0x4c, 0x01, 0xd0, 0x50, 0x48, 0x8d, 0x85, 0xc0, 0xff, 0xff, 0xff, 0x48, 0x05, 0x00,
    0x00, 0x00, 0x00, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a,
    0x48, 0x69, 0xc0, 0x08, 0x00, 0x00, 0x00, 0x4c, 0x01, 0xd0, 0x48, 0x8b, 0x00, 0x41, 0x5a, 0x49,
    0x89, 0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x05, 0x10, 0x00, 0x00, 0x00, 0x50,
    0x48, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x48, 0x69, 0xc0, 0x08,
    0x00, 0x00, 0x00, 0x4c, 0x01, 0xd0, 0x50, 0x48, 0x8d, 0x85, 0xc0, 0xff, 0xff, 0xff, 0x48, 0x05,
    0x00, 0x00, 0x00, 0x00, 0x50, 0x48, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41,
    0x5a, 0x48, 0x69, 0xc0, 0x08, 0x00, 0x00, 0x00, 0x4c, 0x01, 0xd0, 0x48, 0x8b, 0x00, 0x41, 0x5a,
    0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x05, 0x20, 0x00, 0x00, 0x00,
    0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x48, 0x69, 0xc0,
    0x08, 0x00, 0x00, 0x00, 0x4c, 0x01, 0xd0, 0x50, 0x48, 0x8d, 0x85, 0xc0, 0xff, 0xff, 0xff, 0x48,
    0x05, 0x00, 0x00, 0x00, 0x00, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x41, 0x5a, 0x48, 0x69, 0xc0, 0x08, 0x00, 0x00, 0x00, 0x4c, 0x01, 0xd0, 0x48, 0x8b, 0x00, 0x41,
    0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x05, 0x20, 0x00, 0x00,
    0x00, 0x50, 0x48, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x48, 0x69,
    0xc0, 0x08, 0x00, 0x00, 0x00, 0x4c, 0x01, 0xd0, 0x50, 0x48, 0x8d, 0x85, 0xc0, 0xff, 0xff, 0xff,
    0x48, 0x05, 0x00, 0x00, 0x00, 0x00, 0x50, 0x48, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x41, 0x5a, 0x48, 0x69, 0xc0, 0x08, 0x00, 0x00, 0x00, 0x4c, 0x01, 0xd0, 0x48, 0x8b, 0x00,
    0x41, 0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x05, 0x30, 0x00,
    0x00, 0x00, 0x50, 0x48, 0x8d, 0x85, 0xc0, 0xff, 0xff, 0xff, 0x48, 0x05, 0x10, 0x00, 0x00, 0x00,
    0x8b, 0x00, 0x41, 0x5a, 0x89, 0xc0, 0x41, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff,
    0x48, 0x05, 0x38, 0x00, 0x00, 0x00, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00,
    0x00, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x05, 0x40,
    0x00, 0x00, 0x00, 0x50, 0x48, 0xb8, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a,
    0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x05, 0x48, 0x00, 0x00, 0x00,
    0x50, 0x48, 0xb8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x89, 0x02,
    0x48, 0x8d, 0x85, 0xc0, 0xff, 0xff, 0xff, 0x48, 0x05, 0x18, 0x00, 0x00, 0x00, 0x8b, 0x00, 0x50,
    0x48, 0xb8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f,
    0x94, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x68, 0x00, 0x00, 0x00, 0x48, 0x8d,
    0x85, 0x68, 0xff, 0xff, 0xff, 0x50, 0x48, 0x8b, 0xbc, 0x24, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb8,
    0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x81, 0xc4, 0x08, 0x00, 0x00, 0x00, 0x0f,
    0x05, 0x50, 0x48, 0xb8, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0xf7, 0xd8, 0x41,
    0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x94, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x0f,
    0x00, 0x00, 0x00, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe9, 0x0a, 0x00,
    0x00, 0x00, 0x48, 0xb8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe9, 0x3d, 0x03, 0x00,
    0x00, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x05, 0x10,
    0x00, 0x00, 0x00, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a,
    0x48, 0x69, 0xc0, 0x08, 0x00, 0x00, 0x00, 0x4c, 0x01, 0xd0, 0x50, 0x48, 0x8b, 0x00, 0x50, 0x48,
    0xb8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x4c, 0x31, 0xd0, 0x41, 0x5a,
    0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x50, 0x48, 0x8b, 0xbc, 0x24, 0x00,
    0x00, 0x00, 0x00, 0x48, 0xb8, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x81, 0xc4,
    0x08, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x50, 0x48, 0xb8, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x48, 0xf7, 0xd8, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48,
    0x85, 0xc0, 0x0f, 0x84, 0x14, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xe9, 0xa7, 0x02, 0x00, 0x00, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x85, 0x68,
    0xff, 0xff, 0xff, 0x48, 0x05, 0x10, 0x00, 0x00, 0x00, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x48, 0x69, 0xc0, 0x08, 0x00, 0x00, 0x00, 0x4c, 0x01, 0xd0,
    0x50, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41,
    0x5a, 0x4c, 0x31, 0xd0, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff,
    0x48, 0x05, 0x38, 0x00, 0x00, 0x00, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x50, 0x48, 0x8b,
    0xbc, 0x24, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x81, 0xc4, 0x08, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x50, 0x48, 0xb8, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x48, 0xf7, 0xd8, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x95, 0xc0, 0x0f,
    0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x14, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x04, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xe9, 0xf4, 0x01, 0x00, 0x00, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x48,
    0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x05, 0x38, 0x00, 0x00, 0x00, 0x50, 0x48, 0xb8, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x68,
    0xff, 0xff, 0xff, 0x48, 0x05, 0x48, 0x00, 0x00, 0x00, 0x50, 0x48, 0xb8, 0x07, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff,
    0x50, 0x48, 0x8b, 0xbc, 0x24, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x11, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x48, 0x81, 0xc4, 0x08, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x50, 0x48, 0xb8, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0xf7, 0xd8, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f,
    0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x14, 0x00, 0x00, 0x00, 0x48, 0xb8,
    0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe9, 0x61, 0x01, 0x00, 0x00, 0xe9, 0x00, 0x00,
    0x00, 0x00, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x05, 0x48, 0x00, 0x00, 0x00, 0x50,
    0x48, 0xb8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48,
    0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x50, 0x48, 0x8b, 0xbc, 0x24, 0x00, 0x00, 0x00, 0x00, 0x48,
    0xb8, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x81, 0xc4, 0x08, 0x00, 0x00, 0x00,
    0x0f, 0x05, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49,
    0x39, 0xc2, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x14, 0x00, 0x00,
    0x00, 0x48, 0xb8, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe9, 0xee, 0x00, 0x00, 0x00,
    0xe9, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x85, 0x50, 0xff, 0xff, 0xff, 0x50, 0x48, 0x8d, 0x85,
    0x68, 0xff, 0xff, 0xff, 0x48, 0x05, 0x38, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x00, 0x41, 0x5a, 0x49,
    0x89, 0x02, 0x48, 0x8d, 0x85, 0x50, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8, 0xcd,
    0xab, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x50,
    0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8, 0xcd, 0xab, 0x34, 0x12,
    0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48,
    0x85, 0xc0, 0x0f, 0x84, 0x14, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xe9, 0x77, 0x00, 0x00, 0x00, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x85, 0x68,
    0xff, 0xff, 0xff, 0x50, 0x48, 0x8b, 0xbc, 0x24, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x11, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x81, 0xc4, 0x08, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x50,
    0x48, 0xb8, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0xf7, 0xd8, 0x41, 0x5a, 0x49,
    0x39, 0xc2, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x14, 0x00, 0x00,
    0x00, 0x48, 0xb8, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe9, 0x1e, 0x00, 0x00, 0x00,
    0xe9, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe9,
    0x0a, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc9, 0xc3,
};
static const UINT8 tls_fixture[] = {
    0x52, 0x49, 0x45, 0x46, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xd0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0xd8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xcb, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xcb, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x6d, 0x61, 0x69, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xec, 0x08, 0xe8, 0x09, 0x00, 0x00,
    0x00, 0x48, 0x89, 0xc7, 0x31, 0xc0, 0x0f, 0x05, 0x0f, 0x0b, 0x55, 0x48, 0x89, 0xe5, 0x48, 0x81,
    0xec, 0x20, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x85, 0xf8, 0xff, 0xff, 0xff, 0x50, 0x48, 0xb8, 0x16,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d,
    0x85, 0xf8, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x41, 0x5a, 0x48, 0x69, 0xc0, 0x08, 0x00, 0x00, 0x00, 0x4c, 0x01, 0xd0, 0x48,
    0x8b, 0x00, 0x50, 0x48, 0x8d, 0x85, 0xf8, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x41, 0x5a, 0x49,
    0x39, 0xc2, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x85, 0x3c, 0x00, 0x00,
    0x00, 0x48, 0x8d, 0x85, 0xf8, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x48, 0x69, 0xc0, 0x08, 0x00, 0x00, 0x00, 0x4c,
    0x01, 0xd0, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0f, 0x05, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0,
    0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x85, 0x28, 0x00, 0x00, 0x00, 0x64,
    0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, 0x48, 0x05, 0x40, 0x00, 0x00, 0x00, 0x48, 0x8b,
    0x00, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x39,
    0xc2, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0,
    0x48, 0x85, 0xc0, 0x0f, 0x84, 0x14, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xe9, 0xb9, 0x01, 0x00, 0x00, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x85,
    0xf0, 0xff, 0xff, 0xff, 0x50, 0x48, 0xb8, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f,
    0x05, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0xe8, 0xff, 0xff, 0xff, 0x50, 0x48, 0xb8,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85,
    0xe8, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x9c, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0,
    0x0f, 0x84, 0x42, 0x01, 0x00, 0x00, 0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, 0x48,
    0x05, 0x40, 0x00, 0x00, 0x00, 0x50, 0x48, 0x8b, 0x00, 0x49, 0x89, 0xc3, 0x48, 0x05, 0x01, 0x00,
    0x00, 0x00, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x4c, 0x89, 0xd8, 0x48, 0xb8, 0x0a, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x50, 0x48, 0x8b, 0xbc, 0x24, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x81, 0xc4, 0x08, 0x00, 0x00, 0x00, 0x0f, 0x05,
    0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, 0x48, 0x05, 0x40, 0x00, 0x00, 0x00, 0x48,
    0x8b, 0x00, 0x50, 0x48, 0x8d, 0x85, 0xe8, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x4c, 0x01, 0xd0, 0x41, 0x5a, 0x49,
    0x39, 0xc2, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x85, 0x3c, 0x00, 0x00,
    0x00, 0x48, 0x8d, 0x85, 0xf8, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x48, 0x69, 0xc0, 0x08, 0x00, 0x00, 0x00, 0x4c,
    0x01, 0xd0, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0f, 0x05, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0,
    0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x85, 0x22, 0x00, 0x00, 0x00, 0x48,
    0xb8, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x50, 0x48, 0x8d, 0x85, 0xf0,
    0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x95, 0xc0, 0x0f, 0xb6,
    0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x14,
    0x00, 0x00, 0x00, 0x48, 0xb8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe9, 0x3f, 0x00,
    0x00, 0x00, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x85, 0xe8, 0xff, 0xff, 0xff, 0x50, 0x48,
    0x8b, 0x00, 0x49, 0x89, 0xc3, 0x48, 0x05, 0x01, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x89, 0x02,
    0x4c, 0x89, 0xd8, 0xe9, 0x95, 0xfe, 0xff, 0xff, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xe9, 0x0a, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xc9, 0xc3, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const UINT8 hash_helper_fixture[] = {
    0x52, 0x49, 0x45, 0x46, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x18, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x33, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x33, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x50, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x3c, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2f, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x61, 0x73, 0x68, 0x00, 0x6d, 0x61, 0x69,
    0x6e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xec, 0x08, 0xe8, 0x26, 0x01, 0x00,
    0x00, 0x48, 0x89, 0xc7, 0x31, 0xc0, 0x0f, 0x05, 0x0f, 0x0b, 0x55, 0x48, 0x89, 0xe5, 0x48, 0x81,
    0xec, 0x20, 0x00, 0x00, 0x00, 0x48, 0x89, 0xf8, 0x50, 0x48, 0x8d, 0x85, 0xf8, 0xff, 0xff, 0xff,
    0x49, 0x89, 0xc2, 0x58, 0x49, 0x89, 0x02, 0x48, 0x89, 0xf0, 0x50, 0x48, 0x8d, 0x85, 0xf0, 0xff,
    0xff, 0xff, 0x49, 0x89, 0xc2, 0x58, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0xe8, 0xff, 0xff, 0xff,
    0x50, 0x48, 0xb8, 0x25, 0x23, 0x22, 0x84, 0xe4, 0x9c, 0xf2, 0xcb, 0x41, 0x5a, 0x49, 0x89, 0x02,
    0x48, 0x8d, 0x85, 0xe0, 0xff, 0xff, 0xff, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0xe0, 0xff, 0xff, 0xff, 0x48, 0x8b,
    0x00, 0x50, 0x48, 0x8d, 0x85, 0xf0, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x41, 0x5a, 0x49, 0x39,
    0xc2, 0x0f, 0x92, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x7c, 0x00, 0x00, 0x00,
    0x48, 0x8d, 0x85, 0xe8, 0xff, 0xff, 0xff, 0x50, 0x48, 0x8b, 0x00, 0x50, 0x48, 0x8d, 0x85, 0xf8,
    0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x50, 0x48, 0x8d, 0x85, 0xe0, 0xff, 0xff, 0xff, 0x48, 0x8b,
    0x00, 0x41, 0x5a, 0x48, 0x69, 0xc0, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x01, 0xd0, 0x0f, 0xb6, 0x00,
    0x41, 0x5a, 0x4c, 0x31, 0xd0, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0xe8, 0xff, 0xff,
    0xff, 0x50, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8, 0xb3, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x41, 0x5a, 0x49, 0x0f, 0xaf, 0xc2, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0xe0, 0xff,
    0xff, 0xff, 0x50, 0x48, 0x8b, 0x00, 0x49, 0x89, 0xc3, 0x48, 0x05, 0x01, 0x00, 0x00, 0x00, 0x41,
    0x5a, 0x49, 0x89, 0x02, 0x4c, 0x89, 0xd8, 0xe9, 0x5b, 0xff, 0xff, 0xff, 0x48, 0x8d, 0x85, 0xe8,
    0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0xe9, 0x0a, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xc9, 0xc3, 0x55, 0x48, 0x89, 0xe5, 0x48, 0x81, 0xec, 0xa0, 0x00,
    0x00, 0x00, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x48, 0x8b, 0xbc,
    0x24, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48,
    0x81, 0xc4, 0x08, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x9c, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85,
    0xc0, 0x0f, 0x84, 0x14, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xe9, 0xa3, 0x01, 0x00, 0x00, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x81, 0x01, 0x00, 0x00, 0x48, 0x8d,
    0x85, 0x68, 0xff, 0xff, 0xff, 0x50, 0x48, 0x8d, 0x85, 0x78, 0xff, 0xff, 0xff, 0x50, 0x48, 0xb8,
    0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x48, 0x8b, 0xbc, 0x24, 0x08, 0x00, 0x00,
    0x00, 0x48, 0x8b, 0xb4, 0x24, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x07, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x48, 0x81, 0xc4, 0x10, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x41, 0x5a, 0x49, 0x89,
    0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8, 0x0a, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0xf7, 0xd8, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x94,
    0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x30, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x48, 0x8b, 0xbc, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x48, 0xb8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x81, 0xc4, 0x08, 0x00, 0x00,
    0x00, 0x0f, 0x05, 0xe9, 0xe2, 0x00, 0x00, 0x00, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x85,
    0x68, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x9c, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0,
    0x0f, 0x84, 0x14, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xe9, 0xb4, 0x00, 0x00, 0x00, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x85, 0x70, 0xff, 0xff,
    0xff, 0x50, 0x48, 0x8d, 0x85, 0x78, 0xff, 0xff, 0xff, 0x48, 0x05, 0x08, 0x00, 0x00, 0x00, 0x50,
    0x48, 0x8d, 0x85, 0x78, 0xff, 0xff, 0xff, 0x48, 0x05, 0x04, 0x00, 0x00, 0x00, 0x8b, 0x00, 0x50,
    0x48, 0x8b, 0xbc, 0x24, 0x08, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xb4, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x81, 0xc4, 0x10, 0x00, 0x00, 0x00, 0x48, 0x83, 0xec, 0x08, 0xe8, 0x4a, 0xfd, 0xff, 0xff,
    0x48, 0x81, 0xc4, 0x08, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x78,
    0xff, 0xff, 0xff, 0x48, 0x05, 0x00, 0x00, 0x00, 0x00, 0x8b, 0x00, 0x50, 0x48, 0x8d, 0x85, 0x70,
    0xff, 0xff, 0xff, 0x50, 0x48, 0xb8, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x48,
    0x8b, 0xbc, 0x24, 0x10, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xb4, 0x24, 0x08, 0x00, 0x00, 0x00, 0x48,
    0x8b, 0x94, 0x24, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x48, 0x81, 0xc4, 0x18, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xe9, 0x6c, 0xfe, 0xff, 0xff, 0x48,
    0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc9, 0xc3, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x68, 0x61, 0x73, 0x68, 0x00,
};
static const UINT8 hash_client_fixture[] = {
    0x52, 0x49, 0x45, 0x46, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x50, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0f, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x60, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xae, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6d, 0x61, 0x69, 0x6e, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x83, 0xec, 0x08, 0xe8, 0x09, 0x00, 0x00, 0x00, 0x48, 0x89, 0xc7, 0x31, 0xc0, 0x0f, 0x05,
    0x0f, 0x0b, 0x55, 0x48, 0x89, 0xe5, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x85,
    0x70, 0xff, 0xff, 0xff, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50,
    0x48, 0x8b, 0xbc, 0x24, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x48, 0x81, 0xc4, 0x08, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x41, 0x5a, 0x49, 0x89, 0x02,
    0x48, 0x8d, 0x85, 0x70, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x9c, 0xc0, 0x0f, 0xb6, 0xc0,
    0x48, 0x85, 0xc0, 0x0f, 0x84, 0x14, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xe9, 0x85, 0x02, 0x00, 0x00, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x85,
    0x70, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x50, 0x48, 0xb8, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x48, 0x8b,
    0xbc, 0x24, 0x10, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xb4, 0x24, 0x08, 0x00, 0x00, 0x00, 0x48, 0x8b,
    0x94, 0x24, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x81, 0xc4, 0x18, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x9c, 0xc0, 0x0f, 0xb6, 0xc0, 0x48,
    0x85, 0xc0, 0x0f, 0x84, 0x14, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xe9, 0x06, 0x02, 0x00, 0x00, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x85, 0x68,
    0xff, 0xff, 0xff, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a,
    0x49, 0x89, 0x02, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x50, 0x48, 0xb8,
    0xe8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x9c, 0xc0,
    0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0xa8, 0x01, 0x00, 0x00, 0x48, 0x8d, 0x85, 0x78,
    0xff, 0xff, 0xff, 0x50, 0x48, 0xb8, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x48,
    0x8b, 0xbc, 0x24, 0x08, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xb4, 0x24, 0x00, 0x00, 0x00, 0x00, 0x48,
    0xb8, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x81, 0xc4, 0x10, 0x00, 0x00, 0x00,
    0x0f, 0x05, 0x50, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49,
    0x39, 0xc2, 0x0f, 0x94, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x0c, 0x01, 0x00,
    0x00, 0x48, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x48, 0xb8, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x48, 0xb8, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x50, 0x48, 0x8b, 0xbc, 0x24, 0x10, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xb4, 0x24, 0x08, 0x00,
    0x00, 0x00, 0x48, 0x8b, 0x94, 0x24, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x48, 0x81, 0xc4, 0x18, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x8d, 0x85,
    0x78, 0xff, 0xff, 0xff, 0x48, 0x05, 0x00, 0x00, 0x00, 0x00, 0x8b, 0x00, 0x50, 0x48, 0x8d, 0x85,
    0x70, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x00, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x94, 0xc0, 0x0f,
    0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x25, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x85, 0x78, 0xff,
    0xff, 0xff, 0x48, 0x05, 0x04, 0x00, 0x00, 0x00, 0x8b, 0x00, 0x50, 0x48, 0xb8, 0x08, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x39, 0xc2, 0x0f, 0x94, 0xc0, 0x0f, 0xb6, 0xc0,
    0x48, 0x85, 0xc0, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x26, 0x00,
    0x00, 0x00, 0x48, 0x8d, 0x85, 0x78, 0xff, 0xff, 0xff, 0x48, 0x05, 0x08, 0x00, 0x00, 0x00, 0x48,
    0x8b, 0x00, 0x50, 0x48, 0xb8, 0x0b, 0xbd, 0xaa, 0x80, 0x46, 0xd8, 0x30, 0xa4, 0x41, 0x5a, 0x49,
    0x39, 0xc2, 0x0f, 0x94, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x95, 0xc0, 0x0f, 0xb6,
    0xc0, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x0f, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xe9, 0x0a, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xe9, 0x65, 0x00, 0x00, 0x00, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x48, 0x8b, 0xbc, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x48, 0xb8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x81, 0xc4, 0x08, 0x00, 0x00,
    0x00, 0x0f, 0x05, 0x48, 0x8d, 0x85, 0x68, 0xff, 0xff, 0xff, 0x50, 0x48, 0x8b, 0x00, 0x49, 0x89,
    0xc3, 0x48, 0x05, 0x01, 0x00, 0x00, 0x00, 0x41, 0x5a, 0x49, 0x89, 0x02, 0x4c, 0x89, 0xd8, 0xe9,
    0x2f, 0xfe, 0xff, 0xff, 0x48, 0xb8, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe9, 0x0a,
    0x00, 0x00, 0x00, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc9, 0xc3, 0x00,
    0x68, 0x61, 0x73, 0x68, 0x00, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x00, 0x68, 0x65, 0x6c, 0x70, 0x65,
    0x72, 0x20, 0x72, 0x65, 0x70, 0x6c, 0x69, 0x65, 0x64, 0x0a, 0x00,
};

static void namespace_setup(void)
{
    const char *dirs[] = {"/system",        "/system/bin", "/users",
                          "/users/pavlkon", "/devices",    "/volumes"};
    int e = k_vfs_init();
    if (e)
        kernel_halt("VFS initialization failed");
    for (UINTN i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i)
        if (k_vfs_mkdir(dirs[i]))
            kernel_halt("namespace allocation failed");
    if (k_root_bind("system", "/system") || k_root_bind("user", "/users/pavlkon"))
        kernel_halt("named-root allocation failed");
    UINT8 bytes[1024];
    UINT32 n = make_rief(bytes, hello_code, sizeof(hello_code), hello_data, sizeof(hello_data) - 1,
                         1, 12, RIEF_RELOC_ABS64);
    if (k_vfs_put("/system/bin/hello.rief", bytes, n))
        kernel_halt("built-in application allocation failed");
    if (k_vfs_put("/system/bin/ring1check.rief", ring1_fixture, sizeof(ring1_fixture)) ||
        k_vfs_put("/system/bin/tlsproc.rief", tls_fixture, sizeof(tls_fixture)) ||
        k_vfs_put("/system/bin/hash-helper.rief", hash_helper_fixture,
                  sizeof(hash_helper_fixture)) ||
        k_vfs_put("/system/bin/hash-client.rief", hash_client_fixture, sizeof(hash_client_fixture)))
        kernel_halt("RIEF fixture allocation failed");
    const char *readme =
        "mini-os: native FAT16/32 file writes; raw physical writes blocked.\nUse help, "
        "testall, smpstart, then smptest.\n";
    k_vfs_put("/system/readme.txt", readme, strlen_(readme));
}
static void os_shell_task(void *unused)
{
    (void)unused;
    int e;
    char line[CMD_BUF_SIZE];
    for (;;) {
        con_print("mini-os> ");
        if (!kb_read_line(line, sizeof(line)))
            continue;
        if (streq_(line, "help"))
            cmd_help();
        else if (streq_(line, "clear"))
            kernel_console_clear();
        else if (starts_with_(line, "echo ")) {
            con_print(line + 5);
            con_print("\n");
        } else if (streq_(line, "echo"))
            con_print("\n");
        else if (streq_(line, "time"))
            cmd_time();
        else if (streq_(line, "meminfo")) {
            con_print("EFI snapshot entries=");
            con_print_uint(snap_entries);
            con_print(" described MiB=");
            con_print_uint(snap_total_mib);
            con_print(" conventional MiB=");
            con_print_uint(snap_free_mib);
            con_print("\n");
        } else if (streq_(line, "pmm"))
            cmd_pmm();
        else if (streq_(line, "uptime")) {
            con_print_uint(k_uptime_ms());
            con_print(" ms\n");
        } else if (streq_(line, "pmmtest"))
            cmd_pmmtest();
        else if (streq_(line, "vmmtest"))
            cmd_vmmtest();
        else if (streq_(line, "astest"))
            cmd_astest();
        else if (streq_(line, "timertest"))
            cmd_timertest();
        else if (streq_(line, "tasktest"))
            cmd_tasktest();
        else if (streq_(line, "preempttest"))
            cpu_share_test(FALSE);
        else if (streq_(line, "schedtest"))
            cpu_share_test(TRUE);
        else if (streq_(line, "synctest"))
            cmd_synctest();
        else if (streq_(line, "ipctest"))
            cmd_ipctest();
        else if (streq_(line, "pathtest"))
            cmd_pathtest();
        else if (streq_(line, "rieftest"))
            cmd_rieftest();
        else if (streq_(line, "usertest"))
            cmd_usertest();
        else if (streq_(line, "isolationtest"))
            cmd_isolationtest();
        else if (streq_(line, "disktest"))
            cmd_disktest();
        else if (streq_(line, "ramdisktest"))
            cmd_ramdisktest();
        else if (streq_(line, "testall"))
            cmd_testall();
        else if (streq_(line, "cachetest"))
            cmd_cachetest();
        else if (streq_(line, "consoletest"))
            cmd_consoletest();
        else if (streq_(line, "inputdevices"))
            cmd_inputdevices();
        else if (streq_(line, "inputtest"))
            cmd_inputtest();
        else if (streq_(line, "mousetest"))
            cmd_mousetest();
        else if (streq_(line, "smptest"))
            cmd_smptest();
        else if (streq_(line, "smpstart")) {
            e = k_smp_start();
            if (e)
                error_("SMP startup", e);
            cmd_cpus();
        } else if (streq_(line, "cpus"))
            cmd_cpus();
        else if (streq_(line, "ring1test"))
            cmd_ring1test();
        else if (streq_(line, "tlstest"))
            cmd_procsmp(FALSE);
        else if (streq_(line, "procsmp"))
            cmd_procsmp(TRUE);
        else if (streq_(line, "helpertest"))
            cmd_helpertest();
        else if (starts_with_(line, "overwrite "))
            cmd_overwrite(line + 10);
        else if (streq_(line, "pids"))
            cmd_pids();
        else if (streq_(line, "tasks"))
            cmd_ps();
        else if (streq_(line, "partitions"))
            cmd_partitions();
        else if (starts_with_(line, "ring1cpu "))
            cmd_execpu(line + 9, K_EXEC_RING1);
        else if (starts_with_(line, "execpu "))
            cmd_execpu(line + 7, K_EXEC_RING3);
        else if (starts_with_(line, "ring1 "))
            cmd_launch(line + 6, K_EXEC_RING1, K_CPU_AUTO);
        else if (starts_with_(line, "helper "))
            cmd_launch(line + 7, K_EXEC_RING1, K_CPU_AUTO);
        else if (starts_with_(line, "service "))
            cmd_service(line + 8);
        else if (starts_with_(line, "wait "))
            cmd_wait(line + 5);
        else if (starts_with_(line, "kill "))
            cmd_control(line + 5, K_CTL_KILL);
        else if (starts_with_(line, "stop "))
            cmd_control(line + 5, K_CTL_STOP);
        else if (starts_with_(line, "cont "))
            cmd_control(line + 5, K_CTL_CONTINUE);
        else if (streq_(line, "ps"))
            cmd_ps();
        else if (streq_(line, "reap"))
            cmd_reap();
        else if (streq_(line, "disks"))
            cmd_disks();
        else if (starts_with_(line, "diskread "))
            cmd_diskread(line + 9);
        else if (streq_(line, "roots"))
            k_roots_list(mapping_entry, NULL);
        else if (streq_(line, "mounts"))
            k_mounts_list(mapping_entry, NULL);
        else if (streq_(line, "pwd")) {
            con_print(cwd);
            con_print("\n");
        } else if (streq_(line, "ls"))
            cmd_ls(cwd);
        else if (starts_with_(line, "ls "))
            cmd_ls(line + 3);
        else if (starts_with_(line, "cat "))
            cmd_cat(line + 4);
        else if (starts_with_(line, "exec "))
            cmd_exec(line + 5);
        else if (starts_with_(line, "write "))
            cmd_write(line + 6, FALSE);
        else if (starts_with_(line, "append "))
            cmd_write(line + 7, TRUE);
        else if (starts_with_(line, "touch "))
            cmd_touch(line + 6);
        else if (starts_with_(line, "mkdir "))
            cmd_mkdir(line + 6);
        else if (starts_with_(line, "cp "))
            cmd_cp(line + 3);
        else if (streq_(line, "sync")) {
            e = k_vfs_sync();
            if (e)
                error_("sync", e);
        } else if (starts_with_(line, "writetest "))
            cmd_writetest(line + 10);
        else if (streq_(line, "fatwritetest"))
            cmd_fatwritetest();
        else if (starts_with_(line, "cd ")) {
            char path[K_PATH_MAX];
            k_file f;
            e = k_path_resolve(line + 3, cwd, path);
            if (!e)
                e = k_vfs_open(path, &f);
            if (!e && !(f.attributes & 0x10))
                e = K_EINVAL;
            if (e)
                error_("cd", e);
            else
                textcopy_(cwd, path, sizeof(cwd));
        } else if (streq_(line, "reboot") || streq_(line, "coldboot") || streq_(line, "poweroff")) {
            EFI_RESET_TYPE kind = streq_(line, "poweroff")   ? EfiResetShutdown
                                  : streq_(line, "coldboot") ? EfiResetCold
                                                             : EfiResetWarm;
            e = k_vfs_sync();
            if (e) {
                error_("sync before shutdown", e);
                continue;
            }
            k_irq_save();
            RT->ResetSystem(kind, EFI_SUCCESS, 0, NULL);
            kernel_halt("firmware ResetSystem returned");
        } else if (starts_with_(line, "./") || starts_with_(line, "../") || line[0] == '/' ||
                   line[0] == '@') {
            cmd_exec(line);
        } else {
            con_print("Unknown command. Type help.\n");
        }
    }
}
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table)
{
    ST = system_table;
    BS = ST->BootServices;
    RT = ST->RuntimeServices;
    BS->SetWatchdogTimer(0, 0, 0, NULL);
    ST->ConOut->ClearScreen(ST->ConOut);
    boot_print(L"mini-os: preparing standalone kernel...\r\n");
    if (!setup_framebuffer()) {
        boot_print(L"A linear 32-bit GOP framebuffer is required.\r\n");
        return EFI_UNSUPPORTED;
    }
    UINT64 rsdp = find_rsdp();
    EFI_PHYSICAL_ADDRESS trampoline = 0x9ffff;
    if (EFI_ERROR(BS->AllocatePages(AllocateMaxAddress, EfiLoaderCode, 1, &trampoline)))
        trampoline = 0;
    if (!exit_boot_services(image))
        kernel_halt("ExitBootServices failed; no firmware console calls attempted");
    kernel_console_clear();
    int e = kernel_vmm_init();
    if (e) {
        error_("VMM", e);
        kernel_halt("cannot install kernel page tables");
    }
    con_print("mini-os :: standalone x86-64 kernel\n");
    if (!kernel_console_buffered())
        con_print("Console text buffer unavailable: early-boot clear-on-wrap fallback.\n");
    k_platform_init(rsdp, trampoline);
    kernel_interrupts_init();
    if (k_scheduler_init())
        kernel_halt("scheduler initialization failed");
    e = k_timer_init(100);
    if (e) {
        error_("timer", e);
        kernel_halt("no supported timer; refusing an unpreemptible shell");
    }
    kernel_interrupts_enable();
    UINT64 first_tick = k_ticks();
    k_delay_us(100000);
    if (k_ticks() == first_tick)
        kernel_halt("timer configured but no IRQ arrived");
    con_print("GDT/TSS/IDT, scheduler and timer initialized.\n");
    e = k_storage_init();
    if (e)
        error_("storage", e);
    namespace_setup();
    e = k_vfs_mount_disks();
    if (e)
        error_("volume discovery", e);
    kernel_usb_init();
    os_input_init();
    con_print("\nFAT16/32, ext4 and NTFS file writes; bounded layouts. RAM is volatile.\n");
    con_print("Type help. Suggested first command: testall. SMP starts only with smpstart.\n");
    UINT32 shell_id;
    e = k_task_create("shell", os_shell_task, NULL, 1024, 0, &shell_id);
    if (e)
        kernel_halt("cannot allocate shell task");
    for (;;)
        k_sleep(86400000);
}
