// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"
int k_cpu_features_get(k_cpu_features *out) { return arch_cpu_features_get(out); }
int k_cpu_vector_test(void) { return arch_cpu_vector_test(); }
int k_power_get(k_power_info *out) { return arch_power_get(out); }
int k_power_set(UINT32 lo, UINT32 hi, UINT32 preference)
{
    return arch_power_set(lo, hi, preference);
}
int k_perf_start(void) { return arch_perf_start(); }
int k_perf_read(k_perf_info *out) { return arch_perf_read(out); }
int k_perf_stop(void) { return arch_perf_stop(); }
int k_tickless_set(BOOLEAN enabled) { return arch_tickless_set(enabled); }
BOOLEAN k_tickless_enabled(void) { return arch_tickless_enabled(); }

UINT16 k_architecture(void) { return ARCH_RIEF_MACHINE; }
