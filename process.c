// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"

struct k_process processes[MAX_PROCESSES];
static k_mutex process_mutex = K_MUTEX_INIT;
static UINT32 input_owner, next_process_id = 1;
static BOOLEAN valid_process(k_process *p)
{
    UINTN a = (UINTN)p, b = (UINTN)processes;
    return a >= b && a < b + sizeof(processes) && !((a - b) % sizeof(*p)) && p->used;
}
static k_process *process_create_locked(const char *name)
{
    if (!name)
        return NULL;
    UINT64 f = k_spin_lock(&sched_lock);
    if (!next_process_id) {
        k_spin_unlock(&sched_lock, f);
        return NULL;
    }
    k_process *p = NULL;
    for (UINT32 i = 0; i < MAX_PROCESSES; ++i)
        if (!processes[i].used) {
            p = &processes[i];
            mem_zero(p, sizeof(*p));
            p->used = TRUE;
            p->id = next_process_id++;
            p->execution_class = K_EXEC_RING3;
            /* Process UUIDs are instance identifiers, never secrets. */
            wr64(p->uuid, arch_identity_seed());
            wr64(p->uuid + 8, ((UINT64)p->id << 32) | p->id);
            str_copy(p->name, name, sizeof(p->name));
            break;
        }
    k_spin_unlock(&sched_lock, f);
    if (!p)
        return NULL;
    p->as = k_as_create();
    if (!p->as) {
        f = k_spin_lock(&sched_lock);
        p->used = FALSE;
        k_spin_unlock(&sched_lock, f);
        return NULL;
    }
    p->as->owner = p;
    return p;
}
k_process *k_process_create(const char *name)
{
    if (k_mutex_lock(&process_mutex))
        return NULL;
    k_process *p = process_create_locked(name);
    k_mutex_unlock(&process_mutex);
    return p;
}
k_address_space *k_process_space(k_process *p) { return valid_process(p) ? p->as : NULL; }
UINT32 k_process_id(k_process *p) { return valid_process(p) ? p->id : 0; }
static int rief_string(const UINT8 *bytes, const rief_header_t *h, UINT32 off, char out[128])
{
    if (off >= h->string_table_size)
        return K_ENOEXEC;
    const UINT8 *s = bytes + h->string_table_offset + off;
    for (UINT32 i = 0; i < 128 && off + i < h->string_table_size; ++i) {
        if (!s[i]) {
            out[i] = 0;
            return i ? 0 : K_ENOEXEC;
        }
        if (s[i] < 33 || s[i] > 126)
            return K_ENOEXEC;
        out[i] = (char)s[i];
    }
    return K_ENOEXEC;
}
BOOLEAN overlap(UINT64 a, UINT64 n, UINT64 b, UINT64 m) { return n && m && a < b + m && b < a + n; }
static int rief_load_locked(k_process *p, const void *image, UINT64 size,
                            const k_rief_binding *bindings, UINT32 binding_count)
{
    if (!valid_process(p) || p->loaded || p->task_id || !image || (binding_count && !bindings) ||
        binding_count > 64)
        return K_EINVAL;
    if (size < sizeof(rief_header_t) || size > 64ULL * 1024 * 1024)
        return K_ENOEXEC;
    const UINT8 *bytes = image;
    rief_header_t h;
    mem_copy(&h, image, sizeof(h));
    if (h.magic != RIEF_MAGIC || h.version_major != 1 || h.version_minor != 0 ||
        h.architecture != ARCH_RIEF_MACHINE || h.flags || h.reserved || h.header_size < sizeof(h) ||
        h.header_size > 4096 || h.header_size > size || !h.region_count ||
        h.region_count > MAX_REGIONS || h.reloc_count > 4096 || h.import_count > 64 ||
        h.export_count > MAX_EXPORTS || h.entry_region >= h.region_count)
        return K_ENOEXEC;
    for (UINT32 i = sizeof(h); i < h.header_size; ++i)
        if (bytes[i])
            return K_ENOEXEC;
    UINT64 meta_off[] = {0,
                         h.region_table_offset,
                         h.reloc_table_offset,
                         h.import_table_offset,
                         h.export_table_offset,
                         h.string_table_offset};
    UINT64 meta_len[] = {h.header_size,
                         h.region_count * sizeof(rief_region_t),
                         h.reloc_count * sizeof(rief_reloc_t),
                         h.import_count * sizeof(rief_import_t),
                         h.export_count * sizeof(rief_export_t),
                         h.string_table_size};
    for (UINT32 i = 0; i < ARRAY_LEN(meta_off); ++i) {
        if (!span_ok(meta_off[i], meta_len[i], size) || (!meta_len[i] && meta_off[i]) ||
            (i > 0 && i < 5 && (meta_off[i] & 7)))
            return K_ENOEXEC;
        for (UINT32 j = 0; j < i; ++j)
            if (overlap(meta_off[i], meta_len[i], meta_off[j], meta_len[j]))
                return K_ENOEXEC;
    }
    rief_region_t regions[MAX_REGIONS];
    UINT64 total = 0;
    for (UINT32 i = 0; i < h.region_count; ++i) {
        mem_copy(&regions[i], bytes + h.region_table_offset + i * sizeof(rief_region_t),
                 sizeof(rief_region_t));
        rief_region_t *r = &regions[i];
        if (!r->memory_size || r->memory_size > 64ULL * 1024 * 1024 ||
            r->file_size > r->memory_size || !span_ok(r->file_offset, r->file_size, size) ||
            !power2(r->alignment) || r->alignment > 0x40000000 || (r->flags & ~7) ||
            !(r->flags & RIEF_REGION_R) || ((r->flags & 6) == 6))
            return K_ENOEXEC;
        total += ALIGN_UP(r->memory_size, 4096);
        if (total > 64ULL * 1024 * 1024)
            return K_E2BIG;
        for (UINT32 j = 0; j < ARRAY_LEN(meta_off); ++j)
            if (overlap(r->file_offset, r->file_size, meta_off[j], meta_len[j]))
                return K_ENOEXEC;
        for (UINT32 j = 0; j < i; ++j)
            if (overlap(r->file_offset, r->file_size, regions[j].file_offset, regions[j].file_size))
                return K_ENOEXEC;
    }
    if (!(regions[h.entry_region].flags & RIEF_REGION_X) ||
        h.entry_offset >= regions[h.entry_region].file_size)
        return K_ENOEXEC;
    UINT64 imports[64];
    char symbol[128];
    for (UINT32 i = 0; i < h.import_count; ++i) {
        rief_import_t imp;
        mem_copy(&imp, bytes + h.import_table_offset + i * sizeof(imp), sizeof(imp));
        if (imp.reserved || rief_string(bytes, &h, imp.name_offset, symbol))
            return K_ENOEXEC;
        BOOLEAN found = FALSE;
        for (UINT32 b = 0; b < binding_count; ++b)
            if (bindings[b].name && str_eq(symbol, bindings[b].name)) {
                if (found || k_as_check(p->as, bindings[b].address, 1, FALSE))
                    return K_EFAULT;
                imports[i] = bindings[b].address;
                found = TRUE;
            }
        if (!found)
            return K_ENOENT;
    }
    /* RIEF relocations are sorted and disjoint to avoid overlapping-patch ambiguity. */
    UINT32 last_region = 0;
    UINT64 last_end = 0;
    for (UINT32 i = 0; i < h.reloc_count; ++i) {
        rief_reloc_t r;
        mem_copy(&r, bytes + h.reloc_table_offset + i * sizeof(r), sizeof(r));
        UINT32 width = r.type == RIEF_RELOC_REL32 ? 4 : 8;
        if (r.reserved || r.type < RIEF_RELOC_ABS64 || r.type > RIEF_RELOC_IMPORT64 ||
            r.patch_region >= h.region_count ||
            !span_ok(r.patch_offset, width, regions[r.patch_region].memory_size) ||
            (i && (r.patch_region < last_region ||
                   (r.patch_region == last_region && r.patch_offset < last_end))))
            return K_ENOEXEC;
        if (r.type == RIEF_RELOC_IMPORT64) {
            if (r.target_region >= h.import_count || r.target_offset)
                return K_ENOEXEC;
        } else if (r.target_region >= h.region_count ||
                   r.target_offset > regions[r.target_region].memory_size)
            return K_ENOEXEC;
        last_region = r.patch_region;
        last_end = r.patch_offset + width;
    }
    for (UINT32 i = 0; i < h.export_count; ++i) {
        rief_export_t ex;
        mem_copy(&ex, bytes + h.export_table_offset + i * sizeof(ex), sizeof(ex));
        if (ex.region >= h.region_count || ex.offset >= regions[ex.region].memory_size ||
            rief_string(bytes, &h, ex.name_offset, p->exports[i].name))
            return K_ENOEXEC;
        for (UINT32 j = 0; j < i; ++j)
            if (str_eq(p->exports[i].name, p->exports[j].name))
                return K_ENOEXEC;
    }
    UINT64 old_next = p->as->next;
    UINT32 old_pages = p->as->pages;
    int error = 0;
    for (UINT32 i = 0; i < h.region_count; ++i) {
        rief_region_t *r = &regions[i];
        error = k_as_alloc(p->as, r->memory_size, r->alignment, r->flags, &p->region_base[i]);
        if (error)
            goto fail;
        p->region_size[i] = r->memory_size;
        error = as_copy(p->as, p->region_base[i], (void *)(bytes + r->file_offset), r->file_size,
                        TRUE, TRUE);
        if (error)
            goto fail;
    }
    for (UINT32 i = 0; i < h.reloc_count; ++i) {
        rief_reloc_t r;
        mem_copy(&r, bytes + h.reloc_table_offset + i * sizeof(r), sizeof(r));
        UINT64 target;
        if (r.type == RIEF_RELOC_IMPORT64)
            target = imports[r.target_region];
        else
            target = p->region_base[r.target_region] + r.target_offset;

        if (r.addend >= 0) {
            if ((UINT64)r.addend > ~0ULL - target) {
                error = K_ENOEXEC;
                goto fail;
            }
            target += (UINT64)r.addend;
        } else {
            UINT64 sub = 0 - (UINT64)r.addend;
            if (sub > target) {
                error = K_ENOEXEC;
                goto fail;
            }
            target -= sub;
        }
        if (target < K_USER_BASE || target >= K_USER_LIMIT) {
            error = K_ENOEXEC;
            goto fail;
        }
        if (r.type != RIEF_RELOC_IMPORT64 &&
            (target < p->region_base[r.target_region] ||
             target - p->region_base[r.target_region] > p->region_size[r.target_region])) {
            error = K_ENOEXEC;
            goto fail;
        }
        if (r.type == RIEF_RELOC_IMPORT64 && k_as_check(p->as, target, 1, FALSE)) {
            error = K_EFAULT;
            goto fail;
        }
        UINT64 patch = p->region_base[r.patch_region] + r.patch_offset, value = target;
        UINT32 width = r.type == RIEF_RELOC_REL32 ? 4 : 8;
        if (r.type == RIEF_RELOC_REL32 || r.type == RIEF_RELOC_REL64) {
            INT64 delta = (INT64)target - (INT64)(patch + width);
            if (width == 4 && (delta < (-2147483647LL - 1) || delta > 2147483647LL)) {
                error = K_ENOEXEC;
                goto fail;
            }
            value = (UINT64)delta;
        }
        error = as_copy(p->as, patch, &value, width, TRUE, TRUE);
        if (error)
            goto fail;
    }
    for (UINT32 i = 0; i < h.export_count; ++i) {
        rief_export_t ex;
        mem_copy(&ex, bytes + h.export_table_offset + i * sizeof(ex), sizeof(ex));
        p->exports[i].address = p->region_base[ex.region] + ex.offset;
    }
    p->region_count = h.region_count;
    p->export_count = h.export_count;
    p->entry = p->region_base[h.entry_region] + h.entry_offset;
    p->loaded = TRUE;
    return 0;
fail:
    arch_as_rollback(p->as, old_next, old_pages);
    mem_zero(p->region_base, sizeof(p->region_base));
    mem_zero(p->region_size, sizeof(p->region_size));
    mem_zero(p->exports, sizeof(p->exports));
    return error;
}
int k_rief_load(k_process *p, const void *image, UINT64 size, const k_rief_binding *bindings,
                UINT32 binding_count)
{
    int e = k_mutex_lock(&process_mutex);
    if (e)
        return e;
    e = rief_load_locked(p, image, size, bindings, binding_count);
    k_mutex_unlock(&process_mutex);
    return e;
}
int k_rief_export(k_process *p, const char *symbol, UINT64 *address)
{
    if (!valid_process(p) || !p->loaded || !symbol || !address)
        return K_EINVAL;
    for (UINT32 i = 0; i < p->export_count; ++i)
        if (str_eq(symbol, p->exports[i].name)) {
            *address = p->exports[i].address;
            return 0;
        }
    return K_ENOENT;
}
static int process_start_locked(k_process *p, UINT32 cpu, UINT32 *id)
{
    if (!valid_process(p) || !p->loaded || p->task_id || !id)
        return K_EINVAL;
    if (!p->user_stack) {
        UINT64 stack;
        int e = k_as_alloc(p->as, 8 * 4096, 4096, RIEF_REGION_R | RIEF_REGION_W, &stack);
        if (e)
            return e;
        p->user_stack = arch_initial_user_stack(stack + 8 * 4096);
    }
    if (!p->tls) {
        int e = k_as_alloc(p->as, K_TLS_BYTES, 4096, RIEF_REGION_R | RIEF_REGION_W, &p->tls);
        if (e)
            return e;
        UINT64 tcb[3] = {p->tls, p->id, 0};
        e = k_copy_to_user(p->as, p->tls, tcb, sizeof(tcb));
        if (e)
            return e;
    }
    UINT64 f = k_spin_lock(&sched_lock);
    if (cpu == K_CPU_AUTO) {
        UINT64 least = ~0ULL;
        cpu = 0;
        for (UINT32 c = 0; c < platform.discovered_cpus; ++c)
            if (cpus[c].online) {
                UINT64 load = 0;
                for (UINT32 i = 0; i < K_MAX_TASKS; ++i)
                    if (tasks[i].cpu == c && !tasks[i].idle && tasks[i].state != K_TASK_UNUSED &&
                        tasks[i].state != K_TASK_ZOMBIE)
                        load += tasks[i].weight;
                if (load < least) {
                    least = load;
                    cpu = c;
                }
            }
    }
    if (cpu >= platform.discovered_cpus || !cpus[cpu].online) {
        k_spin_unlock(&sched_lock, f);
        return K_EINVAL;
    }
    task *t = new_task(p->name, NULL, NULL, 1024, cpu, FALSE);
    if (!t) {
        k_spin_unlock(&sched_lock, f);
        return K_ENOMEM;
    }
    t->process = p;
    arch_task_set_tls(t, p->tls);
    UINT64 tid = t->id;
    (void)k_copy_to_user(p->as, p->tls + 16, &tid, sizeof(tid));
    arch_user_context_init(t, p->entry, p->user_stack);
    p->task_id = t->id;
    p->state = K_TASK_READY;
    *id = t->id;
    k_spin_unlock(&sched_lock, f);
    return 0;
}
int k_process_start_on(k_process *p, UINT32 cpu, UINT32 *id)
{
    int e = k_mutex_lock(&process_mutex);
    if (e)
        return e;
    e = process_start_locked(p, cpu, id);
    k_mutex_unlock(&process_mutex);
    return e;
}
int k_process_start(k_process *p, UINT32 *id) { return k_process_start_on(p, K_CPU_AUTO, id); }
static int process_destroy_locked(k_process *p)
{
    if (!valid_process(p))
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    if (p->task_id) {
        if (p->state != K_TASK_ZOMBIE) {
            k_spin_unlock(&sched_lock, f);
            return K_EBUSY;
        }
        for (UINT32 i = 0; i < K_MAX_TASKS; ++i)
            if (tasks[i].process == p) {
                for (UINT32 c = 0; c < platform.discovered_cpus; ++c)
                    if (cpus[c].current == &tasks[i] ||
                        __atomic_load_n(&cpus[c].stack_owner, __ATOMIC_ACQUIRE) == &tasks[i]) {
                        k_spin_unlock(&sched_lock, f);
                        return K_EBUSY;
                    }
                free_task(&tasks[i]);
            }
    }
    process_resources_release(p);
    p->as->owner = NULL;
    k_spin_unlock(&sched_lock, f);
    int e = k_as_destroy(p->as);
    if (e) {
        p->as->owner = p;
        return e;
    }
    f = k_spin_lock(&sched_lock);
    mem_zero(p, sizeof(*p));
    k_spin_unlock(&sched_lock, f);
    return 0;
}
int k_process_destroy(k_process *p)
{
    int e = k_mutex_lock(&process_mutex);
    if (e)
        return e;
    e = process_destroy_locked(p);
    k_mutex_unlock(&process_mutex);
    return e;
}
int k_process_get(UINT32 i, k_process_info *out)
{
    if (i >= MAX_PROCESSES || !out)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    k_process *p = &processes[i];
    if (!p->used) {
        k_spin_unlock(&sched_lock, f);
        return K_ENOENT;
    }
    mem_zero(out, sizeof(*out));
    out->id = p->id;
    out->task_id = p->task_id;
    out->state = p->state;
    out->fault_vector = p->fault_vector;
    out->exit_code = p->exit_code;
    out->fault_address = p->fault_address;
    out->fault_ip = p->fault_ip;
    out->parent = p->parent;
    out->execution_class = p->execution_class;
    mem_copy(out->uuid, p->uuid, 16);
    for (UINT32 j = 0; j < K_MAX_TASKS; ++j)
        if (tasks[j].process == p) {
            out->cpu = tasks[j].cpu;
            break;
        }
    str_copy(out->name, p->name, sizeof(out->name));
    k_spin_unlock(&sched_lock, f);
    return 0;
}
static int user_string(k_process *p, UINT64 address, char out[K_PATH_MAX])
{
    for (UINT32 i = 0; i < K_PATH_MAX; ++i) {
        if (address > K_USER_CANON_LIMIT - 1 - i ||
            k_copy_from_user(p->as, &out[i], address + i, 1))
            return K_EFAULT;
        if (!out[i])
            return i ? 0 : K_EINVAL;
    }
    return K_E2BIG;
}
INT64 syscall_dispatch(const k_syscall_request *f)
{
    task *t = current_task();
    if (!t || !t->process)
        return K_EPERM;
    k_process *p = t->process;
    switch (f->number) {
    case K_SYS_EXIT:
        k_task_exit((INT32)f->args[0]);
    case K_SYS_WRITE: {
        if (f->args[0] >= 3 && f->args[0] < 16) {
            UINT32 fd = (UINT32)f->args[0];
            if (!p->fd_used[fd] || !(p->fd_flags[fd] & K_OPEN_WRITE))
                return K_EPERM;
            if (f->args[2] > 4096)
                return K_E2BIG;
            if (!f->args[2])
                return 0;
            UINT8 data[4096];
            int e = k_copy_from_user(p->as, data, f->args[1], f->args[2]);
            if (e)
                return e;
            UINT64 written = 0, off = (p->fd_flags[fd] & K_OPEN_APPEND) ? ~0ULL : p->fd_offset[fd];
            e = k_vfs_write(&p->files[fd], off, data, f->args[2], &written);
            if (e)
                return e;
            p->fd_offset[fd] =
                (p->fd_flags[fd] & K_OPEN_APPEND) ? p->files[fd].size : off + written;
            return (INT64)written;
        }
        if (f->args[0] != 1 && f->args[0] != 2)
            return K_EINVAL;
        if (f->args[2] > 4096)
            return K_E2BIG;
        if (!f->args[2])
            return 0;
        int e = k_as_check(p->as, f->args[1], f->args[2], FALSE);
        if (e)
            return e;
        char buf[129];
        UINT64 done = 0;
        while (done < f->args[2]) {
            UINT64 n = MIN(f->args[2] - done, 128);
            e = k_copy_from_user(p->as, buf, f->args[1] + done, n);
            if (e)
                return e;
            for (UINT64 i = 0; i < n; ++i) {
                /* Sanitize user console output; do not interpret terminal control bytes. */
                char c = buf[i];
                con_putc((c == '\n' || (c >= 32 && c < 127)) ? c : '?');
            }
            done += n;
        }
        return (INT64)done;
    }
    case K_SYS_YIELD:
        k_yield();
        return 0;
    case K_SYS_SLEEP:
        if (f->args[0] > 86400000)
            return K_EINVAL;
        k_sleep(f->args[0]);
        return 0;
    case K_SYS_GETPID:
        return p->id;
    case K_SYS_TICKS:
        return (INT64)k_ticks();
    case K_SYS_SEND: {
        if (f->args[2] > K_IPC_BYTES || f->args[0] > 0xffffffff)
            return K_EINVAL;
        UINT8 bytes[K_IPC_BYTES];
        int e = k_copy_from_user(p->as, bytes, f->args[1], f->args[2]);
        return e ? e : k_ipc_send((UINT32)f->args[0], bytes, (UINT32)f->args[2]);
    }
    case K_SYS_RECV: {
        if (f->args[1] < sizeof(k_message))
            return K_EINVAL;
        int e = k_as_check(p->as, f->args[0], sizeof(k_message), TRUE);
        if (e)
            return e;
        k_message msg;
        e = k_ipc_receive(&msg);
        return e ? e : k_copy_to_user(p->as, f->args[0], &msg, sizeof(msg));
    }
    case K_SYS_OPEN: {
        UINT64 flags = f->args[1];
        if ((flags & ~31ULL) || ((flags & 30) && !(flags & K_OPEN_WRITE)) ||
            ((flags & K_OPEN_EXCL) && !(flags & K_OPEN_CREATE)))
            return K_EINVAL;
        char path[K_PATH_MAX];
        int e = user_string(p, f->args[0], path);
        if (e)
            return e;
        UINT32 fd;
        for (fd = 3; fd < 16 && p->fd_used[fd]; ++fd)
            ;
        if (fd == 16)
            return K_E2BIG;
        e = k_vfs_open(path, &p->files[fd]);
        if (!e && (flags & K_OPEN_EXCL))
            return K_EEXIST;
        if (e == K_ENOENT && (flags & K_OPEN_CREATE))
            e = k_vfs_create(path, &p->files[fd]);
        if (e)
            return e;
        if (p->files[fd].attributes & 0x10)
            return K_EINVAL;
        /* Raw block devices are privileged; ordinary applications use files. */
        if (p->files[fd].kind == 3 && p->execution_class != K_EXEC_RING1)
            return K_EPERM;
        if (flags & K_OPEN_TRUNC) {
            e = k_vfs_put(path, NULL, 0);
            if (e)
                return e;
            e = k_vfs_open(path, &p->files[fd]);
            if (e)
                return e;
        }
        p->fd_flags[fd] = (UINT32)flags;
        p->fd_used[fd] = TRUE;
        p->fd_offset[fd] = 0;
        return fd;
    }
    case K_SYS_READ: {
        if (f->args[0] < 3 || f->args[0] >= 16 || !p->fd_used[f->args[0]])
            return K_EINVAL;
        if (f->args[2] > 4096)
            return K_E2BIG;
        if (!f->args[2])
            return 0;
        int e = k_as_check(p->as, f->args[1], f->args[2], TRUE);
        if (e)
            return e;
        UINT8 buf[4096];
        UINT64 got = 0;
        e = k_vfs_read(&p->files[f->args[0]], p->fd_offset[f->args[0]], buf, f->args[2], &got);
        if (e)
            return e;
        e = k_copy_to_user(p->as, f->args[1], buf, got);
        if (e)
            return e;
        p->fd_offset[f->args[0]] += got;
        return (INT64)got;
    }
    case K_SYS_MKDIR: {
        char path[K_PATH_MAX];
        int e = user_string(p, f->args[0], path);
        return e ? e : k_vfs_mkdir(path);
    }
    case K_SYS_SYNC:
        return k_vfs_sync();
    case K_SYS_CLOSE:
        if (f->args[0] < 3 || f->args[0] >= 16 || !p->fd_used[f->args[0]])
            return K_EINVAL;
        p->fd_used[f->args[0]] = FALSE;
        return 0;
    default:
        return syscall_extended(p, t, f);
    }
}

#define MAX_SERVICES 32
static struct {
    char name[64];
    UINT32 pid, task;
} services[MAX_SERVICES];
static k_process *process_by_pid(UINT32 pid)
{
    for (UINT32 i = 0; i < MAX_PROCESSES; ++i)
        if (processes[i].used && processes[i].id == pid)
            return &processes[i];
    return NULL;
}
static task *process_task(k_process *p)
{
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i)
        if (tasks[i].state != K_TASK_UNUSED && tasks[i].process == p)
            return &tasks[i];
    return NULL;
}
void process_resources_release(k_process *p)
{
    /* Resource leases cannot outlive the owning process instance. */
    for (UINT32 i = 0; i < MAX_SERVICES; ++i)
        if (services[i].pid == p->id)
            mem_zero(&services[i], sizeof(services[i]));
    for (UINT32 i = 0; i < partition_count; ++i)
        if (partitions[i].owner_pid == p->id)
            partitions[i].owner_pid = 0;
    if (input_owner == p->id)
        input_owner = 0;
}
BOOLEAN process_return_control(task *t)
{
    UINT64 f = k_spin_lock(&sched_lock);
    UINT32 request = t->control_request;
    if (!request) {
        k_spin_unlock(&sched_lock, f);
        return FALSE;
    }
    t->control_request = 0;
    if (request == K_CTL_KILL) {
        t->exit_code = -143;
        t->state = K_TASK_ZOMBIE;
        t->process->exit_code = -143;
        t->process->state = K_TASK_ZOMBIE;
        process_resources_release(t->process);
    } else if (request == K_CTL_STOP) {
        t->state = K_TASK_STOPPED;
        t->process->state = K_TASK_STOPPED;
    }
    t->preempt_depth = 0;
    k_spin_unlock(&sched_lock, f);
    return TRUE;
}
int k_process_query(UINT32 pid, k_process_info *out)
{
    if (!out)
        return K_EINVAL;
    for (UINT32 i = 0; i < MAX_PROCESSES; ++i) {
        int e = k_process_get(i, out);
        if (!e && out->id == pid)
            return 0;
    }
    return K_ENOENT;
}
int k_process_identity(UINT32 pid, k_identity *out)
{
    if (!out)
        return K_EINVAL;
    UINT64 irq = k_spin_lock(&sched_lock);
    k_process *p = process_by_pid(pid);
    if (!p) {
        k_spin_unlock(&sched_lock, irq);
        return K_ENOENT;
    }
    mem_zero(out, sizeof(*out));
    mem_copy(out->uuid, p->uuid, 16);
    out->pid = p->id;
    out->parent = p->parent;
    out->execution_class = p->execution_class;
    out->tls = p->tls;
    task *t = process_task(p);
    out->cpu = t ? t->cpu : K_CPU_AUTO;
    k_spin_unlock(&sched_lock, irq);
    return 0;
}
static int process_control_locked(UINT32 pid, UINT32 op)
{
    if (op < K_CTL_KILL || op > K_CTL_CONTINUE)
        return K_EINVAL;
    UINT64 irq = k_spin_lock(&sched_lock);
    k_process *p = process_by_pid(pid);
    task *t = p ? process_task(p) : NULL;
    if (!t || t->state == K_TASK_ZOMBIE) {
        k_spin_unlock(&sched_lock, irq);
        return K_ENOENT;
    }
    if (op == K_CTL_CONTINUE) {
        if (t->control_request != K_CTL_KILL)
            t->control_request = 0;
        if (t->state == K_TASK_STOPPED) {
            task_ready(t);
            p->state = t->state;
        }
    } else {
        if (op == K_CTL_KILL || t->control_request != K_CTL_KILL)
            t->control_request = op;
        /* Control requests do not abandon an in-progress blocked kernel mutex operation. */
        if (t->state == K_TASK_STOPPED || t->state == K_TASK_SLEEPING) {
            task_ready(t);
            p->state = t->state;
        }
    }
    __atomic_store_n(&cpus[t->cpu].work_pending, 1, __ATOMIC_RELEASE);
    k_spin_unlock(&sched_lock, irq);
    return 0;
}
int k_process_control(UINT32 pid, UINT32 op)
{
    int e = k_mutex_lock(&process_mutex);
    if (e)
        return e;
    e = process_control_locked(pid, op);
    k_mutex_unlock(&process_mutex);
    return e;
}
int k_process_reap_pid(UINT32 pid, k_process_info *out)
{
    int e = k_mutex_lock(&process_mutex);
    if (e)
        return e;
    k_process *p = process_by_pid(pid);
    if (!p)
        e = K_ENOENT;
    else {
        if (out)
            e = k_process_query(pid, out);
        if (!e)
            e = process_destroy_locked(p);
    }
    k_mutex_unlock(&process_mutex);
    return e;
}
int k_process_launch(const char *path, const char *arguments, UINT32 cls, UINT32 cpu, UINT32 parent,
                     UINT32 *pid)
{
    if (!path || !arguments || !pid || (cls != K_EXEC_RING1 && cls != K_EXEC_RING3) ||
        str_len(arguments) >= K_PROCESS_ARGS_MAX)
        return K_EINVAL;
    int e = k_mutex_lock(&process_mutex);
    if (e)
        return e;
    task *caller = current_task();
    k_process *actor = caller ? caller->process : NULL;
    if (actor &&
        (parent != actor->id || (cls == K_EXEC_RING1 && actor->execution_class != K_EXEC_RING1))) {
        e = K_EPERM;
        goto done;
    }
    k_file file;
    e = k_vfs_open(path, &file);
    if (e)
        goto done;
    if (file.kind == 3 || (file.attributes & 0x10) || file.size < sizeof(rief_header_t) ||
        file.size > 4 * 1024 * 1024) {
        e = K_ENOEXEC;
        goto done;
    }
    UINT32 pages = (UINT32)((file.size + 4095) / 4096);
    UINT64 image = k_pmm_alloc_pages(pages, 0), got = 0;
    if (!image) {
        e = K_ENOMEM;
        goto done;
    }
    e = k_vfs_read(&file, 0, (void *)(UINTN)image, file.size, &got);
    if (!e && got != file.size)
        e = K_EIO;
    k_process *p = NULL;
    if (!e) {
        p = process_create_locked(path);
        if (!p)
            e = K_ENOMEM;
    }
    if (!e) {
        p->parent = parent;
        p->execution_class = cls;
        str_copy(p->arguments, arguments, sizeof(p->arguments));
        e = rief_load_locked(p, (void *)(UINTN)image, file.size, NULL, 0);
    }
    k_pmm_free_pages(image, pages);
    if (!e) {
        UINT32 tid;
        e = process_start_locked(p, cpu, &tid);
        if (!e)
            *pid = p->id;
    }
    if (e && p)
        (void)process_destroy_locked(p);
done:
    k_mutex_unlock(&process_mutex);
    return e;
}
static int service_name(const char *name)
{
    UINTN n = str_len(name);
    if (!n || n >= 64)
        return K_EINVAL;
    for (UINTN i = 0; i < n; ++i)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '_' || name[i] == '-' ||
              name[i] == '.'))
            return K_EINVAL;
    return 0;
}
static int grant_locked(k_process *p, UINT32 tid)
{
    for (UINT32 i = 0; i < p->grant_count; ++i)
        if (p->grants[i] == tid)
            return 0;
    /* Prune dead service endpoints so restarts do not exhaust IPC grants. */
    for (UINT32 i = 0; i < p->grant_count;) {
        BOOLEAN live = FALSE;
        for (UINT32 j = 0; j < K_MAX_TASKS; ++j)
            if (tasks[j].id == p->grants[i] && tasks[j].state != K_TASK_UNUSED &&
                tasks[j].state != K_TASK_ZOMBIE)
                live = TRUE;
        if (live)
            ++i;
        else
            p->grants[i] = p->grants[--p->grant_count];
    }
    if (p->grant_count == ARRAY_LEN(p->grants))
        return K_E2BIG;
    p->grants[p->grant_count++] = tid;
    return 0;
}
static int service_register(k_process *p, const char *name)
{
    if (p->execution_class != K_EXEC_RING1)
        return K_EPERM;
    int e = service_name(name);
    if (e)
        return e;
    UINT64 irq = k_spin_lock(&sched_lock);
    int slot = -1;
    for (UINT32 i = 0; i < MAX_SERVICES; ++i) {
        k_process *owner = process_by_pid(services[i].pid);
        if (!owner || owner->state == K_TASK_ZOMBIE)
            mem_zero(&services[i], sizeof(services[i]));
        if (services[i].pid && str_eq(services[i].name, name)) {
            e = services[i].pid == p->id ? 0 : K_EEXIST;
            goto done;
        }
        if (!services[i].pid && slot < 0)
            slot = (int)i;
    }
    if (slot < 0) {
        e = K_E2BIG;
        goto done;
    }
    str_copy(services[slot].name, name, 64);
    services[slot].pid = p->id;
    services[slot].task = p->task_id;
done:
    k_spin_unlock(&sched_lock, irq);
    return e;
}
int k_service_lookup(const char *name, UINT32 *tid)
{
    if (!name || !tid)
        return K_EINVAL;
    int e = service_name(name);
    if (e)
        return e;
    task *caller = current_task();
    if (!caller)
        return K_EPERM;
    UINT64 irq = k_spin_lock(&sched_lock);
    e = K_ENOENT;
    for (UINT32 i = 0; i < MAX_SERVICES; ++i)
        if (services[i].pid && str_eq(services[i].name, name)) {
            k_process *p = process_by_pid(services[i].pid);
            if (!p || p->state == K_TASK_ZOMBIE)
                break;
            /* Reciprocal service IPC grants are installed transactionally. */
            BOOLEAN already = FALSE;
            for (UINT32 j = 0; j < p->grant_count; ++j)
                if (p->grants[j] == caller->id)
                    already = TRUE;
            e = grant_locked(p, caller->id);
            if (!e && caller->process)
                e = grant_locked(caller->process, services[i].task);
            if (e) {
                if (!already)
                    for (UINT32 j = 0; j < p->grant_count; ++j)
                        if (p->grants[j] == caller->id) {
                            p->grants[j] = p->grants[--p->grant_count];
                            break;
                        }
                break;
            }
            *tid = services[i].task;
            break;
        }
    k_spin_unlock(&sched_lock, irq);
    return e;
}
BOOLEAN k_input_claimed(void)
{
    UINT64 irq = k_spin_lock(&sched_lock);
    k_process *p = process_by_pid(input_owner);
    BOOLEAN claimed = p && p->state != K_TASK_ZOMBIE;
    if (!claimed)
        input_owner = 0;
    k_spin_unlock(&sched_lock, irq);
    return claimed;
}
int k_partition_get(UINT32 index, k_partition_info *out)
{
    if (!out || index >= partition_count)
        return K_ENOENT;
    UINT64 irq = k_spin_lock(&sched_lock);
    *out = partitions[index];
    k_spin_unlock(&sched_lock, irq);
    return 0;
}

static INT64 admin_operation(k_process *p, const k_admin_request *r)
{
    if (p->execution_class != K_EXEC_RING1 || memcmp(r->caller_uuid, p->uuid, 16))
        return K_EPERM;
    if (r->version != 1 || r->size != sizeof(*r) || r->flags)
        return K_EINVAL;
    if (r->operation >= K_ADMIN_MEM_READ && r->operation <= K_ADMIN_MAP_AT) {
        int e = k_mutex_lock(&process_mutex);
        if (e)
            return e;
        k_process *target = process_by_pid(r->target_pid);
        if (!target || memcmp(target->uuid, r->target_uuid, 16)) {
            e = K_ENOENT;
            goto memory_done;
        }
        UINT64 irq = k_spin_lock(&sched_lock);
        task *t = process_task(target);
        BOOLEAN safe = (target == p || !t || t->state == K_TASK_STOPPED);
        /* STOPPED may publish before the old CPU has left the target's kernel stack. */
        if (t && target != p)
            for (UINT32 c = 0; c < platform.discovered_cpus; ++c)
                if (cpus[c].current == t ||
                    __atomic_load_n(&cpus[c].stack_owner, __ATOMIC_ACQUIRE) == t)
                    safe = FALSE;
        k_spin_unlock(&sched_lock, irq);
        if (!safe) {
            e = K_EBUSY;
            goto memory_done;
        }
        if (r->operation == K_ADMIN_MAP_AT) {
            if (r->value > 7)
                e = K_EINVAL;
            else
                e = arch_as_map_at(target->as, r->address, r->length, (UINT32)r->value);
            goto memory_done;
        }
        if (!r->length || r->length > 4096) {
            e = K_EINVAL;
            goto memory_done;
        }
        UINT8 buf[4096];
        BOOLEAN write = r->operation == K_ADMIN_MEM_WRITE;
        e = k_as_check(p->as, r->buffer, r->length, !write);
        if (e)
            goto memory_done;
        e = k_as_check(target->as, r->address, r->length, write);
        if (e)
            goto memory_done;
        if (write) {
            e = k_copy_from_user(p->as, buf, r->buffer, r->length);
            if (!e)
                e = k_copy_to_user(target->as, r->address, buf, r->length);
        } else {
            e = k_copy_from_user(target->as, buf, r->address, r->length);
            if (!e)
                e = k_copy_to_user(p->as, r->buffer, buf, r->length);
        }
    memory_done:
        k_mutex_unlock(&process_mutex);
        return e;
    }
    if (r->operation == K_ADMIN_INPUT_CLAIM || r->operation == K_ADMIN_INPUT_RELEASE ||
        r->operation == K_ADMIN_INPUT_READ) {
        if (k_cpu_id() != 0)
            return K_EPERM;
        if (r->operation == K_ADMIN_INPUT_CLAIM) {
            UINT64 irq = k_spin_lock(&sched_lock);
            int e = input_owner && input_owner != p->id ? K_EBUSY : 0;
            if (!e)
                input_owner = p->id;
            k_spin_unlock(&sched_lock, irq);
            return e;
        }
        if (input_owner != p->id)
            return K_EPERM;
        if (r->operation == K_ADMIN_INPUT_RELEASE) {
            input_owner = 0;
            return 0;
        }
        if (r->length != sizeof(k_input_report))
            return K_EINVAL;
        int e = k_as_check(p->as, r->buffer, sizeof(k_input_report), TRUE);
        if (e)
            return e;
        k_input_report report;
        k_input_pump();
        e = k_input_read(&report);
        return e ? e : k_copy_to_user(p->as, r->buffer, &report, sizeof(report));
    }
    if (r->operation < K_ADMIN_DISK_READ || r->operation > K_ADMIN_PART_RELEASE)
        return K_ENOTSUP;
    int e = k_mutex_lock(&process_mutex);
    if (e)
        return e;
    UINT32 disk_id = 0;
    UINT64 start = 0, limit = 0;
    k_partition_info *part = NULL;
    if (r->operation == K_ADMIN_DISK_READ) {
        if (r->resource >= k_disk_count()) {
            e = K_ENOENT;
            goto disk_done;
        }
        disk_id = r->resource;
        limit = disks[disk_id].info.sectors;
    } else {
        if (r->resource >= partition_count) {
            e = K_ENOENT;
            goto disk_done;
        }
        part = &partitions[r->resource];
        disk_id = part->disk;
        start = part->start;
        limit = part->sectors;
        if (r->operation == K_ADMIN_PART_CLAIM) {
            if (part->read_only || part->mounted || part->failed) {
                e = K_EROFS;
                goto disk_done;
            }
            if (part->owner_pid && part->owner_pid != p->id) {
                e = K_EBUSY;
                goto disk_done;
            }
            /* Publish only validated, non-overlapping partitions before SMP. */
            part->owner_pid = p->id;
            e = 0;
            goto disk_done;
        }
        if (part->owner_pid != p->id) {
            e = K_EPERM;
            goto disk_done;
        }
        if (r->operation == K_ADMIN_PART_RELEASE) {
            part->owner_pid = 0;
            e = 0;
            goto disk_done;
        }
        if (part->failed) {
            e = K_EROFS;
            goto disk_done;
        }
    }
    UINT32 sector = disks[disk_id].info.sector_size;
    if (!r->length || r->length > 4096 || r->length % sector || r->address >= limit ||
        r->length / sector > limit - r->address) {
        e = K_EINVAL;
        goto disk_done;
    }
    UINT8 buffer[4096];
    BOOLEAN write = r->operation == K_ADMIN_PART_WRITE;
    e = k_as_check(p->as, r->buffer, r->length, !write);
    if (e)
        goto disk_done;
    if (write) {
        e = k_copy_from_user(p->as, buffer, r->buffer, r->length);
        if (e)
            goto disk_done;
        e = disk_write_fs(disk_id, start + r->address, (UINT32)(r->length / sector), buffer,
                          r->length);
        if (!e)
            e = disk_flush(disk_id);
        if (e)
            part->failed = 1;
    } else {
        e = k_disk_read(disk_id, start + r->address, (UINT32)(r->length / sector), buffer,
                        r->length);
        if (!e)
            e = k_copy_to_user(p->as, r->buffer, buffer, r->length);
    }
disk_done:
    k_mutex_unlock(&process_mutex);
    return e;
}
static int user_args(k_process *p, UINT64 address, char out[K_PROCESS_ARGS_MAX])
{
    if (!address) {
        out[0] = 0;
        return 0;
    }
    for (UINT32 i = 0; i < K_PROCESS_ARGS_MAX; ++i) {
        if (address > K_USER_CANON_LIMIT - 1 - i ||
            k_copy_from_user(p->as, &out[i], address + i, 1))
            return K_EFAULT;
        if (!out[i])
            return 0;
    }
    return K_E2BIG;
}
INT64 syscall_extended(k_process *p, task *t, const k_syscall_request *f)
{
    if (f->number >= K_SYS_NETINFO && f->number <= K_SYS_WIFISCAN)
        return net_syscall(p, f);
    switch (f->number) {
    case K_SYS_SPAWN: {
        char path[K_PATH_MAX], args[K_PROCESS_ARGS_MAX];
        int e = user_string(p, f->args[0], path);
        if (!e)
            e = user_args(p, f->args[1], args);
        if (e)
            return e;
        if (f->args[2] != K_EXEC_RING1 && f->args[2] != K_EXEC_RING3)
            return K_EINVAL;
        if (f->args[3] != ~0ULL && f->args[3] > K_CPU_AUTO)
            return K_EINVAL;
        UINT32 pid;
        e = k_process_launch(path, args, (UINT32)f->args[2], (UINT32)f->args[3], p->id, &pid);
        return e ? e : (INT64)pid;
    }
    case K_SYS_PROCINFO: {
        if (f->args[0] >= MAX_PROCESSES)
            return K_EINVAL;
        int e = k_as_check(p->as, f->args[1], sizeof(k_process_info), TRUE);
        if (e)
            return e;
        k_process_info info;
        e = k_process_get((UINT32)f->args[0], &info);
        return e ? e : k_copy_to_user(p->as, f->args[1], &info, sizeof(info));
    }
    case K_SYS_WAITPID: {
        if (!f->args[0] || f->args[0] > 0xffffffff || f->args[2] & ~3ULL)
            return K_EINVAL;
        if (f->args[1] && k_as_check(p->as, f->args[1], sizeof(k_process_info), TRUE))
            return K_EFAULT;
        for (;;) {
            int e = k_mutex_lock(&process_mutex);
            if (e)
                return e;
            k_process *target = process_by_pid((UINT32)f->args[0]);
            if (!target || target == p)
                e = K_ENOENT;
            else if (target->parent != p->id && p->execution_class != K_EXEC_RING1)
                e = K_EPERM;
            else if (target->state != K_TASK_ZOMBIE)
                e = K_EAGAIN;
            else {
                k_process_info info;
                e = k_process_query(target->id, &info);
                if (!e && f->args[1])
                    e = k_copy_to_user(p->as, f->args[1], &info, sizeof(info));
                if (!e && (f->args[2] & K_WAIT_REAP))
                    e = process_destroy_locked(target);
                if (e == K_EBUSY)
                    e = K_EAGAIN;
            }
            k_mutex_unlock(&process_mutex);
            if (e != K_EAGAIN || (f->args[2] & K_WAIT_NOHANG) || t->control_request)
                return e;
            k_sleep(1);
        }
    }
    case K_SYS_CONTROL: {
        if (f->args[0] > 0xffffffff || f->args[1] > 0xffffffff)
            return K_EINVAL;
        int e = k_mutex_lock(&process_mutex);
        if (e)
            return e;
        k_process *target = process_by_pid((UINT32)f->args[0]);
        if (!target)
            e = K_ENOENT;
        else if (target != p && target->parent != p->id && p->execution_class != K_EXEC_RING1)
            e = K_EPERM;
        else
            e = process_control_locked(target->id, (UINT32)f->args[1]);
        k_mutex_unlock(&process_mutex);
        return e;
    }
    case K_SYS_ADMIN: {
        k_admin_request r;
        int e = k_copy_from_user(p->as, &r, f->args[0], sizeof(r));
        return e ? e : admin_operation(p, &r);
    }
    case K_SYS_IDENTITY: {
        k_identity id;
        int e = k_process_identity(p->id, &id);
        return e ? e : k_copy_to_user(p->as, f->args[0], &id, sizeof(id));
    }
    case K_SYS_SERVICE_REGISTER:
    case K_SYS_SERVICE_LOOKUP: {
        char name[K_PATH_MAX];
        int e = user_string(p, f->args[0], name);
        if (e)
            return e;
        if (f->number == K_SYS_SERVICE_REGISTER)
            return service_register(p, name);
        UINT32 tid;
        e = k_service_lookup(name, &tid);
        return e ? e : (INT64)tid;
    }
    case K_SYS_SERVICE_REPLY: {
        if (f->args[2] > K_IPC_BYTES || f->args[0] > 0xffffffff)
            return K_EINVAL;
        if (p->execution_class != K_EXEC_RING1)
            return K_EPERM;
        UINT8 bytes[K_IPC_BYTES];
        int e = k_copy_from_user(p->as, bytes, f->args[1], f->args[2]);
        return e ? e : k_ipc_send((UINT32)f->args[0], bytes, (UINT32)f->args[2]);
    }
    case K_SYS_GETCPU:
        return k_cpu_id();
    case K_SYS_TLSBASE:
        return (INT64)p->tls;
    case K_SYS_GETARGS: {
        UINTN n = str_len(p->arguments) + 1;
        if (f->args[1] < n)
            return K_E2BIG;
        int e = k_copy_to_user(p->as, f->args[0], p->arguments, n);
        return e ? e : (INT64)(n - 1);
    }
    case K_SYS_SEEK: {
        UINT64 fd = f->args[0];
        if (fd < 3 || fd >= 16 || !p->fd_used[fd] || f->args[2] > 2)
            return K_EINVAL;
        if (f->args[2] == 2) {
            int e = k_vfs_stat(&p->files[fd]);
            if (e)
                return e;
        }
        UINT64 base = f->args[2] == 0 ? 0 : f->args[2] == 1 ? p->fd_offset[fd] : p->files[fd].size;
        if (base > 0x7fffffffffffffffULL)
            return K_EINVAL;
        INT64 off = (INT64)f->args[1];
        if ((off < 0 && 0 - (UINT64)off > base) ||
            (off >= 0 && (UINT64)off > 0x7fffffffffffffffULL - base))
            return K_EINVAL;
        p->fd_offset[fd] = base + (UINT64)off;
        return (INT64)p->fd_offset[fd];
    }
    case K_SYS_MAP: {
        if (f->args[2] > 7)
            return K_EINVAL;
        UINT64 address;
        int e = k_as_alloc(p->as, f->args[0], f->args[1], (UINT32)f->args[2], &address);
        return e ? e : (INT64)address;
    }
    case K_SYS_DISKINFO: {
        if (p->execution_class != K_EXEC_RING1)
            return K_EPERM;
        if (f->args[0] > 0xffffffff)
            return K_EINVAL;
        k_disk_info info;
        int e = k_disk_get((UINT32)f->args[0], &info);
        return e ? e : k_copy_to_user(p->as, f->args[1], &info, sizeof(info));
    }
    case K_SYS_PARTITIONINFO: {
        if (p->execution_class != K_EXEC_RING1)
            return K_EPERM;
        if (f->args[0] > 0xffffffff)
            return K_EINVAL;
        k_partition_info info;
        int e = k_partition_get((UINT32)f->args[0], &info);
        return e ? e : k_copy_to_user(p->as, f->args[1], &info, sizeof(info));
    }
    default:
        return K_ENOTSUP;
    }
}
