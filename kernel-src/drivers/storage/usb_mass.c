// SPDX-License-Identifier: GPL-2.0-only
#include "usb_storage_internal.h"

typedef struct {
    UINT32 link, sector, tag;
    UINT64 sectors;
    BOOLEAN dead, flush;
    UINT8 protocol;
} usb_disk;
static usb_disk usb_disks[K_MAX_DISKS];
static UINT32 usb_disks_count;
static UINT32 scanned_links;
static k_mutex scan_lock = K_MUTEX_INIT;
static k_mutex bot_lock = K_MUTEX_INIT;
static UINT32 be32(const UINT8 *p)
{
    return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) | ((UINT32)p[2] << 8) | p[3];
}
static void put32(UINT8 *p, UINT32 n)
{
    p[0] = (UINT8)(n >> 24);
    p[1] = (UINT8)(n >> 16);
    p[2] = (UINT8)(n >> 8);
    p[3] = (UINT8)n;
}
static void put64(UINT8 *p, UINT64 n)
{
    put32(p, (UINT32)(n >> 32));
    put32(p + 4, (UINT32)n);
}
static int bot(usb_disk *d, const UINT8 *cdb, UINT32 cdb_bytes, BOOLEAN input, void *data,
               UINT32 bytes, UINT32 *got)
{
    if (d->dead || !cdb_bytes || cdb_bytes > 16 || bytes > 4096 || (bytes && !data))
        return K_EIO;
    if (d->protocol == 0x62)
        return usb_uas_command(d->link, cdb, cdb_bytes, input, data, bytes, got);
    UINT8 cbw[31] = {0}, csw[13] = {0};
    UINT32 n = 0, actual = 0;
    wr32(cbw, 0x43425355);
    wr32(cbw + 4, ++d->tag);
    wr32(cbw + 8, bytes);
    cbw[12] = input ? 0x80 : 0;
    cbw[14] = (UINT8)cdb_bytes;
    mem_copy(cbw + 15, cdb, cdb_bytes);
    int e = usb_bulk_transfer(d->link, FALSE, cbw, 31, &n);
    if (e || n != 31)
        goto broken;
    if (bytes) {
        e = usb_bulk_transfer(d->link, input, data, bytes, &actual);
        if (e || (!input && actual != bytes))
            goto broken;
    }
    e = usb_bulk_transfer(d->link, TRUE, csw, 13, &n);
    if (e || n != 13 || rd32(csw) != 0x53425355 || rd32(csw + 4) != d->tag || csw[12] > 2 ||
        rd32(csw + 8) > bytes || actual > bytes || (!csw[12] && rd32(csw + 8) != bytes - actual) ||
        csw[12] == 2)
        goto broken;
    if (got)
        *got = actual;
    return csw[12] ? K_EIO : 0;
broken:
    d->dead = TRUE;
    usb_bulk_abort(d->link);
    return e ? e : K_EIO;
}
int usb_storage_init(void)
{
    if (k_cpu_id() != 0)
        return K_EBUSY;
    int scan_error = k_mutex_lock(&scan_lock);
    if (scan_error)
        return scan_error;
    for (UINT32 i = 0; i < usb_disks_count; ++i) {
        usb_bulk_info info;
        if (usb_bulk_get(usb_disks[i].link, &info) || !info.online)
            storage_detach_usb(i);
    }
    for (UINT32 link = scanned_links;
         link < usb_bulk_count() && usb_disks_count < ARRAY_LEN(usb_disks); ++link) {
        scanned_links = link + 1;
        usb_bulk_info info;
        if (usb_bulk_get(link, &info) || !info.online)
            continue;
        usb_disk *d = &usb_disks[usb_disks_count];
        mem_zero(d, sizeof(*d));
        d->link = link;
        d->protocol = info.protocol;
        UINT8 cdb[16] = {0}, data[64] = {0};
        UINT32 got = 0;
        int e = k_mutex_lock(&bot_lock);
        if (e) {
            k_mutex_unlock(&scan_lock);
            return e;
        }
        for (UINT32 attempt = 0; attempt < 5; ++attempt) {
            mem_zero(cdb, 16);
            e = bot(d, cdb, 6, TRUE, NULL, 0, NULL);
            if (!e || d->dead)
                break;
            cdb[0] = 3;
            cdb[4] = 18;
            (void)bot(d, cdb, 6, TRUE, data, 18, &got);
            k_sleep(200);
        }
        char model[48] = {0};
        if (!e) {
            mem_zero(cdb, 16);
            cdb[0] = 0x12;
            cdb[4] = 36;
            e = bot(d, cdb, 6, TRUE, data, 36, &got);
            if (!e && (got < 36 || (data[0] & 31) != 0))
                e = K_ENOTSUP;
            if (!e) {
                mem_copy(model, data + 8, 8);
                model[8] = ' ';
                mem_copy(model + 9, data + 16, 16);
            }
        }
        if (!e) {
            mem_zero(cdb, 16);
            cdb[0] = 0x25;
            e = bot(d, cdb, 10, TRUE, data, 8, &got);
            if (!e && got != 8)
                e = K_EIO;
            if (!e) {
                UINT32 last = be32(data);
                d->sector = be32(data + 4);
                d->sectors = (UINT64)last + 1;
                if (last == 0xffffffffu) {
                    mem_zero(cdb, 16);
                    cdb[0] = 0x9e;
                    cdb[1] = 0x10;
                    put32(cdb + 10, 32);
                    e = bot(d, cdb, 16, TRUE, data, 32, &got);
                    if (!e && got != 32)
                        e = K_EIO;
                    if (!e) {
                        UINT64 last64 = ((UINT64)be32(data) << 32) | be32(data + 4);
                        if (last64 == ~0ULL)
                            e = K_E2BIG;
                        else
                            d->sectors = last64 + 1;
                        d->sector = be32(data + 8);
                    }
                }
            }
        }
        if (!e && (!power2(d->sector) || d->sector < 512 || d->sector > 4096 ||
                   d->sectors > ~0ULL / d->sector))
            e = K_ENOTSUP;
        if (!e) {
            mem_zero(cdb, 16);
            cdb[0] = 0x35;
            d->flush = bot(d, cdb, 10, FALSE, NULL, 0, NULL) == 0;
            if (d->dead)
                e = K_EIO;
        }
        k_mutex_unlock(&bot_lock);
        if (!e) {
            UINT32 instance = usb_disks_count;
            __atomic_add_fetch(&usb_disks_count, 1, __ATOMIC_RELEASE);
            e = storage_attach_usb(instance, d->sector, d->sectors, model, d->flush,
                                   d->protocol == 0x62);
            if (e)
                usb_bulk_abort(d->link);
        }
    }
    k_mutex_unlock(&scan_lock);
    return 0;
}
static int transfer(UINT32 id, UINT64 lba, UINT32 count, void *buffer, BOOLEAN write)
{
    if (id >= __atomic_load_n(&usb_disks_count, __ATOMIC_ACQUIRE) || !buffer || !count)
        return K_EINVAL;
    usb_disk *d = &usb_disks[id];
    if (lba >= d->sectors || count > d->sectors - lba)
        return K_EINVAL;
    if (write && !d->flush)
        return K_EROFS;
    int e = k_mutex_lock(&bot_lock);
    if (e)
        return e;
    UINT8 *p = buffer;
    while (count && !e) {
        UINT32 n = MIN(count, 4096 / d->sector), got = 0;
        UINT8 cmd[16] = {0};
        cmd[0] = write ? 0x8a : 0x88;
        put64(cmd + 2, lba);
        put32(cmd + 10, n);
        UINT32 length = 16;
        if (lba <= 0xffffffffu && n - 1 <= 0xffffffffu - lba) {
            mem_zero(cmd, 16);
            cmd[0] = write ? 0x2a : 0x28;
            put32(cmd + 2, (UINT32)lba);
            cmd[7] = (UINT8)(n >> 8);
            cmd[8] = (UINT8)n;
            length = 10;
        }
        e = bot(d, cmd, length, !write, p, n * d->sector, &got);
        if (!e && got != n * d->sector)
            e = K_EIO;
        p += n * d->sector;
        lba += n;
        count -= n;
    }
    k_mutex_unlock(&bot_lock);
    return e;
}
int usb_storage_read(UINT32 id, UINT64 lba, UINT32 n, void *p)
{
    return transfer(id, lba, n, p, FALSE);
}
int usb_storage_write(UINT32 id, UINT64 lba, UINT32 n, const void *p)
{
    return transfer(id, lba, n, (void *)p, TRUE);
}
int usb_storage_flush(UINT32 id)
{
    if (id >= __atomic_load_n(&usb_disks_count, __ATOMIC_ACQUIRE) || !usb_disks[id].flush)
        return K_EROFS;
    int e = k_mutex_lock(&bot_lock);
    if (e)
        return e;
    UINT8 cmd[10] = {0x35};
    e = bot(&usb_disks[id], cmd, 10, FALSE, NULL, 0, NULL);
    k_mutex_unlock(&bot_lock);
    return e;
}

int k_usb_storage_init(void) { return usb_storage_init(); }
