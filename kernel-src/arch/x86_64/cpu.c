// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"
#include "x86_64_internal.h"

UINT64 x86_64_rdmsr(UINT32 m)
{
    UINT32 a, d;
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(m));
    return a | ((UINT64)d << 32);
}

void x86_64_wrmsr(UINT32 m, UINT64 v)
{
    __asm__ volatile("wrmsr" ::"c"(m), "a"((UINT32)v), "d"((UINT32)(v >> 32)) : "memory");
}

void x86_64_cpuid(UINT32 leaf, UINT32 sub, UINT32 *a, UINT32 *b, UINT32 *c, UINT32 *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

UINT64 x86_64_cycle_count(void)
{
    UINT32 a, d;
    __asm__ volatile("lfence; rdtsc" : "=a"(a), "=d"(d)::"memory");
    return a | ((UINT64)d << 32);
}

UINT64 x86_64_irq_save(void)
{
    UINT64 f;
    __asm__ volatile("pushfq;popq %0;cli" : "=r"(f)::"memory");
    return f;
}

void x86_64_irq_restore(UINT64 f)
{
    if (f & (1 << 9))
        __asm__ volatile("sti" ::: "memory");
}

static void x86_64_pic_eoi(UINT32 v)
{
    if (v == 0x27) {
        arch_out8(0x20, 0x0b);
        if (!(arch_in8(0x20) & 0x80))
            return;
    }
    if (v == 0x2f) {
        arch_out8(0xa0, 0x0b);
        if (!(arch_in8(0xa0) & 0x80)) {
            arch_out8(0x20, 0x20);
            return;
        }
    }
    if (v >= 0x28)
        arch_out8(0xa0, 0x20);
    arch_out8(0x20, 0x20);
}

static irq_frame *x86_64_irq_dispatch_inner(irq_frame *f)
{
    UINT32 v = (UINT32)f->vector, c = k_cpu_id();
    if (v == 13 && x86_64_msr_fixup(f))
        return f;
    if (v == 2)
        return f; /* NMI uses a private IST and never takes scheduler locks. */
    if (v != 8 && v != 18 && (f->cs & 3) == 3 && cpus[c].current && cpus[c].current->process &&
        cpus[c].current->control_request && process_return_control(cpus[c].current)) {
        if (v == 0xf0 || (v == 0x20 && pic_timer)) {
            x86_64_timer_account(c);
        }
        /* Acknowledge the interrupt before abandoning the user frame. */
        if (v >= 0x20 && v < 0x30) {
            if (platform.apic && !pic_timer)
                arch_lapic_eoi();
            else
                x86_64_pic_eoi(v);
        } else if (v >= 0x20 && v != 128 && v != 129 && v != 0xff && platform.apic)
            arch_lapic_eoi();
        return kernel_schedule(f, TRUE);
    }
    if (v == 0xff)
        return f;
    if (v < 32) {
        UINT64 cr2;
        __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
        task *t = cpus[c].current;
        if (v == 14 && (f->cs & 3) == 3 && t && t->process &&
            !x86_64_demand_fault(t->process->as, cr2, f->error))
            return f;
        if ((f->cs & 3) == 3 && t && t->process && v != 8 && v != 18) {
            UINT64 lock = k_spin_lock(&sched_lock);
            k_process *p = t->process;
            p->fault_vector = v;
            p->fault_ip = f->rip;
            p->fault_address = v == 14 ? cr2 : 0;
            p->exit_code = -(INT32)(128 + v);
            p->state = K_TASK_ZOMBIE;
            t->exit_code = p->exit_code;
            t->state = K_TASK_ZOMBIE;
            k_spin_unlock(&sched_lock, lock);
            return kernel_schedule(f, TRUE);
        }
        console_panic_mode();
        con_print("\nCPU ");
        con_print_uint(c);
        con_print(" exception ");
        con_print_uint(v);
        con_print(" error=");
        con_print_hex(f->error);
        con_print(" RIP=");
        con_print_hex(f->rip);
        con_print(" CR2=");
        con_print_hex(cr2);
        kernel_halt("kernel exception");
    }
    if (v == 128) {
        if ((f->cs & 3) != 3) {
            f->rax = (UINT64)K_EPERM;
            return f;
        }
        __asm__ volatile("sti" ::: "memory");
        k_syscall_request request = {.number = f->rax,
                                     .args = {f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9}};
        INT64 result = syscall_dispatch(&request);
        __asm__ volatile("cli" ::: "memory");
        f->rax = (UINT64)result;
        return f;
    }
    if (v == 0xf1) {
        x86_64_lapic_eoi();
        return scheduler_ready ? kernel_schedule(f, TRUE) : f;
    }
    if (v == 129)
        return scheduler_ready ? kernel_schedule(f, TRUE) : f;
    if (v == 0xf0 || (v == 0x20 && pic_timer)) {
        if (v == 0x20)
            x86_64_pic_eoi(v);
        else
            arch_lapic_eoi();
        x86_64_timer_account(c);
        return scheduler_ready ? kernel_schedule(f, FALSE) : f;
    }
    if (v == 0x21 || v == 0x2c)
        ps2_irq();
    if (v >= 0x20 && v < 0x30) {
        if (platform.apic && !pic_timer)
            arch_lapic_eoi();
        else {
            if (platform.pic)
                x86_64_pic_eoi(v);
            if (platform.apic)
                arch_lapic_eoi();
        }
    } else if (platform.apic) {
        if (v == 0xfe) {
            arch_lapic_write(0x280, 0);
            (void)arch_lapic_read(0x280);
        }
        arch_lapic_eoi();
    }
    return f;
}

irq_frame *x86_64_irq_dispatch(irq_frame *f)
{
    irq_frame *r = x86_64_irq_dispatch_inner(f);
    /* Validate user IRET state so a corrupt RSP/NT cannot turn return into a ring-0 #GP. */
    while ((r->cs & 3) == 3) {
        task *t = current_task();
        if (t && t->process && process_return_control(t)) {
            r = kernel_schedule(r, TRUE);
            continue;
        }
        BOOLEAN bad = r->cs != 0x23 || r->ss != 0x1b || r->rsp < 0x10000 ||
                      r->rsp >= K_USER_CANON_LIMIT || r->rip < 0x10000 ||
                      r->rip >= K_USER_CANON_LIMIT || !t || !t->process;
        if (!bad) {
            r->rflags = (r->rflags & 0x250dd5ULL) | 0x202;
            break;
        }
        if (!t || !t->process)
            kernel_halt("invalid user return without process");
        UINT64 saved = k_spin_lock(&sched_lock);
        t->state = K_TASK_ZOMBIE;
        t->exit_code = -141;
        t->preempt_depth = 0;
        t->process->state = K_TASK_ZOMBIE;
        t->process->exit_code = -141;
        t->process->fault_vector = 13;
        t->process->fault_address = r->rsp;
        t->process->fault_ip = r->rip;
        k_spin_unlock(&sched_lock, saved);
        r = kernel_schedule(r, TRUE);
    }
    return r;
}

void x86_64_pause(void) { __asm__ volatile("pause" ::: "memory"); }

UINT8 x86_64_in8(UINT16 p)
{
    UINT8 v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}

UINT16 x86_64_in16(UINT16 p)
{
    UINT16 v;
    __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}

UINT32 x86_64_in32(UINT16 p)
{
    UINT32 v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}

void x86_64_out8(UINT16 p, UINT8 v) { __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p)); }

void x86_64_out16(UINT16 p, UINT16 v) { __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(p)); }

void x86_64_out32(UINT16 p, UINT32 v) { __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p)); }

UINT8 initial_fx[4096] __attribute__((aligned(64)));
_Static_assert(offsetof(cpu_state, entry.syscall_stack) == 0, "syscall stack ABI");
_Static_assert(offsetof(cpu_state, entry.syscall_user_rsp) == 8, "syscall scratch ABI");
_Static_assert(offsetof(cpu_state, stack_owner) == 16, "stack ownership ABI");
_Static_assert(offsetof(cpu_state, current) == 24, "current task ABI");
_Static_assert(_Alignof(x86_64_task_state) >= 64, "XSAVE alignment");
_Static_assert(offsetof(x86_64_task_state, fx) == 0, "XSAVE area offset");

void x86_64_irq_enable(void) { __asm__ volatile("sti" ::: "memory"); }
void x86_64_irq_disable(void) { __asm__ volatile("cli" ::: "memory"); }
void x86_64_wait_interrupt(void) { __asm__ volatile("sti; hlt" ::: "memory"); }
__attribute__((noreturn)) void x86_64_stop(void)
{
    for (;;)
        __asm__ volatile("cli; hlt" ::: "memory");
}
void x86_64_reschedule(void) { __asm__ volatile("int $0x81" ::: "memory"); }
void x86_64_read_fence(void) { __asm__ volatile("lfence" ::: "memory"); }
void x86_64_write_fence(void) { __asm__ volatile("sfence" ::: "memory"); }
void x86_64_full_fence(void) { __asm__ volatile("mfence" ::: "memory"); }
void x86_64_compiler_barrier(void) { __asm__ volatile("" ::: "memory"); }
UINT64 x86_64_read_cr0(void)
{
    UINT64 value;
    __asm__ volatile("mov %%cr0,%0" : "=r"(value)::"memory");
    return value;
}
UINT64 x86_64_read_cr2(void)
{
    UINT64 value;
    __asm__ volatile("mov %%cr2,%0" : "=r"(value)::"memory");
    return value;
}
UINT64 x86_64_read_cr3(void)
{
    UINT64 value;
    __asm__ volatile("mov %%cr3,%0" : "=r"(value)::"memory");
    return value;
}
UINT64 x86_64_read_cr4(void)
{
    UINT64 value;
    __asm__ volatile("mov %%cr4,%0" : "=r"(value)::"memory");
    return value;
}
void x86_64_write_cr0(UINT64 value) { __asm__ volatile("mov %0,%%cr0" ::"r"(value) : "memory"); }
void x86_64_write_cr3(UINT64 value) { __asm__ volatile("mov %0,%%cr3" ::"r"(value) : "memory"); }
void x86_64_write_cr4(UINT64 value) { __asm__ volatile("mov %0,%%cr4" ::"r"(value) : "memory"); }
void x86_64_invalidate_page(UINT64 address)
{
    __asm__ volatile("invlpg (%0)" ::"r"(address) : "memory");
}
void x86_64_cache_flush(void) { __asm__ volatile("wbinvd" ::: "memory"); }
void x86_64_fill32(volatile UINT32 *dst, UINT32 value, UINTN count)
{
    __asm__ volatile("cld; rep stosl" : "+D"(dst), "+c"(count) : "a"(value) : "memory");
}
BOOLEAN x86_64_random64(UINT64 *value)
{
    UINT32 a, b, c, d;
    if (!value)
        return FALSE;
    x86_64_cpuid(1, 0, &a, &b, &c, &d);
    if (!(c & (1u << 30)))
        return FALSE;
    UINT64 result;
    UINT8 ok;
    __asm__ volatile("rdrand %0;setc %1" : "=r"(result), "=qm"(ok)::"cc");
    if (ok)
        *value = result;
    return ok != 0;
}
BOOLEAN x86_64_caches_enabled(void) { return !(x86_64_read_cr0() & ((1ULL << 30) | (1ULL << 29))); }
void x86_64_fxsave(x86_64_task_state *state)
{
    if (x86_64_xstate_mask) {
        /* Full standard-format save: XSAVE leaves other header bits untouched. */
        mem_zero(state->fx + X86_64_XSAVE_HEADER_OFFSET, X86_64_XSAVE_HEADER_SIZE);
        __asm__ volatile("xsave64 %0"
                         : "+m"(*state)
                         : "a"((UINT32)x86_64_xstate_mask), "d"(0)
                         : "memory");
    } else
        __asm__ volatile("fxsave64 %0" : "=m"(*state)::"memory");
}
void x86_64_fxrestore(const x86_64_task_state *state)
{
    if (x86_64_xstate_mask)
        __asm__ volatile("xrstor64 %0" ::"m"(*state), "a"((UINT32)x86_64_xstate_mask), "d"(0)
                         : "memory");
    else
        __asm__ volatile("fxrstor64 %0" ::"m"(*state) : "memory");
}
void x86_64_load_tables(const x86_64_descriptor_ptr *gdt, const x86_64_descriptor_ptr *idt)
{
    __asm__ volatile("lgdt %0\npushq $8\nleaq 1f(%%rip),%%rax\npushq %%rax\nlretq\n1:\n"
                     "movw $0x10,%%ax\nmovw %%ax,%%ds\nmovw %%ax,%%es\nmovw %%ax,%%ss\n"
                     "xor %%eax,%%eax\nmovw %%ax,%%fs\nmovw %%ax,%%gs\n"
                     "movw $0x28,%%ax\nltr %%ax\nlidt %1" ::"m"(*gdt),
                     "m"(*idt)
                     : "rax", "memory");
}
void x86_64_fpu_init(void)
{
    mem_zero(initial_fx, sizeof(initial_fx));
    initial_fx[0] = 0x7f;
    initial_fx[1] = 3;
    wr32(initial_fx + 24, 0x1f80);
    x86_64_fxrestore((const x86_64_task_state *)initial_fx);
}
void x86_64_task_state_init(task *t) { mem_copy(t->arch.fx, initial_fx, sizeof(t->arch.fx)); }
void x86_64_task_context_init(task *t, void (*entry)(void))
{
    t->frame = (void *)(UINTN)((t->stack_top - 64 - sizeof(irq_frame)) & ~15ULL);
    mem_zero(t->frame, sizeof(*t->frame));
    t->frame->rip = (UINT64)(UINTN)entry;
    t->frame->cs = 8;
    t->frame->ss = 0x10;
    t->frame->rflags = 0x202;
    t->frame->rsp = t->stack_top - 40;
    x86_64_task_state_init(t);
}
void x86_64_user_context_init(task *t, UINT64 entry, UINT64 stack)
{
    t->frame->rip = entry;
    t->frame->cs = 0x23;
    t->frame->ss = 0x1b;
    t->frame->rsp = stack;
    t->frame->rflags = 0x202;
}
void x86_64_task_set_tls(task *t, UINT64 base) { t->tls_base = base; }
UINT64 x86_64_initial_user_stack(UINT64 top) { return top - 40; }
UINT64 x86_64_identity_seed(void)
{
    return x86_64_cycle_count() ^ (UINT64)(UINTN)kernel_pml4 ^ saved_pat;
}
void x86_64_task_switch(cpu_state *cpu, task *old, task *next)
{
    old->tls_base = x86_64_rdmsr(0xc0000100);
    x86_64_wrmsr(0xc0000100, next->tls_base);
    x86_64_fxsave(&old->arch);
    x86_64_fxrestore(&next->arch);
    cpu->current = next;
    cpu->tables.tss.rsp[0] = next->stack_top;
    cpu->entry.syscall_stack = next->stack_top;
    UINT64 *root = next->process ? next->process->as->mmu.root : kernel_pml4;
    UINT64 *previous = old->process ? old->process->as->mmu.root : kernel_pml4;
    if (root != previous)
        x86_64_write_cr3((UINT64)(UINTN)root);
}

void x86_64_cache_disable(UINT64 control)
{
    __asm__ volatile("mov %0,%%cr0; wbinvd" ::"r"(control) : "memory");
}
void x86_64_cache_flush_root(UINT64 root)
{
    __asm__ volatile("wbinvd; mov %0,%%cr3" ::"r"(root) : "memory");
}
