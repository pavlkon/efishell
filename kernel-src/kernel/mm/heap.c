// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"

#define ARENAS 64u
#define UNITS 1024u
#define UNIT 64u
#define CANARY 0xd831ad7e56b0c429ULL
typedef struct {
    UINT64 base, used[16];
    UINT16 length[UNITS];
    UINT32 requested[UNITS], live;
} arena;
typedef struct {
    UINT64 base, pointer, requested;
    UINT32 pages;
} large_block;
static arena arenas[ARENAS];
static large_block large[64];
static k_spinlock heap_lock = K_SPINLOCK_INIT;
static k_heap_info heap_stats;
static BOOLEAN occupied(arena *a, UINT32 u) { return (a->used[u / 64] >> (u % 64)) & 1; }
void *k_heap_alloc(UINT64 bytes, UINT32 alignment)
{
    if (!bytes || bytes > 64ULL * 1024 * 1024 || !power2(alignment) || alignment > 0x200000)
        return NULL;
    if (alignment < 16)
        alignment = 16;
    UINT64 irq = k_spin_lock(&heap_lock), result = 0;
    if (bytes + 8 > 32768 || alignment > 4096) {
        for (UINT32 i = 0; i < ARRAY_LEN(large); ++i)
            if (!large[i].base) {
                UINT32 pages = (UINT32)((bytes + alignment + 7 + 4095) / 4096);
                UINT64 base = k_pmm_alloc_pages(pages, 0);
                if (!base)
                    break;
                result = ALIGN_UP(base, alignment);
                large[i] = (large_block){base, result, bytes, pages};
                heap_stats.committed_bytes += (UINT64)pages * 4096;
                break;
            }
    } else {
        UINT32 needed = (UINT32)((bytes + 8 + UNIT - 1) / UNIT);
        for (UINT32 i = 0; i < ARENAS && !result; ++i) {
            arena *a = &arenas[i];
            if (!a->base) {
                a->base = k_pmm_alloc_pages(16, 0);
                if (!a->base)
                    break;
                heap_stats.committed_bytes += 65536;
            }
            for (UINT32 u = 0; u + needed <= UNITS;) {
                if ((a->base + (UINT64)u * UNIT) & (alignment - 1)) {
                    ++u;
                    continue;
                }
                UINT32 j = 0;
                while (j < needed && !occupied(a, u + j))
                    ++j;
                if (j != needed) {
                    u += j + 1;
                    continue;
                }
                for (j = 0; j < needed; ++j)
                    a->used[(u + j) / 64] |= 1ULL << ((u + j) % 64);
                a->length[u] = (UINT16)needed;
                a->requested[u] = (UINT32)bytes;
                ++a->live;
                result = a->base + (UINT64)u * UNIT;
                break;
            }
        }
    }
    if (result) {
        mem_zero((void *)(UINTN)result, bytes);
        wr64((void *)(UINTN)(result + bytes), CANARY ^ result ^ bytes);
        heap_stats.live_bytes += bytes;
        ++heap_stats.allocations;
    }
    k_spin_unlock(&heap_lock, irq);
    return (void *)(UINTN)result;
}
int k_heap_free(void *pointer)
{
    if (!pointer)
        return 0;
    UINT64 p = (UINT64)(UINTN)pointer, irq = k_spin_lock(&heap_lock);
    int e = K_EINVAL;
    for (UINT32 i = 0; i < ARRAY_LEN(large); ++i)
        if (large[i].pointer == p) {
            large_block *b = &large[i];
            if (rd64((void *)(UINTN)(p + b->requested)) != (CANARY ^ p ^ b->requested)) {
                e = K_EFAULT;
                goto done;
            }
            heap_stats.live_bytes -= b->requested;
            heap_stats.committed_bytes -= (UINT64)b->pages * 4096;
            k_pmm_free_pages(b->base, b->pages);
            mem_zero(b, sizeof(*b));
            e = 0;
            goto done;
        }
    for (UINT32 i = 0; i < ARENAS; ++i) {
        arena *a = &arenas[i];
        if (!a->base || p < a->base || p - a->base >= 65536 || (p - a->base) % UNIT)
            continue;
        UINT32 u = (UINT32)((p - a->base) / UNIT), n = a->length[u], bytes = a->requested[u];
        if (!n || n > UNITS - u)
            goto done;
        if (rd64((void *)(UINTN)(p + bytes)) != (CANARY ^ p ^ bytes)) {
            e = K_EFAULT;
            goto done;
        }
        for (UINT32 j = 0; j < n; ++j)
            if (!occupied(a, u + j)) {
                e = K_EFAULT;
                goto done;
            }
        mem_zero(pointer, (UINTN)n * UNIT);
        for (UINT32 j = 0; j < n; ++j)
            a->used[(u + j) / 64] &= ~(1ULL << ((u + j) % 64));
        a->length[u] = 0;
        a->requested[u] = 0;
        heap_stats.live_bytes -= bytes;
        if (!--a->live) {
            k_pmm_free_pages(a->base, 16);
            heap_stats.committed_bytes -= 65536;
            mem_zero(a, sizeof(*a));
        }
        e = 0;
        goto done;
    }
done:
    if (e)
        ++heap_stats.errors;
    k_spin_unlock(&heap_lock, irq);
    return e;
}
int k_heap_get(k_heap_info *out)
{
    if (!out)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&heap_lock);
    *out = heap_stats;
    k_spin_unlock(&heap_lock, f);
    return 0;
}
