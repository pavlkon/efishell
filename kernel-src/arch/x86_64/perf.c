// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"

typedef struct {
    BOOLEAN active, global;
    UINT32 owner, select[2], counter[2], width;
    UINT64 old_select[2], old_counter[2], old_global;
} perf_state;
static perf_state counters[K_MAX_CPUS];
static int restore(perf_state *p)
{
    int result = 0;
    for (UINT32 i = 0; i < 2; ++i) {
        if (x86_64_try_wrmsr(p->select[i], 0))
            result = K_EIO;
        if (x86_64_try_wrmsr(p->counter[i], p->old_counter[i]))
            result = K_EIO;
        if (x86_64_try_wrmsr(p->select[i], p->old_select[i]))
            result = K_EIO;
    }
    if (p->global && x86_64_try_wrmsr(0x38f, p->old_global))
        result = K_EIO;
    return result;
}
int x86_64_perf_start(void)
{
    UINT64 irq = x86_64_irq_save();
    k_cpu_features f;
    x86_64_cpu_features_get(&f);
    perf_state *p = &counters[f.cpu];
    int e = K_ENOTSUP;
    UINT32 event[2];
    if (p->active) {
        e = K_EBUSY;
        goto out;
    }
    mem_zero(p, sizeof(*p));
    if (str_eq(f.vendor, "GenuineIntel")) {
        UINT32 a, b, c, d, max;
        x86_64_cpuid(0, 0, &max, &b, &c, &d);
        if (max < 10)
            goto out;
        x86_64_cpuid(10, 0, &a, &b, &c, &d);
        if (!(a & 255) || ((a >> 8) & 255) < 2 || ((a >> 24) & 255) < 2 || (b & 3))
            goto out;
        p->width = (a >> 16) & 255;
        if (!p->width || p->width > 63)
            goto out;
        p->select[0] = 0x186;
        p->select[1] = 0x187;
        p->counter[0] = 0xc1;
        p->counter[1] = 0xc2;
        p->global = (a & 255) >= 2;
        event[0] = 0x3c;
        event[1] = 0xc0;
    } else if (str_eq(f.vendor, "AuthenticAMD") && f.family >= 0x10 && f.family <= 0x19) {
        p->width = 48;
        p->select[0] = 0xc0010000;
        p->select[1] = 0xc0010001;
        p->counter[0] = 0xc0010004;
        p->counter[1] = 0xc0010005;
        event[0] = 0x76;
        event[1] = 0xc0;
    } else
        goto out;
    for (UINT32 i = 0; i < 2; ++i) {
        e = x86_64_try_rdmsr(p->select[i], &p->old_select[i]);
        if (e)
            goto out;
        if (p->old_select[i] & (1u << 22)) {
            e = K_EBUSY;
            goto out;
        }
        e = x86_64_try_rdmsr(p->counter[i], &p->old_counter[i]);
        if (e)
            goto out;
    }
    if (p->global) {
        e = x86_64_try_rdmsr(0x38f, &p->old_global);
        if (e)
            goto out;
    }
    for (UINT32 i = 0; i < 2; ++i) {
        e = x86_64_try_wrmsr(p->counter[i], 0);
        if (e)
            goto rollback;
        e = x86_64_try_wrmsr(p->select[i], event[i] | (1u << 16) | (1u << 17) | (1u << 22));
        if (e)
            goto rollback;
    }
    if (p->global) {
        e = x86_64_try_wrmsr(0x38f, p->old_global | 3);
        if (e)
            goto rollback;
    }
    p->owner = current_task() ? current_task()->id : 0;
    p->active = TRUE;
    e = 0;
    goto out;
rollback:
    (void)restore(p);
out:
    x86_64_irq_restore(irq);
    return e;
}
int x86_64_perf_read(k_perf_info *out)
{
    if (!out)
        return K_EINVAL;
    UINT64 irq = x86_64_irq_save();
    UINT32 cpu = x86_64_cpu_id();
    perf_state *p = &counters[cpu];
    mem_zero(out, sizeof(*out));
    out->cpu = cpu;
    out->active = p->active;
    out->width = p->width;
    int e = p->active ? 0 : K_ENOENT;
    if (!e)
        e = x86_64_try_rdmsr(p->counter[0], &out->cycles);
    if (!e)
        e = x86_64_try_rdmsr(p->counter[1], &out->instructions);
    if (!e) {
        UINT64 mask = (1ULL << p->width) - 1;
        out->cycles &= mask;
        out->instructions &= mask;
    }
    x86_64_irq_restore(irq);
    return e;
}
int x86_64_perf_stop(void)
{
    UINT64 irq = x86_64_irq_save();
    perf_state *p = &counters[x86_64_cpu_id()];
    int e = K_ENOENT;
    if (p->active) {
        e = restore(p);
        if (!e)
            p->active = FALSE;
    }
    x86_64_irq_restore(irq);
    return e;
}
