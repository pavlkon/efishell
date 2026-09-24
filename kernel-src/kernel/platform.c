// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"

int k_platform_init(UINT64 rsdp, UINT64 trampoline) { return arch_platform_init(rsdp, trampoline); }

const k_platform_info *k_platform(void) { return arch_platform(); }

UINT32 k_cpu_id(void) { return arch_cpu_id(); }

void k_delay_us(UINT32 us) { arch_delay_us(us); }

int k_timer_init(UINT32 hz) { return arch_timer_init(hz); }

UINT64 k_ticks(void) { return arch_ticks(); }

UINT64 k_uptime_ms(void) { return arch_uptime_ms(); }

UINT32 k_tick_hz(void) { return arch_tick_hz(); }

int k_cpu_get(UINT32 i, k_cpu_info *out) { return arch_cpu_get(i, out); }

void kernel_interrupts_init(void) { arch_interrupts_init(); }

void kernel_interrupts_enable(void) { arch_interrupts_enable(); }

int k_smp_start(void) { return arch_smp_start(); }
