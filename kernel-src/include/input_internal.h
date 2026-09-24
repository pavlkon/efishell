// SPDX-License-Identifier: GPL-2.0-only
#ifndef INPUT_INTERNAL_H
#define INPUT_INTERNAL_H
#include "kernel_internal.h"
extern k_mutex input_transport_lock;
void input_raw_push(UINT32 id, const void *bytes, UINT32 length, UINT32 flags);
void input_raw_error(UINT32 id);
BOOLEAN i8042_online(UINT32 port);
UINT32 usb_input_count(void);
int usb_input_get(UINT32 index, k_input_device *out);
void usb_input_pump(void);
#endif
