// SPDX-License-Identifier: GPL-2.0-only
#ifndef X86_64_INTERNAL_H
#define X86_64_INTERNAL_H

#define PHYS_MASK 0x000ffffffffff000ULL
#define PAGE_LARGE (1ULL << 7)
#define USER_SLOT 128
extern BOOLEAN has_pat;
extern BOOLEAN has_mtrr;
extern BOOLEAN mtrr_fixed_supported;
extern UINT8 initial_fx[512];
extern UINT64 *kernel_pml4;
extern BOOLEAN pic_timer;
extern UINT32 saved_mtrr_count;
extern UINT32 saved_phys_bits;
extern UINT64 saved_pat;
extern UINT64 saved_mtrr_default;
extern UINT64 saved_mtrr_fixed[11];
extern UINT64 saved_mtrr_variable[64];
extern k_spinlock vm_lock;
#endif
