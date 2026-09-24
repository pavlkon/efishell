// SPDX-License-Identifier: GPL-2.0-only
#ifndef USB_STORAGE_INTERNAL_H
#define USB_STORAGE_INTERNAL_H
#include "kernel_internal.h"
typedef struct {
    UINT32 id;
    UINT16 vendor, product;
    UINT8 interface_number, protocol, streams, online;
} usb_bulk_info;
UINT32 usb_bulk_count(void);
int usb_bulk_get(UINT32, usb_bulk_info *);
int usb_bulk_submit(UINT32, UINT32, const void *, UINT32);
int usb_bulk_result(UINT32, UINT32, void *, UINT32, UINT32 *);
void usb_bulk_abort(UINT32);
int usb_uas_command(UINT32, const UINT8 *, UINT32, BOOLEAN, void *, UINT32, UINT32 *);
int usb_bulk_transfer(UINT32, BOOLEAN, void *, UINT32, UINT32 *);
int usb_storage_init(void);
int usb_storage_read(UINT32, UINT64, UINT32, void *);
int usb_storage_write(UINT32, UINT64, UINT32, const void *);
int usb_storage_flush(UINT32);
int storage_attach_usb(UINT32, UINT32, UINT64, const char *, BOOLEAN, BOOLEAN);
void storage_detach_usb(UINT32);
#endif
