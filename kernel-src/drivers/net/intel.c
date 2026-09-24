// SPDX-License-Identifier: GPL-2.0-only
#include "net_internal.h"
void nic_snapshot(net_device *d)
{
    k_net_diag *s = &d->diag;
    s->revision = pci_get(d->bdf, 8) & 255;
    s->pci_command = pci_get(d->bdf, 4) & 65535;
    s->rx_next = d->rx_head;
    s->tx_next = d->tx_tail;
    s->tx_pending = d->tx_used;
    s->error = d->info.error;
    if (!d->regs || (d->kind != NIC_IGC && d->kind != NIC_E1000))
        return;
    s->flags |= K_NET_DIAG_INTEL;
    UINT32 rb = d->kind == NIC_IGC ? 0xc000 : 0x2800;
    UINT32 tb = d->kind == NIC_IGC ? 0xe000 : 0x3800;
    s->control = nic_get(d, 0);
    s->status = nic_get(d, 8);
    s->eecd = nic_get(d, 0x10);
    s->mdic = nic_get(d, 0x20);
    s->rx_control = nic_get(d, 0x100);
    s->tx_control = nic_get(d, 0x400);
    s->rx_desc_control = nic_get(d, rb + 40);
    s->tx_desc_control = nic_get(d, tb + 40);
    s->rx_head = nic_get(d, rb + 16);
    s->rx_tail = nic_get(d, rb + 24);
    s->tx_head = nic_get(d, tb + 16);
    s->tx_tail = nic_get(d, tb + 24);
    if (d->kind == NIC_IGC)
        s->phy_power = nic_get(d, 0xe14);
}

static int nic_hw_sem(net_device *d)
{
    for (UINT32 i = 0; i < 2000; ++i) {
        UINT32 v = nic_get(d, 0x5b50);
        if (v == 0xffffffffu)
            return K_EIO;
        if (!(v & 1)) {
            for (UINT32 j = 0; j < 100; ++j) {
                nic_put(d, 0x5b50, nic_get(d, 0x5b50) | 2);
                if (nic_get(d, 0x5b50) & 2)
                    return 0;
                k_delay_us(50);
            }
            nic_put(d, 0x5b50, nic_get(d, 0x5b50) & ~3u);
            return K_ETIMEDOUT;
        }
        k_delay_us(50);
    }
    return K_EBUSY;
}

static int nic_phy_claim(net_device *d, BOOLEAN release)
{
    for (UINT32 i = 0; i < 200; ++i) {
        int e = nic_hw_sem(d);
        if (e)
            return e;
        UINT32 v = nic_get(d, 0x5b5c);
        if (release || !(v & 0x00020002u)) {
            nic_put(d, 0x5b5c, release ? v & ~2u : v | 2u);
            nic_put(d, 0x5b50, nic_get(d, 0x5b50) & ~3u);
            return 0;
        }
        nic_put(d, 0x5b50, nic_get(d, 0x5b50) & ~3u);
        k_delay_us(5000);
    }
    return K_EBUSY;
}

static int nic_mdio(net_device *d, UINT32 reg, UINT16 *value, BOOLEAN write)
{
    if (reg > 31 || !value)
        return K_EINVAL;
    nic_stage(d, write ? "PHY write" : "PHY read");
    /* The integrated I225/I226 PHY uses address 0; the older e1000 uses 1. */
    UINT32 address = d->kind == NIC_IGC ? 0 : 1;
    d->diag.phy_address = address;
    nic_put(d, 0x20,
            ((UINT32)(write ? *value : 0)) | (reg << 16) | (address << 21) |
                (write ? 1u << 26 : 1u << 27));
    int e = nic_wait(d, 0x20, 1u << 28, 1u << 28, 100000);
    if (e)
        return e;
    UINT32 v = nic_get(d, 0x20);
    if ((v & (1u << 30)) || ((v >> 16) & 31) != reg || ((v >> 21) & 31) != address)
        return K_EIO;
    *value = (UINT16)v;
    return 0;
}

static int nic_intel_phy(net_device *d, BOOLEAN advanced)
{
    int e = 0;
    if (advanced) {
        nic_stage(d, "PHY ownership");
        e = nic_phy_claim(d, FALSE);
        if (e)
            return e;
        UINT32 phpm = nic_get(d, 0xe14), manc = nic_get(d, 0x5820);
        if (phpm == 0xffffffffu || manc == 0xffffffffu) {
            e = K_EIO;
            goto release;
        }
        nic_put(d, 0xe14, phpm & ~0x20u);
        nic_fence(d);
        /* Reset page-select state inherited from firmware, unless management blocks reset. */
        if (!(manc & (1u << 18))) {
            nic_stage(d, "PHY reset");
            UINT32 ctrl = nic_get(d, 0) & ~(1u << 31);
            nic_put(d, 0, ctrl | (1u << 31));
            nic_fence(d);
            k_delay_us(100);
            nic_put(d, 0, ctrl);
            nic_fence(d);
            k_delay_us(100);
            e = nic_wait(d, 0xe14, 0x100, 0x100, 100000);
            d->diag.phy_reset_value = nic_get(d, 0xe14);
            if (e && e != K_ETIMEDOUT)
                goto release;
            if (e) {
                /* PHPM reset-complete is advisory; require working MDIO below. */
                d->diag.flags |= K_NET_DIAG_PHY_RESET_TIMEOUT;
                d->diag.flags &= ~K_NET_DIAG_WAIT;
            }
            k_delay_us(100);
        }
        UINT16 id1, id2;
        e = nic_mdio(d, 2, &id1, FALSE);
        if (!e)
            e = nic_mdio(d, 3, &id2, FALSE);
        if (e)
            goto release;
        d->diag.phy_id = ((UINT32)id1 << 16) | id2;
        if (!id1 || id1 == 0xffff || !d->diag.phy_id || d->diag.phy_id == 0xffffffffu) {
            e = K_EIO;
            goto release;
        }
    }
    UINT16 phy;
    e = nic_mdio(d, 0, &phy, FALSE);
    if (!e && advanced) {
        for (UINT32 i = 0; !e && (phy & (1u << 15)) && i < 20; ++i) {
            k_delay_us(1000);
            e = nic_mdio(d, 0, &phy, FALSE);
        }
        d->diag.phy_control = phy;
        if (!e && (phy & (1u << 15))) {
            nic_stage(d, "PHY still in reset");
            e = K_ETIMEDOUT;
        }
    }
    if (!e) {
        /* Normal operation, autonegotiation; remove reset/loopback/power-down/isolate. */
        phy = (phy & ~((1u << 15) | (1u << 14) | (1u << 11) | (1u << 10))) | (1u << 12) | (1u << 9);
        d->diag.phy_control = phy;
        e = nic_mdio(d, 0, &phy, TRUE);
    }
    if (!e && advanced) {
        e = nic_mdio(d, 0, &phy, FALSE);
        if (!e) {
            d->diag.phy_control = phy;
            if ((phy & ((1u << 15) | (1u << 14) | (1u << 12) | (1u << 11) | (1u << 10))) !=
                (1u << 12)) {
                nic_stage(d, "PHY control verify");
                e = K_EIO;
            }
        }
    }
    if (!e && advanced) {
        e = nic_mdio(d, 1, &phy, FALSE);
        if (!e)
            e = nic_mdio(d, 1, &phy, FALSE);
        if (!e) {
            d->diag.phy_status = phy;
            if (!phy || phy == 0xffff) {
                nic_stage(d, "PHY status verify");
                e = K_EIO;
            } else
                d->diag.flags |= K_NET_DIAG_PHY_VERIFIED;
        }
    }
release:
    if (advanced) {
        if (!e)
            nic_stage(d, "PHY release");
        int released = nic_phy_claim(d, TRUE);
        if (!e)
            e = released;
    }
    return advanced ? e : 0; /* Preserve legacy MACs whose firmware manages the PHY. */
}

int nic_intel_start(net_device *d, BOOLEAN advanced)
{
    nic_stage(d, "PCI mapping");
    UINT64 base = pci_mbar(d->bdf, 0x10);
    if (!base || k_mmio_map(base, 0x20000))
        return K_ENOTSUP;
    nic_pci_prepare(d->bdf, FALSE);
    d->regs = (void *)(UINTN)base;
    d->kind = advanced ? NIC_IGC : NIC_E1000;
    UINT32 imc = advanced ? 0x150c : 0xd8;
    nic_put(d, imc, 0xffffffff);
    if (advanced)
        nic_put(d, 0x1528, 0xffffffff);
    nic_put(d, 0x100, 0);
    nic_put(d, 0x400, 8);
    nic_fence(d);
    k_delay_us(10000);
    nic_stage(d, "MAC reset");
    nic_put(d, 0, nic_get(d, 0) | (1u << 26));
    int e = nic_wait(d, 0, 1u << 26, 0, 100000);
    if (e)
        return e;
    k_delay_us(20000);
    if (advanced) {
        nic_stage(d, "NVM auto-load");
        e = nic_wait(d, 0x10, 0x200, 0x200, 500000);
        if (e)
            return e;
        nic_put(d, 0x18, nic_get(d, 0x18) | (1u << 28));
    }
    nic_put(d, imc, 0xffffffff);
    if (advanced)
        nic_put(d, 0x1528, 0xffffffff);
    nic_stage(d, "MAC address");
    UINT32 low = nic_get(d, 0x5400), high = nic_get(d, 0x5404);
    if (!(high & 0x80000000))
        return K_EIO;
    wr32(d->info.mac, low);
    d->info.mac[4] = (UINT8)high;
    d->info.mac[5] = (UINT8)(high >> 8);
    if (!nic_valid_mac(d->info.mac))
        return K_EIO;
    nic_stage(d, "DMA allocation");
    d->rx_ring = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
    d->tx_ring = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
    d->rx_data = (void *)(UINTN)k_pmm_alloc_pages(32, 0x100000000ULL);
    d->tx_data = (void *)(UINTN)k_pmm_alloc_pages(32, 0x100000000ULL);
    if (!d->rx_ring || !d->tx_ring || !d->rx_data || !d->tx_data)
        return K_ENOMEM;
    for (UINT32 i = 0; i < 64; ++i) {
        wr64(d->rx_ring + i * 16, (UINT64)(UINTN)(d->rx_data + i * 2048));
        d->tx_ring[i * 16 + 12] = 1;
    }
    if (advanced) {
        nic_stage(d, "queue disable");
        for (UINT32 i = 0; i < 4; ++i) {
            nic_put(d, 0xc028 + i * 64, 0);
            nic_put(d, 0xe028 + i * 64, 0);
        }
        nic_fence(d);
        for (UINT32 i = 0; i < 4; ++i) {
            e = nic_wait(d, 0xc028 + i * 64, 1u << 25, 0, 100000);
            if (!e)
                e = nic_wait(d, 0xe028 + i * 64, 1u << 25, 0, 100000);
            if (e)
                return e;
        }
        /* I225/I226 SRAM allocation: RX 34+2 KiB, TX queue 0 20+4 KiB. */
        nic_put(d, 0x2404, 34u | (2u << 6));
        nic_put(d, 0x3404, 20u | (4u << 24));
        nic_put(d, 0x5404, (high & 0xffffu) | 0x80000000u);
    }
    nic_stage(d, "queue setup");
    for (UINT32 i = 0; i < 128; ++i)
        nic_put(d, 0x5200 + 4 * i, 0xffffffff); /* IPv6 multicast; software filters. */
    for (UINT32 i = 1; i < 16; ++i) {
        nic_put(d, 0x5400 + i * 8, 0);
        nic_put(d, 0x5404 + i * 8, 0);
    }
    nic_put(d, 0x5000, 0);
    nic_put(d, 0x38, 0x8100);
    UINT32 rb = advanced ? 0xc000 : 0x2800, tb = advanced ? 0xe000 : 0x3800;
    nic_put(d, rb, (UINT32)(UINTN)d->rx_ring);
    nic_put(d, rb + 4, 0);
    nic_put(d, rb + 8, 1024);
    nic_put(d, rb + 16, 0);
    nic_put(d, rb + 24, 0);
    nic_put(d, tb, (UINT32)(UINTN)d->tx_ring);
    nic_put(d, tb + 4, 0);
    nic_put(d, tb + 8, 1024);
    nic_put(d, tb + 16, 0);
    nic_put(d, tb + 24, 0);
    if (advanced) {
        nic_put(d, 0x5818, 0);
        nic_put(d, 0x5004, 1522);
        nic_put(d, 0x5480, 0);
        nic_put(d, rb + 12, 2u | 0x02000000u | 0x80000000u);
    } else {
        nic_put(d, 0x2820, 0);
        nic_put(d, 0x3820, 0);
        nic_put(d, 0x3828, 0x01010000);
    }
    UINT32 ctrl = nic_get(d, 0);
    ctrl &= ~((1u << 3) | (1u << 11) | (1u << 12) | (1u << 27) | (1u << 28) | (3u << 8));
    if (advanced)
        ctrl &= ~((1u << 2) | (1u << 31));
    ctrl |= 1u << 6;
    nic_put(d, 0, ctrl);
    e = nic_intel_phy(d, advanced);
    if (e)
        return e;
    nic_stage(d, "DMA enable");
    pci_command(d->bdf, 6, 0);
    if ((pci_get(d->bdf, 4) & 6) != 6)
        return K_EIO;
    nic_put(d, 0x410, 10u | (8u << 10) | (6u << 20));
    nic_put(d, 0x400, 2u | 8u | (15u << 4) | (64u << 12) | (advanced ? 1u << 24 : 0));
    nic_put(d, 0x100, 2u | (1u << 15) | (1u << 26));
    nic_fence(d);
    if (advanced) {
        /* Global engines and bus mastering must be on before queue activation. */
        nic_stage(d, "TX queue enable");
        nic_put(d, tb + 40, 0x02000000u | (1u << 16) | (1u << 8) | 8u);
        e = nic_wait(d, tb + 40, 1u << 25, 1u << 25, 100000);
        if (e)
            return e;
        nic_stage(d, "RX queue enable");
        nic_put(d, rb + 40, 0x02000000u | (4u << 16) | (8u << 8) | 8u);
        e = nic_wait(d, rb + 40, 1u << 25, 1u << 25, 100000);
        if (e)
            return e;
    }
    nic_put(d, rb + 24, 63);
    nic_fence(d);
    d->diag.flags &= ~K_NET_DIAG_WAIT;
    nic_stage(d, "running");
    return 0;
}

void nic_intel_poll(net_device *d)
{
    UINT64 now = k_uptime_ms();
    if (now >= d->link_check) {
        d->link_check = now + 250;
        UINT32 st = nic_get(d, 8);
        if (st == 0xffffffffu) {
            nic_fail(d, K_EIO);
            return;
        }
        if (st & 2)
            d->info.flags |= K_NET_LINK;
        else
            d->info.flags &= ~K_NET_LINK;
        d->info.speed_mbps = (d->kind == NIC_IGC && (st & (1u << 22))) ? 2500
                             : (st & 128)                              ? 1000
                             : (st & 64)                               ? 100
                                                                       : 10;
    }
    BOOLEAN adv = d->kind == NIC_IGC;
    for (UINT32 budget = 0; budget < 64; ++budget) {
        UINT32 index = d->rx_head;
        UINT8 *r = d->rx_ring + index * 16;
        UINT32 status = adv ? *(volatile UINT32 *)(r + 8) : *(volatile UINT8 *)(r + 12);
        if (!(status & 1))
            break;
        arch_read_fence();
        UINT32 n = adv ? rd16(r + 12) : rd16(r + 8);
        ++d->diag.rx_completed;
        d->diag.last_rx_status = status;
        d->diag.last_rx_length = n;
        BOOLEAN bad = adv ? (status & 0xff000000u) != 0 : r[13] != 0;
        if (!(status & 2) || d->discarding) {
            d->discarding = !(status & 2);
            ++d->info.dropped;
        } else if (bad || n < 14 || n > K_NET_FRAME_MAX)
            ++d->info.errors;
        else
            net_enqueue(d->info.id, d->rx_data + index * 2048, n);
        wr64(r, (UINT64)(UINTN)(d->rx_data + index * 2048));
        wr64(r + 8, 0);
        nic_fence(d);
        nic_put(d, adv ? 0xc018 : 0x2818, index);
        d->rx_head = (index + 1) % 64;
    }
    while (d->tx_used) {
        UINT32 index = d->tx_clean;
        UINT8 status = *(volatile UINT8 *)(d->tx_ring + index * 16 + 12);
        if (!(status & 1)) {
            if (now - d->tx_when[index] > 5000) {
                nic_stage(d, "TX completion");
                nic_fail(d, K_ETIMEDOUT);
            }
            break;
        }
        arch_read_fence();
        ++d->diag.tx_completed;
        d->tx_clean = (index + 1) % 64;
        --d->tx_used;
    }
}
void nic_intel_kick(net_device *d, UINT8 *data, UINT32 wire, UINT32 index)
{
    UINT8 *r = d->tx_ring + index * 16;
    wr64(r, (UINT64)(UINTN)data);
    if (d->kind == NIC_IGC) {
        wr32(r + 8, wire | 0x2b300000u);
        wr32(r + 12, wire << 14);
    } else {
        wr32(r + 8, wire | 0x0b000000u);
        wr32(r + 12, 0);
    }
    nic_fence(d);
    d->tx_tail = (index + 1) % 64;
    nic_put(d, d->kind == NIC_IGC ? 0xe018 : 0x3818, d->tx_tail);
}
