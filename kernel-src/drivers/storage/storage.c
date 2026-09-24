// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"
#include "usb_storage_internal.h"

disk disks[K_MAX_DISKS];
static UINT32 disk_count;
static BOOLEAN storage_initialized;
static k_mutex storage_mutex = K_MUTEX_INIT;
static disk *disk_add(UINT32 kind, UINT32 sector_size, UINT64 sectors, const char *model,
                      const char *driver)
{
    if (disk_count == K_MAX_DISKS || !sectors || !power2(sector_size) || sector_size < 512 ||
        sector_size > 4096 || sectors > ~0ULL / sector_size)
        return NULL;
    disk *d = &disks[disk_count];
    mem_zero(d, sizeof(*d));
    d->kind = kind;
    d->info.id = disk_count;
    d->info.sector_size = sector_size;
    d->info.sectors = sectors;
    d->info.read_only = kind != DISK_RAM;
    d->info.online = TRUE;
    str_copy(d->info.model, model, sizeof(d->info.model));
    str_copy(d->info.driver, driver, sizeof(d->info.driver));
    decimal_name(d->info.name, "disk", disk_count);
    return d;
}
static void trim_model(char *m)
{
    UINTN n = str_len(m);
    while (n && m[n - 1] == ' ')
        m[--n] = 0;
}
static int ata_identity(const UINT8 *id, UINT32 *sector, UINT64 *count, char model[48],
                        BOOLEAN *lba48)
{
    if (!(rd16(id + 98) & (1 << 9)))
        return K_ENOTSUP;
    UINT16 w83 = rd16(id + 166);
    *lba48 = (w83 & 0xc400) == 0x4400;
    *count = *lba48 ? rd64(id + 200) : rd32(id + 120);
    if (!*count || (*lba48 && *count > (1ULL << 48)) || (!*lba48 && *count > (1ULL << 28)))
        return K_ENOTSUP;
    *sector = 512;
    UINT16 w106 = rd16(id + 212);
    if ((w106 & 0xd000) == 0x5000) {
        UINT32 words = rd32(id + 234);
        if (words < 256 || words > 2048)
            return K_ENOTSUP;
        *sector = words * 2;
    }
    if (!power2(*sector))
        return K_ENOTSUP;
    for (UINT32 i = 0; i < 40; i += 2) {
        model[i] = (char)id[54 + i + 1];
        model[i + 1] = (char)id[54 + i];
    }
    model[40] = 0;
    trim_model(model);
    return 0;
}

/* NVMe uses one admin and one I/O queue pair; DMA buffers are pinned 4 KiB pages. */
#define NVME_DEPTH 32
typedef struct {
    volatile UINT32 dw[16];
} nvme_command;
typedef struct {
    volatile UINT32 result, reserved;
    volatile UINT16 head, sqid, cid, status;
} nvme_completion;
_Static_assert(sizeof(nvme_command) == 64 && sizeof(nvme_completion) == 16, "NVMe ABI");
typedef struct {
    nvme_command *sq;
    nvme_completion *cq;
    UINT16 tail, head, cid, depth, id;
    UINT8 phase;
} nvme_queue;
typedef struct {
    UINT16 bdf;
    volatile UINT8 *regs;
    UINT32 stride, timeout_ms;
    BOOLEAN dead;
    nvme_queue admin, io;
    UINT8 *data;
} nvme_controller;
static nvme_controller nvme[8];
static UINT32 nvme_count;
static void nvme_dead(nvme_controller *n)
{
    n->dead = TRUE;
    pci_command(n->bdf, 0, 4); /* After a DMA failure, pinned pages are not recycled. */
    for (UINT32 i = 0; i < k_disk_count(); ++i)
        if (disks[i].kind == DISK_NVME && &nvme[disks[i].controller] == n)
            __atomic_store_n(&disks[i].info.online, FALSE, __ATOMIC_RELEASE);
}
static int nvme_submit(nvme_controller *n, nvme_queue *q, const UINT32 cmd[16], UINT32 *result)
{
    if (n->dead)
        return K_EIO;
    UINT16 cid = ++q->cid;
    for (UINT32 i = 0; i < 16; ++i)
        q->sq[q->tail].dw[i] = cmd[i];
    q->sq[q->tail].dw[0] = (cmd[0] & 0xffff) | ((UINT32)cid << 16);
    arch_write_fence();
    q->tail = (q->tail + 1) % q->depth;
    *(volatile UINT32 *)(n->regs + 0x1000 + (2 * q->id) * n->stride) = q->tail;
    UINT32 budget = n->timeout_ms * 10;
    while (budget--) {
        UINT16 status = q->cq[q->head].status;
        if ((status & 1) == q->phase) {
            arch_read_fence();
            if (q->cq[q->head].cid != cid || q->cq[q->head].sqid != q->id) {
                nvme_dead(n);
                return K_EIO;
            }
            if (result)
                *result = q->cq[q->head].result;
            ++q->head;
            if (q->head == q->depth) {
                q->head = 0;
                q->phase ^= 1;
            }
            arch_write_fence();
            *(volatile UINT32 *)(n->regs + 0x1000 + (2 * q->id + 1) * n->stride) = q->head;
            return (status >> 1) ? K_EIO : 0;
        }
        if (*(volatile UINT32 *)(n->regs + 0x1c) & 2) {
            nvme_dead(n);
            return K_EIO;
        }
        if (arch_delay_checked(100)) {
            nvme_dead(n);
            return K_ETIMEDOUT;
        }
    }
    nvme_dead(n);
    return K_ETIMEDOUT;
}
static int nvme_identify(nvme_controller *n, UINT32 ns, UINT32 cns)
{
    UINT32 cmd[16] = {0};
    cmd[0] = 6;
    cmd[1] = ns;
    cmd[6] = (UINT32)(UINTN)n->data;
    cmd[7] = (UINT32)((UINT64)(UINTN)n->data >> 32);
    cmd[10] = cns;
    mem_zero(n->data, 4096);
    return nvme_submit(n, &n->admin, cmd, NULL);
}
static int nvme_queue_alloc(nvme_queue *q, UINT16 id, UINT16 depth)
{
    q->sq = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
    q->cq = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
    q->id = id;
    q->depth = depth;
    q->phase = 1;
    return q->sq && q->cq ? 0 : K_ENOMEM;
}
static int nvme_probe(UINT16 bdf)
{
    if (!platform.dma_allowed)
        return K_ENOTSUP;
    if (nvme_count == ARRAY_LEN(nvme))
        return K_E2BIG;
    UINT64 base = pci_mbar(bdf, 0x10);
    if (!base || k_mmio_map(base, 8192))
        return K_ENOTSUP;
    pci_command(bdf, 2, 4);
    nvme_controller *n = &nvme[nvme_count];
    mem_zero(n, sizeof(*n));
    n->bdf = bdf;
    n->regs = (void *)(UINTN)base;
    UINT64 cap = *(volatile UINT64 *)n->regs;
    if (!(cap & (1ULL << 37)) || ((cap >> 48) & 15) != 0 || !(cap & 0xffff))
        return K_ENOTSUP;
    n->stride = 4U << ((cap >> 32) & 15);
    if (k_mmio_map(base, 0x1000 + 4ULL * n->stride))
        return K_ENOTSUP;
    n->timeout_ms = (UINT32)((cap >> 24) & 255) * 500;
    if (n->timeout_ms < 5000)
        n->timeout_ms = 5000;
    *(volatile UINT32 *)(n->regs + 0x0c) = 0xffffffff;
    *(volatile UINT32 *)(n->regs + 0x14) &= ~1U;
    if (!arch_wait_reg((void *)(n->regs + 0x1c), 1, 0, n->timeout_ms))
        return K_ETIMEDOUT;
    UINT16 depth = (UINT16)MIN((cap & 0xffff) + 1, NVME_DEPTH);
    if (nvme_queue_alloc(&n->admin, 0, depth) || nvme_queue_alloc(&n->io, 1, depth))
        return K_ENOMEM;
    n->data = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
    if (!n->data)
        return K_ENOMEM;
    ++nvme_count;
    *(volatile UINT32 *)(n->regs + 0x24) = ((UINT32)(depth - 1) << 16) | (depth - 1);
    *(volatile UINT64 *)(n->regs + 0x28) = (UINT64)(UINTN)n->admin.sq;
    *(volatile UINT64 *)(n->regs + 0x30) = (UINT64)(UINTN)n->admin.cq;
    arch_write_fence();
    pci_command(bdf, 6, 0);
    *(volatile UINT32 *)(n->regs + 0x14) = 1 | (6 << 16) | (4 << 20);
    if (!arch_wait_reg((void *)(n->regs + 0x1c), 1, 1, n->timeout_ms)) {
        nvme_dead(n);
        return K_ETIMEDOUT;
    }
    int e = nvme_identify(n, 0, 1);
    if (e) {
        nvme_dead(n);
        return e;
    }
    UINT32 namespace_count = rd32(n->data + 516);
    if ((n->data[512] & 15) > 6 || (n->data[512] >> 4) < 6 || (n->data[513] & 15) > 4 ||
        (n->data[513] >> 4) < 4) {
        nvme_dead(n);
        return K_ENOTSUP;
    }
    char model[48];
    mem_copy(model, n->data + 24, 40);
    model[40] = 0;
    trim_model(model);
    UINT32 cmd[16] = {0};
    cmd[0] = 5;
    cmd[6] = (UINT32)(UINTN)n->io.cq;
    cmd[7] = (UINT32)((UINT64)(UINTN)n->io.cq >> 32);
    cmd[10] = 1 | ((UINT32)(depth - 1) << 16);
    cmd[11] = 1;
    e = nvme_submit(n, &n->admin, cmd, NULL);
    if (e) {
        nvme_dead(n);
        return e;
    }
    mem_zero(cmd, sizeof(cmd));
    cmd[0] = 1;
    cmd[6] = (UINT32)(UINTN)n->io.sq;
    cmd[7] = (UINT32)((UINT64)(UINTN)n->io.sq >> 32);
    cmd[10] = 1 | ((UINT32)(depth - 1) << 16);
    cmd[11] = (1 << 16) | 1;
    e = nvme_submit(n, &n->admin, cmd, NULL);
    if (e) {
        nvme_dead(n);
        return e;
    }
    UINT32 ids[K_MAX_DISKS], count = 0;
    e = nvme_identify(n, 0, 2);
    if (!e) {
        for (UINT32 i = 0; i < K_MAX_DISKS; ++i) {
            UINT32 id = rd32(n->data + 4 * i);
            if (!id)
                break;
            ids[count++] = id;
        }
    } else if (!n->dead) {

        for (UINT32 i = 1; i <= namespace_count && count < K_MAX_DISKS; ++i)
            ids[count++] = i;
    }
    for (UINT32 i = 0; i < count && disk_count < K_MAX_DISKS && !n->dead; ++i) {
        if (nvme_identify(n, ids[i], 0))
            continue;
        UINT64 sectors = rd64(n->data), capacity = rd64(n->data + 8);
        UINT8 flbas = n->data[26], format = flbas & 15;
        if (!sectors || !capacity || capacity > sectors || (flbas & 0xe0) || format > n->data[25] ||
            rd16(n->data + 128 + 4 * format) || (n->data[29] & 7))
            continue;
        UINT8 shift = n->data[130 + 4 * format];
        if (shift < 9 || shift > 12)
            continue;
        disk *d = disk_add(DISK_NVME, 1U << shift, sectors, model, "nvme");
        if (d) {
            d->controller = nvme_count - 1;
            d->nsid = ids[i];
            d->flush_command = 1;
            __atomic_add_fetch(&disk_count, 1, __ATOMIC_RELEASE);
        }
    }
    return n->dead ? K_EIO : 0;
}
static int nvme_read(disk *d, UINT64 lba, UINT32 count, void *out)
{
    nvme_controller *n = &nvme[d->controller];
    UINT8 *p = out;
    while (count) {
        UINT32 sectors = MIN(count, 4096 / d->info.sector_size), cmd[16] = {0};
        cmd[0] = 2;
        cmd[1] = d->nsid;
        cmd[6] = (UINT32)(UINTN)n->data;
        cmd[7] = (UINT32)((UINT64)(UINTN)n->data >> 32);
        cmd[10] = (UINT32)lba;
        cmd[11] = (UINT32)(lba >> 32);
        cmd[12] = sectors - 1;
        int e = nvme_submit(n, &n->io, cmd, NULL);
        if (e)
            return e;
        UINT32 bytes = sectors * d->info.sector_size;
        mem_copy(p, n->data, bytes);
        p += bytes;
        count -= sectors;
        lba += sectors;
    }
    return 0;
}

/* AHCI ports use private command/FIS/table/bounce buffers; failed DMA paths are quarantined. */
typedef struct {
    UINT16 bdf;
    volatile UINT8 *port;
    UINT8 *list, *fis, *table, *data;
    BOOLEAN dead, lba48;
} ahci_port;
static ahci_port ahci[32];
static UINT32 ahci_count;
static void ahci_dead(ahci_port *a)
{
    a->dead = TRUE;
    pci_command(a->bdf, 0, 4);
    for (UINT32 i = 0; i < ahci_count; ++i)
        if (ahci[i].bdf == a->bdf)
            ahci[i].dead = TRUE;
    for (UINT32 i = 0; i < k_disk_count(); ++i)
        if (disks[i].kind == DISK_AHCI && ahci[disks[i].controller].bdf == a->bdf)
            __atomic_store_n(&disks[i].info.online, FALSE, __ATOMIC_RELEASE);
}
static int ahci_issue(ahci_port *a, UINT8 command, UINT64 lba, UINT32 sectors, UINT32 bytes)
{
    BOOLEAN flush = command == 0xe7 || command == 0xea;
    if (a->dead || bytes > 4096 || (!flush && (!bytes || !sectors)) || sectors > 65535)
        return K_EINVAL;
    if (!arch_wait_reg((void *)(a->port + 0x20), 0x88, 0, 5000)) {
        ahci_dead(a);
        return K_ETIMEDOUT;
    }
    if (*(volatile UINT32 *)(a->port + 0x38) || *(volatile UINT32 *)(a->port + 0x34))
        return K_EBUSY;
    mem_zero(a->table, 4096);
    mem_zero(a->list, 32);
    a->list[0] = 5 | ((command == 0x35 || command == 0xca) ? 0x40 : 0);
    a->list[2] = bytes ? 1 : 0;
    wr64(a->list + 8, (UINT64)(UINTN)a->table);
    UINT8 *fis = a->table;
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = command;
    fis[7] = 0x40;
    fis[4] = (UINT8)lba;
    fis[5] = (UINT8)(lba >> 8);
    fis[6] = (UINT8)(lba >> 16);
    fis[8] = (UINT8)(lba >> 24);
    fis[9] = (UINT8)(lba >> 32);
    fis[10] = (UINT8)(lba >> 40);
    fis[12] = (UINT8)sectors;
    fis[13] = (UINT8)(sectors >> 8);
    if (command == 0xc8 || command == 0xca) {
        fis[7] |= (UINT8)((lba >> 24) & 15);
        fis[8] = fis[9] = fis[10] = 0;
    }
    wr64(a->table + 128, (UINT64)(UINTN)a->data);
    if (bytes)
        wr32(a->table + 140, bytes - 1);
    *(volatile UINT32 *)(a->port + 0x10) = 0xffffffff;
    *(volatile UINT32 *)(a->port + 0x30) = 0xffffffff;
    arch_write_fence();
    *(volatile UINT32 *)(a->port + 0x38) = 1;
    for (UINT32 i = 0; i < 50000; ++i) {
        UINT32 status = *(volatile UINT32 *)(a->port + 0x10);
        if (status & 0x7d000000U) {
            ahci_dead(a);
            return K_EIO;
        }
        if (!(*(volatile UINT32 *)(a->port + 0x38) & 1)) {
            arch_read_fence();
            if ((*(volatile UINT32 *)(a->port + 0x20) & 1) || rd32(a->list + 4) != bytes) {
                ahci_dead(a);
                return K_EIO;
            }
            return 0;
        }
        if (arch_delay_checked(100)) {
            ahci_dead(a);
            return K_ETIMEDOUT;
        }
    }
    ahci_dead(a);
    return K_ETIMEDOUT;
}
static int ahci_probe(UINT16 bdf)
{
    if (!platform.dma_allowed)
        return K_ENOTSUP;
    UINT64 base = pci_mbar(bdf, 0x24);
    if (!base || k_mmio_map(base, 8192))
        return K_ENOTSUP;
    pci_command(bdf, 2, 0);
    volatile UINT8 *hba = (void *)(UINTN)base;
    if (*(volatile UINT32 *)(hba + 0x24) & 1) {
        *(volatile UINT32 *)(hba + 0x28) |= 2;
        if (!arch_wait_reg((void *)(hba + 0x28), 0x11, 0, 5000))
            return K_EBUSY;
    }
    *(volatile UINT32 *)(hba + 4) = (*(volatile UINT32 *)(hba + 4) | 0x80000000U) & ~2U;
    UINT32 implemented = *(volatile UINT32 *)(hba + 0x0c);
    for (UINT32 port = 0; port < 32 && ahci_count < ARRAY_LEN(ahci); ++port) {
        if (!(implemented & (1U << port)))
            continue;
        volatile UINT8 *p = hba + 0x100 + port * 0x80;
        UINT32 ssts = *(volatile UINT32 *)(p + 0x28);
        if ((ssts & 15) != 3 || ((ssts >> 8) & 15) != 1 ||
            *(volatile UINT32 *)(p + 0x24) != 0x00000101)
            continue;
        *(volatile UINT32 *)(p + 0x14) = 0;
        *(volatile UINT32 *)(p + 0x18) &= ~1U;
        if (!arch_wait_reg((void *)(p + 0x18), 1 << 15, 0, 500))
            continue;
        *(volatile UINT32 *)(p + 0x18) &= ~(1U << 4);
        if (!arch_wait_reg((void *)(p + 0x18), 1 << 14, 0, 500))
            continue;
        if (*(volatile UINT32 *)(p + 0x38) || *(volatile UINT32 *)(p + 0x34))
            continue;
        ahci_port *a = &ahci[ahci_count];
        mem_zero(a, sizeof(*a));
        a->bdf = bdf;
        a->port = p;
        a->list = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
        a->fis = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
        a->table = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
        a->data = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
        if (!a->list || !a->fis || !a->table || !a->data)
            return K_ENOMEM;
        ++ahci_count;
        *(volatile UINT64 *)(p + 0) = (UINT64)(UINTN)a->list;
        *(volatile UINT64 *)(p + 8) = (UINT64)(UINTN)a->fis;
        *(volatile UINT32 *)(p + 0x30) = 0xffffffff;
        *(volatile UINT32 *)(p + 0x10) = 0xffffffff;
        pci_command(bdf, 6, 0);
        arch_write_fence();
        *(volatile UINT32 *)(p + 0x18) |= (1 << 4);
        *(volatile UINT32 *)(p + 0x18) |= 1;
        int e = ahci_issue(a, 0xec, 0, 1, 512);
        if (e)
            return e;
        UINT32 sector;
        UINT64 count;
        char model[48];
        if (ata_identity(a->data, &sector, &count, model, &a->lba48))
            continue;
        disk *d = disk_add(DISK_AHCI, sector, count, model, "ahci");
        if (d) {
            d->controller = ahci_count - 1;
            d->port = port;
            UINT16 caps = rd16(a->data + 166);
            if ((caps & 0xc000) == 0x4000)
                d->flush_command = (a->lba48 && (caps & 0x2000)) ? 0xea
                                   : (caps & 0x1000)             ? 0xe7
                                                                 : 0;
            __atomic_add_fetch(&disk_count, 1, __ATOMIC_RELEASE);
        }
    }
    return 0;
}
static int ahci_read(disk *d, UINT64 lba, UINT32 count, void *out)
{
    ahci_port *a = &ahci[d->controller];
    UINT8 *p = out;
    while (count) {
        UINT32 sectors = MIN(count, 4096 / d->info.sector_size),
               bytes = sectors * d->info.sector_size;
        int e = ahci_issue(a, a->lba48 ? 0x25 : 0xc8, lba, sectors, bytes);
        if (e)
            return e;
        mem_copy(p, a->data, bytes);
        p += bytes;
        count -= sectors;
        lba += sectors;
    }
    return 0;
}

/* IDE PIO is discovered through PCI functions, not blind I/O-port probing. */
typedef struct {
    UINT16 base, control;
    UINT8 slave;
    BOOLEAN lba48, dead;
} ata_device;
static ata_device ata[16];
static UINT32 ata_count;
static void ata_quarantine(ata_device *a)
{
    /* A timed-out ATA command quarantines the shared channel. */
    for (UINT32 i = 0; i < ata_count; ++i)
        if (ata[i].base == a->base)
            ata[i].dead = TRUE;
    for (UINT32 i = 0; i < k_disk_count(); ++i)
        if (disks[i].kind == DISK_ATA && ata[disks[i].controller].base == a->base)
            __atomic_store_n(&disks[i].info.online, FALSE, __ATOMIC_RELEASE);
}
static void ata_settle(ata_device *a)
{
    for (int i = 0; i < 4; ++i)
        (void)arch_in8(a->control);
}
static int ata_wait(ata_device *a, BOOLEAN drq)
{
    for (UINT32 i = 0; i < 50000; ++i) {
        UINT8 status = arch_in8(a->base + 7);
        if (!status || status == 0xff)
            return K_ENOENT;
        if (!(status & 0x80)) {
            if (status & 0x21)
                return K_EIO;
            if (drq ? ((status & 8) != 0) : ((status & 8) == 0))
                return 0;
        }
        if (arch_delay_checked(100))
            return K_ETIMEDOUT;
    }
    return K_ETIMEDOUT;
}
static int ata_probe_channel(UINT16 base, UINT16 control)
{
    if (!base || !control || base > 0xfff8)
        return K_EINVAL;
    for (UINT8 slave = 0; slave < 2 && ata_count < ARRAY_LEN(ata); ++slave) {
        ata_device a = {base, control, slave, FALSE, FALSE};
        arch_out8(control, 2);
        arch_out8(base + 6, 0xa0 | (slave << 4));
        ata_settle(&a);
        UINT8 s = arch_in8(base + 7);
        if (!s || s == 0xff)
            continue;
        UINT32 ready = 50000;
        while ((arch_in8(base + 7) & 0x80) && --ready)
            k_delay_us(100);
        if (!ready)
            continue;
        arch_out8(base + 2, 0);
        arch_out8(base + 3, 0);
        arch_out8(base + 4, 0);
        arch_out8(base + 5, 0);
        arch_out8(base + 7, 0xec);
        ata_settle(&a);
        if (ata_wait(&a, TRUE) || arch_in8(base + 4) || arch_in8(base + 5))
            continue;
        UINT8 identity[512];
        for (UINT32 j = 0; j < 256; ++j) {
            UINT16 w = arch_in16(base);
            identity[2 * j] = (UINT8)w;
            identity[2 * j + 1] = (UINT8)(w >> 8);
        }
        if (ata_wait(&a, FALSE))
            continue;
        UINT32 sector;
        UINT64 sectors;
        char model[48];
        if (ata_identity(identity, &sector, &sectors, model, &a.lba48))
            continue;
        ata[ata_count] = a;
        disk *d = disk_add(DISK_ATA, sector, sectors, model, "ata-pio");
        if (d) {
            d->controller = ata_count;
            UINT16 caps = rd16(identity + 166);
            if ((caps & 0xc000) == 0x4000)
                d->flush_command = (a.lba48 && (caps & 0x2000)) ? 0xea : (caps & 0x1000) ? 0xe7 : 0;
            __atomic_add_fetch(&disk_count, 1, __ATOMIC_RELEASE);
        }
        ++ata_count;
    }
    return 0;
}
static int ata_probe(UINT16 bdf)
{
    UINT8 prog = (pci_get(bdf, 8) >> 8) & 255;
    pci_command(bdf, 1, 4);
    for (UINT32 channel = 0; channel < 2; ++channel) {
        BOOLEAN native = (prog & (channel ? 4 : 1)) != 0;
        UINT16 base = channel ? 0x170 : 0x1f0, ctrl = channel ? 0x376 : 0x3f6;
        if (native) {
            UINT32 b = pci_get(bdf, (UINT8)(0x10 + 8 * channel)),
                   c = pci_get(bdf, (UINT8)(0x14 + 8 * channel));
            if (!(b & 1) || !(c & 1) || (b & ~3U) > 65535 || (c & ~3U) > 65533)
                continue;
            base = (UINT16)(b & ~3U);
            ctrl = (UINT16)((c & ~3U) + 2);
        } else if (!platform.pic)
            continue;
        (void)ata_probe_channel(base, ctrl);
    }
    return 0;
}
static int ata_read(disk *d, UINT64 lba, UINT32 count, void *out)
{
    ata_device *a = &ata[d->controller];
    UINT8 *p = out;
    if (a->dead)
        return K_EIO;
    while (count--) {
        arch_out8(a->control, 2);
        arch_out8(a->base + 6,
                  (UINT8)(0xe0 | (a->slave << 4) | (a->lba48 ? 0 : ((lba >> 24) & 15))));
        ata_settle(a);
        int e = ata_wait(a, FALSE);
        if (e) {
            ata_quarantine(a);
            return e;
        }
        if (a->lba48) {
            arch_out8(a->base + 2, 0);
            arch_out8(a->base + 3, (UINT8)(lba >> 24));
            arch_out8(a->base + 4, (UINT8)(lba >> 32));
            arch_out8(a->base + 5, (UINT8)(lba >> 40));
        }
        arch_out8(a->base + 2, 1);
        arch_out8(a->base + 3, (UINT8)lba);
        arch_out8(a->base + 4, (UINT8)(lba >> 8));
        arch_out8(a->base + 5, (UINT8)(lba >> 16));
        arch_out8(a->base + 7, a->lba48 ? 0x24 : 0x20);
        ata_settle(a);
        e = ata_wait(a, TRUE);
        if (e) {
            ata_quarantine(a);
            return e;
        }
        for (UINT32 i = 0; i < d->info.sector_size; i += 2) {
            UINT16 w = arch_in16(a->base);
            p[i] = (UINT8)w;
            p[i + 1] = (UINT8)(w >> 8);
        }
        e = ata_wait(a, FALSE);
        if (e) {
            ata_quarantine(a);
            return e;
        }
        p += d->info.sector_size;
        ++lba;
    }
    return 0;
}
int k_storage_init(void)
{
    if (storage_initialized || k_cpu_id() != 0 || smp_started)
        return K_EBUSY;
    storage_initialized = TRUE;
    for (UINT32 bus = 0; bus < 256; ++bus)
        for (UINT32 slot = 0; slot < 32; ++slot) {
            UINT16 bdf = (UINT16)((bus << 8) | (slot << 3));
            if ((pci_get(bdf, 0) & 0xffff) == 0xffff)
                continue;
            UINT32 functions = (pci_get(bdf, 0x0c) & 0x00800000) ? 8 : 1;
            for (UINT32 fn = 0; fn < functions; ++fn) {
                UINT16 id = bdf | fn;
                if ((pci_get(id, 0) & 0xffff) == 0xffff)
                    continue;
                UINT32 cls = pci_get(id, 8) >> 8;
                int e = 0;
                if (cls == 0x010802)
                    e = nvme_probe(id);
                else if (cls == 0x010601)
                    e = ahci_probe(id);
                else if ((cls >> 8) == 0x0101)
                    e = ata_probe(id);
                else if ((cls >> 16) == 1) {
                    con_print("storage: unsupported PCI storage class ");
                    con_print_hex(cls);
                    con_print("\n");
                }
                if (e) {
                    con_print("storage PCI ");
                    con_print_hex(id);
                    con_print(": ");
                    con_print(k_strerror(e));
                    con_print("\n");
                }
            }
        }
    return 0;
}
UINT32 k_disk_count(void) { return __atomic_load_n(&disk_count, __ATOMIC_ACQUIRE); }
int k_disk_get(UINT32 id, k_disk_info *out)
{
    if (id >= k_disk_count() || !out)
        return K_ENOENT;
    int e = k_mutex_lock(&storage_mutex);
    if (e)
        return e;
    *out = disks[id].info;
    k_mutex_unlock(&storage_mutex);
    return 0;
}
static int disk_range(UINT32 id, UINT64 lba, UINT32 count, UINT64 bytes)
{
    if (id >= k_disk_count() || !count)
        return K_EINVAL;
    disk *d = &disks[id];
    if (!__atomic_load_n(&d->info.online, __ATOMIC_ACQUIRE))
        return K_EIO;
    if (lba >= d->info.sectors || count > d->info.sectors - lba ||
        (UINT64)count * d->info.sector_size > bytes)
        return K_EINVAL;
    return 0;
}
int k_disk_read(UINT32 id, UINT64 lba, UINT32 count, void *out, UINT64 bytes)
{
    if (!out)
        return K_EINVAL;
    int e = disk_range(id, lba, count, bytes);
    if (e)
        return e;
    e = k_mutex_lock(&storage_mutex);
    if (e)
        return e;
    disk *d = &disks[id];
    if (!d->info.online)
        e = K_EIO;
    else if (d->kind == DISK_NVME)
        e = nvme_read(d, lba, count, out);
    else if (d->kind == DISK_AHCI)
        e = ahci_read(d, lba, count, out);
    else if (d->kind == DISK_ATA)
        e = ata_read(d, lba, count, out);
    else if (d->kind == DISK_USB)
        e = usb_storage_read(d->controller, lba, count, out);
    else if (d->kind == DISK_RAM)
        mem_copy(out, d->ram + lba * d->info.sector_size, (UINT64)count * d->info.sector_size);
    else
        e = K_ENOTSUP;
    k_mutex_unlock(&storage_mutex);
    return e;
}
int k_disk_write(UINT32 id, UINT64 lba, UINT32 count, const void *in, UINT64 bytes)
{
    if (id >= k_disk_count())
        return K_EINVAL;
    /* Check write fencing before inspecting buffers or submitting commands. */
    if (disks[id].kind != DISK_RAM)
        return K_EROFS;
    if (!in)
        return K_EINVAL;
    int e = disk_range(id, lba, count, bytes);
    if (e)
        return e;
    e = k_mutex_lock(&storage_mutex);
    if (e)
        return e;
    mem_copy(disks[id].ram + lba * 512, in, (UINT64)count * 512);
    k_mutex_unlock(&storage_mutex);
    return 0;
}
/* Physical disk writes are private to checked filesystem paths; the public raw API stays fenced. */
static int disk_flush_locked(disk *d)
{
    if (!d->info.online)
        return K_EIO;
    if (d->kind == DISK_RAM)
        return 0;
    if (!d->flush_command)
        return K_EROFS;
    if (d->kind == DISK_USB)
        return usb_storage_flush(d->controller);
    if (d->kind == DISK_NVME) {
        nvme_controller *n = &nvme[d->controller];
        UINT32 cmd[16] = {0};
        cmd[1] = d->nsid;
        return nvme_submit(n, &n->io, cmd, NULL);
    }
    if (d->kind == DISK_AHCI)
        return ahci_issue(&ahci[d->controller], d->flush_command, 0, 0, 0);
    if (d->kind == DISK_ATA) {
        ata_device *a = &ata[d->controller];
        if (a->dead)
            return K_EIO;
        arch_out8(a->base + 6, 0xe0 | (a->slave << 4));
        ata_settle(a);
        int e = ata_wait(a, FALSE);
        if (!e) {
            arch_out8(a->base + 7, d->flush_command);
            ata_settle(a);
            e = ata_wait(a, FALSE);
        }
        if (e)
            ata_quarantine(a);
        return e;
    }
    return K_ENOTSUP;
}
int disk_flush(UINT32 id)
{
    if (id >= k_disk_count())
        return K_EINVAL;
    int e = k_mutex_lock(&storage_mutex);
    if (e)
        return e;
    e = disks[id].info.online ? disk_flush_locked(&disks[id]) : K_EIO;
    k_mutex_unlock(&storage_mutex);
    return e;
}
int disk_write_fs(UINT32 id, UINT64 lba, UINT32 count, const void *in, UINT64 bytes)
{
    if (!in)
        return K_EINVAL;
    int e = disk_range(id, lba, count, bytes);
    if (e)
        return e;
    disk *d = &disks[id];
    if (d->kind != DISK_RAM && !d->flush_command)
        return K_EROFS;
    e = k_mutex_lock(&storage_mutex);
    if (e)
        return e;
    if (!d->info.online)
        e = K_EIO;
    const UINT8 *p = in;
    while (count && !e) {
        UINT32 sectors = MIN(count, 4096 / d->info.sector_size), n = sectors * d->info.sector_size;
        if (d->kind == DISK_NVME) {
            nvme_controller *c = &nvme[d->controller];
            UINT32 cmd[16] = {0};
            mem_copy(c->data, p, n);
            cmd[0] = 1;
            cmd[1] = d->nsid;
            cmd[6] = (UINT32)(UINTN)c->data;
            cmd[7] = (UINT32)((UINT64)(UINTN)c->data >> 32);
            cmd[10] = (UINT32)lba;
            cmd[11] = (UINT32)(lba >> 32);
            cmd[12] = sectors - 1;
            e = nvme_submit(c, &c->io, cmd, NULL);
        } else if (d->kind == DISK_AHCI) {
            ahci_port *a = &ahci[d->controller];
            mem_copy(a->data, p, n);
            e = ahci_issue(a, a->lba48 ? 0x35 : 0xca, lba, sectors, n);
        } else if (d->kind == DISK_ATA) {
            ata_device *a = &ata[d->controller];
            sectors = 1;
            n = d->info.sector_size;
            if (a->dead) {
                e = K_EIO;
                break;
            }
            arch_out8(a->control, 2);
            arch_out8(a->base + 6,
                      (UINT8)(0xe0 | (a->slave << 4) | (a->lba48 ? 0 : ((lba >> 24) & 15))));
            ata_settle(a);
            e = ata_wait(a, FALSE);
            if (e)
                break;
            if (a->lba48) {
                arch_out8(a->base + 2, 0);
                arch_out8(a->base + 3, (UINT8)(lba >> 24));
                arch_out8(a->base + 4, (UINT8)(lba >> 32));
                arch_out8(a->base + 5, (UINT8)(lba >> 40));
            }
            arch_out8(a->base + 2, 1);
            arch_out8(a->base + 3, (UINT8)lba);
            arch_out8(a->base + 4, (UINT8)(lba >> 8));
            arch_out8(a->base + 5, (UINT8)(lba >> 16));
            arch_out8(a->base + 7, a->lba48 ? 0x34 : 0x30);
            ata_settle(a);
            e = ata_wait(a, TRUE);
            if (!e)
                for (UINT32 i = 0; i < n; i += 2)
                    arch_out16(a->base, rd16(p + i));
            if (!e) {
                ata_settle(a);
                e = ata_wait(a, FALSE);
            }
        } else if (d->kind == DISK_USB)
            e = usb_storage_write(d->controller, lba, sectors, p);
        else if (d->kind == DISK_RAM)
            mem_copy(d->ram + lba * d->info.sector_size, p, n);
        else
            e = K_ENOTSUP;
        p += n;
        lba += sectors;
        count -= sectors;
    }
    if (e) {
        if (d->kind == DISK_NVME)
            nvme_dead(&nvme[d->controller]);
        else if (d->kind == DISK_AHCI)
            ahci_dead(&ahci[d->controller]);
        else if (d->kind == DISK_ATA)
            ata_quarantine(&ata[d->controller]);
    }
    k_mutex_unlock(&storage_mutex);
    return e;
}

int k_ramdisk_create(UINT32 sectors, UINT32 *id)
{
    if (!id || !sectors || sectors > 8192 || disk_count == K_MAX_DISKS || k_cpu_id() != 0)
        return K_EINVAL;
    UINT32 pages = (sectors + 7) / 8;
    UINT64 p = k_pmm_alloc_pages(pages, 0);
    if (!p)
        return K_ENOMEM;
    disk *d = disk_add(DISK_RAM, 512, sectors, "volatile test disk", "ram");
    if (!d) {
        k_pmm_free_pages(p, pages);
        return K_ENOMEM;
    }
    d->ram = (void *)(UINTN)p;
    __atomic_add_fetch(&disk_count, 1, __ATOMIC_RELEASE);
    *id = d->info.id;
    return 0;
}

int storage_attach_usb(UINT32 instance, UINT32 sector, UINT64 sectors, const char *model,
                       BOOLEAN flush, BOOLEAN uas)
{
    if (!storage_initialized || k_cpu_id() != 0)
        return K_EBUSY;
    int e = k_mutex_lock(&storage_mutex);
    if (e)
        return e;
    disk *d = disk_add(DISK_USB, sector, sectors, model, uas ? "usb-uas" : "usb-bot");
    if (d) {
        d->controller = instance;
        d->flush_command = flush ? 1 : 0;
        __atomic_add_fetch(&disk_count, 1, __ATOMIC_RELEASE);
    }
    k_mutex_unlock(&storage_mutex);
    return d ? 0 : K_ENOSPC;
}

void storage_detach_usb(UINT32 instance)
{
    if (k_mutex_lock(&storage_mutex))
        return;
    for (UINT32 i = 0; i < disk_count; ++i)
        if (disks[i].kind == DISK_USB && disks[i].controller == instance) {
            __atomic_store_n(&disks[i].info.online, FALSE, __ATOMIC_RELEASE);
            disks[i].info.read_only = TRUE;
        }
    k_mutex_unlock(&storage_mutex);
}
