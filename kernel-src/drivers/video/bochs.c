// SPDX-License-Identifier: GPL-2.0-only
#include "display_internal.h"
static UINT16 saved[10];
static UINT16 get(UINT16 index)
{
    arch_out16(0x1ce, index);
    return arch_in16(0x1cf);
}
static void put(UINT16 index, UINT16 value)
{
    arch_out16(0x1ce, index);
    arch_out16(0x1cf, value);
}
static int restore(void *unused)
{
    (void)unused;
    put(4, 0);
    for (UINT16 i = 1; i < 10; ++i)
        if (i != 4 && i != 5)
            put(i, saved[i]);
    put(4, saved[4]);
    return get(4) == saved[4] ? 0 : K_EIO;
}
static int mode(void *unused, k_display_info *d, UINT32 w, UINT32 h, UINT32 bits)
{
    (void)unused;
    if (bits != 32 || w < 320 || h < 200 || (w & 7) || (UINT64)w * h * 4 > d->bytes)
        return K_EINVAL;
    for (UINT16 i = 0; i < 10; ++i)
        saved[i] = get(i);
    put(4, 0);
    put(1, (UINT16)w);
    put(2, (UINT16)h);
    put(3, 32);
    put(6, (UINT16)w);
    put(8, 0);
    put(9, 0);
    put(4, 0x41);
    if (get(1) != w || get(2) != h || get(3) != 32 || get(6) != w) {
        (void)restore(NULL);
        return K_EIO;
    }
    d->width = w;
    d->height = h;
    d->bits = 32;
    d->pitch = w * 4;
    return 0;
}
void display_bochs_probe(void)
{
    for (UINT32 bdf = 0; bdf < 65536; ++bdf) {
        UINT32 id = pci_get((UINT16)bdf, 0);
        if (id != 0x11111234 && id != 0xbeef80ee)
            continue;
        if ((pci_get((UINT16)bdf, 8) >> 16) != 0x0300 || !(pci_get((UINT16)bdf, 4) & 1))
            continue;
        UINT16 version = get(0);
        if (version < 0xb0c4 || version > 0xb0c5)
            continue;
        UINT64 base = pci_mbar((UINT16)bdf, 0x10), bytes = (UINT64)get(10) * 65536;
        if (!base || bytes < 0x100000 || bytes > 256 * 1024 * 1024 || k_mmio_map(base, bytes))
            continue;
        display_device d = {0};
        d.info.framebuffer = base;
        d.info.bytes = bytes;
        d.info.width = get(1);
        d.info.height = get(2);
        d.info.bits = get(3);
        d.info.pitch = get(6) * (d.info.bits / 8);
        d.info.capabilities = K_DISPLAY_MODESET;
        d.info.red_shift = 16;
        d.info.green_shift = 8;
        str_copy(d.info.driver, "bochs-dispi", 24);
        if (!d.info.width || !d.info.height || d.info.bits != 32 ||
            (UINT64)d.info.pitch * d.info.height > bytes)
            continue;
        d.mode = mode;
        d.restore = restore;
        display_register(&d);
        return;
    }
}
