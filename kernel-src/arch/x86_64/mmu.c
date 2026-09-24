// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"
#include "x86_64_internal.h"

static struct k_address_space spaces[MAX_AS];
k_spinlock vm_lock = K_SPINLOCK_INIT;
static BOOLEAN vm_ready;
UINT64 *kernel_pml4;
static BOOLEAN canonical(UINT64 a) { return (a >> 47) == 0 || (a >> 47) == 0x1ffff; }
/* Split large mappings before descent; translate PAT bit 12 to 4 KiB PAT bit 7. */
int x86_64_pt_child(UINT64 *table, UINT32 idx, UINT32 level, BOOLEAN user, UINT64 **out)
{
    UINT64 e = table[idx];
    if (!(e & PAGE_PRESENT)) {
        UINT64 p = pmm_alloc_page();
        if (!p)
            return K_ENOMEM;
        table[idx] = p | PAGE_PRESENT | PAGE_RW | (user ? PAGE_USER : 0);
    } else if (e & PAGE_LARGE) {
        if (level != 2 && level != 3)
            return K_EINVAL;
        UINT64 p = pmm_alloc_page();
        if (!p)
            return K_ENOMEM;
        UINT64 *child = (void *)(UINTN)p;
        UINT64 stride = level == 3 ? 0x200000ULL : 4096;
        UINT64 base = e & PHYS_MASK & ~((level == 3 ? 0x40000000ULL : 0x200000ULL) - 1);
        UINT64 flags = e & ~PHYS_MASK;
        if (e & (1ULL << 12))
            flags |= level == 3 ? (1ULL << 12) : PAGE_LARGE;
        else if (level == 2)
            flags &= ~PAGE_LARGE;
        for (UINT32 j = 0; j < 512; ++j)
            child[j] = (base + j * stride) | flags;
        table[idx] = p | PAGE_PRESENT | PAGE_RW | (e & PAGE_USER);
    }
    if (user && !(table[idx] & PAGE_USER))
        return K_EPERM;
    *out = (void *)(UINTN)(table[idx] & PHYS_MASK);
    return 0;
}
int x86_64_map_page_raw(UINT64 *root, UINT64 v, UINT64 p, UINT64 flags, BOOLEAN replace)
{
    if (!root || !canonical(v) || ((v | p) & 4095) || (p & ~PHYS_MASK))
        return K_EINVAL;
    BOOLEAN user = (flags & PAGE_USER) != 0;
    if (user && (v < 0x10000 || v >= K_USER_CANON_LIMIT ||
                 (kernel_pml4 && (kernel_pml4[v >> 39] & PAGE_PRESENT))))
        return K_EPERM;
    UINT64 *t = root;
    for (UINT32 level = 4; level > 1; --level) {
        int e = x86_64_pt_child(t, (v >> (12 + 9 * (level - 1))) & 511, level, user, &t);
        if (e)
            return e;
    }
    UINT32 idx = (v >> 12) & 511;
    if (!replace && t[idx])
        return K_EEXIST;
    t[idx] = p | flags | PAGE_PRESENT;
    if (vm_ready)
        arch_invalidate_page(v);
    return 0;
}
int x86_64_vmm_map_page(UINT64 *root, UINT64 v, UINT64 p, UINT64 flags)
{
    /* Kernel mappings freeze once APs run; user mappings stay private to fixed-affinity processes.
     */
    if ((smp_started && root == kernel_pml4) || (flags & PAGE_USER))
        return K_EPERM;
    UINT64 f = k_spin_lock(&vm_lock);
    int e = x86_64_map_page_raw(root, v, p, flags, FALSE);
    k_spin_unlock(&vm_lock, f);
    return e;
}
int x86_64_vmm_unmap_page(UINT64 *root, UINT64 v)
{
    if (!root || !canonical(v) || (v & 4095) || (smp_started && root == kernel_pml4))
        return K_EINVAL;
    UINT64 f = k_spin_lock(&vm_lock), *t = root;
    for (UINT32 level = 4; level > 1; --level) {
        UINT32 idx = (v >> (12 + 9 * (level - 1))) & 511;
        if (!(t[idx] & PAGE_PRESENT)) {
            k_spin_unlock(&vm_lock, f);
            return K_ENOENT;
        }
        int e = x86_64_pt_child(t, idx, level, FALSE, &t);
        if (e) {
            k_spin_unlock(&vm_lock, f);
            return e;
        }
    }
    UINT32 idx = (v >> 12) & 511;
    if (!(t[idx] & PAGE_PRESENT)) {
        k_spin_unlock(&vm_lock, f);
        return K_ENOENT;
    }
    t[idx] = 0;
    arch_invalidate_page(v);
    k_spin_unlock(&vm_lock, f);
    return 0;
}
int x86_64_vmm_translate(UINT64 *root, UINT64 v, UINT64 *phys, UINT64 *flags)
{
    if (!root || !canonical(v))
        return K_EFAULT;
    UINT64 *t = root, allow = PAGE_USER | PAGE_RW, nx = 0;
    for (UINT32 level = 4; level; --level) {
        UINT64 e = t[(v >> (12 + 9 * (level - 1))) & 511];
        if (!(e & PAGE_PRESENT))
            return K_EFAULT;
        allow &= e;
        nx |= e & PAGE_NX;
        if (level == 1 || ((level == 2 || level == 3) && (e & PAGE_LARGE))) {
            UINT64 mask = (1ULL << (12 + 9 * (level - 1))) - 1;
            if (phys)
                *phys = (e & PHYS_MASK & ~mask) | (v & mask);
            if (flags)
                *flags = PAGE_PRESENT | allow | nx;
            return 0;
        }
        t = (void *)(UINTN)(e & PHYS_MASK);
    }
    return K_EFAULT;
}
static int map_large(UINT64 v, UINT64 p, UINT64 flags)
{
    UINT64 *t = kernel_pml4;
    for (UINT32 level = 4; level > 2; --level) {
        int e = x86_64_pt_child(t, (v >> (12 + 9 * (level - 1))) & 511, level, FALSE, &t);
        if (e)
            return e;
    }
    UINT32 idx = (v >> 21) & 511;
    if ((t[idx] & PAGE_PRESENT) && !(t[idx] & PAGE_LARGE)) {
        for (UINT32 j = 0; j < 512; ++j) {
            int e = x86_64_map_page_raw(kernel_pml4, v + j * 4096, p + j * 4096, flags, TRUE);
            if (e)
                return e;
        }
    } else
        t[idx] = p | flags | PAGE_PRESENT | PAGE_LARGE;
    return 0;
}
int x86_64_map_identity(UINT64 start, UINT64 size, UINT64 flags)
{
    if (!size)
        return 0;
    if (start >= K_USER_BASE || size > K_USER_BASE - start)
        return K_ENOTSUP;
    UINT64 end = ALIGN_UP(start + size, 4096);
    start &= ~4095ULL;
    while (start < end) {
        int e;
        if (!(start & 0x1fffff) && end - start >= 0x200000) {
            e = map_large(start, start, flags);
            start += 0x200000;
        } else {
            e = x86_64_map_page_raw(kernel_pml4, start, start, flags, TRUE);
            start += 4096;
        }
        if (e)
            return e;
    }
    return 0;
}
int x86_64_mmio_map(UINT64 p, UINT64 bytes)
{
    if (smp_started || !p || !bytes || p >= K_USER_BASE || bytes > K_USER_BASE - p)
        return K_ENOTSUP;
    UINT64 f = k_spin_lock(&vm_lock);
    int e =
        x86_64_map_identity(p, bytes, PAGE_RW | PAGE_PCD | PAGE_PWT | (platform.nx ? PAGE_NX : 0));
    if (vm_ready)
        arch_write_cr3((UINT64)(UINTN)kernel_pml4);
    k_spin_unlock(&vm_lock, f);
    return e;
}
int x86_64_vmm_init(void)
{
    UINT32 a, b, c, d;
    arch_cpuid(0x80000000, 0, &a, &b, &c, &d);
    UINT32 extended_max = a;
    if (a >= 0x80000001) {
        arch_cpuid(0x80000001, 0, &a, &b, &c, &d);
        platform.nx = (d & (1 << 20)) != 0;
    }
    if (extended_max >= 0x80000008) {
        arch_cpuid(0x80000008, 0, &a, &b, &c, &d);
        saved_phys_bits = a & 255;
    }
    arch_cpuid(1, 0, &a, &b, &c, &d);
    has_pat = (d & (1 << 16)) != 0;
    has_mtrr = (d & (1 << 12)) != 0;
    if (has_pat) {
        saved_pat = arch_rdmsr(0x277);
        if ((saved_pat & 255) != 6 || ((saved_pat >> 24) & 255) != 0)
            return K_ENOTSUP;
    }
    if (has_mtrr) {
        UINT64 cap = arch_rdmsr(0xfe);
        saved_mtrr_count = (UINT32)(cap & 255);
        if (saved_mtrr_count > 32)
            return K_ENOTSUP;
        mtrr_fixed_supported = (cap & (1 << 8)) != 0;
        saved_mtrr_default = arch_rdmsr(0x2ff);
        for (UINT32 i = 0; i < saved_mtrr_count * 2; ++i)
            saved_mtrr_variable[i] = arch_rdmsr(0x200 + i);
        if (mtrr_fixed_supported) {
            static const UINT16 msrs[] = {0x250, 0x258, 0x259, 0x268, 0x269, 0x26a,
                                          0x26b, 0x26c, 0x26d, 0x26e, 0x26f};
            for (UINT32 i = 0; i < 11; ++i)
                saved_mtrr_fixed[i] = arch_rdmsr(msrs[i]);
        }
    }
    UINT64 cr4;
    cr4 = arch_read_cr4();
    if (cr4 & (1ULL << 12))
        return K_ENOTSUP;
    if (platform.nx)
        arch_wrmsr(0xc0000080, arch_rdmsr(0xc0000080) | (1 << 11));
    kernel_pml4 = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
    if (!kernel_pml4)
        return K_ENOMEM;
    for (UINTN i = 0; i < boot_entries; ++i) {
        EFI_MEMORY_DESCRIPTOR *m = (void *)((UINT8 *)boot_map + i * boot_desc_size);
        if (m->NumberOfPages > ~0ULL / 4096)
            return K_EINVAL;
        if (m->Type == EfiUnusableMemory || m->Type == EfiMemoryMappedIOPortSpace ||
            m->PhysicalStart >= K_USER_BASE)
            continue;
        UINT64 flags = PAGE_RW;
        /* EFI memory attributes advertise supported cache modes, not one selected mode.
         * Prefer WB for ordinary RAM; UC there would uncache kernel data on every CPU. */
        UINT64 cache_caps =
            m->Attribute & (EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB);
        BOOLEAN ordinary_ram = m->Type == EfiConventionalMemory || m->Type == EfiLoaderCode ||
                               m->Type == EfiLoaderData || m->Type == EfiBootServicesCode ||
                               m->Type == EfiBootServicesData ||
                               m->Type == EfiRuntimeServicesCode ||
                               m->Type == EfiRuntimeServicesData ||
                               m->Type == EfiACPIReclaimMemory || m->Type == EfiACPIMemoryNVS;
        if (m->Type == EfiMemoryMappedIO ||
            (!(cache_caps & EFI_MEMORY_WB) && !(ordinary_ram && !cache_caps)))
            flags |= PAGE_PCD | PAGE_PWT;
        /* Firmware/loader code keeps executable supervisor identity mappings. */
        if (platform.nx && m->Type != EfiLoaderCode && m->Type != EfiBootServicesCode &&
            m->Type != EfiRuntimeServicesCode && m->Type != EfiReservedMemoryType)
            flags |= PAGE_NX;
        int e = x86_64_map_identity(m->PhysicalStart, m->NumberOfPages * 4096, flags);
        if (e)
            return e;
    }
    /* Framebuffer may be absent from the EFI memory map. */
    if (fb_base) {
        int e = x86_64_map_identity((UINT64)(UINTN)fb_base, (UINT64)fb_pitch * fb_height * 4,
                                    PAGE_RW | PAGE_PCD | PAGE_PWT | (platform.nx ? PAGE_NX : 0));
        if (e)
            return e;
    }

    UINT64 *ignored;
    int e = x86_64_pt_child(kernel_pml4, (K_TEST_VA >> 39) & 511, 4, FALSE, &ignored);
    if (e)
        return e;
    /* Change cache policy with interrupts/caching disabled before AP start to avoid WB/UC aliases.
     */
    UINT64 irq = k_irq_save(), cr0;
    cr0 = arch_read_cr0();
    cr0 = (cr0 | (1ULL << 16)) & ~((1ULL << 30) | (1ULL << 29));
    UINT64 uncached_cr0 = cr0 | (1ULL << 30);
    arch_cache_disable(uncached_cr0);
    /* Drop inherited PCID/global translations before enabling isolation. */
    if (cr4 & (1ULL << 17)) {
        UINT64 old_cr3;
        old_cr3 = arch_read_cr3();
        old_cr3 &= PHYS_MASK;
        arch_write_cr3((UINT64)(UINTN)old_cr3);
    }
    cr4 &= ~((1ULL << 17) | (1ULL << 7));
    arch_write_cr4((UINT64)(UINTN)cr4);
    arch_write_cr3((UINT64)(UINTN)kernel_pml4);
    vm_ready = TRUE;
    /* PAT[0]=WB, PAT[3]=UC, PAT[1]=WC for the framebuffer. */
    if (has_pat && fb_base) {
        saved_pat = (saved_pat & ~(255ULL << 8)) | (1ULL << 8);
        arch_wrmsr(0x277, saved_pat);
        e = x86_64_map_identity((UINT64)(UINTN)fb_base, (UINT64)fb_pitch * fb_height * 4,
                                PAGE_RW | PAGE_PWT | (platform.nx ? PAGE_NX : 0));
        fb_pat_wc = !e; /* MTRRs may still impose a stricter type. */
    }
    arch_cache_flush_root((UINT64)(UINTN)kernel_pml4);
    arch_write_cr0((UINT64)(UINTN)cr0);
    k_irq_restore(irq);
    if (e)
        return e;
    console_buffer_init();
    return 0;
}
UINT64 *x86_64_get_pml4(void) { return kernel_pml4; }

k_address_space *x86_64_as_create(void)
{
    UINT64 f = k_spin_lock(&vm_lock);
    for (UINT32 i = 0; i < MAX_AS; ++i)
        if (!spaces[i].used) {
            UINT64 *p = (void *)(UINTN)pmm_alloc_page();
            if (!p)
                break;
            mem_copy(p, kernel_pml4, 4096);
            p[USER_SLOT] = 0;
            spaces[i] =
                (k_address_space){.used = TRUE, .mmu.root = p, .next = K_USER_BASE + 0x200000};
            spaces[i].mmu.owned_slots[USER_SLOT / 64] |= 1ULL << (USER_SLOT % 64);
            k_spin_unlock(&vm_lock, f);
            return &spaces[i];
        }
    k_spin_unlock(&vm_lock, f);
    return NULL;
}
BOOLEAN x86_64_as_valid(k_address_space *as)
{
    UINTN p = (UINTN)as, b = (UINTN)spaces;
    return p >= b && p < b + sizeof(spaces) && !((p - b) % sizeof(*as)) && as->used;
}
static void free_user_tree(UINT64 *table, UINT32 level)
{
    for (UINT32 i = 0; i < 512; ++i)
        if (table[i] & PAGE_PRESENT) {
            UINT64 p = table[i] & PHYS_MASK;
            if (level == 2 && (table[i] & PAGE_LARGE)) {
                k_pmm_free_pages(p, 512);
                continue;
            }
            if (level > 1)
                free_user_tree((void *)(UINTN)p, level - 1);
            pmm_free_page(p);
        }
}
int x86_64_as_destroy(k_address_space *as)
{
    if (!x86_64_as_valid(as))
        return K_EINVAL;
    if (as->owner)
        return K_EBUSY;
    UINT64 f = k_spin_lock(&sched_lock);
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i)
        if (tasks[i].state != K_TASK_UNUSED && tasks[i].state != K_TASK_ZOMBIE &&
            tasks[i].process && tasks[i].process->as == as) {
            k_spin_unlock(&sched_lock, f);
            return K_EBUSY;
        }
    k_spin_unlock(&sched_lock, f);
    f = k_spin_lock(&vm_lock);
    for (UINT32 slot = 0; slot < 256; ++slot)
        if ((as->mmu.owned_slots[slot / 64] & (1ULL << (slot % 64))) &&
            (as->mmu.root[slot] & PAGE_PRESENT)) {
            UINT64 p = as->mmu.root[slot] & PHYS_MASK;
            free_user_tree((void *)(UINTN)p, 3);
            pmm_free_page(p);
        }
    pmm_free_page((UINT64)(UINTN)as->mmu.root);
    mem_zero(as, sizeof(*as));
    k_spin_unlock(&vm_lock, f);
    return 0;
}
int x86_64_as_alloc(k_address_space *as, UINT64 bytes, UINT64 align, UINT32 rwx, UINT64 *base)
{
    if (!x86_64_as_valid(as) || !base || !bytes || !power2(align) || align > 0x40000000 ||
        (rwx & ~7) || !(rwx & RIEF_REGION_R) ||
        ((rwx & (RIEF_REGION_W | RIEF_REGION_X)) == (RIEF_REGION_W | RIEF_REGION_X)))
        return K_EINVAL;
    if (!platform.nx)
        return K_ENOTSUP;
    if (bytes > MAX_USER_PAGES * 4096ULL)
        return K_E2BIG;
    UINT64 n = ALIGN_UP(bytes, 4096) / 4096;
    align = align < 4096 ? 4096 : align;
    UINT64 f = k_spin_lock(&vm_lock), v = ALIGN_UP(as->next, align);
    if (n > MAX_USER_PAGES - as->pages || v >= K_USER_LIMIT || n * 4096 + 4096 > K_USER_LIMIT - v) {
        k_spin_unlock(&vm_lock, f);
        return K_ENOMEM;
    }
    UINT64 flags =
        PAGE_USER | ((rwx & RIEF_REGION_W) ? PAGE_RW : 0) | ((rwx & RIEF_REGION_X) ? 0 : PAGE_NX);
    UINT64 done = 0;
    int e = 0;
    for (; done < n; ++done) {
        UINT64 p = pmm_alloc_page();
        if (!p) {
            e = K_ENOMEM;
            break;
        }
        e = x86_64_map_page_raw(as->mmu.root, v + done * 4096, p, flags, FALSE);
        if (e) {
            pmm_free_page(p);
            break;
        }
    }
    if (e) {
        for (UINT64 j = 0; j < done; ++j) {
            UINT64 *t = as->mmu.root, a = v + j * 4096;
            for (int l = 4; l > 1; --l)
                t = (void *)(UINTN)(t[(a >> (12 + 9 * (l - 1))) & 511] & PHYS_MASK);
            UINT64 p = t[(a >> 12) & 511] & PHYS_MASK;
            t[(a >> 12) & 511] = 0;
            pmm_free_page(p);
        }
    } else {
        as->next = v + n * 4096 + 4096;
        as->pages += (UINT32)n;
        *base = v;
    }
    k_spin_unlock(&vm_lock, f);
    return e;
}
int x86_64_as_check(k_address_space *as, UINT64 address, UINT64 bytes, BOOLEAN write)
{
    if (!x86_64_as_valid(as) || address < 0x10000 || address >= K_USER_CANON_LIMIT ||
        bytes > K_USER_CANON_LIMIT - address)
        return K_EFAULT;
    if (!bytes)
        return 0;
    UINT64 end = address + bytes;
    for (UINT64 v = address & ~4095ULL; v < end; v += 4096) {
        UINT64 flags;
        if (x86_64_vmm_translate(as->mmu.root, v, NULL, &flags)) {
            if (x86_64_demand_fault(as, v, write ? 2u : 0u))
                return K_EFAULT;
        }
        if (x86_64_vmm_translate(as->mmu.root, v, NULL, &flags) || !(flags & PAGE_USER) ||
            (write && !(flags & PAGE_RW)))
            return K_EFAULT;
    }
    return 0;
}
/* User copies use supervisor physical aliases; untrusted VAs are never dereferenced directly. */
int x86_64_as_copy(k_address_space *as, UINT64 u, void *buf, UINT64 n, BOOLEAN to, BOOLEAN loader)
{
    int e = x86_64_as_check(as, u, n, to && !loader);
    if (e)
        return e;
    UINT8 *b = buf;
    while (n) {
        UINT64 p;
        e = x86_64_vmm_translate(as->mmu.root, u, &p, NULL);
        if (e)
            return e;
        UINT64 chunk = MIN(n, 4096 - (u & 4095));
        if (to)
            mem_copy((void *)(UINTN)p, b, chunk);
        else
            mem_copy(b, (void *)(UINTN)p, chunk);
        u += chunk;
        b += chunk;
        n -= chunk;
    }
    return 0;
}
int x86_64_copy_to_user(k_address_space *as, UINT64 u, const void *b, UINT64 n)
{
    return x86_64_as_copy(as, u, (void *)b, n, TRUE, FALSE);
}
int x86_64_copy_from_user(k_address_space *as, void *b, UINT64 u, UINT64 n)
{
    return x86_64_as_copy(as, u, b, n, FALSE, FALSE);
}

/* RIEF load rollback removes only mappings created by that load. */
static BOOLEAN rollback_tree(UINT64 *table, UINT32 level, UINT64 base, UINT64 from)
{
    BOOLEAN empty = TRUE;
    UINT64 span = 1ULL << (12 + 9 * (level - 1));
    for (UINT32 i = 0; i < 512; ++i) {
        UINT64 entry = table[i];
        if (!(entry & PAGE_PRESENT)) {
            if ((entry & X86_LAZY) && base + i * span >= from)
                table[i] = 0;
            else if (entry)
                empty = FALSE;
            continue;
        }
        UINT64 address = base + i * span, phys = entry & PHYS_MASK;
        if (address + span > from) {
            if (level == 2 && (entry & PAGE_LARGE) && address >= from) {
                k_pmm_free_pages(phys, 512);
                table[i] = 0;
                continue;
            }
            if (level == 1) {
                pmm_free_page(phys);
                table[i] = 0;
                continue;
            }
            if (rollback_tree((void *)(UINTN)phys, level - 1, address, from)) {
                pmm_free_page(phys);
                table[i] = 0;
                continue;
            }
        }
        empty = FALSE;
    }
    return empty;
}

void x86_64_as_rollback(k_address_space *as, UINT64 old_next, UINT32 old_pages)
{
    UINT64 f = k_spin_lock(&vm_lock), entry = as->mmu.root[USER_SLOT];
    if (entry & PAGE_PRESENT) {
        UINT64 phys = entry & PHYS_MASK;
        if (rollback_tree((void *)(UINTN)phys, 3, K_USER_BASE, old_next)) {
            pmm_free_page(phys);
            as->mmu.root[USER_SLOT] = 0;
        }
    }
    as->next = old_next;
    as->pages = old_pages;
    k_spin_unlock(&vm_lock, f);
}

int x86_64_as_map_at(k_address_space *as, UINT64 address, UINT64 bytes, UINT32 rwx)
{
    if (!bytes || bytes > MAX_USER_PAGES * 4096ULL || (address & 4095) || address < 0x10000 ||
        address >= K_USER_CANON_LIMIT || bytes > K_USER_CANON_LIMIT - address ||
        !(rwx & RIEF_REGION_R) || (rwx & ~7) || ((rwx & 6) == 6) || !platform.nx)
        return K_EINVAL;
    UINT64 count = (bytes + 4095) / 4096, slot = address >> 39;
    if ((address + count * 4096 - 1) >> 39 != slot)
        return K_EINVAL;
    if (kernel_pml4[slot] & PAGE_PRESENT)
        return K_EPERM;
    UINT64 irq = k_spin_lock(&vm_lock);
    int e = 0;
    if (count > MAX_USER_PAGES - as->pages) {
        e = K_ENOMEM;
        goto done;
    }
    for (UINT64 i = 0; i < count; ++i)
        if (!x86_64_vmm_translate(as->mmu.root, address + i * 4096, NULL, NULL)) {
            e = K_EEXIST;
            goto done;
        }
    as->mmu.owned_slots[slot / 64] |= 1ULL << (slot % 64);
    UINT64 flags = PAGE_USER | ((rwx & 2) ? PAGE_RW : 0) | ((rwx & 4) ? 0 : PAGE_NX), made = 0;
    for (; made < count; ++made) {
        UINT64 pa = pmm_alloc_page();
        if (!pa) {
            e = K_ENOMEM;
            break;
        }
        e = x86_64_map_page_raw(as->mmu.root, address + made * 4096, pa, flags, FALSE);
        if (e) {
            pmm_free_page(pa);
            break;
        }
    }
    if (e) {
        for (UINT64 i = 0; i < made; ++i) {
            UINT64 va = address + i * 4096, *t = as->mmu.root;
            for (int level = 4; level > 1; --level)
                t = (void *)(UINTN)(t[(va >> (12 + 9 * (level - 1))) & 511] & PHYS_MASK);
            UINT32 idx = (va >> 12) & 511;
            pmm_free_page(t[idx] & PHYS_MASK);
            t[idx] = 0;
        }
    } else
        as->pages += (UINT32)count;
done:
    k_spin_unlock(&vm_lock, irq);
    return e;
}
