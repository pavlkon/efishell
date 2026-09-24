// SPDX-License-Identifier: GPL-2.0-only
#include "usb_storage_internal.h"

/* Serialized LUN 0 commands; tag/stream 1. The caller holds the storage transport lock. */
int usb_uas_command(UINT32 link, const UINT8 *cdb, UINT32 cdb_bytes, BOOLEAN input, void *data,
                    UINT32 bytes, UINT32 *actual)
{
    if (!cdb || !cdb_bytes || cdb_bytes > 16 || bytes > 4096 || (bytes && !data))
        return K_EINVAL;
    usb_bulk_info info;
    int e = usb_bulk_get(link, &info);
    if (e || !info.online || info.protocol != 0x62)
        return e ? e : K_EIO;
    if (actual)
        *actual = 0;
    UINT8 command[32] = {1, 0, 0, 1}, status[512] = {0};
    mem_copy(command + 16, cdb, cdb_bytes);
    BOOLEAN command_done = FALSE, data_started = FALSE, data_done = !bytes, status_done = FALSE;
    UINT32 transferred = 0, pipe = input ? 2 : 3;
    UINT8 scsi_status = 0;
    e = usb_bulk_submit(link, 1, status, sizeof(status));
    if (e)
        goto broken;
    if (bytes && info.streams) {
        e = usb_bulk_submit(link, pipe, data, bytes);
        if (e)
            goto broken;
        data_started = TRUE;
    }
    e = usb_bulk_submit(link, 0, command, sizeof(command));
    if (e)
        goto broken;
    UINT64 start = k_uptime_ms();
    for (UINT32 bounded = 0; bounded < 3000 && k_uptime_ms() - start < 3000; ++bounded) {
        UINT32 got;
        if (!command_done) {
            e = usb_bulk_result(link, 0, command, sizeof(command), &got);
            if (e && e != K_EAGAIN)
                goto broken;
            if (!e) {
                if (got != sizeof(command)) {
                    e = K_EIO;
                    goto broken;
                }
                command_done = TRUE;
            }
        }
        if (data_started && !data_done) {
            e = usb_bulk_result(link, pipe, data, bytes, &got);
            if (e && e != K_EAGAIN)
                goto broken;
            if (!e) {
                data_done = TRUE;
                transferred = got;
            }
        }
        if (!status_done) {
            e = usb_bulk_result(link, 1, status, sizeof(status), &got);
            if (e && e != K_EAGAIN)
                goto broken;
            if (!e) {
                if (got < 4 || status[1] || status[2] || status[3] != 1) {
                    e = K_EIO;
                    goto broken;
                }
                if (status[0] == 6 || status[0] == 7) {
                    if (info.streams || !bytes || data_started || status[0] != (input ? 6 : 7)) {
                        e = K_EIO;
                        goto broken;
                    }
                    /* Rearm status before allowing the target to complete the data phase. */
                    e = usb_bulk_submit(link, 1, status, sizeof(status));
                    if (!e)
                        e = usb_bulk_submit(link, pipe, data, bytes);
                    if (e)
                        goto broken;
                    data_started = TRUE;
                } else if (status[0] == 3) {
                    if (got < 16 || (((UINT32)status[14] << 8) | status[15]) > got - 16) {
                        e = K_EIO;
                        goto broken;
                    }
                    scsi_status = status[6];
                    status_done = TRUE;
                    if (scsi_status && !data_started)
                        data_done = TRUE;
                    /* Error status can terminate a stream without completing its queued data.
                     * Quarantine that link instead of recycling live DMA or stale stream tags. */
                    if (scsi_status && !data_done) {
                        e = K_EIO;
                        goto broken;
                    }
                } else {
                    e = K_EIO;
                    goto broken;
                }
            }
        }
        if (command_done && status_done && data_done) {
            if (actual)
                *actual = transferred;
            return scsi_status ? K_EIO : 0;
        }
        k_sleep(1);
    }
    e = K_ETIMEDOUT;
broken:
    usb_bulk_abort(link);
    return e;
}
