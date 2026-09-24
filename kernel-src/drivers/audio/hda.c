// SPDX-License-Identifier: GPL-2.0-only
#include "hda_hdmi.h"

#define HDA_MAX 8u
#define PATH_MAX 16u
typedef struct {
    UINT32 caps, pin, config, pcm, formats, in_amp, out_amp;
    UINT8 connections[32], count;
} widget;
typedef struct {
    UINT8 path[PATH_MAX], selection[PATH_MAX], depth;
    UINT32 pin, converter, stream;
    BOOLEAN valid, digital;
} hda_route;
typedef struct {
    hda_route routes[2];
    UINT32 direction, first, count;
    k_audio_info info;
    UINT16 bdf;
    UINT8 codec, fg, iss, oss, path[PATH_MAX], selection[PATH_MAX], depth;
    UINT32 stream, tag, wp, rp;
    volatile UINT8 *regs;
    UINT32 *corb;
    UINT64 *rirb;
    UINT8 *pcm, *bdl;
    widget *widgets;
    BOOLEAN dead;
} hda_device;
static hda_device devices[HDA_MAX];
static UINT32 device_count;
static BOOLEAN initialized;
static k_mutex audio_lock = K_MUTEX_INIT;
static UINT8 get8(hda_device *d, UINT32 r) { return *(volatile UINT8 *)(d->regs + r); }
static UINT16 get16(hda_device *d, UINT32 r) { return *(volatile UINT16 *)(d->regs + r); }
static UINT32 get32(hda_device *d, UINT32 r) { return *(volatile UINT32 *)(d->regs + r); }
static void put8(hda_device *d, UINT32 r, UINT8 v) { *(volatile UINT8 *)(d->regs + r) = v; }
static void put16(hda_device *d, UINT32 r, UINT16 v) { *(volatile UINT16 *)(d->regs + r) = v; }
static void put32(hda_device *d, UINT32 r, UINT32 v) { *(volatile UINT32 *)(d->regs + r) = v; }
static int wait32(hda_device *d, UINT32 r, UINT32 mask, UINT32 value)
{
    for (UINT32 n = 0; n < 10000; ++n) {
        if ((get32(d, r) & mask) == value)
            return 0;
        k_delay_us(10);
    }
    return K_ETIMEDOUT;
}
static void fail(hda_device *d, int e)
{
    d->dead = TRUE;
    d->info.error = e;
    if (d->regs) {
        put32(d, 0x20, 0);
        put8(d, 0x4c, 0);
        put8(d, 0x5c, 0);
    }
    pci_command(d->bdf, 0, 4); /* DMA pages stay pinned after a failed controller. */
}
static int command(hda_device *d, UINT32 node, UINT32 payload, UINT32 *response)
{
    if (d->dead || node > 255)
        return K_EIO;
    d->wp = (d->wp + 1) & 255;
    d->corb[d->wp] = ((UINT32)d->codec << 28) | (node << 20) | payload;
    arch_write_fence();
    put16(d, 0x48, (UINT16)d->wp);
    for (UINT32 n = 0; n < 5000; ++n) {
        UINT32 wp = get16(d, 0x58) & 255;
        while (d->rp != wp) {
            d->rp = (d->rp + 1) & 255;
            arch_read_fence();
            UINT64 value = d->rirb[d->rp];
            UINT32 high = (UINT32)(value >> 32);
            if (!(high & 16) && (high & 15) == d->codec) {
                if (response)
                    *response = (UINT32)value;
                return 0;
            }
        }
        if ((get8(d, 0x5d) & 4) || (get8(d, 0x4d) & 1))
            break;
        k_delay_us(10);
    }
    fail(d, K_ETIMEDOUT);
    return K_ETIMEDOUT;
}
static int digital_verb(void *context, UINT32 node, UINT32 payload, UINT32 *response)
{
    return command(context, node, payload, response);
}
static UINT32 parameter(hda_device *d, UINT32 node, UINT32 p)
{
    UINT32 result = 0;
    (void)command(d, node, 0xf0000 | p, &result);
    return result;
}
static int read_widgets(hda_device *d, UINT32 first, UINT32 count)
{
    UINT32 pcm = parameter(d, d->fg, 0xa), formats = parameter(d, d->fg, 0xb);
    UINT32 in_amp = parameter(d, d->fg, 0xd), out_amp = parameter(d, d->fg, 0x12);
    for (UINT32 id = first; id < first + count; ++id) {
        widget *w = &d->widgets[id];
        w->caps = parameter(d, id, 9);
        w->pcm = (w->caps & 16) ? parameter(d, id, 0xa) : pcm;
        w->formats = (w->caps & 16) ? parameter(d, id, 0xb) : formats;
        w->in_amp = (w->caps & 8) ? parameter(d, id, 0xd) : in_amp;
        w->out_amp = (w->caps & 8) ? parameter(d, id, 0x12) : out_amp;
        if (((w->caps >> 20) & 15) == 4) {
            w->pin = parameter(d, id, 0xc);
            (void)command(d, id, 0xf1c00, &w->config);
        }
        if (!(w->caps & 256))
            continue;
        UINT32 list = parameter(d, id, 0xe), entries = list & 127, long_form = list & 128;
        UINT32 per = long_form ? 2 : 4, bits = long_form ? 16 : 8, mask = long_form ? 0x7fff : 127;
        UINT32 previous = 0;
        for (UINT32 entry = 0; entry < entries;) {
            UINT32 packed = 0;
            if (command(d, id, 0xf0200 | entry, &packed))
                return K_EIO;
            for (UINT32 j = 0; j < per && entry < entries; ++j, ++entry) {
                UINT32 value = (packed >> (j * bits)) & ((1u << bits) - 1), end = value & mask;
                UINT32 start = (value & ~mask) ? previous + 1 : end;
                if (!end || end >= 256 || start > end || ((value & ~mask) && !previous))
                    return K_ENOTSUP;
                for (UINT32 node = start; node <= end; ++node) {
                    if (w->count == 32)
                        return K_ENOTSUP;
                    w->connections[w->count++] = (UINT8)node;
                }
                previous = end;
            }
        }
    }
    return d->dead ? K_EIO : 0;
}
static BOOLEAN route(hda_device *d, UINT32 node, UINT32 depth, BOOLEAN capture, BOOLEAN digital)
{
    if (!node || node >= 256 || depth == PATH_MAX)
        return FALSE;
    for (UINT32 i = 0; i < depth; ++i)
        if (d->path[i] == node)
            return FALSE;
    widget *w = &d->widgets[node];
    UINT32 type = (w->caps >> 20) & 15;
    if (capture && (w->caps & 512))
        return FALSE;
    if (type == 0 && !!(w->caps & 512) != digital)
        return FALSE;
    d->path[depth] = (UINT8)node;
    if ((!capture && type == 0 && (w->caps & 1) && (w->pcm & (1u << 6)) && (w->pcm & (1u << 17)) &&
         (w->formats & 1)) ||
        (capture && type == 4 && (w->pin & 32) && (w->config >> 30) != 1)) {
        d->depth = (UINT8)(depth + 1);
        return TRUE;
    }
    if (type != 1 && type != 2 && type != 3 && type != 4)
        return FALSE;
    for (UINT32 i = 0; i < w->count && i < 16; ++i) {
        d->selection[depth] = (UINT8)i;
        if (route(d, w->connections[i], depth + 1, capture, digital))
            return TRUE;
    }
    return FALSE;
}
static int select_route(hda_device *d, UINT32 first, UINT32 count)
{
    if (d->oss)
        for (UINT32 pass = 0; pass < 3; ++pass)
            for (UINT32 n = first; n < first + count; ++n) {
                widget *w = &d->widgets[n];
                UINT32 dev = (w->config >> 20) & 15;
                if (((w->caps >> 20) & 15) != 4 || !(w->pin & 16) || (w->config >> 30) == 1 ||
                    dev != pass)
                    continue;
                if (route(d, n, 0, FALSE, FALSE)) {
                    d->info.capabilities = K_AUDIO_PLAYBACK;
                    d->info.pin = n;
                    d->info.converter = d->path[d->depth - 1];
                    d->stream = 0x80 + d->iss * 32;
                    d->tag = 1;
                    return 0;
                }
            }
    if (d->oss)
        for (UINT32 pass = 0; pass < 2; ++pass)
            for (UINT32 n = first; n < first + count; ++n) {
                widget *w = &d->widgets[n];
                if (((w->caps >> 20) & 15) != 4 || !(w->pin & 16) ||
                    !(w->pin & ((1u << 7) | (1u << 24))))
                    continue;
                UINT32 sense = 0;
                if (!pass && (command(d, n, 0xf0900, &sense) || !(sense & 0x80000000u)))
                    continue;
                if (route(d, n, 0, FALSE, TRUE)) {
                    d->info.capabilities = K_AUDIO_PLAYBACK | K_AUDIO_DIGITAL;
                    d->info.pin = n;
                    d->info.converter = d->path[d->depth - 1];
                    d->stream = 0x80 + d->iss * 32;
                    d->tag = 1;
                    return 0;
                }
            }
    if (d->iss)
        for (UINT32 n = first; n < first + count; ++n) {
            widget *w = &d->widgets[n];
            if (((w->caps >> 20) & 15) != 1 || !(w->caps & 1) || !(w->pcm & (1u << 6)) ||
                !(w->pcm & (1u << 17)) || !(w->formats & 1))
                continue;
            if (route(d, n, 0, TRUE, FALSE)) {
                d->info.capabilities = K_AUDIO_CAPTURE;
                d->info.converter = n;
                d->info.pin = d->path[d->depth - 1];
                d->stream = 0x80;
                d->tag = 1;
                return 0;
            }
        }
    return K_ENOTSUP;
}
static int activate_route(hda_device *d)
{
    int e = command(d, d->fg, 0x70500, NULL);
    if (e)
        return e;
    for (UINT32 i = 0; i < d->depth; ++i) {
        UINT32 n = d->path[i];
        widget *w = &d->widgets[n];
        UINT32 type = (w->caps >> 20) & 15;
        if (w->caps & 1024) {
            e = command(d, n, 0x70500, NULL);
            if (e)
                return e;
        }
        if (i + 1 < d->depth && w->count > 1 && type != 2) {
            e = command(d, n, 0x70100 | d->selection[i], NULL);
            if (e)
                return e;
        }
        if (w->caps & 4) {
            UINT32 gain = MIN(w->out_amp & 127, (w->out_amp >> 8) & 127);
            e = command(d, n, 0x3b000 | gain, NULL);
            if (e)
                return e;
        }
        if (w->caps & 2)
            for (UINT32 input = 0; input < (w->count ? w->count : 1) && input < 16; ++input) {
                UINT32 gain = MIN(w->in_amp & 127, (w->in_amp >> 8) & 127);
                if (w->count > 1 && input != d->selection[i])
                    gain |= 128;
                e = command(d, n, 0x37000 | (input << 8) | gain, NULL);
                if (e)
                    return e;
            }
    }
    widget *pin = &d->widgets[d->info.pin];
    UINT32 control = d->direction == K_AUDIO_PLAYBACK ? 0x40 : 0x20;
    if (control == 0x40 && (pin->pin & 8))
        control |= 0x80;
    if (control == 0x20 && (pin->pin & (1u << 12)))
        control |= 4;
    e = command(d, d->info.pin, 0x70700 | control, NULL);
    if (e)
        return e;
    if (pin->pin & (1u << 16)) {
        e = command(d, d->info.pin, 0x70c02, NULL);
        if (e)
            return e;
    }
    if (d->direction == K_AUDIO_PLAYBACK && d->routes[0].digital) {
        e = hda_hdmi_prepare(d, digital_verb, d->info.pin, d->info.converter, d->info.vendor);
        if (e)
            return e;
    }
    e = command(d, d->info.converter, 0x20011, NULL);
    if (e)
        return e;
    return command(d, d->info.converter, 0x70600 | (d->tag << 4), NULL);
}
static int start(hda_device *d)
{
    UINT64 base = pci_mbar(d->bdf, 0x10);
    if (!base || k_mmio_map(base, 0x4000))
        return K_ENOTSUP;
    d->regs = (void *)(UINTN)base;
    pci_command(d->bdf, 0x402, 4);
    put32(d, 0x20, 0);
    put8(d, 0x4c, 0);
    put8(d, 0x5c, 0);
    put32(d, 8, get32(d, 8) & ~1u);
    int e = wait32(d, 8, 1, 0);
    if (e)
        return e;
    k_delay_us(100);
    put32(d, 8, get32(d, 8) | 1);
    e = wait32(d, 8, 1, 1);
    if (e)
        return e;
    k_delay_us(1000);
    UINT16 caps = get16(d, 0);
    d->iss = (caps >> 8) & 15;
    d->oss = (caps >> 12) & 15;
    if (!(get8(d, 0x4e) & 64) || !(get8(d, 0x5e) & 64))
        return K_ENOTSUP;
    d->corb = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
    d->rirb = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
    d->pcm = (void *)(UINTN)k_pmm_alloc_pages(16, 0x100000000ULL);
    d->bdl = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
    d->widgets = k_heap_alloc(sizeof(widget) * 256, 16);
    if (!d->corb || !d->rirb || !d->pcm || !d->bdl || !d->widgets)
        return K_ENOMEM;
    put32(d, 0x40, (UINT32)(UINTN)d->corb);
    put32(d, 0x44, 0);
    put8(d, 0x4e, 2);
    put16(d, 0x48, 0);
    put16(d, 0x4a, 0x8000);
    k_delay_us(10);
    put16(d, 0x4a, 0);
    if (get16(d, 0x4a) & 0x8000)
        return K_EIO;
    put32(d, 0x50, (UINT32)(UINTN)d->rirb);
    put32(d, 0x54, 0);
    put8(d, 0x5e, 2);
    put16(d, 0x58, 0x8000);
    put16(d, 0x5a, 1);
    put8(d, 0x4d, 1);
    put8(d, 0x5d, 5);
    pci_command(d->bdf, 6, 0);
    if ((pci_get(d->bdf, 4) & 6) != 6)
        return K_EIO;
    put8(d, 0x5c, 2);
    put8(d, 0x4c, 2);
    UINT32 codecs = get16(d, 0xe) & 0x7fff;
    for (UINT32 codec = 0; codec < 15; ++codec)
        if (codecs & (1u << codec)) {
            d->codec = (UINT8)codec;
            UINT32 root = parameter(d, 0, 4), first = (root >> 16) & 255, count = root & 255;
            if (!count || count > 256 - first)
                continue;
            for (UINT32 fg = first; fg < first + count; ++fg)
                if ((parameter(d, fg, 5) & 255) == 1) {
                    d->fg = (UINT8)fg;
                    UINT32 sub = parameter(d, fg, 4), begin = (sub >> 16) & 255, n = sub & 255;
                    if (!n || n > 256 - begin)
                        continue;
                    mem_zero(d->widgets, sizeof(widget) * 256);
                    e = read_widgets(d, begin, n);
                    if (e)
                        return e;
                    UINT8 iss = d->iss, oss = d->oss;
                    UINT32 capabilities = 0;
                    for (UINT32 dir = 0; dir < 2; ++dir) {
                        d->iss = dir ? iss : 0;
                        d->oss = dir ? 0 : oss;
                        e = select_route(d, begin, n);
                        if (!e) {
                            hda_route *r = &d->routes[dir];
                            r->valid = TRUE;
                            r->digital = !!(d->info.capabilities & K_AUDIO_DIGITAL);
                            r->depth = d->depth;
                            mem_copy(r->path, d->path, PATH_MAX);
                            mem_copy(r->selection, d->selection, PATH_MAX);
                            r->pin = d->info.pin;
                            r->converter = d->info.converter;
                            r->stream = dir ? 0x80 : 0x80 + iss * 32;
                            capabilities |= dir ? K_AUDIO_CAPTURE : K_AUDIO_PLAYBACK;
                            if (r->digital)
                                capabilities |= K_AUDIO_DIGITAL;
                        }
                    }
                    d->iss = iss;
                    d->oss = oss;
                    d->info.capabilities = capabilities;
                    if (capabilities) {
                        d->first = begin;
                        d->count = n;
                        str_copy(d->info.driver,
                                 (capabilities & K_AUDIO_DIGITAL) ? "hda-hdmi/dp" : "hda-analog",
                                 24);
                        hda_route *r = d->routes[0].valid ? &d->routes[0] : &d->routes[1];
                        d->info.pin = r->pin;
                        d->info.converter = r->converter;
                        d->info.codec = codec;
                        d->info.rate = 48000;
                        d->info.channels = 2;
                        d->info.bits = 16;
                        return 0;
                    }
                }
            if (d->dead)
                return K_EIO;
        }
    return K_ENOTSUP;
}
int k_audio_init(void)
{
    if (initialized || smp_started || k_cpu_id() != 0)
        return K_EBUSY;
    initialized = TRUE;
    if (!platform.dma_allowed)
        return K_ENOTSUP;
    for (UINT32 bdf = 0; bdf < 65536 && device_count < HDA_MAX; ++bdf) {
        UINT32 id = pci_get((UINT16)bdf, 0);
        if ((id & 65535) == 65535)
            continue;
        if ((pci_get((UINT16)bdf, 8) >> 16) != 0x0403)
            continue;
        hda_device *d = &devices[device_count];
        d->bdf = (UINT16)bdf;
        d->info.id = device_count;
        d->info.vendor = (UINT16)id;
        d->info.device = (UINT16)(id >> 16);
        str_copy(d->info.driver, "hda-analog", 24);
        int e = start(d);
        if (e)
            fail(d, e);
        ++device_count;
    }
    return 0;
}
UINT32 k_audio_count(void) { return device_count; }
int k_audio_get(UINT32 id, k_audio_info *out)
{
    if (!out || id >= device_count)
        return K_EINVAL;
    int e = k_mutex_lock(&audio_lock);
    if (e)
        return e;
    *out = devices[id].info;
    k_mutex_unlock(&audio_lock);
    return 0;
}
int k_audio_transfer(UINT32 id, UINT32 direction, void *pcm, UINT32 bytes, UINT32 timeout_ms)
{
    if (id >= device_count || !pcm || bytes < 256 || bytes > 65536 || (bytes & 127) ||
        !timeout_ms || timeout_ms > 5000)
        return K_EINVAL;
    int e = k_mutex_lock(&audio_lock);
    if (e)
        return e;
    hda_device *d = &devices[id];
    UINT32 s = 0;
    BOOLEAN digital_active = FALSE;
    if (d->dead) {
        e = d->info.error;
        goto out;
    }
    if ((direction != K_AUDIO_PLAYBACK && direction != K_AUDIO_CAPTURE) ||
        !(d->info.capabilities & direction)) {
        e = K_ENOTSUP;
        goto out;
    }
    hda_route *r = &d->routes[direction == K_AUDIO_CAPTURE];
    d->direction = direction;
    d->depth = r->depth;
    d->stream = r->stream;
    s = r->stream;
    d->info.pin = r->pin;
    d->info.converter = r->converter;
    mem_copy(d->path, r->path, PATH_MAX);
    mem_copy(d->selection, r->selection, PATH_MAX);
    if (timeout_ms < (bytes * 1000ULL + 191999) / 192000 + 20) {
        e = K_EINVAL;
        goto out;
    }
    digital_active = direction == K_AUDIO_PLAYBACK && r->digital;
    e = activate_route(d);
    if (e)
        goto out;
    put8(d, s, 0);
    e = wait32(d, s, 2, 0);
    if (e)
        goto broken;
    put8(d, s, 1);
    e = wait32(d, s, 1, 1);
    if (e)
        goto broken;
    put8(d, s, 0);
    e = wait32(d, s, 1, 0);
    if (e)
        goto broken;
    if (direction == K_AUDIO_PLAYBACK)
        mem_copy(d->pcm, pcm, bytes);
    else
        mem_zero(d->pcm, bytes);
    mem_zero(d->bdl, 4096);
    wr64(d->bdl, (UINT64)(UINTN)d->pcm);
    wr32(d->bdl + 8, bytes / 2);
    wr64(d->bdl + 16, (UINT64)(UINTN)(d->pcm + bytes / 2));
    wr32(d->bdl + 24, bytes / 2);
    wr32(d->bdl + 28, 1);
    put32(d, s + 0x18, (UINT32)(UINTN)d->bdl);
    put32(d, s + 0x1c, 0);
    put32(d, s + 8, bytes);
    put16(d, s + 0xc, 1);
    put16(d, s + 0x12, 0x11);
    put8(d, s + 2, (UINT8)(d->tag << 4));
    put8(d, s + 3, 0x1c);
    arch_write_fence();
    put8(d, s, 2);
    e = K_ETIMEDOUT;
    for (UINT32 n = 0; n < timeout_ms * 10; ++n) {
        UINT8 status = get8(d, s + 3);
        if (status & 0x18) {
            e = K_EIO;
            break;
        }
        if (status & 4) {
            e = 0;
            break;
        }
        k_delay_us(100);
    }
    put8(d, s, 0);
    if (wait32(d, s, 2, 0)) {
        e = K_ETIMEDOUT;
        goto broken;
    }
    if (e)
        goto broken;
    arch_read_fence();
    if (direction == K_AUDIO_CAPTURE)
        mem_copy(pcm, d->pcm, bytes);
    (void)command(d, d->info.converter, 0x70600, NULL);
    goto out;
broken:
    fail(d, e);
out:
    if (digital_active && !d->dead)
        hda_hdmi_stop(d, digital_verb, d->info.pin, d->info.converter);
    k_mutex_unlock(&audio_lock);
    return e;
}

int k_audio_output_get(UINT32 id, UINT32 index, k_audio_sink *out)
{
    if (id >= device_count || !out)
        return K_EINVAL;
    int e = k_mutex_lock(&audio_lock);
    if (e)
        return e;
    hda_device *d = &devices[id];
    e = d->dead ? d->info.error : K_ENOENT;
    for (UINT32 n = d->first; !d->dead && n < d->first + d->count; ++n) {
        widget *w = &d->widgets[n];
        if (((w->caps >> 20) & 15) != 4 || !(w->pin & 16) || (w->config >> 30) == 1)
            continue;
        if (index--)
            continue;
        if (w->pin & ((1u << 7) | (1u << 24)))
            e = hda_hdmi_sink(d, digital_verb, n, out);
        else {
            mem_zero(out, sizeof(*out));
            out->pin = n;
            out->channels = 2;
            str_copy(out->name, "analog", sizeof(out->name));
            e = 0;
        }
        break;
    }
    k_mutex_unlock(&audio_lock);
    return e;
}
int k_audio_select_output(UINT32 id, UINT32 pin)
{
    if (id >= device_count || pin > 255)
        return K_EINVAL;
    int e = k_mutex_lock(&audio_lock);
    if (e)
        return e;
    hda_device *d = &devices[id];
    if (d->dead) {
        e = d->info.error;
        goto out;
    }
    widget *w = &d->widgets[pin];
    if (!d->oss || pin < d->first || pin >= d->first + d->count || ((w->caps >> 20) & 15) != 4 ||
        !(w->pin & 16)) {
        e = K_EINVAL;
        goto out;
    }
    BOOLEAN digital = !!(w->pin & ((1u << 7) | (1u << 24)));
    if (!route(d, pin, 0, FALSE, digital)) {
        e = K_ENOTSUP;
        goto out;
    }
    hda_route *r = &d->routes[0];
    r->valid = TRUE;
    r->digital = digital;
    r->pin = pin;
    r->converter = d->path[d->depth - 1];
    r->stream = 0x80 + d->iss * 32;
    r->depth = d->depth;
    mem_copy(r->path, d->path, PATH_MAX);
    mem_copy(r->selection, d->selection, PATH_MAX);
    d->info.pin = pin;
    d->info.converter = r->converter;
    d->info.capabilities = (d->info.capabilities & ~K_AUDIO_DIGITAL) | K_AUDIO_PLAYBACK |
                           (digital ? K_AUDIO_DIGITAL : 0);
    str_copy(d->info.driver, digital ? "hda-hdmi/dp" : "hda-analog", 24);
out:
    k_mutex_unlock(&audio_lock);
    return e;
}
