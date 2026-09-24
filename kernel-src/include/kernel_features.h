// SPDX-License-Identifier: GPL-2.0-only
#ifndef KERNEL_FEATURES_H
#define KERNEL_FEATURES_H

#define K_CPU_XSAVE 1u
#define K_CPU_AVX 2u
#define K_CPU_AVX2 4u
#define K_CPU_INVARIANT_TSC 8u
#define K_CPU_HWP 16u
#define K_CPU_AMD_PSTATE 32u
#define K_CPU_THERMAL 64u
#define K_CPU_APERF 128u
typedef struct {
    char vendor[16], brand[48];
    UINT32 family, model, stepping, features, xstate_bytes, cpu;
    UINT64 xstate_mask;
} k_cpu_features;
UINT16 k_architecture(void);
int k_cpu_features_get(k_cpu_features *);
int k_cpu_vector_test(void); /* Preserves caller state; checks vector state across yields. */

typedef struct {
    UINT64 committed_bytes, live_bytes, allocations, errors;
} k_heap_info;
UINT64 k_pmm_alloc_aligned(UINT32 pages, UINT32 alignment_pages, UINT64 maximum);
void *k_heap_alloc(UINT64 bytes, UINT32 alignment);
int k_heap_free(void *);
int k_heap_get(k_heap_info *);
#define K_VM_LAZY 1u
#define K_VM_HUGE 2u
int k_as_alloc_ex(k_address_space *, UINT64 bytes, UINT64 alignment, UINT32 rwx, UINT32 options,
                  UINT64 *base);
int k_as_mapping_size(k_address_space *, UINT64 address, UINT64 *bytes);

typedef struct {
    UINT32 cpu, features, current_level, minimum_level, maximum_level, preference;
    INT32 temperature_millic;
    UINT32 throttled;
    UINT64 actual_cycles, reference_cycles;
} k_power_info;
int k_power_get(k_power_info *); /* Snapshot of the calling CPU. */
int k_power_set(UINT32 minimum, UINT32 maximum, UINT32 preference);
int k_tickless_set(BOOLEAN enabled);
BOOLEAN k_tickless_enabled(void);
#define K_PERF_CYCLES 1u
#define K_PERF_INSTRUCTIONS 2u
typedef struct {
    UINT32 cpu, active, width, reserved;
    UINT64 cycles, instructions;
} k_perf_info;
int k_perf_start(void);
int k_perf_read(k_perf_info *);
int k_perf_stop(void);

#define K_AUDIO_PLAYBACK 1u
#define K_AUDIO_CAPTURE 2u
#define K_AUDIO_DIGITAL 4u
typedef struct {
    UINT32 id, capabilities, codec, pin, converter, rate, channels, bits;
    UINT16 vendor, device;
    INT32 error;
    char driver[24];
} k_audio_info;
int k_audio_init(void);
int k_usb_storage_init(void);
int k_usb_rescan(void);
UINT32 k_audio_count(void);
int k_audio_get(UINT32, k_audio_info *);
int k_audio_transfer(UINT32, UINT32 direction, void *pcm, UINT32 bytes, UINT32 timeout_ms);

#define K_AUDIO_SINK_DIGITAL 1u
#define K_AUDIO_SINK_PRESENT 2u
#define K_AUDIO_SINK_ELD 4u
#define K_AUDIO_SINK_PCM48 8u
typedef struct {
    UINT32 pin, flags, connection, channels, rates, sample_bits;
    char name[32];
} k_audio_sink;
int k_audio_eld_decode(const void *, UINT32, k_audio_sink *);
int k_audio_output_get(UINT32 device, UINT32 index, k_audio_sink *);
int k_audio_select_output(UINT32 device, UINT32 pin);

#define K_DISPLAY_BOOT 1u
#define K_DISPLAY_MODESET 2u
#define K_DISPLAY_VGA 4u
typedef struct {
    UINT32 id, width, height, pitch, bits, capabilities;
    UINT64 framebuffer, bytes;
    UINT8 red_shift, green_shift, blue_shift, reserved;
    char driver[24];
} k_display_info;
int k_display_init(UINT64 framebuffer_bytes, UINT8 red, UINT8 green, UINT8 blue);
UINT32 k_display_count(void);
int k_display_get(UINT32, k_display_info *);
int k_display_mode(UINT32 id, UINT32 width, UINT32 height, UINT32 bits);
int k_display_write(UINT32 id, UINT32 x, UINT32 y, UINT32 width, UINT32 height, const UINT32 *argb,
                    UINT32 stride);
int k_display_restore(UINT32 id);

#endif
