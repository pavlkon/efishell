// SPDX-License-Identifier: GPL-2.0-only
#ifndef HDA_HDMI_H
#define HDA_HDMI_H
#include "kernel_internal.h"
typedef int (*hda_verb_fn)(void *, UINT32, UINT32, UINT32 *);
int hda_hdmi_sink(void *, hda_verb_fn, UINT32 pin, k_audio_sink *);
int hda_hdmi_prepare(void *, hda_verb_fn, UINT32 pin, UINT32 converter, UINT16 vendor);
void hda_hdmi_stop(void *, hda_verb_fn, UINT32 pin, UINT32 converter);
#endif
