// SPDX-License-Identifier: GPL-2.0-only
#include "input_internal.h"
static BOOLEAN i8042_enabled[2];
static void i8042_receive(UINT8 status, UINT8 value)
{
    UINT32 id = (status & 0x20) ? 2 : 1;
    if (status & 0xc0) {
        input_raw_error(id);
    } else
        input_raw_push(id, &value, 1, 0);
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

BOOLEAN i8042_online(UINT32 port) { return port < 2 && (port == 0 || i8042_enabled[port]); }
