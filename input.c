// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"

static void xhci_mdelay(UINT32 ms)
{
    while (ms--)
        k_delay_us(1000);
}
static UINT64 usb_dma_page(void) { return k_pmm_alloc_pages(1, 0x100000000ULL); }
/* Input drivers expose raw transport; report semantics belong to the OS. */
#define RAW_QUEUE_DEPTH 256
static k_input_report raw_reports[RAW_QUEUE_DEPTH];
static UINT32 raw_head, raw_count, raw_lost[K_INPUT_MAX_DEVICES];
static k_spinlock raw_lock = K_SPINLOCK_INIT;
static k_mutex input_transport_lock = K_MUTEX_INIT;
static BOOLEAN input_started, i8042_enabled[2];
static void raw_push(UINT32 id, const void *bytes, UINT32 n, UINT32 flags)
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
static void i8042_receive(UINT8 status, UINT8 value)
{
    UINT32 id = (status & 0x20) ? 2 : 1;
    if (status & 0xc0) {
        UINT64 f = k_spin_lock(&raw_lock);
        ++raw_lost[id - 1];
        k_spin_unlock(&raw_lock, f);
        raw_push(id, NULL, 0, K_INPUT_ERROR);
    } else
        raw_push(id, &value, 1, 0);
}
void ps2_irq(void)
{
    /* Both i8042 ports share 0x60; route bytes by AUX status. */
    for (UINT32 n = 0; n < 32; ++n) {
        UINT8 s = arch_in8(0x64);
        if (!(s & 1))
            break;
        i8042_receive(s, arch_in8(0x60));
    }
}
static int i8042_wait_write(void)
{
    for (UINT32 n = 0; n < 2000; ++n) {
        UINT8 s = arch_in8(0x64);
        if (s == 0xff)
            return K_ENOENT;
        if (!(s & 2))
            return 0;
        k_delay_us(10);
    }
    return K_ETIMEDOUT;
}
static int i8042_controller_byte(UINT8 command)
{
    int e = i8042_wait_write();
    if (!e)
        arch_out8(0x64, command);
    return e;
}
/* Controller response while both device clocks are disabled. */
static int i8042_controller_read(UINT8 *out)
{
    for (UINT32 n = 0; n < 2000; ++n) {
        UINT8 s = arch_in8(0x64);
        if (s & 1) {
            UINT8 value = arch_in8(0x60);
            if (s & 0xe0) {
                i8042_receive(s, value);
                continue;
            }
            *out = value;
            return 0;
        }
        k_delay_us(10);
    }
    return K_ETIMEDOUT;
}
int k_i8042_enable(UINT32 port)
{
    if (port > 1 || !platform.ps2 || k_cpu_id())
        return K_ENOTSUP;
    int e = k_mutex_lock(&input_transport_lock);
    if (e)
        return e;
    UINT64 irq = k_irq_save();
    /* Preserve unrelated firmware i8042 configuration bits. */
    UINT8 cfg = 0;
    e = i8042_controller_byte(0xad);
    if (!e)
        e = i8042_controller_byte(0xa7);
    ps2_irq();
    if (!e)
        e = i8042_controller_byte(0x20);
    if (!e)
        e = i8042_controller_read(&cfg);
    if (!e && port == 1) {
        UINT8 result;
        e = i8042_controller_byte(0xa9);
        if (!e)
            e = i8042_controller_read(&result);
        if (!e && result)
            e = K_ENOENT;
    }
    BOOLEAN was_enabled = i8042_enabled[port];
    if (!e) {
        i8042_enabled[port] = TRUE;
        cfg |= 1;
        cfg &= ~0x10u;
        if (i8042_enabled[1]) {
            cfg |= 2;
            cfg &= ~0x20u;
        }
        e = i8042_controller_byte(0x60);
        if (!e)
            e = i8042_wait_write();
        if (!e)
            arch_out8(0x60, cfg);
    }
    /* Restore port 1 even if port 2 testing fails. */
    (void)i8042_controller_byte(0xae);
    if (!e && i8042_enabled[1])
        e = i8042_controller_byte(0xa8);
    if (e)
        i8042_enabled[port] = was_enabled;
    k_irq_restore(irq);
    k_mutex_unlock(&input_transport_lock);
    return e;
}
static int i8042_device_read(UINT32 port, UINT8 *out)
{
    for (UINT32 n = 0; n < 2000; ++n) {
        UINT8 s = arch_in8(0x64);
        if (s & 1) {
            UINT8 value = arch_in8(0x60);
            if (((s >> 5) & 1) == port && !(s & 0xc0)) {
                *out = value;
                return 0;
            }
            i8042_receive(s, value);
        }
        k_delay_us(10);
    }
    return K_ETIMEDOUT;
}
int k_i8042_exchange(UINT32 port, UINT8 byte, UINT8 *response, UINT32 size)
{
    if (port > 1 || size > 8 || (size && !response) || !platform.ps2 || k_cpu_id())
        return K_EINVAL;
    int e = k_mutex_lock(&input_transport_lock);
    if (e)
        return e;
    UINT64 irq = k_irq_save();
    /* ACK/RESEND are transport handshakes; command semantics belong to the OS. */
    for (UINT32 retry = 0; retry < 3; ++retry) {
        if (port)
            e = i8042_controller_byte(0xd4);
        if (!e)
            e = i8042_wait_write();
        if (!e)
            arch_out8(0x60, byte);
        if (e)
            break;
        UINT8 ack = 0;
        for (UINT32 skipped = 0; skipped < 32; ++skipped) {
            e = i8042_device_read(port, &ack);
            if (e || ack == 0xfa || ack == 0xfe)
                break;
            i8042_receive(port ? 0x20 : 0, ack);
        }
        if (e)
            break;
        if (ack == 0xfe) {
            e = retry == 2 ? K_EIO : 0;
            continue;
        }
        if (ack != 0xfa) {
            e = K_EIO;
            break;
        }
        for (UINT32 n = 0; !e && n < size; ++n)
            e = i8042_device_read(port, response + n);
        break;
    }
    k_irq_restore(irq);
    k_mutex_unlock(&input_transport_lock);
    return e;
}

/* TRB fields are volatile because the controller updates them asynchronously. */
typedef struct {
    volatile UINT32 param1;
    volatile UINT32 param2;
    volatile UINT32 status;
    volatile UINT32 control;
} xhci_trb_t;

#define MAX_XHCI_CONTROLLERS 8
#define USB_RAW_ENDPOINTS (K_INPUT_MAX_DEVICES - 2)
typedef struct {
    UINT32 slot;
    xhci_trb_t *ep0_ring;
    UINT32 ep0_enqueue, ep0_cycle;
} xhci_dev_t;
typedef struct {
    BOOLEAN failed, running;
    UINT32 index;
    volatile UINT8 *cap, *op;
    volatile UINT32 *db, *rt;
    xhci_trb_t *command_ring, *events;
    UINT32 command_enqueue, command_cycle, event_read, event_phase;
    UINT64 *dcbaa;
    UINT32 context_dwords;
    UINT8 *scratch, *config;
    xhci_dev_t devices[256];
} xhci_controller;
static xhci_controller xhci_controllers[MAX_XHCI_CONTROLLERS];
static xhci_controller *xhci_current = &xhci_controllers[0];
static UINT32 xhci_controller_count;
#define xhci_failed (xhci_current->failed)
#define xhci_cap_regs (xhci_current->cap)
#define xhci_op_regs (xhci_current->op)
#define xhci_db_regs (xhci_current->db)
#define xhci_rt_regs (xhci_current->rt)
#define cmd_ring (xhci_current->command_ring)
#define cmd_enqueue (xhci_current->command_enqueue)
#define cmd_cycle (xhci_current->command_cycle)
#define event_ring (xhci_current->events)
#define event_dequeue (xhci_current->event_read)
#define event_cycle (xhci_current->event_phase)
#define g_dcbaa (xhci_current->dcbaa)
#define g_context_dwords (xhci_current->context_dwords)
#define dma_scratch_buf (xhci_current->scratch)
#define usb_desc_buf (xhci_current->config)
typedef struct {
    k_input_device info;
    UINT32 controller, slot, dci, root_port;
    xhci_trb_t *ring;
    UINT8 *buffer, *descriptor;
    UINT32 enqueue, cycle, requested;
    UINT64 pending_trb;
    BOOLEAN pending, enabled;
} usb_raw_endpoint;
static usb_raw_endpoint usb_endpoints[USB_RAW_ENDPOINTS];
static UINT32 usb_endpoint_count;
static BOOLEAN xhci_route_raw(UINT32, UINT32, UINT64);
static void xhci_queue_raw(usb_raw_endpoint *);

typedef struct {
    UINT32 route_string;
    UINT8 root_port;
    UINT8 parent_slot;
    UINT8 parent_port;
    UINT8 tier;
} usb_topo_t;

static void xhci_ring_doorbell(UINT32 slot, UINT32 target)
{
    if (xhci_failed)
        return;
    arch_write_fence();
    xhci_db_regs[slot] = target;
}

static void xhci_enqueue_cmd(UINT64 param, UINT32 status, UINT32 control)
{
    cmd_ring[cmd_enqueue].param1 = (UINT32)param;
    cmd_ring[cmd_enqueue].param2 = (UINT32)(param >> 32);
    cmd_ring[cmd_enqueue].status = status;
    cmd_ring[cmd_enqueue].control = control | (cmd_cycle ? 1 : 0);

    cmd_enqueue++;
    if (cmd_enqueue == 255) {
        cmd_ring[255].param1 = (UINT32)(UINTN)cmd_ring;
        cmd_ring[255].param2 = (UINT32)(((UINT64)(UINTN)cmd_ring) >> 32);
        cmd_ring[255].status = 0;
        cmd_ring[255].control = (6 << 10) | 2 | (cmd_cycle ? 1 : 0);
        cmd_enqueue = 0;
        cmd_cycle ^= 1;
    }
}

static void xhci_enqueue_ep0(xhci_dev_t *dev, UINT64 param, UINT32 status, UINT32 control)
{
    dev->ep0_ring[dev->ep0_enqueue].param1 = (UINT32)param;
    dev->ep0_ring[dev->ep0_enqueue].param2 = (UINT32)(param >> 32);
    dev->ep0_ring[dev->ep0_enqueue].status = status;
    dev->ep0_ring[dev->ep0_enqueue].control = control | (dev->ep0_cycle ? 1 : 0);

    dev->ep0_enqueue++;
    if (dev->ep0_enqueue == 255) {
        dev->ep0_ring[255].param1 = (UINT32)(UINTN)dev->ep0_ring;
        dev->ep0_ring[255].param2 = (UINT32)(((UINT64)(UINTN)dev->ep0_ring) >> 32);
        dev->ep0_ring[255].status = 0;
        dev->ep0_ring[255].control = (6 << 10) | 2 | (dev->ep0_cycle ? 1 : 0);
        dev->ep0_enqueue = 0;
        dev->ep0_cycle ^= 1;
    }
}

static UINT64 xhci_wait_event(UINT32 expected_trb_type)
{
    if (xhci_failed)
        return 0;
    UINT64 timeout = 50000;
    while (timeout--) {
        UINT32 control = event_ring[event_dequeue].control;

        if ((control & 1) == event_cycle) {
            UINT32 type = (control >> 10) & 0x3F;
            arch_read_fence();
            UINT32 status = event_ring[event_dequeue].status;
            UINT64 pointer =
                event_ring[event_dequeue].param1 | ((UINT64)event_ring[event_dequeue].param2 << 32);
            BOOLEAN routed = xhci_route_raw(control, status, pointer);
            event_dequeue++;
            if (event_dequeue == 256) {
                event_dequeue = 0;
                event_cycle ^= 1;
            }

            UINT64 erdp = (UINT64)(UINTN)event_ring + (event_dequeue * 16);
            xhci_rt_regs[6] = (UINT32)erdp | 8;
            xhci_rt_regs[7] = (UINT32)(erdp >> 32);

            if (!routed && type == expected_trb_type) {
                arch_compiler_barrier();
                return ((UINT64)control << 32) | status;
            }
        }
        k_delay_us(100);
    }
    xhci_failed = TRUE;
    return 0;
}

#define XHCI_TRB_SETUP_STAGE (2U << 10)
#define XHCI_TRB_DATA_STAGE (3U << 10)
#define XHCI_TRB_STATUS_STAGE (4U << 10)
#define XHCI_TRB_IOC (1U << 5)
#define XHCI_TRB_IDT (1U << 6)
#define XHCI_TRB_DIR_IN (1U << 16)
#define XHCI_SETUP_TRT_IN (3U << 16)
#define XHCI_SETUP_TRT_NONE (0U << 16)
#define XHCI_EP_TYPE_INTERRUPT_IN 7

#define USB_CLASS_HUB 9
#define HUB_REQ_GET_STATUS 0x00
#define HUB_REQ_CLEAR_FEATURE 0x01
#define HUB_REQ_SET_FEATURE 0x03
#define HUB_REQ_GET_DESCRIPTOR 0x06
#define HUB_FEATURE_PORT_RESET 4
#define HUB_FEATURE_PORT_POWER 8
#define HUB_FEATURE_C_PORT_CONNECTION 16
#define HUB_FEATURE_C_PORT_RESET 20

static UINT64 usb_setup_packet(UINT8 request_type, UINT8 request, UINT16 value, UINT16 index,
                               UINT16 length)
{
    return (UINT64)request_type | ((UINT64)request << 8) | ((UINT64)value << 16) |
           ((UINT64)index << 32) | ((UINT64)length << 48);
}

static UINT64 xhci_control_in(xhci_dev_t *dev, UINT64 setup, void *buffer, UINT16 length)
{
    xhci_enqueue_ep0(dev, setup, 8, XHCI_TRB_SETUP_STAGE | XHCI_TRB_IDT | XHCI_SETUP_TRT_IN);
    xhci_enqueue_ep0(dev, (UINT64)(UINTN)buffer, length, XHCI_TRB_DATA_STAGE | XHCI_TRB_DIR_IN);
    xhci_enqueue_ep0(dev, 0, 0, XHCI_TRB_STATUS_STAGE | XHCI_TRB_IOC);

    xhci_ring_doorbell(dev->slot, 1);
    return xhci_wait_event(32);
}

static UINT64 xhci_control_no_data(xhci_dev_t *dev, UINT64 setup)
{
    xhci_enqueue_ep0(dev, setup, 8, XHCI_TRB_SETUP_STAGE | XHCI_TRB_IDT | XHCI_SETUP_TRT_NONE);
    xhci_enqueue_ep0(dev, 0, 0, XHCI_TRB_STATUS_STAGE | XHCI_TRB_DIR_IN | XHCI_TRB_IOC);

    xhci_ring_doorbell(dev->slot, 1);
    return xhci_wait_event(32);
}

static BOOLEAN xhci_transfer_ok(UINT64 event)
{
    if (event == 0)
        return FALSE;
    UINT32 comp_code = (UINT32)(event >> 24) & 0xFF;
    return (comp_code == 1 || comp_code == 13);
}

static UINT8 usb_interval_to_xhci(UINT8 bInterval, UINT32 port_speed)
{
    if (port_speed == 3 || port_speed >= 4) {
        if (bInterval == 0)
            bInterval = 1;
        if (bInterval > 16)
            bInterval = 16;
        return (UINT8)(bInterval - 1);
    } else {
        UINT32 units = (bInterval ? bInterval : 1) * 8;
        UINT8 exp = 0;
        while ((1U << (exp + 1)) <= units && exp < 15)
            exp++;
        return exp;
    }
}

/* Interrupt-IN endpoints expose bytes plus descriptor metadata; no HID semantics here. */
static BOOLEAN xhci_configure_raw(xhci_dev_t *dev, usb_raw_endpoint *ep, UINT8 address,
                                  UINT16 packet, UINT8 interval, UINT32 speed, UINT8 burst,
                                  UINT16 esit)
{
    UINT8 number = address & 15, dci = (UINT8)(number * 2 + 1);
    UINT32 max_packet = packet & 0x7ff;
    UINT32 mult = speed == 3 ? ((packet >> 11) & 3) : 0;
    if (!number || !max_packet || max_packet > K_INPUT_REPORT_MAX || mult == 3 ||
        (speed == 2 && max_packet > 8) || (speed == 1 && max_packet > 64) || burst > 15)
        return FALSE;
    UINT32 payload = speed >= 4 ? esit : max_packet * (mult + 1);
    if (!payload || payload > K_INPUT_REPORT_MAX)
        return FALSE;
    ep->ring = (void *)(UINTN)usb_dma_page();
    ep->buffer = (void *)(UINTN)usb_dma_page();
    UINT32 *ctx = (void *)(UINTN)usb_dma_page();
    if (!ep->ring || !ep->buffer || !ctx)
        return FALSE;
    ep->cycle = 1;
    ep->dci = dci;
    ep->requested = payload;
    ep->info.max_packet = payload;
    ctx[1] = 1 | (1U << dci);
    UINT32 *slot = (void *)(UINTN)g_dcbaa[dev->slot];
    mem_copy(ctx + g_context_dwords, slot, g_context_dwords * 4);
    UINT32 last = slot[0] >> 27;
    if (dci > last)
        last = dci;
    ctx[g_context_dwords] = (slot[0] & 0x07ffffff) | (last << 27);
    UINT32 *ec = ctx + (1 + dci) * g_context_dwords;
    ec[0] = (UINT32)usb_interval_to_xhci(interval, speed) << 16;
    ec[1] = (3 << 1) | (7 << 3) | ((speed >= 4 ? burst : mult) << 8) | (max_packet << 16);
    wr64(ec + 2, (UINT64)(UINTN)ep->ring | 1);
    ec[4] = payload | (payload << 16);
    xhci_enqueue_cmd((UINT64)(UINTN)ctx, 0, (dev->slot << 24) | (12U << 10));
    xhci_ring_doorbell(0, 0);
    BOOLEAN ok = xhci_transfer_ok(xhci_wait_event(33));
    /* Do not free timed-out DMA contexts; hardware may still reference them. */
    if (!xhci_failed)
        pmm_free_page((UINT64)(UINTN)ctx);
    return ok;
}
static void xhci_queue_raw(usb_raw_endpoint *ep)
{
    if (ep->pending || !ep->enabled || !ep->info.online || xhci_failed)
        return;
    UINT32 index = ep->enqueue;
    xhci_trb_t *trb = ep->ring + index;
    trb->param1 = (UINT32)(UINTN)ep->buffer;
    trb->param2 = (UINT32)((UINT64)(UINTN)ep->buffer >> 32);
    trb->status = ep->requested;
    ep->pending_trb = (UINT64)(UINTN)trb;
    ep->pending = TRUE;
    arch_write_fence();
    trb->control = (1U << 10) | XHCI_TRB_IOC | (1U << 2) | ep->cycle;
    if (++ep->enqueue == 255) {
        xhci_trb_t *link = ep->ring + 255;
        link->param1 = (UINT32)(UINTN)ep->ring;
        link->param2 = (UINT32)((UINT64)(UINTN)ep->ring >> 32);
        link->status = 0;
        arch_write_fence();
        link->control = (6U << 10) | 2 | ep->cycle;
        ep->enqueue = 0;
        ep->cycle ^= 1;
    }
    xhci_ring_doorbell(ep->slot, ep->dci);
}
static BOOLEAN xhci_route_raw(UINT32 control, UINT32 status, UINT64 pointer)
{
    if (((control >> 10) & 63) != 32)
        return FALSE;
    UINT32 slot = control >> 24, dci = (control >> 16) & 31;
    for (UINT32 i = 0; i < usb_endpoint_count; ++i) {
        usb_raw_endpoint *ep = &usb_endpoints[i];
        if (ep->controller != xhci_current->index || ep->slot != slot || ep->dci != dci ||
            !ep->info.online)
            continue;
        UINT32 cc = status >> 24, residual = status & 0xffffff;
        if (!ep->pending)
            return TRUE;
        ep->pending = FALSE;
        if ((pointer & ~15ULL) != ep->pending_trb || (cc != 1 && cc != 13) ||
            residual > ep->requested) {
            ep->info.online = FALSE;
            raw_push(ep->info.id, NULL, 0, K_INPUT_ERROR | K_INPUT_OFFLINE);
            return TRUE;
        }
        arch_read_fence();
        raw_push(ep->info.id, ep->buffer, ep->requested - residual, 0);
        xhci_queue_raw(ep);
        return TRUE;
    }
    return FALSE;
}
static void xhci_pump_raw(void)
{
    for (UINT32 i = 0; i < usb_endpoint_count; ++i)
        if (usb_endpoints[i].controller == xhci_current->index)
            xhci_queue_raw(&usb_endpoints[i]);
    for (UINT32 count = 0; count < 128; ++count) {
        xhci_trb_t *event = event_ring + event_dequeue;
        UINT32 control = event->control;
        if ((control & 1) != event_cycle)
            break;
        arch_read_fence();
        UINT32 status = event->status;
        UINT64 pointer = event->param1 | ((UINT64)event->param2 << 32);
        if (++event_dequeue == 256) {
            event_dequeue = 0;
            event_cycle ^= 1;
        }
        UINT64 erdp = (UINT64)(UINTN)(event_ring + event_dequeue);
        xhci_rt_regs[6] = (UINT32)erdp | 8;
        xhci_rt_regs[7] = (UINT32)(erdp >> 32);
        (void)xhci_route_raw(control, status, pointer);
    }
    for (UINT32 i = 0; i < usb_endpoint_count; ++i) {
        usb_raw_endpoint *ep = &usb_endpoints[i];
        if (ep->controller != xhci_current->index || !ep->info.online)
            continue;
        volatile UINT32 *port = (void *)(xhci_op_regs + 0x400 + 16 * (ep->root_port - 1));
        if (xhci_failed || !(*port & 1)) {
            ep->info.online = FALSE;
            raw_push(ep->info.id, NULL, 0, K_INPUT_OFFLINE);
        }
    }
}

static BOOLEAN hub_get_descriptor(xhci_dev_t *dev, BOOLEAN superspeed, void *buf, UINT16 len)
{
    UINT16 desc_type = superspeed ? 0x2A00 : 0x2900;
    UINT64 res = xhci_control_in(
        dev, usb_setup_packet(0xA0, HUB_REQ_GET_DESCRIPTOR, desc_type, 0, len), buf, len);
    return xhci_transfer_ok(res);
}

static BOOLEAN hub_get_port_status(xhci_dev_t *dev, UINT8 port, UINT32 *status_out)
{
    volatile UINT8 *buf = (volatile UINT8 *)dma_scratch_buf + 128;
    UINT64 res = xhci_control_in(dev, usb_setup_packet(0xA3, HUB_REQ_GET_STATUS, 0, port, 4),
                                 (void *)buf, 4);
    if (!xhci_transfer_ok(res))
        return FALSE;
    *status_out =
        (UINT32)buf[0] | ((UINT32)buf[1] << 8) | ((UINT32)buf[2] << 16) | ((UINT32)buf[3] << 24);
    return TRUE;
}

static void hub_set_port_feature(xhci_dev_t *dev, UINT8 port, UINT16 feature)
{
    xhci_control_no_data(dev, usb_setup_packet(0x23, HUB_REQ_SET_FEATURE, feature, port, 0));
}

static void hub_clear_port_feature(xhci_dev_t *dev, UINT8 port, UINT16 feature)
{
    xhci_control_no_data(dev, usb_setup_packet(0x23, HUB_REQ_CLEAR_FEATURE, feature, port, 0));
}

static void xhci_print_completion(CONST char *label, UINT64 event)
{
    UINT8 comp_code = (UINT8)((UINT32)(event & 0xFFFFFFFF) >> 24);
    con_print(label);
    con_print(": completion code ");
    con_print_uint(comp_code);
    con_print("\n");
}

static BOOLEAN xhci_address_device(xhci_dev_t *dev, usb_topo_t *topo, UINT32 port_speed)
{
    UINT32 *input_ctx = (UINT32 *)(UINTN)usb_dma_page();
    UINT32 *output_ctx = (UINT32 *)(UINTN)usb_dma_page();
    dev->ep0_ring = (xhci_trb_t *)(UINTN)usb_dma_page();

    if (!input_ctx || !output_ctx || !dev->ep0_ring) {
        return FALSE;
    }

    for (UINT32 i = 0; i < 1024; i++) {
        input_ctx[i] = 0;
        output_ctx[i] = 0;
    }
    for (UINT32 i = 0; i < 512; i++) {
        ((UINT64 *)dev->ep0_ring)[i] = 0;
    }

    dev->ep0_enqueue = 0;
    dev->ep0_cycle = 1;

    g_dcbaa[dev->slot] = (UINT64)(UINTN)output_ctx;

    UINT32 max_packet = (port_speed >= 4) ? 512 : (port_speed == 3) ? 64 : 8;

    input_ctx[1] = 0x03;

    input_ctx[g_context_dwords + 0] =
        (1U << 27) | (port_speed << 20) | (topo->route_string & 0xFFFFF);
    input_ctx[g_context_dwords + 1] = ((UINT32)topo->root_port << 16);
    input_ctx[g_context_dwords + 2] =
        ((UINT32)topo->parent_slot) | ((UINT32)topo->parent_port << 8);

    input_ctx[2 * g_context_dwords + 1] = (3 << 1) | (4U << 3) | (max_packet << 16);
    input_ctx[2 * g_context_dwords + 2] = (UINT32)(UINT64)(UINTN)dev->ep0_ring | 1;
    input_ctx[2 * g_context_dwords + 3] = (UINT32)((UINT64)(UINTN)dev->ep0_ring >> 32);
    input_ctx[2 * g_context_dwords + 4] = 8;

    xhci_enqueue_cmd((UINT64)(UINTN)input_ctx, 0, (dev->slot << 24) | (11U << 10));
    xhci_ring_doorbell(0, 0);
    UINT64 res = xhci_wait_event(33);
    xhci_print_completion("Address Device", res);
    return xhci_transfer_ok(res);
}

static BOOLEAN xhci_set_hub_slot_info(xhci_dev_t *dev, UINT8 num_ports, BOOLEAN multi_tt)
{
    UINT32 *input_ctx = (UINT32 *)(UINTN)usb_dma_page();
    if (!input_ctx)
        return FALSE;
    for (UINT32 i = 0; i < 1024; i++)
        input_ctx[i] = 0;

    input_ctx[1] = 0x01;

    UINT32 *old = (UINT32 *)(UINTN)g_dcbaa[dev->slot];
    for (UINT32 i = 0; i < g_context_dwords; ++i)
        input_ctx[g_context_dwords + i] = old[i];
    UINT32 slot0 = old[0];
    slot0 |= (1U << 26);
    if (multi_tt)
        slot0 |= (1U << 25);
    input_ctx[g_context_dwords + 0] = slot0;
    input_ctx[g_context_dwords + 1] = (old[1] & 0x00ffffffU) | ((UINT32)num_ports << 24);

    xhci_enqueue_cmd((UINT64)(UINTN)input_ctx, 0, (dev->slot << 24) | (12U << 10));
    xhci_ring_doorbell(0, 0);
    UINT64 res = xhci_wait_event(33);
    return xhci_transfer_ok(res);
}

static BOOLEAN xhci_finish_interfaces(xhci_dev_t *dev, const UINT8 *cfg, UINT16 size, UINT32 speed,
                                      UINT32 root_port, UINT16 vendor, UINT16 product)
{
    if (size < 9 || cfg[0] < 9 || cfg[1] != 2 || !cfg[5])
        return FALSE;
    BOOLEAN configured = FALSE;
    UINT32 added = 0;
    /* Enumerate alternate-zero HID interfaces, including composite receivers. */
    for (UINT32 pos = cfg[0]; pos + 2 <= size;) {
        UINT32 len = cfg[pos];
        if (len < 2 || len > size - pos)
            break;
        if (cfg[pos + 1] != 4 || len < 9 || cfg[pos + 3] || cfg[pos + 5] != 3) {
            pos += len;
            continue;
        }
        UINT32 end = pos + len;
        UINT16 report_size = 0, packet = 0, esit = 0;
        UINT8 address = 0, interval = 0, burst = 0;
        while (end + 2 <= size && cfg[end + 1] != 4) {
            UINT32 n = cfg[end];
            if (n < 2 || n > size - end)
                return added != 0;
            if (cfg[end + 1] == 0x21 && n >= 9) {
                for (UINT32 at = 6; at + 3 <= n; at += 3)
                    if (cfg[end + at] == 0x22)
                        report_size = rd16(cfg + end + at + 1);
            }
            if (!address && cfg[end + 1] == 5 && n >= 7 && (cfg[end + 2] & 0x80) &&
                (cfg[end + 3] & 3) == 3) {
                address = cfg[end + 2];
                packet = rd16(cfg + end + 4);
                interval = cfg[end + 6];
                if (speed >= 4 && end + n + 6 <= size && cfg[end + n] >= 6 &&
                    cfg[end + n + 1] == 0x30) {
                    burst = cfg[end + n + 2];
                    esit = rd16(cfg + end + n + 4);
                    if (cfg[end + n + 3] & 3)
                        address = 0;
                }
            }
            end += n;
        }
        if (address && usb_endpoint_count < USB_RAW_ENDPOINTS) {
            if (!configured) {
                if (!xhci_transfer_ok(
                        xhci_control_no_data(dev, usb_setup_packet(0, 9, cfg[5], 0, 0))))
                    return FALSE;
                configured = TRUE;
            }
            usb_raw_endpoint *ep = &usb_endpoints[usb_endpoint_count];
            mem_zero(ep, sizeof(*ep));
            ep->controller = xhci_current->index;
            ep->slot = dev->slot;
            ep->root_port = root_port;
            ep->info = (k_input_device){.id = usb_endpoint_count + 3,
                                        .transport = K_INPUT_USB,
                                        .controller = ep->controller,
                                        .port = root_port,
                                        .vendor = vendor,
                                        .product = product,
                                        .interface_number = cfg[pos + 2],
                                        .interface_class = cfg[pos + 5],
                                        .interface_subclass = cfg[pos + 6],
                                        .interface_protocol = cfg[pos + 7],
                                        .endpoint = address};
            if (report_size && report_size <= K_INPUT_DESCRIPTOR_MAX) {
                ep->descriptor = (void *)(UINTN)usb_dma_page();
                if (ep->descriptor &&
                    xhci_transfer_ok(xhci_control_in(
                        dev,
                        usb_setup_packet(0x81, 6, 0x2200, ep->info.interface_number, report_size),
                        ep->descriptor, report_size)))
                    ep->info.descriptor_size = report_size;
                else if (xhci_failed)
                    return added != 0;
            }
            if (xhci_configure_raw(dev, ep, address, packet, interval, speed, burst, esit)) {
                ep->info.online = TRUE;
                ++usb_endpoint_count;
                ++added;
            } else if (xhci_failed)
                return added != 0;
        }
        pos = end;
    }
    xhci_current->devices[dev->slot] = *dev;
    return added != 0;
}

static BOOLEAN xhci_probe_device(usb_topo_t *topo, UINT32 port_speed)
{
    if (xhci_failed || topo->tier > 5) {
        return FALSE;
    }

    xhci_enqueue_cmd(0, 0, (9U << 10));
    xhci_ring_doorbell(0, 0);
    UINT64 res = xhci_wait_event(33);
    if (!xhci_transfer_ok(res)) {
        con_print("xhci: Enable Slot failed\n");
        return FALSE;
    }

    xhci_dev_t dev;
    dev.slot = (UINT32)(res >> 56) & 0xFF;
    if (!dev.slot) {
        return FALSE;
    }

    xhci_mdelay(50);

    if (!xhci_address_device(&dev, topo, port_speed)) {
        con_print("xhci: Address Device failed\n");
        goto fail_disable;
    }

    xhci_mdelay(20);

    con_print("xhci: about to GET_DESCRIPTOR(Device)\n");

    {
        volatile UINT8 *devdesc = (volatile UINT8 *)dma_scratch_buf + 0;

        res = xhci_control_in(&dev, usb_setup_packet(0x80, 0x06, 0x0100, 0, 8), (void *)devdesc, 8);
        if (!xhci_transfer_ok(res)) {
            con_print("xhci: GET_DESCRIPTOR(Device) 8-byte read failed\n");
            goto fail_disable;
        }

        UINT32 initial_max_packet = (port_speed >= 4) ? 512 : (port_speed == 3) ? 64 : 8;
        UINT32 actual_max_packet = devdesc[7];
        if (port_speed >= 4) {
            if (devdesc[7] != 9)
                goto fail_disable;
            actual_max_packet = 512;
        }
        if (actual_max_packet != 8 && actual_max_packet != 16 && actual_max_packet != 32 &&
            actual_max_packet != 64 && actual_max_packet != 512)
            goto fail_disable;

        if (actual_max_packet != initial_max_packet && actual_max_packet != 0) {
            con_print("xhci: updating EP0 max packet to ");
            con_print_uint(actual_max_packet);
            con_print("\n");

            UINT32 *input_ctx = (UINT32 *)(UINTN)usb_dma_page();
            if (input_ctx) {
                for (UINT32 i = 0; i < 1024; i++)
                    input_ctx[i] = 0;
                input_ctx[1] = 0x02;

                UINT32 *output_ctx = (UINT32 *)(UINTN)g_dcbaa[dev.slot];

                for (UINT32 i = 0; i < g_context_dwords; i++) {
                    input_ctx[2 * g_context_dwords + i] = output_ctx[1 * g_context_dwords + i];
                }
                input_ctx[2 * g_context_dwords + 1] &= ~0xFFFF0000;
                input_ctx[2 * g_context_dwords + 1] |= (actual_max_packet << 16);

                xhci_enqueue_cmd((UINT64)(UINTN)input_ctx, 0, (dev.slot << 24) | (13U << 10));
                xhci_ring_doorbell(0, 0);
                UINT64 evaluated = xhci_wait_event(33);
                if (!xhci_transfer_ok(evaluated))
                    goto fail_disable;
                pmm_free_page((UINT64)(UINTN)input_ctx);
            }
        }

        res =
            xhci_control_in(&dev, usb_setup_packet(0x80, 0x06, 0x0100, 0, 18), (void *)devdesc, 18);
        if (!xhci_transfer_ok(res)) {
            con_print("xhci: GET_DESCRIPTOR(Device) full read failed\n");
            goto fail_disable;
        }

        if (devdesc[4] == USB_CLASS_HUB) {
            BOOLEAN is_ss = (devdesc[3] >= 3);
            volatile UINT8 *hubdesc = (volatile UINT8 *)dma_scratch_buf + 32;
            UINT8 num_ports;
            BOOLEAN multi_tt = FALSE;

            volatile UINT8 *hubcfg = (volatile UINT8 *)dma_scratch_buf + 64;
            res = xhci_control_in(&dev, usb_setup_packet(0x80, 6, 0x0200, 0, 9), (void *)hubcfg, 9);
            if (!xhci_transfer_ok(res) || hubcfg[1] != 2 || !hubcfg[5])
                goto fail_disable;
            res = xhci_control_no_data(&dev, usb_setup_packet(0, 9, hubcfg[5], 0, 0));
            if (!xhci_transfer_ok(res))
                goto fail_disable;
            if (is_ss) {
                res = xhci_control_no_data(
                    &dev, usb_setup_packet(0x20, 12, (UINT16)(topo->tier - 1), 0, 0));
                if (!xhci_transfer_ok(res))
                    goto fail_disable;
            }
            if (!hub_get_descriptor(&dev, is_ss, (void *)hubdesc, is_ss ? 12 : 9)) {
                con_print("xhci: could not read hub descriptor\n");
                goto fail_disable;
            }

            num_ports = hubdesc[2];
            if (!is_ss) {
                if (devdesc[6] == 2) {
                    UINT64 alternate = xhci_control_no_data(&dev, usb_setup_packet(1, 11, 1, 0, 0));
                    multi_tt = xhci_transfer_ok(alternate);
                }
            }

            if (!xhci_set_hub_slot_info(&dev, num_ports, multi_tt)) {
                con_print("xhci: failed to configure hub slot\n");
                goto fail_disable;
            }

            con_print("xhci: hub found (slot ");
            con_print_uint(dev.slot);
            con_print(", ");
            con_print_uint(num_ports);
            con_print(" ports)\n");

            for (UINT8 p = 1; p <= num_ports && p <= 15; p++) {
                hub_set_port_feature(&dev, p, HUB_FEATURE_PORT_POWER);
            }
            xhci_mdelay(200);

            for (UINT8 p = 1; p <= num_ports && p <= 15; p++) {
                UINT32 status;

                if (!hub_get_port_status(&dev, p, &status))
                    continue;
                if (!(status & 0x0001))
                    continue;

                hub_clear_port_feature(&dev, p, HUB_FEATURE_C_PORT_CONNECTION);
                hub_set_port_feature(&dev, p, HUB_FEATURE_PORT_RESET);

                UINT64 to = 500;
                do {
                    if (!hub_get_port_status(&dev, p, &status))
                        break;
                    if (status & 0x00100000)
                        break;
                    xhci_mdelay(1);
                } while (--to);

                hub_clear_port_feature(&dev, p, HUB_FEATURE_C_PORT_RESET);
                if (!hub_get_port_status(&dev, p, &status))
                    continue;
                if (!(status & 0x0001) || !(status & 0x0002))
                    continue;

                UINT32 child_speed;
                if (is_ss)
                    child_speed = 4;
                else if (status & 0x0400)
                    child_speed = 3;
                else if (status & 0x0200)
                    child_speed = 2;
                else
                    child_speed = 1;

                usb_topo_t child = *topo;
                child.route_string |= ((UINT32)p << (4 * (topo->tier - 1)));
                if (child_speed < 3 && port_speed == 3) {
                    child.parent_slot = (UINT8)dev.slot;
                    child.parent_port = p;
                } else if (child_speed >= 3) {
                    child.parent_slot = 0;
                    child.parent_port = 0;
                }
                child.tier = topo->tier + 1;

                con_print("xhci: hub port ");
                con_print_uint(p);
                con_print(": device connected, probing...\n");

                (void)xhci_probe_device(&child, child_speed);
                if (xhci_failed)
                    return FALSE;
            }
            return TRUE; /* Keep hub slots alive while enumerating downstream ports. */
        }

        {
            volatile UINT8 *cfgdesc = (volatile UINT8 *)dma_scratch_buf + 64;
            UINT16 total_len;

            res = xhci_control_in(&dev, usb_setup_packet(0x80, 0x06, 0x0200, 0, 9), (void *)cfgdesc,
                                  9);
            if (!xhci_transfer_ok(res)) {
                con_print("xhci: GET_DESCRIPTOR(Config short) transfer failed\n");
                goto fail_disable;
            }
            if (cfgdesc[1] != 2) {
                con_print("xhci: unexpected config desc type, byte1=");
                con_print_uint(cfgdesc[1]);
                con_print(" byte0=");
                con_print_uint(cfgdesc[0]);
                con_print("\n");
                goto fail_disable;
            }

            total_len = (UINT16)cfgdesc[2] | ((UINT16)cfgdesc[3] << 8);
            if (total_len < 9 || total_len > 4096) {
                goto fail_disable;
            }

            volatile UINT8 *v_usb_desc_buf = (volatile UINT8 *)usb_desc_buf;
            res = xhci_control_in(&dev, usb_setup_packet(0x80, 0x06, 0x0200, 0, total_len),
                                  (void *)v_usb_desc_buf, total_len);
            if (!xhci_transfer_ok(res)) {
                goto fail_disable;
            }

            con_print("xhci: device desc bytes = ");
            for (UINT16 i = 0; i < 8 && i < 18; i++) {
                con_print_uint(devdesc[i]);
                con_print(" ");
            }
            con_print("\n");

            con_print("xhci: config total_len = ");
            con_print_uint(total_len);
            con_print("\n");

            {
                UINT16 pos = v_usb_desc_buf[0];
                while (pos + 2 <= total_len) {
                    UINT8 len = v_usb_desc_buf[pos];
                    UINT8 type = v_usb_desc_buf[pos + 1];
                    if (len < 2 || pos + len > total_len)
                        break;
                    con_print("  desc @");
                    con_print_uint(pos);
                    con_print(" len=");
                    con_print_uint(len);
                    con_print(" type=");
                    con_print_uint(type);
                    if (type == 4 && len >= 9) {
                        con_print(" class=");
                        con_print_uint(v_usb_desc_buf[pos + 5]);
                        con_print(" subclass=");
                        con_print_uint(v_usb_desc_buf[pos + 6]);
                        con_print(" proto=");
                        con_print_uint(v_usb_desc_buf[pos + 7]);
                    }
                    con_print("\n");
                    pos += len;
                }
            }

            if (!xhci_finish_interfaces(&dev, (const UINT8 *)v_usb_desc_buf, total_len, port_speed,
                                        topo->root_port, rd16((const void *)(devdesc + 8)),
                                        rd16((const void *)(devdesc + 10)))) {
                con_print("xhci: no supported interrupt-IN interface\n");
                goto fail_disable;
            }

            return TRUE;
        }
    }

fail_disable:
    for (UINT32 i = 0; i < usb_endpoint_count; ++i)
        if (usb_endpoints[i].controller == xhci_current->index && usb_endpoints[i].slot == dev.slot)
            usb_endpoints[i].info.online = FALSE;
    xhci_enqueue_cmd(0, 0, (dev.slot << 24) | (10U << 10));
    xhci_ring_doorbell(0, 0);
    xhci_wait_event(33);
    return FALSE;
}

static BOOLEAN xhci_bios_handoff(void)
{
    UINT32 hccparams1 = *(volatile UINT32 *)(xhci_cap_regs + 0x10);
    UINT32 xecp = (hccparams1 >> 16) & 0xFFFF;

    if (!xecp) {
        return TRUE;
    }

    volatile UINT32 *ext_cap = (volatile UINT32 *)(xhci_cap_regs + (xecp << 2));

    UINT32 hops = 0;
    while (++hops <= 256) {
        if ((UINTN)ext_cap < (UINTN)xhci_cap_regs ||
            (UINTN)ext_cap + 8 > (UINTN)xhci_cap_regs + 0x100000)
            return FALSE;
        UINT32 cap = *ext_cap;
        UINT8 cap_id = cap & 0xFF;
        UINT8 next = (cap >> 8) & 0xFF;

        if (cap_id == 1) {
            con_print("xhci: Found USB Legacy Support. Requesting OS ownership...\n");

            *ext_cap = cap | (1U << 24);
            if (cap & (1 << 16)) {
                *ext_cap = cap | (1 << 24);

                UINT32 timeout = 1000;
                while ((*ext_cap & (1 << 16)) && timeout > 0) {
                    xhci_mdelay(1);
                    timeout--;
                }

                if (*ext_cap & (1 << 16)) {
                    con_print("xhci: firmware ownership timeout; skipping controller\n");
                    return FALSE;
                } else {
                    con_print("xhci: OS ownership acquired.\n");
                }
            } else {
                con_print("xhci: OS ownership already acquired.\n");
            }

            volatile UINT32 *legctlsts = ext_cap + 1;
            UINT32 ctlsts = *legctlsts;
            ctlsts &= 0x1EE00000;
            *legctlsts = ctlsts;
        }

        if (next == 0) {
            return TRUE;
        }
        ext_cap += next;
    }
    return FALSE;
}

#define PORTSC_RW1C_MASK 0x00FE0002

static UINT32 xhci_find_controllers(UINT64 *out_bases, UINT32 max_out)
{
    UINT16 bus;
    UINT8 slot, func;
    UINT32 count = 0;

    for (bus = 0; bus < 256; bus++) {
        for (slot = 0; slot < 32; slot++) {
            for (func = 0; func < 8; func++) {
                if ((pci_read_32(bus, slot, func, 0) & 0xFFFF) == 0xFFFF) {
                    continue;
                }

                if ((pci_read_32(bus, slot, func, 8) >> 8) == 0x0C0330) {
                    UINT32 bar0 = pci_read_32(bus, slot, func, 0x10);
                    UINT32 bar1 = pci_read_32(bus, slot, func, 0x14);
                    UINT64 phys_base;

                    if ((bar0 & 6) == 4) {
                        phys_base = (bar0 & 0xFFFFFFF0) | ((UINT64)bar1 << 32);
                    } else {
                        phys_base = (bar0 & 0xFFFFFFF0);
                    }

                    UINT32 vendor_dev = pci_read_32(bus, slot, func, 0x00);
                    if (!phys_base || (bar0 & 1))
                        continue;
                    UINT16 bdf = (UINT16)((bus << 8) | (slot << 3) | func);
                    pci_command(bdf, 2 | 0x400, 0);

                    con_print("xhci: controller at PCI ");
                    con_print_uint(bus);
                    con_print(":");
                    con_print_uint(slot);
                    con_print(".");
                    con_print_uint(func);
                    con_print(", vendor:device=");
                    con_print_uint(vendor_dev & 0xFFFF);
                    con_print(":");
                    con_print_uint((vendor_dev >> 16) & 0xFFFF);
                    con_print("\n");

                    if (count < max_out) {
                        out_bases[count] = phys_base;
                    }
                    count++;
                }
            }
        }
    }

    return count;
}

static BOOLEAN xhci_try_controller(UINT64 phys_base)
{
    xhci_failed = FALSE;
    if (k_mmio_map(phys_base, 0x100000))
        return FALSE;
    xhci_cap_regs = (volatile UINT8 *)(UINTN)phys_base;
    UINT32 db = *(volatile UINT32 *)(xhci_cap_regs + 0x14) & ~3U;
    UINT32 rt = *(volatile UINT32 *)(xhci_cap_regs + 0x18) & ~31U;
    if (xhci_cap_regs[0] < 0x20 || db > 0xffbfc || rt > 0xff000)
        return FALSE;
    xhci_op_regs = xhci_cap_regs + xhci_cap_regs[0];
    xhci_db_regs = (volatile UINT32 *)(xhci_cap_regs + db);
    xhci_rt_regs = (volatile UINT32 *)(xhci_cap_regs + rt + 0x20);

    if (!xhci_bios_handoff())
        return FALSE;

    *(volatile UINT32 *)(xhci_op_regs + 0) &= ~1;
    if (!arch_wait_reg((void *)(xhci_op_regs + 4), 1, 1, 1000))
        return FALSE;

    *(volatile UINT32 *)(xhci_op_regs + 0) |= 2;
    if (!arch_wait_reg((void *)xhci_op_regs, 2, 0, 1000))
        return FALSE;
    if (!arch_wait_reg((void *)(xhci_op_regs + 4), 1 << 11, 0, 1000))
        return FALSE;
    if (!(*(volatile UINT32 *)(xhci_op_regs + 8) & 1))
        return FALSE;

    UINT32 hcsparams1_reg = *(volatile UINT32 *)(xhci_cap_regs + 4);
    UINT32 max_slots = hcsparams1_reg & 0xFF;
    if (!max_slots)
        return FALSE;
    *(volatile UINT32 *)(xhci_op_regs + 0x38) = max_slots;

    UINT64 *dcbaa = (UINT64 *)(UINTN)usb_dma_page();
    cmd_ring = (xhci_trb_t *)(UINTN)usb_dma_page();
    event_ring = (xhci_trb_t *)(UINTN)usb_dma_page();
    UINT64 *erst = (UINT64 *)(UINTN)usb_dma_page();

    if (!dcbaa || !cmd_ring || !event_ring || !erst)
        return FALSE;
    cmd_enqueue = event_dequeue = 0;
    cmd_cycle = event_cycle = 1;
    for (int i = 0; i < 512; i++) {
        dcbaa[i] = 0;
        ((UINT64 *)cmd_ring)[i] = 0;
        ((UINT64 *)event_ring)[i] = 0;
        erst[i] = 0;
    }

    UINT32 hcsparams2 = *(volatile UINT32 *)(xhci_cap_regs + 8);
    UINT32 max_scratch = ((hcsparams2 >> 27) & 0x1F) | (((hcsparams2 >> 21) & 0x1F) << 5);

    if (max_scratch > 0) {
        UINT64 *scratch_arr =
            (UINT64 *)(UINTN)k_pmm_alloc_pages((max_scratch + 511) / 512, 0x100000000ULL);
        if (!scratch_arr)
            return FALSE;
        for (UINT32 i = 0; i < max_scratch; i++) {
            scratch_arr[i] = usb_dma_page();
            if (!scratch_arr[i])
                return FALSE;
        }
        dcbaa[0] = (UINT64)(UINTN)scratch_arr;
    }

    *(volatile UINT64 *)(xhci_op_regs + 0x30) = (UINT64)(UINTN)dcbaa;
    *(volatile UINT64 *)(xhci_op_regs + 0x18) = (UINT64)(UINTN)cmd_ring | 1;

    erst[0] = (UINT64)(UINTN)event_ring;
    erst[1] = 256;
    xhci_rt_regs[2] = 1;
    xhci_rt_regs[4] = (UINT32)(UINT64)(UINTN)erst;
    xhci_rt_regs[5] = (UINT32)((UINT64)(UINTN)erst >> 32);
    xhci_rt_regs[6] = (UINT32)(UINT64)(UINTN)event_ring | 8;
    xhci_rt_regs[7] = (UINT32)((UINT64)(UINTN)event_ring >> 32);

    /* Enable bus mastering only after all controller DMA pointers are valid. */
    for (UINT32 bus = 0; bus < 256; ++bus)
        for (UINT32 slot = 0; slot < 32; ++slot)
            for (UINT32 fn = 0; fn < 8; ++fn) {
                UINT16 bdf = (UINT16)((bus << 8) | (slot << 3) | fn);
                if ((pci_get(bdf, 0) & 0xffff) != 0xffff && (pci_get(bdf, 8) >> 8) == 0x0c0330 &&
                    pci_mbar(bdf, 0x10) == phys_base)
                    pci_command(bdf, 6 | 0x400, 0);
            }
    arch_write_fence();
    *(volatile UINT32 *)(xhci_op_regs + 0) |= 1;
    if (!arch_wait_reg((void *)(xhci_op_regs + 4), 1, 0, 1000))
        return FALSE;

    UINT32 max_ports = (*(volatile UINT32 *)(xhci_cap_regs + 4) >> 24) & 0xFF;
    con_print("xhci: max_ports = ");
    con_print_uint(max_ports);
    con_print("\n");
    volatile UINT32 *port_regs = (volatile UINT32 *)(xhci_op_regs + 0x400);

    /* Power ports even when PPC reporting is unreliable. */
    for (UINT32 pi = 0; pi < max_ports; pi++) {
        UINT32 portsc = port_regs[pi * 4];
        port_regs[pi * 4] = (portsc & ~PORTSC_RW1C_MASK) | (1U << 9);
    }

    con_print("xhci: Powered on all ports. Waiting 300ms for devices to connect...\n");
    xhci_mdelay(300);
    con_print("xhci: Controller running.\n");

    UINT32 hccparams1 = *(volatile UINT32 *)(xhci_cap_regs + 0x10);
    g_context_dwords = (hccparams1 & (1U << 2)) ? 16 : 8;
    g_dcbaa = dcbaa;

    dma_scratch_buf = (UINT8 *)(UINTN)usb_dma_page();
    usb_desc_buf = (UINT8 *)(UINTN)usb_dma_page();
    if (!usb_desc_buf || !dma_scratch_buf) {
        con_print("xhci: insufficient memory for USB enumeration\n");
        return FALSE;
    }

    for (UINT32 port_index = 0; port_index < max_ports; port_index++) {
        UINT32 portsc = port_regs[port_index * 4];

        if (!(portsc & 1)) {
            continue;
        }

        con_print("xhci: probing root port ");
        con_print_uint(port_index + 1);
        con_print("\n");

        if (((portsc >> 10) & 0x0F) < 4) {
            /* Mask xHCI W1C bits when asserting port reset. */
            port_regs[port_index * 4] = (portsc & ~PORTSC_RW1C_MASK) | (1U << 4);

            xhci_mdelay(50);

            UINT64 timeout = 1000;
            while (timeout-- && (port_regs[port_index * 4] & (1U << 4))) {
                xhci_mdelay(1);
            }

            /* Clear PRC with write-one-to-clear. */
            port_regs[port_index * 4] =
                (port_regs[port_index * 4] & ~PORTSC_RW1C_MASK) | (1U << 21);
            xhci_mdelay(20);
        } else {

            UINT64 timeout = 100;
            while (timeout-- && !(port_regs[port_index * 4] & (1U << 1))) {
                xhci_mdelay(1);
            }
        }

        if (xhci_failed)
            return FALSE;
        portsc = port_regs[port_index * 4];
        if (!(portsc & 1) || !(portsc & (1U << 1))) {
            con_print("xhci: port failed to enable\n");
            continue;
        }

        xhci_mdelay(50);

        UINT32 port_speed = (portsc >> 10) & 0x0F;

        usb_topo_t topo;
        topo.route_string = 0;
        topo.root_port = (UINT8)(port_index + 1);
        topo.parent_slot = 0;
        topo.parent_port = 0;
        topo.tier = 1;

        (void)xhci_probe_device(&topo, port_speed);
    }
    xhci_current->running = !xhci_failed;
    return !xhci_failed;
}

void kernel_usb_init(void)
{
    if (input_started || k_cpu_id())
        return;
    input_started = TRUE;
    if (!platform.dma_allowed) {
        con_print("USB DMA unavailable: ");
        con_print(platform.dma_reason);
        con_print("\n");
        return;
    }
    UINT64 bases[MAX_XHCI_CONTROLLERS];
    UINT32 found = xhci_find_controllers(bases, MAX_XHCI_CONTROLLERS);
    if (found > MAX_XHCI_CONTROLLERS)
        found = MAX_XHCI_CONTROLLERS;
    xhci_controller_count = found;
    for (UINT32 i = 0; i < found; ++i) {
        xhci_current = &xhci_controllers[i];
        xhci_current->index = i;
        con_print("xhci: enumerate controller ");
        con_print_uint(i);
        con_print("\n");
        if (!xhci_try_controller(bases[i])) {
            xhci_failed = TRUE;
            for (UINT32 n = 0; n < usb_endpoint_count; ++n)
                if (usb_endpoints[n].controller == i)
                    usb_endpoints[n].info.online = FALSE;
        }
    }
    con_print("USB interrupt input interfaces: ");
    con_print_uint(usb_endpoint_count);
    con_print("\n");
}
UINT32 k_input_count(void) { return usb_endpoint_count + (platform.ps2 ? 2 : 0); }
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
                                    .online = index == 0 || i8042_enabled[1]};
            return 0;
        }
        index -= 2;
    }
    if (index >= usb_endpoint_count)
        return K_ENOENT;
    *out = usb_endpoints[index].info;
    return 0;
}
int k_usb_report_descriptor(UINT32 id, void *out, UINT32 capacity, UINT32 *size)
{
    if (!out || !size || id < 3 || id - 3 >= usb_endpoint_count || k_cpu_id())
        return K_EINVAL;
    usb_raw_endpoint *ep = &usb_endpoints[id - 3];
    *size = ep->info.descriptor_size;
    if (!*size)
        return K_ENOENT;
    if (capacity < *size)
        return K_E2BIG;
    mem_copy(out, ep->descriptor, *size);
    return 0;
}
int k_usb_input_protocol(UINT32 id, UINT16 protocol)
{
    if (id < 3 || id - 3 >= usb_endpoint_count || protocol > 1 || k_cpu_id())
        return K_EINVAL;
    int e = k_mutex_lock(&input_transport_lock);
    if (e)
        return e;
    usb_raw_endpoint *ep = &usb_endpoints[id - 3];
    xhci_current = &xhci_controllers[ep->controller];
    if (!ep->info.online || xhci_failed)
        e = K_EIO;
    else if (ep->pending || ep->enabled)
        e = K_EBUSY;
    else {
        xhci_dev_t *dev = &xhci_current->devices[ep->slot];
        e = xhci_transfer_ok(xhci_control_no_data(
                dev, usb_setup_packet(0x21, 0x0b, protocol, ep->info.interface_number, 0)))
                ? 0
                : K_EIO;
    }
    k_mutex_unlock(&input_transport_lock);
    return e;
}
int k_usb_input_start(UINT32 id)
{
    if (id < 3 || id - 3 >= usb_endpoint_count || k_cpu_id())
        return K_EINVAL;
    usb_raw_endpoint *ep = &usb_endpoints[id - 3];
    xhci_controller *controller = &xhci_controllers[ep->controller];
    if (!ep->info.online || controller->failed || !controller->running)
        return K_EIO;
    ep->enabled = TRUE;
    return 0;
}
void k_input_pump(void)
{
    if (k_cpu_id())
        return;
    if (platform.ps2) {
        UINT64 f = k_irq_save();
        ps2_irq(); /* Polling path also works without usable firmware IRQ routing. */
        k_irq_restore(f);
    }
    if (k_mutex_trylock(&input_transport_lock))
        return;
    for (UINT32 i = 0; i < xhci_controller_count; ++i) {
        xhci_current = &xhci_controllers[i];
        if (xhci_current->running)
            xhci_pump_raw();
    }
    k_mutex_unlock(&input_transport_lock);
}
