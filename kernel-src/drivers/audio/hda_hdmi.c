// SPDX-License-Identifier: GPL-2.0-only
#include "hda_hdmi.h"
int k_audio_eld_decode(const void *data, UINT32 bytes, k_audio_sink *out)
{
    if (!data || !out || bytes < 20 || bytes > 256)
        return K_EINVAL;
    const UINT8 *p = data;
    UINT32 version = p[0] >> 3, name = p[4] & 31, count = p[5] >> 4;
    if ((version != 2 && version != 31) || name > 16 || ((p[5] >> 2) & 3) > 1 ||
        20 + name + count * 3 > bytes)
        return K_EINVAL;
    mem_zero(out, sizeof(*out));
    out->flags = K_AUDIO_SINK_DIGITAL | K_AUDIO_SINK_ELD;
    out->connection = (p[5] >> 2) & 3;
    for (UINT32 i = 0; i < name; ++i)
        out->name[i] = p[20 + i] >= 32 && p[20 + i] < 127 ? (char)p[20 + i] : '?';
    for (UINT32 i = 0; i < count; ++i) {
        const UINT8 *sad = p + 20 + name + 3 * i;
        if (((sad[0] >> 3) & 15) != 1)
            continue;
        UINT32 channels = (sad[0] & 7) + 1;
        out->channels = (out->channels > channels ? out->channels : channels);
        out->rates |= sad[1] & 127;
        out->sample_bits |= sad[2] & 7;
        /* One descriptor must support the complete format, not a union of incompatible SADs. */
        if (channels >= 2 && (sad[1] & 4) && (sad[2] & 1))
            out->flags |= K_AUDIO_SINK_PCM48;
    }
    return 0;
}
int hda_hdmi_sink(void *ctx, hda_verb_fn verb, UINT32 pin, k_audio_sink *out)
{
    mem_zero(out, sizeof(*out));
    out->pin = pin;
    out->flags = K_AUDIO_SINK_DIGITAL;
    UINT32 sense, size;
    int e = verb(ctx, pin, 0xf0900, &sense);
    if (e)
        return e;
    if (!(sense & 0x80000000u))
        return 0;
    out->flags |= K_AUDIO_SINK_PRESENT;
    if (!(sense & 0x40000000u))
        return K_EAGAIN;
    e = verb(ctx, pin, 0xf2e08, &size);
    if (e)
        return e;
    if (size < 20 || size > 256)
        return K_EINVAL;
    UINT8 eld[256];
    for (UINT32 i = 0; i < size; ++i) {
        UINT32 byte;
        e = verb(ctx, pin, 0xf2f00 | i, &byte);
        if (e)
            return e;
        if (!(byte & 0x80000000u))
            return K_EAGAIN;
        eld[i] = (UINT8)byte;
    }
    k_audio_sink parsed;
    e = k_audio_eld_decode(eld, size, &parsed);
    if (!e) {
        *out = parsed;
        out->pin = pin;
        out->flags |= K_AUDIO_SINK_PRESENT;
    }
    return e;
}
int hda_hdmi_prepare(void *ctx, hda_verb_fn verb, UINT32 pin, UINT32 cvt, UINT16 vendor)
{
    k_audio_sink sink;
    int e = hda_hdmi_sink(ctx, verb, pin, &sink);
    if (e)
        return e;
    if (!(sink.flags & K_AUDIO_SINK_PRESENT))
        return K_ENOENT;
    if (!(sink.flags & K_AUDIO_SINK_PCM48))
        return K_ENOTSUP;
    UINT32 size, digital;
    if ((e = verb(ctx, pin, 0xf2e00, &size)))
        return e;
    if (size < 14 || size > 32)
        return K_ENOTSUP;
    UINT8 packet[32] = {0};
    packet[0] = 0x84;
    BOOLEAN hdmi_layout = !sink.connection || vendor == 0x10de;
    if (hdmi_layout) {
        packet[1] = sink.connection ? 0x1b : 1;
        packet[2] = sink.connection ? 0x44 : 10;
        packet[4] = 1;
        UINT8 sum = 0;
        for (UINT32 i = 0; i < 14; ++i)
            sum += packet[i];
        packet[3] = (UINT8)-sum;
    } else {
        packet[1] = 0x1b;
        packet[2] = 0x44;
        packet[3] = 1;
    }
    if ((e = verb(ctx, pin, 0x73000, NULL)) || (e = verb(ctx, pin, 0x73200, NULL)))
        return e;
    for (UINT32 i = 0; i < size; ++i)
        if ((e = verb(ctx, pin, 0x73100 | packet[i], NULL)))
            return e;
    for (UINT32 slot = 0; slot < 8; ++slot)
        if ((e = verb(ctx, pin, 0x73400 | ((slot < 2 ? slot : 15) << 4) | slot, NULL)))
            return e;
    if ((e = verb(ctx, cvt, 0x72d01, NULL)) || (e = verb(ctx, cvt, 0xf0d00, &digital)))
        return e;
    /* Two-channel consumer PCM; preserve unrelated converter controls. */
    digital = ((digital & 255) & ~0x6au) | 1;
    if ((e = verb(ctx, cvt, 0x70d00 | digital, NULL)))
        return e;
    return verb(ctx, pin, 0x732c0, NULL);
}
void hda_hdmi_stop(void *ctx, hda_verb_fn verb, UINT32 pin, UINT32 cvt)
{
    (void)verb(ctx, pin, 0x73200, NULL);
    UINT32 digital;
    if (!verb(ctx, cvt, 0xf0d00, &digital))
        (void)verb(ctx, cvt, 0x70d00 | ((digital & 255) & ~1u), NULL);
}
