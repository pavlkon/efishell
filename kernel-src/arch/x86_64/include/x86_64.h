// SPDX-License-Identifier: GPL-2.0-only
#ifndef X86_64_H
#define X86_64_H
#include "kernel.h"
typedef struct task task;
typedef struct cpu_state cpu_state;

typedef struct {
    UINT64 r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rdx, rcx, rbx, rax;
    UINT64 vector, error, rip, cs, rflags, rsp, ss;
} x86_64_irq_frame;
_Static_assert(offsetof(x86_64_irq_frame, vector) == 120, "interrupt ABI");
_Static_assert(sizeof(x86_64_irq_frame) == 176, "interrupt ABI");
#pragma pack(push, 1)
typedef struct {
    UINT16 limit;
    UINT64 base;
} x86_64_descriptor_ptr;
typedef struct {
    UINT16 low, cs;
    UINT8 ist, attr;
    UINT16 middle;
    UINT32 high, zero;
} x86_64_idt_entry;
typedef struct {
    UINT32 reserved0;
    UINT64 rsp[3];
    UINT64 reserved1;
    UINT64 ist[7];
    UINT64 reserved2;
    UINT16 reserved3, iomap;
} x86_64_tss;
#pragma pack(pop)

#define X86_64_XSAVE_HEADER_OFFSET 512u
#define X86_64_XSAVE_HEADER_SIZE 64u
typedef struct {
    UINT8 fx[4096] __attribute__((aligned(64)));
} x86_64_task_state;
typedef struct {
    UINT64 syscall_stack, syscall_user_rsp;
} x86_64_cpu_entry;
typedef struct {
    UINT64 gdt[7] __attribute__((aligned(16)));
    x86_64_tss tss;
    UINT8 fault_stack[8192] __attribute__((aligned(16)));
    UINT8 nmi_stack[8192] __attribute__((aligned(16)));
    UINT8 machine_stack[8192] __attribute__((aligned(16)));
} x86_64_cpu_tables;
typedef struct {
    UINT64 *root, owned_slots[4];
} x86_64_address_space;

/* Instruction wrappers are implemented in x86_64.c. */
void x86_64_pause(void);
UINT8 x86_64_in8(UINT16 port);
UINT16 x86_64_in16(UINT16 port);
UINT32 x86_64_in32(UINT16 port);
void x86_64_out8(UINT16 port, UINT8 value);
void x86_64_out16(UINT16 port, UINT16 value);
void x86_64_out32(UINT16 port, UINT32 value);
UINT64 x86_64_rdmsr(UINT32 reg);
void x86_64_wrmsr(UINT32 reg, UINT64 value);
void x86_64_cpuid(UINT32 leaf, UINT32 subleaf, UINT32 *, UINT32 *, UINT32 *, UINT32 *);
UINT64 x86_64_cycle_count(void);
UINT64 x86_64_irq_save(void);
void x86_64_irq_restore(UINT64 flags);
void x86_64_irq_enable(void);
void x86_64_irq_disable(void);
void x86_64_wait_interrupt(void);
__attribute__((noreturn)) void x86_64_stop(void);
void x86_64_reschedule(void);
void x86_64_read_fence(void);
void x86_64_write_fence(void);
void x86_64_full_fence(void);
void x86_64_compiler_barrier(void);
UINT64 x86_64_read_cr0(void);
UINT64 x86_64_read_cr2(void);
UINT64 x86_64_read_cr3(void);
UINT64 x86_64_read_cr4(void);
void x86_64_write_cr0(UINT64 value);
void x86_64_write_cr3(UINT64 value);
void x86_64_write_cr4(UINT64 value);
void x86_64_invalidate_page(UINT64 address);
void x86_64_cache_flush(void);
void x86_64_fill32(volatile UINT32 *dst, UINT32 value, UINTN count);
BOOLEAN x86_64_random64(UINT64 *value);
BOOLEAN x86_64_caches_enabled(void);
void x86_64_fxsave(x86_64_task_state *state);
void x86_64_fxrestore(const x86_64_task_state *state);
void x86_64_load_tables(const x86_64_descriptor_ptr *, const x86_64_descriptor_ptr *);

/* Backend mechanisms; task and CPU storage remain kernel-owned. */
int x86_64_map_page_raw(UINT64 *root, UINT64 v, UINT64 p, UINT64 flags, BOOLEAN replace);
int x86_64_vmm_map_page(UINT64 *root, UINT64 v, UINT64 p, UINT64 flags);
int x86_64_vmm_unmap_page(UINT64 *root, UINT64 v);
int x86_64_vmm_translate(UINT64 *root, UINT64 v, UINT64 *phys, UINT64 *flags);
int x86_64_map_identity(UINT64 start, UINT64 size, UINT64 flags);
int x86_64_mmio_map(UINT64 p, UINT64 bytes);
int x86_64_vmm_init(void);
UINT64 *x86_64_get_pml4(void);
k_address_space *x86_64_as_create(void);
int x86_64_as_destroy(k_address_space *as);
int x86_64_as_alloc(k_address_space *as, UINT64 bytes, UINT64 align, UINT32 rwx, UINT64 *base);
int x86_64_as_check(k_address_space *as, UINT64 address, UINT64 bytes, BOOLEAN write);
int x86_64_as_copy(k_address_space *as, UINT64 u, void *buf, UINT64 n, BOOLEAN to, BOOLEAN loader);
int x86_64_copy_to_user(k_address_space *as, UINT64 u, const void *b, UINT64 n);
int x86_64_copy_from_user(k_address_space *as, void *b, UINT64 u, UINT64 n);
void x86_64_as_rollback(k_address_space *as, UINT64 old_next, UINT32 old_pages);
int x86_64_as_map_at(k_address_space *as, UINT64 address, UINT64 bytes, UINT32 rwx);
int x86_64_platform_init(UINT64 rsdp, UINT64 trampoline);
const k_platform_info *x86_64_platform(void);
UINT32 x86_64_lapic_read(UINT32 off);
void x86_64_lapic_write(UINT32 off, UINT32 value);
void x86_64_lapic_eoi(void);
UINT32 x86_64_cpu_id(void);
int x86_64_delay_checked(UINT32 us);
void x86_64_delay_us(UINT32 us);
BOOLEAN x86_64_wait_reg(volatile UINT32 *reg, UINT32 mask, UINT32 expected, UINT32 milliseconds);
int x86_64_timer_init(UINT32 hz);
UINT64 x86_64_ticks(void);
UINT64 x86_64_uptime_ms(void);
UINT32 x86_64_tick_hz(void);
int x86_64_cpu_get(UINT32 i, k_cpu_info *out);
void x86_64_cpu_tables_init(cpu_state *cpu);
void x86_64_interrupts_init(void);
void x86_64_interrupts_enable(void);
int x86_64_smp_start(void);
void x86_64_fpu_init(void);
void x86_64_task_state_init(task *);
void x86_64_task_context_init(task *, void (*entry)(void));
void x86_64_user_context_init(task *, UINT64 entry, UINT64 stack);
void x86_64_task_set_tls(task *, UINT64 base);
void x86_64_task_switch(cpu_state *, task *old, task *next);
UINT64 x86_64_initial_user_stack(UINT64 top);
UINT64 x86_64_identity_seed(void);

void x86_64_cache_disable(UINT64 control);
void x86_64_cache_flush_root(UINT64 root);
UINT32 x86_64_pci_read32(UINT8 bus, UINT8 slot, UINT8 func, UINT8 off);
void x86_64_pci_write32(UINT8 bus, UINT8 slot, UINT8 func, UINT8 off, UINT32 value);
int x86_64_as_alloc_ex(k_address_space *, UINT64, UINT64, UINT32, UINT32, UINT64 *);
int x86_64_as_mapping_size(k_address_space *, UINT64, UINT64 *);
int x86_64_demand_fault(k_address_space *, UINT64, UINT64);
int x86_64_xstate_enable(void);
int x86_64_cpu_features_get(k_cpu_features *);
int x86_64_vector_test(void);
int x86_64_power_get(k_power_info *);
int x86_64_power_set(UINT32, UINT32, UINT32);
int x86_64_perf_start(void);
int x86_64_perf_read(k_perf_info *);
int x86_64_perf_stop(void);
int x86_64_try_rdmsr(UINT32, UINT64 *);
int x86_64_try_wrmsr(UINT32, UINT64);
BOOLEAN x86_64_msr_fixup(x86_64_irq_frame *);
extern UINT64 x86_64_xstate_mask;
extern UINT32 x86_64_xstate_size;
void x86_64_timer_clock_init(volatile UINT8 *, UINT64, UINT64, UINT32);
UINT64 x86_64_clock_ticks(void);
UINT64 x86_64_clock_ms(void);
void x86_64_timer_account(UINT32);
void x86_64_timer_arm(BOOLEAN, UINT64);
int x86_64_tickless_set(BOOLEAN);
BOOLEAN x86_64_tickless_enabled(void);
void x86_64_wake_cpu(UINT32);
#endif
