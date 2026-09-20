// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"

cpu_state cpus[K_MAX_CPUS];
task tasks[K_MAX_TASKS];
k_spinlock sched_lock = K_SPINLOCK_INIT;
BOOLEAN scheduler_ready;
static UINT32 next_task_id = 1;
UINT64 global_ticks;

void mem_zero(void *p, UINTN n)
{
    UINT8 *d = p;
    while (n--)
        *d++ = 0;
}
void mem_copy(void *d, const void *s, UINTN n)
{
    UINT8 *a = d;
    const UINT8 *b = s;
    while (n--)
        *a++ = *b++;
}
/* GCC may emit these in a freestanding PE image. */
void *memset(void *p, int v, size_t n)
{
    UINT8 *d = p;
    while (n--)
        *d++ = (UINT8)v;
    return p;
}
void *memcpy(void *d, const void *s, size_t n)
{
    mem_copy(d, s, n);
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    UINT8 *a = d;
    const UINT8 *b = s;
    if ((UINTN)a < (UINTN)b)
        return memcpy(d, s, n);
    while (n) {
        --n;
        a[n] = b[n];
    }
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const UINT8 *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        ++x;
        ++y;
    }
    return 0;
}
UINTN str_len(const char *s)
{
    UINTN n = 0;
    while (s[n])
        ++n;
    return n;
}
BOOLEAN str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}
void str_copy(char *d, const char *s, UINTN cap)
{
    if (!cap)
        return;
    UINTN i = 0;
    while (i + 1 < cap && s[i]) {
        d[i] = s[i];
        ++i;
    }
    d[i] = 0;
}
BOOLEAN span_ok(UINT64 off, UINT64 n, UINT64 size) { return off <= size && n <= size - off; }

BOOLEAN power2(UINT64 n) { return n && !(n & (n - 1)); }
UINT16 rd16(const void *p)
{
    const UINT8 *b = p;
    return b[0] | ((UINT16)b[1] << 8);
}
UINT32 rd32(const void *p)
{
    const UINT8 *b = p;
    return rd16(b) | ((UINT32)rd16(b + 2) << 16);
}
UINT64 rd64(const void *p)
{
    const UINT8 *b = p;
    return rd32(b) | ((UINT64)rd32(b + 4) << 32);
}
void wr16(void *p, UINT16 v)
{
    UINT8 *b = p;
    b[0] = (UINT8)v;
    b[1] = (UINT8)(v >> 8);
}
void wr32(void *p, UINT32 v)
{
    UINT8 *b = p;
    for (int i = 0; i < 4; ++i)
        b[i] = (UINT8)(v >> (i * 8));
}
void wr64(void *p, UINT64 v)
{
    UINT8 *b = p;
    for (int i = 0; i < 8; ++i)
        b[i] = (UINT8)(v >> (i * 8));
}

UINT64 k_spin_lock(k_spinlock *l)
{
    UINT64 f = k_irq_save();
    while (__atomic_exchange_n(&l->value, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->value, __ATOMIC_RELAXED))
            arch_pause();
    }
    return f;
}
void k_spin_unlock(k_spinlock *l, UINT64 f)
{
    __atomic_store_n(&l->value, 0, __ATOMIC_RELEASE);
    k_irq_restore(f);
}
const char *k_strerror(int e)
{
    static const char *names[] = {"ok",
                                  "invalid argument",
                                  "out of memory",
                                  "not found",
                                  "busy",
                                  "invalid memory",
                                  "unsupported",
                                  "I/O error",
                                  "timeout",
                                  "read-only",
                                  "try again",
                                  "permission denied",
                                  "limit exceeded",
                                  "invalid RIEF",
                                  "deadlock",
                                  "already exists",
                                  "no space left on volume",
                                  "network down",
                                  "no route",
                                  "connection refused",
                                  "connection reset",
                                  "address in use",
                                  "message too large",
                                  "connection in progress",
                                  "canceled"};
    return e <= 0 && (UINT32)(-e) < ARRAY_LEN(names) ? names[-e] : "unknown error";
}

__attribute__((noreturn)) void kernel_halt(const char *s)
{
    arch_irq_disable();
    console_panic_mode();
    con_print("\nHALT: ");
    con_print(s);
    con_print("\n");
    arch_stop();
}

static void wake_object(void *);

task *current_task(void) { return cpus[k_cpu_id()].current; }
UINT32 k_task_id(void)
{
    task *t = current_task();
    return t ? t->id : 0;
}
void k_preempt_disable(void)
{
    UINT64 f = k_irq_save();
    task *t = current_task();
    if (t)
        ++t->preempt_depth;
    k_irq_restore(f);
}
void k_preempt_enable(void)
{
    UINT64 f = k_irq_save();
    task *t = current_task();
    if (t && t->preempt_depth)
        --t->preempt_depth;
    k_irq_restore(f);
}
static UINT64 cpu_floor(UINT32 cpu)
{
    UINT64 min = ~0ULL;
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i) {
        task *t = &tasks[i];
        if (t->cpu == cpu && !t->idle && (t->state == K_TASK_READY || t->state == K_TASK_RUNNING) &&
            t->vruntime < min)
            min = t->vruntime;
    }
    if (min != ~0ULL && min > cpus[cpu].fair_floor)
        cpus[cpu].fair_floor = min;
    return cpus[cpu].fair_floor;
}
void task_ready(task *t)
{
    UINT64 floor = cpu_floor(t->cpu);
    if (t->vruntime < floor)
        t->vruntime = floor;
    t->state = K_TASK_READY;
    t->wait_object = NULL;
    __atomic_store_n(&cpus[t->cpu].work_pending, 1, __ATOMIC_RELEASE);
}
__attribute__((noreturn)) static void task_bootstrap(void)
{
    task *t = current_task();
    if (!t || !t->entry)
        kernel_halt("bad task entry");
    t->entry(t->arg);
    k_task_exit(0);
}
void idle_entry(void *unused)
{
    (void)unused;
    for (;;)
        arch_wait_interrupt();
}
task *new_task(const char *name, k_task_entry entry, void *arg, UINT32 weight, UINT32 cpu,
               BOOLEAN idle)
{
    if (!next_task_id)
        return NULL;
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i)
        if (tasks[i].state == K_TASK_UNUSED) {
            task *t = &tasks[i];
            mem_zero(t, sizeof(*t));
            UINT64 stack = k_pmm_alloc_pages(STACK_PAGES, 0);
            if (!stack)
                return NULL;
            t->id = next_task_id++;
            t->cpu = cpu;
            t->weight = weight;
            t->stack = stack;
            t->stack_top = stack + STACK_PAGES * 4096;
            t->entry = entry;
            t->arg = arg;
            t->idle = idle;
            arch_task_context_init(t, task_bootstrap);
            str_copy(t->name, name, sizeof(t->name));
            t->vruntime = cpu_floor(cpu);
            t->state = K_TASK_READY;
            if (!idle)
                __atomic_store_n(&cpus[cpu].work_pending, 1, __ATOMIC_RELEASE);
            return t;
        }
    return NULL;
}
int k_scheduler_init(void)
{
    if (!interrupts_ready || scheduler_ready)
        return K_EINVAL;
    arch_fpu_init();
    task *boot = &tasks[0];
    boot->id = next_task_id++;
    boot->cpu = 0;
    boot->weight = 1024;
    boot->state = K_TASK_RUNNING;
    str_copy(boot->name, "boot", sizeof(boot->name));
    arch_task_state_init(boot);
    cpus[0].current = boot;
    cpus[0].stack_owner = boot;
    task *idle = new_task("idle", idle_entry, NULL, 1, 0, TRUE);
    if (!idle)
        return K_ENOMEM;
    cpus[0].idle = idle;
    scheduler_ready = TRUE;
    return 0;
}
int k_task_create(const char *name, k_task_entry entry, void *arg, UINT32 weight, UINT32 cpu,
                  UINT32 *id)
{
    if (!scheduler_ready || !name || !entry || !id || !weight || weight > 4096 ||
        cpu >= platform.discovered_cpus || !__atomic_load_n(&cpus[cpu].online, __ATOMIC_ACQUIRE))
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    task *t = new_task(name, entry, arg, weight, cpu, FALSE);
    if (t)
        *id = t->id;
    k_spin_unlock(&sched_lock, f);
    return t ? 0 : K_ENOMEM;
}
irq_frame *kernel_schedule(irq_frame *frame, BOOLEAN forced)
{
    UINT32 c = k_cpu_id();
    cpu_state *cpu = &cpus[c];
    task *old = cpu->current;
    if (!forced && old && old->idle) {
        ++cpu->idle_ticks;
        /* Idle CPUs avoid the global scheduler lock until work_pending is published. */
        if (!__atomic_load_n(&cpu->work_pending, __ATOMIC_ACQUIRE)) {
            ++old->runtime;
            return frame;
        }
    }
    UINT64 f = k_spin_lock(&sched_lock);
    if (!old) {
        k_spin_unlock(&sched_lock, f);
        return frame;
    }
    old->frame = frame;
    if (forced && old->state == K_TASK_RUNNING && !old->idle && !old->preempt_depth)
        old->vruntime += (1024ULL * 1024 + old->weight - 1) / old->weight;
    if (!forced) {
        ++old->runtime;
        if (!old->idle)
            old->vruntime += (1024ULL * 1024 + old->weight - 1) / old->weight;
        if (cpu->slice)
            --cpu->slice;
    }
    UINT64 now = k_ticks();
    BOOLEAN sleepers = FALSE;
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i) {
        task *t = &tasks[i];
        if (t->cpu == c && t->state == K_TASK_SLEEPING) {
            if (now >= t->wake)
                task_ready(t);
            else
                sleepers = TRUE;
        }
    }
    if (old->state == K_TASK_RUNNING &&
        (old->preempt_depth || (!forced && cpu->slice && !old->idle))) {
        k_spin_unlock(&sched_lock, f);
        return frame;
    }
    if (old->state == K_TASK_RUNNING) {
        old->state = K_TASK_READY;
        if (old->process)
            old->process->state = K_TASK_READY;
    }
    task *best = NULL;
    UINT64 weights = 0;
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i) {
        task *t = &tasks[i];
        if (t->cpu != c || t->state != K_TASK_READY || t->idle)
            continue;
        weights += t->weight;
        if (!best || t->vruntime < best->vruntime ||
            (t->vruntime == best->vruntime && t->id < best->id))
            best = t;
    }
    if (!best)
        best = cpu->idle;
    __atomic_store_n(&cpu->work_pending, !best->idle || sleepers, __ATOMIC_RELEASE);
    best->state = K_TASK_RUNNING;
    if (best->process)
        best->process->state = K_TASK_RUNNING;
    (void)cpu_floor(c);
    UINT64 slice = weights ? (8ULL * best->weight + weights - 1) / weights : 1;
    cpu->slice = (UINT32)MIN(slice, 8ULL);
    if (!cpu->slice)
        cpu->slice = 1;
    if (best != old) {
        arch_task_switch(cpu, old, best);
        ++cpu->switches;
        ++best->switches;
    }
    irq_frame *result = best->frame;
    k_spin_unlock(&sched_lock, f);
    return result;
}
void k_yield(void)
{
    if (scheduler_ready)
        arch_reschedule();
}
void k_sleep(UINT64 ms)
{
    if (!scheduler_ready || !ms) {
        k_yield();
        return;
    }
    task *t = current_task();
    if (!t || t->preempt_depth)
        return;
    /* Round untrusted u64 sizes without overflow. */
    if (ms > 86400000ULL)
        ms = 86400000ULL;
    UINT64 ticks = (ms * timer_hz + 999) / 1000;
    UINT64 f = k_spin_lock(&sched_lock);
    t->wake = k_ticks() + ticks;
    t->state = K_TASK_SLEEPING;
    k_spin_unlock(&sched_lock, f);
    k_yield();
}
__attribute__((noreturn)) void k_task_exit(INT32 code)
{
    UINT64 f = k_spin_lock(&sched_lock);
    task *t = current_task();
    if (!t || t->idle || t->id == 1) {
        k_spin_unlock(&sched_lock, f);
        kernel_halt("cannot exit shell/idle");
    }
    t->exit_code = code;
    t->state = K_TASK_ZOMBIE;
    t->preempt_depth = 0;
    if (t->process) {
        t->process->exit_code = code;
        t->process->state = K_TASK_ZOMBIE;
    }
    k_spin_unlock(&sched_lock, f);
    k_yield();
    kernel_halt("zombie resumed");
}
void free_task(task *t)
{
    if (t->stack)
        k_pmm_free_pages(t->stack, STACK_PAGES);
    mem_zero(t, sizeof(*t));
}
int k_task_reap(UINT32 id, INT32 *exit)
{
    UINT64 f = k_spin_lock(&sched_lock);
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i)
        if (tasks[i].state != K_TASK_UNUSED && tasks[i].id == id) {
            task *t = &tasks[i];
            if (t->state != K_TASK_ZOMBIE) {
                k_spin_unlock(&sched_lock, f);
                return K_EBUSY;
            }
            for (UINT32 c = 0; c < platform.discovered_cpus; ++c)
                if (cpus[c].current == t ||
                    __atomic_load_n(&cpus[c].stack_owner, __ATOMIC_ACQUIRE) == t) {
                    k_spin_unlock(&sched_lock, f);
                    return K_EBUSY;
                }
            if (t->process) {
                k_spin_unlock(&sched_lock, f);
                return K_EPERM;
            }
            if (exit)
                *exit = t->exit_code;
            free_task(t);
            k_spin_unlock(&sched_lock, f);
            return 0;
        }
    k_spin_unlock(&sched_lock, f);
    return K_ENOENT;
}
static void task_snapshot(const task *t, k_task_info *out)
{
    *out = (k_task_info){
        t->id,      t->cpu,      t->state,    t->weight,    t->process ? t->process->id : 0,
        t->runtime, t->vruntime, t->switches, t->exit_code, {0}};
    str_copy(out->name, t->name, sizeof(out->name));
}
int k_task_get(UINT32 i, k_task_info *out)
{
    if (i >= K_MAX_TASKS || !out)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    int e = tasks[i].state == K_TASK_UNUSED ? K_ENOENT : 0;
    if (!e)
        task_snapshot(&tasks[i], out);
    k_spin_unlock(&sched_lock, f);
    return e;
}
int k_task_query(UINT32 id, k_task_info *out)
{
    if (!id || !out)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    int e = K_ENOENT;
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i)
        if (tasks[i].id == id && tasks[i].state != K_TASK_UNUSED) {
            task_snapshot(&tasks[i], out);
            e = 0;
            break;
        }
    k_spin_unlock(&sched_lock, f);
    return e;
}
/* Sleep and wake share sched_lock so unlock-before-sleep cannot lose a wakeup. */
static void wake_object(void *obj)
{
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i)
        if (tasks[i].state == K_TASK_BLOCKED && tasks[i].wait_object == obj)
            task_ready(&tasks[i]);
}
int k_mutex_trylock(k_mutex *m)
{
    if (!m || !current_task())
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    UINT32 me = k_task_id();
    int e = m->owner == me ? K_EDEADLK : m->owner ? K_EBUSY : 0;
    if (!e)
        m->owner = me;
    k_spin_unlock(&sched_lock, f);
    return e;
}
int k_mutex_lock(k_mutex *m)
{
    if (!m || !current_task())
        return K_EINVAL;
    task *t = current_task();
    for (;;) {
        UINT64 f = k_spin_lock(&sched_lock);
        if (!m->owner) {
            m->owner = t->id;
            k_spin_unlock(&sched_lock, f);
            return 0;
        }
        if (m->owner == t->id || t->preempt_depth) {
            k_spin_unlock(&sched_lock, f);
            return K_EDEADLK;
        }
        t->wait_object = m;
        t->state = K_TASK_BLOCKED;
        k_spin_unlock(&sched_lock, f);
        k_yield();
    }
}
int k_mutex_unlock(k_mutex *m)
{
    if (!m)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    if (!m->owner || m->owner != k_task_id()) {
        k_spin_unlock(&sched_lock, f);
        return K_EPERM;
    }
    m->owner = 0;
    wake_object(m);
    k_spin_unlock(&sched_lock, f);
    return 0;
}
void k_sem_init(k_semaphore *s, UINT32 n)
{
    if (s)
        s->count = n;
}
int k_sem_trywait(k_semaphore *s)
{
    if (!s)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    int e = s->count ? 0 : K_EAGAIN;
    if (!e)
        --s->count;
    k_spin_unlock(&sched_lock, f);
    return e;
}
int k_sem_wait(k_semaphore *s)
{
    if (!s || !current_task())
        return K_EINVAL;
    task *t = current_task();
    for (;;) {
        UINT64 f = k_spin_lock(&sched_lock);
        if (s->count) {
            --s->count;
            k_spin_unlock(&sched_lock, f);
            return 0;
        }
        if (t->preempt_depth) {
            k_spin_unlock(&sched_lock, f);
            return K_EDEADLK;
        }
        t->wait_object = s;
        t->state = K_TASK_BLOCKED;
        k_spin_unlock(&sched_lock, f);
        k_yield();
    }
}
void k_sem_post(k_semaphore *s)
{
    if (!s)
        return;
    UINT64 f = k_spin_lock(&sched_lock);
    if (s->count != 0xffffffff)
        ++s->count;
    wake_object(s);
    k_spin_unlock(&sched_lock, f);
}
int k_ipc_send(UINT32 target, const void *data, UINT32 n)
{
    task *sender = current_task();
    if (!sender || !data || n > K_IPC_BYTES)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    if (sender->process) {
        BOOLEAN grant = target == sender->id;
        for (UINT32 i = 0; i < sender->process->grant_count; ++i)
            if (sender->process->grants[i] == target)
                grant = TRUE;
        if (!grant) {
            k_spin_unlock(&sched_lock, f);
            return K_EPERM;
        }
    }
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i) {
        task *t = &tasks[i];
        if (t->id != target || t->state == K_TASK_UNUSED || t->state == K_TASK_ZOMBIE)
            continue;
        if (t->mail_count == MAIL_DEPTH) {
            k_spin_unlock(&sched_lock, f);
            return K_EAGAIN;
        }
        k_message *msg = &t->mail[(t->mail_head + t->mail_count) % MAIL_DEPTH];
        mem_zero(msg, sizeof(*msg));
        msg->sender = sender->id;
        msg->size = n;
        mem_copy(msg->bytes, data, n);
        ++t->mail_count;
        k_spin_unlock(&sched_lock, f);
        return 0;
    }
    k_spin_unlock(&sched_lock, f);
    return K_ENOENT;
}
int k_ipc_receive(k_message *msg)
{
    task *t = current_task();
    if (!t || !msg)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    if (!t->mail_count) {
        k_spin_unlock(&sched_lock, f);
        return K_EAGAIN;
    }
    mem_copy(msg, &t->mail[t->mail_head], sizeof(*msg));
    t->mail_head = (t->mail_head + 1) % MAIL_DEPTH;
    --t->mail_count;
    k_spin_unlock(&sched_lock, f);
    return 0;
}
int k_ipc_grant(UINT32 pid, UINT32 target)
{
    if (current_task() && current_task()->process)
        return K_EPERM;
    UINT64 f = k_spin_lock(&sched_lock);
    for (UINT32 i = 0; i < MAX_PROCESSES; ++i)
        if (processes[i].used && processes[i].id == pid) {
            k_process *p = &processes[i];
            for (UINT32 j = 0; j < p->grant_count; ++j)
                if (p->grants[j] == target) {
                    k_spin_unlock(&sched_lock, f);
                    return 0;
                }
            if (p->grant_count == ARRAY_LEN(p->grants)) {
                k_spin_unlock(&sched_lock, f);
                return K_E2BIG;
            }
            p->grants[p->grant_count++] = target;
            k_spin_unlock(&sched_lock, f);
            return 0;
        }
    k_spin_unlock(&sched_lock, f);
    return K_ENOENT;
}

UINT64 k_irq_save(void) { return arch_irq_save(); }
void k_irq_restore(UINT64 flags) { arch_irq_restore(flags); }
void k_cpu_relax(void) { arch_pause(); }
BOOLEAN k_cpu_caches_enabled(void) { return arch_caches_enabled(); }
void kernel_interrupts_disable(void) { arch_irq_disable(); }
