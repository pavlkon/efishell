// SPDX-License-Identifier: GPL-2.0-only
#include "input_internal.h"
/* Input drivers expose raw transport; report semantics belong to the OS. */
#define RAW_QUEUE_DEPTH 256
static k_input_report raw_reports[RAW_QUEUE_DEPTH];
static UINT32 raw_head, raw_count, raw_lost[K_INPUT_MAX_DEVICES];
static k_spinlock raw_lock = K_SPINLOCK_INIT;
k_mutex input_transport_lock = K_MUTEX_INIT;
void input_raw_push(UINT32 id, const void *bytes, UINT32 n, UINT32 flags)
{
    if (!id || id > K_INPUT_MAX_DEVICES || n > K_INPUT_REPORT_MAX)
        return;
    UINT64 irq = k_spin_lock(&raw_lock);
    if (raw_count == RAW_QUEUE_DEPTH) {
        ++raw_lost[id - 1];
        k_spin_unlock(&raw_lock, irq);
        return;
    }
    k_input_report *r = &raw_reports[(raw_head + raw_count) % RAW_QUEUE_DEPTH];
    r->device = id;
    r->size = n;
    r->flags = flags;
    r->lost = raw_lost[id - 1];
    r->tick = k_ticks();
    if (n)
        mem_copy(r->bytes, bytes, n);
    ++raw_count;
    k_spin_unlock(&raw_lock, irq);
}
int k_input_read(k_input_report *out)
{
    if (!out || k_cpu_id())
        return K_EINVAL;
    UINT64 irq = k_spin_lock(&raw_lock);
    if (!raw_count) {
        k_spin_unlock(&raw_lock, irq);
        return K_EAGAIN;
    }
    k_input_report *r = &raw_reports[raw_head];
    out->device = r->device;
    out->size = r->size;
    out->flags = r->flags;
    out->lost = r->lost;
    out->tick = r->tick;
    mem_copy(out->bytes, r->bytes, r->size);
    raw_head = (raw_head + 1) % RAW_QUEUE_DEPTH;
    --raw_count;
    k_spin_unlock(&raw_lock, irq);
    return 0;
}
void input_raw_error(UINT32 id)
{
    if (!id || id > K_INPUT_MAX_DEVICES)
        return;
    UINT64 irq = k_spin_lock(&raw_lock);
    ++raw_lost[id - 1];
    k_spin_unlock(&raw_lock, irq);
    input_raw_push(id, NULL, 0, K_INPUT_ERROR);
}
UINT32 k_input_count(void) { return usb_input_count() + (platform.ps2 ? 2 : 0); }
int k_input_get(UINT32 index, k_input_device *out)
{
    if (!out || k_cpu_id())
        return K_EINVAL;
    if (platform.ps2) {
        if (index < 2) {
            *out = (k_input_device){.id = index + 1,
                                    .transport = K_INPUT_I8042,
                                    .port = index,
                                    .max_packet = 1,
                                    .online = i8042_online(index)};
            return 0;
        }
        index -= 2;
    }
    if (index >= usb_input_count())
        return K_ENOENT;
    return usb_input_get(index, out);
}
void k_input_pump(void)
{
    if (k_cpu_id())
        return;
    if (platform.ps2) {
        UINT64 irq = k_irq_save();
        ps2_irq();
        k_irq_restore(irq);
    }
    usb_input_pump();
}
