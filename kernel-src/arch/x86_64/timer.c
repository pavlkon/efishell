// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"

static volatile UINT8 *clock_mmio;
static UINT64 clock_period, clock_mask, clock_last, clock_ns, clock_fraction;
static UINT32 apic_period;
static BOOLEAN clock_ready, tickless;
static k_spinlock clock_lock = K_SPINLOCK_INIT;
void x86_64_timer_clock_init(volatile UINT8 *hpet, UINT64 period, UINT64 mask, UINT32 lapic_ticks)
{
    apic_period = lapic_ticks;
    /* A 32-bit counter must not wrap within the maximum idle interval. */
    if (!hpet || !period || (mask != ~0ULL && mask * period < 1000000000000000ULL))
        return;
    clock_mmio = hpet;
    clock_period = period;
    clock_mask = mask;
    clock_last = *(volatile UINT64 *)(hpet + 0xf0) & mask;
    clock_ns = 0;
    clock_fraction = 0;
    clock_ready = TRUE;
    tickless = TRUE;
}
static UINT64 elapsed_ns(void)
{
    UINT64 irq = k_spin_lock(&clock_lock),
           now = *(volatile UINT64 *)(clock_mmio + 0xf0) & clock_mask;
    UINT64 delta = (now - clock_last) & clock_mask;
    clock_last = now;
    UINT64 whole = delta / 1000000, part = (delta % 1000000) * clock_period + clock_fraction;
    clock_ns += whole * clock_period + part / 1000000;
    clock_fraction = part % 1000000;
    UINT64 result = clock_ns;
    k_spin_unlock(&clock_lock, irq);
    return result;
}
UINT64 x86_64_clock_ticks(void)
{
    if (!clock_ready)
        return __atomic_load_n(&global_ticks, __ATOMIC_RELAXED);
    UINT64 n = elapsed_ns();
    return n / 1000000000 * timer_hz + (n % 1000000000) * timer_hz / 1000000000;
}
UINT64 x86_64_clock_ms(void)
{
    if (clock_ready)
        return elapsed_ns() / 1000000;
    UINT64 t = __atomic_load_n(&global_ticks, __ATOMIC_RELAXED);
    return (t / timer_hz) * 1000 + (t % timer_hz) * 1000 / timer_hz;
}
BOOLEAN x86_64_tickless_enabled(void) { return __atomic_load_n(&tickless, __ATOMIC_ACQUIRE); }
int x86_64_tickless_set(BOOLEAN enabled)
{
    if (!clock_ready || !platform.apic || !apic_period)
        return K_ENOTSUP;
    __atomic_store_n(&tickless, enabled != 0, __ATOMIC_RELEASE);
    for (UINT32 c = 0; c < platform.discovered_cpus; ++c)
        x86_64_wake_cpu(c);
    k_yield();
    return 0;
}
void x86_64_timer_arm(BOOLEAN idle, UINT64 deadline)
{
    if (!clock_ready || !apic_period || !platform.apic)
        return;
    if (!x86_64_tickless_enabled()) {
        x86_64_lapic_write(0x320, 0x20000 | 0xf0);
        x86_64_lapic_write(0x380, apic_period);
        return;
    }
    UINT64 ticks = 1, now = x86_64_clock_ticks();
    if (idle) {
        ticks = timer_hz / 10;
        if (!ticks)
            ticks = 1;
        if (deadline != ~0ULL)
            ticks = deadline > now ? MIN(ticks, deadline - now) : 1;
    }
    UINT64 count = ticks * apic_period;
    if (count > 0xffffffff)
        count = 0xffffffff;
    x86_64_lapic_write(0x320, 0xf0);
    x86_64_lapic_write(0x380, (UINT32)count);
}
void x86_64_timer_account(UINT32 cpu)
{
    ++cpus[cpu].ticks;
    if (!cpu && !clock_ready)
        __atomic_add_fetch(&global_ticks, 1, __ATOMIC_RELAXED);
}
