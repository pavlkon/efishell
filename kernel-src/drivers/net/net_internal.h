// SPDX-License-Identifier: GPL-2.0-only
#ifndef NET_INTERNAL_H
#define NET_INTERNAL_H
#include "kernel_internal.h"
/* Private to the network subsystem; callers hold the network mutex. */
enum { NIC_LOOP, NIC_E1000, NIC_IGC, NIC_RTL8139, NIC_EXTERNAL };
typedef struct {
    k_net_info info;
    k_net_diag diag;
    k_net_driver external;
    void *context;
    UINT32 kind, rx_head, tx_tail, tx_clean, tx_used;
    volatile UINT8 *regs;
    UINT16 io, bdf;
    UINT8 *rx_ring, *tx_ring, *rx_data, *tx_data;
    UINT64 tx_when[64], link_check, dad_at, config_until, router_until, prefix_until, dns_until;
    UINT32 dad4, dad6, dadglobal, auto_rs;
    UINT64 probe_at, icmp_at, rx_wait_until;
    UINT32 icmp_budget;
    BOOLEAN dead, discarding;
    k_net_frame capture[8];
    UINT32 capture_head, capture_count, capture_owner;
} net_device;
static inline UINT32 nic_get(net_device *d, UINT32 reg)
{
    return *(volatile UINT32 *)(d->regs + reg);
}
static inline void nic_put(net_device *d, UINT32 reg, UINT32 value)
{
    *(volatile UINT32 *)(d->regs + reg) = value;
}

static inline void nic_fence(net_device *d)
{
    arch_write_fence();
    if (d->regs)
        (void)nic_get(d, 8);
}

static inline BOOLEAN nic_valid_mac(const UINT8 mac[6])
{
    UINT8 any = 0;
    for (UINT32 i = 0; i < 6; ++i)
        any |= mac[i];
    return any && !(mac[0] & 1);
}
int net_enqueue(UINT32 id, const void *bytes, UINT32 length);
void nic_stage(net_device *, const char *);
void nic_snapshot(net_device *);
int nic_wait(net_device *, UINT32 reg, UINT32 mask, UINT32 expected, UINT32 us);
void nic_fail(net_device *, int);
void nic_pci_prepare(UINT16 bdf, BOOLEAN io);
int nic_start(net_device *);
void nic_poll(net_device *);
int nic_transmit(net_device *, const void *, UINT32);
int nic_intel_start(net_device *, BOOLEAN advanced);
void nic_intel_poll(net_device *);
void nic_intel_kick(net_device *, UINT8 *data, UINT32 wire, UINT32 index);
int nic_rtl_start(net_device *);
void nic_rtl_poll(net_device *);
void nic_rtl_kick(net_device *, UINT32 wire, UINT32 index);
#endif
