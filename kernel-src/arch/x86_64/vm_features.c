// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"
#include "x86_64_internal.h"

static int leaf_for(k_address_space *as, UINT64 v, UINT32 level, BOOLEAN create, UINT64 **out)
{
    UINT64 *t = as->mmu.root;
    for (UINT32 l = 4; l > level; --l) {
        UINT32 i = (v >> (12 + 9 * (l - 1))) & 511;
        if (!create && (!(t[i] & PAGE_PRESENT) || (t[i] & PAGE_LARGE)))
            return K_EFAULT;
        int e = x86_64_pt_child(t, i, l, TRUE, &t);
        if (e)
            return e;
    }
    *out = &t[(v >> (12 + 9 * (level - 1))) & 511];
    return 0;
}
int x86_64_as_alloc_ex(k_address_space *as, UINT64 bytes, UINT64 align, UINT32 rwx, UINT32 options,
                       UINT64 *base)
{
    if (!options)
        return x86_64_as_alloc(as, bytes, align, rwx, base);
    if (!x86_64_as_valid(as) || !base || !bytes || bytes > MAX_USER_PAGES * 4096ULL ||
        !power2(align) || align > 0x40000000 || !(rwx & 1) || (rwx & ~7u) || (rwx & 6) == 6 ||
        (options != K_VM_LAZY && options != K_VM_HUGE))
        return K_EINVAL;
    if (!platform.nx)
        return K_ENOTSUP;
    if (options == K_VM_LAZY && (rwx & RIEF_REGION_X))
        return K_EPERM;
    UINT64 unit = options == K_VM_HUGE ? 0x200000 : 4096;
    if (options == K_VM_HUGE && (bytes & (unit - 1)))
        return K_EINVAL;
    if (align < unit)
        align = unit;
    bytes = ALIGN_UP(bytes, 4096);
    UINT64 f = k_spin_lock(&vm_lock), v = ALIGN_UP(as->next, align), done = 0;
    int e = 0;
    if (bytes / 4096 > MAX_USER_PAGES - as->pages || v >= K_USER_LIMIT ||
        bytes + 4096 > K_USER_LIMIT - v) {
        e = K_ENOMEM;
        goto out;
    }
    UINT64 flags = PAGE_USER | ((rwx & 2) ? PAGE_RW : 0) | ((rwx & 4) ? 0 : PAGE_NX);
    for (; done < bytes; done += unit) {
        UINT64 *leaf;
        e = leaf_for(as, v + done, options == K_VM_HUGE ? 2 : 1, TRUE, &leaf);
        if (e)
            break;
        if (*leaf) {
            e = K_EEXIST;
            break;
        }
        if (options == K_VM_LAZY)
            *leaf = flags | X86_LAZY;
        else {
            UINT64 phys = k_pmm_alloc_aligned(512, 512, 0);
            if (!phys) {
                e = K_ENOMEM;
                break;
            }
            *leaf = phys | flags | PAGE_PRESENT | PAGE_LARGE;
        }
        x86_64_invalidate_page(v + done);
    }
    if (e) {
        while (done) {
            done -= unit;
            UINT64 *leaf;
            if (!leaf_for(as, v + done, options == K_VM_HUGE ? 2 : 1, FALSE, &leaf)) {
                if (*leaf & PAGE_PRESENT)
                    k_pmm_free_pages(*leaf & PHYS_MASK, 512);
                *leaf = 0;
                x86_64_invalidate_page(v + done);
            }
        }
    } else {
        as->next = v + bytes + 4096;
        as->pages += (UINT32)(bytes / 4096);
        *base = v;
    }
out:
    k_spin_unlock(&vm_lock, f);
    return e;
}
int x86_64_demand_fault(k_address_space *as, UINT64 address, UINT64 error)
{
    if (!x86_64_as_valid(as) || address < K_USER_BASE || address >= K_USER_LIMIT || (error & ~6ULL))
        return K_EFAULT;
    UINT64 f = k_spin_lock(&vm_lock), *leaf;
    int e = leaf_for(as, address, 1, FALSE, &leaf);
    if (!e) {
        UINT64 entry = *leaf;
        if (!(entry & X86_LAZY) || (entry & PAGE_PRESENT) || !(entry & PAGE_USER) ||
            ((error & 2) && !(entry & PAGE_RW)))
            e = K_EFAULT;
        else {
            UINT64 phys = pmm_alloc_page();
            if (!phys)
                e = K_ENOMEM;
            else {
                *leaf = phys | (entry & ~X86_LAZY) | PAGE_PRESENT;
                x86_64_invalidate_page(address);
            }
        }
    }
    k_spin_unlock(&vm_lock, f);
    return e;
}
int x86_64_as_mapping_size(k_address_space *as, UINT64 address, UINT64 *bytes)
{
    if (!x86_64_as_valid(as) || !bytes || address < 0x10000 || address >= K_USER_CANON_LIMIT)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&vm_lock), *t = as->mmu.root;
    int result = K_ENOENT;
    for (UINT32 l = 4; l; --l) {
        UINT64 e = t[(address >> (12 + 9 * (l - 1))) & 511];
        if (!(e & PAGE_USER))
            break;
        if (!(e & PAGE_PRESENT)) {
            if (l == 1 && (e & X86_LAZY)) {
                *bytes = 0;
                result = 0;
            }
            break;
        }
        if (l == 1 || (l == 2 && (e & PAGE_LARGE))) {
            *bytes = 1ULL << (12 + 9 * (l - 1));
            result = 0;
            break;
        }
        t = (void *)(UINTN)(e & PHYS_MASK);
    }
    k_spin_unlock(&vm_lock, f);
    return result;
}
