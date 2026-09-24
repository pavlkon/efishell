// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"

k_partition_info partitions[MAX_PARTITIONS];
UINT32 partition_count;
#define MAX_VOLUMES 48
#define MAX_NODES 192
#define MAX_ROOTS 64
typedef struct {
    UINT32 disk, sector, spc, fat_bits, root_entries, root_cluster, cluster_count;
    UINT64 start, length, fat_start, fat_sectors, root_start, data_start;
    char source[24];
    UINT64 first_fat;
    UINT32 fat_count, fsinfo, backup_boot;
    UINT8 media;
    BOOLEAN mirrored, write_ready, write_failed, partition_ro;
    UINT8 *cache, *used, *dirty;
    UINT32 cache_pages, used_pages, dirty_pages, free_clusters, alloc_hint;
} fat_volume;
typedef struct {
    char path[K_PATH_MAX];
    UINT32 volume, kind;
} mount_entry;
typedef struct {
    BOOLEAN used, directory;
    UINT32 parent, block_id, kind, pages;
    char name[K_NAME_MAX + 1];
    UINT8 *data;
    UINT64 size;
} ram_node;
static fat_volume volumes[MAX_VOLUMES];
static mount_entry mounts[MAX_VOLUMES];
static UINT32 volume_count, mount_count;
static ram_node nodes[MAX_NODES];
static struct {
    char name[32], path[K_PATH_MAX];
} roots[MAX_ROOTS];
static UINT32 root_count;
static k_spinlock roots_lock = K_SPINLOCK_INIT;
static k_mutex vfs_mutex = K_MUTEX_INIT;
static BOOLEAN disk_scanned[K_MAX_DISKS];
static BOOLEAN vfs_ready;
static int native_create(UINT32, const char *, BOOLEAN, k_file *);
static int native_store(UINT32, const char *, const void *, UINT64);
static int ext_replace(k_file *, const void *, UINT64);
static int ntfs_replace(k_file *, const void *, UINT64);
static int ext_create(UINT32, const char *, BOOLEAN, const void *, UINT64, k_file *);
static int native_open(UINT32, const char *, k_file *);
static int native_read(const k_file *, UINT64, void *, UINT64, UINT64 *);
static int native_list(const k_file *, k_dir_callback, void *);
static int native_write(k_file *, UINT64, const void *, UINT64, UINT64 *);
static const char *native_description(UINT32);
static int native_sync_all(void);
static int fat_open(UINT32, const char *, k_file *);
static int fat_read(const k_file *, UINT64, void *, UINT64, UINT64 *);
static int fat_list(const k_file *, k_dir_callback, void *);
static int fat_refresh(k_file *);
static int fat_store_locked(UINT32, const char *, const void *, UINT64);
static int fat_create_locked(UINT32, const char *, BOOLEAN, k_file *);
static int fat_replace_locked(k_file *, const void *, UINT64);
static BOOLEAN path_prefix(const char *p, const char *root)
{
    UINTN n = str_len(root);
    return !memcmp(p, root, MIN(n, str_len(p))) && str_len(p) >= n &&
           (n == 1 || !p[n] || p[n] == '/');
}
int k_path_resolve(const char *path, const char *cwd, char out[K_PATH_MAX])
{
    if (!path || !cwd || !out || !*path)
        return K_EINVAL;
    UINTN len = 1, floor = 1;
    BOOLEAN named = FALSE;
    out[0] = '/';
    out[1] = 0;
    if (*path == '@') {
        named = TRUE;
        ++path;
        char name[32];
        UINT32 n = 0;
        while (*path && *path != '/') {
            if (n == 31)
                return K_E2BIG;
            name[n++] = *path++;
        }
        name[n] = 0;
        BOOLEAN found = FALSE;
        for (UINT32 i = 0; i < __atomic_load_n(&root_count, __ATOMIC_ACQUIRE); ++i)
            if (str_eq(name, roots[i].name)) {
                str_copy(out, roots[i].path, K_PATH_MAX);
                found = TRUE;
                break;
            }
        if (!found)
            return K_ENOENT;
        len = str_len(out);
        floor = len;
    } else if (*path != '/') {
        if (*cwd != '/' || str_len(cwd) >= K_PATH_MAX)
            return K_EINVAL;
        str_copy(out, cwd, K_PATH_MAX);
        len = str_len(out);
    }
    UINT32 scanned = 0;
    while (*path) {
        if (++scanned > 4096)
            return K_E2BIG;
        if (*path == '/') {
            ++path;
            continue;
        }
        const char *start = path;
        UINTN n = 0;
        while (*path && *path != '/') {
            if ((UINT8)*path < 32 || (UINT8)*path > 126 || ++n > K_NAME_MAX)
                return K_EINVAL;
            ++path;
        }
        if (n == 1 && start[0] == '.')
            continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            if (len <= floor) {
                if (named)
                    return K_EPERM;
                continue;
            }
            UINTN next = len;
            while (next > 1 && out[next - 1] != '/')
                --next;
            if (next > 1)
                --next;
            if (next < floor)
                return K_EPERM;
            len = next;
            out[len] = 0;
            continue;
        }
        UINTN slash = len > 1 ? 1 : 0;
        if (n + slash >= K_PATH_MAX - len)
            return K_E2BIG;
        if (slash)
            out[len++] = '/';
        mem_copy(out + len, start, n);
        len += n;
        out[len] = 0;
    }
    return 0;
}
int k_root_bind(const char *name, const char *path)
{
    if (k_cpu_id() != 0)
        return K_EPERM;
    if (!name || !path || !*name || str_len(name) > 31 || *path != '/')
        return K_EINVAL;
    for (const char *p = name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-'))
            return K_EINVAL;
    char normalized[K_PATH_MAX];
    int e = k_path_resolve(path, "/", normalized);
    if (e)
        return e;
    UINT64 irq = k_spin_lock(&roots_lock);
    for (UINT32 i = 0; i < root_count; ++i)
        if (str_eq(name, roots[i].name)) {
            e = str_eq(normalized, roots[i].path) ? 0 : K_EEXIST;
            k_spin_unlock(&roots_lock, irq);
            return e;
        }
    if (root_count == MAX_ROOTS) {
        k_spin_unlock(&roots_lock, irq);
        return K_E2BIG;
    }
    str_copy(roots[root_count].name, name, 32);
    str_copy(roots[root_count].path, normalized, K_PATH_MAX);
    __atomic_add_fetch(&root_count, 1, __ATOMIC_RELEASE);
    k_spin_unlock(&roots_lock, irq);
    return 0;
}
void k_roots_list(void (*cb)(const char *, const char *, void *), void *arg)
{
    if (cb)
        for (UINT32 i = 0; i < __atomic_load_n(&root_count, __ATOMIC_ACQUIRE); ++i)
            cb(roots[i].name, roots[i].path, arg);
}
void k_mounts_list(void (*cb)(const char *, const char *, void *), void *arg)
{
    if (cb) {
        cb("/", "RAM namespace (volatile)", arg);
        for (UINT32 i = 0; i < __atomic_load_n(&mount_count, __ATOMIC_ACQUIRE); ++i) {
            if (mounts[i].kind >= 4) {
                cb(mounts[i].path, native_description(mounts[i].volume), arg);
                continue;
            }
            fat_volume *v = &volumes[mounts[i].volume];
            char description[128];
            str_copy(description, v->source, sizeof(description));
            const char *mode = v->fat_bits == 12 ? " [FAT12 read-only]"
                               : v->partition_ro ? " [partition read-only]"
                               : v->write_failed ? " [write failed; read-only]"
                               : (disks[v->disk].kind != DISK_RAM && !disks[v->disk].flush_command)
                                   ? " [no flush support; read-only]"
                               : v->write_ready ? " [FAT writable]"
                                                : " [FAT; validated on first write]";
            UINTN n = str_len(description);
            str_copy(description + n, mode, sizeof(description) - n);
            cb(mounts[i].path, description, arg);
        }
    }
}
static int ram_child(UINT32 parent, const char *name)
{
    for (UINT32 i = 1; i < MAX_NODES; ++i)
        if (nodes[i].used && nodes[i].parent == parent && str_eq(nodes[i].name, name))
            return (int)i;
    return K_ENOENT;
}
static int ram_walk(const char *path)
{
    UINT32 at = 0;
    while (*path) {
        if (*path == '/') {
            ++path;
            continue;
        }
        if (!nodes[at].directory)
            return K_ENOENT;
        char name[K_NAME_MAX + 1];
        UINT32 n = 0;
        while (*path && *path != '/')
            name[n++] = *path++;
        name[n] = 0;
        int next = ram_child(at, name);
        if (next < 0)
            return next;
        at = (UINT32)next;
    }
    return (int)at;
}
static int ram_new(const char *path, BOOLEAN directory)
{
    char parent[K_PATH_MAX];
    str_copy(parent, path, sizeof(parent));
    UINTN n = str_len(parent);
    if (n <= 1)
        return K_EEXIST;
    UINTN split = n;
    while (split && parent[split - 1] != '/')
        --split;
    char name[K_NAME_MAX + 1];
    str_copy(name, parent + split, sizeof(name));
    parent[split > 1 ? split - 1 : 1] = 0;
    int p = ram_walk(parent);
    if (p < 0 || !nodes[p].directory)
        return K_ENOENT;
    if (ram_child((UINT32)p, name) >= 0)
        return K_EEXIST;
    for (UINT32 i = 1; i < MAX_NODES; ++i)
        if (!nodes[i].used) {
            ram_node *r = &nodes[i];
            mem_zero(r, sizeof(*r));
            r->used = TRUE;
            r->parent = (UINT32)p;
            r->directory = directory;
            r->kind = 1;
            str_copy(r->name, name, sizeof(r->name));
            return (int)i;
        }
    return K_ENOMEM;
}
static int choose_mount(const char *path)
{
    int best = -1;
    UINTN longest = 0;
    for (UINT32 i = 0; i < __atomic_load_n(&mount_count, __ATOMIC_ACQUIRE); ++i)
        if (path_prefix(path, mounts[i].path)) {
            UINTN n = str_len(mounts[i].path);
            if (n > longest) {
                best = (int)i;
                longest = n;
            }
        }
    return best;
}
int k_vfs_init(void)
{
    if (vfs_ready)
        return K_EBUSY;
    nodes[0].used = nodes[0].directory = TRUE;
    nodes[0].kind = 1;
    vfs_ready = TRUE;
    return 0;
}
int k_vfs_mkdir(const char *path)
{
    if (!vfs_ready)
        return K_EINVAL;
    char canonical_path[K_PATH_MAX];
    int e = k_path_resolve(path, "/", canonical_path);
    if (e)
        return e;
    e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    int mount = choose_mount(canonical_path);
    if (mount >= 0)
        e = mounts[mount].kind >= 4
                ? native_create(mounts[mount].volume, canonical_path + str_len(mounts[mount].path),
                                TRUE, NULL)
                : fat_create_locked(mounts[mount].volume,
                                    canonical_path + str_len(mounts[mount].path), TRUE, NULL);
    else {
        e = ram_new(canonical_path, TRUE);
        if (e >= 0)
            e = 0;
    }
    k_mutex_unlock(&vfs_mutex);
    return e;
}
int k_vfs_put(const char *path, const void *bytes, UINT64 size)
{
    if (!vfs_ready || (!bytes && size) || size > 4 * 1024 * 1024)
        return K_EINVAL;
    char canonical_path[K_PATH_MAX];
    int e = k_path_resolve(path, "/", canonical_path);
    if (e)
        return e;
    e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    int mount = choose_mount(canonical_path);
    if (mount >= 0) {
        if (mounts[mount].kind >= 4) {
            e = native_store(mounts[mount].volume, canonical_path + str_len(mounts[mount].path),
                             bytes, size);
            goto end;
        }
        e = fat_store_locked(mounts[mount].volume, canonical_path + str_len(mounts[mount].path),
                             bytes, size);
        goto end;
    }
    int index = ram_walk(canonical_path);
    if (index >= 0 && (nodes[index].directory || nodes[index].kind != 1)) {
        e = K_EINVAL;
        goto end;
    }
    UINT32 pages = (UINT32)((size + 4095) / 4096);
    UINT8 *data = pages ? (void *)(UINTN)k_pmm_alloc_pages(pages, 0) : NULL;
    if (pages && !data) {
        e = K_ENOMEM;
        goto end;
    }
    if (index < 0)
        index = ram_new(canonical_path, FALSE);
    if (index < 0) {
        if (data)
            k_pmm_free_pages((UINT64)(UINTN)data, pages);
        e = index;
        goto end;
    }
    if (size)
        mem_copy(data, bytes, size);
    ram_node *r = &nodes[index];
    if (r->data)
        k_pmm_free_pages((UINT64)(UINTN)r->data, r->pages);
    r->data = data;
    r->size = size;
    r->pages = pages;
    e = 0;
end:
    k_mutex_unlock(&vfs_mutex);
    return e;
}
static int vfs_open_locked(const char *path, k_file *out)
{
    int m = choose_mount(path);
    if (m >= 0 && mounts[m].kind >= 4)
        return native_open(mounts[m].volume, path + str_len(mounts[m].path), out);
    if (m >= 0)
        return fat_open(mounts[m].volume, path + str_len(mounts[m].path), out);
    int n = ram_walk(path);
    if (n < 0)
        return n;
    ram_node *r = &nodes[n];
    *out = (k_file){
        r->kind, r->kind == 3 ? r->block_id : (UINT32)n, 0, r->directory ? 0x10 : 0, r->size, 0};
    return 0;
}
int k_vfs_open(const char *path, k_file *out)
{
    if (!vfs_ready || !out)
        return K_EINVAL;
    char resolved[K_PATH_MAX];
    int e = k_path_resolve(path, "/", resolved);
    if (e)
        return e;
    e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    e = vfs_open_locked(resolved, out);
    k_mutex_unlock(&vfs_mutex);
    return e;
}
static int disk_bytes(UINT32 id, UINT64 offset, void *out, UINT64 n)
{
    if (id >= k_disk_count() || offset > disks[id].info.sectors * disks[id].info.sector_size ||
        n > disks[id].info.sectors * disks[id].info.sector_size - offset)
        return K_EINVAL;
    UINT8 sector[4096], *p = out;
    UINT32 size = disks[id].info.sector_size;
    while (n) {
        if (!(offset % size) && n >= size) {
            UINT32 sectors = (UINT32)MIN(n / size, 128);
            int e = k_disk_read(id, offset / size, sectors, p, n);
            if (e)
                return e;
            UINT64 bytes = (UINT64)sectors * size;
            p += bytes;
            offset += bytes;
            n -= bytes;
            continue;
        }
        UINT64 lba = offset / size;
        UINT32 at = (UINT32)(offset % size), part = (UINT32)MIN(n, size - at);
        int e = k_disk_read(id, lba, 1, sector, sizeof(sector));
        if (e)
            return e;
        mem_copy(p, sector + at, part);
        p += part;
        offset += part;
        n -= part;
    }
    return 0;
}
int k_vfs_read(const k_file *file, UINT64 offset, void *out, UINT64 n, UINT64 *got)
{
    if (!file || (!out && n) || !got)
        return K_EINVAL;
    *got = 0;
    if (file->attributes & 0x10)
        return K_EINVAL;
    int e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    if (file->kind == 1) {
        if (file->object >= MAX_NODES || !nodes[file->object].used) {
            e = K_ENOENT;
            goto end;
        }
        ram_node *r = &nodes[file->object];
        if (offset < r->size) {
            n = MIN(n, r->size - offset);
            mem_copy(out, r->data + offset, n);
            *got = n;
        }
    } else if (file->kind == 4 || file->kind == 5)
        e = native_read(file, offset, out, n, got);
    else if (file->kind == 2)
        e = fat_read(file, offset, out, n, got);
    else if (file->kind == 3) {
        if (file->object >= k_disk_count()) {
            e = K_ENOENT;
            goto end;
        }
        UINT64 size = disks[file->object].info.sectors * disks[file->object].info.sector_size;
        if (offset < size) {
            n = MIN(n, size - offset);
            e = disk_bytes(file->object, offset, out, n);
            if (!e)
                *got = n;
        }
    } else
        e = K_ENOTSUP;
end:
    k_mutex_unlock(&vfs_mutex);
    return e;
}
int k_vfs_list(const char *path, k_dir_callback cb, void *arg)
{
    if (!path || !cb)
        return K_EINVAL;
    char resolved[K_PATH_MAX];
    int e = k_path_resolve(path, "/", resolved);
    if (e)
        return e;
    e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    k_file f;
    e = vfs_open_locked(resolved, &f);
    if (e)
        goto end;
    if (!(f.attributes & 0x10)) {
        e = K_EINVAL;
        goto end;
    }
    if (f.kind == 4 || f.kind == 5)
        e = native_list(&f, cb, arg);
    else if (f.kind == 2)
        e = fat_list(&f, cb, arg);
    else if (f.kind == 1)
        for (UINT32 i = 1; i < MAX_NODES; ++i)
            if (nodes[i].used && nodes[i].parent == f.object) {
                k_dirent entry;
                str_copy(entry.name, nodes[i].name, sizeof(entry.name));
                entry.size = nodes[i].size;
                entry.directory = nodes[i].directory;
                cb(&entry, arg);
            }
end:
    k_mutex_unlock(&vfs_mutex);
    return e;
}

/* FAT walks stay partition-relative and bounded; metadata is never auto-repaired. */
static int volume_bytes(fat_volume *v, UINT64 offset, void *out, UINT64 n)
{
    if (!span_ok(offset, n, v->length * v->sector))
        return K_EIO;
    return disk_bytes(v->disk, v->start * v->sector + offset, out, n);
}
static int fat_next(fat_volume *v, UINT32 cluster, UINT32 *next)
{
    if (cluster < 2 || cluster - 2 >= v->cluster_count)
        return K_EIO;
    UINT64 off =
        v->fat_bits == 12 ? (UINT64)cluster + cluster / 2 : (UINT64)cluster * (v->fat_bits / 8);
    UINT8 raw[4] = {0};
    UINT32 bytes = v->fat_bits == 32 ? 4 : 2;
    if (!span_ok(off, bytes, v->fat_sectors * v->sector))
        return K_EIO;
    int e = volume_bytes(v, v->fat_start * v->sector + off, raw, bytes);
    if (e)
        return e;
    UINT32 n = v->fat_bits == 32 ? rd32(raw) & 0x0fffffff : rd16(raw);
    if (v->fat_bits == 12)
        n = (cluster & 1) ? n >> 4 : n & 0xfff;
    UINT32 end = v->fat_bits == 12 ? 0xff8 : v->fat_bits == 16 ? 0xfff8 : 0x0ffffff8;
    if (n >= end) {
        *next = 0xffffffff;
        return 0;
    }
    if (n < 2 || n - 2 >= v->cluster_count || n == end - 1)
        return K_EIO;
    *next = n;
    return 0;
}
static BOOLEAN name_equal(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'a' && x <= 'z')
            x -= 32;
        if (y >= 'a' && y <= 'z')
            y -= 32;
        if (x != y)
            return FALSE;
    }
    return *a == *b;
}
static UINT8 sfn_checksum(const UINT8 *name)
{
    UINT8 sum = 0;
    for (int i = 0; i < 11; ++i)
        sum = (UINT8)(((sum & 1) ? 128 : 0) + (sum >> 1) + name[i]);
    return sum;
}
typedef int (*fat_callback)(const k_dirent *, UINT32, void *);
static int fat_directory(fat_volume *v, UINT32 cluster, fat_callback cb, void *arg)
{
    UINT8 sector[4096];
    UINT32 visited = 0, scanned = 0, tortoise = cluster;
    char longname[K_NAME_MAX + 1];
    mem_zero(longname, sizeof(longname));
    UINT32 expected = 0;
    UINT8 sum = 0;
    BOOLEAN lfn = FALSE;
    if (!cluster && v->fat_bits == 32)
        return K_EIO;
    for (;;) {
        UINT64 first;
        UINT32 sectors;
        if (!cluster) {
            first = v->root_start;
            sectors = (v->root_entries * 32 + v->sector - 1) / v->sector;
        } else {
            if (cluster < 2 || cluster - 2 >= v->cluster_count || ++visited > 65536 ||
                visited > v->cluster_count)
                return K_EIO;
            first = v->data_start + (UINT64)(cluster - 2) * v->spc;
            sectors = v->spc;
        }
        for (UINT32 s = 0; s < sectors; ++s) {
            int e = volume_bytes(v, (first + s) * v->sector, sector, v->sector);
            if (e)
                return e;
            for (UINT32 p = 0; p < v->sector; p += 32) {
                if (!cluster && scanned >= v->root_entries)
                    return 0;
                if (++scanned > 65536)
                    return K_E2BIG;
                UINT8 *d = sector + p;
                if (!d[0])
                    return 0;
                if (d[0] == 0xe5) {
                    lfn = FALSE;
                    continue;
                }
                if (d[11] == 0x0f) {
                    UINT32 seq = d[0] & 31;
                    if (d[0] & 0x40) {
                        mem_zero(longname, sizeof(longname));
                        lfn = seq >= 1 && seq <= 20;
                        expected = seq;
                        sum = d[13];
                    }
                    if (!lfn || seq != expected || d[13] != sum || d[12] || rd16(d + 26) ||
                        (d[0] & 0xa0)) {
                        lfn = FALSE;
                        continue;
                    }
                    static const UINT8 offsets[13] = {1,  3,  5,  7,  9,  14, 16,
                                                      18, 20, 22, 24, 28, 30};
                    for (UINT32 i = 0; i < 13; ++i) {
                        UINT32 at = (seq - 1) * 13 + i;
                        UINT16 ch = rd16(d + offsets[i]);
                        if (at >= K_NAME_MAX) {
                            if (ch && ch != 0xffff)
                                lfn = FALSE;
                            continue;
                        }
                        if (ch == 0xffff || ch == 0)
                            longname[at] = 0;
                        else if (ch < 32 || ch > 126 || ch == '/' || ch == '\\')
                            lfn = FALSE;
                        else
                            longname[at] = (char)ch;
                    }
                    --expected;
                    continue;
                }
                if (d[11] & 8) {
                    lfn = FALSE;
                    continue;
                }
                k_dirent entry;
                mem_zero(&entry, sizeof(entry));
                if (lfn && !expected && sum == sfn_checksum(d) && longname[0])
                    str_copy(entry.name, longname, sizeof(entry.name));
                else {
                    UINT32 n = 0;
                    for (UINT32 i = 0; i < 8 && d[i] != ' '; ++i)
                        entry.name[n++] = (char)d[i];
                    if (d[8] != ' ') {
                        entry.name[n++] = '.';
                        for (UINT32 i = 8; i < 11 && d[i] != ' '; ++i)
                            entry.name[n++] = (char)d[i];
                    }
                    entry.name[n] = 0;
                }
                lfn = FALSE;
                if (str_eq(entry.name, ".") || str_eq(entry.name, ".."))
                    continue;
                entry.directory = (d[11] & 0x10) != 0;
                entry.attributes = d[11];
                entry.directory_offset = (first + s) * v->sector + p;
                entry.size = rd32(d + 28);
                UINT32 child =
                    rd16(d + 26) | (v->fat_bits == 32 ? ((UINT32)rd16(d + 20) << 16) : 0);
                if (v->fat_bits == 32 && (child & 0xf0000000))
                    return K_EIO;
                if ((entry.directory || entry.size) && (child < 2 || child - 2 >= v->cluster_count))
                    return K_EIO;
                int result = cb(&entry, child, arg);
                if (result)
                    return result > 0 ? 0 : result;
            }
        }
        if (!cluster)
            return 0;
        UINT32 next;
        int e = fat_next(v, cluster, &next);
        if (e)
            return e;
        if (next == 0xffffffff)
            return 0;
        if ((visited & 1) == 0) {
            e = fat_next(v, tortoise, &tortoise);
            if (e)
                return e;
            if (tortoise == 0xffffffff)
                return K_EIO;
        }
        if (next == tortoise)
            return K_EIO;
        cluster = next;
    }
}
typedef struct {
    const char *name;
    BOOLEAN found;
    k_dirent entry;
    UINT32 cluster;
} fat_find;
static int fat_find_cb(const k_dirent *entry, UINT32 cluster, void *arg)
{
    fat_find *f = arg;
    if (!name_equal(f->name, entry->name))
        return 0;
    f->found = TRUE;
    f->entry = *entry;
    f->cluster = cluster;
    return 1;
}
static int fat_open(UINT32 index, const char *path, k_file *out)
{
    if (index >= volume_count)
        return K_ENOENT;
    fat_volume *v = &volumes[index];
    UINT32 cluster = v->fat_bits == 32 ? v->root_cluster : 0;
    *out = (k_file){2, index, cluster, 0x10, 0, 0};
    while (*path) {
        if (*path == '/') {
            ++path;
            continue;
        }
        if (!(out->attributes & 0x10))
            return K_ENOENT;
        char name[K_NAME_MAX + 1];
        UINT32 n = 0;
        while (*path && *path != '/')
            name[n++] = *path++;
        name[n] = 0;
        fat_find found = {.name = name};
        int e = fat_directory(v, cluster, fat_find_cb, &found);
        if (e)
            return e;
        if (!found.found)
            return K_ENOENT;
        cluster = found.cluster;
        *out = (k_file){2,
                        index,
                        cluster,
                        found.entry.attributes,
                        found.entry.size,
                        found.entry.directory_offset};
    }
    return 0;
}
static int fat_read(const k_file *file, UINT64 offset, void *out, UINT64 bytes, UINT64 *got)
{
    k_file refreshed = *file;
    int refresh = fat_refresh(&refreshed);
    if (refresh)
        return refresh;
    file = &refreshed;
    if (file->object >= volume_count)
        return K_ENOENT;
    fat_volume *v = &volumes[file->object];
    if (offset >= file->size || !bytes)
        return 0;
    bytes = MIN(bytes, file->size - offset);
    UINT64 cluster_bytes = (UINT64)v->spc * v->sector, skip = offset / cluster_bytes,
           at = offset % cluster_bytes;
    if (skip > 65536)
        return K_E2BIG;
    UINT32 cluster = file->cluster, steps = 0, tortoise = cluster;
    UINT8 *p = out;
    while (skip || bytes) {
        if (cluster < 2 || cluster - 2 >= v->cluster_count || steps >= v->cluster_count ||
            steps > 65536)
            return K_EIO;
        if (!skip) {
            UINT64 n = MIN(bytes, cluster_bytes - at);
            UINT64 pos = (v->data_start + (UINT64)(cluster - 2) * v->spc) * v->sector + at;
            int e = volume_bytes(v, pos, p, n);
            if (e)
                return e;
            *got += n;
            p += n;
            bytes -= n;
            at = 0;
            if (!bytes)
                return 0;
        } else
            --skip;
        UINT32 next;
        int e = fat_next(v, cluster, &next);
        if (e || next == 0xffffffff)
            return e ? e : K_EIO;
        ++steps;
        if ((steps & 1) == 0) {
            e = fat_next(v, tortoise, &tortoise);
            if (e)
                return e;
        }
        if (next == tortoise)
            return K_EIO;
        cluster = next;
    }
    return 0;
}
typedef struct {
    k_dir_callback cb;
    void *arg;
} fat_listing;
static int fat_list_cb(const k_dirent *entry, UINT32 cluster, void *ctx)
{
    (void)cluster;
    fat_listing *l = ctx;
    l->cb(entry, l->arg);
    return 0;
}
static int fat_list(const k_file *file, k_dir_callback cb, void *arg)
{
    if (file->object >= volume_count)
        return K_ENOENT;
    fat_listing l = {cb, arg};
    return fat_directory(&volumes[file->object], file->cluster, fat_list_cb, &l);
}
/* Before first FAT16/32 write, audit referenced clusters and mirrored FATs.
 * Replacement order is dirty -> data -> FAT -> directory -> old chain -> clean, with flushes.
 * This is not a power-fail-safe journal; mutation errors freeze the volume read-only. */
#define FAT_WRITE_MAX (4ULL * 1024 * 1024)
#define FAT_CACHE_MAX (64ULL * 1024 * 1024)
#define FAT_DIR_LIMIT 4096
static UINT32 fat_cached(fat_volume *v, UINT32 c)
{
    return v->fat_bits == 16 ? rd16(v->cache + 2ULL * c) : rd32(v->cache + 4ULL * c) & 0x0fffffff;
}
static UINT32 fat_eoc(fat_volume *v) { return v->fat_bits == 16 ? 0xffff : 0x0fffffff; }
static BOOLEAN fat_is_end(fat_volume *v, UINT32 n)
{
    return n >= (v->fat_bits == 16 ? 0xfff8U : 0x0ffffff8U);
}
static void fat_cache_set(fat_volume *v, UINT32 c, UINT32 value)
{
    UINT64 off = (UINT64)c * (v->fat_bits / 8);
    if (v->fat_bits == 16) {
        v->cache[off] = (UINT8)value;
        v->cache[off + 1] = (UINT8)(value >> 8);
    } else
        wr32(v->cache + off, (rd32(v->cache + off) & 0xf0000000) | (value & 0x0fffffff));
    bit_set(v->dirty, off / v->sector);
}
static int volume_write(fat_volume *v, UINT64 sector, UINT32 count, const void *data)
{
    if (v->write_failed || sector >= v->length || !count || count > v->length - sector)
        return K_EROFS;
    int e = disk_write_fs(v->disk, v->start + sector, count, data, (UINT64)count * v->sector);
    if (e)
        v->write_failed = TRUE;
    return e;
}
static int volume_flush(fat_volume *v)
{
    int e = disk_flush(v->disk);
    if (e)
        v->write_failed = TRUE;
    return e;
}
static int fat_flush_cache(fat_volume *v)
{
    for (UINT64 s = 0; s < v->fat_sectors; ++s)
        if (bit_get(v->dirty, s)) {
            for (UINT32 f = 0; f < (v->mirrored ? v->fat_count : 1); ++f) {
                UINT64 first = v->mirrored ? v->first_fat + f * v->fat_sectors : v->fat_start;
                int e = volume_write(v, first + s, 1, v->cache + s * v->sector);
                if (e)
                    return e;
            }
            bit_clear(v->dirty, s);
        }
    return volume_flush(v);
}
static int fat_mark_chain(fat_volume *v, UINT32 c, UINT64 size)
{
    if (!c)
        return size ? K_EIO : 0;
    UINT32 count = 0;
    for (;;) {
        if (c < 2 || c - 2 >= v->cluster_count || ++count > 65536 || bit_get(v->used, c))
            return K_EIO;
        bit_set(v->used, c);
        UINT32 next = fat_cached(v, c);
        if (fat_is_end(v, next))
            break;
        c = next;
    }
    return size > (UINT64)count * v->spc * v->sector ? K_EIO : 0;
}
typedef struct {
    fat_volume *v;
    UINT32 *dirs, count;
} fat_audit;
static int fat_audit_entry(const k_dirent *entry, UINT32 c, void *arg)
{
    fat_audit *a = arg;
    int e = fat_mark_chain(a->v, c, entry->size);
    if (e)
        return e;
    if (entry->directory) {
        if (a->count == FAT_DIR_LIMIT)
            return K_E2BIG;
        a->dirs[a->count++] = c;
    }
    return 0;
}
static void fat_drop_cache(fat_volume *v)
{
    if (v->cache)
        k_pmm_free_pages((UINT64)(UINTN)v->cache, v->cache_pages);
    if (v->used)
        k_pmm_free_pages((UINT64)(UINTN)v->used, v->used_pages);
    if (v->dirty)
        k_pmm_free_pages((UINT64)(UINTN)v->dirty, v->dirty_pages);
    v->cache = v->used = v->dirty = NULL;
    v->write_ready = FALSE;
}
static int fat_prepare_write(fat_volume *v)
{
    if (v->write_failed || v->partition_ro)
        return K_EROFS;
    if (v->write_ready)
        return 0;
    if (v->fat_bits == 16 && v->cluster_count > 0xffee)
        return K_ENOTSUP;
    if (v->fat_bits == 12 || (disks[v->disk].kind != DISK_RAM && !disks[v->disk].flush_command))
        return K_EROFS;
    UINT64 bytes = v->fat_sectors * v->sector;
    if (bytes > FAT_CACHE_MAX)
        return K_E2BIG;
    v->cache_pages = (UINT32)((bytes + 4095) / 4096);
    v->used_pages = (v->cluster_count + 2 + 32767) / 32768;
    v->dirty_pages = (UINT32)((v->fat_sectors + 32767) / 32768);
    v->cache = (void *)(UINTN)k_pmm_alloc_pages(v->cache_pages, 0);
    v->used = (void *)(UINTN)k_pmm_alloc_pages(v->used_pages, 0);
    v->dirty = (void *)(UINTN)k_pmm_alloc_pages(v->dirty_pages, 0);
    UINT32 *dirs = (void *)(UINTN)k_pmm_alloc_pages(FAT_DIR_LIMIT / 1024, 0);
    int e = K_ENOMEM;
    if (!v->cache || !v->used || !v->dirty || !dirs)
        goto fail;
    e = volume_bytes(v, v->fat_start * v->sector, v->cache, bytes);
    if (e)
        goto fail;
    UINT32 header = (v->fat_bits == 16 ? 0xff00U : 0x0fffff00U) | v->media;
    if (fat_cached(v, 0) != header) {
        e = K_EIO;
        goto fail;
    }
    UINT32 mask = v->fat_bits == 16 ? 0xc000 : 0x0c000000;
    if ((fat_cached(v, 1) & mask) != mask) {
        e = K_EROFS;
        goto fail;
    }
    if (v->mirrored && v->fat_count > 1) {
        UINT8 sector[4096];
        for (UINT64 s = 0; s < v->fat_sectors; ++s) {
            e = volume_bytes(v, (v->first_fat + v->fat_sectors + s) * v->sector, sector, v->sector);
            if (e)
                goto fail;
            if (memcmp(v->cache + s * v->sector, sector, v->sector)) {
                e = K_EIO;
                goto fail;
            }
        }
    }
    fat_audit a = {v, dirs, 1};
    dirs[0] = v->root_cluster;
    if (v->fat_bits == 32) {
        e = fat_mark_chain(v, v->root_cluster, 0);
        if (e)
            goto fail;
    }
    for (UINT32 i = 0; i < a.count; ++i) {
        e = fat_directory(v, dirs[i], fat_audit_entry, &a);
        if (e)
            goto fail;
    }
    v->free_clusters = 0;
    for (UINT32 c = 2; c < v->cluster_count + 2; ++c) {
        UINT32 value = fat_cached(v, c);
        if (!value) {
            ++v->free_clusters;
            continue;
        }
        if (!bit_get(v->used, c) && value != (v->fat_bits == 16 ? 0xfff7U : 0x0ffffff7U)) {
            e = K_EIO;
            goto fail;
        }
    }
    v->alloc_hint = 2;
    v->write_ready = TRUE;
    k_pmm_free_pages((UINT64)(UINTN)dirs, FAT_DIR_LIMIT / 1024);
    return 0;
fail:
    if (dirs)
        k_pmm_free_pages((UINT64)(UINTN)dirs, FAT_DIR_LIMIT / 1024);
    fat_drop_cache(v);
    return e;
}
static int fat_begin_write(fat_volume *v)
{
    fat_cache_set(v, 1, fat_cached(v, 1) & ~(v->fat_bits == 16 ? 0x8000U : 0x08000000U));
    int e = fat_flush_cache(v);
    if (e)
        return e;
    /* FSInfo is a hint; invalidate stale free-count hints instead of trusting them. */
    if (v->fat_bits == 32 && v->fsinfo && v->fsinfo < v->first_fat) {
        UINT8 sector[4096];
        UINT32 offsets[2] = {v->fsinfo, v->backup_boot + v->fsinfo};
        for (UINT32 i = 0; i < (v->backup_boot ? 2U : 1U); ++i) {
            if (offsets[i] >= v->first_fat)
                continue;
            e = volume_bytes(v, (UINT64)offsets[i] * v->sector, sector, v->sector);
            if (e)
                return e;
            if (rd32(sector) != 0x41615252 || rd32(sector + 484) != 0x61417272 ||
                rd32(sector + 508) != 0xaa550000)
                continue;
            wr32(sector + 488, 0xffffffff);
            wr32(sector + 492, 0xffffffff);
            e = volume_write(v, offsets[i], 1, sector);
            if (e)
                return e;
        }
    }
    return volume_flush(v);
}
static int fat_finish_write(fat_volume *v, int e)
{
    if (e) {
        v->write_failed = TRUE;
        return e;
    }
    fat_cache_set(v, 1, fat_cached(v, 1) | (v->fat_bits == 16 ? 0x8000U : 0x08000000U));
    e = fat_flush_cache(v);
    if (e)
        v->write_failed = TRUE;
    return e;
}
static UINT32 fat_allocate(fat_volume *v)
{
    UINT32 count = v->cluster_count;
    for (UINT32 i = 0; i < count; ++i) {
        UINT32 c = v->alloc_hint++;
        if (v->alloc_hint == count + 2)
            v->alloc_hint = 2;
        if (!fat_cached(v, c)) {
            fat_cache_set(v, c, fat_eoc(v));
            bit_set(v->used, c);
            --v->free_clusters;
            return c;
        }
    }
    return 0;
}
static int fat_write_data(fat_volume *v, UINT32 c, const UINT8 *data, UINT64 size)
{
    UINT8 sector[4096];
    UINT64 first = v->data_start + (UINT64)(c - 2) * v->spc;
    for (UINT32 s = 0; s < v->spc; ++s) {
        UINT32 n = (UINT32)MIN(size, v->sector);
        mem_zero(sector, v->sector);
        if (n) {
            mem_copy(sector, data, n);
            data += n;
            size -= n;
        }
        int e = volume_write(v, first + s, 1, sector);
        if (e)
            return e;
    }
    return 0;
}
static int fat_entry_write(fat_volume *v, UINT64 off, const UINT8 data[32])
{
    UINT8 sector[4096];
    if ((off & 31) || !span_ok(off, 32, v->length * v->sector))
        return K_EINVAL;
    UINT64 lba = off / v->sector;
    UINT32 at = off % v->sector;
    /* Directory entries may not point into BPB/FAT metadata. */
    if (lba < v->root_start || (v->fat_bits == 32 && lba < v->data_start))
        return K_EPERM;
    int e = volume_bytes(v, lba * v->sector, sector, v->sector);
    if (e)
        return e;
    mem_copy(sector + at, data, 32);
    return volume_write(v, lba, 1, sector);
}
static int fat_refresh(k_file *f)
{
    if (f->kind != 2 || f->object >= volume_count)
        return K_EINVAL;
    if (!f->directory_offset)
        return 0;
    fat_volume *v = &volumes[f->object];
    UINT8 d[32];
    int e = volume_bytes(v, f->directory_offset, d, 32);
    if (e)
        return e;
    if (!d[0] || d[0] == 0xe5 || (d[11] & 8))
        return K_ENOENT;
    f->attributes = d[11];
    f->size = rd32(d + 28);
    f->cluster = rd16(d + 26) | (v->fat_bits == 32 ? ((UINT32)rd16(d + 20) << 16) : 0);
    if ((f->size || (f->attributes & 0x10) || f->cluster) &&
        (f->cluster < 2 || f->cluster - 2 >= v->cluster_count))
        return K_EIO;
    return 0;
}
static void fat_set_first(UINT8 d[32], UINT32 cluster, UINT64 size)
{
    d[26] = (UINT8)cluster;
    d[27] = (UINT8)(cluster >> 8);
    d[20] = (UINT8)(cluster >> 16);
    d[21] = (UINT8)(cluster >> 24);
    wr32(d + 28, (UINT32)size);
}
static int fat_replace_locked(k_file *f, const void *data, UINT64 size)
{
    if (size > FAT_WRITE_MAX)
        return K_E2BIG;
    int e = fat_refresh(f);
    if (e)
        return e;
    if (!f->directory_offset || (f->attributes & 0x11))
        return K_EROFS;
    fat_volume *v = &volumes[f->object];
    e = fat_prepare_write(v);
    if (e)
        return e;
    UINT64 per = (UINT64)v->spc * v->sector;
    UINT32 count = (UINT32)((size + per - 1) / per);
    if (count > v->free_clusters)
        return K_ENOSPC;
    UINT8 entry[32];
    e = volume_bytes(v, f->directory_offset, entry, 32);
    if (e)
        return e;
    e = fat_begin_write(v);
    if (e)
        return fat_finish_write(v, e);
    UINT32 first = 0, last = 0;
    UINT64 offset = 0;
    for (UINT32 i = 0; i < count; ++i) {
        UINT32 c = fat_allocate(v);
        if (!c) {
            e = K_ENOMEM;
            break;
        }
        if (last)
            fat_cache_set(v, last, c);
        else
            first = c;
        last = c;
        e = fat_write_data(v, c, (const UINT8 *)data + offset, MIN(per, size - offset));
        if (e)
            break;
        offset += MIN(per, size - offset);
    }
    if (!e)
        e = volume_flush(v);
    if (!e)
        e = fat_flush_cache(v);
    if (!e) {
        fat_set_first(entry, first, size);
        entry[11] |= 0x20;
        e = fat_entry_write(v, f->directory_offset, entry);
    }
    if (!e)
        e = volume_flush(v);
    if (!e) {
        UINT32 old = f->cluster;
        while (old) {
            UINT32 next = fat_cached(v, old);
            fat_cache_set(v, old, 0);
            bit_clear(v->used, old);
            ++v->free_clusters;
            if (fat_is_end(v, next))
                break;
            old = next;
        }
        f->cluster = first;
        f->size = size;
        e = fat_flush_cache(v);
    }
    return fat_finish_write(v, e);
}

static int fat_slots(fat_volume *v, UINT32 dir, UINT32 need, UINT64 slots[22], BOOLEAN *end,
                     BOOLEAN grow)
{
    UINT32 cluster = dir, run = 0, scanned = 0;
    BOOLEAN ended = FALSE;
    UINT8 sector[4096];
    for (UINT32 visited = 0; visited < 65536; ++visited) {
        UINT64 first = cluster ? v->data_start + (UINT64)(cluster - 2) * v->spc : v->root_start;
        UINT32 sectors = cluster ? v->spc : (v->root_entries * 32 + v->sector - 1) / v->sector;
        for (UINT32 s = 0; s < sectors; ++s) {
            int e = volume_bytes(v, (first + s) * v->sector, sector, v->sector);
            if (e)
                return e;
            for (UINT32 p = 0; p < v->sector; p += 32) {
                if ((!cluster && scanned >= v->root_entries) || ++scanned > 65536)
                    return K_E2BIG;
                if (!sector[p])
                    ended = TRUE;
                if (ended || sector[p] == 0xe5) {
                    slots[run++] = (first + s) * v->sector + p;
                    if (run >= need + (ended ? 1U : 0U)) {
                        *end = ended;
                        return 0;
                    }
                } else
                    run = 0;
            }
        }
        if (!cluster)
            return K_ENOSPC;
        UINT32 next = fat_cached(v, cluster);
        if (fat_is_end(v, next)) {
            if (!grow)
                return K_EAGAIN;
            next = fat_allocate(v);
            if (!next)
                return K_ENOSPC;
            int e = fat_write_data(v, next, NULL, 0);
            if (!e)
                e = volume_flush(v);
            if (!e)
                e = fat_flush_cache(v); /* New directory cluster is flushed before linking. */
            if (e)
                return e;
            fat_cache_set(v, cluster, next);
            e = fat_flush_cache(v);
            if (e)
                return e;
        }
        if (next < 2 || next - 2 >= v->cluster_count)
            return K_EIO;
        cluster = next;
    }
    return K_E2BIG;
}
typedef struct {
    const UINT8 *name;
    BOOLEAN found;
} sfn_find;
static int fat_sfn_exists(fat_volume *v, UINT32 dir, const UINT8 name[11])
{
    UINT8 sector[4096];
    UINT32 c = dir;
    for (UINT32 visited = 0; visited < 65536; ++visited) {
        UINT64 first = c ? v->data_start + (UINT64)(c - 2) * v->spc : v->root_start;
        UINT32 sectors = c ? v->spc : (v->root_entries * 32 + v->sector - 1) / v->sector;
        for (UINT32 s = 0; s < sectors; ++s) {
            int e = volume_bytes(v, (first + s) * v->sector, sector, v->sector);
            if (e)
                return e;
            for (UINT32 p = 0; p < v->sector; p += 32) {
                if (!c && ((UINT64)s * v->sector + p) / 32 >= v->root_entries)
                    return 0;
                if (!sector[p])
                    return 0;
                if (sector[p] != 0xe5 && sector[p + 11] != 0x0f && !memcmp(sector + p, name, 11))
                    return 1;
            }
        }
        if (!c)
            return 0;
        UINT32 next = fat_cached(v, c);
        if (fat_is_end(v, next))
            return 0;
        c = next;
    }
    return K_E2BIG;
}
static int fat_create_locked(UINT32 index, const char *path, BOOLEAN directory, k_file *out)
{
    char parent[K_PATH_MAX], name[K_NAME_MAX + 1];
    str_copy(parent, *path ? path : "/", sizeof(parent));
    UINTN n = str_len(parent);
    while (n > 1 && parent[n - 1] == '/')
        parent[--n] = 0;
    UINTN split = n;
    while (split && parent[split - 1] != '/')
        --split;
    str_copy(name, parent + split, sizeof(name));
    if (!name[0] || str_eq(name, ".") || str_eq(name, ".."))
        return K_EINVAL;
    for (UINTN i = 0; name[i]; ++i) {
        UINT8 ch = (UINT8)name[i];
        if (ch < 32 || ch > 126 || ch == '"' || ch == '*' || ch == ':' || ch == '<' || ch == '>' ||
            ch == '?' || ch == '\\' || ch == '|')
            return K_EINVAL;
    }
    n = str_len(name);
    if (name[n - 1] == ' ' || name[n - 1] == '.')
        return K_EINVAL;
    parent[split ? split - 1 : 0] = 0;
    k_file p, existing;
    int e = fat_open(index, parent, &p);
    if (e)
        return e;
    if (!(p.attributes & 0x10))
        return K_ENOENT;
    e = fat_open(index, path, &existing);
    if (!e)
        return K_EEXIST;
    if (e != K_ENOENT)
        return e;
    fat_volume *v = &volumes[index];
    e = fat_prepare_write(v);
    if (e)
        return e;
    if (v->free_clusters < 4)
        return K_ENOSPC;
    UINT8 shortname[11];
    static const char hex[] = "0123456789ABCDEF";
    mem_copy(shortname, "K0000000FIL", 11);
    BOOLEAN available = FALSE;
    for (UINT32 serial = 1; serial < 65536; ++serial) {
        for (UINT32 i = 0; i < 7; ++i)
            shortname[7 - i] = (UINT8)hex[(serial >> (i * 4)) & 15];
        e = fat_sfn_exists(v, p.cluster, shortname);
        if (e < 0)
            return e;
        if (!e) {
            available = TRUE;
            break;
        }
    }
    if (!available)
        return K_E2BIG;
    UINT32 lfns = (UINT32)((n + 12) / 13), needed = lfns + 1;
    UINT64 slots[22];
    BOOLEAN end = FALSE;
    e = fat_slots(v, p.cluster, needed, slots, &end, FALSE);
    BOOLEAN grow = e == K_EAGAIN;
    if (e && !grow)
        return e;
    e = fat_begin_write(v);
    if (e)
        return fat_finish_write(v, e);
    if (grow)
        e = fat_slots(v, p.cluster, needed, slots, &end, TRUE);
    if (e)
        return fat_finish_write(v, e);
    UINT8 d[32] = {0};
    if (end) {
        e = fat_entry_write(v, slots[needed], d);
        if (!e)
            e = volume_flush(v);
    }
    /* Write deleted placeholders first so a partial LFN never exposes stale entries. */
    d[0] = 0xe5;
    for (UINT32 i = 0; !e && i < needed; ++i)
        e = fat_entry_write(v, slots[i], d);
    if (!e)
        e = volume_flush(v);
    UINT32 first = 0;
    if (!e && directory) {
        first = fat_allocate(v);
        if (!first)
            e = K_ENOMEM;
        else
            e = fat_write_data(v, first, NULL, 0);
        if (!e) {
            mem_zero(d, 32);
            mem_copy(d, ".          ", 11);
            d[11] = 0x10;
            fat_set_first(d, first, 0);
            UINT64 off = (v->data_start + (UINT64)(first - 2) * v->spc) * v->sector;
            e = fat_entry_write(v, off, d);
            mem_copy(d, "..         ", 11);
            UINT32 parent_cluster = p.cluster == v->root_cluster ? 0 : p.cluster;
            fat_set_first(d, parent_cluster, 0);
            if (!e)
                e = fat_entry_write(v, off + 32, d);
        }
    }
    if (!e)
        e = volume_flush(v);
    if (!e)
        e = fat_flush_cache(v);
    static const UINT8 positions[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (UINT32 i = 0; !e && i < lfns; ++i) {
        UINT32 seq = lfns - i;
        memset(d, 0xff, 32);
        d[0] = (UINT8)(seq | (i ? 0 : 0x40));
        d[11] = 0x0f;
        d[12] = 0;
        d[13] = sfn_checksum(shortname);
        d[26] = d[27] = 0;
        for (UINT32 j = 0; j < 13; ++j) {
            UINT32 at = (seq - 1) * 13 + j;
            UINT16 ch = at < n ? (UINT8)name[at] : at == n ? 0 : 0xffff;
            d[positions[j]] = (UINT8)ch;
            d[positions[j] + 1] = (UINT8)(ch >> 8);
        }
        e = fat_entry_write(v, slots[i], d);
    }
    if (!e)
        e = volume_flush(v);
    mem_zero(d, 32);
    mem_copy(d, shortname, 11);
    d[11] = directory ? 0x10 : 0x20;
    d[16] = d[18] = d[24] = 0x21;
    fat_set_first(d, first, 0);
    if (!e)
        e = fat_entry_write(v, slots[lfns], d);
    if (!e)
        e = volume_flush(v);
    e = fat_finish_write(v, e);
    if (!e && out)
        *out = (k_file){2, index, first, d[11], 0, slots[lfns]};
    return e;
}
static int fat_store_locked(UINT32 index, const char *path, const void *data, UINT64 size)
{
    k_file f;
    int e = fat_open(index, path, &f);
    if (e == K_ENOENT)
        e = fat_create_locked(index, path, FALSE, &f);
    if (e)
        return e;
    return fat_replace_locked(&f, data, size);
}

static int fat_mount(UINT32 disk_id, UINT64 start, UINT64 length, UINT32 partition)
{
    if (volume_count == MAX_VOLUMES || mount_count == MAX_VOLUMES || disk_id >= k_disk_count() ||
        !length || start >= disks[disk_id].info.sectors ||
        length > disks[disk_id].info.sectors - start)
        return K_EINVAL;
    UINT8 boot[4096];
    int e = k_disk_read(disk_id, start, 1, boot, sizeof(boot));
    if (e)
        return e;
    UINT32 bps = rd16(boot + 11), spc = boot[13], reserved = rd16(boot + 14), fats = boot[16],
           root_entries = rd16(boot + 17);
    UINT64 total = rd16(boot + 19);
    if (!total)
        total = rd32(boot + 32);
    UINT64 fat_size = rd16(boot + 22);
    if (!fat_size)
        fat_size = rd32(boot + 36);
    if (boot[510] != 0x55 || boot[511] != 0xaa || (boot[0] != 0xeb && boot[0] != 0xe9) ||
        bps != disks[disk_id].info.sector_size || !power2(spc) || spc > 128 || !reserved || !fats ||
        fats > 2 || !fat_size || !total || total > length)
        return K_ENOTSUP;
    UINT64 root_sectors = ((UINT64)root_entries * 32 + bps - 1) / bps;
    UINT64 data = reserved + (UINT64)fats * fat_size + root_sectors;
    if (data >= total)
        return K_ENOTSUP;
    UINT64 clusters = (total - data) / spc;
    if (!clusters || clusters > 0x0fffffee)
        return K_ENOTSUP;
    UINT32 bits = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    UINT64 needed = bits == 12 ? ((clusters + 2) * 3 + 1) / 2 : (clusters + 2) * (bits / 8);
    if (needed > fat_size * bps)
        return K_ENOTSUP;
    UINT32 root = 0, active = 0;
    if (bits == 32) {
        if (root_entries || rd16(boot + 22) || rd16(boot + 42))
            return K_ENOTSUP;
        root = rd32(boot + 44);
        if (root < 2 || root - 2 >= clusters)
            return K_ENOTSUP;
        UINT16 flags = rd16(boot + 40);
        if (flags & 0x80)
            active = flags & 15;
        if (active >= fats)
            return K_ENOTSUP;
    } else if (!root_entries || !rd16(boot + 22))
        return K_ENOTSUP;
    char source[24], number[12], path[K_PATH_MAX];
    decimal_name(source, "disk", disk_id);
    UINTN pos = str_len(source);
    source[pos++] = 'p';
    decimal_name(number, "", partition);
    str_copy(source + pos, number, sizeof(source) - pos);
    str_copy(path, "/volumes/", sizeof(path));
    str_copy(path + 9, source, sizeof(path) - 9);
    e = ram_new(path, TRUE);
    if (e >= 0)
        e = 0;
    if (e && e != K_EEXIST)
        return e;
    fat_volume *v = &volumes[volume_count];
    mem_zero(v, sizeof(*v));
    v->disk = disk_id;
    v->sector = bps;
    v->spc = spc;
    v->fat_bits = bits;
    v->root_entries = root_entries;
    v->root_cluster = root;
    v->cluster_count = (UINT32)clusters;
    v->start = start;
    v->length = total;
    v->fat_start = reserved + (UINT64)active * fat_size;
    v->fat_sectors = fat_size;
    v->root_start = reserved + (UINT64)fats * fat_size;
    v->data_start = data;
    v->first_fat = reserved;
    v->fat_count = fats;
    v->media = boot[21];
    v->mirrored = bits != 32 || !(rd16(boot + 40) & 0x80);
    if (bits == 32) {
        v->fsinfo = rd16(boot + 48);
        v->backup_boot = rd16(boot + 50);
    }
    str_copy(v->source, source, sizeof(v->source));
    str_copy(mounts[mount_count].path, path, K_PATH_MAX);
    mounts[mount_count].kind = 2;
    mounts[mount_count].volume = volume_count++;
    __atomic_add_fetch(&mount_count, 1, __ATOMIC_RELEASE);
    (void)k_root_bind(source, path);
    char alias[20];
    decimal_name(alias, "disk", disk_id);
    (void)k_root_bind(alias, path);
    return 0;
}

int k_vfs_create(const char *path, k_file *out)
{
    if (!vfs_ready || !out)
        return K_EINVAL;
    char resolved[K_PATH_MAX];
    int e = k_path_resolve(path, "/", resolved);
    if (e)
        return e;
    e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    k_file exists;
    e = vfs_open_locked(resolved, &exists);
    if (!e)
        e = K_EEXIST;
    else if (e == K_ENOENT) {
        int m = choose_mount(resolved);
        if (m >= 0)
            e = mounts[m].kind >= 4
                    ? native_create(mounts[m].volume, resolved + str_len(mounts[m].path), FALSE,
                                    out)
                    : fat_create_locked(mounts[m].volume, resolved + str_len(mounts[m].path), FALSE,
                                        out);
        else {
            e = ram_new(resolved, FALSE);
            if (e >= 0)
                e = vfs_open_locked(resolved, out);
        }
    }
    k_mutex_unlock(&vfs_mutex);
    return e;
}
int k_vfs_write(k_file *file, UINT64 offset, const void *data, UINT64 n, UINT64 *written)
{
    if (!file || !written || (!data && n))
        return K_EINVAL;
    *written = 0;
    if (file->kind == 3)
        return K_EROFS;
    if (n > FAT_WRITE_MAX)
        return K_E2BIG;
    int e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    if (file->kind == 4 || file->kind == 5) {
        e = native_write(file, offset, data, n, written);
        k_mutex_unlock(&vfs_mutex);
        return e;
    }
    UINT8 *image = NULL;
    UINT32 pages = 0;
    if (file->kind == 2) {
        e = fat_refresh(file);
        if (e)
            goto done;
    } else if (file->kind == 1 && file->object < MAX_NODES && nodes[file->object].used) {
        ram_node *r = &nodes[file->object];
        file->size = r->size;
        if (r->kind != 1 || r->directory) {
            e = K_EINVAL;
            goto done;
        }
    } else {
        e = K_EINVAL;
        goto done;
    }
    if (file->attributes & 0x11) {
        e = K_EROFS;
        goto done;
    }
    if (offset == ~0ULL)
        offset = file->size;
    if (file->size > FAT_WRITE_MAX || offset > FAT_WRITE_MAX || n > FAT_WRITE_MAX - offset) {
        e = K_E2BIG;
        goto done;
    }
    if (!n)
        goto done;
    UINT64 size = file->size > offset + n ? file->size : offset + n;
    pages = (UINT32)((size + 4095) / 4096);
    image = (void *)(UINTN)k_pmm_alloc_pages(pages, 0);
    if (!image) {
        e = K_ENOMEM;
        goto done;
    }
    if (file->kind == 2) {
        UINT64 got = 0;
        e = fat_read(file, 0, image, file->size, &got);
        if (!e && got != file->size)
            e = K_EIO;
        if (e)
            goto done;
    } else if (file->size)
        mem_copy(image, nodes[file->object].data, file->size);
    mem_copy(image + offset, data, n);
    if (file->kind == 2)
        e = fat_replace_locked(file, image, size);
    else {
        ram_node *r = &nodes[file->object];
        if (r->data)
            k_pmm_free_pages((UINT64)(UINTN)r->data, r->pages);
        r->data = image;
        r->pages = pages;
        r->size = size;
        file->size = size;
        image = NULL;
    }
    if (!e)
        *written = n;
done:
    if (image)
        k_pmm_free_pages((UINT64)(UINTN)image, pages);
    k_mutex_unlock(&vfs_mutex);
    return e;
}
int k_vfs_sync(void)
{
    int e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    for (UINT32 i = 0; i < volume_count; ++i) {
        fat_volume *v = &volumes[i];
        int result = 0;
        if (v->write_failed)
            result = K_EIO;
        else if (v->write_ready)
            result = volume_flush(v);
        if (result && !e)
            e = result;
    }
    int ne = native_sync_all();
    if (!e)
        e = ne;
    k_mutex_unlock(&vfs_mutex);
    return e;
}
int k_vfs_mount_ramfat(UINT32 id)
{
    if (k_cpu_id() != 0 || id >= k_disk_count() || disks[id].kind != DISK_RAM)
        return K_EPERM;
    int e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    for (UINT32 i = 0; i < volume_count; ++i)
        if (volumes[i].disk == id) {
            k_mutex_unlock(&vfs_mutex);
            return K_EEXIST;
        }
    e = fat_mount(id, 0, disks[id].info.sectors, 0);
    k_mutex_unlock(&vfs_mutex);
    return e;
}

#define NATIVE_RUNS 256
#define NATIVE_SCAN_MAX (32ULL * 1024 * 1024)
typedef struct {
    UINT64 logical, physical, length;
    BOOLEAN hole;
} native_run;
typedef struct {
    UINT32 kind, disk, sector, block;
    UINT64 start, length, blocks;
    BOOLEAN read_only, failed, wrote;
    char source[24];
    UINT8 super[1024];
    UINT32 inode_size, inodes, inodes_per_group, blocks_per_group, groups, desc_size, csum_seed;
    BOOLEAN csum, gdt_csum;
    UINT32 record_size, index_size, mft_count;
    UINT64 mft_size;
    native_run mft[NATIVE_RUNS];
    UINT32 mft_runs;
} native_volume;
static native_volume native_volumes[MAX_VOLUMES];
static UINT32 native_count;
static UINT32 crc32c_step(UINT32 crc, const UINT8 *p, UINTN n)
{
    while (n--) {
        crc ^= *p++;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0x82f63b78u & (0 - (crc & 1)));
    }
    return crc;
}
static UINT16 crc16_step(UINT16 crc, const UINT8 *p, UINTN n)
{
    while (n--) {
        crc ^= *p++;
        for (int bit = 0; bit < 8; ++bit)
            crc = (UINT16)((crc >> 1) ^ (0xa001u & (0 - (crc & 1))));
    }
    return crc;
}
static int native_bytes(native_volume *v, UINT64 offset, void *out, UINT64 bytes)
{
    if (!span_ok(offset, bytes, v->length * v->sector))
        return K_EIO;
    return disk_bytes(v->disk, v->start * v->sector + offset, out, bytes);
}
static int native_write_bytes(native_volume *v, UINT64 offset, const void *in, UINT64 bytes)
{
    if (v->read_only || v->failed)
        return K_EROFS;
    if (!span_ok(offset, bytes, v->length * v->sector))
        return K_EIO;
    UINT8 sector[4096];
    const UINT8 *p = in;
    int e = 0;
    while (bytes && !e) {
        UINT64 lba = offset / v->sector;
        UINT32 at = (UINT32)(offset % v->sector), n = (UINT32)MIN(bytes, v->sector - at);
        if (at || n < v->sector)
            e = native_bytes(v, lba * v->sector, sector, v->sector);
        if (e)
            break;
        mem_copy(sector + at, p, n);
        e = disk_write_fs(v->disk, v->start + lba, 1, sector, v->sector);
        p += n;
        offset += n;
        bytes -= n;
    }
    if (!e)
        e = disk_flush(v->disk);
    if (e)
        v->failed = TRUE;
    else
        v->wrote = TRUE;
    return e;
}
static int run_add(native_volume *v, native_run *runs, UINT32 *count, UINT64 logical,
                   UINT64 physical, UINT64 length, BOOLEAN hole)
{
    if (!length || *count == NATIVE_RUNS || logical > ~0ULL - length ||
        (!hole && (physical >= v->blocks || length > v->blocks - physical)))
        return K_EIO;
    if (*count && logical < runs[*count - 1].logical + runs[*count - 1].length)
        return K_EIO;
    if (!hole)
        for (UINT32 i = 0; i < *count; ++i)
            if (!runs[i].hole && overlap(physical, length, runs[i].physical, runs[i].length))
                return K_EIO;
    runs[(*count)++] = (native_run){logical, physical, length, hole};
    return 0;
}
static int run_io(native_volume *v, const native_run *runs, UINT32 count, UINT64 offset, void *data,
                  UINT64 bytes, BOOLEAN write)
{
    UINT8 *p = data;
    while (bytes) {
        UINT64 logical = offset / v->block, at = offset % v->block, n = MIN(bytes, v->block - at);
        const native_run *r = NULL;
        for (UINT32 i = 0; i < count; ++i)
            if (logical >= runs[i].logical && logical - runs[i].logical < runs[i].length) {
                r = &runs[i];
                break;
            }
        if (!r || r->hole) {
            if (write)
                return K_ENOTSUP;
            mem_zero(p, n);
        } else {
            UINT64 physical = (r->physical + logical - r->logical) * v->block + at;
            int e = write ? native_write_bytes(v, physical, p, n) : native_bytes(v, physical, p, n);
            if (e)
                return e;
        }
        offset += n;
        p += n;
        bytes -= n;
    }
    return 0;
}
static int ext_group(native_volume *v, UINT32 group, UINT8 desc[64])
{
    if (group >= v->groups)
        return K_EIO;
    UINT64 table = (v->block == 1024 ? 2ULL : 1ULL) * v->block;
    int e = native_bytes(v, table + (UINT64)group * v->desc_size, desc, v->desc_size);
    if (e)
        return e;
    UINT8 g[4];
    wr32(g, group);
    UINT16 wanted = rd16(desc + 30);
    desc[30] = desc[31] = 0;
    if (v->csum) {
        UINT32 crc = crc32c_step(v->csum_seed, g, 4);
        crc = crc32c_step(crc, desc, v->desc_size);
        if ((UINT16)crc != wanted)
            e = K_EIO;
    } else if (v->gdt_csum) {
        UINT16 crc = crc16_step(0xffff, v->super + 104, 16);
        crc = crc16_step(crc, g, 4);
        crc = crc16_step(crc, desc, 30);
        if (v->desc_size > 32)
            crc = crc16_step(crc, desc + 32, v->desc_size - 32);
        if (crc != wanted)
            e = K_EIO;
    }
    if (!e) {
        UINT64 table = rd32(desc + 8) | (v->desc_size == 64 ? (UINT64)rd32(desc + 40) << 32 : 0);
        UINT64 bitmap = rd32(desc) | (v->desc_size == 64 ? (UINT64)rd32(desc + 32) << 32 : 0);
        UINT64 inodemap = rd32(desc + 4) | (v->desc_size == 64 ? (UINT64)rd32(desc + 36) << 32 : 0);
        UINT64 blocks = ((UINT64)v->inodes_per_group * v->inode_size + v->block - 1) / v->block;
        if (!table || table >= v->blocks || blocks > v->blocks - table || !bitmap ||
            bitmap >= v->blocks || !inodemap || inodemap >= v->blocks || bitmap == inodemap ||
            overlap(bitmap, 1, table, blocks) || overlap(inodemap, 1, table, blocks))
            e = K_EIO;
    }
    wr16(desc + 30, wanted);
    return e;
}
static UINT64 ext_desc_block(native_volume *v, const UINT8 *d, UINT32 off)
{
    return rd32(d + off) | (v->desc_size == 64 ? (UINT64)rd32(d + off + 32) << 32 : 0);
}
static UINT32 ext_inode_seed(native_volume *v, UINT32 inode, const UINT8 *raw)
{
    UINT8 n[4];
    wr32(n, inode);
    UINT32 c = crc32c_step(v->csum_seed, n, 4);
    return crc32c_step(c, raw + 100, 4);
}
static int ext_inode(native_volume *v, UINT32 inode, UINT8 raw[512])
{
    if (!inode || inode > v->inodes)
        return K_EIO;
    UINT8 d[64];
    UINT32 group = (inode - 1) / v->inodes_per_group;
    int e = ext_group(v, group, d);
    if (e)
        return e;
    UINT64 table = ext_desc_block(v, d, 8);
    UINT64 offset = (UINT64)((inode - 1) % v->inodes_per_group) * v->inode_size;
    if (!table || table >= v->blocks || offset + v->inode_size > (v->blocks - table) * v->block)
        return K_EIO;
    e = native_bytes(v, table * v->block + offset, raw, v->inode_size);
    if (e)
        return e;
    if (v->csum) {
        UINT32 wanted = rd16(raw + 124);
        wr16(raw + 124, 0);
        BOOLEAN high = v->inode_size >= 132 && rd16(raw + 128) >= 4;
        if (high) {
            wanted |= (UINT32)rd16(raw + 130) << 16;
            wr16(raw + 130, 0);
        }
        UINT32 crc = crc32c_step(ext_inode_seed(v, inode, raw), raw, v->inode_size);
        wr16(raw + 124, (UINT16)wanted);
        if (high)
            wr16(raw + 130, (UINT16)(wanted >> 16));
        if ((high ? crc : (UINT16)crc) != wanted)
            return K_EIO;
    }
    if (!rd16(raw + 26) || rd32(raw + 20))
        return K_ENOENT;
    if (rd32(raw + 32) & (0x10000000u | 0x800u | 0x4u))
        return K_ENOTSUP;
    return 0;
}
static int ext_extents(native_volume *v, UINT32 seed, const UINT8 *h, UINT32 bytes, UINT32 depth,
                       native_run *runs, UINT32 *count, UINT32 *budget)
{
    if (!*budget || bytes < 12 || rd16(h) != 0xf30a || rd16(h + 6) != depth || depth > 5)
        return K_EIO;
    --*budget;
    UINT32 entries = rd16(h + 2), max = rd16(h + 4);
    if (entries > max || max > (bytes - 12) / 12 || !max)
        return K_EIO;
    UINT64 last = 0;
    for (UINT32 i = 0; i < entries; ++i) {
        const UINT8 *x = h + 12 + i * 12;
        UINT64 logical = rd32(x);
        if (i && logical <= last)
            return K_EIO;
        last = logical;
        if (!depth) {
            UINT32 len = rd16(x + 4);
            BOOLEAN hole = len > 32768;
            if (hole)
                len -= 32768;
            UINT64 physical = rd32(x + 8) | ((UINT64)rd16(x + 6) << 32);
            int e = run_add(v, runs, count, logical, physical, len, hole);
            if (e)
                return e;
        } else {
            UINT64 physical = rd32(x + 4) | ((UINT64)rd16(x + 8) << 32);
            if (!physical || physical >= v->blocks)
                return K_EIO;
            UINT64 page = pmm_alloc_page();
            if (!page)
                return K_ENOMEM;
            UINT8 *b = (void *)(UINTN)page;
            int e = native_bytes(v, physical * v->block, b, v->block);
            if (!e && v->csum) {
                UINT32 tail = 12 + 12 * rd16(b + 4);
                if (tail > v->block - 4 || crc32c_step(seed, b, tail) != rd32(b + tail))
                    e = K_EIO;
            }
            if (!e)
                e = ext_extents(v, seed, b, v->block, depth - 1, runs, count, budget);
            pmm_free_page(page);
            if (e)
                return e;
        }
    }
    return 0;
}
static int ext_map(native_volume *v, UINT32 inode, const UINT8 *raw, native_run *runs,
                   UINT32 *count)
{
    *count = 0;
    if (rd32(raw + 32) & 0x80000) {
        UINT32 budget = 1024;
        return ext_extents(v, ext_inode_seed(v, inode, raw), raw + 40, 60, rd16(raw + 46), runs,
                           count, &budget);
    }

    if (rd32(raw + 92) || rd32(raw + 96))
        return K_ENOTSUP;
    for (UINT32 i = 0; i < 12; ++i) {
        UINT32 p = rd32(raw + 40 + 4 * i);
        if (p) {
            int e = run_add(v, runs, count, i, p, 1, FALSE);
            if (e)
                return e;
        }
    }
    UINT32 indirect = rd32(raw + 88);
    if (indirect) {
        if (indirect >= v->blocks)
            return K_EIO;
        UINT8 b[4096];
        int e = native_bytes(v, (UINT64)indirect * v->block, b, v->block);
        if (e)
            return e;
        for (UINT32 i = 0; i < v->block / 4; ++i) {
            UINT32 p = rd32(b + 4 * i);
            if (p) {
                e = run_add(v, runs, count, 12 + i, p, 1, FALSE);
                if (e)
                    return e;
            }
        }
    }
    return 0;
}
static UINT64 ext_size(const UINT8 *raw) { return rd32(raw + 4) | ((UINT64)rd32(raw + 108) << 32); }
static int ext_directory(native_volume *v, UINT32 inode, const char *wanted, k_file *found,
                         k_dir_callback cb, void *arg)
{
    UINT8 raw[512];
    int e = ext_inode(v, inode, raw);
    if (e)
        return e;
    if ((rd16(raw) & 0xf000) != 0x4000)
        return K_EINVAL;
    UINT64 size = ext_size(raw);
    if (size > NATIVE_SCAN_MAX || size % v->block)
        return K_ENOTSUP;
    native_run runs[NATIVE_RUNS];
    UINT32 count;
    e = ext_map(v, inode, raw, runs, &count);
    if (e)
        return e;
    UINT8 block[4096];
    UINT32 seed = ext_inode_seed(v, inode, raw);
    for (UINT64 off = 0; off < size; off += v->block) {
        e = run_io(v, runs, count, off, block, v->block, FALSE);
        if (e)
            return e;
        UINT32 limit = v->block;
        if (v->csum) {

            if (rd32(block + v->block - 12) == 0 && rd16(block + v->block - 8) == 12 &&
                block[v->block - 5] == 0xde) {
                if (crc32c_step(seed, block, v->block - 12) != rd32(block + v->block - 4))
                    return K_EIO;
                limit -= 12;
            } else if (!(rd32(raw + 32) & 0x1000))
                return K_EIO;
        }
        for (UINT32 at = 0; at < limit;) {
            if (limit - at < 8)
                return K_EIO;
            UINT32 ino = rd32(block + at), rec = rd16(block + at + 4), len = block[at + 6];
            if (rec < 8 || (rec & 3) || rec > limit - at || len > rec - 8)
                return K_EIO;
            if (ino && len && len <= 255) {
                char name[256];
                mem_copy(name, block + at + 8, len);
                name[len] = 0;
                for (UINT32 j = 0; j < len; ++j)
                    if (!name[j])
                        return K_EIO;
                if (wanted && str_eq(name, wanted)) {
                    UINT8 target[512];
                    e = ext_inode(v, ino, target);
                    if (e)
                        return e;
                    UINT32 mode = rd16(target) & 0xf000;
                    if (mode != 0x4000 && mode != 0x8000)
                        return K_ENOTSUP;
                    *found = (k_file){4,
                                      (UINT32)(v - native_volumes),
                                      ino,
                                      mode == 0x4000 ? 0x10 : 0,
                                      ext_size(target),
                                      rd32(target + 100)};
                    return 0;
                }
                if (cb) {
                    k_dirent entry;
                    mem_zero(&entry, sizeof(entry));
                    str_copy(entry.name, name, sizeof(entry.name));
                    entry.directory = block[at + 7] == 2;
                    UINT8 target[512];
                    e = ext_inode(v, ino, target);
                    if (e)
                        return e;
                    entry.size = ext_size(target);
                    entry.directory = (rd16(target) & 0xf000) == 0x4000;
                    cb(&entry, arg);
                }
            }
            at += rec;
        }
    }
    return wanted ? K_ENOENT : 0;
}
/* Verify NTFS USA fixups before parsing attributes or indexes. */
static int ntfs_fixup(native_volume *v, UINT8 *record, UINT32 size, const char *magic)
{
    if (size < 48 || memcmp(record, magic, 4))
        return K_EIO;
    UINT32 off = rd16(record + 4), count = rd16(record + 6);
    /* NTFS USA protection units are 512 bytes, including on 4Kn media. */
    (void)v;
    if (size % 512 || count != size / 512 + 1 || off < 8 || off > size || count * 2 > size - off)
        return K_EIO;
    UINT16 sequence = rd16(record + off);
    for (UINT32 i = 1; i < count; ++i) {
        UINT32 end = i * 512 - 2;
        if (rd16(record + end) != sequence)
            return K_EIO;
        wr16(record + end, rd16(record + off + i * 2));
    }
    return 0;
}
static int ntfs_runs(native_volume *v, const UINT8 *a, UINT32 size, native_run *runs, UINT32 *count)
{
    if (size < 64 || a[8] != 1 || rd64(a + 16) != 0 || rd16(a + 12) & 0xc001 || rd16(a + 34))
        return K_ENOTSUP;
    UINT32 at = rd16(a + 32);
    UINT64 vcn = 0;
    INT64 lcn = 0;
    *count = 0;
    if (at < 64 || at >= size)
        return K_EIO;
    while (at < size && a[at]) {
        UINT8 header = a[at++];
        UINT32 lenbytes = header & 15, offbytes = header >> 4;
        if (!lenbytes || lenbytes > 8 || offbytes > 8 || lenbytes + offbytes > size - at)
            return K_EIO;
        UINT64 len = 0, delta = 0;
        for (UINT32 i = 0; i < lenbytes; ++i)
            len |= (UINT64)a[at++] << (8 * i);
        for (UINT32 i = 0; i < offbytes; ++i)
            delta |= (UINT64)a[at++] << (8 * i);
        if (offbytes && offbytes < 8 && (delta & (1ULL << (offbytes * 8 - 1))))
            delta |= ~0ULL << (offbytes * 8);
        if (offbytes) {
            INT64 change = (INT64)delta;
            if (change < 0) {
                UINT64 sub = 0 - delta;
                if (sub > (UINT64)lcn)
                    return K_EIO;
                lcn -= (INT64)sub;
            } else {
                if ((UINT64)change > 0x7fffffffffffffffULL - (UINT64)lcn)
                    return K_EIO;
                lcn += change;
            }
        }
        int e = run_add(v, runs, count, vcn, (UINT64)lcn, len, !offbytes);
        if (e)
            return e;
        vcn += len;
    }
    if (at >= size || !vcn || vcn > ~0ULL / v->block || rd64(a + 24) != vcn - 1 ||
        rd64(a + 56) > rd64(a + 48) || rd64(a + 48) > vcn * v->block)
        return K_EIO;
    return 0;
}
static int ntfs_record(native_volume *v, UINT32 number, UINT8 record[4096])
{
    UINT64 off = (UINT64)number * v->record_size;
    if (!span_ok(off, v->record_size, v->mft_size))
        return K_EIO;
    int e = run_io(v, v->mft, v->mft_runs, off, record, v->record_size, FALSE);
    if (e)
        return e;
    e = ntfs_fixup(v, record, v->record_size, "FILE");
    if (e)
        return e;
    UINT32 used = rd32(record + 24), alloc = rd32(record + 28), attrs = rd16(record + 20);
    if (!(rd16(record + 22) & 1) || used > v->record_size || alloc != v->record_size ||
        attrs < 48 || attrs > used || rd64(record + 32))
        return K_EIO;
    return 0;
}
static int ntfs_attr(native_volume *v, UINT8 *record, UINT32 type, const char *name, UINT8 **found,
                     UINT32 *length)
{
    (void)v;
    UINT32 at = rd16(record + 20), used = rd32(record + 24);
    BOOLEAN seen = FALSE;
    while (at + 4 <= used) {
        UINT32 kind = rd32(record + at);
        if (kind == 0xffffffff)
            return seen ? 0 : K_ENOENT;
        if (at + 16 > used)
            return K_EIO;
        UINT8 *a = record + at;
        UINT32 n = rd32(a + 4);
        if (n < 24 || (n & 7) || n > used - at || a[8] > 1)
            return K_EIO;
        if (kind == 0x20)
            return K_ENOTSUP; /* Attribute lists require multi-record joining and are not handled
                                 here. */
        UINT32 chars = a[9], noff = rd16(a + 10);
        if (chars && (!span_ok(noff, chars * 2, n) || noff < 16))
            return K_EIO;
        BOOLEAN match = kind == type && chars == str_len(name);
        for (UINT32 i = 0; match && i < chars; ++i)
            if (rd16(a + noff + i * 2) != (UINT8)name[i])
                match = FALSE;
        if (match) {
            if (seen)
                return K_ENOTSUP;
            *found = a;
            *length = n;
            seen = TRUE;
        }
        at += n;
    }
    return K_EIO;
}
static int ntfs_value(UINT8 *a, UINT32 n, UINT8 **bytes, UINT32 *size)
{
    if (a[8])
        return K_ENOTSUP;
    UINT32 at = rd16(a + 20), len = rd32(a + 16);
    if (at < 24 || !span_ok(at, len, n) || rd16(a + 12))
        return K_EIO;
    *bytes = a + at;
    *size = len;
    return 0;
}
static int ntfs_store_record(native_volume *, UINT32, const UINT8 *);
static int ntfs_dirty(native_volume *, BOOLEAN);
static int ntfs_regular_writable(native_volume *, UINT32, UINT8 *);
static int ntfs_data_boundaries(native_volume *, const native_run *, UINT32);
static int ntfs_data(native_volume *v, UINT32 ino, UINT64 offset, void *buffer, UINT64 n,
                     UINT64 *size, BOOLEAN write)
{
    UINT8 record[4096], *a;
    UINT32 length;
    int e = ntfs_record(v, ino, record);
    if (e)
        return e;
    if (write) {
        e = ntfs_regular_writable(v, ino, record);
        if (e)
            return e;
    }
    if (rd16(record + 22) & 2)
        return K_EINVAL;
    e = ntfs_attr(v, record, 0x80, "", &a, &length);
    if (e)
        return e;
    if (!a[8]) {
        UINT8 *bytes;
        UINT32 len;
        e = ntfs_value(a, length, &bytes, &len);
        if (e)
            return e;
        *size = len;
        if (offset >= len)
            return n && write ? K_ENOTSUP : 0;
        n = MIN(n, len - offset);
        if (write) {
            mem_copy(bytes + offset, buffer, n);
            e = ntfs_dirty(v, TRUE);
            if (!e)
                e = ntfs_store_record(v, ino, record);
            if (!e)
                e = ntfs_dirty(v, FALSE);
            if (e)
                v->failed = TRUE;
            return e;
        }
        mem_copy(buffer, bytes + offset, n);
        return 0;
    }
    native_run runs[NATIVE_RUNS];
    UINT32 count;
    e = ntfs_runs(v, a, length, runs, &count);
    if (e)
        return e;
    *size = rd64(a + 48);
    if (offset >= *size)
        return n && write ? K_ENOTSUP : 0;
    n = MIN(n, *size - offset);
    UINT64 initialized = rd64(a + 56);
    if (write) {
        if (ino < 24 || rd16(record + 18) != 1 || offset > initialized || n > initialized - offset)
            return K_ENOTSUP;
        /* Reject data runs overlapping the MFT allocation. */
        for (UINT32 i = 0; i < count; ++i)
            for (UINT32 j = 0; j < v->mft_runs; ++j)
                if (!runs[i].hole && !v->mft[j].hole &&
                    overlap(runs[i].physical, runs[i].length, v->mft[j].physical, v->mft[j].length))
                    return K_EIO;
        for (UINT32 i = 0; i < count; ++i)
            if (runs[i].hole)
                return K_ENOTSUP;
    }
    if (write) {
        e = ntfs_data_boundaries(v, runs, count);
        if (e)
            return e;
    }
    UINT64 backed = offset < initialized ? MIN(n, initialized - offset) : 0;
    if (backed)
        e = run_io(v, runs, count, offset, buffer, backed, write);
    if (!e && !write && n > backed)
        mem_zero((UINT8 *)buffer + backed, n - backed);
    return e;
}
static int ntfs_store_record(native_volume *v, UINT32 ino, const UINT8 *unpacked)
{
    UINT8 raw[4096];
    mem_copy(raw, unpacked, v->record_size);
    UINT32 usa = rd16(raw + 4), count = rd16(raw + 6);
    if (count != v->record_size / 512 + 1 || usa < 8 || usa + count * 2 > v->record_size)
        return K_EIO;
    UINT16 sequence = (UINT16)(rd16(raw + usa) + 1);
    if (!sequence || sequence == 0xffff)
        sequence = 1;
    wr16(raw + usa, sequence);
    for (UINT32 i = 1; i < count; ++i) {
        wr16(raw + usa + i * 2, rd16(raw + i * 512 - 2));
        wr16(raw + i * 512 - 2, sequence);
    }
    int e = run_io(v, v->mft, v->mft_runs, (UINT64)ino * v->record_size, raw, v->record_size, TRUE);
    UINT64 mirror = rd64(v->super + 56);
    if (!e && ino < 4) {
        if (!mirror || mirror >= v->blocks)
            return K_EIO;
        e = native_write_bytes(v, mirror * v->block + (UINT64)ino * v->record_size, raw,
                               v->record_size);
    }
    return e;
}
static int ntfs_dirty(native_volume *v, BOOLEAN dirty)
{
    UINT8 raw[4096], *a, *value;
    UINT32 n, size;
    int e = ntfs_record(v, 3, raw);
    if (e)
        return e;
    e = ntfs_attr(v, raw, 0x70, "", &a, &n);
    if (e)
        return e;
    e = ntfs_value(a, n, &value, &size);
    if (e)
        return e;
    if (size < 12)
        return K_EIO;
    UINT16 flags = rd16(value + 10);
    if (flags & ~1u)
        return K_EROFS;
    wr16(value + 10, dirty ? (flags | 1) : (flags & ~1u));
    return ntfs_store_record(v, 3, raw);
}
static int ntfs_regular_writable(native_volume *v, UINT32 ino, UINT8 *record)
{
    if (ino < 24 || rd16(record + 18) != 1 || (rd16(record + 22) & 2))
        return K_ENOTSUP;
    UINT8 *a, *value;
    UINT32 n, size;
    int e = ntfs_attr(v, record, 0x10, "", &a, &n);
    if (e)
        return e;
    e = ntfs_value(a, n, &value, &size);
    if (e)
        return e;
    /* Reject unsupported NTFS mutation flags. */
    if (size < 36 || (rd32(value + 32) & (1u | 4u | 0x200u | 0x400u | 0x800u | 0x1000u | 0x4000u)))
        return K_EROFS;
    return 0;
}
static int ntfs_metadata_boundaries(native_volume *v, const native_run *runs, UINT32 count)
{
    /* Reserved MFT records may own nonresident metadata; include them in overlap checks. */
    UINT8 raw[4096];
    for (UINT32 ino = 0; ino < 24 && ino < v->mft_count; ++ino) {
        int e = ntfs_record(v, ino, raw);
        if (e) {
            e = run_io(v, v->mft, v->mft_runs, (UINT64)ino * v->record_size, raw, v->record_size,
                       FALSE);
            if (e)
                return e;
            if (ntfs_fixup(v, raw, v->record_size, "FILE"))
                return K_EIO;
            if (!(rd16(raw + 22) & 1))
                continue;
            return K_ENOTSUP;
        }
        UINT32 at = rd16(raw + 20), used = rd32(raw + 24);
        while (at + 4 <= used && rd32(raw + at) != 0xffffffff) {
            UINT8 *a = raw + at;
            UINT32 len = rd32(a + 4);
            if (len < 24 || len > used - at || len & 7)
                return K_EIO;
            if (rd32(a) == 0x20)
                return K_ENOTSUP;
            if (a[8]) {
                native_run protected[NATIVE_RUNS];
                UINT32 pc;
                /* $BadClus may be sparse; other unsupported encoding flags still reject. */
                UINT16 flags = rd16(a + 12);
                wr16(a + 12, flags & ~0x8000u);
                e = ntfs_runs(v, a, len, protected, &pc);
                wr16(a + 12, flags);
                if (e)
                    return e;
                for (UINT32 i = 0; i < count; ++i)
                    for (UINT32 j = 0; j < pc; ++j)
                        if (!runs[i].hole && !protected[j].hole &&
                            overlap(runs[i].physical, runs[i].length, protected[j].physical,
                                    protected[j].length))
                            return K_EIO;
            }
            at += len;
        }
    }
    return 0;
}
static int ntfs_data_boundaries(native_volume *v, const native_run *runs, UINT32 count)
{
    int checked = ntfs_metadata_boundaries(v, runs, count);
    if (checked)
        return checked;
    /* Verify each NTFS data run against $Bitmap. */
    UINT64 bitmap_size;
    UINT8 bit;
    for (UINT32 i = 0; i < count; ++i) {
        if (runs[i].hole)
            return K_ENOTSUP;
        if (runs[i].physical + runs[i].length >= v->blocks)
            return K_EIO;
        for (UINT64 c = runs[i].physical; c < runs[i].physical + runs[i].length; ++c) {
            int e = ntfs_data(v, 6, c / 8, &bit, 1, &bitmap_size, FALSE);
            if (e)
                return e;
            if (c / 8 >= bitmap_size || !(bit & (1u << (c % 8))))
                return K_EIO;
        }
    }
    return 0;
}

static int ntfs_name(const UINT8 *key, UINT32 n, char out[256])
{
    if (n < 66 || 66u + 2u * key[64] > n)
        return K_EIO;
    UINT32 len = key[64];
    for (UINT32 i = 0; i < len; ++i) {
        UINT16 c = rd16(key + 66 + i * 2);
        if (c < 32 || c > 126 || c == '/')
            return K_ENOTSUP;
        out[i] = (char)c;
    }
    out[len] = 0;
    return len ? 0 : K_EIO;
}
static BOOLEAN ntfs_name_equal(const char *a, const char *b)
{
    while (*a && *b) {
        UINT8 x = (UINT8)*a++, y = (UINT8)*b++;
        if (x >= 'a' && x <= 'z')
            x -= 32;
        if (y >= 'a' && y <= 'z')
            y -= 32;
        if (x != y)
            return FALSE;
    }
    return !*a && !*b;
}
static int ntfs_index_entries(native_volume *v, const UINT8 *header, UINT32 capacity,
                              const char *wanted, k_file *found, k_dir_callback cb, void *arg)
{
    if (capacity < 16)
        return K_EIO;
    UINT32 off = rd32(header), size = rd32(header + 4), alloc = rd32(header + 8);
    if (off < 16 || size < off || size > alloc || alloc > capacity)
        return K_EIO;
    for (UINT32 at = off; at + 16 <= size;) {
        const UINT8 *entry = header + at;
        UINT32 len = rd16(entry + 8), keylen = rd16(entry + 10), flags = rd16(entry + 12);
        if (len < 16 + ((flags & 1) ? 8u : 0u) || (len & 7) || len > size - at || (flags & ~3) ||
            keylen > len - 16 - ((flags & 1) ? 8 : 0))
            return K_EIO;
        if (flags & 2)
            return 0;
        char name[256];
        int e = ntfs_name(entry + 16, keylen, name);
        if (e != K_ENOTSUP && e)
            return e;
        if (!e) {
            UINT64 reference = rd64(entry), ino = reference & 0xffffffffffffULL;
            if (ino > 0xffffffff)
                return K_ENOTSUP;
            if (wanted && ntfs_name_equal(name, wanted)) {
                UINT8 raw[4096];
                e = ntfs_record(v, (UINT32)ino, raw);
                if (e)
                    return e;
                if (rd16(raw + 16) != (reference >> 48))
                    return K_EIO;
                UINT32 attrs = rd32(entry + 16 + 56);
                if (attrs & 0x400)
                    return K_ENOTSUP;
                *found = (k_file){5,
                                  (UINT32)(v - native_volumes),
                                  (UINT32)ino,
                                  (rd16(raw + 22) & 2) ? 0x10 : 0,
                                  rd64(entry + 16 + 48),
                                  reference >> 48};
                if (!(found->attributes & 0x10)) {
                    UINT64 actual;
                    e = ntfs_data(v, (UINT32)ino, 0, NULL, 0, &actual, FALSE);
                    if (e)
                        return e;
                    found->size = actual;
                }
                return 1;
            }
            if (cb && entry[16 + 65] != 2) {
                k_dirent d;
                mem_zero(&d, sizeof(d));
                str_copy(d.name, name, sizeof(d.name));
                d.size = rd64(entry + 16 + 48);
                d.directory = (rd32(entry + 16 + 56) & 0x10000000) != 0;
                cb(&d, arg);
            }
        }
        at += len;
    }
    return K_EIO;
}
static int ntfs_directory(native_volume *v, UINT32 ino, const char *name, k_file *found,
                          k_dir_callback cb, void *arg)
{
    UINT8 record[4096], *attr, *value;
    UINT32 length, bytes;
    int e = ntfs_record(v, ino, record);
    if (e)
        return e;
    if (!(rd16(record + 22) & 2))
        return K_EINVAL;
    e = ntfs_attr(v, record, 0x90, "$I30", &attr, &length);
    if (e)
        return e;
    e = ntfs_value(attr, length, &value, &bytes);
    if (e)
        return e;
    if (bytes < 32 || rd32(value) != 0x30 || rd32(value + 4) != 1 ||
        rd32(value + 8) != v->index_size)
        return K_ENOTSUP;
    e = ntfs_index_entries(v, value + 16, bytes - 16, name, found, cb, arg);
    if (e)
        return e == 1 ? 0 : e;
    e = ntfs_attr(v, record, 0xa0, "$I30", &attr, &length);
    if (e == K_ENOENT)
        return name ? K_ENOENT : 0;
    if (e)
        return e;
    native_run runs[NATIVE_RUNS];
    UINT32 count;
    e = ntfs_runs(v, attr, length, runs, &count);
    if (e)
        return e;
    UINT64 size = rd64(attr + 48);
    if (size > NATIVE_SCAN_MAX || size % v->index_size)
        return K_ENOTSUP;
    UINT8 *bitmap_attr;
    UINT32 bitmap_len;
    e = ntfs_attr(v, record, 0xb0, "$I30", &bitmap_attr, &bitmap_len);
    if (e)
        return e;
    UINT8 bitmap[4096];
    UINT32 bitmap_size;
    if (!bitmap_attr[8]) {
        UINT8 *p;
        e = ntfs_value(bitmap_attr, bitmap_len, &p, &bitmap_size);
        if (e)
            return e;
        if (bitmap_size > sizeof(bitmap))
            return K_ENOTSUP;
        mem_copy(bitmap, p, bitmap_size);
    } else {
        native_run br[NATIVE_RUNS];
        UINT32 bn;
        e = ntfs_runs(v, bitmap_attr, bitmap_len, br, &bn);
        if (e)
            return e;
        if (rd64(bitmap_attr + 48) > sizeof(bitmap))
            return K_ENOTSUP;
        bitmap_size = (UINT32)rd64(bitmap_attr + 48);
        e = run_io(v, br, bn, 0, bitmap, bitmap_size, FALSE);
        if (e)
            return e;
    }
    if (size / v->index_size > (UINT64)bitmap_size * 8)
        return K_EIO;
    UINT8 block[4096];
    for (UINT64 off = 0; off < size; off += v->index_size) {
        UINT32 bit = (UINT32)(off / v->index_size);
        if (!(bitmap[bit / 8] & (1u << (bit % 8))))
            continue;
        e = run_io(v, runs, count, off, block, v->index_size, FALSE);
        if (e)
            return e;
        e = ntfs_fixup(v, block, v->index_size, "INDX");
        if (e)
            return e;
        e = ntfs_index_entries(v, block + 24, v->index_size - 24, name, found, cb, arg);
        if (e)
            return e == 1 ? 0 : e;
    }
    return name ? K_ENOENT : 0;
}
static int native_open(UINT32 index, const char *path, k_file *out)
{
    if (index >= native_count)
        return K_ENOENT;
    native_volume *v = &native_volumes[index];
    k_file cur = {v->kind, index, v->kind == 4 ? 2 : 5, 0x10, 0, 0};
    while (*path) {
        while (*path == '/')
            ++path;
        if (!*path)
            break;
        char name[256];
        UINT32 n = 0;
        while (*path && *path != '/') {
            if (n == 255)
                return K_E2BIG;
            name[n++] = *path++;
        }
        name[n] = 0;
        int e = v->kind == 4 ? ext_directory(v, cur.cluster, name, &cur, NULL, NULL)
                             : ntfs_directory(v, cur.cluster, name, &cur, NULL, NULL);
        if (e)
            return e;
    }
    *out = cur;
    return 0;
}
static int native_refresh(k_file *f)
{
    if (f->object >= native_count)
        return K_EINVAL;
    native_volume *v = &native_volumes[f->object];
    UINT8 raw[4096];
    if (v->kind == 4) {
        int e = ext_inode(v, f->cluster, raw);
        if (e)
            return e;
        if (rd32(raw + 100) != f->directory_offset)
            return K_ENOENT;
        f->size = ext_size(raw);
    } else {
        int e = ntfs_record(v, f->cluster, raw);
        if (e)
            return e;
        if (rd16(raw + 16) != f->directory_offset)
            return K_ENOENT;
        UINT64 size;
        e = ntfs_data(v, f->cluster, 0, NULL, 0, &size, FALSE);
        if (e)
            return e;
        f->size = size;
    }
    return 0;
}
int k_vfs_stat(k_file *f)
{
    if (!f)
        return K_EINVAL;
    int e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    if (f->kind == 1) {
        if (f->object >= MAX_NODES || !nodes[f->object].used)
            e = K_ENOENT;
        else
            f->size = nodes[f->object].size;
    } else if (f->kind == 2)
        e = fat_refresh(f);
    else if (f->kind == 4 || f->kind == 5)
        e = native_refresh(f);
    else if (f->kind != 3 || f->object >= k_disk_count())
        e = K_EINVAL;
    k_mutex_unlock(&vfs_mutex);
    return e;
}
static int native_read(const k_file *f, UINT64 offset, void *out, UINT64 n, UINT64 *got)
{
    if (f->object >= native_count || f->attributes & 0x10)
        return K_EINVAL;
    native_volume *v = &native_volumes[f->object];
    *got = 0;
    if (v->kind == 4) {
        UINT8 raw[512];
        int e = ext_inode(v, f->cluster, raw);
        if (e)
            return e;
        if (rd32(raw + 100) != f->directory_offset)
            return K_ENOENT;
        UINT64 size = ext_size(raw);
        if (offset >= size)
            return 0;
        n = MIN(n, size - offset);
        native_run runs[NATIVE_RUNS];
        UINT32 count;
        e = ext_map(v, f->cluster, raw, runs, &count);
        if (!e)
            e = run_io(v, runs, count, offset, out, n, FALSE);
        if (!e)
            *got = n;
        return e;
    }
    k_file refreshed = *f;
    int e = native_refresh(&refreshed);
    if (e)
        return e;
    UINT64 size;
    e = ntfs_data(v, f->cluster, offset, out, n, &size, FALSE);
    if (!e && offset < size)
        *got = MIN(n, size - offset);
    return e;
}
static int native_list(const k_file *f, k_dir_callback cb, void *arg)
{
    if (f->object >= native_count)
        return K_EINVAL;
    native_volume *v = &native_volumes[f->object];
    return v->kind == 4 ? ext_directory(v, f->cluster, NULL, NULL, cb, arg)
                        : ntfs_directory(v, f->cluster, NULL, NULL, cb, arg);
}
static int native_clean(native_volume *v)
{
    if (v->read_only || v->failed)
        return K_EROFS;
    if (v->kind == 4) {
        UINT8 super[1024];
        int e = native_bytes(v, 1024, super, 1024);
        if (e)
            return e;
        if (rd16(super + 56) != 0xef53 || rd16(super + 58) != 1 || rd32(super + 96) & 4 ||
            rd32(super + 232) || memcmp(super + 104, v->super + 104, 16))
            return K_EROFS;
        if (v->csum && crc32c_step(0xffffffff, super, 1020) != rd32(super + 1020))
            return K_EIO;
    } else {
        UINT8 record[4096], *a, *value;
        UINT32 len, n;
        int e = ntfs_record(v, 3, record);
        if (e)
            return e;
        e = ntfs_attr(v, record, 0x70, "", &a, &len);
        if (e)
            return e;
        e = ntfs_value(a, len, &value, &n);
        if (e)
            return e;
        if (n < 12 || rd16(value + 10))
            return K_EROFS;
        k_file hiber;
        e = ntfs_directory(v, 5, "hiberfil.sys", &hiber, NULL, NULL);
        if (!e) {
            UINT8 header[4096];
            UINT64 got;
            e = native_read(&hiber, 0, header, sizeof(header), &got);
            if (e)
                return K_EROFS;
            /* Never clear an active or unrecognized hibernation header. */
            for (UINT64 i = 0; i < got; ++i)
                if (header[i])
                    return K_EROFS;
        } else if (e != K_ENOENT)
            return K_EROFS;
    }
    return disk_flush(v->disk);
}
static int native_write(k_file *f, UINT64 offset, const void *data, UINT64 n, UINT64 *written)
{
    *written = 0;
    if (f->object >= native_count || f->attributes & 0x10)
        return K_EINVAL;
    native_volume *v = &native_volumes[f->object];
    int refresh = native_refresh(f);
    if (refresh)
        return refresh;
    if (offset == ~0ULL)
        offset = f->size;
    if ((v->kind == 4 || v->kind == 5) && f->size <= 4 * 1024 * 1024 && offset <= 4 * 1024 * 1024 &&
        n <= 4 * 1024 * 1024 - offset) {
        if (!n)
            return 0;
        UINT64 size = (f->size > offset + n ? f->size : offset + n);
        UINT32 pages = (UINT32)((size + 4095) / 4096);
        UINT64 memory = k_pmm_alloc_pages(pages, 0);
        if (!memory)
            return K_ENOMEM;
        UINT64 got = 0;
        int result = native_read(f, 0, (void *)(UINTN)memory, f->size, &got);
        if (!result && got != f->size)
            result = K_EIO;
        if (!result) {
            mem_copy((UINT8 *)(UINTN)memory + offset, data, n);
            result = v->kind == 4 ? ext_replace(f, (void *)(UINTN)memory, size)
                                  : ntfs_replace(f, (void *)(UINTN)memory, size);
        }
        k_pmm_free_pages(memory, pages);
        if (!result)
            *written = n;
        return result;
    }
    if (offset > f->size || n > f->size - offset)
        return K_ENOTSUP;
    if (!n)
        return 0;
    int e = native_clean(v);
    if (e)
        return e;
    if (v->kind == 5) {
        UINT64 size;
        e = ntfs_data(v, f->cluster, offset, (void *)data, n, &size, TRUE);
        if (!e)
            *written = n;
        return e;
    }
    UINT8 raw[512];
    e = ext_inode(v, f->cluster, raw);
    if (e)
        return e;
    if ((rd16(raw) & 0xf000) != 0x8000 || f->cluster < rd32(v->super + 84) || rd16(raw + 26) != 1 ||
        rd32(raw + 32) & (0x10 | 0x20 | 0x4000 | 0x100000 | 0x2000000))
        return K_ENOTSUP;
    if (rd32(raw + 100) != f->directory_offset || ext_size(raw) != f->size)
        return K_ENOENT;
    native_run runs[NATIVE_RUNS];
    UINT32 count;
    e = ext_map(v, f->cluster, raw, runs, &count);
    if (e)
        return e;
    UINT64 first = offset / v->block, last = (offset + n - 1) / v->block;
    for (UINT64 logical = first; logical <= last; ++logical) {
        BOOLEAN found = FALSE;
        for (UINT32 i = 0; i < count; ++i)
            if (logical >= runs[i].logical && logical - runs[i].logical < runs[i].length &&
                !runs[i].hole)
                found = TRUE;
        if (!found)
            return K_ENOTSUP;
    }
    /* ext4 data extents must avoid every group's static metadata and have allocation bits set. */
    for (UINT32 g = 0; g < v->groups; ++g) {
        UINT8 d[64];
        e = ext_group(v, g, d);
        if (e)
            return e;
        UINT64 bb = ext_desc_block(v, d, 0), ib = ext_desc_block(v, d, 4),
               it = ext_desc_block(v, d, 8);
        UINT64 itn = ((UINT64)v->inodes_per_group * v->inode_size + v->block - 1) / v->block;
        for (UINT32 i = 0; i < count; ++i)
            if (!runs[i].hole && (overlap(runs[i].physical, runs[i].length, bb, 1) ||
                                  overlap(runs[i].physical, runs[i].length, ib, 1) ||
                                  overlap(runs[i].physical, runs[i].length, it, itn)))
                return K_EIO;
    }
    UINT8 bitmap[4096];
    UINT32 oldgroup = ~0u;
    for (UINT32 i = 0; i < count; ++i) {
        if (runs[i].hole)
            return K_ENOTSUP;
        for (UINT64 p = runs[i].physical; p < runs[i].physical + runs[i].length; ++p) {
            UINT64 first_data = rd32(v->super + 20);
            if (p < first_data)
                return K_EIO;
            UINT32 g = (UINT32)((p - first_data) / v->blocks_per_group),
                   bit = (UINT32)((p - first_data) % v->blocks_per_group);
            if (g != oldgroup) {
                UINT8 d[64];
                e = ext_group(v, g, d);
                if (e)
                    return e;
                if (rd16(d + 18) & 2)
                    return K_EIO;
                UINT64 bb = ext_desc_block(v, d, 0);
                e = native_bytes(v, bb * v->block, bitmap, v->block);
                if (e)
                    return e;
                if (v->csum) {
                    UINT32 wanted = rd16(d + 24);
                    if (v->desc_size == 64)
                        wanted |= (UINT32)rd16(d + 56) << 16;
                    UINT32 crc = crc32c_step(v->csum_seed, bitmap, v->blocks_per_group / 8);
                    if ((v->desc_size == 64 ? crc : (UINT16)crc) != wanted)
                        return K_EIO;
                }
                oldgroup = g;
            }
            if (!(bitmap[bit / 8] & (1u << (bit % 8))))
                return K_EIO;
        }
    }
    e = run_io(v, runs, count, offset, (void *)data, n, TRUE);
    if (!e)
        *written = n;
    return e;
}
static int native_probe(UINT32 disk_id, UINT64 start, UINT64 length, UINT32 partition, BOOLEAN ro)
{
    if (native_count == MAX_VOLUMES || mount_count == MAX_VOLUMES)
        return K_E2BIG;
    native_volume *v = &native_volumes[native_count];
    mem_zero(v, sizeof(*v));
    v->disk = disk_id;
    v->sector = disks[disk_id].info.sector_size;
    v->start = start;
    v->length = length;
    v->read_only = ro;
    if (!length || start >= disks[disk_id].info.sectors ||
        length > disks[disk_id].info.sectors - start)
        return K_EINVAL;
    UINT8 boot[4096];
    int e = native_bytes(v, 0, boot, 512);
    if (e)
        return e;
    if (!memcmp(boot + 3, "NTFS    ", 8)) {
        v->kind = 5;
        mem_copy(v->super, boot, 512);
        UINT32 bps = rd16(boot + 11), spc = boot[13];
        UINT64 sectors = rd64(boot + 40), mft = rd64(boot + 48);
        if (bps != v->sector || !power2(spc) || spc > 128 || !sectors || sectors > length ||
            boot[510] != 0x55 || boot[511] != 0xaa)
            return K_ENOTSUP;
        v->block = bps * spc;
        v->blocks = sectors / spc;
        INT8 rs = (INT8)boot[64], is = (INT8)boot[68];
        if (!rs || !is || rs < -12 || is < -12)
            return K_ENOTSUP;
        v->record_size = rs < 0 ? (1u << -rs) : (UINT32)rs * v->block;
        v->index_size = is < 0 ? (1u << -is) : (UINT32)is * v->block;
        if (!power2(v->record_size) || v->record_size < 512 || v->record_size > 4096 ||
            !power2(v->index_size) || v->index_size < 512 || v->index_size > 4096 || !mft ||
            mft >= v->blocks)
            return K_ENOTSUP;
        e = native_bytes(v, mft * v->block, boot, v->record_size);
        if (e)
            return e;
        e = ntfs_fixup(v, boot, v->record_size, "FILE");
        if (e)
            return e;
        if (rd32(boot + 24) > v->record_size || rd16(boot + 20) < 48 ||
            rd16(boot + 20) > rd32(boot + 24))
            return K_EIO;
        UINT8 *a;
        UINT32 len;
        e = ntfs_attr(v, boot, 0x80, "", &a, &len);
        if (e)
            return e;
        e = ntfs_runs(v, a, len, v->mft, &v->mft_runs);
        if (e)
            return e;
        v->mft_size = rd64(a + 56);
        if (v->mft_size < v->record_size * 24ULL)
            return K_ENOTSUP;
        if (v->mft_size / v->record_size > 0xffffffff)
            return K_ENOTSUP;
        v->mft_count = (UINT32)(v->mft_size / v->record_size);
    } else {
        e = native_bytes(v, 1024, v->super, 1024);
        if (e)
            return e;
        UINT8 *s = v->super;
        if (rd16(s + 56) != 0xef53)
            return K_ENOTSUP;
        v->kind = 4;
        UINT32 log = rd32(s + 24), incompat = rd32(s + 96), roc = rd32(s + 100);
        if (log > 2 || rd32(s + 76) > 1 ||
            (incompat & ~(2u | 0x40u | 0x80u | 0x200u | 0x2000u | 4u)) || rd32(s + 28) != log)
            return K_ENOTSUP;
        v->block = 1024u << log;
        v->blocks = rd32(s + 4) | ((incompat & 0x80) ? (UINT64)rd32(s + 336) << 32 : 0);
        v->inode_size = rd32(s + 76) ? rd16(s + 88) : 128;
        v->inodes = rd32(s);
        v->blocks_per_group = rd32(s + 32);
        v->inodes_per_group = rd32(s + 40);
        v->desc_size = (incompat & 0x80) ? rd16(s + 254) : 32;
        v->csum = (roc & 0x400) != 0;
        v->gdt_csum = (roc & 0x10) != 0;
        if (!v->blocks || v->blocks > length * v->sector / v->block || !v->inodes ||
            !v->blocks_per_group || v->blocks_per_group > 8 * v->block || v->blocks_per_group % 8 ||
            !v->inodes_per_group || v->inodes_per_group > 8 * v->block || v->inode_size < 128 ||
            v->inode_size > 512 || !power2(v->inode_size) ||
            (v->desc_size != 32 && v->desc_size != 64) || rd32(s + 20) != (log ? 0u : 1u))
            return K_ENOTSUP;
        UINT64 groups = (v->blocks - rd32(s + 20) + v->blocks_per_group - 1) / v->blocks_per_group;
        if (!groups || groups > 65536 || (UINT64)v->inodes > groups * v->inodes_per_group)
            return K_ENOTSUP;
        v->groups = (UINT32)groups;
        v->csum_seed = (incompat & 0x2000) ? rd32(s + 624) : crc32c_step(0xffffffff, s + 104, 16);
        if (v->csum && (s[373] != 1 || crc32c_step(0xffffffff, s, 1020) != rd32(s + 1020)))
            return K_EIO;
        if ((roc & ~(1u | 2u | 8u | 0x10u | 0x20u | 0x40u | 0x400u)) || rd16(s + 58) != 1 ||
            (incompat & 4) || rd32(s + 232))
            v->read_only = TRUE;
    }
    char source[24], num[12], path[K_PATH_MAX];
    decimal_name(source, "disk", disk_id);
    UINTN pos = str_len(source);
    source[pos++] = 'p';
    decimal_name(num, "", partition);
    str_copy(source + pos, num, sizeof(source) - pos);
    str_copy(path, "/volumes/", sizeof(path));
    str_copy(path + 9, source, sizeof(path) - 9);
    str_copy(v->source, source, sizeof(v->source));
    e = ram_new(path, TRUE);
    if (e >= 0)
        e = 0;
    if (e && e != K_EEXIST)
        return e;
    str_copy(mounts[mount_count].path, path, K_PATH_MAX);
    mounts[mount_count].kind = v->kind;
    mounts[mount_count].volume = native_count++;
    __atomic_add_fetch(&mount_count, 1, __ATOMIC_RELEASE);
    (void)k_root_bind(source, path);
    decimal_name(num, "disk", disk_id);
    (void)k_root_bind(num, path);
    return 0;
}

/* ext4 writes require clean supported metadata, initialized bitmaps and a quiescent journal.
 * Data is staged before metadata; I/O failure freezes writes. */
#define EXT_TX_BLOCKS 128
static struct {
    UINT64 number;
    UINT8 bytes[4096];
} ext_tx_blocks[EXT_TX_BLOCKS];
static UINT32 ext_tx_count;
static native_volume *ext_tx_volume;
static UINT8 *ext_tx_get(UINT64 block, int *error)
{
    native_volume *v = ext_tx_volume;
    if (*error)
        return NULL;
    if (block >= v->blocks) {
        *error = K_EIO;
        return NULL;
    }
    for (UINT32 i = 0; i < ext_tx_count; ++i)
        if (ext_tx_blocks[i].number == block)
            return ext_tx_blocks[i].bytes;
    if (ext_tx_count == EXT_TX_BLOCKS) {
        *error = K_E2BIG;
        return NULL;
    }
    UINT8 *p = ext_tx_blocks[ext_tx_count].bytes;
    *error = native_bytes(v, block * v->block, p, v->block);
    if (*error)
        return NULL;
    ext_tx_blocks[ext_tx_count++].number = block;
    return p;
}
static UINT8 *ext_tx_desc(UINT32 group, int *e)
{
    native_volume *v = ext_tx_volume;
    UINT8 checked[64];
    if (!*e)
        *e = ext_group(v, group, checked);
    if (*e)
        return NULL;
    UINT64 at = (v->block == 1024 ? 2ULL : 1ULL) * v->block + (UINT64)group * v->desc_size;
    UINT8 *b = ext_tx_get(at / v->block, e);
    return b ? b + at % v->block : NULL;
}
static UINT8 *ext_tx_super(int *e)
{
    native_volume *v = ext_tx_volume;
    UINT8 *b = ext_tx_get(1024 / v->block, e);
    return b ? b + 1024 % v->block : NULL;
}
static void ext_desc_checksum(native_volume *v, UINT32 group, UINT8 *d)
{
    UINT8 g[4];
    wr32(g, group);
    wr16(d + 30, 0);
    if (v->csum) {
        UINT32 c = crc32c_step(v->csum_seed, g, 4);
        wr16(d + 30, (UINT16)crc32c_step(c, d, v->desc_size));
    } else if (v->gdt_csum) {
        UINT16 c = crc16_step(0xffff, v->super + 104, 16);
        c = crc16_step(c, g, 4);
        c = crc16_step(c, d, 30);
        if (v->desc_size > 32)
            c = crc16_step(c, d + 32, v->desc_size - 32);
        wr16(d + 30, c);
    }
}
static UINT8 *ext_tx_bitmap(UINT32 group, BOOLEAN inode, UINT8 **desc, int *e)
{
    native_volume *v = ext_tx_volume;
    UINT8 *d = ext_tx_desc(group, e);
    if (!d)
        return NULL;
    if (rd16(d + 18) & (inode ? 1 : 2)) {
        *e = K_ENOTSUP;
        return NULL;
    }
    UINT64 block = ext_desc_block(v, d, inode ? 4 : 0);
    UINT8 *b = ext_tx_get(block, e);
    if (!b)
        return NULL;
    UINT32 bytes = (inode ? v->inodes_per_group : v->blocks_per_group) / 8;
    if (v->csum) {
        UINT32 wanted = rd16(d + (inode ? 26 : 24));
        if (v->desc_size == 64)
            wanted |= (UINT32)rd16(d + (inode ? 58 : 56)) << 16;
        UINT32 crc = crc32c_step(v->csum_seed, b, bytes);
        if ((v->desc_size == 64 ? crc : (UINT16)crc) != wanted) {
            *e = K_EIO;
            return NULL;
        }
    }
    *desc = d;
    return b;
}
static int ext_tx_bit(UINT64 number, BOOLEAN inode, BOOLEAN allocate)
{
    native_volume *v = ext_tx_volume;
    UINT64 first = inode ? 1 : rd32(v->super + 20);
    UINT32 per = inode ? v->inodes_per_group : v->blocks_per_group;
    if (number < first)
        return K_EIO;
    UINT32 group = (UINT32)((number - first) / per), bit = (UINT32)((number - first) % per);
    int e = 0;
    UINT8 *d = NULL, *b = ext_tx_bitmap(group, inode, &d, &e);
    if (e)
        return e;
    BOOLEAN old = (b[bit / 8] & (1u << (bit % 8))) != 0;
    if (old == allocate)
        return K_EIO;
    UINT32 low = inode ? 14 : 12, high = inode ? 46 : 44;
    UINT32 free = rd16(d + low) | (v->desc_size == 64 ? (UINT32)rd16(d + high) << 16 : 0);
    if ((allocate && !free) || (!allocate && free >= per))
        return K_EIO;
    if (allocate)
        b[bit / 8] |= (UINT8)(1u << (bit % 8));
    else
        b[bit / 8] &= (UINT8) ~(1u << (bit % 8));
    free = allocate ? free - 1 : free + 1;
    wr16(d + low, (UINT16)free);
    if (v->desc_size == 64)
        wr16(d + high, (UINT16)(free >> 16));
    if (v->csum) {
        UINT32 crc = crc32c_step(v->csum_seed, b, per / 8);
        wr16(d + (inode ? 26 : 24), (UINT16)crc);
        if (v->desc_size == 64)
            wr16(d + (inode ? 58 : 56), (UINT16)(crc >> 16));
    }
    if (inode && allocate) {
        UINT32 unused = rd16(d + 28) | (v->desc_size == 64 ? (UINT32)rd16(d + 50) << 16 : 0);
        UINT32 now = per - bit - 1;
        if (unused > now) {
            wr16(d + 28, (UINT16)now);
            if (v->desc_size == 64)
                wr16(d + 50, (UINT16)(now >> 16));
        }
    }
    ext_desc_checksum(v, group, d);
    UINT8 *super = ext_tx_super(&e);
    if (e)
        return e;
    if (inode) {
        UINT32 n = rd32(super + 16);
        if ((allocate && !n) || (!allocate && n == v->inodes))
            return K_EIO;
        wr32(super + 16, allocate ? n - 1 : n + 1);
    } else {
        UINT64 n =
            rd32(super + 12) | ((rd32(super + 96) & 0x80) ? (UINT64)rd32(super + 344) << 32 : 0);
        if ((allocate && !n) || (!allocate && n == v->blocks))
            return K_EIO;
        n = allocate ? n - 1 : n + 1;
        wr32(super + 12, (UINT32)n);
        if (rd32(super + 96) & 0x80)
            wr32(super + 344, (UINT32)(n >> 32));
    }
    return 0;
}
static int ext_tx_allocate(BOOLEAN inode, UINT64 *number)
{
    native_volume *v = ext_tx_volume;
    UINT32 per = inode ? v->inodes_per_group : v->blocks_per_group;
    for (UINT32 g = 0; g < v->groups; ++g) {
        int e = 0;
        UINT8 *d = ext_tx_desc(g, &e);
        if (e)
            return e;
        if (rd16(d + 18) & (inode ? 1 : 2))
            continue;
        UINT32 free = rd16(d + (inode ? 14 : 12)) |
                      (v->desc_size == 64 ? (UINT32)rd16(d + (inode ? 46 : 44)) << 16 : 0);
        if (!free)
            continue;
        UINT8 *b = ext_tx_bitmap(g, inode, &d, &e);
        if (e)
            return e;
        for (UINT32 bit = 0; bit < per; ++bit) {
            UINT64 n = (UINT64)g * per + bit + (inode ? 1 : rd32(v->super + 20));
            if (inode ? (n > v->inodes) : (n >= v->blocks))
                break;
            if (inode && n < rd32(v->super + 84))
                continue;
            if (!(b[bit / 8] & (1u << (bit % 8)))) {
                e = ext_tx_bit(n, inode, TRUE);
                if (e)
                    return e;
                *number = n;
                return 0;
            }
        }
    }
    return K_ENOSPC;
}
static void ext_set_size(UINT8 *raw, UINT64 size)
{
    wr32(raw + 4, (UINT32)size);
    wr32(raw + 108, (UINT32)(size >> 32));
}
static void ext_inode_checksum(native_volume *v, UINT32 ino, UINT8 *raw)
{
    if (!v->csum)
        return;
    wr16(raw + 124, 0);
    BOOLEAN high = v->inode_size >= 132 && rd16(raw + 128) >= 4;
    if (high)
        wr16(raw + 130, 0);
    UINT32 crc = crc32c_step(ext_inode_seed(v, ino, raw), raw, v->inode_size);
    wr16(raw + 124, (UINT16)crc);
    if (high)
        wr16(raw + 130, (UINT16)(crc >> 16));
}
static int ext_tx_inode(UINT32 ino, UINT8 *raw)
{
    native_volume *v = ext_tx_volume;
    int e = 0;
    UINT8 *d = ext_tx_desc((ino - 1) / v->inodes_per_group, &e);
    if (e)
        return e;
    UINT64 pos = ext_desc_block(v, d, 8) * v->block +
                 (UINT64)((ino - 1) % v->inodes_per_group) * v->inode_size;
    UINT8 *b = ext_tx_get(pos / v->block, &e);
    if (e)
        return e;
    if (v->inode_size > v->block - pos % v->block)
        return K_EIO;
    ext_inode_checksum(v, ino, raw);
    mem_copy(b + pos % v->block, raw, v->inode_size);
    return 0;
}
static void ext_inline_map(UINT8 *raw, const native_run *runs, UINT32 count, UINT32 blocksize)
{
    mem_zero(raw + 40, 60);
    wr16(raw + 40, 0xf30a);
    wr16(raw + 42, (UINT16)count);
    wr16(raw + 44, 4);
    UINT64 blocks = 0;
    for (UINT32 i = 0; i < count; ++i) {
        UINT8 *r = raw + 52 + 12 * i;
        wr32(r, (UINT32)runs[i].logical);
        wr16(r + 4, (UINT16)runs[i].length);
        wr16(r + 6, (UINT16)(runs[i].physical >> 32));
        wr32(r + 8, (UINT32)runs[i].physical);
        blocks += runs[i].length;
    }
    wr32(raw + 32, rd32(raw + 32) | 0x80000);
    wr32(raw + 28, (UINT32)(blocks * (blocksize / 512)));
    wr16(raw + 116, 0);
}
static int ext_alloc_runs(UINT64 size, native_run runs[4], UINT32 *count)
{
    native_volume *v = ext_tx_volume;
    *count = 0;
    UINT64 blocks = (size + v->block - 1) / v->block;
    for (UINT64 logical = 0; logical < blocks; ++logical) {
        UINT64 physical;
        int e = ext_tx_allocate(FALSE, &physical);
        if (e)
            return e;
        if (*count && runs[*count - 1].physical + runs[*count - 1].length == physical &&
            runs[*count - 1].length < 32768)
            ++runs[*count - 1].length;
        else {
            if (*count == 4)
                return K_ENOSPC;
            runs[(*count)++] = (native_run){logical, physical, 1, FALSE};
        }
    }
    return 0;
}
static BOOLEAN ext_has_super(native_volume *v, UINT32 group)
{
    UINT32 compat = rd32(v->super + 92), roc = rd32(v->super + 100);
    if (!group)
        return TRUE;
    if (compat & 0x200)
        return group == rd32(v->super + 588) || group == rd32(v->super + 592);
    if (!(roc & 1) || group == 1)
        return TRUE;
    const UINT32 bases[3] = {3, 5, 7};
    for (UINT32 i = 0; i < 3; ++i) {
        UINT32 n = group;
        while (n > 1 && n % bases[i] == 0)
            n /= bases[i];
        if (n == 1)
            return TRUE;
    }
    return FALSE;
}
static int ext_data_boundaries(native_volume *v, const native_run *runs, UINT32 count)
{

    for (UINT32 g = 0; g < v->groups; ++g) {
        UINT8 d[64];
        int e = ext_group(v, g, d);
        if (e)
            return e;
        UINT64 starts[4] = {ext_desc_block(v, d, 0), ext_desc_block(v, d, 4),
                            ext_desc_block(v, d, 8),
                            rd32(v->super + 20) + (UINT64)g * v->blocks_per_group};
        UINT64 lengths[4] = {
            1, 1, ((UINT64)v->inodes_per_group * v->inode_size + v->block - 1) / v->block,
            ext_has_super(v, g) ? 1 + ((UINT64)v->groups * v->desc_size + v->block - 1) / v->block +
                                      rd16(v->super + 206)
                                : 0};
        for (UINT32 j = 0; j < 4; ++j) {
            if (lengths[j] && (!span_ok(starts[j], lengths[j], v->blocks)))
                return K_EIO;
            for (UINT32 i = 0; i < count; ++i)
                if (!runs[i].hole && lengths[j] &&
                    overlap(runs[i].physical, runs[i].length, starts[j], lengths[j]))
                    return K_EIO;
        }
    }
    if (rd32(v->super + 92) & 4) {
        UINT8 raw[512];
        UINT32 ino = rd32(v->super + 224);
        int e = ext_inode(v, ino, raw);
        if (e)
            return e;
        native_run jr[NATIVE_RUNS];
        UINT32 jc;
        e = ext_map(v, ino, raw, jr, &jc);
        if (e)
            return e;
        for (UINT32 i = 0; i < count; ++i)
            for (UINT32 j = 0; j < jc; ++j)
                if (!runs[i].hole && !jr[j].hole &&
                    overlap(runs[i].physical, runs[i].length, jr[j].physical, jr[j].length))
                    return K_EIO;
    }
    return 0;
}
static int ext_prepare_metadata(native_volume *v)
{
    int e = native_clean(v);
    if (e)
        return e;
    UINT32 compat = rd32(v->super + 92), roc = rd32(v->super + 100);
    if (compat & ~(4u | 8u | 0x10u | 0x20u | 0x200u) ||
        roc & ~(1u | 2u | 8u | 0x10u | 0x20u | 0x40u | 0x400u) || rd32(v->super + 228) ||
        !(rd32(v->super + 96) & 2))
        return K_ENOTSUP;
    if (compat & 4) {
        UINT32 ino = rd32(v->super + 224);
        UINT8 raw[512], journal[32];
        e = ext_inode(v, ino, raw);
        if (e)
            return e;
        native_run runs[NATIVE_RUNS];
        UINT32 count;
        e = ext_map(v, ino, raw, runs, &count);
        if (e)
            return e;
        e = run_io(v, runs, count, 0, journal, 32, FALSE);
        if (e)
            return e;
        /* JBD2 superblock is big-endian; s_start=0 means no live transaction. */
        if (journal[0] != 0xc0 || journal[1] != 0x3b || journal[2] != 0x39 || journal[3] != 0x98 ||
            journal[7] != 4 || rd32(journal + 28))
            return K_EROFS;
    }
    ext_tx_volume = v;
    ext_tx_count = 0;
    return 0;
}
static int ext_commit(native_volume *v, const native_run *fresh, UINT32 count, const void *data,
                      UINT64 size)
{
    int e = 0;
    UINT8 *s = ext_tx_super(&e);
    if (e)
        return e;
    UINT8 dirty[1024];
    mem_copy(dirty, v->super, 1024);
    wr16(dirty + 58, 0);
    if (v->csum)
        wr32(dirty + 1020, crc32c_step(0xffffffff, dirty, 1020));
    e = native_write_bytes(v, 1024, dirty, 1024);
    if (e)
        return e;
    UINT8 block[4096];
    UINT64 offset = 0;
    for (UINT32 i = 0; !e && i < count; ++i)
        for (UINT64 j = 0; !e && j < fresh[i].length; ++j) {
            mem_zero(block, v->block);
            UINT64 n = offset < size ? MIN(size - offset, v->block) : 0;
            if (n)
                mem_copy(block, (const UINT8 *)data + offset, n);
            e = native_write_bytes(v, (fresh[i].physical + j) * v->block, block, v->block);
            offset += n;
        }
    /* Keep the primary ext4 superblock dirty until staged metadata is durable. */
    wr16(s + 58, 0);
    if (v->csum)
        wr32(s + 1020, crc32c_step(0xffffffff, s, 1020));
    for (UINT32 i = 0; !e && i < ext_tx_count; ++i)
        e = native_write_bytes(v, ext_tx_blocks[i].number * v->block, ext_tx_blocks[i].bytes,
                               v->block);
    if (e) {
        v->failed = TRUE;
        return e;
    }
    wr16(s + 58, 1);
    if (v->csum)
        wr32(s + 1020, crc32c_step(0xffffffff, s, 1020));
    e = native_write_bytes(v, 1024, s, 1024);
    if (!e)
        mem_copy(v->super, s, 1024);
    else
        v->failed = TRUE;
    return e;
}
static int ext_replace(k_file *f, const void *bytes, UINT64 size)
{
    native_volume *v = &native_volumes[f->object];
    if (size > 4 * 1024 * 1024)
        return K_E2BIG;
    UINT8 raw[512];
    int e = ext_inode(v, f->cluster, raw);
    if (e)
        return e;
    if ((rd16(raw) & 0xf000) != 0x8000 || f->cluster < rd32(v->super + 84) || rd16(raw + 26) != 1 ||
        rd32(raw + 100) != f->directory_offset || rd32(raw + 32) & ~0x80000u ||
        !(rd32(raw + 32) & 0x80000) || rd16(raw + 46) || rd32(raw + 104) || rd16(raw + 118))
        return K_ENOTSUP;
    native_run old[NATIVE_RUNS];
    UINT32 oldn;
    e = ext_map(v, f->cluster, raw, old, &oldn);
    if (e)
        return e;
    e = ext_data_boundaries(v, old, oldn);
    if (e)
        return e;
    e = ext_prepare_metadata(v);
    if (e)
        return e;
    native_run fresh[4];
    UINT32 count;
    e = ext_alloc_runs(size, fresh, &count);
    if (e)
        return e;
    e = ext_data_boundaries(v, fresh, count);
    if (e)
        return e;
    for (UINT32 i = 0; i < oldn; ++i)
        for (UINT32 j = 0; j < count; ++j)
            if (overlap(old[i].physical, old[i].length, fresh[j].physical, fresh[j].length))
                return K_EIO;
    for (UINT32 i = 0; i < oldn; ++i)
        for (UINT64 j = 0; j < old[i].length; ++j) {
            e = ext_tx_bit(old[i].physical + j, FALSE, FALSE);
            if (e)
                return e;
        }
    ext_inline_map(raw, fresh, count, v->block);
    ext_set_size(raw, size);
    e = ext_tx_inode(f->cluster, raw);
    if (e)
        return e;
    e = ext_commit(v, fresh, count, bytes, size);
    if (!e)
        f->size = size;
    return e;
}
static int ext_add_dirent(native_volume *v, UINT32 parent, UINT8 *raw, const char *name,
                          UINT32 inode, BOOLEAN directory)
{
    UINT32 namelen = (UINT32)str_len(name), needed = (8 + namelen + 3) & ~3u;
    if (!namelen || namelen > 255 || rd32(raw + 32) & ~0x80000u || !(rd32(raw + 32) & 0x80000) ||
        rd16(raw + 46))
        return K_ENOTSUP;
    native_run runs[NATIVE_RUNS];
    UINT32 count;
    int e = ext_map(v, parent, raw, runs, &count);
    if (e)
        return e;
    UINT64 size = ext_size(raw);
    if (size > NATIVE_SCAN_MAX || size % v->block)
        return K_ENOTSUP;
    for (UINT32 i = 0; i < count; ++i)
        for (UINT64 j = 0; j < runs[i].length; ++j) {
            if (runs[i].hole)
                return K_EIO;
            UINT8 *b = ext_tx_get(runs[i].physical + j, &e);
            if (e)
                return e;
            UINT32 limit = v->block - (v->csum ? 12 : 0), at = 0;
            if (v->csum &&
                (rd32(b + limit) || rd16(b + limit + 4) != 12 || b[limit + 7] != 0xde ||
                 crc32c_step(ext_inode_seed(v, parent, raw), b, limit) != rd32(b + limit + 8)))
                return K_EIO;
            while (at < limit) {
                if (limit - at < 8)
                    return K_EIO;
                UINT32 rec = rd16(b + at + 4), len = b[at + 6];
                if (rec < 8 || (rec & 3) || rec > limit - at || len > rec - 8)
                    return K_EIO;
                UINT32 used = rd32(b + at) ? (8 + len + 3) & ~3u : 0;
                if (rec - used >= needed) {
                    UINT32 target = at + used;
                    if (used)
                        wr16(b + at + 4, (UINT16)used);
                    mem_zero(b + target, rec - used);
                    wr32(b + target, inode);
                    wr16(b + target + 4, (UINT16)(rec - used));
                    b[target + 6] = (UINT8)namelen;
                    b[target + 7] = directory ? 2 : 1;
                    mem_copy(b + target + 8, name, namelen);
                    if (v->csum)
                        wr32(b + limit + 8, crc32c_step(ext_inode_seed(v, parent, raw), b, limit));
                    return 0;
                }
                at += rec;
            }
        }
    /* Do not fake HTree splitting or directory growth. */
    return K_ENOSPC;
}
static int ext_create(UINT32 index, const char *path, BOOLEAN directory, const void *data,
                      UINT64 size, k_file *out)
{
    if (index >= native_count || size > 4 * 1024 * 1024)
        return K_EINVAL;
    native_volume *v = &native_volumes[index];
    char parent_path[K_PATH_MAX], name[256];
    str_copy(parent_path, path, sizeof(parent_path));
    UINTN end = str_len(parent_path), split = end;
    while (split && parent_path[split - 1] != '/')
        --split;
    str_copy(name, parent_path + split, sizeof(name));
    parent_path[split ? split - 1 : 0] = 0;
    k_file parent;
    int e = native_open(index, parent_path, &parent);
    if (e)
        return e;
    if (!(parent.attributes & 0x10))
        return K_EINVAL;
    k_file existing;
    e = ext_directory(v, parent.cluster, name, &existing, NULL, NULL);
    if (!e)
        return K_EEXIST;
    if (e != K_ENOENT)
        return e;
    UINT8 praw[512];
    e = ext_inode(v, parent.cluster, praw);
    if (e)
        return e;
    if (rd16(praw + 26) >= 65000)
        return K_E2BIG;
    e = ext_prepare_metadata(v);
    if (e)
        return e;
    UINT64 inode;
    e = ext_tx_allocate(TRUE, &inode);
    if (e)
        return e;
    UINT8 raw[512];
    mem_zero(raw, sizeof(raw));
    wr16(raw, directory ? 0x41ed : 0x81a4);
    wr16(raw + 26, directory ? 2 : 1);
    wr32(raw + 100, (UINT32)(arch_cycle_count() ^ inode));
    if (v->inode_size >= 160)
        wr16(raw + 128, 32);
    native_run fresh[4];
    UINT32 count;
    UINT64 stored = directory ? v->block : size;
    e = ext_alloc_runs(stored, fresh, &count);
    if (e)
        return e;
    e = ext_data_boundaries(v, fresh, count);
    if (e)
        return e;
    native_run parent_runs[NATIVE_RUNS];
    UINT32 pn;
    e = ext_map(v, parent.cluster, praw, parent_runs, &pn);
    if (e)
        return e;
    for (UINT32 i = 0; i < pn; ++i)
        for (UINT32 j = 0; j < count; ++j)
            if (overlap(parent_runs[i].physical, parent_runs[i].length, fresh[j].physical,
                        fresh[j].length))
                return K_EIO;
    ext_inline_map(raw, fresh, count, v->block);
    ext_set_size(raw, stored);
    UINT8 contents[4096];
    const void *initial = data;
    if (directory) {
        mem_zero(contents, v->block);
        UINT32 limit = v->block - (v->csum ? 12 : 0);
        wr32(contents, (UINT32)inode);
        wr16(contents + 4, 12);
        contents[6] = 1;
        contents[7] = 2;
        contents[8] = '.';
        wr32(contents + 12, parent.cluster);
        wr16(contents + 16, (UINT16)(limit - 12));
        contents[18] = 2;
        contents[19] = 2;
        contents[20] = contents[21] = '.';
        if (v->csum) {
            wr16(contents + limit + 4, 12);
            contents[limit + 7] = 0xde;
            wr32(contents + limit + 8,
                 crc32c_step(ext_inode_seed(v, (UINT32)inode, raw), contents, limit));
        }
        initial = contents;
        wr16(praw + 26, rd16(praw + 26) + 1);
        int de = 0;
        UINT32 g = ((UINT32)inode - 1) / v->inodes_per_group;
        UINT8 *d = ext_tx_desc(g, &de);
        if (de)
            return de;
        UINT32 used = rd16(d + 16) | (v->desc_size == 64 ? (UINT32)rd16(d + 48) << 16 : 0);
        if (used >= v->inodes_per_group)
            return K_EIO;
        ++used;
        wr16(d + 16, (UINT16)used);
        if (v->desc_size == 64)
            wr16(d + 48, (UINT16)(used >> 16));
        ext_desc_checksum(v, g, d);
    }
    e = ext_add_dirent(v, parent.cluster, praw, name, (UINT32)inode, directory);
    if (e)
        return e;
    e = ext_tx_inode((UINT32)inode, raw);
    if (e)
        return e;
    e = ext_tx_inode(parent.cluster, praw);
    if (e)
        return e;
    e = ext_commit(v, fresh, count, initial, stored);
    if (!e && out)
        *out = (k_file){4, index, (UINT32)inode, directory ? 0x10 : 0, stored, rd32(raw + 100)};
    return e;
}

/* NTFS writes reuse existing MFT/index capacity; no MFT growth, attribute-list extension or index
 * split. */
#define NT_TX_SECTORS 128
static struct {
    UINT64 lba;
    UINT8 bytes[4096];
} nt_tx[NT_TX_SECTORS];
static UINT32 nt_tx_count;
static native_volume *nt_tx_volume;
static int nt_tx_bytes(UINT64 offset, void *bytes, UINT64 length, BOOLEAN write)
{
    native_volume *v = nt_tx_volume;
    if (!span_ok(offset, length, v->length * v->sector))
        return K_EIO;
    UINT8 *p = bytes;
    while (length) {
        UINT64 lba = offset / v->sector;
        UINT32 at = (UINT32)(offset % v->sector), n = (UINT32)MIN(length, v->sector - at), i;
        for (i = 0; i < nt_tx_count; ++i)
            if (nt_tx[i].lba == lba)
                break;
        if (i == nt_tx_count) {
            if (!write) {
                int e = native_bytes(v, offset, p, n);
                if (e)
                    return e;
                goto next;
            }
            if (nt_tx_count == NT_TX_SECTORS)
                return K_E2BIG;
            int e = native_bytes(v, lba * v->sector, nt_tx[i].bytes, v->sector);
            if (e)
                return e;
            nt_tx[i].lba = lba;
            ++nt_tx_count;
        }
        if (write)
            mem_copy(nt_tx[i].bytes + at, p, n);
        else
            mem_copy(p, nt_tx[i].bytes + at, n);
    next:
        p += n;
        offset += n;
        length -= n;
    }
    return 0;
}
static int nt_tx_stream(const native_run *runs, UINT32 count, UINT64 offset, void *bytes,
                        UINT64 length, BOOLEAN write)
{
    native_volume *v = nt_tx_volume;
    UINT8 *p = bytes;
    while (length) {
        UINT64 logical = offset / v->block, at = offset % v->block, n = MIN(length, v->block - at);
        const native_run *r = NULL;
        for (UINT32 i = 0; i < count; ++i)
            if (logical >= runs[i].logical && logical - runs[i].logical < runs[i].length) {
                r = &runs[i];
                break;
            }
        if (!r || r->hole)
            return K_ENOTSUP;
        int e = nt_tx_bytes((r->physical + logical - r->logical) * v->block + at, p, n, write);
        if (e)
            return e;
        p += n;
        offset += n;
        length -= n;
    }
    return 0;
}
static int nt_tx_record(UINT32 ino, UINT8 *raw)
{
    native_volume *v = nt_tx_volume;
    UINT32 usa = rd16(raw + 4), count = rd16(raw + 6);
    if (usa < 48 || count != v->record_size / 512 + 1 || usa + count * 2 > v->record_size)
        return K_EIO;
    UINT8 packed[4096];
    mem_copy(packed, raw, v->record_size);
    UINT16 sequence = (UINT16)(rd16(raw + usa) + 1);
    if (!sequence || sequence == 0xffff)
        sequence = 1;
    wr16(packed + usa, sequence);
    for (UINT32 i = 1; i < count; ++i) {
        wr16(packed + usa + i * 2, rd16(packed + i * 512 - 2));
        wr16(packed + i * 512 - 2, sequence);
    }
    int e = nt_tx_stream(v->mft, v->mft_runs, (UINT64)ino * v->record_size, packed, v->record_size,
                         TRUE);
    if (!e && ino < 4)
        e = nt_tx_bytes(rd64(v->super + 56) * v->block + (UINT64)ino * v->record_size, packed,
                        v->record_size, TRUE);
    return e;
}
static int nt_attr_replace(native_volume *v, UINT8 *raw, UINT8 *old, UINT32 oldlen,
                           const UINT8 *replacement, UINT32 newlen)
{
    UINT32 at = (UINT32)(old - raw), used = rd32(raw + 24);
    if (at < rd16(raw + 20) || at > used || oldlen > used - at || (newlen & 7) ||
        newlen > v->record_size - (used - oldlen))
        return K_ENOSPC;
    memmove(raw + at + newlen, raw + at + oldlen, used - at - oldlen);
    mem_copy(raw + at, replacement, newlen);
    wr32(raw + 24, used - oldlen + newlen);
    return 0;
}
static UINT32 nt_resident_attr(UINT8 *out, UINT32 type, UINT16 instance, const char *name,
                               const void *value, UINT32 size)
{
    UINT32 chars = (UINT32)str_len(name), off = (24 + chars * 2 + 7) & ~7u,
           len = (off + size + 7) & ~7u;
    mem_zero(out, len);
    wr32(out, type);
    wr32(out + 4, len);
    out[9] = (UINT8)chars;
    wr16(out + 10, chars ? 24 : 0);
    wr16(out + 14, instance);
    wr32(out + 16, size);
    wr16(out + 20, (UINT16)off);
    for (UINT32 i = 0; i < chars; ++i)
        wr16(out + 24 + i * 2, (UINT8)name[i]);
    if (size)
        mem_copy(out + off, value, size);
    return len;
}
static int nt_record_append(native_volume *v, UINT8 *raw, const UINT8 *attribute, UINT32 n)
{
    UINT32 used = rd32(raw + 24);
    if (used < 4 || used > v->record_size || n > v->record_size - used)
        return K_ENOSPC;

    if (used < 8 || rd32(raw + used - 8) != 0xffffffff)
        return K_EIO;
    mem_copy(raw + used - 8, attribute, n);
    mem_zero(raw + used - 8 + n, 8);
    wr32(raw + used - 8 + n, 0xffffffff);
    wr32(raw + 24, used + n);
    return 0;
}
static int nt_bitmap_bit(UINT32 ino, UINT32 type, UINT64 bit, BOOLEAN value, BOOLEAN change)
{
    native_volume *v = nt_tx_volume;
    UINT8 raw[4096], *a, *p;
    UINT32 len, size;
    int e = ntfs_record(v, ino, raw);
    if (e)
        return e;
    e = ntfs_attr(v, raw, type, "", &a, &len);
    if (e)
        return e;
    UINT8 byte;
    if (a[8]) {
        native_run runs[NATIVE_RUNS];
        UINT32 count;
        e = ntfs_runs(v, a, len, runs, &count);
        if (e)
            return e;
        if (bit / 8 >= rd64(a + 48))
            return K_EIO;
        e = nt_tx_stream(runs, count, bit / 8, &byte, 1, FALSE);
        if (e)
            return e;
        if (!change)
            return (byte & (1u << (bit % 8))) ? 1 : 0;
        if (((byte & (1u << (bit % 8))) != 0) == value)
            return K_EIO;
        if (value)
            byte |= (UINT8)(1u << (bit % 8));
        else
            byte &= (UINT8) ~(1u << (bit % 8));
        return nt_tx_stream(runs, count, bit / 8, &byte, 1, TRUE);
    }
    e = ntfs_value(a, len, &p, &size);
    if (e)
        return e;
    if (bit / 8 >= size)
        return K_EIO;
    /* Resident MFT bitmap edits may share sectors with other FILE records; stage from the latest
     * sector image. */
    e = nt_tx_stream(v->mft, v->mft_runs, (UINT64)ino * v->record_size, raw, v->record_size, FALSE);
    if (e)
        return e;
    e = ntfs_fixup(v, raw, v->record_size, "FILE");
    if (e)
        return e;
    e = ntfs_attr(v, raw, type, "", &a, &len);
    if (e)
        return e;
    e = ntfs_value(a, len, &p, &size);
    if (e)
        return e;
    byte = p[bit / 8];
    if (!change)
        return (byte & (1u << (bit % 8))) ? 1 : 0;
    if (((byte & (1u << (bit % 8))) != 0) == value)
        return K_EIO;
    if (value)
        p[bit / 8] |= (UINT8)(1u << (bit % 8));
    else
        p[bit / 8] &= (UINT8) ~(1u << (bit % 8));
    return nt_tx_record(ino, raw);
}
static int nt_alloc_data(UINT64 size, native_run *run)
{
    native_volume *v = nt_tx_volume;
    UINT64 want = (size + v->block - 1) / v->block;
    if (!want) {
        *run = (native_run){0};
        return 0;
    }
    UINT8 bitmap[4096];
    UINT64 total, available = 0, first = 0;
    int e = ntfs_data(v, 6, 0, NULL, 0, &total, FALSE);
    if (e)
        return e;
    if (total > 64ULL * 1024 * 1024 || total < (v->blocks + 7) / 8)
        return K_ENOTSUP;
    for (UINT64 off = 0; off < total; off += sizeof(bitmap)) {
        UINT64 n = MIN(total - off, sizeof(bitmap)), got;
        e = ntfs_data(v, 6, off, bitmap, n, &got, FALSE);
        if (e)
            return e;
        for (UINT64 bit = 0; bit < n * 8; ++bit) {
            UINT64 c = off * 8 + bit;
            if (!c || c >= v->blocks - 1)
                continue;
            if (!(bitmap[bit / 8] & (1u << (bit % 8)))) {
                if (!available)
                    first = c;
                ++available;
                if (available == want) {
                    *run = (native_run){0, first, want, FALSE};
                    for (UINT64 j = 0; j < want; ++j) {
                        e = nt_bitmap_bit(6, 0x80, first + j, TRUE, TRUE);
                        if (e)
                            return e;
                    }
                    return 0;
                }
            } else
                available = 0;
        }
    }
    return K_ENOSPC;
}
static UINT32 nt_data_attribute(UINT8 out[128], UINT16 id, const native_run *run, UINT64 size)
{
    if (!size)
        return nt_resident_attr(out, 0x80, id, "", NULL, 0);
    UINT64 len = run->length, lcn = run->physical;
    UINT32 lb = 1, ob = 1;
    while (lb < 8 && (len >> (lb * 8)))
        ++lb;
    while (ob < 8 && (lcn >> (ob * 8 - 1)))
        ++ob;
    UINT32 bytes = (64 + 1 + lb + ob + 1 + 7) & ~7u;
    mem_zero(out, 128);
    wr32(out, 0x80);
    wr32(out + 4, bytes);
    out[8] = 1;
    wr16(out + 14, id);
    wr64(out + 24, len - 1);
    wr16(out + 32, 64);
    wr64(out + 40, len * nt_tx_volume->block);
    wr64(out + 48, size);
    wr64(out + 56, size);
    out[64] = (UINT8)(lb | (ob << 4));
    for (UINT32 i = 0; i < lb; ++i)
        out[65 + i] = (UINT8)(len >> (8 * i));
    for (UINT32 i = 0; i < ob; ++i)
        out[65 + lb + i] = (UINT8)(lcn >> (8 * i));
    return bytes;
}
static int nt_name_compare(const char *a, const char *b)
{
    while (*a && *b) {
        UINT8 x = (UINT8)*a++, y = (UINT8)*b++;
        if (x >= 'a' && x <= 'z')
            x -= 32;
        if (y >= 'a' && y <= 'z')
            y -= 32;
        if (x != y)
            return x < y ? -1 : 1;
    }
    return *a ? 1 : *b ? -1 : 0;
}
static int nt_index_change(native_volume *v, UINT32 parent, UINT64 reference, const UINT8 *filename,
                           UINT32 keylen, BOOLEAN create)
{
    char name[256];
    int e = ntfs_name(filename, keylen, name);
    if (e)
        return e;
    UINT8 raw[4096], *attr, *value;
    UINT32 len, size;
    e = ntfs_record(v, parent, raw);
    if (e)
        return e;
    if (!(rd16(raw + 22) & 2))
        return K_EINVAL;
    e = ntfs_attr(v, raw, 0x90, "$I30", &attr, &len);
    if (e)
        return e;
    e = ntfs_value(attr, len, &value, &size);
    if (e)
        return e;
    if (size < 32 || rd32(value + 4) != 1)
        return K_ENOTSUP;
    native_run runs[NATIVE_RUNS];
    UINT32 count = 0;
    UINT8 *ia;
    UINT32 ialen;
    int alloc_e = ntfs_attr(v, raw, 0xa0, "$I30", &ia, &ialen);
    if (!alloc_e) {
        e = ntfs_runs(v, ia, ialen, runs, &count);
        if (e)
            return e;
    } else if (alloc_e != K_ENOENT)
        return alloc_e;
    UINT8 block[4096];
    UINT8 *header = value + 16;
    UINT32 cap = size - 16;
    BOOLEAN root = TRUE;
    UINT64 block_offset = 0;
    for (UINT32 depth = 0; depth < 16; ++depth) {
        if (cap < 16)
            return K_EIO;
        UINT32 at = rd32(header), used = rd32(header + 4), allocated = rd32(header + 8);
        if (at < 16 || used < at || used > allocated || allocated > cap)
            return K_EIO;
        BOOLEAN descended = FALSE;
        while (at + 16 <= used) {
            UINT8 *entry = header + at;
            UINT32 n = rd16(entry + 8), k = rd16(entry + 10), flags = rd16(entry + 12);
            if (n < 16 + ((flags & 1) ? 8u : 0u) || (n & 7) || n > used - at ||
                k > n - 16 - ((flags & 1) ? 8u : 0u) || (flags & ~3))
                return K_EIO;
            char existing[256];
            int cmp = -1;
            if (!(flags & 2)) {
                e = ntfs_name(entry + 16, k, existing);
                if (e)
                    return K_ENOTSUP;
                cmp = nt_name_compare(name, existing);
            }
            if (!(flags & 2) && cmp == 0) {
                if (create)
                    return K_EEXIST;
                if (rd64(entry) != reference || k != keylen)
                    return K_EIO;
                mem_copy(entry + 16, filename, keylen);
                goto store_index;
            }
            if ((flags & 2) || cmp < 0) {
                if (flags & 1) {
                    if (!count)
                        return K_EIO;
                    UINT64 vcn = rd64(entry + n - 8),
                           unit = v->block <= v->index_size ? v->block : 512;
                    if (vcn > ~0ULL / unit)
                        return K_EIO;
                    block_offset = vcn * unit;
                    if (block_offset % v->index_size || block_offset > rd64(ia + 48) ||
                        v->index_size > rd64(ia + 48) - block_offset)
                        return K_EIO;
                    UINT8 *ba, *bp;
                    UINT32 bl, bs;
                    UINT64 bitmap_bit = block_offset / v->index_size;
                    UINT8 allocated_bit;
                    e = ntfs_attr(v, raw, 0xb0, "$I30", &ba, &bl);
                    if (e)
                        return e;
                    if (!ba[8]) {
                        e = ntfs_value(ba, bl, &bp, &bs);
                        if (e)
                            return e;
                        if (bitmap_bit / 8 >= bs)
                            return K_EIO;
                        allocated_bit = bp[bitmap_bit / 8];
                    } else {
                        native_run bm[NATIVE_RUNS];
                        UINT32 bc;
                        e = ntfs_runs(v, ba, bl, bm, &bc);
                        if (e)
                            return e;
                        if (bitmap_bit / 8 >= rd64(ba + 48))
                            return K_EIO;
                        e = run_io(v, bm, bc, bitmap_bit / 8, &allocated_bit, 1, FALSE);
                        if (e)
                            return e;
                    }
                    if (!(allocated_bit & (1u << (bitmap_bit % 8))))
                        return K_EIO;
                    e = run_io(v, runs, count, block_offset, block, v->index_size, FALSE);
                    if (e)
                        return e;
                    e = ntfs_fixup(v, block, v->index_size, "INDX");
                    if (e)
                        return e;
                    if (rd64(block + 16) != vcn)
                        return K_EIO;
                    header = block + 24;
                    cap = v->index_size - 24;
                    root = FALSE;
                    descended = TRUE;
                    break;
                }
                if (!create)
                    return K_ENOENT;
                if (header[12])
                    return K_ENOTSUP;
                UINT32 need = (16 + keylen + 7) & ~7u;
                if (root) { /* Only resident leaf indexes grow; no B-tree split approximation. */
                    UINT8 replacement[4096];
                    UINT32 oldvalue = (UINT32)(value - attr),
                           newlen = (oldvalue + size + need + 7) & ~7u;
                    if (newlen > sizeof(replacement) ||
                        rd32(raw + 24) - len + newlen > v->record_size)
                        return K_ENOSPC;
                    mem_zero(replacement, newlen);
                    mem_copy(replacement, attr, oldvalue + size);
                    wr32(replacement + 4, newlen);
                    wr32(replacement + 16, size + need);
                    UINT8 *h = replacement + oldvalue + 16;
                    memmove(h + at + need, h + at, used - at);
                    mem_zero(h + at, need);
                    wr64(h + at, reference);
                    wr16(h + at + 8, (UINT16)need);
                    wr16(h + at + 10, (UINT16)keylen);
                    mem_copy(h + at + 16, filename, keylen);
                    wr32(h + 4, used + need);
                    wr32(h + 8, allocated + need);
                    e = nt_attr_replace(v, raw, attr, len, replacement, newlen);
                    if (e)
                        return e;
                    return nt_tx_record(parent, raw);
                }
                if (need > allocated - used)
                    return K_ENOSPC;
                memmove(header + at + need, header + at, used - at);
                mem_zero(header + at, need);
                wr64(header + at, reference);
                wr16(header + at + 8, (UINT16)need);
                wr16(header + at + 10, (UINT16)keylen);
                mem_copy(header + at + 16, filename, keylen);
                wr32(header + 4, used + need);
                goto store_index;
            }
            at += n;
        }
        if (!descended)
            return K_EIO;
        continue;
    store_index:
        if (root)
            return nt_tx_record(parent, raw);
        {
            UINT32 usa = rd16(block + 4), usac = rd16(block + 6);
            if (usa < 40 || usa + usac * 2 > v->index_size)
                return K_EIO;
            UINT16 seq = (UINT16)(rd16(block + usa) + 1);
            if (!seq || seq == 0xffff)
                seq = 1;
            wr16(block + usa, seq);
            for (UINT32 i = 1; i < usac; ++i) {
                wr16(block + usa + i * 2, rd16(block + i * 512 - 2));
                wr16(block + i * 512 - 2, seq);
            }
            return nt_tx_stream(runs, count, block_offset, block, v->index_size, TRUE);
        }
    }
    return K_E2BIG;
}
static int nt_prepare(native_volume *v)
{
    int e = native_clean(v);
    if (e)
        return e;
    if (v->block > v->index_size || !rd64(v->super + 56) || rd64(v->super + 56) >= v->blocks)
        return K_ENOTSUP;
    UINT8 mirror_record[4096], *attribute;
    UINT32 attrlen;
    e = ntfs_record(v, 1, mirror_record);
    if (e)
        return e;
    e = ntfs_attr(v, mirror_record, 0x80, "", &attribute, &attrlen);
    if (e)
        return e;
    native_run mirror_runs[NATIVE_RUNS];
    UINT32 mirror_count;
    e = ntfs_runs(v, attribute, attrlen, mirror_runs, &mirror_count);
    if (e)
        return e;
    if (mirror_count != 1 || mirror_runs[0].hole || mirror_runs[0].logical ||
        mirror_runs[0].physical != rd64(v->super + 56) ||
        mirror_runs[0].length * v->block < v->record_size * 4ULL)
        return K_EIO;
    nt_tx_volume = v;
    nt_tx_count = 0;
    return 0;
}
static int nt_commit(native_volume *v, const native_run *fresh, const void *data, UINT64 size)
{
    /* Stage the dirty volume record with other MFT edits to preserve 4Kn sector sharing. */
    UINT8 vol[4096], *a, *p;
    UINT32 len, n;
    int e = ntfs_record(v, 3, vol);
    if (e)
        return e;
    e = ntfs_attr(v, vol, 0x70, "", &a, &len);
    if (e)
        return e;
    e = ntfs_value(a, len, &p, &n);
    if (e)
        return e;
    if (n < 12 || rd16(p + 10))
        return K_EROFS;
    wr16(p + 10, 1);
    e = nt_tx_record(3, vol);
    if (e)
        return e;
    e = ntfs_dirty(v, TRUE);
    if (e) {
        v->failed = TRUE;
        return e;
    }
    UINT8 sector[4096];
    UINT64 off = 0, allocated = fresh->length * v->block;
    while (!e && off < allocated) {
        UINT64 nbytes = MIN(allocated - off, v->sector);
        mem_zero(sector, v->sector);
        UINT64 used = off < size ? MIN(size - off, nbytes) : 0;
        if (used)
            mem_copy(sector, (const UINT8 *)data + off, used);
        e = native_write_bytes(v, fresh->physical * v->block + off, sector, nbytes);
        off += nbytes;
    }
    for (UINT32 i = 0; !e && i < nt_tx_count; ++i)
        e = native_write_bytes(v, nt_tx[i].lba * v->sector, nt_tx[i].bytes, v->sector);
    if (!e)
        e = ntfs_dirty(v, FALSE);
    if (e)
        v->failed = TRUE;
    return e;
}
static int nt_new_record(native_volume *v, UINT32 *ino, UINT8 raw[4096])
{
    /* Use only initialized MFT slots; MFT expansion is unsupported. */
    for (UINT32 i = 24; i < v->mft_count && i < 1048576; ++i) {
        int e = nt_bitmap_bit(0, 0xb0, i, FALSE, FALSE);
        if (e < 0)
            return e;
        if (e)
            continue;
        e = run_io(v, v->mft, v->mft_runs, (UINT64)i * v->record_size, raw, v->record_size, FALSE);
        if (e)
            return e;
        UINT16 sequence = 1;
        if (!memcmp(raw, "FILE", 4)) {
            e = ntfs_fixup(v, raw, v->record_size, "FILE");
            if (e)
                return e;
            if (rd16(raw + 22) & 1)
                return K_EIO;
            sequence = (UINT16)(rd16(raw + 16) + 1);
            if (!sequence)
                sequence = 1;
        } else {
            for (UINT32 j = 0; j < v->record_size; ++j)
                if (raw[j])
                    return K_EIO;
        }
        mem_zero(raw, v->record_size);
        mem_copy(raw, "FILE", 4);
        wr16(raw + 4, 48);
        wr16(raw + 6, (UINT16)(v->record_size / 512 + 1));
        wr16(raw + 16, sequence);
        wr16(raw + 18, 1);
        UINT32 attrs = (48 + 2 * (v->record_size / 512 + 1) + 7) & ~7u;
        wr16(raw + 20, (UINT16)attrs);
        wr16(raw + 22, 1);
        wr32(raw + 24, attrs + 8);
        wr32(raw + 28, v->record_size);
        wr32(raw + 44, i);
        wr32(raw + attrs, 0xffffffff);
        e = nt_bitmap_bit(0, 0xb0, i, TRUE, TRUE);
        if (e)
            return e;
        *ino = i;
        return 0;
    }
    return K_ENOSPC;
}
static int ntfs_replace(k_file *f, const void *data, UINT64 size)
{
    native_volume *v = &native_volumes[f->object];
    if (size > 4 * 1024 * 1024)
        return K_E2BIG;
    int e = nt_prepare(v);
    if (e)
        return e;
    UINT8 raw[4096], *a, *fn;
    UINT32 len, fnsize;
    e = ntfs_record(v, f->cluster, raw);
    if (e)
        return e;
    if (rd16(raw + 16) != f->directory_offset)
        return K_ENOENT;
    e = ntfs_regular_writable(v, f->cluster, raw);
    if (e)
        return e;
    e = ntfs_attr(v, raw, 0x30, "", &a, &len);
    if (e)
        return e;
    e = ntfs_value(a, len, &fn, &fnsize);
    if (e)
        return e;
    char name[256];
    e = ntfs_name(fn, fnsize, name);
    if (e)
        return e;
    UINT64 parentref = rd64(fn);
    UINT32 parent = (UINT32)(parentref & 0xffffffffffffULL);
    if ((parentref & 0xffffffffffffULL) > 0xffffffff)
        return K_ENOTSUP;
    UINT8 parentraw[4096];
    e = ntfs_record(v, parent, parentraw);
    if (e)
        return e;
    if (rd16(parentraw + 16) != (parentref >> 48))
        return K_EIO;
    e = ntfs_attr(v, raw, 0x80, "", &a, &len);
    if (e)
        return e;
    native_run old[NATIVE_RUNS];
    UINT32 oldn = 0;
    if (a[8]) {
        e = ntfs_runs(v, a, len, old, &oldn);
        if (e)
            return e;
        e = ntfs_data_boundaries(v, old, oldn);
        if (e)
            return e;
    }
    native_run fresh;
    e = nt_alloc_data(size, &fresh);
    if (e)
        return e;
    if (fresh.length) {
        e = ntfs_metadata_boundaries(v, &fresh, 1);
        if (e)
            return e;
    }
    for (UINT32 i = 0; i < oldn; ++i) {
        if (old[i].hole)
            return K_ENOTSUP;
        if (fresh.length && overlap(old[i].physical, old[i].length, fresh.physical, fresh.length))
            return K_EIO;
        for (UINT64 j = 0; j < old[i].length; ++j) {
            e = nt_bitmap_bit(6, 0x80, old[i].physical + j, FALSE, TRUE);
            if (e)
                return e;
        }
    }
    UINT8 replacement[128];
    UINT32 newlen = nt_data_attribute(replacement, rd16(a + 14), &fresh, size);
    e = nt_attr_replace(v, raw, a, len, replacement, newlen);
    if (e)
        return e;
    e = ntfs_attr(v, raw, 0x30, "", &a, &len);
    if (e)
        return e;
    e = ntfs_value(a, len, &fn, &fnsize);
    if (e)
        return e;
    wr64(fn + 40, fresh.length * v->block);
    wr64(fn + 48, size);
    e = nt_index_change(v, parent, (UINT64)f->cluster | ((UINT64)rd16(raw + 16) << 48), fn, fnsize,
                        FALSE);
    if (e)
        return e;
    e = nt_tx_record(f->cluster, raw);
    if (e)
        return e;
    e = nt_commit(v, &fresh, data, size);
    if (!e)
        f->size = size;
    return e;
}
static int ntfs_create(UINT32 index, const char *path, BOOLEAN directory, const void *data,
                       UINT64 size, k_file *out)
{
    native_volume *v = &native_volumes[index];
    if (size > 4 * 1024 * 1024)
        return K_E2BIG;
    char parent_path[K_PATH_MAX], name[256];
    str_copy(parent_path, path, sizeof(parent_path));
    UINTN split = str_len(parent_path);
    while (split && parent_path[split - 1] != '/')
        --split;
    str_copy(name, parent_path + split, sizeof(name));
    parent_path[split ? split - 1 : 0] = 0;
    UINT32 chars = (UINT32)str_len(name);
    if (!chars || chars > 255 || name[chars - 1] == ' ' || name[chars - 1] == '.')
        return K_EINVAL;
    for (UINT32 i = 0; i < chars; ++i)
        if (name[i] == '\\' || name[i] == ':' || name[i] == '*' || name[i] == '?' ||
            name[i] == '"' || name[i] == '<' || name[i] == '>' || name[i] == '|')
            return K_EINVAL;
    k_file parent, exists;
    int e = native_open(index, parent_path, &parent);
    if (e)
        return e;
    if (!(parent.attributes & 0x10))
        return K_EINVAL;
    e = ntfs_directory(v, parent.cluster, name, &exists, NULL, NULL);
    if (!e)
        return K_EEXIST;
    if (e != K_ENOENT)
        return e;
    e = nt_prepare(v);
    if (e)
        return e;
    UINT8 parentraw[4096], *a, *si;
    UINT32 len, silen;
    e = ntfs_record(v, parent.cluster, parentraw);
    if (e)
        return e;
    e = ntfs_attr(v, parentraw, 0x10, "", &a, &len);
    if (e)
        return e;
    e = ntfs_value(a, len, &si, &silen);
    if (e)
        return e;
    if (silen < 72 || !rd32(si + 52))
        return K_ENOTSUP;
    UINT8 raw[4096];
    UINT32 ino;
    e = nt_new_record(v, &ino, raw);
    if (e)
        return e;
    if (directory)
        wr16(raw + 22, 3);
    native_run fresh = {0};
    if (!directory) {
        e = nt_alloc_data(size, &fresh);
        if (e)
            return e;
    }
    if (fresh.length) {
        e = ntfs_metadata_boundaries(v, &fresh, 1);
        if (e)
            return e;
    }
    UINT8 standard[72];
    mem_copy(standard, si, 72);
    wr32(standard + 32, directory ? 0x10000000 : 0x20);
    wr32(standard + 48, 0);
    wr64(standard + 56, 0);
    wr64(standard + 64, 0);
    UINT8 attr[1024];
    UINT32 n = nt_resident_attr(attr, 0x10, 0, "", standard, 72);
    e = nt_record_append(v, raw, attr, n);
    if (e)
        return e;
    UINT8 filename[576];
    mem_zero(filename, sizeof(filename));
    wr64(filename, (UINT64)parent.cluster | ((UINT64)rd16(parentraw + 16) << 48));
    mem_copy(filename + 8, standard, 32);
    wr64(filename + 40, fresh.length * v->block);
    wr64(filename + 48, size);
    wr32(filename + 56, directory ? 0x10000000 : 0x20);
    filename[64] = (UINT8)chars;
    filename[65] = 1;
    for (UINT32 i = 0; i < chars; ++i)
        wr16(filename + 66 + i * 2, (UINT8)name[i]);
    UINT32 fnsize = 66 + chars * 2;
    n = nt_resident_attr(attr, 0x30, 1, "", filename, fnsize);
    e = nt_record_append(v, raw, attr, n);
    if (e)
        return e;
    if (directory) {
        UINT8 root[48];
        mem_zero(root, 48);
        wr32(root, 0x30);
        wr32(root + 4, 1);
        wr32(root + 8, v->index_size);
        root[12] =
            (UINT8)(v->index_size >= v->block ? v->index_size / v->block : v->index_size / 512);
        wr32(root + 16, 16);
        wr32(root + 20, 32);
        wr32(root + 24, 32);
        wr16(root + 40, 16);
        wr16(root + 44, 2);
        n = nt_resident_attr(attr, 0x90, 2, "$I30", root, 48);
    } else
        n = nt_data_attribute(attr, 2, &fresh, size);
    e = nt_record_append(v, raw, attr, n);
    if (e)
        return e;
    wr16(raw + 40, 3);
    e = nt_index_change(v, parent.cluster, (UINT64)ino | ((UINT64)rd16(raw + 16) << 48), filename,
                        fnsize, TRUE);
    if (e)
        return e;
    e = nt_tx_record(ino, raw);
    if (e)
        return e;
    e = nt_commit(v, &fresh, data, size);
    if (!e && out)
        *out = (k_file){5, index, ino, directory ? 0x10 : 0, size, rd16(raw + 16)};
    return e;
}

static int native_create(UINT32 index, const char *path, BOOLEAN directory, k_file *out)
{
    if (index >= native_count)
        return K_EINVAL;
    if (native_volumes[index].kind == 4)
        return ext_create(index, path, directory, NULL, 0, out);
    return ntfs_create(index, path, directory, NULL, 0, out);
}
static int native_store(UINT32 index, const char *path, const void *data, UINT64 size)
{
    if (index >= native_count)
        return K_EINVAL;
    k_file f;
    int e = native_open(index, path, &f);
    if (e == K_ENOENT)
        return native_volumes[index].kind == 4 ? ext_create(index, path, FALSE, data, size, NULL)
                                               : ntfs_create(index, path, FALSE, data, size, NULL);
    if (e)
        return e;
    if (f.attributes & 0x10)
        return K_EINVAL;
    if (f.kind == 4)
        return ext_replace(&f, data, size);
    return ntfs_replace(&f, data, size);
}

static const char *native_description(UINT32 i)
{
    if (i >= native_count)
        return "invalid volume";
    native_volume *v = &native_volumes[i];
    return v->kind == 4 ? (v->read_only || v->failed
                               ? "ext4 read-only"
                               : "ext4 read/write (bounded inline extents, non-indexed creation)")
                        : (v->read_only || v->failed
                               ? "NTFS read-only"
                               : "NTFS read/write (existing MFT capacity, no index splits)");
}
static int native_sync_all(void)
{
    int e = 0;
    for (UINT32 i = 0; i < native_count; ++i) {
        int n = native_volumes[i].failed  ? K_EIO
                : native_volumes[i].wrote ? disk_flush(native_volumes[i].disk)
                                          : 0;
        if (n && !e)
            e = n;
    }
    return e;
}

/* GPT requires valid header and entry-array CRCs; corrupt GPT is not reinterpreted as MBR. */
static UINT32 crc_step(UINT32 crc, const UINT8 *p, UINTN n)
{
    while (n--) {
        crc ^= *p++;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0 - (crc & 1)));
    }
    return crc;
}
typedef struct {
    UINT64 start, length;
    UINT32 number;
    BOOLEAN read_only;
} partition_entry;
static int partition_add(partition_entry *parts, UINT32 *count, UINT64 start, UINT64 length,
                         UINT32 number, UINT64 disk_size)
{
    if (!length || !start || start >= disk_size || length > disk_size - start || *count >= 128)
        return K_EINVAL;
    for (UINT32 i = 0; i < *count; ++i)
        if (overlap(start, length, parts[i].start, parts[i].length))
            return K_EINVAL;
    parts[*count] = (partition_entry){start, length, number, FALSE};
    ++*count;
    return 0;
}
static int gpt_parse(UINT32 disk_id, UINT64 header_lba, partition_entry parts[128], UINT32 *count)
{
    UINT8 sector[4096];
    disk *d = &disks[disk_id];
    UINT32 bps = d->info.sector_size;
    int e = k_disk_read(disk_id, header_lba, 1, sector, sizeof(sector));
    if (e)
        return e;
    UINT32 len = rd32(sector + 12), saved = rd32(sector + 16);
    if (memcmp(sector, "EFI PART", 8) || rd32(sector + 8) != 0x10000 || len < 92 || len > bps ||
        rd32(sector + 20) || rd64(sector + 24) != header_lba)
        return K_EINVAL;
    wr32(sector + 16, 0);
    if ((crc_step(0xffffffff, sector, len) ^ 0xffffffff) != saved)
        return K_EINVAL;
    UINT64 backup = rd64(sector + 32), first = rd64(sector + 40), last = rd64(sector + 48),
           table = rd64(sector + 72);
    UINT32 entries = rd32(sector + 80), entry_size = rd32(sector + 84), wanted = rd32(sector + 88);
    UINT64 table_bytes = (UINT64)entries * entry_size,
           table_sectors = (table_bytes + bps - 1) / bps;
    if (backup >= d->info.sectors || backup == header_lba || first > last || first < 2 ||
        last >= d->info.sectors - 1 || !entries || entries > 4096 || entry_size < 128 ||
        entry_size > 1024 || (entry_size & 127) || !table || table >= d->info.sectors ||
        table_sectors > d->info.sectors - table ||
        overlap(table, table_sectors, first, last - first + 1) ||
        overlap(table, table_sectors, header_lba, 1) || overlap(table, table_sectors, backup, 1))
        return K_EINVAL;
    UINT32 crc = 0xffffffff;
    UINT64 remaining = table_bytes;
    for (UINT64 i = 0; i < table_sectors; ++i) {
        e = k_disk_read(disk_id, table + i, 1, sector, sizeof(sector));
        if (e)
            return e;
        UINT64 n = MIN(remaining, bps);
        crc = crc_step(crc, sector, n);
        remaining -= n;
    }
    if ((crc ^ 0xffffffff) != wanted)
        return K_EINVAL;
    *count = 0;
    for (UINT32 i = 0; i < entries; ++i) {
        UINT8 entry[128];
        e = disk_bytes(disk_id, table * bps + (UINT64)i * entry_size, entry, sizeof(entry));
        if (e)
            return e;
        BOOLEAN nonzero = FALSE;
        for (UINT32 j = 0; j < 16; ++j)
            if (entry[j])
                nonzero = TRUE;
        if (!nonzero)
            continue;
        UINT64 a = rd64(entry + 32), b = rd64(entry + 40);
        if (a < first || a > b || b > last)
            return K_EINVAL;
        e = partition_add(parts, count, a, b - a + 1, i + 1, d->info.sectors);
        if (e)
            return e;
        parts[*count - 1].read_only = (rd64(entry + 48) & (1ULL << 60)) != 0;
    }
    return 0;
}
static BOOLEAN extended_type(UINT8 t) { return t == 5 || t == 0x0f || t == 0x85; }
static int mbr_parse(UINT32 id, const UINT8 mbr[512], partition_entry parts[128], UINT32 *count)
{
    UINT64 disk_size = disks[id].info.sectors, ext_base = 0, ext_length = 0;
    *count = 0;
    for (UINT32 i = 0; i < 4; ++i) {
        const UINT8 *p = mbr + 446 + 16 * i;
        UINT8 type = p[4];
        UINT64 start = rd32(p + 8), length = rd32(p + 12);
        if (!type || !length)
            continue;
        if (type == 0xee)
            return K_EINVAL;
        if (!start || start >= disk_size || length > disk_size - start)
            return K_EINVAL;
        if (extended_type(type)) {
            if (ext_length)
                return K_ENOTSUP;
            ext_base = start;
            ext_length = length;
        } else {
            int e = partition_add(parts, count, start, length, i + 1, disk_size);
            if (e)
                return e;
        }
    }
    if (ext_length)
        for (UINT32 i = 0; i < *count; ++i)
            if (overlap(ext_base, ext_length, parts[i].start, parts[i].length))
                return K_EINVAL;
    UINT64 ebr = ext_base, seen[124];
    UINT32 visits = 0;
    while (ext_length) {
        if (ebr < ext_base || ebr - ext_base >= ext_length || visits == ARRAY_LEN(seen))
            return K_EINVAL;
        for (UINT32 i = 0; i < visits; ++i)
            if (seen[i] == ebr)
                return K_EINVAL;
        seen[visits++] = ebr;
        UINT8 sector[4096];
        int e = k_disk_read(id, ebr, 1, sector, sizeof(sector));
        if (e)
            return e;
        if (sector[510] != 0x55 || sector[511] != 0xaa)
            return K_EINVAL;
        const UINT8 *p = sector + 446, *link = sector + 462;
        UINT64 relative = rd32(p + 8), length = rd32(p + 12);
        if (p[4] && length) {
            if (extended_type(p[4]) || !relative || relative >= ext_base + ext_length - ebr ||
                length > ext_base + ext_length - ebr - relative)
                return K_EINVAL;
            e = partition_add(parts, count, ebr + relative, length, 4 + visits, disk_size);
            if (e)
                return e;
        }
        if (!link[4])
            break;
        if (!extended_type(link[4]))
            return K_EINVAL;
        UINT64 next = rd32(link + 8), len = rd32(link + 12);
        if (!next || next >= ext_length || !len || len > ext_length - next)
            return K_EINVAL;
        ebr = ext_base + next;
    }
    for (UINT32 i = 0; i < visits; ++i)
        for (UINT32 j = 0; j < *count; ++j)
            if (overlap(seen[i], 1, parts[j].start, parts[j].length))
                return K_EINVAL;
    return 0;
}
int k_vfs_mount_disks(void)
{
    if (!vfs_ready || k_cpu_id() != 0)
        return K_EBUSY;
    int lock_error = k_mutex_lock(&vfs_mutex);
    if (lock_error)
        return lock_error;
    for (UINT32 id = 0; id < k_disk_count(); ++id) {
        if (disk_scanned[id] || !disks[id].info.online)
            continue;
        disk_scanned[id] = TRUE;
        char path[K_PATH_MAX];
        str_copy(path, "/devices/", sizeof(path));
        str_copy(path + 9, disks[id].info.name, K_PATH_MAX - 9);
        int node = ram_new(path, FALSE);
        if (node >= 0) {
            nodes[node].kind = 3;
            nodes[node].block_id = id;
            nodes[node].size = disks[id].info.sectors * disks[id].info.sector_size;
        }
        if (disks[id].kind == DISK_RAM)
            continue;
        UINT8 sector[4096];
        int e = k_disk_read(id, 0, 1, sector, sizeof(sector));
        if (e) {
            con_print("partition read: ");
            con_print(k_strerror(e));
            con_print("\n");
            continue;
        }
        if (sector[510] != 0x55 || sector[511] != 0xaa)
            continue;
        BOOLEAN protective = FALSE, recovered = FALSE;
        for (UINT32 j = 0; j < 4; ++j)
            if (sector[446 + 16 * j + 4] == 0xee)
                protective = TRUE;
        partition_entry parts[128];
        UINT32 count = 0;
        if (protective) {
            e = gpt_parse(id, 1, parts, &count);
            if (e && disks[id].info.sectors > 2) {
                e = gpt_parse(id, disks[id].info.sectors - 1, parts, &count);
                if (!e) {
                    con_print(disks[id].info.name);
                    con_print(": using validated backup GPT (read-only until repaired)\n");
                    recovered = TRUE;
                }
            }
        } else
            e = mbr_parse(id, sector, parts, &count);
        if (!e && count) {
            for (UINT32 p = 0; p < count; ++p) {
                UINT32 part_index = partition_count;
                if (partition_count < MAX_PARTITIONS) {
                    partitions[partition_count] =
                        (k_partition_info){partition_count,
                                           id,
                                           parts[p].number,
                                           disks[id].info.sector_size,
                                           parts[p].start,
                                           parts[p].length,
                                           recovered || parts[p].read_only,
                                           0,
                                           0,
                                           0};
                    ++partition_count;
                }
                UINT32 before = volume_count, prior_mount = mount_count;
                if (!fat_mount(id, parts[p].start, parts[p].length, parts[p].number) &&
                    before < volume_count)
                    volumes[before].partition_ro = recovered || parts[p].read_only;
                if (prior_mount == mount_count)
                    (void)native_probe(id, parts[p].start, parts[p].length, parts[p].number,
                                       recovered || parts[p].read_only);
                if (part_index < partition_count)
                    partitions[part_index].mounted = prior_mount < mount_count;
            }
        } else if (!protective) {
            /* Superfloppy BPBs still undergo full FAT validation. */
            UINT32 before = volume_count;
            if (!fat_mount(id, 0, disks[id].info.sectors, 0) && before < volume_count)
                volumes[before].partition_ro = e != 0;
        }
        if (e && protective) {
            con_print(disks[id].info.name);
            con_print(": invalid/unsupported GPT; no volumes mounted\n");
        }
    }
    k_mutex_unlock(&vfs_mutex);
    return 0;
}
