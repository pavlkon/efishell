// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"

int x86_64_power_get(k_power_info *out)
{
    if (!out)
        return K_EINVAL;
    k_cpu_features cpu;
    x86_64_cpu_features_get(&cpu);
    mem_zero(out, sizeof(*out));
    out->cpu = cpu.cpu;
    out->features = cpu.features;
    out->temperature_millic = (-2147483647 - 1);
    UINT64 irq = x86_64_irq_save(), v, w;
    if (cpu.features & K_CPU_APERF) {
        if (x86_64_try_rdmsr(0xe8, &out->actual_cycles) ||
            x86_64_try_rdmsr(0xe7, &out->reference_cycles))
            out->features &= ~K_CPU_APERF;
    }
    if (cpu.features & K_CPU_THERMAL) {
        if (!x86_64_try_rdmsr(0x19c, &v) && (v & (1ULL << 31)) && !x86_64_try_rdmsr(0x1a2, &w) &&
            ((w >> 16) & 255)) {
            out->temperature_millic = ((INT32)((w >> 16) & 255) - (INT32)((v >> 16) & 127)) * 1000;
            out->throttled = (v & 1) != 0;
        } else
            out->features &= ~K_CPU_THERMAL;
    }
    if (cpu.features & K_CPU_HWP) {
        if (!x86_64_try_rdmsr(0x770, &v) && (v & 1) && !x86_64_try_rdmsr(0x771, &v) &&
            !x86_64_try_rdmsr(0x774, &w)) {
            out->minimum_level = (v >> 24) & 255;
            out->maximum_level = v & 255;
            out->current_level = (w >> 16) & 255;
            out->preference = (w >> 24) & 255;
        } else
            out->features &= ~K_CPU_HWP;
    }
    if (cpu.features & K_CPU_AMD_PSTATE) {
        if (!x86_64_try_rdmsr(0xc0010061, &v) && !x86_64_try_rdmsr(0xc0010063, &w)) {
            out->minimum_level = 0;
            out->maximum_level = (v >> 4) & 7;
            out->current_level = w & 7;
        } else
            out->features &= ~K_CPU_AMD_PSTATE;
    }
    x86_64_irq_restore(irq);
    return 0;
}
int x86_64_power_set(UINT32 minimum, UINT32 maximum, UINT32 preference)
{
    k_power_info p;
    x86_64_power_get(&p);
    UINT64 irq = x86_64_irq_save(), request, readback;
    int e = K_ENOTSUP;
    if ((p.features & K_CPU_HWP) && minimum >= p.minimum_level && maximum <= p.maximum_level &&
        minimum <= maximum && preference <= 255) {
        UINT32 a, b, c, d;
        x86_64_cpuid(6, 0, &a, &b, &c, &d);
        if (!(a & (1u << 10)) && preference)
            goto out;
        e = x86_64_try_rdmsr(0x774, &request);
        if (e)
            goto out;
        request = (request & ~0xffffffffULL) | minimum | ((UINT64)maximum << 8) |
                  ((UINT64)preference << 24);
        e = x86_64_try_wrmsr(0x774, request);
        if (!e)
            e = x86_64_try_rdmsr(0x774, &readback);
        if (!e && (readback & 0xffffffff) != (request & 0xffffffff))
            e = K_EIO;
    } else if ((p.features & K_CPU_AMD_PSTATE) && minimum == maximum &&
               maximum <= p.maximum_level && !preference) {
        e = x86_64_try_rdmsr(0xc0010064 + maximum, &request);
        if (!e && !(request & (1ULL << 63)))
            e = K_EINVAL;
        if (!e)
            e = x86_64_try_wrmsr(0xc0010062, maximum);
    } else if (p.features & (K_CPU_HWP | K_CPU_AMD_PSTATE))
        e = K_EINVAL;
out:
    x86_64_irq_restore(irq);
    return e;
}
