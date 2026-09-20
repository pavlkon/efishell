// SPDX-License-Identifier: GPL-2.0-only
#ifndef ARCH_H
#define ARCH_H

/* Select another backend here when adding an architecture. */
#if !defined(__x86_64__)
#error "No kernel backend for this architecture yet"
#endif
#include "x86_64.h"
#define ARCH_RIEF_MACHINE RIEF_ARCH_X86_64
typedef x86_64_irq_frame arch_irq_frame;
typedef x86_64_task_state arch_task_state;
typedef x86_64_cpu_entry arch_cpu_entry;
typedef x86_64_cpu_tables arch_cpu_tables;
typedef x86_64_address_space arch_address_space;

#define arch_as_alloc x86_64_as_alloc
#define arch_as_check x86_64_as_check
#define arch_as_copy x86_64_as_copy
#define arch_as_create x86_64_as_create
#define arch_as_destroy x86_64_as_destroy
#define arch_as_map_at x86_64_as_map_at
#define arch_as_rollback x86_64_as_rollback
#define arch_cache_flush x86_64_cache_flush
#define arch_caches_enabled x86_64_caches_enabled
#define arch_compiler_barrier x86_64_compiler_barrier
#define arch_copy_from_user x86_64_copy_from_user
#define arch_copy_to_user x86_64_copy_to_user
#define arch_cpu_get x86_64_cpu_get
#define arch_cpu_id x86_64_cpu_id
#define arch_cpuid x86_64_cpuid
#define arch_cycle_count x86_64_cycle_count
#define arch_delay_checked x86_64_delay_checked
#define arch_delay_us x86_64_delay_us
#define arch_fill32 x86_64_fill32
#define arch_fpu_init x86_64_fpu_init
#define arch_full_fence x86_64_full_fence
#define arch_fxrestore x86_64_fxrestore
#define arch_fxsave x86_64_fxsave
#define arch_get_pml4 x86_64_get_pml4
#define arch_identity_seed x86_64_identity_seed
#define arch_in16 x86_64_in16
#define arch_in32 x86_64_in32
#define arch_in8 x86_64_in8
#define arch_initial_user_stack x86_64_initial_user_stack
#define arch_interrupts_enable x86_64_interrupts_enable
#define arch_interrupts_init x86_64_interrupts_init
#define arch_invalidate_page x86_64_invalidate_page
#define arch_irq_disable x86_64_irq_disable
#define arch_irq_enable x86_64_irq_enable
#define arch_irq_restore x86_64_irq_restore
#define arch_irq_save x86_64_irq_save
#define arch_lapic_eoi x86_64_lapic_eoi
#define arch_lapic_read x86_64_lapic_read
#define arch_lapic_write x86_64_lapic_write
#define arch_load_tables x86_64_load_tables
#define arch_map_identity x86_64_map_identity
#define arch_map_page_raw x86_64_map_page_raw
#define arch_mmio_map x86_64_mmio_map
#define arch_out16 x86_64_out16
#define arch_out32 x86_64_out32
#define arch_out8 x86_64_out8
#define arch_pause x86_64_pause
#define arch_platform x86_64_platform
#define arch_platform_init x86_64_platform_init
#define arch_random64 x86_64_random64
#define arch_rdmsr x86_64_rdmsr
#define arch_read_cr0 x86_64_read_cr0
#define arch_read_cr2 x86_64_read_cr2
#define arch_read_cr3 x86_64_read_cr3
#define arch_read_cr4 x86_64_read_cr4
#define arch_read_fence x86_64_read_fence
#define arch_reschedule x86_64_reschedule
#define arch_smp_start x86_64_smp_start
#define arch_stop x86_64_stop
#define arch_task_context_init x86_64_task_context_init
#define arch_task_set_tls x86_64_task_set_tls
#define arch_task_state_init x86_64_task_state_init
#define arch_task_switch x86_64_task_switch
#define arch_tick_hz x86_64_tick_hz
#define arch_ticks x86_64_ticks
#define arch_timer_init x86_64_timer_init
#define arch_uptime_ms x86_64_uptime_ms
#define arch_user_context_init x86_64_user_context_init
#define arch_vmm_init x86_64_vmm_init
#define arch_vmm_map_page x86_64_vmm_map_page
#define arch_vmm_translate x86_64_vmm_translate
#define arch_vmm_unmap_page x86_64_vmm_unmap_page
#define arch_wait_interrupt x86_64_wait_interrupt
#define arch_wait_reg x86_64_wait_reg
#define arch_write_cr0 x86_64_write_cr0
#define arch_write_cr3 x86_64_write_cr3
#define arch_write_cr4 x86_64_write_cr4
#define arch_write_fence x86_64_write_fence
#define arch_wrmsr x86_64_wrmsr
#define arch_cache_disable x86_64_cache_disable
#define arch_cache_flush_root x86_64_cache_flush_root
#define arch_pci_read32 x86_64_pci_read32
#define arch_pci_write32 x86_64_pci_write32
#endif
