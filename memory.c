// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"

static k_spinlock pmm_lock = K_SPINLOCK_INIT;
/* PMM returns only EFI-eligible pages it currently owns; low memory and firmware allocations stay
 * reserved. */
static UINT8 page_bitmap[PMM_MAX_PAGES / 8], eligible_bitmap[PMM_MAX_PAGES / 8];
static UINT64 pmm_free_pages, pmm_total_pages_managed, pmm_search_cursor, pmm_high_page;
EFI_MEMORY_DESCRIPTOR *boot_map;
UINTN boot_desc_size, boot_entries;
BOOLEAN bit_get(UINT8 *b, UINT64 n) { return (b[n >> 3] >> (n & 7)) & 1; }
void bit_set(UINT8 *b, UINT64 n) { b[n >> 3] |= (UINT8)(1u << (n & 7)); }
void bit_clear(UINT8 *b, UINT64 n) { b[n >> 3] &= (UINT8) ~(1u << (n & 7)); }
void pmm_seed_from_efi_map(EFI_MEMORY_DESCRIPTOR *map, UINTN stride, UINTN count)
{
    if (scheduler_ready || !map || stride < sizeof(*map))
        kernel_halt("invalid PMM seed");
    boot_map = map;
    boot_desc_size = stride;
    boot_entries = count;
    mem_zero(page_bitmap, sizeof(page_bitmap));
    mem_zero(eligible_bitmap, sizeof(eligible_bitmap));
    pmm_free_pages = pmm_total_pages_managed = pmm_search_cursor = pmm_high_page = 0;
    for (UINTN i = 0; i < count; ++i) {
        EFI_MEMORY_DESCRIPTOR *d = (void *)((UINT8 *)map + i * stride);
        if (d->Type != EfiConventionalMemory || (d->PhysicalStart & 4095))
            continue;
        UINT64 first = d->PhysicalStart / 4096;
        if (first >= PMM_MAX_PAGES)
            continue;
        UINT64 last = first + MIN(d->NumberOfPages, PMM_MAX_PAGES - first);
        if (first < 256)
            first = 256;
        for (UINT64 p = first; p < last; ++p)
            if (!bit_get(eligible_bitmap, p)) {
                bit_set(eligible_bitmap, p);
                bit_set(page_bitmap, p);
                ++pmm_free_pages;
                ++pmm_total_pages_managed;
            }
        if (last > pmm_high_page)
            pmm_high_page = last;
    }
}
UINT64 k_pmm_alloc_pages(UINT32 count, UINT64 maximum)
{
    if (!count)
        return 0;
    UINT64 limit = maximum ? MIN(maximum / 4096, pmm_high_page) : pmm_high_page;
    UINT64 f = k_spin_lock(&pmm_lock);
    if (count > pmm_free_pages || count > limit) {
        k_spin_unlock(&pmm_lock, f);
        return 0;
    }
    UINT64 cursor = count == 1 && pmm_search_cursor < limit ? pmm_search_cursor : 256;
    for (int pass = 0; pass < 2; ++pass) {
        UINT64 end = pass ? cursor : limit, run = 0, start = 0;
        for (UINT64 p = pass ? 256 : cursor; p < end; ++p) {
            if (bit_get(page_bitmap, p)) {
                if (!run)
                    start = p;
                if (++run == count) {
                    for (UINT64 j = start; j < start + count; ++j)
                        bit_clear(page_bitmap, j);
                    pmm_free_pages -= count;
                    pmm_search_cursor = start + count;
                    k_spin_unlock(&pmm_lock, f);
                    mem_zero((void *)(UINTN)(start * 4096), (UINTN)count * 4096);
                    return start * 4096;
                }
            } else
                run = 0;
        }
    }
    k_spin_unlock(&pmm_lock, f);
    return 0;
}
UINT64 pmm_alloc_page(void) { return k_pmm_alloc_pages(1, 0); }
void k_pmm_free_pages(UINT64 phys, UINT32 count)
{
    if (!count || (phys & 4095) || phys / 4096 >= PMM_MAX_PAGES ||
        count > PMM_MAX_PAGES - phys / 4096)
        return;
    UINT64 f = k_spin_lock(&pmm_lock), start = phys / 4096;
    for (UINT64 p = start; p < start + count; ++p)
        if (!bit_get(eligible_bitmap, p) || bit_get(page_bitmap, p)) {
            k_spin_unlock(&pmm_lock, f);
            return;
        }
    for (UINT64 p = start; p < start + count; ++p)
        bit_set(page_bitmap, p);
    pmm_free_pages += count;
    if (start < pmm_search_cursor)
        pmm_search_cursor = start;
    k_spin_unlock(&pmm_lock, f);
}
void pmm_free_page(UINT64 phys) { k_pmm_free_pages(phys, 1); }
UINT64 k_pmm_free_count(void) { return __atomic_load_n(&pmm_free_pages, __ATOMIC_RELAXED); }
UINT64 kernel_pmm_managed_mib(void) { return pmm_total_pages_managed / 256; }
UINT64 kernel_pmm_free_mib(void) { return k_pmm_free_count() / 256; }
UINT64 kernel_pmm_used_mib(void) { return (pmm_total_pages_managed - k_pmm_free_count()) / 256; }

int vmm_map_page(UINT64 *root, UINT64 v, UINT64 p, UINT64 flags)
{
    return arch_vmm_map_page(root, v, p, flags);
}

int vmm_unmap_page(UINT64 *root, UINT64 v) { return arch_vmm_unmap_page(root, v); }

int vmm_translate(UINT64 *root, UINT64 v, UINT64 *phys, UINT64 *flags)
{
    return arch_vmm_translate(root, v, phys, flags);
}

int k_mmio_map(UINT64 p, UINT64 bytes) { return arch_mmio_map(p, bytes); }

int kernel_vmm_init(void) { return arch_vmm_init(); }

UINT64 *kernel_get_pml4(void) { return arch_get_pml4(); }

k_address_space *k_as_create(void) { return arch_as_create(); }

int k_as_destroy(k_address_space *as) { return arch_as_destroy(as); }

int k_as_alloc(k_address_space *as, UINT64 bytes, UINT64 align, UINT32 rwx, UINT64 *base)
{
    return arch_as_alloc(as, bytes, align, rwx, base);
}

int k_as_check(k_address_space *as, UINT64 address, UINT64 bytes, BOOLEAN write)
{
    return arch_as_check(as, address, bytes, write);
}

int as_copy(k_address_space *as, UINT64 u, void *buf, UINT64 n, BOOLEAN to, BOOLEAN loader)
{
    return arch_as_copy(as, u, buf, n, to, loader);
}

int k_copy_to_user(k_address_space *as, UINT64 u, const void *b, UINT64 n)
{
    return arch_copy_to_user(as, u, b, n);
}

int k_copy_from_user(k_address_space *as, void *b, UINT64 u, UINT64 n)
{
    return arch_copy_from_user(as, b, u, n);
}
