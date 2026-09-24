// SPDX-License-Identifier: GPL-2.0-only
#include "display_internal.h"
static UINT8 *backup;
static UINT8 misc, pel_mask, seq[5], crtc[25], gfx[9], attr[21], palette[768];
static UINT16 crtc_port;
static UINT8 indexed_get(UINT16 p, UINT8 i)
{
    arch_out8(p, i);
    return arch_in8(p + 1);
}
static void indexed_put(UINT16 p, UINT8 i, UINT8 v)
{
    arch_out8(p, i);
    arch_out8(p + 1, v);
}
static void attribute(UINT8 i, UINT8 v)
{
    (void)arch_in8(crtc_port + 6);
    arch_out8(0x3c0, i);
    arch_out8(0x3c0, v);
}
static void planes(BOOLEAN save)
{
    indexed_put(0x3c4, 4, 6);
    indexed_put(0x3ce, 5, 0);
    indexed_put(0x3ce, 6, 5);
    for (UINT32 plane = 0; plane < 4; ++plane) {
        indexed_put(0x3ce, 4, (UINT8)plane);
        indexed_put(0x3c4, 2, (UINT8)(1u << plane));
        volatile UINT8 *vram = (void *)(UINTN)0xa0000;
        for (UINT32 i = 0; i < 65536; ++i) {
            if (save)
                backup[plane * 65536 + i] = vram[i];
            else
                vram[i] = backup[plane * 65536 + i];
        }
    }
}
static int restore(void *unused)
{
    (void)unused;
    UINT64 irq = k_irq_save();
    planes(FALSE);
    arch_out8(0x3c2, misc);
    crtc_port = (misc & 1) ? 0x3d4 : 0x3b4;
    indexed_put(0x3c4, 0, 1);
    for (UINT8 i = 1; i < 5; ++i)
        indexed_put(0x3c4, i, seq[i]);
    indexed_put(0x3c4, 0, seq[0]);
    indexed_put(crtc_port, 0x11, crtc[0x11] & 0x7f);
    for (UINT8 i = 0; i < 25; ++i)
        indexed_put(crtc_port, i, crtc[i]);
    for (UINT8 i = 0; i < 9; ++i)
        indexed_put(0x3ce, i, gfx[i]);
    for (UINT8 i = 0; i < 21; ++i)
        attribute(i, attr[i]);
    arch_out8(0x3c8, 0);
    for (UINT32 i = 0; i < 768; ++i)
        arch_out8(0x3c9, palette[i]);
    arch_out8(0x3c6, pel_mask);
    (void)arch_in8(crtc_port + 6);
    arch_out8(0x3c0, 0x20);
    k_irq_restore(irq);
    return 0;
}
static int mode(void *unused, k_display_info *d, UINT32 width, UINT32 height, UINT32 bits)
{
    (void)unused;
    if (width != 320 || height != 200 || bits != 8 || !backup)
        return K_ENOTSUP;
    static const UINT8 s[5] = {3, 1, 15, 0, 14};
    static const UINT8 c[25] = {0x5f, 0x4f, 0x50, 0x82, 0x54, 0x80, 0xbf, 0x1f, 0,
                                0x41, 0,    0,    0,    0,    0,    0,    0x9c, 0x0e,
                                0x8f, 0x28, 0x40, 0x96, 0xb9, 0xa3, 0xff};
    static const UINT8 g[9] = {0, 0, 0, 0, 0, 0x40, 5, 15, 255};
    UINT64 irq = k_irq_save();
    misc = arch_in8(0x3cc);
    pel_mask = arch_in8(0x3c6);
    crtc_port = (misc & 1) ? 0x3d4 : 0x3b4;
    for (UINT8 i = 0; i < 5; ++i)
        seq[i] = indexed_get(0x3c4, i);
    for (UINT8 i = 0; i < 25; ++i)
        crtc[i] = indexed_get(crtc_port, i);
    for (UINT8 i = 0; i < 9; ++i)
        gfx[i] = indexed_get(0x3ce, i);
    for (UINT8 i = 0; i < 21; ++i) {
        (void)arch_in8(crtc_port + 6);
        arch_out8(0x3c0, i);
        attr[i] = arch_in8(0x3c1);
    }
    arch_out8(0x3c7, 0);
    for (UINT32 i = 0; i < 768; ++i)
        palette[i] = arch_in8(0x3c9);
    planes(TRUE);
    arch_out8(0x3c2, 0x63);
    crtc_port = 0x3d4;
    indexed_put(0x3c4, 0, 1);
    for (UINT8 i = 1; i < 5; ++i)
        indexed_put(0x3c4, i, s[i]);
    indexed_put(0x3c4, 0, 3);
    indexed_put(crtc_port, 0x11, indexed_get(crtc_port, 0x11) & 0x7f);
    for (UINT8 i = 0; i < 25; ++i)
        indexed_put(crtc_port, i, c[i]);
    for (UINT8 i = 0; i < 9; ++i)
        indexed_put(0x3ce, i, g[i]);
    for (UINT8 i = 0; i < 16; ++i)
        attribute(i, i);
    attribute(0x10, 0x41);
    attribute(0x11, 0);
    attribute(0x12, 15);
    attribute(0x13, 0);
    attribute(0x14, 0);
    arch_out8(0x3c6, 255);
    arch_out8(0x3c8, 0);
    for (UINT32 i = 0; i < 256; ++i) {
        arch_out8(0x3c9, (UINT8)((i >> 5) * 63 / 7));
        arch_out8(0x3c9, (UINT8)(((i >> 2) & 7) * 63 / 7));
        arch_out8(0x3c9, (UINT8)((i & 3) * 21));
    }
    (void)arch_in8(crtc_port + 6);
    arch_out8(0x3c0, 0x20);
    k_irq_restore(irq);
    d->width = 320;
    d->height = 200;
    d->pitch = 320;
    d->bits = 8;
    return 0;
}
void display_vga_probe(void)
{
    for (UINT32 bdf = 0; bdf < 65536; ++bdf) {
        UINT32 id = pci_get((UINT16)bdf, 0);
        /* Legacy VGA backends: Cirrus GD5446 and S3 Trio64. Modern GPUs keep their boot scanout. */
        if (id != 0x00b81013 && id != 0x88115333)
            continue;
        if ((pci_get((UINT16)bdf, 8) >> 16) != 0x0300 || (pci_get((UINT16)bdf, 4) & 3) != 3)
            continue;
        if (k_mmio_map(0xa0000, 65536))
            return;
        backup = (void *)(UINTN)k_pmm_alloc_pages(64, 0);
        if (!backup)
            return;
        display_device d = {0};
        d.info.framebuffer = 0xa0000;
        d.info.bytes = 65536;
        d.info.bits = 8;
        d.info.capabilities = K_DISPLAY_MODESET | K_DISPLAY_VGA;
        str_copy(d.info.driver, "legacy-vga", 24);
        d.mode = mode;
        d.restore = restore;
        display_register(&d);
        return;
    }
}
