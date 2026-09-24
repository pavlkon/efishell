// SPDX-License-Identifier: GPL-2.0-only
#include "display_internal.h"
static display_device displays[8];
static UINT32 count;
static INT32 active = -1;
static k_mutex display_lock = K_MUTEX_INIT;
int display_register(const display_device *d)
{
    if (!d || count == ARRAY_LEN(displays))
        return K_ENOSPC;
    UINT32 i = count;
    if (d->info.framebuffer && count && d->info.framebuffer == displays[0].info.framebuffer)
        i = 0;
    displays[i] = *d;
    displays[i].info.id = i;
    displays[i].initial = displays[i].info;
    if (i == count)
        ++count;
    return 0;
}
int k_display_init(UINT64 bytes, UINT8 red, UINT8 green, UINT8 blue)
{
    if (count || !fb_base || !bytes || (UINT64)fb_pitch * fb_height * 4 > bytes || smp_started)
        return K_EINVAL;
    display_device d = {0};
    d.info = (k_display_info){0,
                              fb_width,
                              fb_height,
                              fb_pitch * 4,
                              32,
                              K_DISPLAY_BOOT,
                              (UINT64)(UINTN)fb_base,
                              bytes,
                              red,
                              green,
                              blue,
                              0,
                              {0}};
    str_copy(d.info.driver, "boot-framebuffer", 24);
    display_register(&d);
    display_bochs_probe();
    display_vga_probe();
    return 0;
}
UINT32 k_display_count(void) { return count; }
int k_display_get(UINT32 id, k_display_info *out)
{
    if (!out || id >= count)
        return K_EINVAL;
    int e = k_mutex_lock(&display_lock);
    if (e)
        return e;
    *out = displays[id].info;
    k_mutex_unlock(&display_lock);
    return 0;
}
int k_display_mode(UINT32 id, UINT32 width, UINT32 height, UINT32 bits)
{
    if (id >= count || !width || !height || width > 8192 || height > 8192)
        return K_EINVAL;
    int e = k_mutex_lock(&display_lock);
    if (e)
        return e;
    display_device *d = &displays[id];
    if (active >= 0) {
        e = K_EBUSY;
        goto out;
    }
    console_display_pause(TRUE);
    if (d->mode)
        e = d->mode(d->context, &d->info, width, height, bits);
    else if (width != d->info.width || height != d->info.height || bits != d->info.bits)
        e = K_ENOTSUP;
    if (e) {
        console_display_pause(FALSE);
        goto out;
    }
    d->active = TRUE;
    active = (INT32)id;
out:
    k_mutex_unlock(&display_lock);
    return e;
}
int k_display_write(UINT32 id, UINT32 x, UINT32 y, UINT32 width, UINT32 height, const UINT32 *argb,
                    UINT32 stride)
{
    if (id >= count || !argb || !width || !height || stride < width || stride > 8192)
        return K_EINVAL;
    int e = k_mutex_lock(&display_lock);
    if (e)
        return e;
    k_display_info *d = &displays[id].info;
    if (active != (INT32)id || x >= d->width || y >= d->height || width > d->width - x ||
        height > d->height - y) {
        e = K_EINVAL;
        goto out;
    }
    UINT32 pixel_bytes = d->bits / 8;
    if ((d->bits != 8 && d->bits != 32) || (UINT64)d->pitch * d->height > d->bytes) {
        e = K_EFAULT;
        goto out;
    }
    for (UINT32 row = 0; row < height; ++row)
        for (UINT32 col = 0; col < width; ++col) {
            UINT32 rgb = argb[(UINTN)row * stride + col];
            volatile UINT8 *p = (void *)(UINTN)(d->framebuffer + (UINT64)(row + y) * d->pitch +
                                                (UINT64)(col + x) * pixel_bytes);
            if (d->bits == 8)
                *p = (UINT8)(((rgb >> 16) & 0xe0) | ((rgb >> 11) & 0x1c) | ((rgb >> 6) & 3));
            else
                *(volatile UINT32 *)p = (((rgb >> 16) & 255) << d->red_shift) |
                                        (((rgb >> 8) & 255) << d->green_shift) |
                                        ((rgb & 255) << d->blue_shift);
        }
    arch_write_fence();
out:
    k_mutex_unlock(&display_lock);
    return e;
}
int k_display_restore(UINT32 id)
{
    if (id >= count)
        return K_EINVAL;
    int e = k_mutex_lock(&display_lock);
    if (e)
        return e;
    display_device *d = &displays[id];
    if (active != (INT32)id) {
        e = K_EINVAL;
        goto out;
    }
    if (d->restore)
        e = d->restore(d->context);
    if (!e) {
        d->info = d->initial;
        d->active = FALSE;
        active = -1;
        console_display_pause(FALSE);
    }
out:
    k_mutex_unlock(&display_lock);
    return e;
}
