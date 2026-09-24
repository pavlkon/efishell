// SPDX-License-Identifier: GPL-2.0-only
#include "net_internal.h"
int nic_rtl_start(net_device *d)
{
    UINT32 bar = pci_get(d->bdf, 0x10);
    if (!(bar & 1) || (bar & ~3u) > 0xff00)
        return K_ENOTSUP;
    d->io = (UINT16)(bar & ~3u);
    d->kind = NIC_RTL8139;
    nic_pci_prepare(d->bdf, TRUE);
    arch_out8(d->io + 0x52, 0);
    arch_out16(d->io + 0x3c, 0);
    arch_out8(d->io + 0x37, 0x10);
    UINT32 count = 10000;
    while (count && (arch_in8(d->io + 0x37) & 0x10)) {
        --count;
        k_delay_us(10);
    }
    if (arch_in8(d->io + 0x37) & 0x10)
        return K_ETIMEDOUT;
    for (UINT32 i = 0; i < 6; ++i)
        d->info.mac[i] = arch_in8(d->io + (UINT16)i);
    if (!nic_valid_mac(d->info.mac))
        return K_EIO;
    d->rx_data = (void *)(UINTN)k_pmm_alloc_pages(9, 0x100000000ULL);
    d->tx_data = (void *)(UINTN)k_pmm_alloc_pages(4, 0x100000000ULL);
    if (!d->rx_data || !d->tx_data)
        return K_ENOMEM;
    arch_out32(d->io + 0x30, (UINT32)(UINTN)d->rx_data);
    for (UINT32 i = 0; i < 4; ++i)
        arch_out32(d->io + 0x20 + (UINT16)(4 * i), (UINT32)(UINTN)(d->tx_data + 4096 * i));
    arch_out32(d->io + 8, 0xffffffff);
    arch_out32(d->io + 12, 0xffffffff);
    arch_out32(d->io + 0x44, 0x0000f68e);
    arch_out32(d->io + 0x40, 0x03000700);
    arch_out16(d->io + 0x3e, 0xffff);
    arch_out16(d->io + 0x3c, 0);
    pci_command(d->bdf, 5, 0);
    arch_out8(d->io + 0x37, 0x0c);
    return 0;
}

void nic_rtl_poll(net_device *d)
{
    UINT64 now = k_uptime_ms();
    if (now >= d->link_check) {
        d->link_check = now + 250;
        UINT8 p = arch_in8(d->io + 0x58);
        if (!(p & 4))
            d->info.flags |= K_NET_LINK;
        else
            d->info.flags &= ~K_NET_LINK;
        d->info.speed_mbps = (p & 8) ? 10 : 100;
    }
    for (UINT32 budget = 0; budget < 32 && !(arch_in8(d->io + 0x37) & 1); ++budget) {
        arch_read_fence();
        UINT8 *r = d->rx_data + d->rx_head;
        UINT32 h = *(volatile UINT32 *)r;
        UINT32 n = h >> 16;
        if (n == 0xfff0) { /* Hardware is still transferring this frame. */
            if (!d->rx_wait_until)
                d->rx_wait_until = now + 1000;
            else if (now >= d->rx_wait_until)
                nic_fail(d, K_ETIMEDOUT);
            break;
        }
        d->rx_wait_until = 0;
        if (n < 18 || n > 1522) {
            nic_fail(d, K_EIO);
            return;
        }
        if ((h & 0x3f) == 1)
            net_enqueue(d->info.id, r + 4, n - 4);
        else
            ++d->info.errors;
        d->rx_head = (d->rx_head + 4 + n + 3) & ~3u;
        d->rx_head %= 32768;
        arch_out16(d->io + 0x38, (UINT16)(d->rx_head - 16));
    }
    arch_out16(d->io + 0x3e, 0xffff);
    while (d->tx_used) {
        UINT32 st = arch_in32(d->io + 0x10 + (UINT16)(d->tx_clean * 4));
        if (!(st & ((1u << 15) | (1u << 14) | (1u << 30)))) {
            if (now - d->tx_when[d->tx_clean] > 5000)
                nic_fail(d, K_ETIMEDOUT);
            break;
        }
        if (st & ((1u << 14) | (1u << 29) | (1u << 30) | (1u << 31))) {
            nic_fail(d, K_EIO);
            break;
        }
        d->tx_clean = (d->tx_clean + 1) % 4;
        --d->tx_used;
    }
}
void nic_rtl_kick(net_device *d, UINT32 wire, UINT32 index)
{
    arch_write_fence();
    arch_out32(d->io + 0x10 + (UINT16)(index * 4), wire | (0x20u << 16));
    d->tx_tail = (index + 1) % 4;
}
