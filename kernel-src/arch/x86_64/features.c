// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"

UINT64 x86_64_xstate_mask;
UINT32 x86_64_xstate_size = 512;
static BOOLEAN state_selected;
typedef struct {
    UINT64 instruction, recovery;
    BOOLEAN failed;
} msr_guard;
static msr_guard guards[K_MAX_CPUS];
BOOLEAN x86_64_msr_fixup(x86_64_irq_frame *f)
{
    msr_guard *g = &guards[x86_64_cpu_id()];
    if ((f->cs & 3) || !g->instruction || f->rip != g->instruction)
        return FALSE;
    g->failed = TRUE;
    f->rip = g->recovery;
    return TRUE;
}
int x86_64_try_rdmsr(UINT32 msr, UINT64 *value)
{
    if (!value)
        return K_EINVAL;
    UINT64 irq = x86_64_irq_save();
    msr_guard *g = &guards[x86_64_cpu_id()];
    if (g->instruction) {
        x86_64_irq_restore(irq);
        return K_EBUSY;
    }
    UINT32 lo = 0, hi = 0;
    g->failed = FALSE;
    __asm__ volatile("leaq 1f(%%rip),%%rax;movq %%rax,%2;leaq 2f(%%rip),%%rax;movq %%rax,%3;"
                     "1: rdmsr;2:"
                     : "=&a"(lo), "=&d"(hi), "=m"(g->instruction), "=m"(g->recovery)
                     : "c"(msr)
                     : "memory");
    int e = g->failed ? K_ENOTSUP : 0;
    g->instruction = 0;
    if (!e)
        *value = ((UINT64)hi << 32) | lo;
    x86_64_irq_restore(irq);
    return e;
}
int x86_64_try_wrmsr(UINT32 msr, UINT64 value)
{
    UINT64 irq = x86_64_irq_save();
    msr_guard *g = &guards[x86_64_cpu_id()];
    if (g->instruction) {
        x86_64_irq_restore(irq);
        return K_EBUSY;
    }
    g->failed = FALSE;
    __asm__ volatile("leaq 1f(%%rip),%%r8;movq %%r8,%0;leaq 2f(%%rip),%%r8;movq %%r8,%1;"
                     "1: wrmsr;2:"
                     : "=m"(g->instruction), "=m"(g->recovery)
                     : "c"(msr), "a"((UINT32)value), "d"((UINT32)(value >> 32))
                     : "r8", "memory");
    int e = g->failed ? K_ENOTSUP : 0;
    g->instruction = 0;
    x86_64_irq_restore(irq);
    return e;
}
int x86_64_xstate_enable(void)
{
    UINT32 a, b, c, d, max;
    x86_64_cpuid(0, 0, &max, &b, &c, &d);
    x86_64_cpuid(1, 0, &a, &b, &c, &d);
    UINT64 mask = 0;
    UINT32 bytes = 512;
    if (max >= 13 && (c & (1u << 26))) {
        BOOLEAN avx = (c & (1u << 28)) != 0;
        x86_64_cpuid(13, 0, &a, &b, &c, &d);
        if ((a & 3) == 3) {
            mask = 3;
            if (avx && (a & 4)) {
                x86_64_cpuid(13, 2, &a, &b, &c, &d);
                if (a && b >= 576 && (UINT64)a + b <= 4096)
                    mask |= 4;
            }
        }
    }
    if (state_selected && mask != x86_64_xstate_mask)
        return K_ENOTSUP;
    if (mask) {
        x86_64_write_cr4(x86_64_read_cr4() | (1ULL << 18));
        __asm__ volatile("xsetbv" ::"c"(0), "a"((UINT32)mask), "d"(0) : "memory");
        x86_64_cpuid(13, 0, &a, &b, &c, &d);
        bytes = b;
        if (bytes > 4096 || bytes < 576)
            return K_ENOTSUP;
    }
    if (state_selected && bytes != x86_64_xstate_size)
        return K_ENOTSUP;
    x86_64_xstate_mask = mask;
    x86_64_xstate_size = bytes;
    state_selected = TRUE;
    return 0;
}
int x86_64_cpu_features_get(k_cpu_features *out)
{
    if (!out)
        return K_EINVAL;
    mem_zero(out, sizeof(*out));
    UINT32 a, b, c, d, max, ext;
    x86_64_cpuid(0, 0, &max, &b, &c, &d);
    wr32(out->vendor, b);
    wr32(out->vendor + 4, d);
    wr32(out->vendor + 8, c);
    x86_64_cpuid(1, 0, &a, &b, &c, &d);
    UINT32 fam = (a >> 8) & 15;
    out->family = fam == 15 ? fam + ((a >> 20) & 255) : fam;
    out->model = ((a >> 4) & 15) | ((fam == 6 || fam == 15) ? ((a >> 12) & 240) : 0);
    out->stepping = a & 15;
    out->cpu = x86_64_cpu_id();
    out->xstate_mask = x86_64_xstate_mask;
    out->xstate_bytes = x86_64_xstate_size;
    if (x86_64_xstate_mask)
        out->features |= K_CPU_XSAVE;
    if (x86_64_xstate_mask & 4)
        out->features |= K_CPU_AVX;
    if (max >= 7) {
        x86_64_cpuid(7, 0, &a, &b, &c, &d);
        if ((b & 32) && (out->features & K_CPU_AVX))
            out->features |= K_CPU_AVX2;
    }
    if (max >= 6) {
        x86_64_cpuid(6, 0, &a, &b, &c, &d);
        if (c & 1)
            out->features |= K_CPU_APERF;
        if (str_eq(out->vendor, "GenuineIntel")) {
            if (a & 1)
                out->features |= K_CPU_THERMAL;
            if (a & 128)
                out->features |= K_CPU_HWP;
        }
    }
    x86_64_cpuid(0x80000000, 0, &ext, &b, &c, &d);
    if (ext >= 0x80000004)
        for (UINT32 i = 0; i < 3; ++i) {
            x86_64_cpuid(0x80000002 + i, 0, &a, &b, &c, &d);
            wr32(out->brand + 16 * i, a);
            wr32(out->brand + 16 * i + 4, b);
            wr32(out->brand + 16 * i + 8, c);
            wr32(out->brand + 16 * i + 12, d);
        }
    out->brand[47] = 0;
    if (ext >= 0x80000007) {
        x86_64_cpuid(0x80000007, 0, &a, &b, &c, &d);
        if (d & 256)
            out->features |= K_CPU_INVARIANT_TSC;
        if (str_eq(out->vendor, "AuthenticAMD") && (d & 128))
            out->features |= K_CPU_AMD_PSTATE;
    }
    return 0;
}
int x86_64_vector_test(void)
{
    if (!(x86_64_xstate_mask & 4))
        return K_ENOTSUP;
    x86_64_task_state saved;
    UINT8 pattern[32], after[32];
    /* A fresh save must be restorable even when its storage contains garbage. */
    for (UINT32 i = 0; i < sizeof(saved.fx); ++i)
        saved.fx[i] = 0xa5;
    for (UINT32 i = 0; i < 32; ++i)
        pattern[i] = (UINT8)(i * 7 + 19);
    x86_64_fxsave(&saved);
    const UINT8 *header = saved.fx + X86_64_XSAVE_HEADER_OFFSET;
    if (rd64(header) & ~x86_64_xstate_mask)
        return K_EFAULT;
    for (UINT32 i = 8; i < X86_64_XSAVE_HEADER_SIZE; ++i)
        if (header[i])
            return K_EFAULT;
    __asm__ volatile("vmovdqu %0,%%ymm0" ::"m"(pattern) : "memory");
    for (UINT32 i = 0; i < 32; ++i)
        k_yield();
    __asm__ volatile("vmovdqu %%ymm0,%0" : "=m"(after)::"memory");
    x86_64_fxrestore(&saved);
    return memcmp(pattern, after, 32) ? K_EFAULT : 0;
}
