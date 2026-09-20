// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"
#include "x86_64_internal.h"

BOOLEAN interrupts_ready, smp_started;
UINT32 timer_hz = 100;
static UINT64 ap_trampoline, tsc_hz;
UINT64 saved_pat, saved_mtrr_default, saved_mtrr_fixed[11], saved_mtrr_variable[64];
UINT32 saved_mtrr_count, saved_phys_bits;
BOOLEAN has_pat, has_mtrr, mtrr_fixed_supported;
static UINT32 lapic_period;
static volatile UINT8 *lapic_regs;
k_platform_info platform = {.ps2 = TRUE,
                            .pic = TRUE,
                            .dma_allowed = TRUE,
                            .clock_source = "unavailable",
                            .timer_source = "off",
                            .dma_reason = "no active remapping detected"};
static x86_64_idt_entry idt[256] __attribute__((aligned(16)));
static x86_64_descriptor_ptr idtr;
/* Validate ACPI length/checksum before interpreting a table. */
static volatile UINT8 *hpet;
static UINT64 hpet_period_fs, hpet_mask = ~0ULL;
static UINT16 pm_timer_port;
static UINT32 pm_timer_mask = 0xffffff;
static struct {
    UINT32 id, gsi, count;
    volatile UINT32 *regs;
} ioapics[8];
static UINT32 ioapic_count, isa_input_gsi[2] = {1, 12};
static UINT16 isa_input_flags[2];
BOOLEAN pic_timer;
static BOOLEAN physical_described(UINT64 p, UINT64 n)
{
    if (!n || p >= K_USER_BASE || n > K_USER_BASE - p)
        return FALSE;
    for (UINTN i = 0; i < boot_entries; ++i) {
        EFI_MEMORY_DESCRIPTOR *d = (void *)((UINT8 *)boot_map + i * boot_desc_size);
        if (d->Type == EfiUnusableMemory || d->Type == EfiMemoryMappedIOPortSpace)
            continue;
        if (d->NumberOfPages > ~0ULL / 4096)
            continue;
        UINT64 bytes = d->NumberOfPages * 4096;
        if (p >= d->PhysicalStart && span_ok(p - d->PhysicalStart, n, bytes))
            return TRUE;
    }
    return FALSE;
}
static BOOLEAN checksum(const void *ptr, UINT32 n)
{
    const UINT8 *p = ptr;
    UINT8 sum = 0;
    while (n--)
        sum += *p++;
    return sum == 0;
}
static const UINT8 *acpi_table(UINT64 p)
{
    if (!physical_described(p, 36))
        return NULL;
    const UINT8 *t = (void *)(UINTN)p;
    UINT32 n = rd32(t + 4);
    if (n < 36 || n > 1024 * 1024 || !physical_described(p, n) || !checksum(t, n))
        return NULL;
    return t;
}
static UINT32 ioapic_read(UINT32 i, UINT8 r)
{
    ioapics[i].regs[0] = r;
    return ioapics[i].regs[4];
}
static void ioapic_write(UINT32 i, UINT8 r, UINT32 v)
{
    ioapics[i].regs[0] = r;
    ioapics[i].regs[4] = v;
}
static void add_cpu(UINT32 id)
{
    for (UINT32 i = 0; i < platform.discovered_cpus; ++i)
        if (cpus[i].apic_id == id)
            return;
    UINT32 n = platform.discovered_cpus;
    if (n < K_MAX_CPUS) {
        cpus[n].index = n;
        cpus[n].apic_id = id;
        ++platform.discovered_cpus;
    }
}
static void parse_madt(const UINT8 *t)
{
    UINT32 n = rd32(t + 4);
    if (n < 44)
        return;
    platform.pic = (rd32(t + 40) & 1) != 0;
    UINT64 base = rd32(t + 36);
    for (UINT32 p = 44; p + 2 <= n;) {
        UINT8 type = t[p], len = t[p + 1];
        if (len < 2 || len > n - p)
            break;
        const UINT8 *e = t + p;
        if (type == 0 && len >= 8 && (rd32(e + 4) & 1))
            add_cpu(e[3]);
        if (type == 9 && len >= 16 && (rd32(e + 8) & 1))
            add_cpu(rd32(e + 4));
        if (type == 5 && len >= 12)
            base = rd64(e + 4);
        if (type == 1 && len >= 12 && ioapic_count < ARRAY_LEN(ioapics)) {
            UINT64 a = rd32(e + 4);
            if (!k_mmio_map(a, 4096)) {
                UINT32 i = ioapic_count++;
                ioapics[i].regs = (void *)(UINTN)a;
                ioapics[i].id = e[2];
                ioapics[i].gsi = rd32(e + 8);
                ioapics[i].count = ((ioapic_read(i, 1) >> 16) & 0xff) + 1;
                if (ioapics[i].count > 120)
                    ioapics[i].count = 120;
            }
        }
        if (type == 2 && len >= 10 && e[2] == 0 && (e[3] == 1 || e[3] == 12)) {
            UINT32 channel = e[3] == 12;
            isa_input_gsi[channel] = rd32(e + 4);
            isa_input_flags[channel] = rd16(e + 8);
        }
        p += len;
    }
    if (!platform.x2apic && base && !k_mmio_map(base, 4096))
        lapic_regs = (void *)(UINTN)base;
}
static void parse_fadt(const UINT8 *t)
{
    UINT32 n = rd32(t + 4);
    if (n < 116)
        return;
    if (t[8] >= 2)
        platform.ps2 = (rd16(t + 109) & 2) != 0;
    UINT32 flags = rd32(t + 112);
    if (flags & (1u << 20)) {
        platform.pic = FALSE;
        platform.ps2 = FALSE;
        return;
    }
    UINT64 p = rd32(t + 76);
    if (n >= 220 && t[208] == 1 && rd64(t + 212))
        p = rd64(t + 212);
    if (p && p <= 65535 && t[91] == 4) {
        pm_timer_port = (UINT16)p;
        pm_timer_mask = (flags & (1 << 8)) ? 0xffffffff : 0xffffff;
    }
}
static void parse_hpet(const UINT8 *t)
{
    if (rd32(t + 4) < 56 || t[40] != 0)
        return;
    UINT64 p = rd64(t + 44);
    if (k_mmio_map(p, 4096))
        return;
    volatile UINT8 *base = (void *)(UINTN)p;
    UINT64 caps = *(volatile UINT64 *)base, period = caps >> 32;
    if (!period || period > 100000000ULL)
        return;
    hpet = base;
    hpet_period_fs = period;
    hpet_mask = (caps & (1 << 13)) ? ~0ULL : 0xffffffffULL;
    /* HPET main counter only; no legacy replacement/comparator IRQs. */
    UINT64 cfg = *(volatile UINT64 *)(hpet + 0x10);
    *(volatile UINT64 *)(hpet + 0x10) = (cfg & ~2ULL) | 1;
    UINT64 before = *(volatile UINT64 *)(hpet + 0xf0);
    for (UINT32 i = 0; i < 100000; ++i) {
        if (*(volatile UINT64 *)(hpet + 0xf0) != before)
            return;
        arch_pause();
    }
    hpet = NULL;
}
static void parse_dmar(const UINT8 *t)
{
    UINT32 n = rd32(t + 4);
    if (n < 48) {
        platform.dma_allowed = FALSE;
        platform.dma_reason = "invalid Intel DMAR table";
        return;
    }
    for (UINT32 p = 48; p + 4 <= n;) {
        UINT16 type = rd16(t + p), len = rd16(t + p + 2);
        if (len < 4 || len > n - p || (type == 0 && len < 16)) {
            platform.dma_allowed = FALSE;
            platform.dma_reason = "invalid Intel DMAR unit";
            break;
        }
        if (type == 0 && len >= 16) {
            UINT64 base = rd64(t + p + 8);
            if (k_mmio_map(base, 4096) || (*(volatile UINT32 *)(UINTN)(base + 0x1c) & (1U << 31))) {
                platform.dma_allowed = FALSE;
                platform.dma_reason = "active or inaccessible Intel VT-d unit";
            }
        }
        p += len;
    }
}
static void parse_ivrs(const UINT8 *t)
{
    UINT32 n = rd32(t + 4);
    if (n < 48) {
        platform.dma_allowed = FALSE;
        platform.dma_reason = "invalid AMD IVRS table";
        return;
    }
    for (UINT32 p = 48; p + 4 <= n;) {
        UINT16 len = rd16(t + p + 2);
        if (len < 4 || len > n - p ||
            ((t[p] == 0x10 || t[p] == 0x11 || t[p] == 0x40) && len < 24)) {
            platform.dma_allowed = FALSE;
            platform.dma_reason = "invalid AMD IVRS unit";
            break;
        }
        if ((t[p] == 0x10 || t[p] == 0x11 || t[p] == 0x40) && len >= 24) {
            UINT64 base = rd64(t + p + 8) & ~0x3fffULL;
            if (k_mmio_map(base, 16384) || (*(volatile UINT64 *)(UINTN)(base + 0x18) & 1)) {
                platform.dma_allowed = FALSE;
                platform.dma_reason = "active or inaccessible AMD IOMMU unit";
            }
        }
        p += len;
    }
}
int x86_64_platform_init(UINT64 rsdp, UINT64 trampoline)
{
    UINT32 a, b, c, d;
    arch_cpuid(1, 0, &a, &b, &c, &d);
    BOOLEAN acpi_complete = FALSE;
    platform.apic = (d & (1 << 9)) != 0;
    UINT32 bsp = b >> 24;
    if (platform.apic) {
        UINT64 apic_base = arch_rdmsr(0x1b);
        platform.x2apic = (apic_base & (1 << 10)) != 0;
        if (platform.x2apic)
            bsp = (UINT32)arch_rdmsr(0x802);
        else {
            UINT64 base = apic_base & 0x000ffffffffff000ULL;
            if (k_mmio_map(base, 4096))
                platform.apic = FALSE;
            else {
                lapic_regs = (void *)(UINTN)base;
                bsp = x86_64_lapic_read(0x20) >> 24;
            }
        }
    }
    cpus[0].index = 0;
    cpus[0].apic_id = bsp;
    cpus[0].online = 1;
    platform.discovered_cpus = platform.online_cpus = 1;
    ap_trampoline = trampoline;
    if (rsdp && physical_described(rsdp, 20)) {
        const UINT8 *r = (void *)(UINTN)rsdp;
        if (!memcmp(r, "RSD PTR ", 8) && checksum(r, 20)) {
            BOOLEAN extended = r[15] >= 2 && physical_described(rsdp, 36);
            if (extended) {
                UINT32 length = rd32(r + 20);
                extended = length >= 36 && length <= 4096 && physical_described(rsdp, length) &&
                           checksum(r, length);
            }
            UINT64 address = extended ? rd64(r + 24) : rd32(r + 16);
            const UINT8 *root = acpi_table(address);
            UINT32 step = extended ? 8 : 4;
            if (root && !memcmp(root, extended ? "XSDT" : "RSDT", 4) &&
                !((rd32(root + 4) - 36) % step)) {
                acpi_complete = TRUE;
                for (UINT32 p = 36; p + step <= rd32(root + 4); p += step) {
                    const UINT8 *t = acpi_table(step == 8 ? rd64(root + p) : rd32(root + p));
                    if (!t) {
                        acpi_complete = FALSE;
                        continue;
                    }
                    if (!memcmp(t, "APIC", 4))
                        parse_madt(t);
                    else if (!memcmp(t, "FACP", 4))
                        parse_fadt(t);
                    else if (!memcmp(t, "HPET", 4))
                        parse_hpet(t);
                    else if (!memcmp(t, "DMAR", 4))
                        parse_dmar(t);
                    else if (!memcmp(t, "IVRS", 4))
                        parse_ivrs(t);
                }
            }
        }
    }
    if (!acpi_complete) {
        platform.dma_allowed = FALSE;
        platform.dma_reason = "incomplete ACPI; DMA translation state unknown";
    }
    arch_cpuid(0, 0, &a, &b, &c, &d);
    UINT32 maxleaf = a;
    if (maxleaf >= 0x15) {
        arch_cpuid(0x15, 0, &a, &b, &c, &d);
        if (a && b && c)
            tsc_hz = (UINT64)c * b / a;
    }
    platform.clock_source = hpet            ? "HPET"
                            : pm_timer_port ? "ACPI PM timer"
                            : tsc_hz        ? "CPUID TSC"
                                            : "PIT channel 2";
    arch_cpuid(0, 0, &a, &b, &c, &d);
    if (a >= 7) {
        arch_cpuid(7, 0, &a, &b, &c, &d);
        platform.smep = (b & (1 << 7)) != 0;
    }
    return 0;
}
const k_platform_info *x86_64_platform(void) { return &platform; }
UINT32 x86_64_lapic_read(UINT32 off)
{
    return platform.x2apic ? (UINT32)arch_rdmsr(0x800 + (off >> 4))
                           : *(volatile UINT32 *)(lapic_regs + off);
}
void x86_64_lapic_write(UINT32 off, UINT32 value)
{
    if (platform.x2apic)
        arch_wrmsr(0x800 + (off >> 4), value);
    else {
        *(volatile UINT32 *)(lapic_regs + off) = value;
        (void)*(volatile UINT32 *)(lapic_regs + 0x20);
    }
}
void x86_64_lapic_eoi(void)
{
    if (platform.apic)
        x86_64_lapic_write(0xb0, 0);
}
UINT32 x86_64_cpu_id(void)
{
    if (!platform.apic || !interrupts_ready)
        return 0;
    UINT32 id = platform.x2apic ? x86_64_lapic_read(0x20) : x86_64_lapic_read(0x20) >> 24;
    for (UINT32 i = 0; i < platform.discovered_cpus; ++i)
        if (cpus[i].apic_id == id)
            return i;
    return 0;
}
/* Hardware waits have both a time deadline and finite iteration bound. */
static int pit_delay(UINT32 us)
{
    if (!platform.pic)
        return K_ENOTSUP;
    while (us) {
        UINT32 part = MIN(us, 50000U);
        UINT16 count = (UINT16)(((UINT64)part * 1193182 + 999999) / 1000000);
        UINT8 old = arch_in8(0x61);
        arch_out8(0x61, (old & ~3u));
        arch_out8(0x43, 0xb0);
        arch_out8(0x42, (UINT8)count);
        arch_out8(0x42, (UINT8)(count >> 8));
        arch_out8(0x61, (old & ~2u) | 1);
        UINT32 loops = 10000000;
        while (!(arch_in8(0x61) & 0x20) && --loops)
            arch_pause();
        arch_out8(0x61, old);
        if (!loops)
            return K_ETIMEDOUT;
        us -= part;
    }
    return 0;
}
int x86_64_delay_checked(UINT32 us)
{
    if (!us)
        return 0;
    UINT64 start, target, mask;
    UINT32 loops = 100000000;
    if (hpet) {
        start = *(volatile UINT64 *)(hpet + 0xf0) & hpet_mask;
        target = ((UINT64)us * 1000000000ULL + hpet_period_fs - 1) / hpet_period_fs;
        mask = hpet_mask;
        while ((((*(volatile UINT64 *)(hpet + 0xf0) & mask) - start) & mask) < target && --loops)
            arch_pause();
    } else if (pm_timer_port) {
        start = arch_in32(pm_timer_port) & pm_timer_mask;
        target = ((UINT64)us * 3579545 + 999999) / 1000000;
        mask = pm_timer_mask;
        while (((arch_in32(pm_timer_port) - start) & mask) < target && --loops)
            arch_pause();
    } else if (tsc_hz) {
        start = arch_cycle_count();
        target = (tsc_hz / 1000000) * us;
        while (arch_cycle_count() - start < target && --loops)
            arch_pause();
    } else
        return pit_delay(us);
    return loops ? 0 : K_ETIMEDOUT;
}
void x86_64_delay_us(UINT32 us)
{
    while (us) {
        UINT32 part = MIN(us, 50000U);
        if (x86_64_delay_checked(part))
            break;
        us -= part;
    }
}
BOOLEAN x86_64_wait_reg(volatile UINT32 *reg, UINT32 mask, UINT32 expected, UINT32 milliseconds)
{
    for (UINT32 i = 0; i < milliseconds; ++i) {
        if ((*reg & mask) == expected)
            return TRUE;
        if (x86_64_delay_checked(1000))
            return FALSE;
    }
    return (*reg & mask) == expected;
}
static void lapic_enable(void)
{
    UINT64 base = arch_rdmsr(0x1b);
    base |= 1 << 11;
    if (platform.x2apic)
        base |= 1 << 10;
    arch_wrmsr(0x1b, base);
    x86_64_lapic_write(0x80, 0);
    x86_64_lapic_write(0xf0, 0x100 | 0xff);
    x86_64_lapic_write(0x320, 1 << 16);
    x86_64_lapic_write(0x350, 1 << 16);
    x86_64_lapic_write(0x360, 1 << 16);
    x86_64_lapic_write(0x370, 0xfe);
    x86_64_lapic_write(0x280, 0);
    (void)x86_64_lapic_read(0x280);
    x86_64_lapic_eoi();
}
int x86_64_timer_init(UINT32 hz)
{
    if (!scheduler_ready || hz < 20 || hz > 1000)
        return K_EINVAL;
    timer_hz = hz;
    if (platform.apic) {
        lapic_enable();
        x86_64_lapic_write(0x3e0, 3);
        x86_64_lapic_write(0x380, 0xffffffff);
        int e = x86_64_delay_checked(10000);
        UINT32 elapsed = 0xffffffff - x86_64_lapic_read(0x390);
        x86_64_lapic_write(0x380, 0);
        if (!e && elapsed >= 100) {
            lapic_period = (UINT32)(((UINT64)elapsed * 100) / hz);
            x86_64_lapic_write(0x320, 0x20000 | 0xf0);
            x86_64_lapic_write(0x380, lapic_period);
            platform.timer_source = "local APIC";
            pic_timer = FALSE;
            return 0;
        }
    }
    if (platform.pic) {
        UINT32 div = 1193182 / hz;
        if (div > 65535)
            div = 65535;
        arch_out8(0x43, 0x36);
        arch_out8(0x40, (UINT8)div);
        arch_out8(0x40, (UINT8)(div >> 8));
        arch_out8(0x21, arch_in8(0x21) & ~1u);
        if (platform.apic)
            x86_64_lapic_write(0x350, 0x700);
        pic_timer = TRUE;
        platform.timer_source = "8254 PIT / 8259 PIC";
        return 0;
    }
    platform.timer_source = "unavailable";
    return K_ENOTSUP;
}
UINT64 x86_64_ticks(void) { return __atomic_load_n(&global_ticks, __ATOMIC_RELAXED); }
UINT64 x86_64_uptime_ms(void)
{
    UINT64 t = x86_64_ticks();
    return (t / timer_hz) * 1000 + (t % timer_hz) * 1000 / timer_hz;
}
UINT32 x86_64_tick_hz(void) { return timer_hz; }
int x86_64_cpu_get(UINT32 i, k_cpu_info *out)
{
    if (!out || i >= platform.discovered_cpus)
        return K_ENOENT;
    out->index = i;
    out->apic_id = cpus[i].apic_id;
    out->online = __atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE);
    out->ticks = __atomic_load_n(&cpus[i].ticks, __ATOMIC_RELAXED);
    out->switches = __atomic_load_n(&cpus[i].switches, __ATOMIC_RELAXED);
    out->idle_ticks = __atomic_load_n(&cpus[i].idle_ticks, __ATOMIC_RELAXED);
    return 0;
}

/* IDT stubs normalize error-code frames.
 * In 64-bit mode the CPU pushes SS:RSP even without a CPL change.
 * C calls use Microsoft ABI shadow space on a 16-byte aligned stack. */
extern void *x86_64_irq_stubs[256];
extern void x86_64_syscall_entry(void);

void x86_64_cpu_tables_init(cpu_state *cpu)
{
    mem_zero(&cpu->tables.tss, sizeof(cpu->tables.tss));
    cpu->tables.gdt[0] = 0;
    cpu->tables.gdt[1] = 0x00af9a000000ffffULL;
    cpu->tables.gdt[2] = 0x00cf92000000ffffULL;
    cpu->tables.gdt[3] = 0x00cff2000000ffffULL;
    cpu->tables.gdt[4] = 0x00affa000000ffffULL;
    cpu->tables.tss.iomap = sizeof(x86_64_tss);
    cpu->tables.tss.ist[0] =
        (UINT64)(UINTN)(cpu->tables.fault_stack + sizeof(cpu->tables.fault_stack));
    cpu->tables.tss.ist[1] = (UINT64)(UINTN)(cpu->tables.nmi_stack + sizeof(cpu->tables.nmi_stack));
    cpu->tables.tss.ist[2] =
        (UINT64)(UINTN)(cpu->tables.machine_stack + sizeof(cpu->tables.machine_stack));
    UINT64 base = (UINT64)(UINTN)&cpu->tables.tss, limit = sizeof(x86_64_tss) - 1;
    cpu->tables.gdt[5] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ULL << 40) |
                         ((limit & 0xf0000) << 32) | ((base & 0xff000000) << 32);
    cpu->tables.gdt[6] = base >> 32;
    x86_64_descriptor_ptr gdtr = {sizeof(cpu->tables.gdt) - 1, (UINT64)(UINTN)cpu->tables.gdt};
    arch_load_tables(&gdtr, &idtr);
    UINT64 cr0, cr4;
    cr0 = arch_read_cr0();
    cr0 = (cr0 & ~((1ULL << 2) | (1ULL << 3))) | (1 << 1) | (1 << 16);
    arch_write_cr0((UINT64)(UINTN)cr0);
    cr4 = arch_read_cr4();
    /* x87/SSE are saved eagerly; disable unsaved AVX/FSGSBASE/debug extensions. */
    cr4 &= ~((1ULL << 18) | (1ULL << 16) | (1ULL << 11) | (1ULL << 21));
    cr4 |= (1 << 9) | (1 << 10);
    if (platform.smep)
        cr4 |= 1 << 20;
    arch_write_cr4((UINT64)(UINTN)cr4);
    arch_wrmsr(0xc0000101, (UINT64)(UINTN)cpu);
    arch_wrmsr(0xc0000102, 0);
    arch_wrmsr(0xc0000100, 0);
    arch_wrmsr(0xc0000080, arch_rdmsr(0xc0000080) | 1 | (platform.nx ? (1 << 11) : 0));
    arch_wrmsr(0xc0000081, (0x13ULL << 48) | (8ULL << 32));
    arch_wrmsr(0xc0000082, (UINT64)(UINTN)x86_64_syscall_entry);
    arch_wrmsr(0xc0000084, 0x57700);
    UINT32 a, b, c, d;
    arch_cpuid(1, 0, &a, &b, &c, &d);
    if (d & (1 << 11))
        arch_wrmsr(0x174, 0); /* Unconfigured SYSENTER must fault from ring 3. */
}
void x86_64_interrupts_init(void)
{
    arch_irq_disable();
    for (UINT32 i = 0; i < 256; ++i) {
        UINT64 p = (UINT64)(UINTN)x86_64_irq_stubs[i];
        idt[i] = (x86_64_idt_entry){
            (UINT16)p,         8, 0, (UINT8)(i == 128 ? 0xee : 0x8e), (UINT16)(p >> 16),
            (UINT32)(p >> 32), 0};
    }
    idt[8].ist = 1;
    idt[2].ist = 2;
    idt[18].ist = 3;
    idtr = (x86_64_descriptor_ptr){sizeof(idt) - 1, (UINT64)(UINTN)idt};
    x86_64_cpu_tables_init(&cpus[0]);
    interrupts_ready = TRUE;
    if (platform.pic) {
        arch_out8(0x20, 0x11);
        arch_out8(0xa0, 0x11);
        arch_out8(0x21, 0x20);
        arch_out8(0xa1, 0x28);
        arch_out8(0x21, 4);
        arch_out8(0xa1, 2);
        arch_out8(0x21, 1);
        arch_out8(0xa1, 1);
        arch_out8(0x21, 0xff);
        arch_out8(0xa1, 0xff);
    }
    if (platform.apic) {
        lapic_enable();
        for (UINT32 i = 0; i < ioapic_count; ++i)
            for (UINT32 j = 0; j < ioapics[i].count; ++j)
                ioapic_write(i, (UINT8)(0x10 + 2 * j), 1 << 16);
        if (platform.ps2 && cpus[0].apic_id <= 255) {
            for (UINT32 channel = 0; channel < 2; ++channel)
                for (UINT32 i = 0; i < ioapic_count; ++i) {
                    UINT32 gsi = isa_input_gsi[channel];
                    UINT16 mode = isa_input_flags[channel];
                    if (gsi < ioapics[i].gsi || gsi - ioapics[i].gsi >= ioapics[i].count)
                        continue;
                    UINT8 r = (UINT8)(0x10 + 2 * (gsi - ioapics[i].gsi));
                    UINT32 flags = channel ? 0x2c : 0x21;
                    if ((mode & 3) == 3)
                        flags |= 1 << 13;
                    if (((mode >> 2) & 3) == 3)
                        flags |= 1 << 15;
                    ioapic_write(i, r + 1, cpus[0].apic_id << 24);
                    ioapic_write(i, r, flags);
                }
        }
    } else if (platform.ps2 && platform.pic) {
        arch_out8(0x21, 0xf9);
        arch_out8(0xa1, 0xef);
    }
}
void x86_64_interrupts_enable(void) { arch_irq_enable(); }

/* INIT/SIPI trampoline is copied into an EFI-reserved low page and patched in place. */
extern UINT8 x86_64_ap_blob[], x86_64_ap_blob_end[], x86_64_ap_gdtr[], x86_64_ap_gdt[],
    x86_64_ap_base[], x86_64_ap_pm_far[], x86_64_ap_lm_far[];
extern UINT8 x86_64_ap_cr3[], x86_64_ap_stack[], x86_64_ap_entry[], x86_64_ap_argument[],
    x86_64_ap_efer[], x86_64_ap_pm[], x86_64_ap_lm[];

static int ap_cache_init(void)
{
    UINT32 a, b, c, d;
    arch_cpuid(1, 0, &a, &b, &c, &d);
    if (((d & (1 << 16)) != 0) != has_pat || ((d & (1 << 12)) != 0) != has_mtrr)
        return K_ENOTSUP;
    if (saved_phys_bits) {
        arch_cpuid(0x80000000, 0, &a, &b, &c, &d);
        if (a < 0x80000008)
            return K_ENOTSUP;
        arch_cpuid(0x80000008, 0, &a, &b, &c, &d);
        if ((a & 255) != saved_phys_bits)
            return K_ENOTSUP;
    }
    if (has_mtrr) {
        UINT64 cap = arch_rdmsr(0xfe);
        if ((cap & 255) != saved_mtrr_count || (((cap & (1 << 8)) != 0) != mtrr_fixed_supported))
            return K_ENOTSUP;
    }
    UINT64 cr0;
    cr0 = arch_read_cr0();
    UINT64 uncached = (cr0 | (1ULL << 30)) & ~(1ULL << 29);
    arch_cache_disable(uncached);
    if (has_mtrr) {
        arch_wrmsr(0x2ff, saved_mtrr_default & ~(1ULL << 11));
        if (mtrr_fixed_supported) {
            static const UINT16 msrs[] = {0x250, 0x258, 0x259, 0x268, 0x269, 0x26a,
                                          0x26b, 0x26c, 0x26d, 0x26e, 0x26f};
            for (UINT32 i = 0; i < 11; ++i)
                arch_wrmsr(msrs[i], saved_mtrr_fixed[i]);
        }
        for (UINT32 i = 0; i < saved_mtrr_count * 2; ++i)
            arch_wrmsr(0x200 + i, saved_mtrr_variable[i]);
    }
    if (has_pat)
        arch_wrmsr(0x277, saved_pat);
    if (has_mtrr)
        arch_wrmsr(0x2ff, saved_mtrr_default);
    arch_cache_flush_root((UINT64)(UINTN)kernel_pml4);
    cr0 &= ~((1ULL << 30) | (1ULL << 29));
    arch_write_cr0((UINT64)(UINTN)cr0);
    return 0;
}
__attribute__((noreturn)) static void ap_main(cpu_state *cpu)
{
    if (ap_cache_init())
        for (;;)
            arch_stop();
    x86_64_cpu_tables_init(cpu);
    lapic_enable();
    arch_fxrestore((const x86_64_task_state *)initial_fx);
    cpu->current = cpu->idle;
    cpu->stack_owner = cpu->idle;
    cpu->idle->state = K_TASK_RUNNING;
    cpu->tables.tss.rsp[0] = cpu->idle->stack_top;
    cpu->entry.syscall_stack = cpu->idle->stack_top;
    x86_64_lapic_write(0x3e0, 3);
    x86_64_lapic_write(0x320, 0x20000 | 0xf0);
    x86_64_lapic_write(0x380, lapic_period);
    __atomic_store_n(&cpu->online, 1, __ATOMIC_RELEASE);
    for (;;)
        arch_wait_interrupt();
}
static int send_ipi(UINT32 target, UINT32 command)
{
    if (platform.x2apic) {
        arch_wrmsr(0x830, ((UINT64)target << 32) | command);
        return 0;
    }
    if (target > 255)
        return K_ENOTSUP;
    for (UINT32 n = 0; n < 10000; ++n) {
        if (!(x86_64_lapic_read(0x300) & (1 << 12))) {
            x86_64_lapic_write(0x310, target << 24);
            x86_64_lapic_write(0x300, command);
            for (UINT32 j = 0; j < 10000; ++j) {
                if (!(x86_64_lapic_read(0x300) & (1 << 12)))
                    return 0;
                x86_64_delay_us(10);
            }
            return K_ETIMEDOUT;
        }
        x86_64_delay_us(10);
    }
    return K_ETIMEDOUT;
}
int x86_64_smp_start(void)
{
    if (x86_64_cpu_id() != 0 || !scheduler_ready || smp_started)
        return K_EBUSY;
    if (platform.discovered_cpus == 1)
        return 0;
    if (!platform.apic || !lapic_period || pic_timer || !ap_trampoline || (ap_trampoline & 4095) ||
        ap_trampoline >= 0x100000 || x86_64_ap_blob_end - x86_64_ap_blob > 4096)
        return K_ENOTSUP;
    /* Make the trampoline executable before kernel mappings freeze. */
    int e = arch_map_identity(ap_trampoline, 4096, PAGE_RW);
    if (e)
        return e;
    arch_write_cr3((UINT64)(UINTN)kernel_pml4);
    UINT8 *blob = (void *)(UINTN)ap_trampoline;
    mem_copy(blob, x86_64_ap_blob, x86_64_ap_blob_end - x86_64_ap_blob);
    wr32(blob + (x86_64_ap_base - x86_64_ap_blob), (UINT32)ap_trampoline);
    wr32(blob + (x86_64_ap_gdtr - x86_64_ap_blob) + 2,
         (UINT32)(ap_trampoline + (x86_64_ap_gdt - x86_64_ap_blob)));
    wr32(blob + (x86_64_ap_pm_far - x86_64_ap_blob),
         (UINT32)(ap_trampoline + (x86_64_ap_pm - x86_64_ap_blob)));
    wr32(blob + (x86_64_ap_lm_far - x86_64_ap_blob),
         (UINT32)(ap_trampoline + (x86_64_ap_lm - x86_64_ap_blob)));
    wr32(blob + (x86_64_ap_cr3 - x86_64_ap_blob), (UINT32)(UINTN)kernel_pml4);
    wr32(blob + (x86_64_ap_efer - x86_64_ap_blob), (1 << 8) | (platform.nx ? (1 << 11) : 0));
    wr64(blob + (x86_64_ap_entry - x86_64_ap_blob), (UINT64)(UINTN)ap_main);
    smp_started = TRUE;
    for (UINT32 i = 1; i < platform.discovered_cpus; ++i) {
        UINT64 flags = k_spin_lock(&sched_lock);
        task *idle = new_task("idle", idle_entry, NULL, 1, i, TRUE);
        if (!idle) {
            k_spin_unlock(&sched_lock, flags);
            return K_ENOMEM;
        }
        cpus[i].idle = idle;
        cpus[i].current = idle;
        idle->state = K_TASK_RUNNING;
        k_spin_unlock(&sched_lock, flags);
        wr64(blob + (x86_64_ap_stack - x86_64_ap_blob), idle->stack_top);
        wr64(blob + (x86_64_ap_argument - x86_64_ap_blob), (UINT64)(UINTN)&cpus[i]);
        arch_full_fence();
        UINT32 target = cpus[i].apic_id;
        e = send_ipi(target, 0xc500);
        if (!e)
            e = x86_64_delay_checked(10000);
        if (!e)
            e = send_ipi(target, 0x8500);
        if (!e)
            e = send_ipi(target, 0x600 | (UINT32)(ap_trampoline >> 12));
        if (!e)
            e = x86_64_delay_checked(200);
        if (!e && !__atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE))
            e = send_ipi(target, 0x600 | (UINT32)(ap_trampoline >> 12));
        for (UINT32 j = 0; !e && j < 1000 && !__atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE);
             ++j)
            e = x86_64_delay_checked(1000);
        if (e || !__atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE)) {
            /* After a late/failed AP, keep its handoff page/stack reserved and stop starting APs.
             */
            (void)send_ipi(target, 0xc500);
            return e ? e : K_ETIMEDOUT;
        }
        ++platform.online_cpus;
    }
    return 0;
}

/* PCI configuration mechanism #1. */
static k_spinlock pci_lock = K_SPINLOCK_INIT;
UINT32 x86_64_pci_read32(UINT8 bus, UINT8 slot, UINT8 func, UINT8 off)
{
    UINT64 f = k_spin_lock(&pci_lock);
    arch_out32(0xcf8, 0x80000000U | ((UINT32)bus << 16) | ((UINT32)slot << 11) |
                          ((UINT32)func << 8) | (off & 0xfc));
    UINT32 v = arch_in32(0xcfc);
    k_spin_unlock(&pci_lock, f);
    return v;
}
void x86_64_pci_write32(UINT8 bus, UINT8 slot, UINT8 func, UINT8 off, UINT32 value)
{
    UINT64 f = k_spin_lock(&pci_lock);
    arch_out32(0xcf8, 0x80000000U | ((UINT32)bus << 16) | ((UINT32)slot << 11) |
                          ((UINT32)func << 8) | (off & 0xfc));
    arch_out32(0xcfc, value);
    k_spin_unlock(&pci_lock, f);
}
