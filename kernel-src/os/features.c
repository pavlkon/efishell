// SPDX-License-Identifier: GPL-2.0-only
#include "features.h"

static BOOLEAN equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}
static BOOLEAN prefix(const char *a, const char *b)
{
    while (*b)
        if (*a++ != *b++)
            return FALSE;
    return TRUE;
}
static BOOLEAN number(const char **s, UINT32 *value)
{
    while (**s == ' ')
        ++*s;
    if (**s < '0' || **s > '9')
        return FALSE;
    UINT64 n = 0;
    while (**s >= '0' && **s <= '9') {
        n = n * 10 + (UINT32)(*(*s)++ - '0');
        if (n > 0xffffffff)
            return FALSE;
    }
    while (**s == ' ')
        ++*s;
    *value = (UINT32)n;
    return TRUE;
}
static void result(const char *s, int e)
{
    con_print(s);
    con_print(": ");
    con_print(e ? k_strerror(e) : "ok");
    con_putc('\n');
}
static void report_local(const char *name, BOOLEAN ok)
{
    con_print(ok ? "PASS " : "FAIL ");
    con_print(name);
    con_putc('\n');
}
static void features(void)
{
    k_cpu_features f;
    if (k_cpu_features_get(&f))
        return;
    con_print(f.vendor);
    con_putc(' ');
    con_print(f.brand);
    con_putc('\n');
    con_print("family/model/stepping ");
    con_print_uint(f.family);
    con_putc('/');
    con_print_uint(f.model);
    con_putc('/');
    con_print_uint(f.stepping);
    con_print(" xstate bytes/mask ");
    con_print_uint(f.xstate_bytes);
    con_putc('/');
    con_print_hex(f.xstate_mask);
    con_putc('\n');
    con_print("XSAVE/AVX/AVX2 ");
    con_print_uint(!!(f.features & K_CPU_XSAVE));
    con_putc('/');
    con_print_uint(!!(f.features & K_CPU_AVX));
    con_putc('/');
    con_print_uint(!!(f.features & K_CPU_AVX2));
    con_print(" tickless ");
    con_print_uint(k_tickless_enabled());
    con_putc('\n');
}
static void heap_test(void (*report)(const char *, BOOLEAN))
{
    k_heap_info before, after;
    k_heap_get(&before);
    UINT8 *small = k_heap_alloc(73, 64), *large = k_heap_alloc(100000, 4096);
    BOOLEAN ok = small && large && !((UINTN)small & 63) && !((UINTN)large & 4095);
    if (small)
        for (UINT32 i = 0; i < 73; ++i) {
            if (small[i])
                ok = FALSE;
            small[i] = (UINT8)i;
        }
    report("heap alignment, zeroing and small/large allocations", ok);
    if (small) {
        UINT8 saved = small[73];
        small[73] ^= 1;
        report("heap detects overwritten allocation boundary", k_heap_free(small) == K_EFAULT);
        small[73] = saved;
        report("heap frees repaired allocation", !k_heap_free(small));
        report("heap rejects double free", k_heap_free(small) == K_EINVAL);
    }
    if (large)
        k_heap_free(large);
    k_heap_get(&after);
    report("heap returns pages when allocations are released",
           before.live_bytes == after.live_bytes &&
               before.committed_bytes == after.committed_bytes);
}
extern const UINT8 os_vm_probe[], os_vm_probe_end[];
static BOOLEAN page_fault_probe(void)
{
    UINT8 *image = k_heap_alloc(8192, 16);
    if (!image)
        return FALSE;
    rief_header_t *h = (void *)image;
    *h = (rief_header_t){.magic = RIEF_MAGIC,
                         .version_major = 1,
                         .architecture = k_architecture(),
                         .header_size = sizeof(*h),
                         .region_count = 1,
                         .region_table_offset = 96,
                         .string_table_offset = 128,
                         .string_table_size = 1};
    UINT64 code_bytes = (UINT64)(os_vm_probe_end - os_vm_probe);
    rief_region_t *r = (void *)(image + 96);
    *r = (rief_region_t){RIEF_REGION_R | RIEF_REGION_X, 4096, 4096, code_bytes, 4096};
    for (UINT64 i = 0; i < code_bytes; ++i)
        image[4096 + i] = os_vm_probe[i];
    k_process *p = k_process_create("lazy-fault-probe");
    BOOLEAN ok = FALSE;
    UINT32 tid;
    if (p && !k_rief_load(p, image, 8192, NULL, 0) && !k_process_start(p, &tid)) {
        UINT32 pid = k_process_id(p);
        UINT64 began = k_uptime_ms();
        k_process_info info = {0};
        while (k_uptime_ms() - began < 3000) {
            if (!k_process_query(pid, &info) && info.state == K_TASK_ZOMBIE) {
                ok = !info.exit_code && !info.fault_vector;
                break;
            }
            k_sleep(10);
        }
        if (info.state != K_TASK_ZOMBIE)
            (void)k_process_control(pid, K_CTL_KILL);
    }
    BOOLEAN reaped = !p;
    if (p)
        for (UINT32 i = 0; i < 100; ++i) {
            int e = k_process_destroy(p);
            if (!e) {
                reaped = TRUE;
                break;
            }
            if (e != K_EBUSY) {
                ok = FALSE;
                break;
            }
            k_sleep(10);
        }
    ok = ok && reaped;
    k_heap_free(image);
    return ok;
}
static void vm_test(void (*report)(const char *, BOOLEAN))
{
    k_address_space *as = k_as_create();
    UINT64 base = 0, size = 99, value = 0x9a712e30, out = 0;
    int e = as ? k_as_alloc_ex(as, 8192, 4096, 3, K_VM_LAZY, &base) : K_ENOMEM;
    report("lazy reservation starts without resident data pages",
           !e && !k_as_mapping_size(as, base, &size) && !size);
    if (!e) {
        e = k_copy_to_user(as, base + 4096, &value, 8);
        report("user copy commits a lazy page and preserves contents",
               !e && !k_copy_from_user(as, &out, base + 4096, 8) && out == value &&
                   !k_as_mapping_size(as, base + 4096, &size) && size == 4096);
        report("untouched lazy page remains nonresident",
               !k_as_mapping_size(as, base, &size) && !size);
        report("lazy mappings reject executable writable permissions",
               k_as_alloc_ex(as, 4096, 4096, 7, K_VM_LAZY, &out) == K_EINVAL);
    }
    if (as)
        k_as_destroy(as);
    as = k_as_create();
    base = 0;
    e = as ? k_as_alloc_ex(as, 0x200000, 0x200000, 3, K_VM_HUGE, &base) : K_ENOMEM;
    report("aligned user mapping uses a 2 MiB page",
           !e && !(base & 0x1fffff) && !k_as_mapping_size(as, base, &size) && size == 0x200000);
    if (!e)
        report("huge page last-byte access",
               !k_copy_to_user(as, base + 0x200000 - 8, &value, 8) &&
                   !k_copy_from_user(as, &out, base + 0x200000 - 8, 8) && out == value);
    if (as)
        k_as_destroy(as);
    report("Ring 3 page faults resolve lazy mappings and resume", page_fault_probe());
}
static void audio_outputs(UINT32 id)
{
    for (UINT32 i = 0; i < 256; ++i) {
        k_audio_sink sink = {0};
        int e = k_audio_output_get(id, i, &sink);
        if (e == K_ENOENT)
            break;
        if (e && e != K_EAGAIN) {
            result("audiooutputs", e);
            break;
        }
        con_print("pin ");
        con_print_uint(sink.pin);
        con_print(sink.flags & K_AUDIO_SINK_DIGITAL ? (sink.connection ? " DP " : " HDMI ")
                                                    : " analog ");
        con_print(sink.name);
        con_print(" flags/channels/rates/bits ");
        con_print_hex(sink.flags);
        con_putc('/');
        con_print_uint(sink.channels);
        con_putc('/');
        con_print_hex(sink.rates);
        con_putc('/');
        con_print_hex(sink.sample_bits);
        con_putc('\n');
        if (e)
            result("monitor ELD", e);
    }
}
static void audio_eld_test(void (*report)(const char *, BOOLEAN))
{
    UINT8 eld[32] = {0};
    k_audio_sink sink;
    eld[0] = 2 << 3;
    eld[2] = 8;
    eld[4] = 4;
    eld[5] = 1 << 4;
    eld[20] = 'T';
    eld[21] = 'e';
    eld[22] = 's';
    eld[23] = 't';
    eld[24] = (1 << 3) | 1;
    eld[25] = 4;
    eld[26] = 1;
    report("ELD parses stereo PCM48 and monitor name",
           !k_audio_eld_decode(eld, 32, &sink) && sink.channels == 2 && equal(sink.name, "Test") &&
               (sink.flags & K_AUDIO_SINK_PCM48));
    report("ELD rejects truncated audio descriptors",
           k_audio_eld_decode(eld, 26, &sink) == K_EINVAL);
    eld[5] = (2 << 4) | 4;
    eld[25] = 2;
    eld[27] = (1 << 3) | 1;
    eld[28] = 4;
    eld[29] = 4;
    report("ELD does not combine incompatible audio formats",
           !k_audio_eld_decode(eld, 32, &sink) && sink.connection == 1 &&
               !(sink.flags & K_AUDIO_SINK_PCM48));
    eld[4] = 31;
    report("ELD rejects invalid monitor name length",
           k_audio_eld_decode(eld, 32, &sink) == K_EINVAL);
}
static void usb_test(void)
{
    int e = k_usb_rescan();
    result("USB scan", e);
    UINT8 *sector = k_heap_alloc(4096, 16);
    if (!sector) {
        result("USB test", K_ENOMEM);
        return;
    }
    UINT32 found = 0;
    for (UINT32 i = 0; i < k_disk_count(); ++i) {
        k_disk_info d;
        if (k_disk_get(i, &d) || (!equal(d.driver, "usb-bot") && !equal(d.driver, "usb-uas")))
            continue;
        ++found;
        con_print(d.name);
        con_putc(' ');
        con_print(d.driver);
        con_putc(' ');
        if (!d.online)
            report_local("removed USB disk rejects stale handle",
                         k_disk_read(i, 0, 1, sector, 4096) == K_EIO);
        else {
            report_local("USB sector read", !k_disk_read(i, 0, 1, sector, 4096));
            report_local("raw physical writes remain fenced",
                         k_disk_write(i, 0, 1, sector, 4096) == K_EROFS);
        }
    }
    if (!found)
        con_print("SKIP USB storage: no registered USB disks\n");
    k_heap_free(sector);
}

void os_feature_tests(void (*report)(const char *, BOOLEAN))
{
    if (!report)
        report = report_local;
    audio_eld_test(report);
    heap_test(report);
    vm_test(report);
    con_print("Testing AVX state save/restore...\n");
    int e = k_cpu_vector_test();
    if (e == K_ENOTSUP)
        con_print("SKIP AVX: unavailable on this CPU\n");
    else
        report("XSAVE header is valid and YMM state survives scheduler yields", !e);
    UINT64 before = k_uptime_ms();
    k_sleep(30);
    UINT64 delta = k_uptime_ms() - before;
    report("timer deadlines advance with tickless/periodic mode", delta >= 30 && delta < 1000);
}
static void power(void)
{
    k_power_info p;
    int e = k_power_get(&p);
    if (e) {
        result("power", e);
        return;
    }
    con_print("CPU ");
    con_print_uint(p.cpu);
    con_print(" supported flags ");
    con_print_hex(p.features);
    con_print(" min/max/current/preference ");
    con_print_uint(p.minimum_level);
    con_putc('/');
    con_print_uint(p.maximum_level);
    con_putc('/');
    con_print_uint(p.current_level);
    con_putc('/');
    con_print_uint(p.preference);
    con_putc('\n');
    if (p.temperature_millic != (-2147483647 - 1)) {
        con_print("temperature mC ");
        con_print_int(p.temperature_millic);
        con_print(" throttled ");
        con_print_uint(p.throttled);
        con_putc('\n');
    } else
        con_print("Temperature backend unavailable on this CPU.\n");
    con_print("APERF/MPERF ");
    con_print_uint(p.actual_cycles);
    con_putc('/');
    con_print_uint(p.reference_cycles);
    con_putc('\n');
}
static void audio_list(void)
{
    for (UINT32 i = 0; i < k_audio_count(); ++i) {
        k_audio_info a;
        if (k_audio_get(i, &a))
            continue;
        con_print_uint(i);
        con_putc(' ');
        con_print(a.driver);
        con_print(" PCI ");
        con_print_hex(a.vendor);
        con_putc('/');
        con_print_hex(a.device);
        con_print(" playback/capture ");
        con_print_uint(!!(a.capabilities & K_AUDIO_PLAYBACK));
        con_putc('/');
        con_print_uint(!!(a.capabilities & K_AUDIO_CAPTURE));
        con_print(" codec/pin/converter ");
        con_print_uint(a.codec);
        con_putc('/');
        con_print_uint(a.pin);
        con_putc('/');
        con_print_uint(a.converter);
        con_putc('\n');
        if (a.error)
            result("audio", a.error);
    }
    if (!k_audio_count())
        con_print("No supported HDA controller found.\n");
}
static void audio_test(UINT32 id, BOOLEAN capture)
{
    const UINT32 bytes = 19200;
    INT16 *pcm = k_heap_alloc(bytes, 16);
    if (!pcm) {
        result("audio", K_ENOMEM);
        return;
    }
    for (UINT32 i = 0; i < bytes / 4; ++i) {
        INT32 phase = (INT32)((i * 440) % 48000);
        INT16 sample = (INT16)(phase < 24000 ? phase / 12 - 1000 : 3000 - phase / 12);
        pcm[i * 2] = pcm[i * 2 + 1] = sample;
    }
    int e = k_audio_transfer(id, capture ? K_AUDIO_CAPTURE : K_AUDIO_PLAYBACK, pcm, bytes, 1000);
    result(capture ? "capture" : "playback", e);
    if (!e && capture) {
        UINT32 peak = 0;
        for (UINT32 i = 0; i < bytes / 2; ++i) {
            UINT32 v = pcm[i] < 0 ? (UINT32) - (INT32)pcm[i] : (UINT32)pcm[i];
            if (v > peak)
                peak = v;
        }
        con_print("Captured PCM peak ");
        con_print_uint(peak);
        con_putc('\n');
    }
    k_heap_free(pcm);
}
static void display_list(void)
{
    for (UINT32 i = 0; i < k_display_count(); ++i) {
        k_display_info d;
        if (k_display_get(i, &d))
            continue;
        con_print_uint(i);
        con_putc(' ');
        con_print(d.driver);
        con_putc(' ');
        con_print_uint(d.width);
        con_putc('x');
        con_print_uint(d.height);
        con_print(" bits ");
        con_print_uint(d.bits);
        con_print(" modesetting ");
        con_print_uint(!!(d.capabilities & K_DISPLAY_MODESET));
        con_putc('\n');
    }
}
static void display_test(UINT32 id)
{
    k_display_info d;
    int e = k_display_get(id, &d);
    if (e) {
        result("display", e);
        return;
    }
    UINT32 w = (d.capabilities & K_DISPLAY_VGA) ? 320 : d.width,
           h = (d.capabilities & K_DISPLAY_VGA) ? 200 : d.height;
    if ((d.capabilities & K_DISPLAY_MODESET) && !(d.capabilities & K_DISPLAY_VGA)) {
        w = 800;
        h = 600;
    }
    UINT32 *row = k_heap_alloc((UINT64)w * 4, 16);
    if (!row) {
        result("display", K_ENOMEM);
        return;
    }
    con_print("Display pattern for two seconds; original mode will be restored.\n");
    e = k_display_mode(id, w, h, d.bits);
    if (!e) {
        for (UINT32 y = 0; y < h && !e; ++y) {
            for (UINT32 x = 0; x < w; ++x)
                row[x] = 0xff000000 | ((x * 255 / w) << 16) | ((y * 255 / h) << 8) | 64;
            e = k_display_write(id, 0, y, w, 1, row, w);
        }
        k_sleep(2000);
        int restored = k_display_restore(id);
        if (!e)
            e = restored;
    }
    k_heap_free(row);
    result("display", e);
}
void os_feature_help(void)
{
    con_print("cpufeatures | featuretest | heaptest | vmfeaturetest | avxtest\n");
    con_print("power | power set <min> <max> <preference> | tickless on/off\n");
    con_print("perf start/read/stop | audio | audiotest <id> | audiocapture <id>\n");
    con_print("audiooutputs <id> | audioout <id> <pin> | eldtest | usbscan | usbtest\n");
    con_print("display | displaytest <id> (pattern, then restore)\n");
}
BOOLEAN os_feature_command(const char *line)
{
    if (equal(line, "cpufeatures"))
        features();
    else if (equal(line, "featuretest"))
        os_feature_tests(report_local);
    else if (equal(line, "eldtest"))
        audio_eld_test(report_local);
    else if (equal(line, "usbtest"))
        usb_test();
    else if (equal(line, "usbscan")) {
        int e = k_usb_rescan();
        if (!e)
            e = k_vfs_mount_disks();
        result("USB scan", e);
    } else if (prefix(line, "audiooutputs ")) {
        const char *s = line + 13;
        UINT32 id;
        if (!number(&s, &id) || *s)
            result("audiooutputs", K_EINVAL);
        else
            audio_outputs(id);
    } else if (prefix(line, "audioout ")) {
        const char *s = line + 9;
        UINT32 id, pin;
        if (!number(&s, &id) || !number(&s, &pin) || *s)
            result("audioout", K_EINVAL);
        else
            result("audioout", k_audio_select_output(id, pin));
    } else if (equal(line, "heaptest"))
        heap_test(report_local);
    else if (equal(line, "vmfeaturetest"))
        vm_test(report_local);
    else if (equal(line, "avxtest"))
        result("AVX", k_cpu_vector_test());
    else if (equal(line, "power"))
        power();
    else if (prefix(line, "power set ")) {
        const char *s = line + 10;
        UINT32 a, b, c;
        if (!number(&s, &a) || !number(&s, &b) || !number(&s, &c) || *s)
            result("power", K_EINVAL);
        else
            result("power", k_power_set(a, b, c));
    } else if (equal(line, "tickless on"))
        result("tickless", k_tickless_set(TRUE));
    else if (equal(line, "tickless off"))
        result("tickless", k_tickless_set(FALSE));
    else if (equal(line, "perf start"))
        result("perf", k_perf_start());
    else if (equal(line, "perf stop"))
        result("perf", k_perf_stop());
    else if (equal(line, "perf read")) {
        k_perf_info p;
        int e = k_perf_read(&p);
        if (e)
            result("perf", e);
        else {
            con_print("CPU cycles/instructions ");
            con_print_uint(p.cpu);
            con_putc(' ');
            con_print_uint(p.cycles);
            con_putc('/');
            con_print_uint(p.instructions);
            con_putc('\n');
        }
    } else if (equal(line, "audio"))
        audio_list();
    else if (prefix(line, "audiotest ") || prefix(line, "audiocapture ")) {
        BOOLEAN capture = prefix(line, "audiocapture ");
        const char *s = line + (capture ? 13 : 10);
        UINT32 id;
        if (!number(&s, &id) || *s)
            result("audio", K_EINVAL);
        else
            audio_test(id, capture);
    } else if (equal(line, "display"))
        display_list();
    else if (prefix(line, "displaytest ")) {
        const char *s = line + 12;
        UINT32 id;
        if (!number(&s, &id) || *s)
            result("display", K_EINVAL);
        else
            display_test(id);
    } else
        return FALSE;
    return TRUE;
}
