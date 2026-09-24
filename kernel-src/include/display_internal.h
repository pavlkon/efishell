// SPDX-License-Identifier: GPL-2.0-only
#ifndef DISPLAY_INTERNAL_H
#define DISPLAY_INTERNAL_H
#include "kernel_internal.h"
typedef struct {
    k_display_info info;
    int (*mode)(void *, k_display_info *, UINT32, UINT32, UINT32);
    int (*restore)(void *);
    void *context;
    k_display_info initial;
    BOOLEAN active;
} display_device;
int display_register(const display_device *);
void display_bochs_probe(void);
void display_vga_probe(void);
void console_display_pause(BOOLEAN);
#endif
