// SPDX-License-Identifier: GPL-2.0-only
#include "net_internal.h"
void nic_stage(net_device *d, const char *stage)
{
    str_copy(d->diag.stage, stage, sizeof(d->diag.stage));
}

int nic_wait(net_device *d, UINT32 reg, UINT32 mask, UINT32 expected, UINT32 us)
{
    for (UINT32 elapsed = 0;; elapsed += 50) {
        UINT32 value = nic_get(d, reg);
        if (value != 0xffffffffu && (value & mask) == expected)
            return 0;
        int e = value == 0xffffffffu ? K_EIO : elapsed >= us ? K_ETIMEDOUT : 0;
        if (!e)
            e = arch_delay_checked(50);
        if (e) {
            d->diag.flags |= K_NET_DIAG_WAIT;
            d->diag.wait_register = reg;
            d->diag.wait_mask = mask;
            d->diag.wait_expected = expected;
            d->diag.wait_value = value;
            return e;
        }
    }
}

void nic_fail(net_device *d, int error)
{
    if (d->dead)
        return;
    d->dead = TRUE;
    d->info.error = error;
    if (d->kind != NIC_LOOP && d->kind != NIC_EXTERNAL)
        nic_snapshot(d);
    d->info.flags &= ~K_NET_LINK;
    d->info.errors++;
    if (d->kind == NIC_EXTERNAL)
        d->external.stop(d->context);
    else if (d->kind != NIC_LOOP)
        pci_command(d->bdf, 0, 4); /* Failed DMA pages stay pinned. */
}

void nic_pci_prepare(UINT16 bdf, BOOLEAN io)
{
    pci_command(bdf, (UINT16)(0x400 | (io ? 1 : 2)), 4);
    UINT8 at = (UINT8)pci_get(bdf, 0x34) & 0xfc;
    UINT64 seen = 0;
    for (UINT32 n = 0; n < 48 && at >= 0x40; ++n) {
        UINT32 bit = (at - 0x40) / 4;
        if (seen & (1ULL << bit))
            break;
        seen |= 1ULL << bit;
        UINT32 cap = pci_get(bdf, at);
        UINT8 id = (UINT8)cap;
        if (id == 5)
            pci_put(bdf, at, cap & ~(1u << 16));
        if (id == 0x11)
            pci_put(bdf, at, (cap & ~(1u << 31)) | (1u << 30));
        if (id == 1 && at <= 0xf8) {
            UINT32 pm = pci_get(bdf, at + 4);
            pci_put(bdf, at + 4, pm & 0x7ffcu);
            k_delay_us(10000);
        }
        at = (UINT8)(cap >> 8) & 0xfc;
    }
}

int nic_start(net_device *d)
{
    UINT16 vendor = d->info.vendor, device = d->info.device;
    str_copy(d->info.driver, "unsupported", 16);
    if (!platform.dma_allowed)
        return K_ENOTSUP;
    if (vendor == 0x8086 && (device == 0x100e || device == 0x100f || device == 0x10d3 ||
                             device == 0x1010 || device == 0x107c)) {
        str_copy(d->info.driver, "intel-e1000", 16);
        return nic_intel_start(d, FALSE);
    }
    if (vendor == 0x8086 && (device == 0x15f2 || device == 0x15f3 || device == 0x125b ||
                             device == 0x125c || device == 0x125d)) {
        str_copy(d->info.driver, "intel-i225/226", 16);
        return nic_intel_start(d, TRUE);
    }
    if (vendor == 0x10ec && device == 0x8139) {
        str_copy(d->info.driver, "rtl8139", 16);
        return nic_rtl_start(d);
    }
    return K_ENOTSUP;
}
void nic_poll(net_device *d)
{
    if (d->dead || d->kind == NIC_LOOP)
        return;
    if (d->kind == NIC_EXTERNAL) {
        UINT32 speed = 0;
        int up = d->external.link(d->context, &speed);
        if (up < 0) {
            nic_fail(d, up);
            return;
        }
        if (up)
            d->info.flags |= K_NET_LINK;
        else
            d->info.flags &= ~K_NET_LINK;
        d->info.speed_mbps = speed;
        for (UINT32 i = 0; i < 32; ++i) {
            UINT8 frame[K_NET_FRAME_MAX];
            int n = d->external.receive(d->context, frame, sizeof(frame));
            if (n == K_EAGAIN || n == 0)
                break;
            if (n < 0) {
                nic_fail(d, n);
                break;
            }
            if (n < 14 || (UINT32)n > K_NET_FRAME_MAX) {
                nic_fail(d, K_EIO);
                break;
            }
            net_enqueue(d->info.id, frame, (UINT32)n);
        }
        return;
    }
    if (d->kind == NIC_RTL8139)
        nic_rtl_poll(d);
    else
        nic_intel_poll(d);
}
int nic_transmit(net_device *d, const void *bytes, UINT32 n)
{
    if (d->dead || !(d->info.flags & K_NET_UP) || !(d->info.flags & K_NET_LINK))
        return K_ENETDOWN;
    if (n < 14 || n > 1514)
        return K_EMSGSIZE;
    if (d->kind == NIC_LOOP) {
        int e = net_enqueue(d->info.id, bytes, n);
        if (e)
            return e;
    } else if (d->kind == NIC_EXTERNAL) {
        int e = d->external.transmit(d->context, bytes, n);
        if (e)
            return e;
    } else {
        UINT32 limit = d->kind == NIC_RTL8139 ? 4 : 63;
        if (d->tx_used >= limit)
            return K_EAGAIN;
        UINT32 index = d->tx_tail, wire = n < 60 ? 60 : n;
        UINT8 *data = d->tx_data + index * (d->kind == NIC_RTL8139 ? 4096 : 2048);
        mem_copy(data, bytes, n);
        if (wire > n)
            mem_zero(data + n, wire - n);
        if (d->kind == NIC_RTL8139)
            nic_rtl_kick(d, wire, index);
        else
            nic_intel_kick(d, data, wire, index);
        d->tx_when[index] = k_uptime_ms();
        ++d->tx_used;
    }
    ++d->info.tx_packets;
    d->info.tx_bytes += n;
    return 0;
}
