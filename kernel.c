// SPDX-License-Identifier: GPL-2.0-only
#include "kernel.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define PHYS_MASK 0x000ffffffffff000ULL
#define PAGE_LARGE (1ULL << 7)
#define PMM_PAGE_SIZE 4096ULL
#define PMM_MAX_PAGES (256ULL * 262144ULL)
#define USER_SLOT 128
#define STACK_PAGES 8
#define MAX_PROCESSES 24
#define MAX_AS 32
#define MAX_USER_PAGES 16384
#define MAX_REGIONS 64
#define MAX_EXPORTS 64
#define MAIL_DEPTH 16

static void mem_zero(void *p, UINTN n)
{
    UINT8 *d = p;
    while (n--)
        *d++ = 0;
}
static void mem_copy(void *d, const void *s, UINTN n)
{
    UINT8 *a = d;
    const UINT8 *b = s;
    while (n--)
        *a++ = *b++;
}
/* GCC may emit these even in a freestanding PE image. */
void *memset(void *p, int v, size_t n)
{
    UINT8 *d = p;
    while (n--)
        *d++ = (UINT8)v;
    return p;
}
void *memcpy(void *d, const void *s, size_t n)
{
    mem_copy(d, s, n);
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    UINT8 *a = d;
    const UINT8 *b = s;
    if ((UINTN)a < (UINTN)b)
        return memcpy(d, s, n);
    while (n) {
        --n;
        a[n] = b[n];
    }
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const UINT8 *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        ++x;
        ++y;
    }
    return 0;
}
/* MinGW emits this for large stack frames even with -fno-stack-check.
 * Preserve its special ABI (RAX contains the requested size; all GPRs survive)
 * so the original -nostdlib build needs no libgcc/CRT dependency. Stacks are
 * fully allocated: probing does not implement automatic stack growth. */
__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n"
        "pushq %rcx\npushq %rax\nleaq 24(%rsp),%rcx\n"
        "1: cmpq $4096,%rax\njb 2f\nsubq $4096,%rcx\ntestq %rcx,(%rcx)\n"
        "subq $4096,%rax\njmp 1b\n"
        "2: subq %rax,%rcx\ntestq %rcx,(%rcx)\npopq %rax\npopq %rcx\nret\n");
static UINTN str_len(const char *s)
{
    UINTN n = 0;
    while (s[n])
        ++n;
    return n;
}
static BOOLEAN str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}
static void str_copy(char *d, const char *s, UINTN cap)
{
    if (!cap)
        return;
    UINTN i = 0;
    while (i + 1 < cap && s[i]) {
        d[i] = s[i];
        ++i;
    }
    d[i] = 0;
}
static BOOLEAN span_ok(UINT64 off, UINT64 n, UINT64 size)
{
    return off <= size && n <= size - off;
}
static BOOLEAN overlap(UINT64, UINT64, UINT64, UINT64);
static BOOLEAN power2(UINT64 n)
{
    return n && !(n & (n - 1));
}
static UINT16 rd16(const void *p)
{
    const UINT8 *b = p;
    return b[0] | ((UINT16)b[1] << 8);
}
static UINT32 rd32(const void *p)
{
    const UINT8 *b = p;
    return rd16(b) | ((UINT32)rd16(b + 2) << 16);
}
static UINT64 rd64(const void *p)
{
    const UINT8 *b = p;
    return rd32(b) | ((UINT64)rd32(b + 4) << 32);
}
static void wr32(void *p, UINT32 v)
{
    UINT8 *b = p;
    for (int i = 0; i < 4; ++i)
        b[i] = (UINT8)(v >> (i * 8));
}
static void wr64(void *p, UINT64 v)
{
    UINT8 *b = p;
    for (int i = 0; i < 8; ++i)
        b[i] = (UINT8)(v >> (i * 8));
}
static inline void pause_cpu(void)
{
    __asm__ volatile("pause" ::: "memory");
}
static inline UINT8 inb(UINT16 p)
{
    UINT8 v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline UINT16 inw(UINT16 p)
{
    UINT16 v;
    __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline UINT32 inl(UINT16 p)
{
    UINT32 v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void outb(UINT16 p, UINT8 v)
{
    __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p));
}
static inline void outw(UINT16 p, UINT16 v)
{
    __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(p));
}
static inline void outl(UINT16 p, UINT32 v)
{
    __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p));
}
static UINT64 rdmsr(UINT32 m)
{
    UINT32 a, d;
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(m));
    return a | ((UINT64)d << 32);
}
static void wrmsr(UINT32 m, UINT64 v)
{
    __asm__ volatile("wrmsr" ::"c"(m), "a"((UINT32)v), "d"((UINT32)(v >> 32)) : "memory");
}
static void cpuid(UINT32 leaf, UINT32 sub, UINT32 *a, UINT32 *b, UINT32 *c, UINT32 *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
static UINT64 rdtsc(void)
{
    UINT32 a, d;
    __asm__ volatile("lfence; rdtsc" : "=a"(a), "=d"(d)::"memory");
    return a | ((UINT64)d << 32);
}
UINT64 k_irq_save(void)
{
    UINT64 f;
    __asm__ volatile("pushfq;popq %0;cli" : "=r"(f)::"memory");
    return f;
}
void k_irq_restore(UINT64 f)
{
    if (f & (1 << 9))
        __asm__ volatile("sti" ::: "memory");
}
UINT64 k_spin_lock(k_spinlock *l)
{
    UINT64 f = k_irq_save();
    while (__atomic_exchange_n(&l->value, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->value, __ATOMIC_RELAXED))
            pause_cpu();
    }
    return f;
}
void k_spin_unlock(k_spinlock *l, UINT64 f)
{
    __atomic_store_n(&l->value, 0, __ATOMIC_RELEASE);
    k_irq_restore(f);
}
const char *k_strerror(int e)
{
    static const char *names[] = {"ok",
                                  "invalid argument",
                                  "out of memory",
                                  "not found",
                                  "busy",
                                  "invalid memory",
                                  "unsupported",
                                  "I/O error",
                                  "timeout",
                                  "read-only",
                                  "try again",
                                  "permission denied",
                                  "limit exceeded",
                                  "invalid RIEF",
                                  "deadlock",
                                  "already exists",
                                  "no space left on volume"};
    return e <= 0 && (UINT32)(-e) < ARRAY_LEN(names) ? names[-e] : "unknown error";
}
static void console_panic_mode(void);
__attribute__((noreturn)) void kernel_halt(const char *s)
{
    __asm__ volatile("cli");
    console_panic_mode();
    con_print("\nHALT: ");
    con_print(s);
    con_print("\n");
    for (;;)
        __asm__ volatile("cli;hlt");
}

typedef struct {
    UINT64 r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rdx, rcx, rbx, rax;
    UINT64 vector, error, rip, cs, rflags, rsp, ss;
} irq_frame;
_Static_assert(offsetof(irq_frame, vector) == 120, "interrupt ABI");
_Static_assert(sizeof(irq_frame) == 176, "interrupt ABI");
#pragma pack(push, 1)
typedef struct {
    UINT16 limit;
    UINT64 base;
} descriptor_ptr;
typedef struct {
    UINT16 low, cs;
    UINT8 ist, attr;
    UINT16 middle;
    UINT32 high, zero;
} idt_entry;
typedef struct {
    UINT32 reserved0;
    UINT64 rsp[3];
    UINT64 reserved1;
    UINT64 ist[7];
    UINT64 reserved2;
    UINT16 reserved3, iomap;
} tss64;
#pragma pack(pop)
typedef struct task {
    UINT32 id, cpu, state, weight;
    char name[32];
    irq_frame *frame;
    UINT64 stack, stack_top, vruntime, runtime, switches, wake;
    k_task_entry entry;
    void *arg;
    k_process *process;
    void *wait_object;
    INT32 exit_code;
    UINT32 preempt_depth, mail_head, mail_count;
    BOOLEAN idle;
    k_message mail[MAIL_DEPTH];
    UINT8 fx[512] __attribute__((aligned(16)));
} task;
typedef struct {
    UINT64 syscall_stack, syscall_user_rsp;
    task *stack_owner, *current;
    UINT32 index, apic_id;
    volatile UINT32 online;
    UINT64 ticks, switches, fair_floor, idle_ticks;
    UINT32 slice, work_pending;
    task *idle;
    UINT64 gdt[7] __attribute__((aligned(16)));
    tss64 tss;
    UINT8 fault_stack[8192] __attribute__((aligned(16)));
    UINT8 nmi_stack[8192] __attribute__((aligned(16)));
    UINT8 machine_stack[8192] __attribute__((aligned(16)));
} cpu_state;
_Static_assert(offsetof(cpu_state, stack_owner) == 16, "stack ownership ABI");
_Static_assert(offsetof(cpu_state, current) == 24, "current task ABI");
struct k_address_space {
    BOOLEAN used;
    UINT64 *pml4, next;
    UINT32 pages;
    k_process *owner;
};
struct k_process {
    BOOLEAN used, loaded;
    UINT32 id, task_id, state, fault_vector;
    INT32 exit_code;
    UINT64 fault_address, fault_ip, entry, user_stack;
    char name[32];
    k_address_space *as;
    UINT64 region_base[MAX_REGIONS], region_size[MAX_REGIONS];
    UINT32 region_count, export_count;
    struct {
        char name[128];
        UINT64 address;
    } exports[MAX_EXPORTS];
    UINT32 grants[16], grant_count;
    k_file files[16];
    BOOLEAN fd_used[16];
    UINT32 fd_flags[16];
    UINT64 fd_offset[16];
};
static cpu_state cpus[K_MAX_CPUS];
static task tasks[K_MAX_TASKS];
static struct k_process processes[MAX_PROCESSES];
static struct k_address_space spaces[MAX_AS];
static k_spinlock sched_lock = K_SPINLOCK_INIT, pmm_lock = K_SPINLOCK_INIT,
                  vm_lock = K_SPINLOCK_INIT;
static k_spinlock console_lock = K_SPINLOCK_INIT;
static BOOLEAN scheduler_ready, interrupts_ready, vm_ready, smp_started;
static UINT32 next_task_id = 1, next_process_id = 1, timer_hz = 100;
static UINT64 global_ticks, ap_trampoline, tsc_hz;
static UINT64 saved_pat, saved_mtrr_default, saved_mtrr_fixed[11], saved_mtrr_variable[64];
static UINT32 saved_mtrr_count, saved_phys_bits;
static BOOLEAN has_pat, has_mtrr, mtrr_fixed_supported;
static UINT32 lapic_period;
static volatile UINT8 *lapic_regs;
static k_platform_info platform = {.ps2 = TRUE,
                                   .pic = TRUE,
                                   .dma_allowed = TRUE,
                                   .clock_source = "unavailable",
                                   .timer_source = "off",
                                   .dma_reason = "no active remapping detected"};
static idt_entry idt[256] __attribute__((aligned(16)));
static descriptor_ptr idtr;
static UINT8 initial_fx[512] __attribute__((aligned(16)));
static void lapic_write(UINT32, UINT32);
static UINT32 lapic_read(UINT32);
static void lapic_eoi(void);
static void cpu_tables(cpu_state *);
static irq_frame *schedule(irq_frame *, BOOLEAN);
static INT64 syscall_dispatch(irq_frame *);
static void ps2_irq(void);
static void wake_object(void *);
static void free_task(task *);
static task *current_task(void);

/* Retained framebuffer, font and console implementation follows. */

/* Framebuffer state */
static UINT32 *fb_base;
/* Text cells are authoritative. Never read GPU memory to scroll. The second
 * grid records what has already been drawn, so unchanged blank cells cost no
 * PCIe writes. A complete con_print() is presented once, even after scrolling. */
static char *con_cells, *con_shown, *con_history;
#define CON_HISTORY_ROWS 2048
static UINT32 history_head, history_count, history_view;
/* One generic binary-alpha bitmap overlay. The OS supplies its contents and
 * position; the console restores it from RAM, never by reading GPU memory. */
static struct {
    INT32 x, y;
    UINT32 width, height, pixels[32 * 32];
} fb_overlay;
static UINT32 con_dirty_first = ~0U, con_dirty_last;
static BOOLEAN fb_pat_wc;
static volatile UINT32 console_emergency;
static UINT32 fb_width, fb_height, fb_pitch;
static UINT8 red_shift, green_shift, blue_shift;

/* Text console state */
#define FONT_W 8
#define FONT_H 8
static UINT32 con_cols, con_rows;
static UINT32 cur_x, cur_y;
static UINT32 fg_color = 0x00E0E0E0;
static UINT32 bg_color = 0x00101018;

// 8x8 font

typedef struct {
    char ch;
    UINT8 rows[8];
} glyph_t;

static CONST glyph_t FONT[] = {
    {'@', {0x3c, 0x42, 0x5e, 0x52, 0x5e, 0x40, 0x3e, 0}},
    {'[', {0x3c, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3c, 0}},
    {']', {0x3c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x3c, 0}},
    {'#', {0x24, 0x24, 0x7e, 0x24, 0x7e, 0x24, 0x24, 0}},
    {'%', {0x62, 0x64, 0x08, 0x10, 0x26, 0x46, 0, 0}},
    {'|', {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'0', {0x00, 0x3C, 0x66, 0x6E, 0x76, 0x66, 0x3C, 0x00}},
    {'1', {0x00, 0x18, 0x38, 0x18, 0x18, 0x18, 0x3C, 0x00}},
    {'2', {0x00, 0x3C, 0x66, 0x0C, 0x18, 0x30, 0x7E, 0x00}},
    {'3', {0x00, 0x3C, 0x66, 0x1C, 0x06, 0x66, 0x3C, 0x00}},
    {'4', {0x00, 0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x00}},
    {'5', {0x00, 0x7E, 0x60, 0x7C, 0x06, 0x66, 0x3C, 0x00}},
    {'6', {0x00, 0x1C, 0x30, 0x7C, 0x66, 0x66, 0x3C, 0x00}},
    {'7', {0x00, 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x00}},
    {'8', {0x00, 0x3C, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00}},
    {'9', {0x00, 0x3C, 0x66, 0x66, 0x3E, 0x0C, 0x38, 0x00}},
    {'A', {0x00, 0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x00}},
    {'B', {0x00, 0x7C, 0x66, 0x66, 0x7C, 0x66, 0x7C, 0x00}},
    {'C', {0x00, 0x3C, 0x66, 0x60, 0x60, 0x66, 0x3C, 0x00}},
    {'D', {0x00, 0x78, 0x6C, 0x66, 0x66, 0x6C, 0x78, 0x00}},
    {'E', {0x00, 0x7E, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00}},
    {'F', {0x00, 0x7E, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00}},
    {'G', {0x00, 0x3C, 0x66, 0x60, 0x6E, 0x66, 0x3C, 0x00}},
    {'H', {0x00, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00}},
    {'I', {0x00, 0x3C, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00}},
    {'J', {0x00, 0x1E, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00}},
    {'K', {0x00, 0x66, 0x6C, 0x78, 0x78, 0x6C, 0x66, 0x00}},
    {'L', {0x00, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00}},
    {'M', {0x00, 0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x00}},
    {'N', {0x00, 0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x00}},
    {'O', {0x00, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}},
    {'P', {0x00, 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x00}},
    {'Q', {0x00, 0x3C, 0x66, 0x66, 0x66, 0x6C, 0x36, 0x00}},
    {'R', {0x00, 0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x00}},
    {'S', {0x00, 0x3C, 0x66, 0x3C, 0x06, 0x66, 0x3C, 0x00}},
    {'T', {0x00, 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}},
    {'U', {0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}},
    {'V', {0x00, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00}},
    {'W', {0x00, 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x63, 0x00}},
    {'X', {0x00, 0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x00}},
    {'Y', {0x00, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00}},
    {'Z', {0x00, 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x7E, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30}},
    {':', {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00}},
    {';', {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x30, 0x00}},
    {'!', {0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00}},
    {'?', {0x00, 0x3C, 0x66, 0x0C, 0x18, 0x00, 0x18, 0x00}},
    {'\'', {0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'"', {0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E}},
    {'(', {0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00}},
    {')', {0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00}},
    {'*', {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}},
    {'+', {0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00}},
    {'=', {0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00}},
    {'/', {0x06, 0x0C, 0x18, 0x18, 0x30, 0x60, 0x00, 0x00}},
    {'\\', {0x60, 0x30, 0x18, 0x18, 0x0C, 0x06, 0x00, 0x00}},
    {'<', {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}},
    {'>', {0x60, 0x30, 0x18, 0x0C, 0x18, 0x30, 0x60, 0x00}},
};
#define FONT_COUNT (sizeof(FONT) / sizeof(FONT[0]))

static CONST UINT8 FALLBACK_GLYPH[8] = {0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x00};

static CONST UINT8 *font_lookup(char c)
{
    UINTN i;
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 32);
    }
    for (i = 0; i < FONT_COUNT; i++) {
        if (FONT[i].ch == c) {
            return FONT[i].rows;
        }
    }
    return FALLBACK_GLYPH;
}

// Framebuffer Primitives

static UINT32 fb_pixel(UINT32 rgb)
{
    return (((rgb >> 16) & 255) << red_shift) | (((rgb >> 8) & 255) << green_shift) |
           ((rgb & 255) << blue_shift);
}
static void fb_fill(UINT32 rgb)
{
    volatile UINT32 *dst = fb_base;
    UINTN count = (UINTN)fb_pitch * fb_height;
    UINT32 pixel = fb_pixel(rgb);
    /* Write-only, full-width stores; no uncached framebuffer read-modify-write. */
    __asm__ volatile("cld; rep stosl" : "+D"(dst), "+c"(count) : "a"(pixel) : "memory");
    __asm__ volatile("sfence" ::: "memory");
}
static void fb_draw_glyph(char c, UINT32 x, UINT32 y)
{
    const UINT8 *rows = font_lookup(c);
    UINT32 fg = fb_pixel(fg_color), bg = fb_pixel(bg_color);
    for (UINT32 row = 0; row < FONT_H; ++row) {
        volatile UINT32 *dst = fb_base + (UINTN)(y * FONT_H + row) * fb_pitch + x * FONT_W;
        UINT8 bits = rows[row];
        for (UINT32 col = 0; col < FONT_W; ++col)
            dst[col] = (bits & (0x80 >> col)) ? fg : bg;
    }
}
static void console_dirty(UINT32 first, UINT32 last)
{
    if (first < con_dirty_first)
        con_dirty_first = first;
    if (last > con_dirty_last)
        con_dirty_last = last;
}
static void draw_glyph(char c, UINT32 x, UINT32 y)
{
    if (x >= con_cols || y >= con_rows)
        return;
    if (!con_cells) {
        fb_draw_glyph(c, x, y);
        return;
    }
    con_cells[(UINTN)y * con_cols + x] = c;
    if (y + history_view < con_rows)
        console_dirty(y + history_view, y + history_view);
}
static char console_visible_cell(UINT32 x, UINT32 y)
{
    UINT32 row = history_count + y - history_view;
    if (row < history_count)
        return con_history[(UINTN)((history_head + row) % CON_HISTORY_ROWS) * con_cols + x];
    return con_cells[(UINTN)(row - history_count) * con_cols + x];
}
static void overlay_restore(void)
{
    if (!con_shown)
        return;
    UINT32 fg = fb_pixel(fg_color), bg = fb_pixel(bg_color);
    for (UINT32 row = 0; row < fb_overlay.height; ++row) {
        UINT32 y = (UINT32)fb_overlay.y + row;
        if (y >= fb_height)
            break;
        for (UINT32 col = 0; col < fb_overlay.width; ++col) {
            UINT32 x = (UINT32)fb_overlay.x + col;
            if (x >= fb_width)
                break;
            UINT32 pixel = bg;
            if (x / FONT_W < con_cols && y / FONT_H < con_rows) {
                char c = con_shown[(UINTN)(y / FONT_H) * con_cols + x / FONT_W];
                const UINT8 *glyph = font_lookup(c);
                if (glyph[y % FONT_H] & (0x80 >> (x % FONT_W)))
                    pixel = fg;
            }
            ((volatile UINT32 *)fb_base)[(UINTN)y * fb_pitch + x] = pixel;
        }
    }
}
static void overlay_draw(void)
{
    for (UINT32 row = 0; row < fb_overlay.height; ++row) {
        UINT32 y = (UINT32)fb_overlay.y + row;
        if (y >= fb_height)
            break;
        for (UINT32 col = 0; col < fb_overlay.width; ++col) {
            UINT32 x = (UINT32)fb_overlay.x + col;
            if (x >= fb_width)
                break;
            UINT32 pixel = fb_overlay.pixels[row * fb_overlay.width + col];
            if (pixel >> 24)
                ((volatile UINT32 *)fb_base)[(UINTN)y * fb_pitch + x] = fb_pixel(pixel);
        }
    }
}
static void console_present(void)
{
    overlay_restore();
    if (con_cells && con_dirty_first != ~0U) {
        for (UINT32 y = con_dirty_first; y <= con_dirty_last; ++y)
            for (UINT32 x = 0; x < con_cols; ++x) {
                UINTN i = (UINTN)y * con_cols + x;
                char c = console_visible_cell(x, y);
                if (c != con_shown[i]) {
                    fb_draw_glyph(c, x, y);
                    con_shown[i] = c;
                }
            }
        con_dirty_first = ~0U;
        con_dirty_last = 0;
    }
    overlay_draw();
    __asm__ volatile("sfence" ::: "memory");
}
static void fb_scroll_one_line(void)
{
    if (!con_cells) {
        /* Early-boot/OOM fallback clears, rather than reading device memory. */
        fb_fill(bg_color);
        cur_y = 1;
        return;
    }
    if (con_history) {
        UINT32 row = (history_head + history_count) % CON_HISTORY_ROWS;
        mem_copy(con_history + (UINTN)row * con_cols, con_cells, con_cols);
        if (history_count == CON_HISTORY_ROWS)
            history_head = (history_head + 1) % CON_HISTORY_ROWS;
        else
            ++history_count;
        if (history_view && history_view < history_count)
            ++history_view;
    }
    UINTN moved = (UINTN)con_cols * (con_rows - 1);
    memmove(con_cells, con_cells + con_cols, moved);
    memset(con_cells + moved, ' ', con_cols);
    console_dirty(0, con_rows - 1);
}
/* Kernel callers print from task context. Keep timer/PS2 IRQs enabled during
 * rendering, but pin the current task so a same-CPU printer cannot deadlock
 * on the console lock. Fault output is best-effort and bypasses this lock. */
static BOOLEAN console_enter(void)
{
    if (__atomic_load_n(&console_emergency, __ATOMIC_RELAXED))
        return FALSE;
    k_preempt_disable();
    while (__atomic_exchange_n(&console_lock.value, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&console_lock.value, __ATOMIC_RELAXED))
            pause_cpu();
    return TRUE;
}
static void console_leave(BOOLEAN locked)
{
    console_present();
    if (locked) {
        __atomic_store_n(&console_lock.value, 0, __ATOMIC_RELEASE);
        k_preempt_enable();
    }
}
static void console_panic_mode(void)
{
    __atomic_store_n(&console_emergency, 1, __ATOMIC_RELAXED);
}
static void console_buffer_init(void)
{
    UINT64 cells = (UINT64)con_cols * con_rows;
    if (!cells || cells > 16 * 1024 * 1024)
        return;
    UINT64 memory = k_pmm_alloc_pages((UINT32)((cells * 2 + 4095) / 4096), 0);
    if (!memory)
        return;
    con_cells = (char *)(UINTN)memory;
    con_shown = con_cells + cells;
    memset(con_cells, ' ', cells * 2);
    UINT64 history_bytes = (UINT64)con_cols * CON_HISTORY_ROWS;
    con_history = (void *)(UINTN)k_pmm_alloc_pages((UINT32)((history_bytes + 4095) / 4096), 0);
    fb_fill(bg_color);
    cur_x = cur_y = 0;
}
BOOLEAN kernel_console_buffered(void)
{
    return con_cells != NULL;
}
BOOLEAN kernel_fb_pat_wc(void)
{
    return fb_pat_wc;
}

// Kernel console driver API (see kernel.h)

void kernel_fb_init(UINT32 *fb_base_in, UINT32 width, UINT32 height, UINT32 pitch, UINT8 rshift,
                    UINT8 gshift, UINT8 bshift)
{
    fb_base = fb_base_in;
    fb_width = width;
    fb_height = height;
    fb_pitch = pitch;
    red_shift = rshift;
    green_shift = gshift;
    blue_shift = bshift;

    con_cols = fb_width / FONT_W;
    con_rows = fb_height / FONT_H;
    cur_x = 0;
    cur_y = 0;
}

UINT32 kernel_fb_width(void)
{
    return fb_width;
}
UINT32 kernel_fb_height(void)
{
    return fb_height;
}
UINT32 kernel_con_cols(void)
{
    return con_cols;
}
UINT32 kernel_con_rows(void)
{
    return con_rows;
}

void kernel_console_clear(void)
{
    BOOLEAN locked = console_enter();
    history_head = history_count = history_view = 0;
    if (fb_base)
        fb_fill(bg_color);
    if (con_cells)
        memset(con_cells, ' ', (UINTN)con_cols * con_rows * 2);
    con_dirty_first = ~0U;
    con_dirty_last = 0;
    cur_x = cur_y = 0;
    console_leave(locked);
}

/* Generic console viewport and overlay mechanisms; input policy is external. */
UINT32 kernel_console_scroll(INT32 rows)
{
    BOOLEAN locked = console_enter();
    INT64 next = (INT64)history_view + rows;
    UINT32 view = next < 0 ? 0 : next > history_count ? history_count : (UINT32)next;
    if (view != history_view) {
        history_view = view;
        console_dirty(0, con_rows - 1);
    }
    console_leave(locked);
    return view;
}
int kernel_overlay_set(INT32 x, INT32 y, UINT32 width, UINT32 height, const UINT32 *pixels)
{
    if (width > 32 || height > 32 || (width && height && !pixels) || !con_shown)
        return K_EINVAL;
    BOOLEAN locked = console_enter();
    overlay_restore();
    fb_overlay.width = width;
    fb_overlay.height = height;
    fb_overlay.x = x < 0 ? 0 : x >= (INT32)fb_width ? (INT32)fb_width - 1 : x;
    fb_overlay.y = y < 0 ? 0 : y >= (INT32)fb_height ? (INT32)fb_height - 1 : y;
    if (width && height)
        mem_copy(fb_overlay.pixels, pixels, width * height * 4);
    console_leave(locked);
    return 0;
}
void kernel_overlay_move(INT32 x, INT32 y)
{
    BOOLEAN locked = console_enter();
    overlay_restore();
    fb_overlay.x = x < 0 ? 0 : x >= (INT32)fb_width ? (INT32)fb_width - 1 : x;
    fb_overlay.y = y < 0 ? 0 : y >= (INT32)fb_height ? (INT32)fb_height - 1 : y;
    console_leave(locked);
}

// Text console.

static void con_putc_raw(char c)
{
    if (!fb_base || !con_cols || !con_rows)
        return;
    if (c == '\n') {
        cur_x = 0;
        cur_y++;
    } else if (c == '\r') {
        cur_x = 0;
    } else if (c == '\b') {
        if (cur_x > 0) {
            cur_x--;
            draw_glyph(' ', cur_x, cur_y);
        }
    } else {
        draw_glyph(c, cur_x, cur_y);
        cur_x++;
        if (cur_x >= con_cols) {
            cur_x = 0;
            cur_y++;
        }
    }

    while (cur_y >= con_rows) {
        fb_scroll_one_line();
        cur_y--;
    }
}

void con_putc(char c)
{
    BOOLEAN locked = console_enter();
    con_putc_raw(c);
    console_leave(locked);
}
void con_print(CONST char *s)
{
    if (!s)
        return;
    BOOLEAN locked = console_enter();
    while (*s)
        con_putc_raw(*s++);
    console_leave(locked);
}
void con_print_hex(UINT64 v)
{
    static const char hex[] = "0123456789abcdef";
    char b[19];
    b[0] = '0';
    b[1] = 'x';
    for (int i = 0; i < 16; ++i)
        b[2 + i] = hex[(v >> (60 - 4 * i)) & 15];
    b[18] = 0;
    con_print(b);
}

void con_print_uint(UINT64 v)
{
    char buf[24];
    int i = 0, a, b;

    if (v == 0) {
        con_putc('0');
        return;
    }

    while (v > 0 && i < 23) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    buf[i] = 0;

    for (a = 0, b = i - 1; a < b; a++, b--) {
        char tmp = buf[a];
        buf[a] = buf[b];
        buf[b] = tmp;
    }

    con_print(buf);
}

void con_print_2digit(UINT16 v)
{
    char buf[3];
    buf[0] = (char)('0' + ((v / 10) % 10));
    buf[1] = (char)('0' + (v % 10));
    buf[2] = 0;
    con_print(buf);
}

void con_print_int(INT32 v)
{
    if (v < 0) {
        con_putc('-');
        con_print_uint((UINT64)(-(INT64)v));
    } else {
        con_print_uint((UINT64)v);
    }
}

/* Physical allocator: a page must have both EFI eligibility and current
 * ownership before it can be returned. Low 1 MiB and all loader/firmware
 * allocations remain reserved; no boot-services memory is reclaimed. */
static UINT8 page_bitmap[PMM_MAX_PAGES / 8], eligible_bitmap[PMM_MAX_PAGES / 8];
static UINT64 pmm_free_pages, pmm_total_pages_managed, pmm_search_cursor, pmm_high_page;
static EFI_MEMORY_DESCRIPTOR *boot_map;
static UINTN boot_desc_size, boot_entries;
static UINT64 *kernel_pml4;
static BOOLEAN bit_get(UINT8 *b, UINT64 n)
{
    return (b[n >> 3] >> (n & 7)) & 1;
}
static void bit_set(UINT8 *b, UINT64 n)
{
    b[n >> 3] |= (UINT8)(1u << (n & 7));
}
static void bit_clear(UINT8 *b, UINT64 n)
{
    b[n >> 3] &= (UINT8) ~(1u << (n & 7));
}
void pmm_seed_from_efi_map(EFI_MEMORY_DESCRIPTOR *map, UINTN stride, UINTN count)
{
    if (scheduler_ready || !map || stride < sizeof(*map))
        kernel_halt("invalid PMM seed");
    boot_map = map;
    boot_desc_size = stride;
    boot_entries = count;
    mem_zero(page_bitmap, sizeof(page_bitmap));
    mem_zero(eligible_bitmap, sizeof(eligible_bitmap));
    pmm_free_pages = pmm_total_pages_managed = pmm_search_cursor = pmm_high_page = 0;
    for (UINTN i = 0; i < count; ++i) {
        EFI_MEMORY_DESCRIPTOR *d = (void *)((UINT8 *)map + i * stride);
        if (d->Type != EfiConventionalMemory || (d->PhysicalStart & 4095))
            continue;
        UINT64 first = d->PhysicalStart / 4096;
        if (first >= PMM_MAX_PAGES)
            continue;
        UINT64 last = first + MIN(d->NumberOfPages, PMM_MAX_PAGES - first);
        if (first < 256)
            first = 256;
        for (UINT64 p = first; p < last; ++p)
            if (!bit_get(eligible_bitmap, p)) {
                bit_set(eligible_bitmap, p);
                bit_set(page_bitmap, p);
                ++pmm_free_pages;
                ++pmm_total_pages_managed;
            }
        if (last > pmm_high_page)
            pmm_high_page = last;
    }
}
UINT64 k_pmm_alloc_pages(UINT32 count, UINT64 maximum)
{
    if (!count)
        return 0;
    UINT64 limit = maximum ? MIN(maximum / 4096, pmm_high_page) : pmm_high_page;
    UINT64 f = k_spin_lock(&pmm_lock);
    if (count > pmm_free_pages || count > limit) {
        k_spin_unlock(&pmm_lock, f);
        return 0;
    }
    UINT64 cursor = count == 1 && pmm_search_cursor < limit ? pmm_search_cursor : 256;
    for (int pass = 0; pass < 2; ++pass) {
        UINT64 end = pass ? cursor : limit, run = 0, start = 0;
        for (UINT64 p = pass ? 256 : cursor; p < end; ++p) {
            if (bit_get(page_bitmap, p)) {
                if (!run)
                    start = p;
                if (++run == count) {
                    for (UINT64 j = start; j < start + count; ++j)
                        bit_clear(page_bitmap, j);
                    pmm_free_pages -= count;
                    pmm_search_cursor = start + count;
                    k_spin_unlock(&pmm_lock, f);
                    mem_zero((void *)(UINTN)(start * 4096), (UINTN)count * 4096);
                    return start * 4096;
                }
            } else
                run = 0;
        }
    }
    k_spin_unlock(&pmm_lock, f);
    return 0;
}
UINT64 pmm_alloc_page(void)
{
    return k_pmm_alloc_pages(1, 0);
}
void k_pmm_free_pages(UINT64 phys, UINT32 count)
{
    if (!count || (phys & 4095) || phys / 4096 >= PMM_MAX_PAGES ||
        count > PMM_MAX_PAGES - phys / 4096)
        return;
    UINT64 f = k_spin_lock(&pmm_lock), start = phys / 4096;
    for (UINT64 p = start; p < start + count; ++p)
        if (!bit_get(eligible_bitmap, p) || bit_get(page_bitmap, p)) {
            k_spin_unlock(&pmm_lock, f);
            return;
        }
    for (UINT64 p = start; p < start + count; ++p)
        bit_set(page_bitmap, p);
    pmm_free_pages += count;
    if (start < pmm_search_cursor)
        pmm_search_cursor = start;
    k_spin_unlock(&pmm_lock, f);
}
void pmm_free_page(UINT64 phys)
{
    k_pmm_free_pages(phys, 1);
}
UINT64 k_pmm_free_count(void)
{
    return __atomic_load_n(&pmm_free_pages, __ATOMIC_RELAXED);
}
UINT64 kernel_pmm_managed_mib(void)
{
    return pmm_total_pages_managed / 256;
}
UINT64 kernel_pmm_free_mib(void)
{
    return k_pmm_free_count() / 256;
}
UINT64 kernel_pmm_used_mib(void)
{
    return (pmm_total_pages_managed - k_pmm_free_count()) / 256;
}
static BOOLEAN canonical(UINT64 a)
{
    return (a >> 47) == 0 || (a >> 47) == 0x1ffff;
}
/* Split a large mapping before descending. PAT's large-page bit (12) is
 * moved to bit 7 for 4 KiB leaves. No allocation failure is dereferenced. */
static int pt_child(UINT64 *table, UINT32 idx, UINT32 level, BOOLEAN user, UINT64 **out)
{
    UINT64 e = table[idx];
    if (!(e & PAGE_PRESENT)) {
        UINT64 p = pmm_alloc_page();
        if (!p)
            return K_ENOMEM;
        table[idx] = p | PAGE_PRESENT | PAGE_RW | (user ? PAGE_USER : 0);
    } else if (e & PAGE_LARGE) {
        if (level != 2 && level != 3)
            return K_EINVAL;
        UINT64 p = pmm_alloc_page();
        if (!p)
            return K_ENOMEM;
        UINT64 *child = (void *)(UINTN)p;
        UINT64 stride = level == 3 ? 0x200000ULL : 4096;
        UINT64 base = e & PHYS_MASK & ~((level == 3 ? 0x40000000ULL : 0x200000ULL) - 1);
        UINT64 flags = e & ~PHYS_MASK;
        if (e & (1ULL << 12))
            flags |= level == 3 ? (1ULL << 12) : PAGE_LARGE;
        else if (level == 2)
            flags &= ~PAGE_LARGE;
        for (UINT32 j = 0; j < 512; ++j)
            child[j] = (base + j * stride) | flags;
        table[idx] = p | PAGE_PRESENT | PAGE_RW | (e & PAGE_USER);
    }
    if (user && !(table[idx] & PAGE_USER))
        return K_EPERM;
    *out = (void *)(UINTN)(table[idx] & PHYS_MASK);
    return 0;
}
static int map_page_raw(UINT64 *root, UINT64 v, UINT64 p, UINT64 flags, BOOLEAN replace)
{
    if (!root || !canonical(v) || ((v | p) & 4095) || (p & ~PHYS_MASK))
        return K_EINVAL;
    BOOLEAN user = (flags & PAGE_USER) != 0;
    if (user && (v < K_USER_BASE || v >= K_USER_LIMIT))
        return K_EPERM;
    UINT64 *t = root;
    for (UINT32 level = 4; level > 1; --level) {
        int e = pt_child(t, (v >> (12 + 9 * (level - 1))) & 511, level, user, &t);
        if (e)
            return e;
    }
    UINT32 idx = (v >> 12) & 511;
    if (!replace && (t[idx] & PAGE_PRESENT))
        return K_EEXIST;
    t[idx] = p | flags | PAGE_PRESENT;
    if (vm_ready)
        __asm__ volatile("invlpg (%0)" ::"r"(v) : "memory");
    return 0;
}
int vmm_map_page(UINT64 *root, UINT64 v, UINT64 p, UINT64 flags)
{
    /* Kernel mappings are immutable once APs run: no hidden TLB-shootdown gap.
     * User mappings are private and all user tasks are pinned to the BSP. */
    if ((smp_started && root == kernel_pml4) || (flags & PAGE_USER))
        return K_EPERM;
    UINT64 f = k_spin_lock(&vm_lock);
    int e = map_page_raw(root, v, p, flags, FALSE);
    k_spin_unlock(&vm_lock, f);
    return e;
}
int vmm_unmap_page(UINT64 *root, UINT64 v)
{
    if (!root || !canonical(v) || (v & 4095) || (smp_started && root == kernel_pml4))
        return K_EINVAL;
    UINT64 f = k_spin_lock(&vm_lock), *t = root;
    for (UINT32 level = 4; level > 1; --level) {
        UINT32 idx = (v >> (12 + 9 * (level - 1))) & 511;
        if (!(t[idx] & PAGE_PRESENT)) {
            k_spin_unlock(&vm_lock, f);
            return K_ENOENT;
        }
        int e = pt_child(t, idx, level, FALSE, &t);
        if (e) {
            k_spin_unlock(&vm_lock, f);
            return e;
        }
    }
    UINT32 idx = (v >> 12) & 511;
    if (!(t[idx] & PAGE_PRESENT)) {
        k_spin_unlock(&vm_lock, f);
        return K_ENOENT;
    }
    t[idx] = 0;
    __asm__ volatile("invlpg (%0)" ::"r"(v) : "memory");
    k_spin_unlock(&vm_lock, f);
    return 0;
}
int vmm_translate(UINT64 *root, UINT64 v, UINT64 *phys, UINT64 *flags)
{
    if (!root || !canonical(v))
        return K_EFAULT;
    UINT64 *t = root, allow = PAGE_USER | PAGE_RW, nx = 0;
    for (UINT32 level = 4; level; --level) {
        UINT64 e = t[(v >> (12 + 9 * (level - 1))) & 511];
        if (!(e & PAGE_PRESENT))
            return K_EFAULT;
        allow &= e;
        nx |= e & PAGE_NX;
        if (level == 1 || ((level == 2 || level == 3) && (e & PAGE_LARGE))) {
            UINT64 mask = (1ULL << (12 + 9 * (level - 1))) - 1;
            if (phys)
                *phys = (e & PHYS_MASK & ~mask) | (v & mask);
            if (flags)
                *flags = PAGE_PRESENT | allow | nx;
            return 0;
        }
        t = (void *)(UINTN)(e & PHYS_MASK);
    }
    return K_EFAULT;
}
static int map_large(UINT64 v, UINT64 p, UINT64 flags)
{
    UINT64 *t = kernel_pml4;
    for (UINT32 level = 4; level > 2; --level) {
        int e = pt_child(t, (v >> (12 + 9 * (level - 1))) & 511, level, FALSE, &t);
        if (e)
            return e;
    }
    UINT32 idx = (v >> 21) & 511;
    if ((t[idx] & PAGE_PRESENT) && !(t[idx] & PAGE_LARGE)) {
        for (UINT32 j = 0; j < 512; ++j) {
            int e = map_page_raw(kernel_pml4, v + j * 4096, p + j * 4096, flags, TRUE);
            if (e)
                return e;
        }
    } else
        t[idx] = p | flags | PAGE_PRESENT | PAGE_LARGE;
    return 0;
}
static int map_identity(UINT64 start, UINT64 size, UINT64 flags)
{
    if (!size)
        return 0;
    if (start >= K_USER_BASE || size > K_USER_BASE - start)
        return K_ENOTSUP;
    UINT64 end = ALIGN_UP(start + size, 4096);
    start &= ~4095ULL;
    while (start < end) {
        int e;
        if (!(start & 0x1fffff) && end - start >= 0x200000) {
            e = map_large(start, start, flags);
            start += 0x200000;
        } else {
            e = map_page_raw(kernel_pml4, start, start, flags, TRUE);
            start += 4096;
        }
        if (e)
            return e;
    }
    return 0;
}
int k_mmio_map(UINT64 p, UINT64 bytes)
{
    if (smp_started || !p || !bytes || p >= K_USER_BASE || bytes > K_USER_BASE - p)
        return K_ENOTSUP;
    UINT64 f = k_spin_lock(&vm_lock);
    int e = map_identity(p, bytes, PAGE_RW | PAGE_PCD | PAGE_PWT | (platform.nx ? PAGE_NX : 0));
    if (vm_ready)
        __asm__ volatile("mov %0,%%cr3" ::"r"(kernel_pml4) : "memory");
    k_spin_unlock(&vm_lock, f);
    return e;
}
int kernel_vmm_init(void)
{
    UINT32 a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    UINT32 extended_max = a;
    if (a >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        platform.nx = (d & (1 << 20)) != 0;
    }
    if (extended_max >= 0x80000008) {
        cpuid(0x80000008, 0, &a, &b, &c, &d);
        saved_phys_bits = a & 255;
    }
    cpuid(1, 0, &a, &b, &c, &d);
    has_pat = (d & (1 << 16)) != 0;
    has_mtrr = (d & (1 << 12)) != 0;
    if (has_pat) {
        saved_pat = rdmsr(0x277);
        if ((saved_pat & 255) != 6 || ((saved_pat >> 24) & 255) != 0)
            return K_ENOTSUP;
    }
    if (has_mtrr) {
        UINT64 cap = rdmsr(0xfe);
        saved_mtrr_count = (UINT32)(cap & 255);
        if (saved_mtrr_count > 32)
            return K_ENOTSUP;
        mtrr_fixed_supported = (cap & (1 << 8)) != 0;
        saved_mtrr_default = rdmsr(0x2ff);
        for (UINT32 i = 0; i < saved_mtrr_count * 2; ++i)
            saved_mtrr_variable[i] = rdmsr(0x200 + i);
        if (mtrr_fixed_supported) {
            static const UINT16 msrs[] = {0x250, 0x258, 0x259, 0x268, 0x269, 0x26a,
                                          0x26b, 0x26c, 0x26d, 0x26e, 0x26f};
            for (UINT32 i = 0; i < 11; ++i)
                saved_mtrr_fixed[i] = rdmsr(msrs[i]);
        }
    }
    UINT64 cr4;
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    if (cr4 & (1ULL << 12))
        return K_ENOTSUP;
    if (platform.nx)
        wrmsr(0xc0000080, rdmsr(0xc0000080) | (1 << 11));
    kernel_pml4 = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
    if (!kernel_pml4)
        return K_ENOMEM;
    for (UINTN i = 0; i < boot_entries; ++i) {
        EFI_MEMORY_DESCRIPTOR *m = (void *)((UINT8 *)boot_map + i * boot_desc_size);
        if (m->NumberOfPages > ~0ULL / 4096)
            return K_EINVAL;
        if (m->Type == EfiUnusableMemory || m->Type == EfiMemoryMappedIOPortSpace ||
            m->PhysicalStart >= K_USER_BASE)
            continue;
        UINT64 flags = PAGE_RW;
        /* UEFI Attribute is a mask of SUPPORTED cache modes, not a selected
         * mode. UC|WC|WT|WB is normal RAM: choosing UC there uncaches kernel
         * stacks, spinlocks and scheduler data on every core. Prefer WB.
         * UEFI GetMemoryMap(), EFI_MEMORY_DESCRIPTOR.Attribute. */
        UINT64 cache_caps =
            m->Attribute & (EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB);
        BOOLEAN ordinary_ram = m->Type == EfiConventionalMemory || m->Type == EfiLoaderCode ||
                               m->Type == EfiLoaderData || m->Type == EfiBootServicesCode ||
                               m->Type == EfiBootServicesData ||
                               m->Type == EfiRuntimeServicesCode ||
                               m->Type == EfiRuntimeServicesData ||
                               m->Type == EfiACPIReclaimMemory || m->Type == EfiACPIMemoryNVS;
        if (m->Type == EfiMemoryMappedIO ||
            (!(cache_caps & EFI_MEMORY_WB) && !(ordinary_ram && !cache_caps)))
            flags |= PAGE_PCD | PAGE_PWT;
        /* Preserve executable firmware/loader descriptors; supervisor-only.
         * Conventional RAM and MMIO need no executable identity alias. */
        if (platform.nx && m->Type != EfiLoaderCode && m->Type != EfiBootServicesCode &&
            m->Type != EfiRuntimeServicesCode && m->Type != EfiReservedMemoryType)
            flags |= PAGE_NX;
        int e = map_identity(m->PhysicalStart, m->NumberOfPages * 4096, flags);
        if (e)
            return e;
    }
    /* Framebuffer aperture may be absent from the EFI memory map. */
    if (fb_base) {
        int e = map_identity((UINT64)(UINTN)fb_base, (UINT64)fb_pitch * fb_height * 4,
                             PAGE_RW | PAGE_PCD | PAGE_PWT | (platform.nx ? PAGE_NX : 0));
        if (e)
            return e;
    }
    /* Pre-create a private diagnostic slot before cloning any address space. */
    UINT64 *ignored;
    int e = pt_child(kernel_pml4, (K_TEST_VA >> 39) & 511, 4, FALSE, &ignored);
    if (e)
        return e;
    /* Flush while the firmware cache types are still in force. Change cache
     * policy with interrupts and caching disabled; retain firmware MTRRs.
     * No AP has been started yet. This avoids WB/UC aliases during CR3/PAT
     * handoff, including dirty cache lines inherited from the EFI loader. */
    UINT64 irq = k_irq_save(), cr0;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    cr0 = (cr0 | (1ULL << 16)) & ~((1ULL << 30) | (1ULL << 29));
    UINT64 uncached_cr0 = cr0 | (1ULL << 30);
    __asm__ volatile("mov %0,%%cr0; wbinvd" ::"r"(uncached_cr0) : "memory");
    /* Drop firmware PCID/global translations before enabling isolation.
     * None of this kernel's leaves use GLOBAL. */
    if (cr4 & (1ULL << 17)) {
        UINT64 old_cr3;
        __asm__ volatile("mov %%cr3,%0" : "=r"(old_cr3));
        old_cr3 &= PHYS_MASK;
        __asm__ volatile("mov %0,%%cr3" ::"r"(old_cr3) : "memory");
    }
    cr4 &= ~((1ULL << 17) | (1ULL << 7));
    __asm__ volatile("mov %0,%%cr4" ::"r"(cr4) : "memory");
    __asm__ volatile("mov %0,%%cr3" ::"r"(kernel_pml4) : "memory");
    vm_ready = TRUE;
    /* Entry 0 stays WB for RAM/page tables; entry 3 stays strong UC for
     * registers. Entry 1 was unused by our tables and becomes WC only for
     * the framebuffer aperture. ap_cache_init() installs this PAT on APs. */
    if (has_pat && fb_base) {
        saved_pat = (saved_pat & ~(255ULL << 8)) | (1ULL << 8);
        wrmsr(0x277, saved_pat);
        e = map_identity((UINT64)(UINTN)fb_base, (UINT64)fb_pitch * fb_height * 4,
                         PAGE_RW | PAGE_PWT | (platform.nx ? PAGE_NX : 0));
        fb_pat_wc = !e; /* MTRRs may still impose a more restrictive type. */
    }
    __asm__ volatile("wbinvd; mov %0,%%cr3" ::"r"(kernel_pml4) : "memory");
    __asm__ volatile("mov %0,%%cr0" ::"r"(cr0) : "memory");
    k_irq_restore(irq);
    if (e)
        return e;
    console_buffer_init();
    return 0;
}
UINT64 *kernel_get_pml4(void)
{
    return kernel_pml4;
}

k_address_space *k_as_create(void)
{
    if (k_cpu_id() != 0)
        return NULL;
    UINT64 f = k_spin_lock(&vm_lock);
    for (UINT32 i = 0; i < MAX_AS; ++i)
        if (!spaces[i].used) {
            UINT64 *p = (void *)(UINTN)pmm_alloc_page();
            if (!p)
                break;
            mem_copy(p, kernel_pml4, 4096);
            p[USER_SLOT] = 0;
            spaces[i] = (k_address_space){TRUE, p, K_USER_BASE + 0x200000, 0, NULL};
            k_spin_unlock(&vm_lock, f);
            return &spaces[i];
        }
    k_spin_unlock(&vm_lock, f);
    return NULL;
}
static BOOLEAN valid_as(k_address_space *as)
{
    UINTN p = (UINTN)as, b = (UINTN)spaces;
    return p >= b && p < b + sizeof(spaces) && !((p - b) % sizeof(*as)) && as->used;
}
static void free_user_tree(UINT64 *table, UINT32 level)
{
    for (UINT32 i = 0; i < 512; ++i)
        if (table[i] & PAGE_PRESENT) {
            UINT64 p = table[i] & PHYS_MASK;
            if (level > 1)
                free_user_tree((void *)(UINTN)p, level - 1);
            pmm_free_page(p);
        }
}
int k_as_destroy(k_address_space *as)
{
    if (!valid_as(as) || k_cpu_id() != 0)
        return K_EINVAL;
    if (as->owner)
        return K_EBUSY; /* process-owned spaces are borrowed handles */
    UINT64 f = k_spin_lock(&sched_lock);
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i)
        if (tasks[i].state != K_TASK_UNUSED && tasks[i].state != K_TASK_ZOMBIE &&
            tasks[i].process && tasks[i].process->as == as) {
            k_spin_unlock(&sched_lock, f);
            return K_EBUSY;
        }
    k_spin_unlock(&sched_lock, f);
    f = k_spin_lock(&vm_lock);
    if (as->pml4[USER_SLOT] & PAGE_PRESENT) {
        UINT64 p = as->pml4[USER_SLOT] & PHYS_MASK;
        free_user_tree((void *)(UINTN)p, 3);
        pmm_free_page(p);
    }
    pmm_free_page((UINT64)(UINTN)as->pml4);
    mem_zero(as, sizeof(*as));
    k_spin_unlock(&vm_lock, f);
    return 0;
}
int k_as_alloc(k_address_space *as, UINT64 bytes, UINT64 align, UINT32 rwx, UINT64 *base)
{
    if (!valid_as(as) || !base || !bytes || !power2(align) || align > 0x40000000 || (rwx & ~7) ||
        !(rwx & RIEF_REGION_R) ||
        ((rwx & (RIEF_REGION_W | RIEF_REGION_X)) == (RIEF_REGION_W | RIEF_REGION_X)) ||
        k_cpu_id() != 0)
        return K_EINVAL;
    if (!platform.nx)
        return K_ENOTSUP;
    if (bytes > MAX_USER_PAGES * 4096ULL)
        return K_E2BIG;
    UINT64 n = ALIGN_UP(bytes, 4096) / 4096;
    align = align < 4096 ? 4096 : align;
    UINT64 f = k_spin_lock(&vm_lock), v = ALIGN_UP(as->next, align);
    if (n > MAX_USER_PAGES - as->pages || v >= K_USER_LIMIT || n * 4096 + 4096 > K_USER_LIMIT - v) {
        k_spin_unlock(&vm_lock, f);
        return K_ENOMEM;
    }
    UINT64 flags =
        PAGE_USER | ((rwx & RIEF_REGION_W) ? PAGE_RW : 0) | ((rwx & RIEF_REGION_X) ? 0 : PAGE_NX);
    UINT64 done = 0;
    int e = 0;
    for (; done < n; ++done) {
        UINT64 p = pmm_alloc_page();
        if (!p) {
            e = K_ENOMEM;
            break;
        }
        e = map_page_raw(as->pml4, v + done * 4096, p, flags, FALSE);
        if (e) {
            pmm_free_page(p);
            break;
        }
    }
    if (e) {
        for (UINT64 j = 0; j < done; ++j) {
            UINT64 *t = as->pml4, a = v + j * 4096;
            for (int l = 4; l > 1; --l)
                t = (void *)(UINTN)(t[(a >> (12 + 9 * (l - 1))) & 511] & PHYS_MASK);
            UINT64 p = t[(a >> 12) & 511] & PHYS_MASK;
            t[(a >> 12) & 511] = 0;
            pmm_free_page(p);
        }
    } else {
        as->next = v + n * 4096 + 4096;
        as->pages += (UINT32)n;
        *base = v;
    }
    k_spin_unlock(&vm_lock, f);
    return e;
}
int k_as_check(k_address_space *as, UINT64 address, UINT64 bytes, BOOLEAN write)
{
    if (!valid_as(as) || address < K_USER_BASE || address >= K_USER_LIMIT ||
        bytes > K_USER_LIMIT - address)
        return K_EFAULT;
    if (!bytes)
        return 0;
    UINT64 end = address + bytes;
    for (UINT64 v = address & ~4095ULL; v < end; v += 4096) {
        UINT64 flags;
        if (vmm_translate(as->pml4, v, NULL, &flags) || !(flags & PAGE_USER) ||
            (write && !(flags & PAGE_RW)))
            return K_EFAULT;
    }
    return 0;
}
/* Copies via supervisor physical aliases. No temporary permissions on user
 * mappings, and no direct dereference of an untrusted virtual pointer. */
static int as_copy(k_address_space *as, UINT64 u, void *buf, UINT64 n, BOOLEAN to, BOOLEAN loader)
{
    int e = k_as_check(as, u, n, to && !loader);
    if (e)
        return e;
    UINT8 *b = buf;
    while (n) {
        UINT64 p;
        e = vmm_translate(as->pml4, u, &p, NULL);
        if (e)
            return e;
        UINT64 chunk = MIN(n, 4096 - (u & 4095));
        if (to)
            mem_copy((void *)(UINTN)p, b, chunk);
        else
            mem_copy(b, (void *)(UINTN)p, chunk);
        u += chunk;
        b += chunk;
        n -= chunk;
    }
    return 0;
}
int k_copy_to_user(k_address_space *as, UINT64 u, const void *b, UINT64 n)
{
    return as_copy(as, u, (void *)b, n, TRUE, FALSE);
}
int k_copy_from_user(k_address_space *as, void *b, UINT64 u, UINT64 n)
{
    return as_copy(as, u, b, n, FALSE, FALSE);
}

/* ACPI discovery is checksum/length checked before any table is interpreted. */
static volatile UINT8 *hpet;
static UINT64 hpet_period_fs, hpet_mask = ~0ULL;
static UINT16 pm_timer_port;
static UINT32 pm_timer_mask = 0xffffff;
static struct {
    UINT32 id, gsi, count;
    volatile UINT32 *regs;
} ioapics[8];
static UINT32 ioapic_count, isa_input_gsi[2] = {1, 12};
static UINT16 isa_input_flags[2];
static BOOLEAN pic_timer;
static BOOLEAN physical_described(UINT64 p, UINT64 n)
{
    if (!n || p >= K_USER_BASE || n > K_USER_BASE - p)
        return FALSE;
    for (UINTN i = 0; i < boot_entries; ++i) {
        EFI_MEMORY_DESCRIPTOR *d = (void *)((UINT8 *)boot_map + i * boot_desc_size);
        if (d->Type == EfiUnusableMemory || d->Type == EfiMemoryMappedIOPortSpace)
            continue;
        if (d->NumberOfPages > ~0ULL / 4096)
            continue;
        UINT64 bytes = d->NumberOfPages * 4096;
        if (p >= d->PhysicalStart && span_ok(p - d->PhysicalStart, n, bytes))
            return TRUE;
    }
    return FALSE;
}
static BOOLEAN checksum(const void *ptr, UINT32 n)
{
    const UINT8 *p = ptr;
    UINT8 sum = 0;
    while (n--)
        sum += *p++;
    return sum == 0;
}
static const UINT8 *acpi_table(UINT64 p)
{
    if (!physical_described(p, 36))
        return NULL;
    const UINT8 *t = (void *)(UINTN)p;
    UINT32 n = rd32(t + 4);
    if (n < 36 || n > 1024 * 1024 || !physical_described(p, n) || !checksum(t, n))
        return NULL;
    return t;
}
static UINT32 ioapic_read(UINT32 i, UINT8 r)
{
    ioapics[i].regs[0] = r;
    return ioapics[i].regs[4];
}
static void ioapic_write(UINT32 i, UINT8 r, UINT32 v)
{
    ioapics[i].regs[0] = r;
    ioapics[i].regs[4] = v;
}
static void add_cpu(UINT32 id)
{
    for (UINT32 i = 0; i < platform.discovered_cpus; ++i)
        if (cpus[i].apic_id == id)
            return;
    UINT32 n = platform.discovered_cpus;
    if (n < K_MAX_CPUS) {
        cpus[n].index = n;
        cpus[n].apic_id = id;
        ++platform.discovered_cpus;
    }
}
static void parse_madt(const UINT8 *t)
{
    UINT32 n = rd32(t + 4);
    if (n < 44)
        return;
    platform.pic = (rd32(t + 40) & 1) != 0;
    UINT64 base = rd32(t + 36);
    for (UINT32 p = 44; p + 2 <= n;) {
        UINT8 type = t[p], len = t[p + 1];
        if (len < 2 || len > n - p)
            break;
        const UINT8 *e = t + p;
        if (type == 0 && len >= 8 && (rd32(e + 4) & 1))
            add_cpu(e[3]);
        if (type == 9 && len >= 16 && (rd32(e + 8) & 1))
            add_cpu(rd32(e + 4));
        if (type == 5 && len >= 12)
            base = rd64(e + 4);
        if (type == 1 && len >= 12 && ioapic_count < ARRAY_LEN(ioapics)) {
            UINT64 a = rd32(e + 4);
            if (!k_mmio_map(a, 4096)) {
                UINT32 i = ioapic_count++;
                ioapics[i].regs = (void *)(UINTN)a;
                ioapics[i].id = e[2];
                ioapics[i].gsi = rd32(e + 8);
                ioapics[i].count = ((ioapic_read(i, 1) >> 16) & 0xff) + 1;
                if (ioapics[i].count > 120)
                    ioapics[i].count = 120;
            }
        }
        if (type == 2 && len >= 10 && e[2] == 0 && (e[3] == 1 || e[3] == 12)) {
            UINT32 channel = e[3] == 12;
            isa_input_gsi[channel] = rd32(e + 4);
            isa_input_flags[channel] = rd16(e + 8);
        }
        p += len;
    }
    if (!platform.x2apic && base && !k_mmio_map(base, 4096))
        lapic_regs = (void *)(UINTN)base;
}
static void parse_fadt(const UINT8 *t)
{
    UINT32 n = rd32(t + 4);
    if (n < 116)
        return;
    if (t[8] >= 2)
        platform.ps2 = (rd16(t + 109) & 2) != 0;
    UINT32 flags = rd32(t + 112);
    if (flags & (1u << 20)) {
        platform.pic = FALSE;
        platform.ps2 = FALSE;
        return;
    }
    UINT64 p = rd32(t + 76);
    if (n >= 220 && t[208] == 1 && rd64(t + 212))
        p = rd64(t + 212);
    if (p && p <= 65535 && t[91] == 4) {
        pm_timer_port = (UINT16)p;
        pm_timer_mask = (flags & (1 << 8)) ? 0xffffffff : 0xffffff;
    }
}
static void parse_hpet(const UINT8 *t)
{
    if (rd32(t + 4) < 56 || t[40] != 0)
        return;
    UINT64 p = rd64(t + 44);
    if (k_mmio_map(p, 4096))
        return;
    volatile UINT8 *base = (void *)(UINTN)p;
    UINT64 caps = *(volatile UINT64 *)base, period = caps >> 32;
    if (!period || period > 100000000ULL)
        return;
    hpet = base;
    hpet_period_fs = period;
    hpet_mask = (caps & (1 << 13)) ? ~0ULL : 0xffffffffULL;
    /* Main counter only; do not enable legacy replacement or comparator IRQs. */
    UINT64 cfg = *(volatile UINT64 *)(hpet + 0x10);
    *(volatile UINT64 *)(hpet + 0x10) = (cfg & ~2ULL) | 1;
    UINT64 before = *(volatile UINT64 *)(hpet + 0xf0);
    for (UINT32 i = 0; i < 100000; ++i) {
        if (*(volatile UINT64 *)(hpet + 0xf0) != before)
            return;
        pause_cpu();
    }
    hpet = NULL;
}
static void parse_dmar(const UINT8 *t)
{
    UINT32 n = rd32(t + 4);
    if (n < 48) {
        platform.dma_allowed = FALSE;
        platform.dma_reason = "invalid Intel DMAR table";
        return;
    }
    for (UINT32 p = 48; p + 4 <= n;) {
        UINT16 type = rd16(t + p), len = rd16(t + p + 2);
        if (len < 4 || len > n - p || (type == 0 && len < 16)) {
            platform.dma_allowed = FALSE;
            platform.dma_reason = "invalid Intel DMAR unit";
            break;
        }
        if (type == 0 && len >= 16) {
            UINT64 base = rd64(t + p + 8);
            if (k_mmio_map(base, 4096) || (*(volatile UINT32 *)(UINTN)(base + 0x1c) & (1U << 31))) {
                platform.dma_allowed = FALSE;
                platform.dma_reason = "active or inaccessible Intel VT-d unit";
            }
        }
        p += len;
    }
}
static void parse_ivrs(const UINT8 *t)
{
    UINT32 n = rd32(t + 4);
    if (n < 48) {
        platform.dma_allowed = FALSE;
        platform.dma_reason = "invalid AMD IVRS table";
        return;
    }
    for (UINT32 p = 48; p + 4 <= n;) {
        UINT16 len = rd16(t + p + 2);
        if (len < 4 || len > n - p ||
            ((t[p] == 0x10 || t[p] == 0x11 || t[p] == 0x40) && len < 24)) {
            platform.dma_allowed = FALSE;
            platform.dma_reason = "invalid AMD IVRS unit";
            break;
        }
        if ((t[p] == 0x10 || t[p] == 0x11 || t[p] == 0x40) && len >= 24) {
            UINT64 base = rd64(t + p + 8) & ~0x3fffULL;
            if (k_mmio_map(base, 16384) || (*(volatile UINT64 *)(UINTN)(base + 0x18) & 1)) {
                platform.dma_allowed = FALSE;
                platform.dma_reason = "active or inaccessible AMD IOMMU unit";
            }
        }
        p += len;
    }
}
int k_platform_init(UINT64 rsdp, UINT64 trampoline)
{
    UINT32 a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    BOOLEAN acpi_complete = FALSE;
    platform.apic = (d & (1 << 9)) != 0;
    UINT32 bsp = b >> 24;
    if (platform.apic) {
        UINT64 apic_base = rdmsr(0x1b);
        platform.x2apic = (apic_base & (1 << 10)) != 0;
        if (platform.x2apic)
            bsp = (UINT32)rdmsr(0x802);
        else {
            UINT64 base = apic_base & 0x000ffffffffff000ULL;
            if (k_mmio_map(base, 4096))
                platform.apic = FALSE;
            else {
                lapic_regs = (void *)(UINTN)base;
                bsp = lapic_read(0x20) >> 24;
            }
        }
    }
    cpus[0].index = 0;
    cpus[0].apic_id = bsp;
    cpus[0].online = 1;
    platform.discovered_cpus = platform.online_cpus = 1;
    ap_trampoline = trampoline;
    if (rsdp && physical_described(rsdp, 20)) {
        const UINT8 *r = (void *)(UINTN)rsdp;
        if (!memcmp(r, "RSD PTR ", 8) && checksum(r, 20)) {
            BOOLEAN extended = r[15] >= 2 && physical_described(rsdp, 36);
            if (extended) {
                UINT32 length = rd32(r + 20);
                extended = length >= 36 && length <= 4096 && physical_described(rsdp, length) &&
                           checksum(r, length);
            }
            UINT64 address = extended ? rd64(r + 24) : rd32(r + 16);
            const UINT8 *root = acpi_table(address);
            UINT32 step = extended ? 8 : 4;
            if (root && !memcmp(root, extended ? "XSDT" : "RSDT", 4) &&
                !((rd32(root + 4) - 36) % step)) {
                acpi_complete = TRUE;
                for (UINT32 p = 36; p + step <= rd32(root + 4); p += step) {
                    const UINT8 *t = acpi_table(step == 8 ? rd64(root + p) : rd32(root + p));
                    if (!t) {
                        acpi_complete = FALSE;
                        continue;
                    }
                    if (!memcmp(t, "APIC", 4))
                        parse_madt(t);
                    else if (!memcmp(t, "FACP", 4))
                        parse_fadt(t);
                    else if (!memcmp(t, "HPET", 4))
                        parse_hpet(t);
                    else if (!memcmp(t, "DMAR", 4))
                        parse_dmar(t);
                    else if (!memcmp(t, "IVRS", 4))
                        parse_ivrs(t);
                }
            }
        }
    }
    if (!acpi_complete) {
        platform.dma_allowed = FALSE;
        platform.dma_reason = "incomplete ACPI; DMA translation state unknown";
    }
    cpuid(0, 0, &a, &b, &c, &d);
    UINT32 maxleaf = a;
    if (maxleaf >= 0x15) {
        cpuid(0x15, 0, &a, &b, &c, &d);
        if (a && b && c)
            tsc_hz = (UINT64)c * b / a;
    }
    platform.clock_source = hpet            ? "HPET"
                            : pm_timer_port ? "ACPI PM timer"
                            : tsc_hz        ? "CPUID TSC"
                                            : "PIT channel 2";
    cpuid(0, 0, &a, &b, &c, &d);
    if (a >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        platform.smep = (b & (1 << 7)) != 0;
    }
    return 0;
}
const k_platform_info *k_platform(void)
{
    return &platform;
}
static UINT32 lapic_read(UINT32 off)
{
    return platform.x2apic ? (UINT32)rdmsr(0x800 + (off >> 4))
                           : *(volatile UINT32 *)(lapic_regs + off);
}
static void lapic_write(UINT32 off, UINT32 value)
{
    if (platform.x2apic)
        wrmsr(0x800 + (off >> 4), value);
    else {
        *(volatile UINT32 *)(lapic_regs + off) = value;
        (void)*(volatile UINT32 *)(lapic_regs + 0x20);
    }
}
static void lapic_eoi(void)
{
    if (platform.apic)
        lapic_write(0xb0, 0);
}
UINT32 k_cpu_id(void)
{
    if (!platform.apic || !interrupts_ready)
        return 0;
    UINT32 id = platform.x2apic ? lapic_read(0x20) : lapic_read(0x20) >> 24;
    for (UINT32 i = 0; i < platform.discovered_cpus; ++i)
        if (cpus[i].apic_id == id)
            return i;
    return 0;
}
/* All wait paths have both a clock deadline and a finite iteration bound.
 * The PIT fallback uses channel 2, never the scheduler's channel 0. */
static int pit_delay(UINT32 us)
{
    if (!platform.pic)
        return K_ENOTSUP;
    while (us) {
        UINT32 part = MIN(us, 50000U);
        UINT16 count = (UINT16)(((UINT64)part * 1193182 + 999999) / 1000000);
        UINT8 old = inb(0x61);
        outb(0x61, (old & ~3u));
        outb(0x43, 0xb0);
        outb(0x42, (UINT8)count);
        outb(0x42, (UINT8)(count >> 8));
        outb(0x61, (old & ~2u) | 1);
        UINT32 loops = 10000000;
        while (!(inb(0x61) & 0x20) && --loops)
            pause_cpu();
        outb(0x61, old);
        if (!loops)
            return K_ETIMEDOUT;
        us -= part;
    }
    return 0;
}
static int delay_checked(UINT32 us)
{
    if (!us)
        return 0;
    UINT64 start, target, mask;
    UINT32 loops = 100000000;
    if (hpet) {
        start = *(volatile UINT64 *)(hpet + 0xf0) & hpet_mask;
        target = ((UINT64)us * 1000000000ULL + hpet_period_fs - 1) / hpet_period_fs;
        mask = hpet_mask;
        while ((((*(volatile UINT64 *)(hpet + 0xf0) & mask) - start) & mask) < target && --loops)
            pause_cpu();
    } else if (pm_timer_port) {
        start = inl(pm_timer_port) & pm_timer_mask;
        target = ((UINT64)us * 3579545 + 999999) / 1000000;
        mask = pm_timer_mask;
        while (((inl(pm_timer_port) - start) & mask) < target && --loops)
            pause_cpu();
    } else if (tsc_hz) {
        start = rdtsc();
        target = (tsc_hz / 1000000) * us;
        while (rdtsc() - start < target && --loops)
            pause_cpu();
    } else
        return pit_delay(us);
    return loops ? 0 : K_ETIMEDOUT;
}
void k_delay_us(UINT32 us)
{
    while (us) {
        UINT32 part = MIN(us, 50000U);
        if (delay_checked(part))
            break;
        us -= part;
    }
}
static BOOLEAN wait_reg(volatile UINT32 *reg, UINT32 mask, UINT32 expected, UINT32 milliseconds)
{
    for (UINT32 i = 0; i < milliseconds; ++i) {
        if ((*reg & mask) == expected)
            return TRUE;
        if (delay_checked(1000))
            return FALSE;
    }
    return (*reg & mask) == expected;
}
static void lapic_enable(void)
{
    UINT64 base = rdmsr(0x1b);
    base |= 1 << 11;
    if (platform.x2apic)
        base |= 1 << 10;
    wrmsr(0x1b, base);
    lapic_write(0x80, 0);
    lapic_write(0xf0, 0x100 | 0xff);
    lapic_write(0x320, 1 << 16);
    lapic_write(0x350, 1 << 16);
    lapic_write(0x360, 1 << 16);
    lapic_write(0x370, 0xfe);
    lapic_write(0x280, 0);
    (void)lapic_read(0x280);
    lapic_eoi();
}
int k_timer_init(UINT32 hz)
{
    if (!scheduler_ready || hz < 20 || hz > 1000)
        return K_EINVAL;
    timer_hz = hz;
    if (platform.apic) {
        lapic_enable();
        lapic_write(0x3e0, 3);
        lapic_write(0x380, 0xffffffff);
        int e = delay_checked(10000);
        UINT32 elapsed = 0xffffffff - lapic_read(0x390);
        lapic_write(0x380, 0);
        if (!e && elapsed >= 100) {
            lapic_period = (UINT32)(((UINT64)elapsed * 100) / hz);
            lapic_write(0x320, 0x20000 | 0xf0);
            lapic_write(0x380, lapic_period);
            platform.timer_source = "local APIC";
            pic_timer = FALSE;
            return 0;
        }
    }
    if (platform.pic) {
        UINT32 div = 1193182 / hz;
        if (div > 65535)
            div = 65535;
        outb(0x43, 0x36);
        outb(0x40, (UINT8)div);
        outb(0x40, (UINT8)(div >> 8));
        outb(0x21, inb(0x21) & ~1u);
        if (platform.apic)
            lapic_write(0x350, 0x700);
        pic_timer = TRUE;
        platform.timer_source = "8254 PIT / 8259 PIC";
        return 0;
    }
    platform.timer_source = "unavailable";
    return K_ENOTSUP;
}
UINT64 k_ticks(void)
{
    return __atomic_load_n(&global_ticks, __ATOMIC_RELAXED);
}
UINT64 k_uptime_ms(void)
{
    UINT64 t = k_ticks();
    return (t / timer_hz) * 1000 + (t % timer_hz) * 1000 / timer_hz;
}
UINT32 k_tick_hz(void)
{
    return timer_hz;
}
int k_cpu_get(UINT32 i, k_cpu_info *out)
{
    if (!out || i >= platform.discovered_cpus)
        return K_ENOENT;
    out->index = i;
    out->apic_id = cpus[i].apic_id;
    out->online = __atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE);
    out->ticks = __atomic_load_n(&cpus[i].ticks, __ATOMIC_RELAXED);
    out->switches = __atomic_load_n(&cpus[i].switches, __ATOMIC_RELAXED);
    out->idle_ticks = __atomic_load_n(&cpus[i].idle_ticks, __ATOMIC_RELAXED);
    return 0;
}

/* A vector-specific error-code-normalized frame for every IDT entry.
 * In 64-bit mode, the CPU pushes SS:RSP even without a CPL change.
 * C calls receive Microsoft ABI shadow space on a 16-byte aligned stack. */
extern void *irq_stubs[256];
extern void syscall_entry(void);
irq_frame *irq_dispatch(irq_frame *);
__asm__(
    ".text\n"
    ".altmacro\n"
    ".macro MAKE_ISR n\n"
    "ki_\\n:\n"
    ".if (\\n != 8) && (\\n != 10) && (\\n != 11) && (\\n != 12) && (\\n != 13) && (\\n != 14) && "
    "(\\n != 17) && (\\n != 21) && (\\n != 29) && (\\n != 30)\n"
    "pushq $0\n.endif\n"
    "pushq $\\n\njmp irq_common\n.endm\n"
    ".set vector,0\n.rept 256\nMAKE_ISR %vector\n.set vector,vector+1\n.endr\n"
    "irq_common:\n"
    "testb $3,24(%rsp)\njz 1f\nswapgs\n1:\n"
    "irq_save_regs:\n"
    "pushq %rax\npushq %rbx\npushq %rcx\npushq %rdx\npushq %rbp\npushq %rsi\npushq %rdi\n"
    "pushq %r8\npushq %r9\npushq %r10\npushq %r11\npushq %r12\npushq %r13\npushq %r14\npushq %r15\n"
    "cld\nmovq %rsp,%rcx\nandq $-16,%rsp\nsubq $32,%rsp\ncall irq_dispatch\n"
    "movq %rax,%rsp\n"
    /* Publish stack ownership only after RSP has changed. A remote reaper may
     * now release the old stack. NMI may interrupt a SWAPGS window and never
     * schedules a healthy task; its return must not dereference GS. */
    "cmpq $2,120(%rsp)\nje 3f\nmovq %gs:24,%rax\nmovq %rax,%gs:16\n3:\n"
    "popq %r15\npopq %r14\npopq %r13\npopq %r12\npopq %r11\npopq %r10\npopq %r9\npopq %r8\n"
    "popq %rdi\npopq %rsi\npopq %rbp\npopq %rdx\npopq %rcx\npopq %rbx\npopq %rax\n"
    "addq $16,%rsp\ntestb $3,8(%rsp)\njz 2f\nswapgs\n2:\niretq\n"
    ".globl syscall_entry\nsyscall_entry:\n"
    "swapgs\nmovq %rsp,%gs:8\nmovq %gs:0,%rsp\n"
    "pushq $0x1b\npushq %gs:8\npushq %r11\npushq $0x23\npushq %rcx\npushq $0\npushq $128\n"
    "jmp irq_save_regs\n"
    ".section .rdata,\"dr\"\n.balign 8\n.globl irq_stubs\nirq_stubs:\n"
    ".macro ISR_PTR n\n.quad ki_\\n\n.endm\n"
    ".set vector,0\n.rept 256\nISR_PTR %vector\n.set vector,vector+1\n.endr\n"
    ".noaltmacro\n.text\n");

static void cpu_tables(cpu_state *cpu)
{
    mem_zero(&cpu->tss, sizeof(cpu->tss));
    cpu->gdt[0] = 0;
    cpu->gdt[1] = 0x00af9a000000ffffULL;
    cpu->gdt[2] = 0x00cf92000000ffffULL;
    cpu->gdt[3] = 0x00cff2000000ffffULL;
    cpu->gdt[4] = 0x00affa000000ffffULL;
    cpu->tss.iomap = sizeof(tss64);
    cpu->tss.ist[0] = (UINT64)(UINTN)(cpu->fault_stack + sizeof(cpu->fault_stack));
    cpu->tss.ist[1] = (UINT64)(UINTN)(cpu->nmi_stack + sizeof(cpu->nmi_stack));
    cpu->tss.ist[2] = (UINT64)(UINTN)(cpu->machine_stack + sizeof(cpu->machine_stack));
    UINT64 base = (UINT64)(UINTN)&cpu->tss, limit = sizeof(tss64) - 1;
    cpu->gdt[5] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ULL << 40) |
                  ((limit & 0xf0000) << 32) | ((base & 0xff000000) << 32);
    cpu->gdt[6] = base >> 32;
    descriptor_ptr gdtr = {sizeof(cpu->gdt) - 1, (UINT64)(UINTN)cpu->gdt};
    __asm__ volatile("lgdt %0\npushq $8\nleaq 1f(%%rip),%%rax\npushq %%rax\nlretq\n1:\n"
                     "movw $0x10,%%ax\nmovw %%ax,%%ds\nmovw %%ax,%%es\nmovw %%ax,%%ss\n"
                     "xor %%eax,%%eax\nmovw %%ax,%%fs\nmovw %%ax,%%gs\n"
                     "movw $0x28,%%ax\nltr %%ax\nlidt %1" ::"m"(gdtr),
                     "m"(idtr)
                     : "rax", "memory");
    UINT64 cr0, cr4;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    cr0 = (cr0 & ~((1ULL << 2) | (1ULL << 3))) | (1 << 1) | (1 << 16);
    __asm__ volatile("mov %0,%%cr0" ::"r"(cr0) : "memory");
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    /* SSE/x87 are saved eagerly. Disable unsaved AVX, FSGSBASE and debug
     * extensions inherited from firmware; user cannot replace kernel GS. */
    cr4 &= ~((1ULL << 18) | (1ULL << 16) | (1ULL << 11) | (1ULL << 21));
    cr4 |= (1 << 9) | (1 << 10);
    if (platform.smep)
        cr4 |= 1 << 20;
    __asm__ volatile("mov %0,%%cr4" ::"r"(cr4) : "memory");
    wrmsr(0xc0000101, (UINT64)(UINTN)cpu);
    wrmsr(0xc0000102, 0);
    wrmsr(0xc0000100, 0);
    wrmsr(0xc0000080, rdmsr(0xc0000080) | 1 | (platform.nx ? (1 << 11) : 0));
    wrmsr(0xc0000081, (0x13ULL << 48) | (8ULL << 32));
    wrmsr(0xc0000082, (UINT64)(UINTN)syscall_entry);
    wrmsr(0xc0000084, 0x57700);
    UINT32 a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (d & (1 << 11))
        wrmsr(0x174, 0); /* unconfigured SYSENTER must fault in ring 3 */
}
void kernel_interrupts_init(void)
{
    __asm__ volatile("cli");
    for (UINT32 i = 0; i < 256; ++i) {
        UINT64 p = (UINT64)(UINTN)irq_stubs[i];
        idt[i] =
            (idt_entry){(UINT16)p,         8, 0, (UINT8)(i == 128 ? 0xee : 0x8e), (UINT16)(p >> 16),
                        (UINT32)(p >> 32), 0};
    }
    idt[8].ist = 1;
    idt[2].ist = 2;
    idt[18].ist = 3;
    idtr = (descriptor_ptr){sizeof(idt) - 1, (UINT64)(UINTN)idt};
    cpu_tables(&cpus[0]);
    interrupts_ready = TRUE;
    if (platform.pic) {
        outb(0x20, 0x11);
        outb(0xa0, 0x11);
        outb(0x21, 0x20);
        outb(0xa1, 0x28);
        outb(0x21, 4);
        outb(0xa1, 2);
        outb(0x21, 1);
        outb(0xa1, 1);
        outb(0x21, 0xff);
        outb(0xa1, 0xff);
    }
    if (platform.apic) {
        lapic_enable();
        for (UINT32 i = 0; i < ioapic_count; ++i)
            for (UINT32 j = 0; j < ioapics[i].count; ++j)
                ioapic_write(i, (UINT8)(0x10 + 2 * j), 1 << 16);
        if (platform.ps2 && cpus[0].apic_id <= 255) {
            for (UINT32 channel = 0; channel < 2; ++channel)
                for (UINT32 i = 0; i < ioapic_count; ++i) {
                    UINT32 gsi = isa_input_gsi[channel];
                    UINT16 mode = isa_input_flags[channel];
                    if (gsi < ioapics[i].gsi || gsi - ioapics[i].gsi >= ioapics[i].count)
                        continue;
                    UINT8 r = (UINT8)(0x10 + 2 * (gsi - ioapics[i].gsi));
                    UINT32 flags = channel ? 0x2c : 0x21;
                    if ((mode & 3) == 3)
                        flags |= 1 << 13;
                    if (((mode >> 2) & 3) == 3)
                        flags |= 1 << 15;
                    ioapic_write(i, r + 1, cpus[0].apic_id << 24);
                    ioapic_write(i, r, flags);
                }
        }
    } else if (platform.ps2 && platform.pic) {
        outb(0x21, 0xf9); /* first port plus slave-PIC cascade */
        outb(0xa1, 0xef); /* second port IRQ12 */
    }
}
void kernel_interrupts_enable(void)
{
    __asm__ volatile("sti" ::: "memory");
}

static void pic_eoi(UINT32 v)
{
    if (v == 0x27) {
        outb(0x20, 0x0b);
        if (!(inb(0x20) & 0x80))
            return;
    }
    if (v == 0x2f) {
        outb(0xa0, 0x0b);
        if (!(inb(0xa0) & 0x80)) {
            outb(0x20, 0x20);
            return;
        }
    }
    if (v >= 0x28)
        outb(0xa0, 0x20);
    outb(0x20, 0x20);
}
static irq_frame *irq_dispatch_inner(irq_frame *f)
{
    UINT32 v = (UINT32)f->vector, c = k_cpu_id();
    if (v == 2)
        return f; /* NMI uses a private IST and does not touch scheduler locks */
    if (v == 0xff)
        return f; /* architectural spurious APIC vector has no EOI */
    if (v < 32) {
        UINT64 cr2;
        __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
        task *t = cpus[c].current;
        if ((f->cs & 3) == 3 && t && t->process && v != 8 && v != 18) {
            UINT64 lock = k_spin_lock(&sched_lock);
            k_process *p = t->process;
            p->fault_vector = v;
            p->fault_ip = f->rip;
            p->fault_address = v == 14 ? cr2 : 0;
            p->exit_code = -(INT32)(128 + v);
            p->state = K_TASK_ZOMBIE;
            t->exit_code = p->exit_code;
            t->state = K_TASK_ZOMBIE;
            k_spin_unlock(&sched_lock, lock);
            return schedule(f, TRUE);
        }
        console_panic_mode();
        con_print("\nCPU ");
        con_print_uint(c);
        con_print(" exception ");
        con_print_uint(v);
        con_print(" error=");
        con_print_hex(f->error);
        con_print(" RIP=");
        con_print_hex(f->rip);
        con_print(" CR2=");
        con_print_hex(cr2);
        kernel_halt("kernel exception");
    }
    if (v == 128) {
        if ((f->cs & 3) != 3) {
            f->rax = (UINT64)K_EPERM;
            return f;
        }
        __asm__ volatile("sti" ::: "memory");
        INT64 result = syscall_dispatch(f);
        __asm__ volatile("cli" ::: "memory");
        f->rax = (UINT64)result;
        return f;
    }
    if (v == 129)
        return scheduler_ready ? schedule(f, TRUE) : f;
    if (v == 0xf0 || (v == 0x20 && pic_timer)) {
        if (v == 0x20)
            pic_eoi(v);
        else
            lapic_eoi();
        ++cpus[c].ticks;
        if (c == 0)
            __atomic_add_fetch(&global_ticks, 1, __ATOMIC_RELAXED);
        return scheduler_ready ? schedule(f, FALSE) : f;
    }
    if (v == 0x21 || v == 0x2c)
        ps2_irq();
    if (v >= 0x20 && v < 0x30) {
        if (platform.apic && !pic_timer)
            lapic_eoi();
        else {
            if (platform.pic)
                pic_eoi(v);
            if (platform.apic)
                lapic_eoi();
        }
    } else if (platform.apic) {
        if (v == 0xfe) {
            lapic_write(0x280, 0);
            (void)lapic_read(0x280);
        }
        lapic_eoi();
    }
    return f;
}
irq_frame *irq_dispatch(irq_frame *f)
{
    irq_frame *r = irq_dispatch_inner(f);
    /* User-controlled RSP and NT must not turn IRET into a ring-0 #GP.
     * This also covers a timer interrupt after an application corrupts RSP. */
    while ((r->cs & 3) == 3) {
        task *t = current_task();
        BOOLEAN bad = r->cs != 0x23 || r->ss != 0x1b || r->rsp < K_USER_BASE ||
                      r->rsp >= K_USER_LIMIT || r->rip < K_USER_BASE || r->rip >= K_USER_LIMIT ||
                      !t || !t->process;
        if (!bad) {
            r->rflags = (r->rflags & 0x250dd5ULL) |
                        0x202;
            break;
        }
        if (!t || !t->process)
            kernel_halt("invalid user return without process");
        UINT64 saved = k_spin_lock(&sched_lock);
        t->state = K_TASK_ZOMBIE;
        t->exit_code = -141;
        t->preempt_depth = 0;
        t->process->state = K_TASK_ZOMBIE;
        t->process->exit_code = -141;
        t->process->fault_vector = 13;
        t->process->fault_address = r->rsp;
        t->process->fault_ip = r->rip;
        k_spin_unlock(&sched_lock, saved);
        r = schedule(r, TRUE);
    }
    return r;
}

static task *current_task(void)
{
    return cpus[k_cpu_id()].current;
}
UINT32 k_task_id(void)
{
    task *t = current_task();
    return t ? t->id : 0;
}
void k_preempt_disable(void)
{
    UINT64 f = k_irq_save();
    task *t = current_task();
    if (t)
        ++t->preempt_depth;
    k_irq_restore(f);
}
void k_preempt_enable(void)
{
    UINT64 f = k_irq_save();
    task *t = current_task();
    if (t && t->preempt_depth)
        --t->preempt_depth;
    k_irq_restore(f);
}
static UINT64 cpu_floor(UINT32 cpu)
{
    UINT64 min = ~0ULL;
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i) {
        task *t = &tasks[i];
        if (t->cpu == cpu && !t->idle && (t->state == K_TASK_READY || t->state == K_TASK_RUNNING) &&
            t->vruntime < min)
            min = t->vruntime;
    }
    if (min != ~0ULL && min > cpus[cpu].fair_floor)
        cpus[cpu].fair_floor = min;
    return cpus[cpu].fair_floor;
}
static void task_ready(task *t)
{
    UINT64 floor = cpu_floor(t->cpu);
    if (t->vruntime < floor)
        t->vruntime = floor;
    t->state = K_TASK_READY;
    t->wait_object = NULL;
    __atomic_store_n(&cpus[t->cpu].work_pending, 1, __ATOMIC_RELEASE);
}
__attribute__((noreturn)) static void task_bootstrap(void)
{
    task *t = current_task();
    if (!t || !t->entry)
        kernel_halt("bad task entry");
    t->entry(t->arg);
    k_task_exit(0);
}
static void idle_entry(void *unused)
{
    (void)unused;
    for (;;)
        __asm__ volatile("sti;hlt" ::: "memory");
}
static task *new_task(const char *name, k_task_entry entry, void *arg, UINT32 weight, UINT32 cpu,
                      BOOLEAN idle)
{
    if (!next_task_id)
        return NULL;
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i)
        if (tasks[i].state == K_TASK_UNUSED) {
            task *t = &tasks[i];
            mem_zero(t, sizeof(*t));
            UINT64 stack = k_pmm_alloc_pages(STACK_PAGES, 0);
            if (!stack)
                return NULL;
            t->id = next_task_id++;
            t->cpu = cpu;
            t->weight = weight;
            t->stack = stack;
            t->stack_top = stack + STACK_PAGES * 4096;
            t->entry = entry;
            t->arg = arg;
            t->idle = idle;
            t->frame = (void *)(UINTN)((t->stack_top - 64 - sizeof(irq_frame)) & ~15ULL);
            mem_zero(t->frame, sizeof(*t->frame));
            t->frame->rip = (UINT64)(UINTN)task_bootstrap;
            t->frame->cs = 8;
            t->frame->ss = 0x10;
            t->frame->rflags = 0x202;
            t->frame->rsp = t->stack_top - 40;
            str_copy(t->name, name, sizeof(t->name));
            mem_copy(t->fx, initial_fx, sizeof(t->fx));
            t->vruntime = cpu_floor(cpu);
            t->state = K_TASK_READY;
            if (!idle)
                __atomic_store_n(&cpus[cpu].work_pending, 1, __ATOMIC_RELEASE);
            return t;
        }
    return NULL;
}
int k_scheduler_init(void)
{
    if (!interrupts_ready || scheduler_ready)
        return K_EINVAL;
    mem_zero(initial_fx, sizeof(initial_fx));
    initial_fx[0] = 0x7f;
    initial_fx[1] = 3;
    wr32(initial_fx + 24, 0x1f80);
    __asm__ volatile("fxrstor64 %0" ::"m"(initial_fx) : "memory");
    task *boot = &tasks[0];
    boot->id = next_task_id++;
    boot->cpu = 0;
    boot->weight = 1024;
    boot->state = K_TASK_RUNNING;
    str_copy(boot->name, "shell", sizeof(boot->name));
    mem_copy(boot->fx, initial_fx, sizeof(boot->fx));
    cpus[0].current = boot;
    cpus[0].stack_owner = boot;
    task *idle = new_task("idle", idle_entry, NULL, 1, 0, TRUE);
    if (!idle)
        return K_ENOMEM;
    cpus[0].idle = idle;
    scheduler_ready = TRUE;
    return 0;
}
int k_task_create(const char *name, k_task_entry entry, void *arg, UINT32 weight, UINT32 cpu,
                  UINT32 *id)
{
    if (!scheduler_ready || !name || !entry || !id || !weight || weight > 4096 ||
        cpu >= platform.discovered_cpus || !__atomic_load_n(&cpus[cpu].online, __ATOMIC_ACQUIRE))
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    task *t = new_task(name, entry, arg, weight, cpu, FALSE);
    if (t)
        *id = t->id;
    k_spin_unlock(&sched_lock, f);
    return t ? 0 : K_ENOMEM;
}
static irq_frame *schedule(irq_frame *frame, BOOLEAN forced)
{
    UINT32 c = k_cpu_id();
    cpu_state *cpu = &cpus[c];
    task *old = cpu->current;
    if (!forced && old && old->idle) {
        ++cpu->idle_ticks;
        /* An empty CPU stays in HLT without taking the global lock or
         * scanning all tasks at every timer interrupt. A producer publishes
         * work_pending under sched_lock; the next local tick sees it. */
        if (!__atomic_load_n(&cpu->work_pending, __ATOMIC_ACQUIRE)) {
            ++old->runtime;
            return frame;
        }
    }
    UINT64 f = k_spin_lock(&sched_lock);
    if (!old) {
        k_spin_unlock(&sched_lock, f);
        return frame;
    }
    old->frame = frame;
    if (forced && old->state == K_TASK_RUNNING && !old->idle && !old->preempt_depth)
        old->vruntime += (1024ULL * 1024 + old->weight - 1) / old->weight;
    if (!forced) {
        ++old->runtime;
        if (!old->idle)
            old->vruntime += (1024ULL * 1024 + old->weight - 1) / old->weight;
        if (cpu->slice)
            --cpu->slice;
    }
    UINT64 now = k_ticks();
    BOOLEAN sleepers = FALSE;
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i) {
        task *t = &tasks[i];
        if (t->cpu == c && t->state == K_TASK_SLEEPING) {
            if (now >= t->wake)
                task_ready(t);
            else
                sleepers = TRUE;
        }
    }
    if (old->state == K_TASK_RUNNING &&
        (old->preempt_depth || (!forced && cpu->slice && !old->idle))) {
        k_spin_unlock(&sched_lock, f);
        return frame;
    }
    if (old->state == K_TASK_RUNNING) {
        old->state = K_TASK_READY;
        if (old->process)
            old->process->state = K_TASK_READY;
    }
    task *best = NULL;
    UINT64 weights = 0;
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i) {
        task *t = &tasks[i];
        if (t->cpu != c || t->state != K_TASK_READY || t->idle)
            continue;
        weights += t->weight;
        if (!best || t->vruntime < best->vruntime ||
            (t->vruntime == best->vruntime && t->id < best->id))
            best = t;
    }
    if (!best)
        best = cpu->idle;
    __atomic_store_n(&cpu->work_pending, !best->idle || sleepers, __ATOMIC_RELEASE);
    best->state = K_TASK_RUNNING;
    if (best->process)
        best->process->state = K_TASK_RUNNING;
    (void)cpu_floor(c);
    UINT64 slice = weights ? (8ULL * best->weight + weights - 1) / weights : 1;
    cpu->slice = (UINT32)MIN(slice, 8ULL);
    if (!cpu->slice)
        cpu->slice = 1;
    if (best != old) {
        __asm__ volatile("fxsave64 %0" : "=m"(old->fx)::"memory");
        __asm__ volatile("fxrstor64 %0" ::"m"(best->fx) : "memory");
        cpu->current = best;
        ++cpu->switches;
        ++best->switches;
        cpu->tss.rsp[0] = best->stack_top;
        cpu->syscall_stack = best->stack_top;
        UINT64 *root = best->process ? best->process->as->pml4 : kernel_pml4;
        UINT64 *previous_root = old->process ? old->process->as->pml4 : kernel_pml4;
        if (root != previous_root)
            __asm__ volatile("mov %0,%%cr3" ::"r"(root) : "memory");
    }
    irq_frame *result = best->frame;
    k_spin_unlock(&sched_lock, f);
    return result;
}
void k_yield(void)
{
    if (scheduler_ready)
        __asm__ volatile("int $0x81" ::: "memory");
}
void k_sleep(UINT64 ms)
{
    if (!scheduler_ready || !ms) {
        k_yield();
        return;
    }
    task *t = current_task();
    if (!t || t->preempt_depth)
        return;
    /* Saturate and round upward without multiplying an untrusted u64. */
    if (ms > 86400000ULL)
        ms = 86400000ULL;
    UINT64 ticks = (ms * timer_hz + 999) / 1000;
    UINT64 f = k_spin_lock(&sched_lock);
    t->wake = k_ticks() + ticks;
    t->state = K_TASK_SLEEPING;
    k_spin_unlock(&sched_lock, f);
    k_yield();
}
__attribute__((noreturn)) void k_task_exit(INT32 code)
{
    UINT64 f = k_spin_lock(&sched_lock);
    task *t = current_task();
    if (!t || t->idle || t->id == 1) {
        k_spin_unlock(&sched_lock, f);
        kernel_halt("cannot exit shell/idle");
    }
    t->exit_code = code;
    t->state = K_TASK_ZOMBIE;
    t->preempt_depth = 0;
    if (t->process) {
        t->process->exit_code = code;
        t->process->state = K_TASK_ZOMBIE;
    }
    k_spin_unlock(&sched_lock, f);
    k_yield();
    kernel_halt("zombie resumed");
}
static void free_task(task *t)
{
    if (t->stack)
        k_pmm_free_pages(t->stack, STACK_PAGES);
    mem_zero(t, sizeof(*t));
}
int k_task_reap(UINT32 id, INT32 *exit)
{
    UINT64 f = k_spin_lock(&sched_lock);
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i)
        if (tasks[i].state != K_TASK_UNUSED && tasks[i].id == id) {
            task *t = &tasks[i];
            if (t->state != K_TASK_ZOMBIE) {
                k_spin_unlock(&sched_lock, f);
                return K_EBUSY;
            }
            for (UINT32 c = 0; c < platform.discovered_cpus; ++c)
                if (cpus[c].current == t ||
                    __atomic_load_n(&cpus[c].stack_owner, __ATOMIC_ACQUIRE) == t) {
                    k_spin_unlock(&sched_lock, f);
                    return K_EBUSY;
                }
            if (t->process) {
                k_spin_unlock(&sched_lock, f);
                return K_EPERM;
            } /* process owns its task */
            if (exit)
                *exit = t->exit_code;
            free_task(t);
            k_spin_unlock(&sched_lock, f);
            return 0;
        }
    k_spin_unlock(&sched_lock, f);
    return K_ENOENT;
}
static void task_snapshot(const task *t, k_task_info *out)
{
    *out = (k_task_info){
        t->id,      t->cpu,      t->state,    t->weight,    t->process ? t->process->id : 0,
        t->runtime, t->vruntime, t->switches, t->exit_code, {0}};
    str_copy(out->name, t->name, sizeof(out->name));
}
int k_task_get(UINT32 i, k_task_info *out)
{
    if (i >= K_MAX_TASKS || !out)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    int e = tasks[i].state == K_TASK_UNUSED ? K_ENOENT : 0;
    if (!e)
        task_snapshot(&tasks[i], out);
    k_spin_unlock(&sched_lock, f);
    return e;
}
int k_task_query(UINT32 id, k_task_info *out)
{
    if (!id || !out)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    int e = K_ENOENT;
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i)
        if (tasks[i].id == id && tasks[i].state != K_TASK_UNUSED) {
            task_snapshot(&tasks[i], out);
            e = 0;
            break;
        }
    k_spin_unlock(&sched_lock, f);
    return e;
}
/* Blocking and wakeup use the same scheduler lock as the ownership test:
 * the unlock-before-sleep race cannot lose a wakeup. */
static void wake_object(void *obj)
{
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i)
        if (tasks[i].state == K_TASK_BLOCKED && tasks[i].wait_object == obj)
            task_ready(&tasks[i]);
}
int k_mutex_trylock(k_mutex *m)
{
    if (!m || !current_task())
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    UINT32 me = k_task_id();
    int e = m->owner == me ? K_EDEADLK : m->owner ? K_EBUSY : 0;
    if (!e)
        m->owner = me;
    k_spin_unlock(&sched_lock, f);
    return e;
}
int k_mutex_lock(k_mutex *m)
{
    if (!m || !current_task())
        return K_EINVAL;
    task *t = current_task();
    for (;;) {
        UINT64 f = k_spin_lock(&sched_lock);
        if (!m->owner) {
            m->owner = t->id;
            k_spin_unlock(&sched_lock, f);
            return 0;
        }
        if (m->owner == t->id || t->preempt_depth) {
            k_spin_unlock(&sched_lock, f);
            return K_EDEADLK;
        }
        t->wait_object = m;
        t->state = K_TASK_BLOCKED;
        k_spin_unlock(&sched_lock, f);
        k_yield();
    }
}
int k_mutex_unlock(k_mutex *m)
{
    if (!m)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    if (!m->owner || m->owner != k_task_id()) {
        k_spin_unlock(&sched_lock, f);
        return K_EPERM;
    }
    m->owner = 0;
    wake_object(m);
    k_spin_unlock(&sched_lock, f);
    return 0;
}
void k_sem_init(k_semaphore *s, UINT32 n)
{
    if (s)
        s->count = n;
}
int k_sem_trywait(k_semaphore *s)
{
    if (!s)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    int e = s->count ? 0 : K_EAGAIN;
    if (!e)
        --s->count;
    k_spin_unlock(&sched_lock, f);
    return e;
}
int k_sem_wait(k_semaphore *s)
{
    if (!s || !current_task())
        return K_EINVAL;
    task *t = current_task();
    for (;;) {
        UINT64 f = k_spin_lock(&sched_lock);
        if (s->count) {
            --s->count;
            k_spin_unlock(&sched_lock, f);
            return 0;
        }
        if (t->preempt_depth) {
            k_spin_unlock(&sched_lock, f);
            return K_EDEADLK;
        }
        t->wait_object = s;
        t->state = K_TASK_BLOCKED;
        k_spin_unlock(&sched_lock, f);
        k_yield();
    }
}
void k_sem_post(k_semaphore *s)
{
    if (!s)
        return;
    UINT64 f = k_spin_lock(&sched_lock);
    if (s->count != 0xffffffff)
        ++s->count;
    wake_object(s);
    k_spin_unlock(&sched_lock, f);
}
int k_ipc_send(UINT32 target, const void *data, UINT32 n)
{
    task *sender = current_task();
    if (!sender || !data || n > K_IPC_BYTES)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    if (sender->process) {
        BOOLEAN grant = target == sender->id;
        for (UINT32 i = 0; i < sender->process->grant_count; ++i)
            if (sender->process->grants[i] == target)
                grant = TRUE;
        if (!grant) {
            k_spin_unlock(&sched_lock, f);
            return K_EPERM;
        }
    }
    for (UINT32 i = 0; i < K_MAX_TASKS; ++i) {
        task *t = &tasks[i];
        if (t->id != target || t->state == K_TASK_UNUSED || t->state == K_TASK_ZOMBIE)
            continue;
        if (t->mail_count == MAIL_DEPTH) {
            k_spin_unlock(&sched_lock, f);
            return K_EAGAIN;
        }
        k_message *msg = &t->mail[(t->mail_head + t->mail_count) % MAIL_DEPTH];
        mem_zero(msg, sizeof(*msg));
        msg->sender = sender->id;
        msg->size = n;
        mem_copy(msg->bytes, data, n);
        ++t->mail_count;
        k_spin_unlock(&sched_lock, f);
        return 0;
    }
    k_spin_unlock(&sched_lock, f);
    return K_ENOENT;
}
int k_ipc_receive(k_message *msg)
{
    task *t = current_task();
    if (!t || !msg)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    if (!t->mail_count) {
        k_spin_unlock(&sched_lock, f);
        return K_EAGAIN;
    }
    mem_copy(msg, &t->mail[t->mail_head], sizeof(*msg));
    t->mail_head = (t->mail_head + 1) % MAIL_DEPTH;
    --t->mail_count;
    k_spin_unlock(&sched_lock, f);
    return 0;
}
int k_ipc_grant(UINT32 pid, UINT32 target)
{
    if (current_task() && current_task()->process)
        return K_EPERM;
    UINT64 f = k_spin_lock(&sched_lock);
    for (UINT32 i = 0; i < MAX_PROCESSES; ++i)
        if (processes[i].used && processes[i].id == pid) {
            k_process *p = &processes[i];
            for (UINT32 j = 0; j < p->grant_count; ++j)
                if (p->grants[j] == target) {
                    k_spin_unlock(&sched_lock, f);
                    return 0;
                }
            if (p->grant_count == ARRAY_LEN(p->grants)) {
                k_spin_unlock(&sched_lock, f);
                return K_E2BIG;
            }
            p->grants[p->grant_count++] = target;
            k_spin_unlock(&sched_lock, f);
            return 0;
        }
    k_spin_unlock(&sched_lock, f);
    return K_ENOENT;
}

/* INIT/SIPI bootstrap. Every absolute value in this blob is patched after
 * copying it to an EFI-reserved page. It never assumes address 0x7000 is free. */
extern UINT8 ap_blob[], ap_blob_end[], ap_gdtr[], ap_gdt[], ap_base[], ap_pm_far[], ap_lm_far[];
extern UINT8 ap_cr3[], ap_stack[], ap_entry[], ap_argument[], ap_efer[], ap_pm[], ap_lm[];
__asm__(".text\n.balign 16\n.globl ap_blob\nap_blob:\n.code16\ncli\ncld\n"
        "movw %cs,%ax\nmovw %ax,%ds\nmovw %ax,%es\nmovw %ax,%ss\n"
        "movl ap_base-ap_blob,%esi\nlgdt ap_gdtr-ap_blob\n"
        "movl %cr0,%eax\norl $1,%eax\nmovl %eax,%cr0\nljmpl *ap_pm_far-ap_blob\n"
        ".code32\n.globl ap_pm\nap_pm:\nmovw $0x10,%ax\nmovw %ax,%ds\nmovw %ax,%es\nmovw %ax,%ss\n"
        "movl %cr4,%eax\nandl $0xfff9efff,%eax\norl $0x20,%eax\nmovl %eax,%cr4\n"
        "movl ap_cr3-ap_blob(%esi),%eax\nmovl %eax,%cr3\n"
        "movl $0xc0000080,%ecx\nrdmsr\norl ap_efer-ap_blob(%esi),%eax\nwrmsr\n"
        "movl %cr0,%eax\norl $0x80010001,%eax\nmovl %eax,%cr0\nljmp *ap_lm_far-ap_blob(%esi)\n"
        ".code64\n.globl ap_lm\nap_lm:\nmovw $0x10,%ax\nmovw %ax,%ds\nmovw %ax,%es\nmovw %ax,%ss\n"
        "movq ap_stack-ap_blob(%rsi),%rsp\nmovq ap_argument-ap_blob(%rsi),%rcx\n"
        "movq ap_entry-ap_blob(%rsi),%rax\nxorq %rbp,%rbp\nsubq $32,%rsp\ncall *%rax\n"
        "1: cli\nhlt\njmp 1b\n"
        ".balign 8\n.globl ap_gdt\nap_gdt:\n.quad "
        "0,0x00cf9a000000ffff,0x00cf92000000ffff,0x00af9a000000ffff\n"
        ".globl ap_gdtr\nap_gdtr:\n.word 31\n.long 0\n"
        ".globl ap_pm_far\nap_pm_far:\n.long 0\n.word 8\n"
        ".globl ap_lm_far\nap_lm_far:\n.long 0\n.word 0x18\n"
        ".balign 8\n.globl ap_base\nap_base:\n.long 0\n"
        ".globl ap_cr3\nap_cr3:\n.long 0\n"
        ".globl ap_efer\nap_efer:\n.long 0\n.balign 8\n"
        ".globl ap_stack\nap_stack:\n.quad 0\n"
        ".globl ap_entry\nap_entry:\n.quad 0\n"
        ".globl ap_argument\nap_argument:\n.quad 0\n"
        ".globl ap_blob_end\nap_blob_end:\n");
static int ap_cache_init(void)
{
    UINT32 a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (((d & (1 << 16)) != 0) != has_pat || ((d & (1 << 12)) != 0) != has_mtrr)
        return K_ENOTSUP;
    if (saved_phys_bits) {
        cpuid(0x80000000, 0, &a, &b, &c, &d);
        if (a < 0x80000008)
            return K_ENOTSUP;
        cpuid(0x80000008, 0, &a, &b, &c, &d);
        if ((a & 255) != saved_phys_bits)
            return K_ENOTSUP;
    }
    if (has_mtrr) {
        UINT64 cap = rdmsr(0xfe);
        if ((cap & 255) != saved_mtrr_count || (((cap & (1 << 8)) != 0) != mtrr_fixed_supported))
            return K_ENOTSUP;
    }
    UINT64 cr0;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    UINT64 uncached = (cr0 | (1ULL << 30)) & ~(1ULL << 29);
    __asm__ volatile("mov %0,%%cr0;wbinvd" ::"r"(uncached) : "memory");
    if (has_mtrr) {
        wrmsr(0x2ff, saved_mtrr_default & ~(1ULL << 11));
        if (mtrr_fixed_supported) {
            static const UINT16 msrs[] = {0x250, 0x258, 0x259, 0x268, 0x269, 0x26a,
                                          0x26b, 0x26c, 0x26d, 0x26e, 0x26f};
            for (UINT32 i = 0; i < 11; ++i)
                wrmsr(msrs[i], saved_mtrr_fixed[i]);
        }
        for (UINT32 i = 0; i < saved_mtrr_count * 2; ++i)
            wrmsr(0x200 + i, saved_mtrr_variable[i]);
    }
    if (has_pat)
        wrmsr(0x277, saved_pat);
    if (has_mtrr)
        wrmsr(0x2ff, saved_mtrr_default);
    __asm__ volatile("wbinvd;mov %0,%%cr3" ::"r"(kernel_pml4) : "memory");
    cr0 &= ~((1ULL << 30) | (1ULL << 29));
    __asm__ volatile("mov %0,%%cr0" ::"r"(cr0) : "memory");
    return 0;
}
__attribute__((noreturn)) static void ap_main(cpu_state *cpu)
{
    if (ap_cache_init())
        for (;;)
            __asm__ volatile("cli;hlt");
    cpu_tables(cpu);
    lapic_enable();
    __asm__ volatile("fxrstor64 %0" ::"m"(initial_fx) : "memory");
    cpu->current = cpu->idle;
    cpu->stack_owner = cpu->idle;
    cpu->idle->state = K_TASK_RUNNING;
    cpu->tss.rsp[0] = cpu->idle->stack_top;
    cpu->syscall_stack = cpu->idle->stack_top;
    lapic_write(0x3e0, 3);
    lapic_write(0x320, 0x20000 | 0xf0);
    lapic_write(0x380, lapic_period);
    __atomic_store_n(&cpu->online, 1, __ATOMIC_RELEASE);
    for (;;)
        __asm__ volatile("sti;hlt" ::: "memory");
}
static int send_ipi(UINT32 target, UINT32 command)
{
    if (platform.x2apic) {
        wrmsr(0x830, ((UINT64)target << 32) | command);
        return 0;
    }
    if (target > 255)
        return K_ENOTSUP;
    for (UINT32 n = 0; n < 10000; ++n) {
        if (!(lapic_read(0x300) & (1 << 12))) {
            lapic_write(0x310, target << 24);
            lapic_write(0x300, command);
            for (UINT32 j = 0; j < 10000; ++j) {
                if (!(lapic_read(0x300) & (1 << 12)))
                    return 0;
                k_delay_us(10);
            }
            return K_ETIMEDOUT;
        }
        k_delay_us(10);
    }
    return K_ETIMEDOUT;
}
int k_smp_start(void)
{
    if (k_cpu_id() != 0 || !scheduler_ready || smp_started)
        return K_EBUSY;
    if (platform.discovered_cpus == 1)
        return 0;
    if (!platform.apic || !lapic_period || pic_timer || !ap_trampoline || (ap_trampoline & 4095) ||
        ap_trampoline >= 0x100000 || ap_blob_end - ap_blob > 4096)
        return K_ENOTSUP;
    /* Make the trampoline executable, before kernel page tables become fixed. */
    int e = map_identity(ap_trampoline, 4096, PAGE_RW);
    if (e)
        return e;
    __asm__ volatile("mov %0,%%cr3" ::"r"(kernel_pml4) : "memory");
    UINT8 *blob = (void *)(UINTN)ap_trampoline;
    mem_copy(blob, ap_blob, ap_blob_end - ap_blob);
    wr32(blob + (ap_base - ap_blob), (UINT32)ap_trampoline);
    wr32(blob + (ap_gdtr - ap_blob) + 2, (UINT32)(ap_trampoline + (ap_gdt - ap_blob)));
    wr32(blob + (ap_pm_far - ap_blob), (UINT32)(ap_trampoline + (ap_pm - ap_blob)));
    wr32(blob + (ap_lm_far - ap_blob), (UINT32)(ap_trampoline + (ap_lm - ap_blob)));
    wr32(blob + (ap_cr3 - ap_blob), (UINT32)(UINTN)kernel_pml4);
    wr32(blob + (ap_efer - ap_blob), (1 << 8) | (platform.nx ? (1 << 11) : 0));
    wr64(blob + (ap_entry - ap_blob), (UINT64)(UINTN)ap_main);
    smp_started = TRUE;
    for (UINT32 i = 1; i < platform.discovered_cpus; ++i) {
        UINT64 flags = k_spin_lock(&sched_lock);
        task *idle = new_task("idle", idle_entry, NULL, 1, i, TRUE);
        if (!idle) {
            k_spin_unlock(&sched_lock, flags);
            return K_ENOMEM;
        }
        cpus[i].idle = idle;
        cpus[i].current = idle;
        idle->state = K_TASK_RUNNING;
        k_spin_unlock(&sched_lock, flags);
        wr64(blob + (ap_stack - ap_blob), idle->stack_top);
        wr64(blob + (ap_argument - ap_blob), (UINT64)(UINTN)&cpus[i]);
        __asm__ volatile("mfence" ::: "memory");
        UINT32 target = cpus[i].apic_id;
        e = send_ipi(target, 0xc500);
        if (!e)
            e = delay_checked(10000);
        if (!e)
            e = send_ipi(target, 0x8500);
        if (!e)
            e = send_ipi(target, 0x600 | (UINT32)(ap_trampoline >> 12));
        if (!e)
            e = delay_checked(200);
        if (!e && !__atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE))
            e = send_ipi(target, 0x600 | (UINT32)(ap_trampoline >> 12));
        for (UINT32 j = 0; !e && j < 1000 && !__atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE);
             ++j)
            e = delay_checked(1000);
        if (e || !__atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE)) {
            /* Do not reuse the shared handoff page after a late/failed AP.
             * Keep its stack allocated; request INIT and stop starting APs. */
            (void)send_ipi(target, 0xc500);
            return e ? e : K_ETIMEDOUT;
        }
        ++platform.online_cpus;
    }
    return 0;
}

/* Segment-zero PCI configuration mechanism #1, shared by storage and input.
 * Only command-register bits are changed; status W1C bits are never echoed. */
static k_spinlock pci_lock = K_SPINLOCK_INIT;
static UINT32 pci_read_32(UINT8 bus, UINT8 slot, UINT8 func, UINT8 off)
{
    UINT64 f = k_spin_lock(&pci_lock);
    outl(0xcf8, 0x80000000U | ((UINT32)bus << 16) | ((UINT32)slot << 11) | ((UINT32)func << 8) |
                    (off & 0xfc));
    UINT32 v = inl(0xcfc);
    k_spin_unlock(&pci_lock, f);
    return v;
}
static void pci_write_32(UINT8 bus, UINT8 slot, UINT8 func, UINT8 off, UINT32 value)
{
    UINT64 f = k_spin_lock(&pci_lock);
    outl(0xcf8, 0x80000000U | ((UINT32)bus << 16) | ((UINT32)slot << 11) | ((UINT32)func << 8) |
                    (off & 0xfc));
    outl(0xcfc, value);
    k_spin_unlock(&pci_lock, f);
}
static UINT32 pci_get(UINT16 bdf, UINT8 off)
{
    return pci_read_32(bdf >> 8, (bdf >> 3) & 31, bdf & 7, off);
}
static void pci_put(UINT16 bdf, UINT8 off, UINT32 v)
{
    pci_write_32(bdf >> 8, (bdf >> 3) & 31, bdf & 7, off, v);
}
static void pci_command(UINT16 bdf, UINT16 add, UINT16 remove)
{
    UINT16 cmd = (UINT16)pci_get(bdf, 4);
    cmd = (cmd | add) & ~remove;
    pci_put(bdf, 4, cmd);
}
static UINT64 pci_mbar(UINT16 bdf, UINT8 reg)
{
    UINT32 low = pci_get(bdf, reg);
    if (!low || low == 0xffffffff || (low & 1))
        return 0;
    UINT64 base = low & ~15U;
    if ((low & 6) == 4) {
        if (reg >= 0x24)
            return 0;
        base |= (UINT64)pci_get(bdf, reg + 4) << 32;
    } else if (low & 6)
        return 0;
    return base;
}
static void decimal_name(char *out, const char *prefix, UINT32 n)
{
    char digits[11];
    UINT32 count = 0;
    do {
        digits[count++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    UINTN pos = str_len(prefix);
    mem_copy(out, prefix, pos);
    while (count)
        out[pos++] = digits[--count];
    out[pos] = 0;
}
enum { DISK_NVME = 1, DISK_AHCI = 2, DISK_ATA = 3, DISK_RAM = 4 };
typedef struct {
    UINT32 kind, controller, nsid, port;
    k_disk_info info;
    UINT8 *ram;
    UINT8 flush_command; /* zero: persistent writes unavailable */
} disk;
static disk disks[K_MAX_DISKS];
static UINT32 disk_count;
static BOOLEAN storage_initialized;
static k_mutex storage_mutex = K_MUTEX_INIT;
static disk *disk_add(UINT32 kind, UINT32 sector_size, UINT64 sectors, const char *model,
                      const char *driver)
{
    if (disk_count == K_MAX_DISKS || !sectors || !power2(sector_size) || sector_size < 512 ||
        sector_size > 4096 || sectors > ~0ULL / sector_size)
        return NULL;
    disk *d = &disks[disk_count];
    mem_zero(d, sizeof(*d));
    d->kind = kind;
    d->info.id = disk_count;
    d->info.sector_size = sector_size;
    d->info.sectors = sectors;
    d->info.read_only = kind != DISK_RAM;
    d->info.online = TRUE;
    str_copy(d->info.model, model, sizeof(d->info.model));
    str_copy(d->info.driver, driver, sizeof(d->info.driver));
    decimal_name(d->info.name, "disk", disk_count);
    ++disk_count;
    return d;
}
static void trim_model(char *m)
{
    UINTN n = str_len(m);
    while (n && m[n - 1] == ' ')
        m[--n] = 0;
}
static int ata_identity(const UINT8 *id, UINT32 *sector, UINT64 *count, char model[48],
                        BOOLEAN *lba48)
{
    if (!(rd16(id + 98) & (1 << 9)))
        return K_ENOTSUP;
    UINT16 w83 = rd16(id + 166);
    *lba48 = (w83 & 0xc400) == 0x4400;
    *count = *lba48 ? rd64(id + 200) : rd32(id + 120);
    if (!*count || (*lba48 && *count > (1ULL << 48)) || (!*lba48 && *count > (1ULL << 28)))
        return K_ENOTSUP;
    *sector = 512;
    UINT16 w106 = rd16(id + 212);
    if ((w106 & 0xd000) == 0x5000) {
        UINT32 words = rd32(id + 234);
        if (words < 256 || words > 2048)
            return K_ENOTSUP;
        *sector = words * 2;
    }
    if (!power2(*sector))
        return K_ENOTSUP;
    for (UINT32 i = 0; i < 40; i += 2) {
        model[i] = (char)id[54 + i + 1];
        model[i + 1] = (char)id[54 + i];
    }
    model[40] = 0;
    trim_model(model);
    return 0;
}

/* NVMe: one admin and one I/O queue pair per controller. All buffers are
 * pinned 4 KiB pages; I/O uses one PRP, so no PRP-list boundary ambiguity. */
#define NVME_DEPTH 32
typedef struct {
    volatile UINT32 dw[16];
} nvme_command;
typedef struct {
    volatile UINT32 result, reserved;
    volatile UINT16 head, sqid, cid, status;
} nvme_completion;
_Static_assert(sizeof(nvme_command) == 64 && sizeof(nvme_completion) == 16, "NVMe ABI");
typedef struct {
    nvme_command *sq;
    nvme_completion *cq;
    UINT16 tail, head, cid, depth, id;
    UINT8 phase;
} nvme_queue;
typedef struct {
    UINT16 bdf;
    volatile UINT8 *regs;
    UINT32 stride, timeout_ms;
    BOOLEAN dead;
    nvme_queue admin, io;
    UINT8 *data;
} nvme_controller;
static nvme_controller nvme[8];
static UINT32 nvme_count;
static void nvme_dead(nvme_controller *n)
{
    n->dead = TRUE;
    pci_command(n->bdf, 0, 4); /* stop DMA; never recycle pinned pages */
    for (UINT32 i = 0; i < disk_count; ++i)
        if (disks[i].kind == DISK_NVME && &nvme[disks[i].controller] == n)
            disks[i].info.online = FALSE;
}
static int nvme_submit(nvme_controller *n, nvme_queue *q, const UINT32 cmd[16], UINT32 *result)
{
    if (n->dead)
        return K_EIO;
    UINT16 cid = ++q->cid;
    for (UINT32 i = 0; i < 16; ++i)
        q->sq[q->tail].dw[i] = cmd[i];
    q->sq[q->tail].dw[0] = (cmd[0] & 0xffff) | ((UINT32)cid << 16);
    __asm__ volatile("sfence" ::: "memory");
    q->tail = (q->tail + 1) % q->depth;
    *(volatile UINT32 *)(n->regs + 0x1000 + (2 * q->id) * n->stride) = q->tail;
    UINT32 budget = n->timeout_ms * 10;
    while (budget--) {
        UINT16 status = q->cq[q->head].status;
        if ((status & 1) == q->phase) {
            __asm__ volatile("lfence" ::: "memory");
            if (q->cq[q->head].cid != cid || q->cq[q->head].sqid != q->id) {
                nvme_dead(n);
                return K_EIO;
            }
            if (result)
                *result = q->cq[q->head].result;
            ++q->head;
            if (q->head == q->depth) {
                q->head = 0;
                q->phase ^= 1;
            }
            __asm__ volatile("sfence" ::: "memory");
            *(volatile UINT32 *)(n->regs + 0x1000 + (2 * q->id + 1) * n->stride) = q->head;
            return (status >> 1) ? K_EIO : 0;
        }
        if (*(volatile UINT32 *)(n->regs + 0x1c) & 2) {
            nvme_dead(n);
            return K_EIO;
        }
        if (delay_checked(100)) {
            nvme_dead(n);
            return K_ETIMEDOUT;
        }
    }
    nvme_dead(n);
    return K_ETIMEDOUT;
}
static int nvme_identify(nvme_controller *n, UINT32 ns, UINT32 cns)
{
    UINT32 cmd[16] = {0};
    cmd[0] = 6;
    cmd[1] = ns;
    cmd[6] = (UINT32)(UINTN)n->data;
    cmd[7] = (UINT32)((UINT64)(UINTN)n->data >> 32);
    cmd[10] = cns;
    mem_zero(n->data, 4096);
    return nvme_submit(n, &n->admin, cmd, NULL);
}
static int nvme_queue_alloc(nvme_queue *q, UINT16 id, UINT16 depth)
{
    q->sq = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
    q->cq = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
    q->id = id;
    q->depth = depth;
    q->phase = 1;
    return q->sq && q->cq ? 0 : K_ENOMEM;
}
static int nvme_probe(UINT16 bdf)
{
    if (!platform.dma_allowed)
        return K_ENOTSUP;
    if (nvme_count == ARRAY_LEN(nvme))
        return K_E2BIG;
    UINT64 base = pci_mbar(bdf, 0x10);
    if (!base || k_mmio_map(base, 8192))
        return K_ENOTSUP;
    pci_command(bdf, 2, 4); /* MMIO on, DMA off during ownership setup */
    nvme_controller *n = &nvme[nvme_count];
    mem_zero(n, sizeof(*n));
    n->bdf = bdf;
    n->regs = (void *)(UINTN)base;
    UINT64 cap = *(volatile UINT64 *)n->regs;
    if (!(cap & (1ULL << 37)) || ((cap >> 48) & 15) != 0 || !(cap & 0xffff))
        return K_ENOTSUP;
    n->stride = 4U << ((cap >> 32) & 15);
    if (k_mmio_map(base, 0x1000 + 4ULL * n->stride))
        return K_ENOTSUP;
    n->timeout_ms = (UINT32)((cap >> 24) & 255) * 500;
    if (n->timeout_ms < 5000)
        n->timeout_ms = 5000;
    *(volatile UINT32 *)(n->regs + 0x0c) = 0xffffffff; /* mask all interrupts; polling queues */
    *(volatile UINT32 *)(n->regs + 0x14) &= ~1U;
    if (!wait_reg((void *)(n->regs + 0x1c), 1, 0, n->timeout_ms))
        return K_ETIMEDOUT;
    UINT16 depth = (UINT16)MIN((cap & 0xffff) + 1, NVME_DEPTH);
    if (nvme_queue_alloc(&n->admin, 0, depth) || nvme_queue_alloc(&n->io, 1, depth))
        return K_ENOMEM;
    n->data = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
    if (!n->data)
        return K_ENOMEM;
    ++nvme_count;
    *(volatile UINT32 *)(n->regs + 0x24) = ((UINT32)(depth - 1) << 16) | (depth - 1);
    *(volatile UINT64 *)(n->regs + 0x28) = (UINT64)(UINTN)n->admin.sq;
    *(volatile UINT64 *)(n->regs + 0x30) = (UINT64)(UINTN)n->admin.cq;
    __asm__ volatile("sfence" ::: "memory");
    pci_command(bdf, 6, 0);
    *(volatile UINT32 *)(n->regs + 0x14) = 1 | (6 << 16) | (4 << 20);
    if (!wait_reg((void *)(n->regs + 0x1c), 1, 1, n->timeout_ms)) {
        nvme_dead(n);
        return K_ETIMEDOUT;
    }
    int e = nvme_identify(n, 0, 1);
    if (e) {
        nvme_dead(n);
        return e;
    }
    UINT32 namespace_count = rd32(n->data + 516);
    if ((n->data[512] & 15) > 6 || (n->data[512] >> 4) < 6 || (n->data[513] & 15) > 4 ||
        (n->data[513] >> 4) < 4) {
        nvme_dead(n);
        return K_ENOTSUP;
    }
    char model[48];
    mem_copy(model, n->data + 24, 40);
    model[40] = 0;
    trim_model(model);
    UINT32 cmd[16] = {0};
    cmd[0] = 5;
    cmd[6] = (UINT32)(UINTN)n->io.cq;
    cmd[7] = (UINT32)((UINT64)(UINTN)n->io.cq >> 32);
    cmd[10] = 1 | ((UINT32)(depth - 1) << 16);
    cmd[11] = 1;
    e = nvme_submit(n, &n->admin, cmd, NULL);
    if (e) {
        nvme_dead(n);
        return e;
    }
    mem_zero(cmd, sizeof(cmd));
    cmd[0] = 1;
    cmd[6] = (UINT32)(UINTN)n->io.sq;
    cmd[7] = (UINT32)((UINT64)(UINTN)n->io.sq >> 32);
    cmd[10] = 1 | ((UINT32)(depth - 1) << 16);
    cmd[11] = (1 << 16) | 1;
    e = nvme_submit(n, &n->admin, cmd, NULL);
    if (e) {
        nvme_dead(n);
        return e;
    }
    UINT32 ids[K_MAX_DISKS], count = 0;
    e = nvme_identify(n, 0, 2);
    if (!e) {
        for (UINT32 i = 0; i < K_MAX_DISKS; ++i) {
            UINT32 id = rd32(n->data + 4 * i);
            if (!id)
                break;
            ids[count++] = id;
        }
    } else if (!n->dead) {
        /* NVMe 1.0 lacks CNS=2. Query a bounded set of namespace IDs. */
        for (UINT32 i = 1; i <= namespace_count && count < K_MAX_DISKS; ++i)
            ids[count++] = i;
    }
    for (UINT32 i = 0; i < count && disk_count < K_MAX_DISKS && !n->dead; ++i) {
        if (nvme_identify(n, ids[i], 0))
            continue;
        UINT64 sectors = rd64(n->data), capacity = rd64(n->data + 8);
        UINT8 flbas = n->data[26], format = flbas & 15;
        if (!sectors || !capacity || capacity > sectors || (flbas & 0xe0) || format > n->data[25] ||
            rd16(n->data + 128 + 4 * format) || (n->data[29] & 7))
            continue;
        UINT8 shift = n->data[130 + 4 * format];
        if (shift < 9 || shift > 12)
            continue;
        disk *d = disk_add(DISK_NVME, 1U << shift, sectors, model, "nvme");
        if (d) {
            d->controller = nvme_count - 1;
            d->nsid = ids[i];
            d->flush_command = 1; /* NVMe Flush capability sentinel */
        }
    }
    return n->dead ? K_EIO : 0;
}
static int nvme_read(disk *d, UINT64 lba, UINT32 count, void *out)
{
    nvme_controller *n = &nvme[d->controller];
    UINT8 *p = out;
    while (count) {
        UINT32 sectors = MIN(count, 4096 / d->info.sector_size), cmd[16] = {0};
        cmd[0] = 2;
        cmd[1] = d->nsid;
        cmd[6] = (UINT32)(UINTN)n->data;
        cmd[7] = (UINT32)((UINT64)(UINTN)n->data >> 32);
        cmd[10] = (UINT32)lba;
        cmd[11] = (UINT32)(lba >> 32);
        cmd[12] = sectors - 1;
        int e = nvme_submit(n, &n->io, cmd, NULL);
        if (e)
            return e;
        UINT32 bytes = sectors * d->info.sector_size;
        mem_copy(p, n->data, bytes);
        p += bytes;
        count -= sectors;
        lba += sectors;
    }
    return 0;
}

/* AHCI ports have independent command lists/FIS areas/tables/bounce buffers.
 * Slot zero is used only after the firmware engine has stopped and no old
 * command remains. A failed DMA path is quarantined for the rest of the boot. */
typedef struct {
    UINT16 bdf;
    volatile UINT8 *port;
    UINT8 *list, *fis, *table, *data;
    BOOLEAN dead, lba48;
} ahci_port;
static ahci_port ahci[32];
static UINT32 ahci_count;
static void ahci_dead(ahci_port *a)
{
    a->dead = TRUE;
    pci_command(a->bdf, 0, 4);
    for (UINT32 i = 0; i < ahci_count; ++i)
        if (ahci[i].bdf == a->bdf)
            ahci[i].dead = TRUE;
    for (UINT32 i = 0; i < disk_count; ++i)
        if (disks[i].kind == DISK_AHCI && ahci[disks[i].controller].bdf == a->bdf)
            disks[i].info.online = FALSE;
}
static int ahci_issue(ahci_port *a, UINT8 command, UINT64 lba, UINT32 sectors, UINT32 bytes)
{
    BOOLEAN flush = command == 0xe7 || command == 0xea;
    if (a->dead || bytes > 4096 || (!flush && (!bytes || !sectors)) || sectors > 65535)
        return K_EINVAL;
    if (!wait_reg((void *)(a->port + 0x20), 0x88, 0, 5000)) {
        ahci_dead(a);
        return K_ETIMEDOUT;
    }
    if (*(volatile UINT32 *)(a->port + 0x38) || *(volatile UINT32 *)(a->port + 0x34))
        return K_EBUSY;
    mem_zero(a->table, 4096);
    mem_zero(a->list, 32);
    a->list[0] = 5 | ((command == 0x35 || command == 0xca) ? 0x40 : 0);
    a->list[2] = bytes ? 1 : 0;
    wr64(a->list + 8, (UINT64)(UINTN)a->table);
    UINT8 *fis = a->table;
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = command;
    fis[7] = 0x40;
    fis[4] = (UINT8)lba;
    fis[5] = (UINT8)(lba >> 8);
    fis[6] = (UINT8)(lba >> 16);
    fis[8] = (UINT8)(lba >> 24);
    fis[9] = (UINT8)(lba >> 32);
    fis[10] = (UINT8)(lba >> 40);
    fis[12] = (UINT8)sectors;
    fis[13] = (UINT8)(sectors >> 8);
    if (command == 0xc8 || command == 0xca) {
        fis[7] |= (UINT8)((lba >> 24) & 15);
        fis[8] = fis[9] = fis[10] = 0;
    }
    wr64(a->table + 128, (UINT64)(UINTN)a->data);
    if (bytes)
        wr32(a->table + 140, bytes - 1);
    *(volatile UINT32 *)(a->port + 0x10) = 0xffffffff;
    *(volatile UINT32 *)(a->port + 0x30) = 0xffffffff;
    __asm__ volatile("sfence" ::: "memory");
    *(volatile UINT32 *)(a->port + 0x38) = 1;
    for (UINT32 i = 0; i < 50000; ++i) {
        UINT32 status = *(volatile UINT32 *)(a->port + 0x10);
        if (status & 0x7d000000U) {
            ahci_dead(a);
            return K_EIO;
        }
        if (!(*(volatile UINT32 *)(a->port + 0x38) & 1)) {
            __asm__ volatile("lfence" ::: "memory");
            if ((*(volatile UINT32 *)(a->port + 0x20) & 1) || rd32(a->list + 4) != bytes) {
                ahci_dead(a);
                return K_EIO;
            }
            return 0;
        }
        if (delay_checked(100)) {
            ahci_dead(a);
            return K_ETIMEDOUT;
        }
    }
    ahci_dead(a);
    return K_ETIMEDOUT;
}
static int ahci_probe(UINT16 bdf)
{
    if (!platform.dma_allowed)
        return K_ENOTSUP;
    UINT64 base = pci_mbar(bdf, 0x24);
    if (!base || k_mmio_map(base, 8192))
        return K_ENOTSUP;
    pci_command(bdf, 2, 0);
    volatile UINT8 *hba = (void *)(UINTN)base;
    if (*(volatile UINT32 *)(hba + 0x24) & 1) {
        *(volatile UINT32 *)(hba + 0x28) |= 2;
        if (!wait_reg((void *)(hba + 0x28), 0x11, 0, 5000))
            return K_EBUSY;
    }
    *(volatile UINT32 *)(hba + 4) = (*(volatile UINT32 *)(hba + 4) | 0x80000000U) & ~2U;
    UINT32 implemented = *(volatile UINT32 *)(hba + 0x0c);
    for (UINT32 port = 0; port < 32 && ahci_count < ARRAY_LEN(ahci); ++port) {
        if (!(implemented & (1U << port)))
            continue;
        volatile UINT8 *p = hba + 0x100 + port * 0x80;
        UINT32 ssts = *(volatile UINT32 *)(p + 0x28);
        if ((ssts & 15) != 3 || ((ssts >> 8) & 15) != 1 ||
            *(volatile UINT32 *)(p + 0x24) != 0x00000101)
            continue;
        *(volatile UINT32 *)(p + 0x14) = 0;
        *(volatile UINT32 *)(p + 0x18) &= ~1U;
        if (!wait_reg((void *)(p + 0x18), 1 << 15, 0, 500))
            continue;
        *(volatile UINT32 *)(p + 0x18) &= ~(1U << 4);
        if (!wait_reg((void *)(p + 0x18), 1 << 14, 0, 500))
            continue;
        if (*(volatile UINT32 *)(p + 0x38) || *(volatile UINT32 *)(p + 0x34))
            continue;
        ahci_port *a = &ahci[ahci_count];
        mem_zero(a, sizeof(*a));
        a->bdf = bdf;
        a->port = p;
        a->list = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
        a->fis = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
        a->table = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
        a->data = (void *)(UINTN)k_pmm_alloc_pages(1, 0x100000000ULL);
        if (!a->list || !a->fis || !a->table || !a->data)
            return K_ENOMEM;
        ++ahci_count;
        *(volatile UINT64 *)(p + 0) = (UINT64)(UINTN)a->list;
        *(volatile UINT64 *)(p + 8) = (UINT64)(UINTN)a->fis;
        *(volatile UINT32 *)(p + 0x30) = 0xffffffff;
        *(volatile UINT32 *)(p + 0x10) = 0xffffffff;
        pci_command(bdf, 6, 0);
        __asm__ volatile("sfence" ::: "memory");
        *(volatile UINT32 *)(p + 0x18) |= (1 << 4);
        *(volatile UINT32 *)(p + 0x18) |= 1;
        int e = ahci_issue(a, 0xec, 0, 1, 512);
        if (e)
            return e;
        UINT32 sector;
        UINT64 count;
        char model[48];
        if (ata_identity(a->data, &sector, &count, model, &a->lba48))
            continue;
        disk *d = disk_add(DISK_AHCI, sector, count, model, "ahci");
        if (d) {
            d->controller = ahci_count - 1;
            d->port = port;
            UINT16 caps = rd16(a->data + 166);
            if ((caps & 0xc000) == 0x4000)
                d->flush_command = (a->lba48 && (caps & 0x2000)) ? 0xea
                                   : (caps & 0x1000)             ? 0xe7
                                                                 : 0;
        }
    }
    return 0;
}
static int ahci_read(disk *d, UINT64 lba, UINT32 count, void *out)
{
    ahci_port *a = &ahci[d->controller];
    UINT8 *p = out;
    while (count) {
        UINT32 sectors = MIN(count, 4096 / d->info.sector_size),
               bytes = sectors * d->info.sector_size;
        int e = ahci_issue(a, a->lba48 ? 0x25 : 0xc8, lba, sectors, bytes);
        if (e)
            return e;
        mem_copy(p, a->data, bytes);
        p += bytes;
        count -= sectors;
        lba += sectors;
    }
    return 0;
}

/* Legacy ATA PIO is discovered through PCI IDE functions, not blind probing
 * arbitrary embedded I/O ports. Native and compatibility channels both work. */
typedef struct {
    UINT16 base, control;
    UINT8 slave;
    BOOLEAN lba48, dead;
} ata_device;
static ata_device ata[16];
static UINT32 ata_count;
static void ata_quarantine(ata_device *a)
{
    /* Both devices share the command/status registers. A timed-out command
     * makes the entire channel unusable until an explicit future recovery. */
    for (UINT32 i = 0; i < ata_count; ++i)
        if (ata[i].base == a->base)
            ata[i].dead = TRUE;
    for (UINT32 i = 0; i < disk_count; ++i)
        if (disks[i].kind == DISK_ATA && ata[disks[i].controller].base == a->base)
            disks[i].info.online = FALSE;
}
static void ata_settle(ata_device *a)
{
    for (int i = 0; i < 4; ++i)
        (void)inb(a->control);
}
static int ata_wait(ata_device *a, BOOLEAN drq)
{
    for (UINT32 i = 0; i < 50000; ++i) {
        UINT8 status = inb(a->base + 7);
        if (!status || status == 0xff)
            return K_ENOENT;
        if (!(status & 0x80)) {
            if (status & 0x21)
                return K_EIO;
            if (drq ? ((status & 8) != 0) : ((status & 8) == 0))
                return 0;
        }
        if (delay_checked(100))
            return K_ETIMEDOUT;
    }
    return K_ETIMEDOUT;
}
static int ata_probe_channel(UINT16 base, UINT16 control)
{
    if (!base || !control || base > 0xfff8)
        return K_EINVAL;
    for (UINT8 slave = 0; slave < 2 && ata_count < ARRAY_LEN(ata); ++slave) {
        ata_device a = {base, control, slave, FALSE, FALSE};
        outb(control, 2); /* nIEN: polled PIO, no bus-master engine */
        outb(base + 6, 0xa0 | (slave << 4));
        ata_settle(&a);
        UINT8 s = inb(base + 7);
        if (!s || s == 0xff)
            continue;
        UINT32 ready = 50000;
        while ((inb(base + 7) & 0x80) && --ready)
            k_delay_us(100);
        if (!ready)
            continue;
        outb(base + 2, 0);
        outb(base + 3, 0);
        outb(base + 4, 0);
        outb(base + 5, 0);
        outb(base + 7, 0xec);
        ata_settle(&a);
        if (ata_wait(&a, TRUE) || inb(base + 4) || inb(base + 5))
            continue;
        UINT8 identity[512];
        for (UINT32 j = 0; j < 256; ++j) {
            UINT16 w = inw(base);
            identity[2 * j] = (UINT8)w;
            identity[2 * j + 1] = (UINT8)(w >> 8);
        }
        if (ata_wait(&a, FALSE))
            continue;
        UINT32 sector;
        UINT64 sectors;
        char model[48];
        if (ata_identity(identity, &sector, &sectors, model, &a.lba48))
            continue;
        ata[ata_count] = a;
        disk *d = disk_add(DISK_ATA, sector, sectors, model, "ata-pio");
        if (d) {
            d->controller = ata_count;
            UINT16 caps = rd16(identity + 166);
            if ((caps & 0xc000) == 0x4000)
                d->flush_command = (a.lba48 && (caps & 0x2000)) ? 0xea : (caps & 0x1000) ? 0xe7 : 0;
        }
        ++ata_count;
    }
    return 0;
}
static int ata_probe(UINT16 bdf)
{
    UINT8 prog = (pci_get(bdf, 8) >> 8) & 255;
    pci_command(bdf, 1, 4);
    for (UINT32 channel = 0; channel < 2; ++channel) {
        BOOLEAN native = (prog & (channel ? 4 : 1)) != 0;
        UINT16 base = channel ? 0x170 : 0x1f0, ctrl = channel ? 0x376 : 0x3f6;
        if (native) {
            UINT32 b = pci_get(bdf, (UINT8)(0x10 + 8 * channel)),
                   c = pci_get(bdf, (UINT8)(0x14 + 8 * channel));
            if (!(b & 1) || !(c & 1) || (b & ~3U) > 65535 || (c & ~3U) > 65533)
                continue;
            base = (UINT16)(b & ~3U);
            ctrl = (UINT16)((c & ~3U) + 2);
        } else if (!platform.pic)
            continue;
        (void)ata_probe_channel(base, ctrl);
    }
    return 0;
}
static int ata_read(disk *d, UINT64 lba, UINT32 count, void *out)
{
    ata_device *a = &ata[d->controller];
    UINT8 *p = out;
    if (a->dead)
        return K_EIO;
    while (count--) {
        outb(a->control, 2);
        outb(a->base + 6, (UINT8)(0xe0 | (a->slave << 4) | (a->lba48 ? 0 : ((lba >> 24) & 15))));
        ata_settle(a);
        int e = ata_wait(a, FALSE);
        if (e) {
            ata_quarantine(a);
            return e;
        }
        if (a->lba48) {
            outb(a->base + 2, 0);
            outb(a->base + 3, (UINT8)(lba >> 24));
            outb(a->base + 4, (UINT8)(lba >> 32));
            outb(a->base + 5, (UINT8)(lba >> 40));
        }
        outb(a->base + 2, 1);
        outb(a->base + 3, (UINT8)lba);
        outb(a->base + 4, (UINT8)(lba >> 8));
        outb(a->base + 5, (UINT8)(lba >> 16));
        outb(a->base + 7, a->lba48 ? 0x24 : 0x20);
        ata_settle(a);
        e = ata_wait(a, TRUE);
        if (e) {
            ata_quarantine(a);
            return e;
        }
        for (UINT32 i = 0; i < d->info.sector_size; i += 2) {
            UINT16 w = inw(a->base);
            p[i] = (UINT8)w;
            p[i + 1] = (UINT8)(w >> 8);
        }
        e = ata_wait(a, FALSE);
        if (e) {
            ata_quarantine(a);
            return e;
        }
        p += d->info.sector_size;
        ++lba;
    }
    return 0;
}
int k_storage_init(void)
{
    if (storage_initialized || k_cpu_id() != 0 || smp_started)
        return K_EBUSY;
    storage_initialized = TRUE;
    for (UINT32 bus = 0; bus < 256; ++bus)
        for (UINT32 slot = 0; slot < 32; ++slot) {
            UINT16 bdf = (UINT16)((bus << 8) | (slot << 3));
            if ((pci_get(bdf, 0) & 0xffff) == 0xffff)
                continue;
            UINT32 functions = (pci_get(bdf, 0x0c) & 0x00800000) ? 8 : 1;
            for (UINT32 fn = 0; fn < functions; ++fn) {
                UINT16 id = bdf | fn;
                if ((pci_get(id, 0) & 0xffff) == 0xffff)
                    continue;
                UINT32 cls = pci_get(id, 8) >> 8;
                int e = 0;
                if (cls == 0x010802)
                    e = nvme_probe(id);
                else if (cls == 0x010601)
                    e = ahci_probe(id);
                else if ((cls >> 8) == 0x0101)
                    e = ata_probe(id);
                else if ((cls >> 16) == 1) {
                    con_print("storage: unsupported PCI storage class ");
                    con_print_hex(cls);
                    con_print("\n");
                }
                if (e) {
                    con_print("storage PCI ");
                    con_print_hex(id);
                    con_print(": ");
                    con_print(k_strerror(e));
                    con_print("\n");
                }
            }
        }
    return 0;
}
UINT32 k_disk_count(void)
{
    return disk_count;
}
int k_disk_get(UINT32 id, k_disk_info *out)
{
    if (id >= disk_count || !out)
        return K_ENOENT;
    *out = disks[id].info;
    return 0;
}
static int disk_range(UINT32 id, UINT64 lba, UINT32 count, UINT64 bytes)
{
    if (id >= disk_count || !count)
        return K_EINVAL;
    disk *d = &disks[id];
    if (!d->info.online)
        return K_EIO;
    if (lba >= d->info.sectors || count > d->info.sectors - lba ||
        (UINT64)count * d->info.sector_size > bytes)
        return K_EINVAL;
    return 0;
}
int k_disk_read(UINT32 id, UINT64 lba, UINT32 count, void *out, UINT64 bytes)
{
    if (!out || k_cpu_id() != 0)
        return K_EINVAL;
    int e = disk_range(id, lba, count, bytes);
    if (e)
        return e;
    e = k_mutex_lock(&storage_mutex);
    if (e)
        return e;
    disk *d = &disks[id];
    if (d->kind == DISK_NVME)
        e = nvme_read(d, lba, count, out);
    else if (d->kind == DISK_AHCI)
        e = ahci_read(d, lba, count, out);
    else if (d->kind == DISK_ATA)
        e = ata_read(d, lba, count, out);
    else if (d->kind == DISK_RAM)
        mem_copy(out, d->ram + lba * d->info.sector_size, (UINT64)count * d->info.sector_size);
    else
        e = K_ENOTSUP;
    k_mutex_unlock(&storage_mutex);
    return e;
}
int k_disk_write(UINT32 id, UINT64 lba, UINT32 count, const void *in, UINT64 bytes)
{
    if (id >= disk_count)
        return K_EINVAL;
    /* Check immutability before examining data or submitting any command. */
    if (disks[id].kind != DISK_RAM)
        return K_EROFS;
    if (!in || k_cpu_id() != 0)
        return K_EINVAL;
    int e = disk_range(id, lba, count, bytes);
    if (e)
        return e;
    e = k_mutex_lock(&storage_mutex);
    if (e)
        return e;
    mem_copy(disks[id].ram + lba * 512, in, (UINT64)count * 512);
    k_mutex_unlock(&storage_mutex);
    return 0;
}
/* Filesystem-private writes: the public raw physical-disk API stays fenced.
 * Callers hold vfs_mutex and supply a checked volume-relative range. */
static int disk_flush_locked(disk *d)
{
    if (d->kind == DISK_RAM)
        return 0;
    if (!d->flush_command)
        return K_EROFS;
    if (d->kind == DISK_NVME) {
        nvme_controller *n = &nvme[d->controller];
        UINT32 cmd[16] = {0};
        cmd[1] = d->nsid;
        return nvme_submit(n, &n->io, cmd, NULL);
    }
    if (d->kind == DISK_AHCI)
        return ahci_issue(&ahci[d->controller], d->flush_command, 0, 0, 0);
    if (d->kind == DISK_ATA) {
        ata_device *a = &ata[d->controller];
        if (a->dead)
            return K_EIO;
        outb(a->base + 6, 0xe0 | (a->slave << 4));
        ata_settle(a);
        int e = ata_wait(a, FALSE);
        if (!e) {
            outb(a->base + 7, d->flush_command);
            ata_settle(a);
            e = ata_wait(a, FALSE);
        }
        if (e)
            ata_quarantine(a);
        return e;
    }
    return K_ENOTSUP;
}
static int disk_flush(UINT32 id)
{
    if (id >= disk_count || k_cpu_id() != 0)
        return K_EINVAL;
    int e = k_mutex_lock(&storage_mutex);
    if (e)
        return e;
    e = disks[id].info.online ? disk_flush_locked(&disks[id]) : K_EIO;
    k_mutex_unlock(&storage_mutex);
    return e;
}
static int disk_write_fs(UINT32 id, UINT64 lba, UINT32 count, const void *in, UINT64 bytes)
{
    if (!in || k_cpu_id() != 0)
        return K_EINVAL;
    int e = disk_range(id, lba, count, bytes);
    if (e)
        return e;
    disk *d = &disks[id];
    if (d->kind != DISK_RAM && !d->flush_command)
        return K_EROFS;
    e = k_mutex_lock(&storage_mutex);
    if (e)
        return e;
    const UINT8 *p = in;
    while (count && !e) {
        UINT32 sectors = MIN(count, 4096 / d->info.sector_size), n = sectors * d->info.sector_size;
        if (d->kind == DISK_NVME) {
            nvme_controller *c = &nvme[d->controller];
            UINT32 cmd[16] = {0};
            mem_copy(c->data, p, n);
            cmd[0] = 1;
            cmd[1] = d->nsid;
            cmd[6] = (UINT32)(UINTN)c->data;
            cmd[7] = (UINT32)((UINT64)(UINTN)c->data >> 32);
            cmd[10] = (UINT32)lba;
            cmd[11] = (UINT32)(lba >> 32);
            cmd[12] = sectors - 1;
            e = nvme_submit(c, &c->io, cmd, NULL);
        } else if (d->kind == DISK_AHCI) {
            ahci_port *a = &ahci[d->controller];
            mem_copy(a->data, p, n);
            e = ahci_issue(a, a->lba48 ? 0x35 : 0xca, lba, sectors, n);
        } else if (d->kind == DISK_ATA) {
            ata_device *a = &ata[d->controller];
            sectors = 1;
            n = d->info.sector_size;
            if (a->dead) {
                e = K_EIO;
                break;
            }
            outb(a->control, 2);
            outb(a->base + 6,
                 (UINT8)(0xe0 | (a->slave << 4) | (a->lba48 ? 0 : ((lba >> 24) & 15))));
            ata_settle(a);
            e = ata_wait(a, FALSE);
            if (e)
                break;
            if (a->lba48) {
                outb(a->base + 2, 0);
                outb(a->base + 3, (UINT8)(lba >> 24));
                outb(a->base + 4, (UINT8)(lba >> 32));
                outb(a->base + 5, (UINT8)(lba >> 40));
            }
            outb(a->base + 2, 1);
            outb(a->base + 3, (UINT8)lba);
            outb(a->base + 4, (UINT8)(lba >> 8));
            outb(a->base + 5, (UINT8)(lba >> 16));
            outb(a->base + 7, a->lba48 ? 0x34 : 0x30);
            ata_settle(a);
            e = ata_wait(a, TRUE);
            if (!e)
                for (UINT32 i = 0; i < n; i += 2)
                    outw(a->base, rd16(p + i));
            if (!e) {
                ata_settle(a);
                e = ata_wait(a, FALSE);
            }
        } else if (d->kind == DISK_RAM)
            mem_copy(d->ram + lba * d->info.sector_size, p, n);
        else
            e = K_ENOTSUP;
        p += n;
        lba += sectors;
        count -= sectors;
    }
    if (e) {
        if (d->kind == DISK_NVME)
            nvme_dead(&nvme[d->controller]);
        else if (d->kind == DISK_AHCI)
            ahci_dead(&ahci[d->controller]);
        else if (d->kind == DISK_ATA)
            ata_quarantine(&ata[d->controller]);
    }
    k_mutex_unlock(&storage_mutex);
    return e;
}

int k_ramdisk_create(UINT32 sectors, UINT32 *id)
{
    if (!id || !sectors || sectors > 8192 || disk_count == K_MAX_DISKS || k_cpu_id() != 0)
        return K_EINVAL;
    UINT32 pages = (sectors + 7) / 8;
    UINT64 p = k_pmm_alloc_pages(pages, 0);
    if (!p)
        return K_ENOMEM;
    disk *d = disk_add(DISK_RAM, 512, sectors, "volatile test disk", "ram");
    if (!d) {
        k_pmm_free_pages(p, pages);
        return K_ENOMEM;
    }
    d->ram = (void *)(UINTN)p;
    *id = d->info.id;
    return 0;
}

#define MAX_VOLUMES 48
#define MAX_NODES 192
#define MAX_ROOTS 64
typedef struct {
    UINT32 disk, sector, spc, fat_bits, root_entries, root_cluster, cluster_count;
    UINT64 start, length, fat_start, fat_sectors, root_start, data_start;
    char source[24];
    UINT64 first_fat;
    UINT32 fat_count, fsinfo, backup_boot;
    UINT8 media;
    BOOLEAN mirrored, write_ready, write_failed, partition_ro;
    UINT8 *cache, *used, *dirty;
    UINT32 cache_pages, used_pages, dirty_pages, free_clusters, alloc_hint;
} fat_volume;
typedef struct {
    char path[K_PATH_MAX];
    UINT32 volume;
} mount_entry;
typedef struct {
    BOOLEAN used, directory;
    UINT32 parent, block_id, kind, pages;
    char name[K_NAME_MAX + 1];
    UINT8 *data;
    UINT64 size;
} ram_node;
static fat_volume volumes[MAX_VOLUMES];
static mount_entry mounts[MAX_VOLUMES];
static UINT32 volume_count, mount_count;
static ram_node nodes[MAX_NODES];
static struct {
    char name[32], path[K_PATH_MAX];
} roots[MAX_ROOTS];
static UINT32 root_count;
static k_mutex vfs_mutex = K_MUTEX_INIT;
static BOOLEAN vfs_ready;
static int fat_open(UINT32, const char *, k_file *);
static int fat_read(const k_file *, UINT64, void *, UINT64, UINT64 *);
static int fat_list(const k_file *, k_dir_callback, void *);
static int fat_refresh(k_file *);
static int fat_store_locked(UINT32, const char *, const void *, UINT64);
static int fat_create_locked(UINT32, const char *, BOOLEAN, k_file *);
static int fat_replace_locked(k_file *, const void *, UINT64);
static BOOLEAN path_prefix(const char *p, const char *root)
{
    UINTN n = str_len(root);
    return !memcmp(p, root, MIN(n, str_len(p))) && str_len(p) >= n &&
           (n == 1 || !p[n] || p[n] == '/');
}
int k_path_resolve(const char *path, const char *cwd, char out[K_PATH_MAX])
{
    if (!path || !cwd || !out || !*path)
        return K_EINVAL;
    UINTN len = 1, floor = 1;
    BOOLEAN named = FALSE;
    out[0] = '/';
    out[1] = 0;
    if (*path == '@') {
        named = TRUE;
        ++path;
        char name[32];
        UINT32 n = 0;
        while (*path && *path != '/') {
            if (n == 31)
                return K_E2BIG;
            name[n++] = *path++;
        }
        name[n] = 0;
        BOOLEAN found = FALSE;
        for (UINT32 i = 0; i < root_count; ++i)
            if (str_eq(name, roots[i].name)) {
                str_copy(out, roots[i].path, K_PATH_MAX);
                found = TRUE;
                break;
            }
        if (!found)
            return K_ENOENT;
        len = str_len(out);
        floor = len;
    } else if (*path != '/') {
        if (*cwd != '/' || str_len(cwd) >= K_PATH_MAX)
            return K_EINVAL;
        str_copy(out, cwd, K_PATH_MAX);
        len = str_len(out);
    }
    UINT32 scanned = 0;
    while (*path) {
        if (++scanned > 4096)
            return K_E2BIG;
        if (*path == '/') {
            ++path;
            continue;
        }
        const char *start = path;
        UINTN n = 0;
        while (*path && *path != '/') {
            if ((UINT8)*path < 32 || (UINT8)*path > 126 || ++n > K_NAME_MAX)
                return K_EINVAL;
            ++path;
        }
        if (n == 1 && start[0] == '.')
            continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            if (len <= floor) {
                if (named)
                    return K_EPERM;
                continue;
            }
            UINTN next = len;
            while (next > 1 && out[next - 1] != '/')
                --next;
            if (next > 1)
                --next;
            if (next < floor)
                return K_EPERM;
            len = next;
            out[len] = 0;
            continue;
        }
        UINTN slash = len > 1 ? 1 : 0;
        if (n + slash >= K_PATH_MAX - len)
            return K_E2BIG;
        if (slash)
            out[len++] = '/';
        mem_copy(out + len, start, n);
        len += n;
        out[len] = 0;
    }
    return 0;
}
int k_root_bind(const char *name, const char *path)
{
    if (!name || !path || !*name || str_len(name) > 31 || *path != '/')
        return K_EINVAL;
    for (const char *p = name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-'))
            return K_EINVAL;
    char normalized[K_PATH_MAX];
    int e = k_path_resolve(path, "/", normalized);
    if (e)
        return e;
    for (UINT32 i = 0; i < root_count; ++i)
        if (str_eq(name, roots[i].name))
            return str_eq(normalized, roots[i].path) ? 0 : K_EEXIST;
    if (root_count == MAX_ROOTS)
        return K_E2BIG;
    str_copy(roots[root_count].name, name, 32);
    str_copy(roots[root_count].path, normalized, K_PATH_MAX);
    ++root_count;
    return 0;
}
void k_roots_list(void (*cb)(const char *, const char *, void *), void *arg)
{
    if (cb)
        for (UINT32 i = 0; i < root_count; ++i)
            cb(roots[i].name, roots[i].path, arg);
}
void k_mounts_list(void (*cb)(const char *, const char *, void *), void *arg)
{
    if (cb) {
        cb("/", "RAM namespace (volatile)", arg);
        for (UINT32 i = 0; i < mount_count; ++i) {
            fat_volume *v = &volumes[mounts[i].volume];
            char description[128];
            str_copy(description, v->source, sizeof(description));
            const char *mode = v->fat_bits == 12 ? " [FAT12 read-only]"
                               : v->partition_ro ? " [partition read-only]"
                               : v->write_failed ? " [write failed; read-only]"
                               : (disks[v->disk].kind != DISK_RAM && !disks[v->disk].flush_command)
                                   ? " [no flush support; read-only]"
                               : v->write_ready ? " [FAT writable]"
                                                : " [FAT; validated on first write]";
            UINTN n = str_len(description);
            str_copy(description + n, mode, sizeof(description) - n);
            cb(mounts[i].path, description, arg);
        }
    }
}
static int ram_child(UINT32 parent, const char *name)
{
    for (UINT32 i = 1; i < MAX_NODES; ++i)
        if (nodes[i].used && nodes[i].parent == parent && str_eq(nodes[i].name, name))
            return (int)i;
    return K_ENOENT;
}
static int ram_walk(const char *path)
{
    UINT32 at = 0;
    while (*path) {
        if (*path == '/') {
            ++path;
            continue;
        }
        if (!nodes[at].directory)
            return K_ENOENT;
        char name[K_NAME_MAX + 1];
        UINT32 n = 0;
        while (*path && *path != '/')
            name[n++] = *path++;
        name[n] = 0;
        int next = ram_child(at, name);
        if (next < 0)
            return next;
        at = (UINT32)next;
    }
    return (int)at;
}
static int ram_new(const char *path, BOOLEAN directory)
{
    char parent[K_PATH_MAX];
    str_copy(parent, path, sizeof(parent));
    UINTN n = str_len(parent);
    if (n <= 1)
        return K_EEXIST;
    UINTN split = n;
    while (split && parent[split - 1] != '/')
        --split;
    char name[K_NAME_MAX + 1];
    str_copy(name, parent + split, sizeof(name));
    parent[split > 1 ? split - 1 : 1] = 0;
    int p = ram_walk(parent);
    if (p < 0 || !nodes[p].directory)
        return K_ENOENT;
    if (ram_child((UINT32)p, name) >= 0)
        return K_EEXIST;
    for (UINT32 i = 1; i < MAX_NODES; ++i)
        if (!nodes[i].used) {
            ram_node *r = &nodes[i];
            mem_zero(r, sizeof(*r));
            r->used = TRUE;
            r->parent = (UINT32)p;
            r->directory = directory;
            r->kind = 1;
            str_copy(r->name, name, sizeof(r->name));
            return (int)i;
        }
    return K_ENOMEM;
}
static int choose_mount(const char *path)
{
    int best = -1;
    UINTN longest = 0;
    for (UINT32 i = 0; i < mount_count; ++i)
        if (path_prefix(path, mounts[i].path)) {
            UINTN n = str_len(mounts[i].path);
            if (n > longest) {
                best = (int)i;
                longest = n;
            }
        }
    return best;
}
int k_vfs_init(void)
{
    if (vfs_ready)
        return K_EBUSY;
    nodes[0].used = nodes[0].directory = TRUE;
    nodes[0].kind = 1;
    vfs_ready = TRUE;
    return 0;
}
int k_vfs_mkdir(const char *path)
{
    if (!vfs_ready || k_cpu_id() != 0)
        return K_EINVAL;
    char canonical_path[K_PATH_MAX];
    int e = k_path_resolve(path, "/", canonical_path);
    if (e)
        return e;
    e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    int mount = choose_mount(canonical_path);
    if (mount >= 0)
        e = fat_create_locked(mounts[mount].volume, canonical_path + str_len(mounts[mount].path),
                              TRUE, NULL);
    else {
        e = ram_new(canonical_path, TRUE);
        if (e >= 0)
            e = 0;
    }
    k_mutex_unlock(&vfs_mutex);
    return e;
}
int k_vfs_put(const char *path, const void *bytes, UINT64 size)
{
    if (!vfs_ready || (!bytes && size) || size > 4 * 1024 * 1024 || k_cpu_id() != 0)
        return K_EINVAL;
    char canonical_path[K_PATH_MAX];
    int e = k_path_resolve(path, "/", canonical_path);
    if (e)
        return e;
    e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    int mount = choose_mount(canonical_path);
    if (mount >= 0) {
        e = fat_store_locked(mounts[mount].volume, canonical_path + str_len(mounts[mount].path),
                             bytes, size);
        goto end;
    }
    int index = ram_walk(canonical_path);
    if (index >= 0 && (nodes[index].directory || nodes[index].kind != 1)) {
        e = K_EINVAL;
        goto end;
    }
    UINT32 pages = (UINT32)((size + 4095) / 4096);
    UINT8 *data = pages ? (void *)(UINTN)k_pmm_alloc_pages(pages, 0) : NULL;
    if (pages && !data) {
        e = K_ENOMEM;
        goto end;
    }
    if (index < 0)
        index = ram_new(canonical_path, FALSE);
    if (index < 0) {
        if (data)
            k_pmm_free_pages((UINT64)(UINTN)data, pages);
        e = index;
        goto end;
    }
    if (size)
        mem_copy(data, bytes, size);
    ram_node *r = &nodes[index];
    if (r->data)
        k_pmm_free_pages((UINT64)(UINTN)r->data, r->pages);
    r->data = data;
    r->size = size;
    r->pages = pages;
    e = 0;
end:
    k_mutex_unlock(&vfs_mutex);
    return e;
}
static int vfs_open_locked(const char *path, k_file *out)
{
    int m = choose_mount(path);
    if (m >= 0)
        return fat_open(mounts[m].volume, path + str_len(mounts[m].path), out);
    int n = ram_walk(path);
    if (n < 0)
        return n;
    ram_node *r = &nodes[n];
    *out = (k_file){
        r->kind, r->kind == 3 ? r->block_id : (UINT32)n, 0, r->directory ? 0x10 : 0, r->size, 0};
    return 0;
}
int k_vfs_open(const char *path, k_file *out)
{
    if (!vfs_ready || !out || k_cpu_id() != 0)
        return K_EINVAL;
    char resolved[K_PATH_MAX];
    int e = k_path_resolve(path, "/", resolved);
    if (e)
        return e;
    e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    e = vfs_open_locked(resolved, out);
    k_mutex_unlock(&vfs_mutex);
    return e;
}
static int disk_bytes(UINT32 id, UINT64 offset, void *out, UINT64 n)
{
    if (id >= disk_count || offset > disks[id].info.sectors * disks[id].info.sector_size ||
        n > disks[id].info.sectors * disks[id].info.sector_size - offset)
        return K_EINVAL;
    UINT8 sector[4096], *p = out;
    UINT32 size = disks[id].info.sector_size;
    while (n) {
        if (!(offset % size) && n >= size) {
            UINT32 sectors = (UINT32)MIN(n / size, 128);
            int e = k_disk_read(id, offset / size, sectors, p, n);
            if (e)
                return e;
            UINT64 bytes = (UINT64)sectors * size;
            p += bytes;
            offset += bytes;
            n -= bytes;
            continue;
        }
        UINT64 lba = offset / size;
        UINT32 at = (UINT32)(offset % size), part = (UINT32)MIN(n, size - at);
        int e = k_disk_read(id, lba, 1, sector, sizeof(sector));
        if (e)
            return e;
        mem_copy(p, sector + at, part);
        p += part;
        offset += part;
        n -= part;
    }
    return 0;
}
int k_vfs_read(const k_file *file, UINT64 offset, void *out, UINT64 n, UINT64 *got)
{
    if (!file || (!out && n) || !got || k_cpu_id() != 0)
        return K_EINVAL;
    *got = 0;
    if (file->attributes & 0x10)
        return K_EINVAL;
    int e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    if (file->kind == 1) {
        if (file->object >= MAX_NODES || !nodes[file->object].used) {
            e = K_ENOENT;
            goto end;
        }
        ram_node *r = &nodes[file->object];
        if (offset < r->size) {
            n = MIN(n, r->size - offset);
            mem_copy(out, r->data + offset, n);
            *got = n;
        }
    } else if (file->kind == 2)
        e = fat_read(file, offset, out, n, got);
    else if (file->kind == 3) {
        if (file->object >= disk_count) {
            e = K_ENOENT;
            goto end;
        }
        UINT64 size = disks[file->object].info.sectors * disks[file->object].info.sector_size;
        if (offset < size) {
            n = MIN(n, size - offset);
            e = disk_bytes(file->object, offset, out, n);
            if (!e)
                *got = n;
        }
    } else
        e = K_ENOTSUP;
end:
    k_mutex_unlock(&vfs_mutex);
    return e;
}
int k_vfs_list(const char *path, k_dir_callback cb, void *arg)
{
    if (!path || !cb || k_cpu_id() != 0)
        return K_EINVAL;
    char resolved[K_PATH_MAX];
    int e = k_path_resolve(path, "/", resolved);
    if (e)
        return e;
    e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    k_file f;
    e = vfs_open_locked(resolved, &f);
    if (e)
        goto end;
    if (!(f.attributes & 0x10)) {
        e = K_EINVAL;
        goto end;
    }
    if (f.kind == 2)
        e = fat_list(&f, cb, arg);
    else if (f.kind == 1)
        for (UINT32 i = 1; i < MAX_NODES; ++i)
            if (nodes[i].used && nodes[i].parent == f.object) {
                k_dirent entry;
                str_copy(entry.name, nodes[i].name, sizeof(entry.name));
                entry.size = nodes[i].size;
                entry.directory = nodes[i].directory;
                cb(&entry, arg);
            }
end:
    k_mutex_unlock(&vfs_mutex);
    return e;
}

/* FAT12/16/32 read support. All arithmetic remains relative to a validated
 * partition and all cluster walks are bounded. No metadata is ever repaired. */
static int volume_bytes(fat_volume *v, UINT64 offset, void *out, UINT64 n)
{
    if (!span_ok(offset, n, v->length * v->sector))
        return K_EIO;
    return disk_bytes(v->disk, v->start * v->sector + offset, out, n);
}
static int fat_next(fat_volume *v, UINT32 cluster, UINT32 *next)
{
    if (cluster < 2 || cluster - 2 >= v->cluster_count)
        return K_EIO;
    UINT64 off =
        v->fat_bits == 12 ? (UINT64)cluster + cluster / 2 : (UINT64)cluster * (v->fat_bits / 8);
    UINT8 raw[4] = {0};
    UINT32 bytes = v->fat_bits == 32 ? 4 : 2;
    if (!span_ok(off, bytes, v->fat_sectors * v->sector))
        return K_EIO;
    int e = volume_bytes(v, v->fat_start * v->sector + off, raw, bytes);
    if (e)
        return e;
    UINT32 n = v->fat_bits == 32 ? rd32(raw) & 0x0fffffff : rd16(raw);
    if (v->fat_bits == 12)
        n = (cluster & 1) ? n >> 4 : n & 0xfff;
    UINT32 end = v->fat_bits == 12 ? 0xff8 : v->fat_bits == 16 ? 0xfff8 : 0x0ffffff8;
    if (n >= end) {
        *next = 0xffffffff;
        return 0;
    }
    if (n < 2 || n - 2 >= v->cluster_count || n == end - 1)
        return K_EIO;
    *next = n;
    return 0;
}
static BOOLEAN name_equal(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'a' && x <= 'z')
            x -= 32;
        if (y >= 'a' && y <= 'z')
            y -= 32;
        if (x != y)
            return FALSE;
    }
    return *a == *b;
}
static UINT8 sfn_checksum(const UINT8 *name)
{
    UINT8 sum = 0;
    for (int i = 0; i < 11; ++i)
        sum = (UINT8)(((sum & 1) ? 128 : 0) + (sum >> 1) + name[i]);
    return sum;
}
typedef int (*fat_callback)(const k_dirent *, UINT32, void *);
static int fat_directory(fat_volume *v, UINT32 cluster, fat_callback cb, void *arg)
{
    UINT8 sector[4096];
    UINT32 visited = 0, scanned = 0, tortoise = cluster;
    char longname[K_NAME_MAX + 1];
    mem_zero(longname, sizeof(longname));
    UINT32 expected = 0;
    UINT8 sum = 0;
    BOOLEAN lfn = FALSE;
    if (!cluster && v->fat_bits == 32)
        return K_EIO;
    for (;;) {
        UINT64 first;
        UINT32 sectors;
        if (!cluster) {
            first = v->root_start;
            sectors = (v->root_entries * 32 + v->sector - 1) / v->sector;
        } else {
            if (cluster < 2 || cluster - 2 >= v->cluster_count || ++visited > 65536 ||
                visited > v->cluster_count)
                return K_EIO;
            first = v->data_start + (UINT64)(cluster - 2) * v->spc;
            sectors = v->spc;
        }
        for (UINT32 s = 0; s < sectors; ++s) {
            int e = volume_bytes(v, (first + s) * v->sector, sector, v->sector);
            if (e)
                return e;
            for (UINT32 p = 0; p < v->sector; p += 32) {
                if (!cluster && scanned >= v->root_entries)
                    return 0;
                if (++scanned > 65536)
                    return K_E2BIG;
                UINT8 *d = sector + p;
                if (!d[0])
                    return 0;
                if (d[0] == 0xe5) {
                    lfn = FALSE;
                    continue;
                }
                if (d[11] == 0x0f) {
                    UINT32 seq = d[0] & 31;
                    if (d[0] & 0x40) {
                        mem_zero(longname, sizeof(longname));
                        lfn = seq >= 1 && seq <= 20;
                        expected = seq;
                        sum = d[13];
                    }
                    if (!lfn || seq != expected || d[13] != sum || d[12] || rd16(d + 26) ||
                        (d[0] & 0xa0)) {
                        lfn = FALSE;
                        continue;
                    }
                    static const UINT8 offsets[13] = {1,  3,  5,  7,  9,  14, 16,
                                                      18, 20, 22, 24, 28, 30};
                    for (UINT32 i = 0; i < 13; ++i) {
                        UINT32 at = (seq - 1) * 13 + i;
                        UINT16 ch = rd16(d + offsets[i]);
                        if (at >= K_NAME_MAX) {
                            if (ch && ch != 0xffff)
                                lfn = FALSE;
                            continue;
                        }
                        if (ch == 0xffff || ch == 0)
                            longname[at] = 0;
                        else if (ch < 32 || ch > 126 || ch == '/' || ch == '\\')
                            lfn = FALSE;
                        else
                            longname[at] = (char)ch;
                    }
                    --expected;
                    continue;
                }
                if (d[11] & 8) {
                    lfn = FALSE;
                    continue;
                }
                k_dirent entry;
                mem_zero(&entry, sizeof(entry));
                if (lfn && !expected && sum == sfn_checksum(d) && longname[0])
                    str_copy(entry.name, longname, sizeof(entry.name));
                else {
                    UINT32 n = 0;
                    for (UINT32 i = 0; i < 8 && d[i] != ' '; ++i)
                        entry.name[n++] = (char)d[i];
                    if (d[8] != ' ') {
                        entry.name[n++] = '.';
                        for (UINT32 i = 8; i < 11 && d[i] != ' '; ++i)
                            entry.name[n++] = (char)d[i];
                    }
                    entry.name[n] = 0;
                }
                lfn = FALSE;
                if (str_eq(entry.name, ".") || str_eq(entry.name, ".."))
                    continue;
                entry.directory = (d[11] & 0x10) != 0;
                entry.attributes = d[11];
                entry.directory_offset = (first + s) * v->sector + p;
                entry.size = rd32(d + 28);
                UINT32 child =
                    rd16(d + 26) | (v->fat_bits == 32 ? ((UINT32)rd16(d + 20) << 16) : 0);
                if (v->fat_bits == 32 && (child & 0xf0000000))
                    return K_EIO;
                if ((entry.directory || entry.size) && (child < 2 || child - 2 >= v->cluster_count))
                    return K_EIO;
                int result = cb(&entry, child, arg);
                if (result)
                    return result > 0 ? 0 : result;
            }
        }
        if (!cluster)
            return 0;
        UINT32 next;
        int e = fat_next(v, cluster, &next);
        if (e)
            return e;
        if (next == 0xffffffff)
            return 0;
        if ((visited & 1) == 0) {
            e = fat_next(v, tortoise, &tortoise);
            if (e)
                return e;
            if (tortoise == 0xffffffff)
                return K_EIO;
        }
        if (next == tortoise)
            return K_EIO;
        cluster = next;
    }
}
typedef struct {
    const char *name;
    BOOLEAN found;
    k_dirent entry;
    UINT32 cluster;
} fat_find;
static int fat_find_cb(const k_dirent *entry, UINT32 cluster, void *arg)
{
    fat_find *f = arg;
    if (!name_equal(f->name, entry->name))
        return 0;
    f->found = TRUE;
    f->entry = *entry;
    f->cluster = cluster;
    return 1;
}
static int fat_open(UINT32 index, const char *path, k_file *out)
{
    if (index >= volume_count)
        return K_ENOENT;
    fat_volume *v = &volumes[index];
    UINT32 cluster = v->fat_bits == 32 ? v->root_cluster : 0;
    *out = (k_file){2, index, cluster, 0x10, 0, 0};
    while (*path) {
        if (*path == '/') {
            ++path;
            continue;
        }
        if (!(out->attributes & 0x10))
            return K_ENOENT;
        char name[K_NAME_MAX + 1];
        UINT32 n = 0;
        while (*path && *path != '/')
            name[n++] = *path++;
        name[n] = 0;
        fat_find found = {.name = name};
        int e = fat_directory(v, cluster, fat_find_cb, &found);
        if (e)
            return e;
        if (!found.found)
            return K_ENOENT;
        cluster = found.cluster;
        *out = (k_file){2,
                        index,
                        cluster,
                        found.entry.attributes,
                        found.entry.size,
                        found.entry.directory_offset};
    }
    return 0;
}
static int fat_read(const k_file *file, UINT64 offset, void *out, UINT64 bytes, UINT64 *got)
{
    k_file refreshed = *file;
    int refresh = fat_refresh(&refreshed);
    if (refresh)
        return refresh;
    file = &refreshed;
    if (file->object >= volume_count)
        return K_ENOENT;
    fat_volume *v = &volumes[file->object];
    if (offset >= file->size || !bytes)
        return 0;
    bytes = MIN(bytes, file->size - offset);
    UINT64 cluster_bytes = (UINT64)v->spc * v->sector, skip = offset / cluster_bytes,
           at = offset % cluster_bytes;
    if (skip > 65536)
        return K_E2BIG;
    UINT32 cluster = file->cluster, steps = 0, tortoise = cluster;
    UINT8 *p = out;
    while (skip || bytes) {
        if (cluster < 2 || cluster - 2 >= v->cluster_count || steps >= v->cluster_count ||
            steps > 65536)
            return K_EIO;
        if (!skip) {
            UINT64 n = MIN(bytes, cluster_bytes - at);
            UINT64 pos = (v->data_start + (UINT64)(cluster - 2) * v->spc) * v->sector + at;
            int e = volume_bytes(v, pos, p, n);
            if (e)
                return e;
            *got += n;
            p += n;
            bytes -= n;
            at = 0;
            if (!bytes)
                return 0;
        } else
            --skip;
        UINT32 next;
        int e = fat_next(v, cluster, &next);
        if (e || next == 0xffffffff)
            return e ? e : K_EIO;
        ++steps;
        if ((steps & 1) == 0) {
            e = fat_next(v, tortoise, &tortoise);
            if (e)
                return e;
        }
        if (next == tortoise)
            return K_EIO;
        cluster = next;
    }
    return 0;
}
typedef struct {
    k_dir_callback cb;
    void *arg;
} fat_listing;
static int fat_list_cb(const k_dirent *entry, UINT32 cluster, void *ctx)
{
    (void)cluster;
    fat_listing *l = ctx;
    l->cb(entry, l->arg);
    return 0;
}
static int fat_list(const k_file *file, k_dir_callback cb, void *arg)
{
    if (file->object >= volume_count)
        return K_ENOENT;
    fat_listing l = {cb, arg};
    return fat_directory(&volumes[file->object], file->cluster, fat_list_cb, &l);
}
/* Writable FAT16/32. First mutation validates every referenced cluster and
 * mirrored FAT before enabling writes. Allocation tables stay cached under
 * vfs_mutex; there must be no other OS writing this mounted volume.
 *
 * Replacement ordering: dirty flag -> new data -> new FAT chain -> directory
 * entry -> release old chain -> clean flag, with device flushes between stages.
 * This is copy-on-write data replacement, NOT a power-fail-safe journal:
 * torn metadata sectors or a device that lies about flush can still damage FAT.
 * Any error during mutation freezes the volume read-only for this boot.
 */
#define FAT_WRITE_MAX (4ULL * 1024 * 1024)
#define FAT_CACHE_MAX (64ULL * 1024 * 1024)
#define FAT_DIR_LIMIT 4096
static UINT32 fat_cached(fat_volume *v, UINT32 c)
{
    return v->fat_bits == 16 ? rd16(v->cache + 2ULL * c) : rd32(v->cache + 4ULL * c) & 0x0fffffff;
}
static UINT32 fat_eoc(fat_volume *v)
{
    return v->fat_bits == 16 ? 0xffff : 0x0fffffff;
}
static BOOLEAN fat_is_end(fat_volume *v, UINT32 n)
{
    return n >= (v->fat_bits == 16 ? 0xfff8U : 0x0ffffff8U);
}
static void fat_cache_set(fat_volume *v, UINT32 c, UINT32 value)
{
    UINT64 off = (UINT64)c * (v->fat_bits / 8);
    if (v->fat_bits == 16) {
        v->cache[off] = (UINT8)value;
        v->cache[off + 1] = (UINT8)(value >> 8);
    } else
        wr32(v->cache + off, (rd32(v->cache + off) & 0xf0000000) | (value & 0x0fffffff));
    bit_set(v->dirty, off / v->sector);
}
static int volume_write(fat_volume *v, UINT64 sector, UINT32 count, const void *data)
{
    if (v->write_failed || sector >= v->length || !count || count > v->length - sector)
        return K_EROFS;
    int e = disk_write_fs(v->disk, v->start + sector, count, data, (UINT64)count * v->sector);
    if (e)
        v->write_failed = TRUE;
    return e;
}
static int volume_flush(fat_volume *v)
{
    int e = disk_flush(v->disk);
    if (e)
        v->write_failed = TRUE;
    return e;
}
static int fat_flush_cache(fat_volume *v)
{
    for (UINT64 s = 0; s < v->fat_sectors; ++s)
        if (bit_get(v->dirty, s)) {
            for (UINT32 f = 0; f < (v->mirrored ? v->fat_count : 1); ++f) {
                UINT64 first = v->mirrored ? v->first_fat + f * v->fat_sectors : v->fat_start;
                int e = volume_write(v, first + s, 1, v->cache + s * v->sector);
                if (e)
                    return e;
            }
            bit_clear(v->dirty, s);
        }
    return volume_flush(v);
}
static int fat_mark_chain(fat_volume *v, UINT32 c, UINT64 size)
{
    if (!c)
        return size ? K_EIO : 0;
    UINT32 count = 0;
    for (;;) {
        if (c < 2 || c - 2 >= v->cluster_count || ++count > 65536 || bit_get(v->used, c))
            return K_EIO;
        bit_set(v->used, c);
        UINT32 next = fat_cached(v, c);
        if (fat_is_end(v, next))
            break;
        c = next;
    }
    return size > (UINT64)count * v->spc * v->sector ? K_EIO : 0;
}
typedef struct {
    fat_volume *v;
    UINT32 *dirs, count;
} fat_audit;
static int fat_audit_entry(const k_dirent *entry, UINT32 c, void *arg)
{
    fat_audit *a = arg;
    int e = fat_mark_chain(a->v, c, entry->size);
    if (e)
        return e;
    if (entry->directory) {
        if (a->count == FAT_DIR_LIMIT)
            return K_E2BIG;
        a->dirs[a->count++] = c;
    }
    return 0;
}
static void fat_drop_cache(fat_volume *v)
{
    if (v->cache)
        k_pmm_free_pages((UINT64)(UINTN)v->cache, v->cache_pages);
    if (v->used)
        k_pmm_free_pages((UINT64)(UINTN)v->used, v->used_pages);
    if (v->dirty)
        k_pmm_free_pages((UINT64)(UINTN)v->dirty, v->dirty_pages);
    v->cache = v->used = v->dirty = NULL;
    v->write_ready = FALSE;
}
static int fat_prepare_write(fat_volume *v)
{
    if (v->write_failed || v->partition_ro)
        return K_EROFS;
    if (v->write_ready)
        return 0;
    if (v->fat_bits == 16 && v->cluster_count > 0xffee)
        return K_ENOTSUP;
    if (v->fat_bits == 12 || (disks[v->disk].kind != DISK_RAM && !disks[v->disk].flush_command))
        return K_EROFS;
    UINT64 bytes = v->fat_sectors * v->sector;
    if (bytes > FAT_CACHE_MAX)
        return K_E2BIG;
    v->cache_pages = (UINT32)((bytes + 4095) / 4096);
    v->used_pages = (v->cluster_count + 2 + 32767) / 32768;
    v->dirty_pages = (UINT32)((v->fat_sectors + 32767) / 32768);
    v->cache = (void *)(UINTN)k_pmm_alloc_pages(v->cache_pages, 0);
    v->used = (void *)(UINTN)k_pmm_alloc_pages(v->used_pages, 0);
    v->dirty = (void *)(UINTN)k_pmm_alloc_pages(v->dirty_pages, 0);
    UINT32 *dirs = (void *)(UINTN)k_pmm_alloc_pages(FAT_DIR_LIMIT / 1024, 0);
    int e = K_ENOMEM;
    if (!v->cache || !v->used || !v->dirty || !dirs)
        goto fail;
    e = volume_bytes(v, v->fat_start * v->sector, v->cache, bytes);
    if (e)
        goto fail;
    UINT32 header = (v->fat_bits == 16 ? 0xff00U : 0x0fffff00U) | v->media;
    if (fat_cached(v, 0) != header) {
        e = K_EIO;
        goto fail;
    }
    UINT32 mask = v->fat_bits == 16 ? 0xc000 : 0x0c000000;
    if ((fat_cached(v, 1) & mask) != mask) {
        e = K_EROFS;
        goto fail;
    } /* dirty/error flag */
    if (v->mirrored && v->fat_count > 1) {
        UINT8 sector[4096];
        for (UINT64 s = 0; s < v->fat_sectors; ++s) {
            e = volume_bytes(v, (v->first_fat + v->fat_sectors + s) * v->sector, sector, v->sector);
            if (e)
                goto fail;
            if (memcmp(v->cache + s * v->sector, sector, v->sector)) {
                e = K_EIO;
                goto fail;
            }
        }
    }
    fat_audit a = {v, dirs, 1};
    dirs[0] = v->root_cluster;
    if (v->fat_bits == 32) {
        e = fat_mark_chain(v, v->root_cluster, 0);
        if (e)
            goto fail;
    }
    for (UINT32 i = 0; i < a.count; ++i) {
        e = fat_directory(v, dirs[i], fat_audit_entry, &a);
        if (e)
            goto fail;
    }
    v->free_clusters = 0;
    for (UINT32 c = 2; c < v->cluster_count + 2; ++c) {
        UINT32 value = fat_cached(v, c);
        if (!value) {
            ++v->free_clusters;
            continue;
        }
        if (!bit_get(v->used, c) && value != (v->fat_bits == 16 ? 0xfff7U : 0x0ffffff7U)) {
            e = K_EIO;
            goto fail; /* orphan/reserved/cross-linked filesystem needs fsck */
        }
    }
    v->alloc_hint = 2;
    v->write_ready = TRUE;
    k_pmm_free_pages((UINT64)(UINTN)dirs, FAT_DIR_LIMIT / 1024);
    return 0;
fail:
    if (dirs)
        k_pmm_free_pages((UINT64)(UINTN)dirs, FAT_DIR_LIMIT / 1024);
    fat_drop_cache(v);
    return e;
}
static int fat_begin_write(fat_volume *v)
{
    fat_cache_set(v, 1, fat_cached(v, 1) & ~(v->fat_bits == 16 ? 0x8000U : 0x08000000U));
    int e = fat_flush_cache(v);
    if (e)
        return e;
    /* FSInfo is only a hint. Invalidate valid primary/backup hints instead of
     * trusting or incrementally maintaining potentially stale free counts. */
    if (v->fat_bits == 32 && v->fsinfo && v->fsinfo < v->first_fat) {
        UINT8 sector[4096];
        UINT32 offsets[2] = {v->fsinfo, v->backup_boot + v->fsinfo};
        for (UINT32 i = 0; i < (v->backup_boot ? 2U : 1U); ++i) {
            if (offsets[i] >= v->first_fat)
                continue;
            e = volume_bytes(v, (UINT64)offsets[i] * v->sector, sector, v->sector);
            if (e)
                return e;
            if (rd32(sector) != 0x41615252 || rd32(sector + 484) != 0x61417272 ||
                rd32(sector + 508) != 0xaa550000)
                continue;
            wr32(sector + 488, 0xffffffff);
            wr32(sector + 492, 0xffffffff);
            e = volume_write(v, offsets[i], 1, sector);
            if (e)
                return e;
        }
    }
    return volume_flush(v);
}
static int fat_finish_write(fat_volume *v, int e)
{
    if (e) {
        v->write_failed = TRUE;
        return e;
    }
    fat_cache_set(v, 1, fat_cached(v, 1) | (v->fat_bits == 16 ? 0x8000U : 0x08000000U));
    e = fat_flush_cache(v);
    if (e)
        v->write_failed = TRUE;
    return e;
}
static UINT32 fat_allocate(fat_volume *v)
{
    UINT32 count = v->cluster_count;
    for (UINT32 i = 0; i < count; ++i) {
        UINT32 c = v->alloc_hint++;
        if (v->alloc_hint == count + 2)
            v->alloc_hint = 2;
        if (!fat_cached(v, c)) {
            fat_cache_set(v, c, fat_eoc(v));
            bit_set(v->used, c);
            --v->free_clusters;
            return c;
        }
    }
    return 0;
}
static int fat_write_data(fat_volume *v, UINT32 c, const UINT8 *data, UINT64 size)
{
    UINT8 sector[4096];
    UINT64 first = v->data_start + (UINT64)(c - 2) * v->spc;
    for (UINT32 s = 0; s < v->spc; ++s) {
        UINT32 n = (UINT32)MIN(size, v->sector);
        mem_zero(sector, v->sector);
        if (n) {
            mem_copy(sector, data, n);
            data += n;
            size -= n;
        }
        int e = volume_write(v, first + s, 1, sector);
        if (e)
            return e;
    }
    return 0;
}
static int fat_entry_write(fat_volume *v, UINT64 off, const UINT8 data[32])
{
    UINT8 sector[4096];
    if ((off & 31) || !span_ok(off, 32, v->length * v->sector))
        return K_EINVAL;
    UINT64 lba = off / v->sector;
    UINT32 at = off % v->sector;
    /* Directory entries must be in the fixed root or data area, never BPB/FAT. */
    if (lba < v->root_start || (v->fat_bits == 32 && lba < v->data_start))
        return K_EPERM;
    int e = volume_bytes(v, lba * v->sector, sector, v->sector);
    if (e)
        return e;
    mem_copy(sector + at, data, 32);
    return volume_write(v, lba, 1, sector);
}
static int fat_refresh(k_file *f)
{
    if (f->kind != 2 || f->object >= volume_count)
        return K_EINVAL;
    if (!f->directory_offset)
        return 0; /* volume root */
    fat_volume *v = &volumes[f->object];
    UINT8 d[32];
    int e = volume_bytes(v, f->directory_offset, d, 32);
    if (e)
        return e;
    if (!d[0] || d[0] == 0xe5 || (d[11] & 8))
        return K_ENOENT;
    f->attributes = d[11];
    f->size = rd32(d + 28);
    f->cluster = rd16(d + 26) | (v->fat_bits == 32 ? ((UINT32)rd16(d + 20) << 16) : 0);
    if ((f->size || (f->attributes & 0x10) || f->cluster) &&
        (f->cluster < 2 || f->cluster - 2 >= v->cluster_count))
        return K_EIO;
    return 0;
}
static void fat_set_first(UINT8 d[32], UINT32 cluster, UINT64 size)
{
    d[26] = (UINT8)cluster;
    d[27] = (UINT8)(cluster >> 8);
    d[20] = (UINT8)(cluster >> 16);
    d[21] = (UINT8)(cluster >> 24);
    wr32(d + 28, (UINT32)size);
}
static int fat_replace_locked(k_file *f, const void *data, UINT64 size)
{
    if (size > FAT_WRITE_MAX)
        return K_E2BIG;
    int e = fat_refresh(f);
    if (e)
        return e;
    if (!f->directory_offset || (f->attributes & 0x11))
        return K_EROFS;
    fat_volume *v = &volumes[f->object];
    e = fat_prepare_write(v);
    if (e)
        return e;
    UINT64 per = (UINT64)v->spc * v->sector;
    UINT32 count = (UINT32)((size + per - 1) / per);
    if (count > v->free_clusters)
        return K_ENOSPC;
    UINT8 entry[32];
    e = volume_bytes(v, f->directory_offset, entry, 32);
    if (e)
        return e;
    e = fat_begin_write(v);
    if (e)
        return fat_finish_write(v, e);
    UINT32 first = 0, last = 0;
    UINT64 offset = 0;
    for (UINT32 i = 0; i < count; ++i) {
        UINT32 c = fat_allocate(v);
        if (!c) {
            e = K_ENOMEM;
            break;
        }
        if (last)
            fat_cache_set(v, last, c);
        else
            first = c;
        last = c;
        e = fat_write_data(v, c, (const UINT8 *)data + offset, MIN(per, size - offset));
        if (e)
            break;
        offset += MIN(per, size - offset);
    }
    if (!e)
        e = volume_flush(v);
    if (!e)
        e = fat_flush_cache(v);
    if (!e) {
        fat_set_first(entry, first, size);
        entry[11] |= 0x20;
        e = fat_entry_write(v, f->directory_offset, entry);
    }
    if (!e)
        e = volume_flush(v);
    if (!e) {
        UINT32 old = f->cluster;
        while (old) {
            UINT32 next = fat_cached(v, old);
            fat_cache_set(v, old, 0);
            bit_clear(v->used, old);
            ++v->free_clusters;
            if (fat_is_end(v, next))
                break;
            old = next;
        }
        f->cluster = first;
        f->size = size;
        e = fat_flush_cache(v);
    }
    return fat_finish_write(v, e);
}
/* Locate up to 21 contiguous deleted/end entries plus a replacement end
 * marker. If needed, grow a cluster-backed directory using zeroed clusters. */
static int fat_slots(fat_volume *v, UINT32 dir, UINT32 need, UINT64 slots[22], BOOLEAN *end,
                     BOOLEAN grow)
{
    UINT32 cluster = dir, run = 0, scanned = 0;
    BOOLEAN ended = FALSE;
    UINT8 sector[4096];
    for (UINT32 visited = 0; visited < 65536; ++visited) {
        UINT64 first = cluster ? v->data_start + (UINT64)(cluster - 2) * v->spc : v->root_start;
        UINT32 sectors = cluster ? v->spc : (v->root_entries * 32 + v->sector - 1) / v->sector;
        for (UINT32 s = 0; s < sectors; ++s) {
            int e = volume_bytes(v, (first + s) * v->sector, sector, v->sector);
            if (e)
                return e;
            for (UINT32 p = 0; p < v->sector; p += 32) {
                if ((!cluster && scanned >= v->root_entries) || ++scanned > 65536)
                    return K_E2BIG;
                if (!sector[p])
                    ended = TRUE;
                if (ended || sector[p] == 0xe5) {
                    slots[run++] = (first + s) * v->sector + p;
                    if (run >= need + (ended ? 1U : 0U)) {
                        *end = ended;
                        return 0;
                    }
                } else
                    run = 0;
            }
        }
        if (!cluster)
            return K_ENOSPC;
        UINT32 next = fat_cached(v, cluster);
        if (fat_is_end(v, next)) {
            if (!grow)
                return K_EAGAIN;
            next = fat_allocate(v);
            if (!next)
                return K_ENOSPC;
            int e = fat_write_data(v, next, NULL, 0);
            if (!e)
                e = volume_flush(v);
            if (!e)
                e = fat_flush_cache(v); /* new cluster durable before linking */
            if (e)
                return e;
            fat_cache_set(v, cluster, next);
            e = fat_flush_cache(v);
            if (e)
                return e;
        }
        if (next < 2 || next - 2 >= v->cluster_count)
            return K_EIO;
        cluster = next;
    }
    return K_E2BIG;
}
typedef struct {
    const UINT8 *name;
    BOOLEAN found;
} sfn_find;
static int fat_sfn_exists(fat_volume *v, UINT32 dir, const UINT8 name[11])
{
    UINT8 sector[4096];
    UINT32 c = dir;
    for (UINT32 visited = 0; visited < 65536; ++visited) {
        UINT64 first = c ? v->data_start + (UINT64)(c - 2) * v->spc : v->root_start;
        UINT32 sectors = c ? v->spc : (v->root_entries * 32 + v->sector - 1) / v->sector;
        for (UINT32 s = 0; s < sectors; ++s) {
            int e = volume_bytes(v, (first + s) * v->sector, sector, v->sector);
            if (e)
                return e;
            for (UINT32 p = 0; p < v->sector; p += 32) {
                if (!c && ((UINT64)s * v->sector + p) / 32 >= v->root_entries)
                    return 0;
                if (!sector[p])
                    return 0;
                if (sector[p] != 0xe5 && sector[p + 11] != 0x0f && !memcmp(sector + p, name, 11))
                    return 1;
            }
        }
        if (!c)
            return 0;
        UINT32 next = fat_cached(v, c);
        if (fat_is_end(v, next))
            return 0;
        c = next;
    }
    return K_E2BIG;
}
static int fat_create_locked(UINT32 index, const char *path, BOOLEAN directory, k_file *out)
{
    char parent[K_PATH_MAX], name[K_NAME_MAX + 1];
    str_copy(parent, *path ? path : "/", sizeof(parent));
    UINTN n = str_len(parent);
    while (n > 1 && parent[n - 1] == '/')
        parent[--n] = 0;
    UINTN split = n;
    while (split && parent[split - 1] != '/')
        --split;
    str_copy(name, parent + split, sizeof(name));
    if (!name[0] || str_eq(name, ".") || str_eq(name, ".."))
        return K_EINVAL;
    for (UINTN i = 0; name[i]; ++i) {
        UINT8 ch = (UINT8)name[i];
        if (ch < 32 || ch > 126 || ch == '"' || ch == '*' || ch == ':' || ch == '<' || ch == '>' ||
            ch == '?' || ch == '\\' || ch == '|')
            return K_EINVAL;
    }
    n = str_len(name);
    if (name[n - 1] == ' ' || name[n - 1] == '.')
        return K_EINVAL;
    parent[split ? split - 1 : 0] = 0;
    k_file p, existing;
    int e = fat_open(index, parent, &p);
    if (e)
        return e;
    if (!(p.attributes & 0x10))
        return K_ENOENT;
    e = fat_open(index, path, &existing);
    if (!e)
        return K_EEXIST;
    if (e != K_ENOENT)
        return e;
    fat_volume *v = &volumes[index];
    e = fat_prepare_write(v);
    if (e)
        return e;
    if (v->free_clusters < 4)
        return K_ENOSPC; /* directory growth plus new subdirectory */
    UINT8 shortname[11];
    static const char hex[] = "0123456789ABCDEF";
    mem_copy(shortname, "K0000000FIL", 11);
    BOOLEAN available = FALSE;
    for (UINT32 serial = 1; serial < 65536; ++serial) {
        for (UINT32 i = 0; i < 7; ++i)
            shortname[7 - i] = (UINT8)hex[(serial >> (i * 4)) & 15];
        e = fat_sfn_exists(v, p.cluster, shortname);
        if (e < 0)
            return e;
        if (!e) {
            available = TRUE;
            break;
        }
    }
    if (!available)
        return K_E2BIG;
    UINT32 lfns = (UINT32)((n + 12) / 13), needed = lfns + 1;
    UINT64 slots[22];
    BOOLEAN end = FALSE;
    e = fat_slots(v, p.cluster, needed, slots, &end, FALSE);
    BOOLEAN grow = e == K_EAGAIN;
    if (e && !grow)
        return e;
    e = fat_begin_write(v);
    if (e)
        return fat_finish_write(v, e);
    if (grow)
        e = fat_slots(v, p.cluster, needed, slots, &end, TRUE);
    if (e)
        return fat_finish_write(v, e);
    UINT8 d[32] = {0};
    if (end) {
        e = fat_entry_write(v, slots[needed], d);
        if (!e)
            e = volume_flush(v);
    }
    /* Deleted placeholders ensure a partial LFN never exposes stale entries. */
    d[0] = 0xe5;
    for (UINT32 i = 0; !e && i < needed; ++i)
        e = fat_entry_write(v, slots[i], d);
    if (!e)
        e = volume_flush(v);
    UINT32 first = 0;
    if (!e && directory) {
        first = fat_allocate(v);
        if (!first)
            e = K_ENOMEM;
        else
            e = fat_write_data(v, first, NULL, 0);
        if (!e) {
            mem_zero(d, 32);
            mem_copy(d, ".          ", 11);
            d[11] = 0x10;
            fat_set_first(d, first, 0);
            UINT64 off = (v->data_start + (UINT64)(first - 2) * v->spc) * v->sector;
            e = fat_entry_write(v, off, d);
            mem_copy(d, "..         ", 11);
            UINT32 parent_cluster = p.cluster == v->root_cluster ? 0 : p.cluster;
            fat_set_first(d, parent_cluster, 0);
            if (!e)
                e = fat_entry_write(v, off + 32, d);
        }
    }
    if (!e)
        e = volume_flush(v);
    if (!e)
        e = fat_flush_cache(v);
    static const UINT8 positions[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (UINT32 i = 0; !e && i < lfns; ++i) {
        UINT32 seq = lfns - i;
        memset(d, 0xff, 32);
        d[0] = (UINT8)(seq | (i ? 0 : 0x40));
        d[11] = 0x0f;
        d[12] = 0;
        d[13] = sfn_checksum(shortname);
        d[26] = d[27] = 0;
        for (UINT32 j = 0; j < 13; ++j) {
            UINT32 at = (seq - 1) * 13 + j;
            UINT16 ch = at < n ? (UINT8)name[at] : at == n ? 0 : 0xffff;
            d[positions[j]] = (UINT8)ch;
            d[positions[j] + 1] = (UINT8)(ch >> 8);
        }
        e = fat_entry_write(v, slots[i], d);
    }
    if (!e)
        e = volume_flush(v);
    mem_zero(d, 32);
    mem_copy(d, shortname, 11);
    d[11] = directory ? 0x10 : 0x20;
    d[16] = d[18] = d[24] = 0x21; /* valid DOS date: 1980-01-01 */
    fat_set_first(d, first, 0);
    if (!e)
        e = fat_entry_write(v, slots[lfns], d);
    if (!e)
        e = volume_flush(v);
    e = fat_finish_write(v, e);
    if (!e && out)
        *out = (k_file){2, index, first, d[11], 0, slots[lfns]};
    return e;
}
static int fat_store_locked(UINT32 index, const char *path, const void *data, UINT64 size)
{
    k_file f;
    int e = fat_open(index, path, &f);
    if (e == K_ENOENT)
        e = fat_create_locked(index, path, FALSE, &f);
    if (e)
        return e;
    return fat_replace_locked(&f, data, size);
}

static int fat_mount(UINT32 disk_id, UINT64 start, UINT64 length, UINT32 partition)
{
    if (volume_count == MAX_VOLUMES || disk_id >= disk_count || !length ||
        start >= disks[disk_id].info.sectors || length > disks[disk_id].info.sectors - start)
        return K_EINVAL;
    UINT8 boot[4096];
    int e = k_disk_read(disk_id, start, 1, boot, sizeof(boot));
    if (e)
        return e;
    UINT32 bps = rd16(boot + 11), spc = boot[13], reserved = rd16(boot + 14), fats = boot[16],
           root_entries = rd16(boot + 17);
    UINT64 total = rd16(boot + 19);
    if (!total)
        total = rd32(boot + 32);
    UINT64 fat_size = rd16(boot + 22);
    if (!fat_size)
        fat_size = rd32(boot + 36);
    if (boot[510] != 0x55 || boot[511] != 0xaa || (boot[0] != 0xeb && boot[0] != 0xe9) ||
        bps != disks[disk_id].info.sector_size || !power2(spc) || spc > 128 || !reserved || !fats ||
        fats > 2 || !fat_size || !total || total > length)
        return K_ENOTSUP;
    UINT64 root_sectors = ((UINT64)root_entries * 32 + bps - 1) / bps;
    UINT64 data = reserved + (UINT64)fats * fat_size + root_sectors;
    if (data >= total)
        return K_ENOTSUP;
    UINT64 clusters = (total - data) / spc;
    if (!clusters || clusters > 0x0fffffee)
        return K_ENOTSUP;
    UINT32 bits = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    UINT64 needed = bits == 12 ? ((clusters + 2) * 3 + 1) / 2 : (clusters + 2) * (bits / 8);
    if (needed > fat_size * bps)
        return K_ENOTSUP;
    UINT32 root = 0, active = 0;
    if (bits == 32) {
        if (root_entries || rd16(boot + 22) || rd16(boot + 42))
            return K_ENOTSUP;
        root = rd32(boot + 44);
        if (root < 2 || root - 2 >= clusters)
            return K_ENOTSUP;
        UINT16 flags = rd16(boot + 40);
        if (flags & 0x80)
            active = flags & 15;
        if (active >= fats)
            return K_ENOTSUP;
    } else if (!root_entries || !rd16(boot + 22))
        return K_ENOTSUP;
    char source[24], number[12], path[K_PATH_MAX];
    decimal_name(source, "disk", disk_id);
    UINTN pos = str_len(source);
    source[pos++] = 'p';
    decimal_name(number, "", partition);
    str_copy(source + pos, number, sizeof(source) - pos);
    str_copy(path, "/volumes/", sizeof(path));
    str_copy(path + 9, source, sizeof(path) - 9);
    e = k_vfs_mkdir(path);
    if (e && e != K_EEXIST)
        return e;
    fat_volume *v = &volumes[volume_count];
    mem_zero(v, sizeof(*v));
    v->disk = disk_id;
    v->sector = bps;
    v->spc = spc;
    v->fat_bits = bits;
    v->root_entries = root_entries;
    v->root_cluster = root;
    v->cluster_count = (UINT32)clusters;
    v->start = start;
    v->length = total;
    v->fat_start = reserved + (UINT64)active * fat_size;
    v->fat_sectors = fat_size;
    v->root_start = reserved + (UINT64)fats * fat_size;
    v->data_start = data;
    v->first_fat = reserved;
    v->fat_count = fats;
    v->media = boot[21];
    v->mirrored = bits != 32 || !(rd16(boot + 40) & 0x80);
    if (bits == 32) {
        v->fsinfo = rd16(boot + 48);
        v->backup_boot = rd16(boot + 50);
    }
    str_copy(v->source, source, sizeof(v->source));
    str_copy(mounts[mount_count].path, path, K_PATH_MAX);
    mounts[mount_count++].volume = volume_count++;
    (void)k_root_bind(source, path);
    char alias[20];
    decimal_name(alias, "disk", disk_id);
    (void)k_root_bind(alias, path);
    return 0;
}

int k_vfs_create(const char *path, k_file *out)
{
    if (!vfs_ready || !out || k_cpu_id() != 0)
        return K_EINVAL;
    char resolved[K_PATH_MAX];
    int e = k_path_resolve(path, "/", resolved);
    if (e)
        return e;
    e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    k_file exists;
    e = vfs_open_locked(resolved, &exists);
    if (!e)
        e = K_EEXIST;
    else if (e == K_ENOENT) {
        int m = choose_mount(resolved);
        if (m >= 0)
            e = fat_create_locked(mounts[m].volume, resolved + str_len(mounts[m].path), FALSE, out);
        else {
            e = ram_new(resolved, FALSE);
            if (e >= 0)
                e = vfs_open_locked(resolved, out);
        }
    }
    k_mutex_unlock(&vfs_mutex);
    return e;
}
int k_vfs_write(k_file *file, UINT64 offset, const void *data, UINT64 n, UINT64 *written)
{
    if (!file || !written || (!data && n) || k_cpu_id() != 0)
        return K_EINVAL;
    *written = 0;
    if (file->kind == 3)
        return K_EROFS;
    if (n > FAT_WRITE_MAX)
        return K_E2BIG;
    int e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    UINT8 *image = NULL;
    UINT32 pages = 0;
    if (file->kind == 2) {
        e = fat_refresh(file);
        if (e)
            goto done;
    } else if (file->kind == 1 && file->object < MAX_NODES && nodes[file->object].used) {
        ram_node *r = &nodes[file->object];
        file->size = r->size;
        if (r->kind != 1 || r->directory) {
            e = K_EINVAL;
            goto done;
        }
    } else {
        e = K_EINVAL;
        goto done;
    }
    if (file->attributes & 0x11) {
        e = K_EROFS;
        goto done;
    }
    if (offset == ~0ULL)
        offset = file->size;
    if (file->size > FAT_WRITE_MAX || offset > FAT_WRITE_MAX || n > FAT_WRITE_MAX - offset) {
        e = K_E2BIG;
        goto done;
    }
    if (!n)
        goto done;
    UINT64 size = file->size > offset + n ? file->size : offset + n;
    pages = (UINT32)((size + 4095) / 4096);
    image = (void *)(UINTN)k_pmm_alloc_pages(pages, 0);
    if (!image) {
        e = K_ENOMEM;
        goto done;
    }
    if (file->kind == 2) {
        UINT64 got = 0;
        e = fat_read(file, 0, image, file->size, &got);
        if (!e && got != file->size)
            e = K_EIO;
        if (e)
            goto done;
    } else if (file->size)
        mem_copy(image, nodes[file->object].data, file->size);
    mem_copy(image + offset, data, n);
    if (file->kind == 2)
        e = fat_replace_locked(file, image, size);
    else {
        ram_node *r = &nodes[file->object];
        if (r->data)
            k_pmm_free_pages((UINT64)(UINTN)r->data, r->pages);
        r->data = image;
        r->pages = pages;
        r->size = size;
        file->size = size;
        image = NULL;
    }
    if (!e)
        *written = n;
done:
    if (image)
        k_pmm_free_pages((UINT64)(UINTN)image, pages);
    k_mutex_unlock(&vfs_mutex);
    return e;
}
int k_vfs_sync(void)
{
    if (k_cpu_id() != 0)
        return K_EINVAL;
    int e = k_mutex_lock(&vfs_mutex);
    if (e)
        return e;
    for (UINT32 i = 0; i < volume_count; ++i) {
        fat_volume *v = &volumes[i];
        int result = 0;
        if (v->write_failed)
            result = K_EIO;
        else if (v->write_ready)
            result = volume_flush(v);
        if (result && !e)
            e = result;
    }
    k_mutex_unlock(&vfs_mutex);
    return e;
}
int k_vfs_mount_ramfat(UINT32 id)
{
    if (k_cpu_id() != 0 || id >= disk_count || disks[id].kind != DISK_RAM)
        return K_EPERM;
    for (UINT32 i = 0; i < volume_count; ++i)
        if (volumes[i].disk == id)
            return K_EEXIST;
    return fat_mount(id, 0, disks[id].info.sectors, 0);
}

/* GPT and MBR/EBR are decoded read-only. GPT header AND entry-array CRCs
 * must match; corrupt GPT is never reinterpreted as an MBR FAT partition. */
static UINT32 crc_step(UINT32 crc, const UINT8 *p, UINTN n)
{
    while (n--) {
        crc ^= *p++;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0 - (crc & 1)));
    }
    return crc;
}
typedef struct {
    UINT64 start, length;
    UINT32 number;
    BOOLEAN read_only;
} partition_entry;
static int partition_add(partition_entry *parts, UINT32 *count, UINT64 start, UINT64 length,
                         UINT32 number, UINT64 disk_size)
{
    if (!length || !start || start >= disk_size || length > disk_size - start || *count >= 128)
        return K_EINVAL;
    for (UINT32 i = 0; i < *count; ++i)
        if (overlap(start, length, parts[i].start, parts[i].length))
            return K_EINVAL;
    parts[*count] = (partition_entry){start, length, number, FALSE};
    ++*count;
    return 0;
}
static int gpt_parse(UINT32 disk_id, UINT64 header_lba, partition_entry parts[128], UINT32 *count)
{
    UINT8 sector[4096];
    disk *d = &disks[disk_id];
    UINT32 bps = d->info.sector_size;
    int e = k_disk_read(disk_id, header_lba, 1, sector, sizeof(sector));
    if (e)
        return e;
    UINT32 len = rd32(sector + 12), saved = rd32(sector + 16);
    if (memcmp(sector, "EFI PART", 8) || rd32(sector + 8) != 0x10000 || len < 92 || len > bps ||
        rd32(sector + 20) || rd64(sector + 24) != header_lba)
        return K_EINVAL;
    wr32(sector + 16, 0);
    if ((crc_step(0xffffffff, sector, len) ^ 0xffffffff) != saved)
        return K_EINVAL;
    UINT64 backup = rd64(sector + 32), first = rd64(sector + 40), last = rd64(sector + 48),
           table = rd64(sector + 72);
    UINT32 entries = rd32(sector + 80), entry_size = rd32(sector + 84), wanted = rd32(sector + 88);
    UINT64 table_bytes = (UINT64)entries * entry_size,
           table_sectors = (table_bytes + bps - 1) / bps;
    if (backup >= d->info.sectors || backup == header_lba || first > last || first < 2 ||
        last >= d->info.sectors - 1 || !entries || entries > 4096 || entry_size < 128 ||
        entry_size > 1024 || (entry_size & 127) || !table || table >= d->info.sectors ||
        table_sectors > d->info.sectors - table ||
        overlap(table, table_sectors, first, last - first + 1) ||
        overlap(table, table_sectors, header_lba, 1) || overlap(table, table_sectors, backup, 1))
        return K_EINVAL;
    UINT32 crc = 0xffffffff;
    UINT64 remaining = table_bytes;
    for (UINT64 i = 0; i < table_sectors; ++i) {
        e = k_disk_read(disk_id, table + i, 1, sector, sizeof(sector));
        if (e)
            return e;
        UINT64 n = MIN(remaining, bps);
        crc = crc_step(crc, sector, n);
        remaining -= n;
    }
    if ((crc ^ 0xffffffff) != wanted)
        return K_EINVAL;
    *count = 0;
    for (UINT32 i = 0; i < entries; ++i) {
        UINT8 entry[128];
        e = disk_bytes(disk_id, table * bps + (UINT64)i * entry_size, entry, sizeof(entry));
        if (e)
            return e;
        BOOLEAN nonzero = FALSE;
        for (UINT32 j = 0; j < 16; ++j)
            if (entry[j])
                nonzero = TRUE;
        if (!nonzero)
            continue;
        UINT64 a = rd64(entry + 32), b = rd64(entry + 40);
        if (a < first || a > b || b > last)
            return K_EINVAL;
        e = partition_add(parts, count, a, b - a + 1, i + 1, d->info.sectors);
        if (e)
            return e;
        parts[*count - 1].read_only = (rd64(entry + 48) & (1ULL << 60)) != 0;
    }
    return 0;
}
static BOOLEAN extended_type(UINT8 t)
{
    return t == 5 || t == 0x0f || t == 0x85;
}
static int mbr_parse(UINT32 id, const UINT8 mbr[512], partition_entry parts[128], UINT32 *count)
{
    UINT64 disk_size = disks[id].info.sectors, ext_base = 0, ext_length = 0;
    *count = 0;
    for (UINT32 i = 0; i < 4; ++i) {
        const UINT8 *p = mbr + 446 + 16 * i;
        UINT8 type = p[4];
        UINT64 start = rd32(p + 8), length = rd32(p + 12);
        if (!type || !length)
            continue;
        if (type == 0xee)
            return K_EINVAL;
        if (!start || start >= disk_size || length > disk_size - start)
            return K_EINVAL;
        if (extended_type(type)) {
            if (ext_length)
                return K_ENOTSUP;
            ext_base = start;
            ext_length = length;
        } else {
            int e = partition_add(parts, count, start, length, i + 1, disk_size);
            if (e)
                return e;
        }
    }
    if (ext_length)
        for (UINT32 i = 0; i < *count; ++i)
            if (overlap(ext_base, ext_length, parts[i].start, parts[i].length))
                return K_EINVAL;
    UINT64 ebr = ext_base, seen[124];
    UINT32 visits = 0;
    while (ext_length) {
        if (ebr < ext_base || ebr - ext_base >= ext_length || visits == ARRAY_LEN(seen))
            return K_EINVAL;
        for (UINT32 i = 0; i < visits; ++i)
            if (seen[i] == ebr)
                return K_EINVAL;
        seen[visits++] = ebr;
        UINT8 sector[4096];
        int e = k_disk_read(id, ebr, 1, sector, sizeof(sector));
        if (e)
            return e;
        if (sector[510] != 0x55 || sector[511] != 0xaa)
            return K_EINVAL;
        const UINT8 *p = sector + 446, *link = sector + 462;
        UINT64 relative = rd32(p + 8), length = rd32(p + 12);
        if (p[4] && length) {
            if (extended_type(p[4]) || !relative || relative >= ext_base + ext_length - ebr ||
                length > ext_base + ext_length - ebr - relative)
                return K_EINVAL;
            e = partition_add(parts, count, ebr + relative, length, 4 + visits, disk_size);
            if (e)
                return e;
        }
        if (!link[4])
            break;
        if (!extended_type(link[4]))
            return K_EINVAL;
        UINT64 next = rd32(link + 8), len = rd32(link + 12);
        if (!next || next >= ext_length || !len || len > ext_length - next)
            return K_EINVAL;
        ebr = ext_base + next;
    }
    for (UINT32 i = 0; i < visits; ++i)
        for (UINT32 j = 0; j < *count; ++j)
            if (overlap(seen[i], 1, parts[j].start, parts[j].length))
                return K_EINVAL;
    return 0;
}
int k_vfs_mount_disks(void)
{
    if (!vfs_ready || k_cpu_id() != 0 || mount_count)
        return K_EBUSY;
    for (UINT32 id = 0; id < disk_count; ++id) {
        char path[K_PATH_MAX];
        str_copy(path, "/devices/", sizeof(path));
        str_copy(path + 9, disks[id].info.name, K_PATH_MAX - 9);
        int node = ram_new(path, FALSE);
        if (node >= 0) {
            nodes[node].kind = 3;
            nodes[node].block_id = id;
            nodes[node].size = disks[id].info.sectors * disks[id].info.sector_size;
        }
        if (disks[id].kind == DISK_RAM)
            continue;
        UINT8 sector[4096];
        int e = k_disk_read(id, 0, 1, sector, sizeof(sector));
        if (e) {
            con_print("partition read: ");
            con_print(k_strerror(e));
            con_print("\n");
            continue;
        }
        if (sector[510] != 0x55 || sector[511] != 0xaa)
            continue;
        BOOLEAN protective = FALSE, recovered = FALSE;
        for (UINT32 j = 0; j < 4; ++j)
            if (sector[446 + 16 * j + 4] == 0xee)
                protective = TRUE;
        partition_entry parts[128];
        UINT32 count = 0;
        if (protective) {
            e = gpt_parse(id, 1, parts, &count);
            if (e && disks[id].info.sectors > 2) {
                e = gpt_parse(id, disks[id].info.sectors - 1, parts, &count);
                if (!e) {
                    con_print(disks[id].info.name);
                    con_print(": using validated backup GPT (read-only until repaired)\n");
                    recovered = TRUE;
                }
            }
        } else
            e = mbr_parse(id, sector, parts, &count);
        if (!e && count) {
            for (UINT32 p = 0; p < count; ++p) {
                UINT32 before = volume_count;
                if (!fat_mount(id, parts[p].start, parts[p].length, parts[p].number) &&
                    before < volume_count)
                    volumes[before].partition_ro = recovered || parts[p].read_only;
            }
        } else if (!protective) {
            /* Superfloppy BPB still undergoes full FAT validation. */
            UINT32 before = volume_count;
            if (!fat_mount(id, 0, disks[id].info.sectors, 0) && before < volume_count)
                volumes[before].partition_ro = e != 0;
        }
        if (e && protective) {
            con_print(disks[id].info.name);
            con_print(": invalid/unsupported GPT; no volumes mounted\n");
        }
    }
    return 0;
}

static BOOLEAN valid_process(k_process *p)
{
    UINTN a = (UINTN)p, b = (UINTN)processes;
    return a >= b && a < b + sizeof(processes) && !((a - b) % sizeof(*p)) && p->used;
}
k_process *k_process_create(const char *name)
{
    if (k_cpu_id() != 0 || !name || !next_process_id)
        return NULL;
    UINT64 f = k_spin_lock(&sched_lock);
    k_process *p = NULL;
    for (UINT32 i = 0; i < MAX_PROCESSES; ++i)
        if (!processes[i].used) {
            p = &processes[i];
            mem_zero(p, sizeof(*p));
            p->used = TRUE;
            p->id = next_process_id++;
            str_copy(p->name, name, sizeof(p->name));
            break;
        }
    k_spin_unlock(&sched_lock, f);
    if (!p)
        return NULL;
    p->as = k_as_create();
    if (!p->as) {
        p->used = FALSE;
        return NULL;
    }
    p->as->owner = p;
    return p;
}
k_address_space *k_process_space(k_process *p)
{
    return valid_process(p) ? p->as : NULL;
}
UINT32 k_process_id(k_process *p)
{
    return valid_process(p) ? p->id : 0;
}
static int rief_string(const UINT8 *bytes, const rief_header_t *h, UINT32 off, char out[128])
{
    if (off >= h->string_table_size)
        return K_ENOEXEC;
    const UINT8 *s = bytes + h->string_table_offset + off;
    for (UINT32 i = 0; i < 128 && off + i < h->string_table_size; ++i) {
        if (!s[i]) {
            out[i] = 0;
            return i ? 0 : K_ENOEXEC;
        }
        if (s[i] < 33 || s[i] > 126)
            return K_ENOEXEC;
        out[i] = (char)s[i];
    }
    return K_ENOEXEC;
}
static BOOLEAN overlap(UINT64 a, UINT64 n, UINT64 b, UINT64 m)
{
    return n && m && a < b + m && b < a + n;
}
/* Roll back only mappings made by this load, preserving previously allocated
 * user objects that an explicit import binding might refer to. */
static BOOLEAN rollback_tree(UINT64 *table, UINT32 level, UINT64 base, UINT64 from)
{
    BOOLEAN empty = TRUE;
    UINT64 span = 1ULL << (12 + 9 * (level - 1));
    for (UINT32 i = 0; i < 512; ++i) {
        UINT64 entry = table[i];
        if (!(entry & PAGE_PRESENT))
            continue;
        UINT64 address = base + i * span, phys = entry & PHYS_MASK;
        if (address + span > from) {
            if (level == 1) {
                pmm_free_page(phys);
                table[i] = 0;
                continue;
            }
            if (rollback_tree((void *)(UINTN)phys, level - 1, address, from)) {
                pmm_free_page(phys);
                table[i] = 0;
                continue;
            }
        }
        empty = FALSE;
    }
    return empty;
}
static void as_rollback(k_address_space *as, UINT64 old_next, UINT32 old_pages)
{
    UINT64 f = k_spin_lock(&vm_lock), entry = as->pml4[USER_SLOT];
    if (entry & PAGE_PRESENT) {
        UINT64 phys = entry & PHYS_MASK;
        if (rollback_tree((void *)(UINTN)phys, 3, K_USER_BASE, old_next)) {
            pmm_free_page(phys);
            as->pml4[USER_SLOT] = 0;
        }
    }
    as->next = old_next;
    as->pages = old_pages;
    k_spin_unlock(&vm_lock, f);
}
int k_rief_load(k_process *p, const void *image, UINT64 size, const k_rief_binding *bindings,
                UINT32 binding_count)
{
    if (!valid_process(p) || p->loaded || p->task_id || !image || k_cpu_id() != 0 ||
        (binding_count && !bindings) || binding_count > 64)
        return K_EINVAL;
    if (size < sizeof(rief_header_t) || size > 64ULL * 1024 * 1024)
        return K_ENOEXEC;
    const UINT8 *bytes = image;
    rief_header_t h;
    mem_copy(&h, image, sizeof(h));
    if (h.magic != RIEF_MAGIC || h.version_major != 1 || h.version_minor != 0 ||
        h.architecture != RIEF_ARCH_X86_64 || h.flags || h.reserved || h.header_size < sizeof(h) ||
        h.header_size > 4096 || h.header_size > size || !h.region_count ||
        h.region_count > MAX_REGIONS || h.reloc_count > 4096 || h.import_count > 64 ||
        h.export_count > MAX_EXPORTS || h.entry_region >= h.region_count)
        return K_ENOEXEC;
    for (UINT32 i = sizeof(h); i < h.header_size; ++i)
        if (bytes[i])
            return K_ENOEXEC;
    UINT64 meta_off[] = {0,
                         h.region_table_offset,
                         h.reloc_table_offset,
                         h.import_table_offset,
                         h.export_table_offset,
                         h.string_table_offset};
    UINT64 meta_len[] = {h.header_size,
                         h.region_count * sizeof(rief_region_t),
                         h.reloc_count * sizeof(rief_reloc_t),
                         h.import_count * sizeof(rief_import_t),
                         h.export_count * sizeof(rief_export_t),
                         h.string_table_size};
    for (UINT32 i = 0; i < ARRAY_LEN(meta_off); ++i) {
        if (!span_ok(meta_off[i], meta_len[i], size) || (!meta_len[i] && meta_off[i]) ||
            (i > 0 && i < 5 && (meta_off[i] & 7)))
            return K_ENOEXEC;
        for (UINT32 j = 0; j < i; ++j)
            if (overlap(meta_off[i], meta_len[i], meta_off[j], meta_len[j]))
                return K_ENOEXEC;
    }
    rief_region_t regions[MAX_REGIONS];
    UINT64 total = 0;
    for (UINT32 i = 0; i < h.region_count; ++i) {
        mem_copy(&regions[i], bytes + h.region_table_offset + i * sizeof(rief_region_t),
                 sizeof(rief_region_t));
        rief_region_t *r = &regions[i];
        if (!r->memory_size || r->memory_size > 64ULL * 1024 * 1024 ||
            r->file_size > r->memory_size || !span_ok(r->file_offset, r->file_size, size) ||
            !power2(r->alignment) || r->alignment > 0x40000000 || (r->flags & ~7) ||
            !(r->flags & RIEF_REGION_R) || ((r->flags & 6) == 6))
            return K_ENOEXEC;
        total += ALIGN_UP(r->memory_size, 4096);
        if (total > 64ULL * 1024 * 1024)
            return K_E2BIG;
        for (UINT32 j = 0; j < ARRAY_LEN(meta_off); ++j)
            if (overlap(r->file_offset, r->file_size, meta_off[j], meta_len[j]))
                return K_ENOEXEC;
        for (UINT32 j = 0; j < i; ++j)
            if (overlap(r->file_offset, r->file_size, regions[j].file_offset, regions[j].file_size))
                return K_ENOEXEC;
    }
    if (!(regions[h.entry_region].flags & RIEF_REGION_X) ||
        h.entry_offset >= regions[h.entry_region].file_size)
        return K_ENOEXEC;
    UINT64 imports[64];
    char symbol[128];
    for (UINT32 i = 0; i < h.import_count; ++i) {
        rief_import_t imp;
        mem_copy(&imp, bytes + h.import_table_offset + i * sizeof(imp), sizeof(imp));
        if (imp.reserved || rief_string(bytes, &h, imp.name_offset, symbol))
            return K_ENOEXEC;
        BOOLEAN found = FALSE;
        for (UINT32 b = 0; b < binding_count; ++b)
            if (bindings[b].name && str_eq(symbol, bindings[b].name)) {
                if (found || k_as_check(p->as, bindings[b].address, 1, FALSE))
                    return K_EFAULT;
                imports[i] = bindings[b].address;
                found = TRUE;
            }
        if (!found)
            return K_ENOENT;
    }
    /* Relocations are sorted by (patch_region, patch_offset) and disjoint.
     * This bounds validation work and prevents last-relocation-wins ambiguity. */
    UINT32 last_region = 0;
    UINT64 last_end = 0;
    for (UINT32 i = 0; i < h.reloc_count; ++i) {
        rief_reloc_t r;
        mem_copy(&r, bytes + h.reloc_table_offset + i * sizeof(r), sizeof(r));
        UINT32 width = r.type == RIEF_RELOC_REL32 ? 4 : 8;
        if (r.reserved || r.type < RIEF_RELOC_ABS64 || r.type > RIEF_RELOC_IMPORT64 ||
            r.patch_region >= h.region_count ||
            !span_ok(r.patch_offset, width, regions[r.patch_region].memory_size) ||
            (i && (r.patch_region < last_region ||
                   (r.patch_region == last_region && r.patch_offset < last_end))))
            return K_ENOEXEC;
        if (r.type == RIEF_RELOC_IMPORT64) {
            if (r.target_region >= h.import_count || r.target_offset)
                return K_ENOEXEC;
        } else if (r.target_region >= h.region_count ||
                   r.target_offset > regions[r.target_region].memory_size)
            return K_ENOEXEC;
        last_region = r.patch_region;
        last_end = r.patch_offset + width;
    }
    for (UINT32 i = 0; i < h.export_count; ++i) {
        rief_export_t ex;
        mem_copy(&ex, bytes + h.export_table_offset + i * sizeof(ex), sizeof(ex));
        if (ex.region >= h.region_count || ex.offset >= regions[ex.region].memory_size ||
            rief_string(bytes, &h, ex.name_offset, p->exports[i].name))
            return K_ENOEXEC;
        for (UINT32 j = 0; j < i; ++j)
            if (str_eq(p->exports[i].name, p->exports[j].name))
                return K_ENOEXEC;
    }
    UINT64 old_next = p->as->next;
    UINT32 old_pages = p->as->pages;
    int error = 0;
    for (UINT32 i = 0; i < h.region_count; ++i) {
        rief_region_t *r = &regions[i];
        error = k_as_alloc(p->as, r->memory_size, r->alignment, r->flags, &p->region_base[i]);
        if (error)
            goto fail;
        p->region_size[i] = r->memory_size;
        error = as_copy(p->as, p->region_base[i], (void *)(bytes + r->file_offset), r->file_size,
                        TRUE, TRUE);
        if (error)
            goto fail;
    }
    for (UINT32 i = 0; i < h.reloc_count; ++i) {
        rief_reloc_t r;
        mem_copy(&r, bytes + h.reloc_table_offset + i * sizeof(r), sizeof(r));
        UINT64 target;
        if (r.type == RIEF_RELOC_IMPORT64)
            target = imports[r.target_region];
        else
            target = p->region_base[r.target_region] + r.target_offset;
        if (r.addend >= 0) {
            if ((UINT64)r.addend > ~0ULL - target) {
                error = K_ENOEXEC;
                goto fail;
            }
            target += (UINT64)r.addend;
        } else {
            UINT64 sub = 0 - (UINT64)r.addend;
            if (sub > target) {
                error = K_ENOEXEC;
                goto fail;
            }
            target -= sub;
        }
        if (target < K_USER_BASE || target >= K_USER_LIMIT) {
            error = K_ENOEXEC;
            goto fail;
        }
        if (r.type != RIEF_RELOC_IMPORT64 &&
            (target < p->region_base[r.target_region] ||
             target - p->region_base[r.target_region] > p->region_size[r.target_region])) {
            error = K_ENOEXEC;
            goto fail;
        }
        if (r.type == RIEF_RELOC_IMPORT64 && k_as_check(p->as, target, 1, FALSE)) {
            error = K_EFAULT;
            goto fail;
        }
        UINT64 patch = p->region_base[r.patch_region] + r.patch_offset, value = target;
        UINT32 width = r.type == RIEF_RELOC_REL32 ? 4 : 8;
        if (r.type == RIEF_RELOC_REL32 || r.type == RIEF_RELOC_REL64) {
            INT64 delta = (INT64)target - (INT64)(patch + width);
            if (width == 4 && (delta < (-2147483647LL - 1) || delta > 2147483647LL)) {
                error = K_ENOEXEC;
                goto fail;
            }
            value = (UINT64)delta;
        }
        error = as_copy(p->as, patch, &value, width, TRUE, TRUE);
        if (error)
            goto fail;
    }
    for (UINT32 i = 0; i < h.export_count; ++i) {
        rief_export_t ex;
        mem_copy(&ex, bytes + h.export_table_offset + i * sizeof(ex), sizeof(ex));
        p->exports[i].address = p->region_base[ex.region] + ex.offset;
    }
    p->region_count = h.region_count;
    p->export_count = h.export_count;
    p->entry = p->region_base[h.entry_region] + h.entry_offset;
    p->loaded = TRUE;
    return 0;
fail:
    as_rollback(p->as, old_next, old_pages);
    mem_zero(p->region_base, sizeof(p->region_base));
    mem_zero(p->region_size, sizeof(p->region_size));
    mem_zero(p->exports, sizeof(p->exports));
    return error;
}
int k_rief_export(k_process *p, const char *symbol, UINT64 *address)
{
    if (!valid_process(p) || !p->loaded || !symbol || !address)
        return K_EINVAL;
    for (UINT32 i = 0; i < p->export_count; ++i)
        if (str_eq(symbol, p->exports[i].name)) {
            *address = p->exports[i].address;
            return 0;
        }
    return K_ENOENT;
}
int k_process_start(k_process *p, UINT32 *id)
{
    if (!valid_process(p) || !p->loaded || p->task_id || !id || k_cpu_id() != 0)
        return K_EINVAL;
    if (!p->user_stack) {
        UINT64 stack;
        int e = k_as_alloc(p->as, 8 * 4096, 4096, RIEF_REGION_R | RIEF_REGION_W, &stack);
        if (e)
            return e;
        p->user_stack = stack + 8 * 4096 - 40;
    }
    UINT64 f = k_spin_lock(&sched_lock);
    task *t = new_task(p->name, NULL, NULL, 1024, 0, FALSE);
    if (!t) {
        k_spin_unlock(&sched_lock, f);
        return K_ENOMEM;
    }
    t->process = p;
    t->frame->rip = p->entry;
    t->frame->cs = 0x23;
    t->frame->ss = 0x1b;
    t->frame->rsp = p->user_stack;
    t->frame->rflags = 0x202;
    p->task_id = t->id;
    p->state = K_TASK_READY;
    *id = t->id;
    k_spin_unlock(&sched_lock, f);
    return 0;
}
int k_process_destroy(k_process *p)
{
    if (!valid_process(p) || k_cpu_id() != 0)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    if (p->task_id) {
        if (p->state != K_TASK_ZOMBIE) {
            k_spin_unlock(&sched_lock, f);
            return K_EBUSY;
        }
        for (UINT32 i = 0; i < K_MAX_TASKS; ++i)
            if (tasks[i].process == p) {
                for (UINT32 c = 0; c < platform.discovered_cpus; ++c)
                    if (cpus[c].current == &tasks[i] ||
                        __atomic_load_n(&cpus[c].stack_owner, __ATOMIC_ACQUIRE) == &tasks[i]) {
                        k_spin_unlock(&sched_lock, f);
                        return K_EBUSY;
                    }
                free_task(&tasks[i]);
            }
    }
    p->as->owner = NULL;
    k_spin_unlock(&sched_lock, f);
    int e = k_as_destroy(p->as);
    if (e) {
        p->as->owner = p;
        return e;
    }
    mem_zero(p, sizeof(*p));
    return 0;
}
int k_process_get(UINT32 i, k_process_info *out)
{
    if (i >= MAX_PROCESSES || !out)
        return K_EINVAL;
    UINT64 f = k_spin_lock(&sched_lock);
    k_process *p = &processes[i];
    if (!p->used) {
        k_spin_unlock(&sched_lock, f);
        return K_ENOENT;
    }
    *out = (k_process_info){p->id,        p->task_id,       p->state,    p->fault_vector,
                            p->exit_code, p->fault_address, p->fault_ip, {0}};
    str_copy(out->name, p->name, sizeof(out->name));
    k_spin_unlock(&sched_lock, f);
    return 0;
}
static int user_string(k_process *p, UINT64 address, char out[K_PATH_MAX])
{
    for (UINT32 i = 0; i < K_PATH_MAX; ++i) {
        if (address > K_USER_LIMIT - 1 - i || k_copy_from_user(p->as, &out[i], address + i, 1))
            return K_EFAULT;
        if (!out[i])
            return i ? 0 : K_EINVAL;
    }
    return K_E2BIG;
}
static INT64 syscall_dispatch(irq_frame *f)
{
    task *t = current_task();
    if (!t || !t->process)
        return K_EPERM;
    k_process *p = t->process;
    switch (f->rax) {
    case K_SYS_EXIT:
        k_task_exit((INT32)f->rdi);
    case K_SYS_WRITE: {
        if (f->rdi >= 3 && f->rdi < 16) {
            UINT32 fd = (UINT32)f->rdi;
            if (!p->fd_used[fd] || !(p->fd_flags[fd] & K_OPEN_WRITE))
                return K_EPERM;
            if (f->rdx > 4096)
                return K_E2BIG;
            if (!f->rdx)
                return 0;
            UINT8 data[4096];
            int e = k_copy_from_user(p->as, data, f->rsi, f->rdx);
            if (e)
                return e;
            UINT64 written = 0, off = (p->fd_flags[fd] & K_OPEN_APPEND) ? ~0ULL : p->fd_offset[fd];
            e = k_vfs_write(&p->files[fd], off, data, f->rdx, &written);
            if (e)
                return e;
            p->fd_offset[fd] =
                (p->fd_flags[fd] & K_OPEN_APPEND) ? p->files[fd].size : off + written;
            return (INT64)written;
        }
        if (f->rdi != 1 && f->rdi != 2)
            return K_EINVAL;
        if (f->rdx > 4096)
            return K_E2BIG;
        if (!f->rdx)
            return 0;
        int e = k_as_check(p->as, f->rsi, f->rdx, FALSE);
        if (e)
            return e;
        char buf[129];
        UINT64 done = 0;
        while (done < f->rdx) {
            UINT64 n = MIN(f->rdx - done, 128);
            e = k_copy_from_user(p->as, buf, f->rsi + done, n);
            if (e)
                return e;
            for (UINT64 i = 0; i < n; ++i) {
                /* Keep user output from interpreting terminal control bytes. */
                char c = buf[i];
                con_putc((c == '\n' || (c >= 32 && c < 127)) ? c : '?');
            }
            done += n;
        }
        return (INT64)done;
    }
    case K_SYS_YIELD:
        k_yield();
        return 0;
    case K_SYS_SLEEP:
        if (f->rdi > 86400000)
            return K_EINVAL;
        k_sleep(f->rdi);
        return 0;
    case K_SYS_GETPID:
        return p->id;
    case K_SYS_TICKS:
        return (INT64)k_ticks();
    case K_SYS_SEND: {
        if (f->rdx > K_IPC_BYTES || f->rdi > 0xffffffff)
            return K_EINVAL;
        UINT8 bytes[K_IPC_BYTES];
        int e = k_copy_from_user(p->as, bytes, f->rsi, f->rdx);
        return e ? e : k_ipc_send((UINT32)f->rdi, bytes, (UINT32)f->rdx);
    }
    case K_SYS_RECV: {
        if (f->rsi < sizeof(k_message))
            return K_EINVAL;
        int e = k_as_check(p->as, f->rdi, sizeof(k_message), TRUE);
        if (e)
            return e;
        k_message msg;
        e = k_ipc_receive(&msg);
        return e ? e : k_copy_to_user(p->as, f->rdi, &msg, sizeof(msg));
    }
    case K_SYS_OPEN: {
        UINT64 flags = f->rsi;
        if ((flags & ~31ULL) || ((flags & 30) && !(flags & K_OPEN_WRITE)) ||
            ((flags & K_OPEN_EXCL) && !(flags & K_OPEN_CREATE)))
            return K_EINVAL;
        char path[K_PATH_MAX];
        int e = user_string(p, f->rdi, path);
        if (e)
            return e;
        UINT32 fd;
        for (fd = 3; fd < 16 && p->fd_used[fd]; ++fd)
            ;
        if (fd == 16)
            return K_E2BIG;
        e = k_vfs_open(path, &p->files[fd]);
        if (!e && (flags & K_OPEN_EXCL))
            return K_EEXIST;
        if (e == K_ENOENT && (flags & K_OPEN_CREATE))
            e = k_vfs_create(path, &p->files[fd]);
        if (e)
            return e;
        if (p->files[fd].attributes & 0x10)
            return K_EINVAL;
        /* Raw block-device nodes are privileged; applications use files. */
        if (p->files[fd].kind == 3)
            return K_EPERM;
        if (flags & K_OPEN_TRUNC) {
            e = k_vfs_put(path, NULL, 0);
            if (e)
                return e;
            e = k_vfs_open(path, &p->files[fd]);
            if (e)
                return e;
        }
        p->fd_flags[fd] = (UINT32)flags;
        p->fd_used[fd] = TRUE;
        p->fd_offset[fd] = 0;
        return fd;
    }
    case K_SYS_READ: {
        if (f->rdi < 3 || f->rdi >= 16 || !p->fd_used[f->rdi])
            return K_EINVAL;
        if (f->rdx > 4096)
            return K_E2BIG;
        if (!f->rdx)
            return 0;
        int e = k_as_check(p->as, f->rsi, f->rdx, TRUE);
        if (e)
            return e;
        UINT8 buf[4096];
        UINT64 got = 0;
        e = k_vfs_read(&p->files[f->rdi], p->fd_offset[f->rdi], buf, f->rdx, &got);
        if (e)
            return e;
        e = k_copy_to_user(p->as, f->rsi, buf, got);
        if (e)
            return e;
        p->fd_offset[f->rdi] += got;
        return (INT64)got;
    }
    case K_SYS_MKDIR: {
        char path[K_PATH_MAX];
        int e = user_string(p, f->rdi, path);
        return e ? e : k_vfs_mkdir(path);
    }
    case K_SYS_SYNC:
        return k_vfs_sync();
    case K_SYS_CLOSE:
        if (f->rdi < 3 || f->rdi >= 16 || !p->fd_used[f->rdi])
            return K_EINVAL;
        p->fd_used[f->rdi] = FALSE;
        return 0;
    default:
        return K_ENOTSUP;
    }
}

/* Native xHCI and i8042 byte transports. Input interpretation lives in the OS. */
static void xhci_mdelay(UINT32 ms)
{
    while (ms--)
        k_delay_us(1000);
}
static UINT64 usb_dma_page(void)
{
    return k_pmm_alloc_pages(1, 0x100000000ULL);
}
/* Raw transport only: device semantics and report decoding belong to the OS. */
#define RAW_QUEUE_DEPTH 256
static k_input_report raw_reports[RAW_QUEUE_DEPTH];
static UINT32 raw_head, raw_count, raw_lost[K_INPUT_MAX_DEVICES];
static k_spinlock raw_lock = K_SPINLOCK_INIT;
static k_mutex input_transport_lock = K_MUTEX_INIT;
static BOOLEAN input_started, i8042_enabled[2];
static void raw_push(UINT32 id, const void *bytes, UINT32 n, UINT32 flags)
{
    if (!id || id > K_INPUT_MAX_DEVICES || n > K_INPUT_REPORT_MAX)
        return;
    UINT64 irq = k_spin_lock(&raw_lock);
    if (raw_count == RAW_QUEUE_DEPTH) {
        ++raw_lost[id - 1];
        k_spin_unlock(&raw_lock, irq);
        return;
    }
    k_input_report *r = &raw_reports[(raw_head + raw_count) % RAW_QUEUE_DEPTH];
    r->device = id;
    r->size = n;
    r->flags = flags;
    r->lost = raw_lost[id - 1];
    r->tick = k_ticks();
    if (n)
        mem_copy(r->bytes, bytes, n);
    ++raw_count;
    k_spin_unlock(&raw_lock, irq);
}
int k_input_read(k_input_report *out)
{
    if (!out || k_cpu_id())
        return K_EINVAL;
    UINT64 irq = k_spin_lock(&raw_lock);
    if (!raw_count) {
        k_spin_unlock(&raw_lock, irq);
        return K_EAGAIN;
    }
    k_input_report *r = &raw_reports[raw_head];
    out->device = r->device;
    out->size = r->size;
    out->flags = r->flags;
    out->lost = r->lost;
    out->tick = r->tick;
    mem_copy(out->bytes, r->bytes, r->size);
    raw_head = (raw_head + 1) % RAW_QUEUE_DEPTH;
    --raw_count;
    k_spin_unlock(&raw_lock, irq);
    return 0;
}
static void i8042_receive(UINT8 status, UINT8 value)
{
    UINT32 id = (status & 0x20) ? 2 : 1;
    if (status & 0xc0) {
        UINT64 f = k_spin_lock(&raw_lock);
        ++raw_lost[id - 1];
        k_spin_unlock(&raw_lock, f);
        raw_push(id, NULL, 0, K_INPUT_ERROR);
    } else
        raw_push(id, &value, 1, 0);
}
static void ps2_irq(void)
{
    /* Both i8042 ports share 0x60. Route by AUX status, never by IRQ number. */
    for (UINT32 n = 0; n < 32; ++n) {
        UINT8 s = inb(0x64);
        if (!(s & 1))
            break;
        i8042_receive(s, inb(0x60));
    }
}
static int i8042_wait_write(void)
{
    for (UINT32 n = 0; n < 2000; ++n) {
        UINT8 s = inb(0x64);
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
        outb(0x64, command);
    return e;
}
/* Controller response, with both device clocks disabled by the caller. */
static int i8042_controller_read(UINT8 *out)
{
    for (UINT32 n = 0; n < 2000; ++n) {
        UINT8 s = inb(0x64);
        if (s & 1) {
            UINT8 value = inb(0x60);
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
    /* Keep translation and all unrelated firmware configuration bits. */
    UINT8 cfg = 0;
    e = i8042_controller_byte(0xad);
    if (!e)
        e = i8042_controller_byte(0xa7);
    ps2_irq(); /* retain pending device bytes instead of throwing them away */
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
            outb(0x60, cfg);
    }
    /* Restore the primary port even if an absent second port failed its test. */
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
        UINT8 s = inb(0x64);
        if (s & 1) {
            UINT8 value = inb(0x60);
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
    /* ACK/RESEND are transport handshakes. The command byte is OS supplied. */
    for (UINT32 retry = 0; retry < 3; ++retry) {
        if (port)
            e = i8042_controller_byte(0xd4);
        if (!e)
            e = i8042_wait_write();
        if (!e)
            outb(0x60, byte);
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

/* TRB fields MUST be volatile so the compiler doesn't cache them! */
typedef struct {
    volatile UINT32 param1;
    volatile UINT32 param2;
    volatile UINT32 status;
    volatile UINT32 control;
} xhci_trb_t;

#define MAX_XHCI_CONTROLLERS 8
#define USB_RAW_ENDPOINTS (K_INPUT_MAX_DEVICES - 2)
typedef struct {
    UINT32 slot;
    xhci_trb_t *ep0_ring;
    UINT32 ep0_enqueue, ep0_cycle;
} xhci_dev_t;
typedef struct {
    BOOLEAN failed, running;
    UINT32 index;
    volatile UINT8 *cap, *op;
    volatile UINT32 *db, *rt;
    xhci_trb_t *command_ring, *events;
    UINT32 command_enqueue, command_cycle, event_read, event_phase;
    UINT64 *dcbaa;
    UINT32 context_dwords;
    UINT8 *scratch, *config;
    xhci_dev_t devices[256];
} xhci_controller;
static xhci_controller xhci_controllers[MAX_XHCI_CONTROLLERS];
static xhci_controller *xhci_current = &xhci_controllers[0];
static UINT32 xhci_controller_count;
#define xhci_failed (xhci_current->failed)
#define xhci_cap_regs (xhci_current->cap)
#define xhci_op_regs (xhci_current->op)
#define xhci_db_regs (xhci_current->db)
#define xhci_rt_regs (xhci_current->rt)
#define cmd_ring (xhci_current->command_ring)
#define cmd_enqueue (xhci_current->command_enqueue)
#define cmd_cycle (xhci_current->command_cycle)
#define event_ring (xhci_current->events)
#define event_dequeue (xhci_current->event_read)
#define event_cycle (xhci_current->event_phase)
#define g_dcbaa (xhci_current->dcbaa)
#define g_context_dwords (xhci_current->context_dwords)
#define dma_scratch_buf (xhci_current->scratch)
#define usb_desc_buf (xhci_current->config)
typedef struct {
    k_input_device info;
    UINT32 controller, slot, dci, root_port;
    xhci_trb_t *ring;
    UINT8 *buffer, *descriptor;
    UINT32 enqueue, cycle, requested;
    UINT64 pending_trb;
    BOOLEAN pending, enabled;
} usb_raw_endpoint;
static usb_raw_endpoint usb_endpoints[USB_RAW_ENDPOINTS];
static UINT32 usb_endpoint_count;
static BOOLEAN xhci_route_raw(UINT32, UINT32, UINT64);
static void xhci_queue_raw(usb_raw_endpoint *);

typedef struct {
    UINT32 route_string;
    UINT8 root_port;
    UINT8 parent_slot;
    UINT8 parent_port;
    UINT8 tier;
} usb_topo_t;

static void xhci_ring_doorbell(UINT32 slot, UINT32 target)
{
    if (xhci_failed)
        return;
    __asm__ volatile("sfence" ::: "memory");
    xhci_db_regs[slot] = target;
}

static void xhci_enqueue_cmd(UINT64 param, UINT32 status, UINT32 control)
{
    cmd_ring[cmd_enqueue].param1 = (UINT32)param;
    cmd_ring[cmd_enqueue].param2 = (UINT32)(param >> 32);
    cmd_ring[cmd_enqueue].status = status;
    cmd_ring[cmd_enqueue].control = control | (cmd_cycle ? 1 : 0);

    cmd_enqueue++;
    if (cmd_enqueue == 255) {
        cmd_ring[255].param1 = (UINT32)(UINTN)cmd_ring;
        cmd_ring[255].param2 = (UINT32)(((UINT64)(UINTN)cmd_ring) >> 32);
        cmd_ring[255].status = 0;
        cmd_ring[255].control = (6 << 10) | 2 | (cmd_cycle ? 1 : 0);
        cmd_enqueue = 0;
        cmd_cycle ^= 1;
    }
}

static void xhci_enqueue_ep0(xhci_dev_t *dev, UINT64 param, UINT32 status, UINT32 control)
{
    dev->ep0_ring[dev->ep0_enqueue].param1 = (UINT32)param;
    dev->ep0_ring[dev->ep0_enqueue].param2 = (UINT32)(param >> 32);
    dev->ep0_ring[dev->ep0_enqueue].status = status;
    dev->ep0_ring[dev->ep0_enqueue].control = control | (dev->ep0_cycle ? 1 : 0);

    dev->ep0_enqueue++;
    if (dev->ep0_enqueue == 255) {
        dev->ep0_ring[255].param1 = (UINT32)(UINTN)dev->ep0_ring;
        dev->ep0_ring[255].param2 = (UINT32)(((UINT64)(UINTN)dev->ep0_ring) >> 32);
        dev->ep0_ring[255].status = 0;
        dev->ep0_ring[255].control = (6 << 10) | 2 | (dev->ep0_cycle ? 1 : 0);
        dev->ep0_enqueue = 0;
        dev->ep0_cycle ^= 1;
    }
}

static UINT64 xhci_wait_event(UINT32 expected_trb_type)
{
    if (xhci_failed)
        return 0;
    UINT64 timeout = 50000;
    while (timeout--) {
        UINT32 control = event_ring[event_dequeue].control;

        if ((control & 1) == event_cycle) {
            UINT32 type = (control >> 10) & 0x3F;
            __asm__ volatile("lfence" ::: "memory");
            UINT32 status = event_ring[event_dequeue].status;
            UINT64 pointer =
                event_ring[event_dequeue].param1 | ((UINT64)event_ring[event_dequeue].param2 << 32);
            BOOLEAN routed = xhci_route_raw(control, status, pointer);
            event_dequeue++;
            if (event_dequeue == 256) {
                event_dequeue = 0;
                event_cycle ^= 1;
            }

            UINT64 erdp = (UINT64)(UINTN)event_ring + (event_dequeue * 16);
            xhci_rt_regs[6] = (UINT32)erdp | 8;
            xhci_rt_regs[7] = (UINT32)(erdp >> 32);

            if (!routed && type == expected_trb_type) {
                __asm__ volatile("" : : : "memory");
                return ((UINT64)control << 32) | status;
            }
        }
        k_delay_us(100);
    }
    xhci_failed = TRUE;
    return 0;
}

#define XHCI_TRB_SETUP_STAGE (2U << 10)
#define XHCI_TRB_DATA_STAGE (3U << 10)
#define XHCI_TRB_STATUS_STAGE (4U << 10)
#define XHCI_TRB_IOC (1U << 5)
#define XHCI_TRB_IDT (1U << 6)
#define XHCI_TRB_DIR_IN (1U << 16)
#define XHCI_SETUP_TRT_IN (3U << 16)
#define XHCI_SETUP_TRT_NONE (0U << 16)
#define XHCI_EP_TYPE_INTERRUPT_IN 7

#define USB_CLASS_HUB 9
#define HUB_REQ_GET_STATUS 0x00
#define HUB_REQ_CLEAR_FEATURE 0x01
#define HUB_REQ_SET_FEATURE 0x03
#define HUB_REQ_GET_DESCRIPTOR 0x06
#define HUB_FEATURE_PORT_RESET 4
#define HUB_FEATURE_PORT_POWER 8
#define HUB_FEATURE_C_PORT_CONNECTION 16
#define HUB_FEATURE_C_PORT_RESET 20

static UINT64 usb_setup_packet(UINT8 request_type, UINT8 request, UINT16 value, UINT16 index,
                               UINT16 length)
{
    return (UINT64)request_type | ((UINT64)request << 8) | ((UINT64)value << 16) |
           ((UINT64)index << 32) | ((UINT64)length << 48);
}

static UINT64 xhci_control_in(xhci_dev_t *dev, UINT64 setup, void *buffer, UINT16 length)
{
    xhci_enqueue_ep0(dev, setup, 8, XHCI_TRB_SETUP_STAGE | XHCI_TRB_IDT | XHCI_SETUP_TRT_IN);
    xhci_enqueue_ep0(dev, (UINT64)(UINTN)buffer, length, XHCI_TRB_DATA_STAGE | XHCI_TRB_DIR_IN);
    xhci_enqueue_ep0(dev, 0, 0, XHCI_TRB_STATUS_STAGE | XHCI_TRB_IOC);

    xhci_ring_doorbell(dev->slot, 1);
    return xhci_wait_event(32);
}

static UINT64 xhci_control_no_data(xhci_dev_t *dev, UINT64 setup)
{
    xhci_enqueue_ep0(dev, setup, 8, XHCI_TRB_SETUP_STAGE | XHCI_TRB_IDT | XHCI_SETUP_TRT_NONE);
    xhci_enqueue_ep0(dev, 0, 0, XHCI_TRB_STATUS_STAGE | XHCI_TRB_DIR_IN | XHCI_TRB_IOC);

    xhci_ring_doorbell(dev->slot, 1);
    return xhci_wait_event(32);
}

static BOOLEAN xhci_transfer_ok(UINT64 event)
{
    if (event == 0)
        return FALSE;
    UINT32 comp_code = (UINT32)(event >> 24) & 0xFF;
    return (comp_code == 1 || comp_code == 13);
}

// endpoint internal conversion

static UINT8 usb_interval_to_xhci(UINT8 bInterval, UINT32 port_speed)
{
    if (port_speed == 3 || port_speed >= 4) {
        if (bInterval == 0)
            bInterval = 1;
        if (bInterval > 16)
            bInterval = 16;
        return (UINT8)(bInterval - 1);
    } else {
        UINT32 units = (bInterval ? bInterval : 1) * 8;
        UINT8 exp = 0;
        while ((1U << (exp + 1)) <= units && exp < 15)
            exp++;
        return exp;
    }
}

/* An interrupt-IN pipe is a byte stream with descriptor metadata. No usages,
 * scan codes, buttons, coordinates or device-specific packets are decoded. */
static BOOLEAN xhci_configure_raw(xhci_dev_t *dev, usb_raw_endpoint *ep, UINT8 address,
                                  UINT16 packet, UINT8 interval, UINT32 speed, UINT8 burst,
                                  UINT16 esit)
{
    UINT8 number = address & 15, dci = (UINT8)(number * 2 + 1);
    UINT32 max_packet = packet & 0x7ff;
    UINT32 mult = speed == 3 ? ((packet >> 11) & 3) : 0;
    if (!number || !max_packet || max_packet > K_INPUT_REPORT_MAX || mult == 3 ||
        (speed == 2 && max_packet > 8) || (speed == 1 && max_packet > 64) || burst > 15)
        return FALSE;
    UINT32 payload = speed >= 4 ? esit : max_packet * (mult + 1);
    if (!payload || payload > K_INPUT_REPORT_MAX)
        return FALSE;
    ep->ring = (void *)(UINTN)usb_dma_page();
    ep->buffer = (void *)(UINTN)usb_dma_page();
    UINT32 *ctx = (void *)(UINTN)usb_dma_page();
    if (!ep->ring || !ep->buffer || !ctx)
        return FALSE;
    ep->cycle = 1;
    ep->dci = dci;
    ep->requested = payload;
    ep->info.max_packet = payload;
    ctx[1] = 1 | (1U << dci);
    UINT32 *slot = (void *)(UINTN)g_dcbaa[dev->slot];
    mem_copy(ctx + g_context_dwords, slot, g_context_dwords * 4);
    UINT32 last = slot[0] >> 27;
    if (dci > last)
        last = dci;
    ctx[g_context_dwords] = (slot[0] & 0x07ffffff) | (last << 27);
    UINT32 *ec = ctx + (1 + dci) * g_context_dwords;
    ec[0] = (UINT32)usb_interval_to_xhci(interval, speed) << 16;
    ec[1] = (3 << 1) | (7 << 3) | ((speed >= 4 ? burst : mult) << 8) | (max_packet << 16);
    wr64(ec + 2, (UINT64)(UINTN)ep->ring | 1);
    ec[4] = payload | (payload << 16);
    xhci_enqueue_cmd((UINT64)(UINTN)ctx, 0, (dev->slot << 24) | (12U << 10));
    xhci_ring_doorbell(0, 0);
    BOOLEAN ok = xhci_transfer_ok(xhci_wait_event(33));
    /* Never free DMA contexts after a timeout: the controller may still use them. */
    if (!xhci_failed)
        pmm_free_page((UINT64)(UINTN)ctx);
    return ok;
}
static void xhci_queue_raw(usb_raw_endpoint *ep)
{
    if (ep->pending || !ep->enabled || !ep->info.online || xhci_failed)
        return;
    UINT32 index = ep->enqueue;
    xhci_trb_t *trb = ep->ring + index;
    trb->param1 = (UINT32)(UINTN)ep->buffer;
    trb->param2 = (UINT32)((UINT64)(UINTN)ep->buffer >> 32);
    trb->status = ep->requested;
    ep->pending_trb = (UINT64)(UINTN)trb;
    ep->pending = TRUE;
    __asm__ volatile("sfence" ::: "memory");
    trb->control = (1U << 10) | XHCI_TRB_IOC | (1U << 2) | ep->cycle;
    if (++ep->enqueue == 255) {
        xhci_trb_t *link = ep->ring + 255;
        link->param1 = (UINT32)(UINTN)ep->ring;
        link->param2 = (UINT32)((UINT64)(UINTN)ep->ring >> 32);
        link->status = 0;
        __asm__ volatile("sfence" ::: "memory");
        link->control = (6U << 10) | 2 | ep->cycle;
        ep->enqueue = 0;
        ep->cycle ^= 1;
    }
    xhci_ring_doorbell(ep->slot, ep->dci);
}
static BOOLEAN xhci_route_raw(UINT32 control, UINT32 status, UINT64 pointer)
{
    if (((control >> 10) & 63) != 32)
        return FALSE;
    UINT32 slot = control >> 24, dci = (control >> 16) & 31;
    for (UINT32 i = 0; i < usb_endpoint_count; ++i) {
        usb_raw_endpoint *ep = &usb_endpoints[i];
        if (ep->controller != xhci_current->index || ep->slot != slot || ep->dci != dci ||
            !ep->info.online)
            continue;
        UINT32 cc = status >> 24, residual = status & 0xffffff;
        if (!ep->pending)
            return TRUE;
        ep->pending = FALSE;
        if ((pointer & ~15ULL) != ep->pending_trb || (cc != 1 && cc != 13) ||
            residual > ep->requested) {
            ep->info.online = FALSE;
            raw_push(ep->info.id, NULL, 0, K_INPUT_ERROR | K_INPUT_OFFLINE);
            return TRUE;
        }
        __asm__ volatile("lfence" ::: "memory");
        raw_push(ep->info.id, ep->buffer, ep->requested - residual, 0);
        xhci_queue_raw(ep);
        return TRUE;
    }
    return FALSE;
}
static void xhci_pump_raw(void)
{
    for (UINT32 i = 0; i < usb_endpoint_count; ++i)
        if (usb_endpoints[i].controller == xhci_current->index)
            xhci_queue_raw(&usb_endpoints[i]);
    for (UINT32 count = 0; count < 128; ++count) {
        xhci_trb_t *event = event_ring + event_dequeue;
        UINT32 control = event->control;
        if ((control & 1) != event_cycle)
            break;
        __asm__ volatile("lfence" ::: "memory");
        UINT32 status = event->status;
        UINT64 pointer = event->param1 | ((UINT64)event->param2 << 32);
        if (++event_dequeue == 256) {
            event_dequeue = 0;
            event_cycle ^= 1;
        }
        UINT64 erdp = (UINT64)(UINTN)(event_ring + event_dequeue);
        xhci_rt_regs[6] = (UINT32)erdp | 8;
        xhci_rt_regs[7] = (UINT32)(erdp >> 32);
        (void)xhci_route_raw(control, status, pointer);
    }
    for (UINT32 i = 0; i < usb_endpoint_count; ++i) {
        usb_raw_endpoint *ep = &usb_endpoints[i];
        if (ep->controller != xhci_current->index || !ep->info.online)
            continue;
        volatile UINT32 *port = (void *)(xhci_op_regs + 0x400 + 16 * (ep->root_port - 1));
        if (xhci_failed || !(*port & 1)) {
            ep->info.online = FALSE;
            raw_push(ep->info.id, NULL, 0, K_INPUT_OFFLINE);
        }
    }
}

// Hub helpers

static BOOLEAN hub_get_descriptor(xhci_dev_t *dev, BOOLEAN superspeed, void *buf, UINT16 len)
{
    UINT16 desc_type = superspeed ? 0x2A00 : 0x2900;
    UINT64 res = xhci_control_in(
        dev, usb_setup_packet(0xA0, HUB_REQ_GET_DESCRIPTOR, desc_type, 0, len), buf, len);
    return xhci_transfer_ok(res);
}

static BOOLEAN hub_get_port_status(xhci_dev_t *dev, UINT8 port, UINT32 *status_out)
{
    volatile UINT8 *buf = (volatile UINT8 *)dma_scratch_buf + 128;
    UINT64 res = xhci_control_in(dev, usb_setup_packet(0xA3, HUB_REQ_GET_STATUS, 0, port, 4),
                                 (void *)buf, 4);
    if (!xhci_transfer_ok(res))
        return FALSE;
    *status_out =
        (UINT32)buf[0] | ((UINT32)buf[1] << 8) | ((UINT32)buf[2] << 16) | ((UINT32)buf[3] << 24);
    return TRUE;
}

static void hub_set_port_feature(xhci_dev_t *dev, UINT8 port, UINT16 feature)
{
    xhci_control_no_data(dev, usb_setup_packet(0x23, HUB_REQ_SET_FEATURE, feature, port, 0));
}

static void hub_clear_port_feature(xhci_dev_t *dev, UINT8 port, UINT16 feature)
{
    xhci_control_no_data(dev, usb_setup_packet(0x23, HUB_REQ_CLEAR_FEATURE, feature, port, 0));
}

// slot + device managment

static void xhci_print_completion(CONST char *label, UINT64 event)
{
    UINT8 comp_code = (UINT8)((UINT32)(event & 0xFFFFFFFF) >> 24);
    con_print(label);
    con_print(": completion code ");
    con_print_uint(comp_code);
    con_print("\n");
}

static BOOLEAN xhci_address_device(xhci_dev_t *dev, usb_topo_t *topo, UINT32 port_speed)
{
    UINT32 *input_ctx = (UINT32 *)(UINTN)usb_dma_page();
    UINT32 *output_ctx = (UINT32 *)(UINTN)usb_dma_page();
    dev->ep0_ring = (xhci_trb_t *)(UINTN)usb_dma_page();

    if (!input_ctx || !output_ctx || !dev->ep0_ring) {
        return FALSE;
    }

    for (UINT32 i = 0; i < 1024; i++) {
        input_ctx[i] = 0;
        output_ctx[i] = 0;
    }
    for (UINT32 i = 0; i < 512; i++) {
        ((UINT64 *)dev->ep0_ring)[i] = 0;
    }

    dev->ep0_enqueue = 0;
    dev->ep0_cycle = 1;

    g_dcbaa[dev->slot] = (UINT64)(UINTN)output_ctx;

    UINT32 max_packet = (port_speed >= 4) ? 512 : (port_speed == 3) ? 64 : 8;

    input_ctx[1] = 0x03;

    input_ctx[g_context_dwords + 0] =
        (1U << 27) | (port_speed << 20) | (topo->route_string & 0xFFFFF);
    input_ctx[g_context_dwords + 1] = ((UINT32)topo->root_port << 16);
    input_ctx[g_context_dwords + 2] =
        ((UINT32)topo->parent_slot) | ((UINT32)topo->parent_port << 8);

    input_ctx[2 * g_context_dwords + 1] = (3 << 1) /* Err */ | (4U << 3) | (max_packet << 16);
    input_ctx[2 * g_context_dwords + 2] = (UINT32)(UINT64)(UINTN)dev->ep0_ring | 1;
    input_ctx[2 * g_context_dwords + 3] = (UINT32)((UINT64)(UINTN)dev->ep0_ring >> 32);
    input_ctx[2 * g_context_dwords + 4] = 8;

    xhci_enqueue_cmd((UINT64)(UINTN)input_ctx, 0, (dev->slot << 24) | (11U << 10));
    xhci_ring_doorbell(0, 0);
    UINT64 res = xhci_wait_event(33);
    xhci_print_completion("Address Device", res);
    return xhci_transfer_ok(res);
}

static BOOLEAN xhci_set_hub_slot_info(xhci_dev_t *dev, UINT8 num_ports, BOOLEAN multi_tt)
{
    UINT32 *input_ctx = (UINT32 *)(UINTN)usb_dma_page();
    if (!input_ctx)
        return FALSE;
    for (UINT32 i = 0; i < 1024; i++)
        input_ctx[i] = 0;

    input_ctx[1] = 0x01; /* Add Slot Context only */

    UINT32 *old = (UINT32 *)(UINTN)g_dcbaa[dev->slot];
    for (UINT32 i = 0; i < g_context_dwords; ++i)
        input_ctx[g_context_dwords + i] = old[i];
    UINT32 slot0 = old[0];
    slot0 |= (1U << 26);
    if (multi_tt)
        slot0 |= (1U << 25);
    input_ctx[g_context_dwords + 0] = slot0;
    input_ctx[g_context_dwords + 1] = (old[1] & 0x00ffffffU) | ((UINT32)num_ports << 24);

    xhci_enqueue_cmd((UINT64)(UINTN)input_ctx, 0, (dev->slot << 24) | (12U << 10));
    xhci_ring_doorbell(0, 0);
    UINT64 res = xhci_wait_event(33);
    return xhci_transfer_ok(res);
}

static BOOLEAN xhci_finish_interfaces(xhci_dev_t *dev, const UINT8 *cfg, UINT16 size, UINT32 speed,
                                      UINT32 root_port, UINT16 vendor, UINT16 product)
{
    if (size < 9 || cfg[0] < 9 || cfg[1] != 2 || !cfg[5])
        return FALSE;
    BOOLEAN configured = FALSE;
    UINT32 added = 0;
    /* Walk every alternate-zero HID interface, including composite receivers. */
    for (UINT32 pos = cfg[0]; pos + 2 <= size;) {
        UINT32 len = cfg[pos];
        if (len < 2 || len > size - pos)
            break;
        if (cfg[pos + 1] != 4 || len < 9 || cfg[pos + 3] || cfg[pos + 5] != 3) {
            pos += len;
            continue;
        }
        UINT32 end = pos + len;
        UINT16 report_size = 0, packet = 0, esit = 0;
        UINT8 address = 0, interval = 0, burst = 0;
        while (end + 2 <= size && cfg[end + 1] != 4) {
            UINT32 n = cfg[end];
            if (n < 2 || n > size - end)
                return added != 0;
            if (cfg[end + 1] == 0x21 && n >= 9) {
                for (UINT32 at = 6; at + 3 <= n; at += 3)
                    if (cfg[end + at] == 0x22)
                        report_size = rd16(cfg + end + at + 1);
            }
            if (!address && cfg[end + 1] == 5 && n >= 7 && (cfg[end + 2] & 0x80) &&
                (cfg[end + 3] & 3) == 3) {
                address = cfg[end + 2];
                packet = rd16(cfg + end + 4);
                interval = cfg[end + 6];
                if (speed >= 4 && end + n + 6 <= size && cfg[end + n] >= 6 &&
                    cfg[end + n + 1] == 0x30) {
                    burst = cfg[end + n + 2];
                    esit = rd16(cfg + end + n + 4);
                    if (cfg[end + n + 3] & 3)
                        address = 0; /* no high-bandwidth SS Mult input endpoint */
                }
            }
            end += n;
        }
        if (address && usb_endpoint_count < USB_RAW_ENDPOINTS) {
            if (!configured) {
                if (!xhci_transfer_ok(
                        xhci_control_no_data(dev, usb_setup_packet(0, 9, cfg[5], 0, 0))))
                    return FALSE;
                configured = TRUE;
            }
            usb_raw_endpoint *ep = &usb_endpoints[usb_endpoint_count];
            mem_zero(ep, sizeof(*ep));
            ep->controller = xhci_current->index;
            ep->slot = dev->slot;
            ep->root_port = root_port;
            ep->info = (k_input_device){.id = usb_endpoint_count + 3,
                                        .transport = K_INPUT_USB,
                                        .controller = ep->controller,
                                        .port = root_port,
                                        .vendor = vendor,
                                        .product = product,
                                        .interface_number = cfg[pos + 2],
                                        .interface_class = cfg[pos + 5],
                                        .interface_subclass = cfg[pos + 6],
                                        .interface_protocol = cfg[pos + 7],
                                        .endpoint = address};
            if (report_size && report_size <= K_INPUT_DESCRIPTOR_MAX) {
                ep->descriptor = (void *)(UINTN)usb_dma_page();
                if (ep->descriptor &&
                    xhci_transfer_ok(xhci_control_in(
                        dev,
                        usb_setup_packet(0x81, 6, 0x2200, ep->info.interface_number, report_size),
                        ep->descriptor, report_size)))
                    ep->info.descriptor_size = report_size;
                else if (xhci_failed)
                    return added != 0;
            }
            if (xhci_configure_raw(dev, ep, address, packet, interval, speed, burst, esit)) {
                ep->info.online = TRUE;
                ++usb_endpoint_count;
                ++added;
            } else if (xhci_failed)
                return added != 0;
        }
        pos = end;
    }
    xhci_current->devices[dev->slot] = *dev;
    return added != 0;
}

static BOOLEAN xhci_probe_device(usb_topo_t *topo, UINT32 port_speed)
{
    if (xhci_failed || topo->tier > 5) {
        return FALSE;
    }

    xhci_enqueue_cmd(0, 0, (9U << 10));
    xhci_ring_doorbell(0, 0);
    UINT64 res = xhci_wait_event(33);
    if (!xhci_transfer_ok(res)) {
        con_print("xhci: Enable Slot failed\n");
        return FALSE;
    }

    xhci_dev_t dev;
    dev.slot = (UINT32)(res >> 56) & 0xFF;
    if (!dev.slot) {
        return FALSE;
    }

    xhci_mdelay(50);

    if (!xhci_address_device(&dev, topo, port_speed)) {
        con_print("xhci: Address Device failed\n");
        goto fail_disable;
    }

    xhci_mdelay(20);

    con_print("xhci: about to GET_DESCRIPTOR(Device)\n");

    {
        volatile UINT8 *devdesc = (volatile UINT8 *)dma_scratch_buf + 0;

        res = xhci_control_in(&dev, usb_setup_packet(0x80, 0x06, 0x0100, 0, 8), (void *)devdesc, 8);
        if (!xhci_transfer_ok(res)) {
            con_print("xhci: GET_DESCRIPTOR(Device) 8-byte read failed\n");
            goto fail_disable;
        }

        UINT32 initial_max_packet = (port_speed >= 4) ? 512 : (port_speed == 3) ? 64 : 8;
        UINT32 actual_max_packet = devdesc[7];
        if (port_speed >= 4) {
            if (devdesc[7] != 9)
                goto fail_disable;
            actual_max_packet = 512;
        }
        if (actual_max_packet != 8 && actual_max_packet != 16 && actual_max_packet != 32 &&
            actual_max_packet != 64 && actual_max_packet != 512)
            goto fail_disable;

        /* If the device has a different max packet size, update the xHCI context */
        if (actual_max_packet != initial_max_packet && actual_max_packet != 0) {
            con_print("xhci: updating EP0 max packet to ");
            con_print_uint(actual_max_packet);
            con_print("\n");

            UINT32 *input_ctx = (UINT32 *)(UINTN)usb_dma_page();
            if (input_ctx) {
                for (UINT32 i = 0; i < 1024; i++)
                    input_ctx[i] = 0;
                input_ctx[1] = 0x02;

                UINT32 *output_ctx = (UINT32 *)(UINTN)g_dcbaa[dev.slot];
                for (UINT32 i = 0; i < g_context_dwords; i++) {
                    input_ctx[2 * g_context_dwords + i] = output_ctx[1 * g_context_dwords + i];
                }
                input_ctx[2 * g_context_dwords + 1] &= ~0xFFFF0000;
                input_ctx[2 * g_context_dwords + 1] |= (actual_max_packet << 16);

                xhci_enqueue_cmd((UINT64)(UINTN)input_ctx, 0, (dev.slot << 24) | (13U << 10));
                xhci_ring_doorbell(0, 0);
                UINT64 evaluated = xhci_wait_event(33);
                if (!xhci_transfer_ok(evaluated))
                    goto fail_disable;
                pmm_free_page((UINT64)(UINTN)input_ctx);
            }
        }

        /* Now safely read the full 18 bytes */
        res =
            xhci_control_in(&dev, usb_setup_packet(0x80, 0x06, 0x0100, 0, 18), (void *)devdesc, 18);
        if (!xhci_transfer_ok(res)) {
            con_print("xhci: GET_DESCRIPTOR(Device) full read failed\n");
            goto fail_disable;
        }

        if (devdesc[4] == USB_CLASS_HUB) {
            BOOLEAN is_ss = (devdesc[3] >= 3);
            volatile UINT8 *hubdesc = (volatile UINT8 *)dma_scratch_buf + 32;
            UINT8 num_ports;
            BOOLEAN multi_tt = FALSE;

            volatile UINT8 *hubcfg = (volatile UINT8 *)dma_scratch_buf + 64;
            res = xhci_control_in(&dev, usb_setup_packet(0x80, 6, 0x0200, 0, 9), (void *)hubcfg, 9);
            if (!xhci_transfer_ok(res) || hubcfg[1] != 2 || !hubcfg[5])
                goto fail_disable;
            res = xhci_control_no_data(&dev, usb_setup_packet(0, 9, hubcfg[5], 0, 0));
            if (!xhci_transfer_ok(res))
                goto fail_disable;
            if (is_ss) {
                res = xhci_control_no_data(
                    &dev, usb_setup_packet(0x20, 12, (UINT16)(topo->tier - 1), 0, 0));
                if (!xhci_transfer_ok(res))
                    goto fail_disable;
            }
            if (!hub_get_descriptor(&dev, is_ss, (void *)hubdesc, is_ss ? 12 : 9)) {
                con_print("xhci: could not read hub descriptor\n");
                goto fail_disable;
            }

            num_ports = hubdesc[2];
            if (!is_ss) {
                if (devdesc[6] == 2) {
                    UINT64 alternate = xhci_control_no_data(&dev, usb_setup_packet(1, 11, 1, 0, 0));
                    multi_tt = xhci_transfer_ok(alternate);
                }
            }

            if (!xhci_set_hub_slot_info(&dev, num_ports, multi_tt)) {
                con_print("xhci: failed to configure hub slot\n");
                goto fail_disable;
            }

            con_print("xhci: hub found (slot ");
            con_print_uint(dev.slot);
            con_print(", ");
            con_print_uint(num_ports);
            con_print(" ports)\n");

            for (UINT8 p = 1; p <= num_ports && p <= 15; p++) {
                hub_set_port_feature(&dev, p, HUB_FEATURE_PORT_POWER);
            }
            xhci_mdelay(200); /* power settle, per USB spec (increased for hardware) */

            for (UINT8 p = 1; p <= num_ports && p <= 15; p++) {
                UINT32 status;

                if (!hub_get_port_status(&dev, p, &status))
                    continue;
                if (!(status & 0x0001))
                    continue; /* nothing connected */

                hub_clear_port_feature(&dev, p, HUB_FEATURE_C_PORT_CONNECTION);
                hub_set_port_feature(&dev, p, HUB_FEATURE_PORT_RESET);

                UINT64 to = 500;
                do {
                    if (!hub_get_port_status(&dev, p, &status))
                        break;
                    if (status & 0x00100000)
                        break; /* C_PORT_RESET */
                    xhci_mdelay(1);
                } while (--to);

                hub_clear_port_feature(&dev, p, HUB_FEATURE_C_PORT_RESET);
                if (!hub_get_port_status(&dev, p, &status))
                    continue;
                if (!(status & 0x0001) || !(status & 0x0002))
                    continue;

                UINT32 child_speed;
                if (is_ss)
                    child_speed = 4;
                else if (status & 0x0400)
                    child_speed = 3; /* High */
                else if (status & 0x0200)
                    child_speed = 2; /* Low  */
                else
                    child_speed = 1; /* Full */

                usb_topo_t child = *topo;
                child.route_string |= ((UINT32)p << (4 * (topo->tier - 1)));
                if (child_speed < 3 && port_speed == 3) {
                    child.parent_slot = (UINT8)dev.slot;
                    child.parent_port = p;
                } else if (child_speed >= 3) {
                    child.parent_slot = 0;
                    child.parent_port = 0;
                }
                child.tier = topo->tier + 1;

                con_print("xhci: hub port ");
                con_print_uint(p);
                con_print(": device connected, probing...\n");

                (void)xhci_probe_device(&child, child_speed);
                if (xhci_failed)
                    return FALSE;
            }
            return TRUE; /* retain hub and enumerate every downstream port */
        }

        {
            volatile UINT8 *cfgdesc = (volatile UINT8 *)dma_scratch_buf + 64;
            UINT16 total_len;

            res = xhci_control_in(&dev, usb_setup_packet(0x80, 0x06, 0x0200, 0, 9), (void *)cfgdesc,
                                  9);
            if (!xhci_transfer_ok(res)) {
                con_print("xhci: GET_DESCRIPTOR(Config short) transfer failed\n");
                goto fail_disable;
            }
            if (cfgdesc[1] != 2) {
                con_print("xhci: unexpected config desc type, byte1=");
                con_print_uint(cfgdesc[1]);
                con_print(" byte0=");
                con_print_uint(cfgdesc[0]);
                con_print("\n");
                goto fail_disable;
            }

            total_len = (UINT16)cfgdesc[2] | ((UINT16)cfgdesc[3] << 8);
            if (total_len < 9 || total_len > 4096) {
                goto fail_disable;
            }

            volatile UINT8 *v_usb_desc_buf = (volatile UINT8 *)usb_desc_buf;
            res = xhci_control_in(&dev, usb_setup_packet(0x80, 0x06, 0x0200, 0, total_len),
                                  (void *)v_usb_desc_buf, total_len);
            if (!xhci_transfer_ok(res)) {
                goto fail_disable;
            }

            con_print("xhci: device desc bytes = ");
            for (UINT16 i = 0; i < 8 && i < 18; i++) {
                con_print_uint(devdesc[i]);
                con_print(" ");
            }
            con_print("\n");

            con_print("xhci: config total_len = ");
            con_print_uint(total_len);
            con_print("\n");

            /* Dump every descriptor header we walk past, so we can see all interfaces */
            {
                UINT16 pos = v_usb_desc_buf[0];
                while (pos + 2 <= total_len) {
                    UINT8 len = v_usb_desc_buf[pos];
                    UINT8 type = v_usb_desc_buf[pos + 1];
                    if (len < 2 || pos + len > total_len)
                        break;
                    con_print("  desc @");
                    con_print_uint(pos);
                    con_print(" len=");
                    con_print_uint(len);
                    con_print(" type=");
                    con_print_uint(type);
                    if (type == 4 && len >= 9) {
                        con_print(" class=");
                        con_print_uint(v_usb_desc_buf[pos + 5]);
                        con_print(" subclass=");
                        con_print_uint(v_usb_desc_buf[pos + 6]);
                        con_print(" proto=");
                        con_print_uint(v_usb_desc_buf[pos + 7]);
                    }
                    con_print("\n");
                    pos += len;
                }
            }

            if (!xhci_finish_interfaces(&dev, (const UINT8 *)v_usb_desc_buf, total_len, port_speed,
                                        topo->root_port, rd16((const void *)(devdesc + 8)),
                                        rd16((const void *)(devdesc + 10)))) {
                con_print("xhci: no supported interrupt-IN interface\n");
                goto fail_disable;
            }

            return TRUE;
        }
    }

fail_disable:
    for (UINT32 i = 0; i < usb_endpoint_count; ++i)
        if (usb_endpoints[i].controller == xhci_current->index && usb_endpoints[i].slot == dev.slot)
            usb_endpoints[i].info.online = FALSE;
    xhci_enqueue_cmd(0, 0, (dev.slot << 24) | (10U << 10)); /* Disable Slot */
    xhci_ring_doorbell(0, 0);
    xhci_wait_event(33);
    return FALSE;
}

static BOOLEAN xhci_bios_handoff(void)
{
    UINT32 hccparams1 = *(volatile UINT32 *)(xhci_cap_regs + 0x10);
    UINT32 xecp = (hccparams1 >> 16) & 0xFFFF;

    if (!xecp) {
        return TRUE;
    }

    volatile UINT32 *ext_cap = (volatile UINT32 *)(xhci_cap_regs + (xecp << 2));

    UINT32 hops = 0;
    while (++hops <= 256) {
        if ((UINTN)ext_cap < (UINTN)xhci_cap_regs ||
            (UINTN)ext_cap + 8 > (UINTN)xhci_cap_regs + 0x100000)
            return FALSE;
        UINT32 cap = *ext_cap;
        UINT8 cap_id = cap & 0xFF;
        UINT8 next = (cap >> 8) & 0xFF;

        if (cap_id == 1) {
            con_print("xhci: Found USB Legacy Support. Requesting OS ownership...\n");

            *ext_cap = cap | (1U << 24);
            if (cap & (1 << 16)) {
                *ext_cap = cap | (1 << 24);

                UINT32 timeout = 1000;
                while ((*ext_cap & (1 << 16)) && timeout > 0) {
                    xhci_mdelay(1);
                    timeout--;
                }

                if (*ext_cap & (1 << 16)) {
                    con_print("xhci: firmware ownership timeout; skipping controller\n");
                    return FALSE;
                } else {
                    con_print("xhci: OS ownership acquired.\n");
                }
            } else {
                con_print("xhci: OS ownership already acquired.\n");
            }

            volatile UINT32 *legctlsts = ext_cap + 1;
            UINT32 ctlsts = *legctlsts;
            ctlsts &= 0x1EE00000;
            *legctlsts = ctlsts;
        }

        if (next == 0) {
            return TRUE;
        }
        ext_cap += next;
    }
    return FALSE;
}

#define PORTSC_RW1C_MASK 0x00FE0002

static UINT32 xhci_find_controllers(UINT64 *out_bases, UINT32 max_out)
{
    UINT16 bus;
    UINT8 slot, func;
    UINT32 count = 0;

    for (bus = 0; bus < 256; bus++) {
        for (slot = 0; slot < 32; slot++) {
            for (func = 0; func < 8; func++) {
                if ((pci_read_32(bus, slot, func, 0) & 0xFFFF) == 0xFFFF) {
                    continue;
                }

                if ((pci_read_32(bus, slot, func, 8) >> 8) == 0x0C0330) {
                    UINT32 bar0 = pci_read_32(bus, slot, func, 0x10);
                    UINT32 bar1 = pci_read_32(bus, slot, func, 0x14);
                    UINT64 phys_base;

                    if ((bar0 & 6) == 4) {
                        phys_base = (bar0 & 0xFFFFFFF0) | ((UINT64)bar1 << 32);
                    } else {
                        phys_base = (bar0 & 0xFFFFFFF0);
                    }

                    UINT32 vendor_dev = pci_read_32(bus, slot, func, 0x00);
                    if (!phys_base || (bar0 & 1))
                        continue;
                    UINT16 bdf = (UINT16)((bus << 8) | (slot << 3) | func);
                    pci_command(bdf, 2 | 0x400, 0); /* memory and INTx-disable; rings are polled */

                    con_print("xhci: controller at PCI ");
                    con_print_uint(bus);
                    con_print(":");
                    con_print_uint(slot);
                    con_print(".");
                    con_print_uint(func);
                    con_print(", vendor:device=");
                    con_print_uint(vendor_dev & 0xFFFF);
                    con_print(":");
                    con_print_uint((vendor_dev >> 16) & 0xFFFF);
                    con_print("\n");

                    if (count < max_out) {
                        out_bases[count] = phys_base;
                    }
                    count++;
                }
            }
        }
    }

    return count;
}

static BOOLEAN xhci_try_controller(UINT64 phys_base)
{
    xhci_failed = FALSE;
    if (k_mmio_map(phys_base, 0x100000))
        return FALSE;
    xhci_cap_regs = (volatile UINT8 *)(UINTN)phys_base;
    UINT32 db = *(volatile UINT32 *)(xhci_cap_regs + 0x14) & ~3U;
    UINT32 rt = *(volatile UINT32 *)(xhci_cap_regs + 0x18) & ~31U;
    if (xhci_cap_regs[0] < 0x20 || db > 0xffbfc || rt > 0xff000)
        return FALSE;
    xhci_op_regs = xhci_cap_regs + xhci_cap_regs[0];
    xhci_db_regs = (volatile UINT32 *)(xhci_cap_regs + db);
    xhci_rt_regs = (volatile UINT32 *)(xhci_cap_regs + rt + 0x20);

    if (!xhci_bios_handoff())
        return FALSE;

    *(volatile UINT32 *)(xhci_op_regs + 0) &= ~1;
    if (!wait_reg((void *)(xhci_op_regs + 4), 1, 1, 1000))
        return FALSE;

    *(volatile UINT32 *)(xhci_op_regs + 0) |= 2;
    if (!wait_reg((void *)xhci_op_regs, 2, 0, 1000))
        return FALSE;
    if (!wait_reg((void *)(xhci_op_regs + 4), 1 << 11, 0, 1000))
        return FALSE;
    if (!(*(volatile UINT32 *)(xhci_op_regs + 8) & 1))
        return FALSE;

    UINT32 hcsparams1_reg = *(volatile UINT32 *)(xhci_cap_regs + 4);
    UINT32 max_slots = hcsparams1_reg & 0xFF;
    if (!max_slots)
        return FALSE;
    *(volatile UINT32 *)(xhci_op_regs + 0x38) = max_slots;

    UINT64 *dcbaa = (UINT64 *)(UINTN)usb_dma_page();
    cmd_ring = (xhci_trb_t *)(UINTN)usb_dma_page();
    event_ring = (xhci_trb_t *)(UINTN)usb_dma_page();
    UINT64 *erst = (UINT64 *)(UINTN)usb_dma_page();

    if (!dcbaa || !cmd_ring || !event_ring || !erst)
        return FALSE;
    cmd_enqueue = event_dequeue = 0;
    cmd_cycle = event_cycle = 1;
    for (int i = 0; i < 512; i++) {
        dcbaa[i] = 0;
        ((UINT64 *)cmd_ring)[i] = 0;
        ((UINT64 *)event_ring)[i] = 0;
        erst[i] = 0;
    }

    UINT32 hcsparams2 = *(volatile UINT32 *)(xhci_cap_regs + 8);
    UINT32 max_scratch = ((hcsparams2 >> 27) & 0x1F) | (((hcsparams2 >> 21) & 0x1F) << 5);

    if (max_scratch > 0) {
        UINT64 *scratch_arr =
            (UINT64 *)(UINTN)k_pmm_alloc_pages((max_scratch + 511) / 512, 0x100000000ULL);
        if (!scratch_arr)
            return FALSE;
        for (UINT32 i = 0; i < max_scratch; i++) {
            scratch_arr[i] = usb_dma_page();
            if (!scratch_arr[i])
                return FALSE;
        }
        dcbaa[0] = (UINT64)(UINTN)scratch_arr;
    }

    *(volatile UINT64 *)(xhci_op_regs + 0x30) = (UINT64)(UINTN)dcbaa;
    *(volatile UINT64 *)(xhci_op_regs + 0x18) = (UINT64)(UINTN)cmd_ring | 1;

    erst[0] = (UINT64)(UINTN)event_ring;
    erst[1] = 256;
    xhci_rt_regs[2] = 1;
    xhci_rt_regs[4] = (UINT32)(UINT64)(UINTN)erst;
    xhci_rt_regs[5] = (UINT32)((UINT64)(UINTN)erst >> 32);
    xhci_rt_regs[6] = (UINT32)(UINT64)(UINTN)event_ring | 8;
    xhci_rt_regs[7] = (UINT32)((UINT64)(UINTN)event_ring >> 32);

    /* Enable bus mastering only after all controller DMA pointers are valid. */
    for (UINT32 bus = 0; bus < 256; ++bus)
        for (UINT32 slot = 0; slot < 32; ++slot)
            for (UINT32 fn = 0; fn < 8; ++fn) {
                UINT16 bdf = (UINT16)((bus << 8) | (slot << 3) | fn);
                if ((pci_get(bdf, 0) & 0xffff) != 0xffff && (pci_get(bdf, 8) >> 8) == 0x0c0330 &&
                    pci_mbar(bdf, 0x10) == phys_base)
                    pci_command(bdf, 6 | 0x400, 0);
            }
    __asm__ volatile("sfence" ::: "memory");
    *(volatile UINT32 *)(xhci_op_regs + 0) |= 1;
    if (!wait_reg((void *)(xhci_op_regs + 4), 1, 0, 1000))
        return FALSE;

    UINT32 max_ports = (*(volatile UINT32 *)(xhci_cap_regs + 4) >> 24) & 0xFF;
    con_print("xhci: max_ports = ");
    con_print_uint(max_ports);
    con_print("\n");
    volatile UINT32 *port_regs = (volatile UINT32 *)(xhci_op_regs + 0x400);

    /* Unconditionally Power ON all ports (ignoring PPC because motherboards lie) */
    for (UINT32 pi = 0; pi < max_ports; pi++) {
        UINT32 portsc = port_regs[pi * 4];
        port_regs[pi * 4] = (portsc & ~PORTSC_RW1C_MASK) | (1U << 9);
    }

    con_print("xhci: Powered on all ports. Waiting 300ms for devices to connect...\n");
    xhci_mdelay(300); /* Wait a full 300ms for slow RGB keyboards to assert connection */
    con_print("xhci: Controller running.\n");

    UINT32 hccparams1 = *(volatile UINT32 *)(xhci_cap_regs + 0x10);
    g_context_dwords = (hccparams1 & (1U << 2)) ? 16 : 8;
    g_dcbaa = dcbaa;

    dma_scratch_buf = (UINT8 *)(UINTN)usb_dma_page();
    usb_desc_buf = (UINT8 *)(UINTN)usb_dma_page();
    if (!usb_desc_buf || !dma_scratch_buf) {
        con_print("xhci: insufficient memory for USB enumeration\n");
        return FALSE;
    }

    for (UINT32 port_index = 0; port_index < max_ports; port_index++) {
        UINT32 portsc = port_regs[port_index * 4];

        if (!(portsc & 1)) {
            continue; /* nothing connected */
        }

        con_print("xhci: probing root port ");
        con_print_uint(port_index + 1);
        con_print("\n");

        if (((portsc >> 10) & 0x0F) < 4) {
            /* Mask W1C bits and set Port Reset (PR - bit 4) */
            port_regs[port_index * 4] = (portsc & ~PORTSC_RW1C_MASK) | (1U << 4);

            xhci_mdelay(50); /* Let hardware stabilize the reset lines */

            UINT64 timeout = 1000;
            while (timeout-- && (port_regs[port_index * 4] & (1U << 4))) {
                xhci_mdelay(1);
            }

            /* Write 1 to PRC (Bit 21) to clear the reset change event */
            port_regs[port_index * 4] =
                (port_regs[port_index * 4] & ~PORTSC_RW1C_MASK) | (1U << 21);
            xhci_mdelay(20);
        } else {
            /* Wait for SuperSpeed port to automatically transition to Enabled */
            UINT64 timeout = 100;
            while (timeout-- && !(port_regs[port_index * 4] & (1U << 1))) {
                xhci_mdelay(1);
            }
        }

        if (xhci_failed)
            return FALSE;
        portsc = port_regs[port_index * 4];
        if (!(portsc & 1) || !(portsc & (1U << 1))) {
            con_print("xhci: port failed to enable\n");
            continue;
        }

        xhci_mdelay(50);

        UINT32 port_speed = (portsc >> 10) & 0x0F;

        usb_topo_t topo;
        topo.route_string = 0;
        topo.root_port = (UINT8)(port_index + 1);
        topo.parent_slot = 0;
        topo.parent_port = 0;
        topo.tier = 1;

        (void)xhci_probe_device(&topo, port_speed);
    }
    xhci_current->running = !xhci_failed;
    return !xhci_failed;
}

void kernel_usb_init(void)
{
    if (input_started || k_cpu_id())
        return;
    input_started = TRUE;
    if (!platform.dma_allowed) {
        con_print("USB DMA unavailable: ");
        con_print(platform.dma_reason);
        con_print("\n");
        return;
    }
    UINT64 bases[MAX_XHCI_CONTROLLERS];
    UINT32 found = xhci_find_controllers(bases, MAX_XHCI_CONTROLLERS);
    if (found > MAX_XHCI_CONTROLLERS)
        found = MAX_XHCI_CONTROLLERS;
    xhci_controller_count = found;
    for (UINT32 i = 0; i < found; ++i) {
        xhci_current = &xhci_controllers[i];
        xhci_current->index = i;
        con_print("xhci: enumerate controller ");
        con_print_uint(i);
        con_print("\n");
        if (!xhci_try_controller(bases[i])) {
            xhci_failed = TRUE;
            for (UINT32 n = 0; n < usb_endpoint_count; ++n)
                if (usb_endpoints[n].controller == i)
                    usb_endpoints[n].info.online = FALSE;
        }
    }
    con_print("USB interrupt input interfaces: ");
    con_print_uint(usb_endpoint_count);
    con_print("\n");
}
UINT32 k_input_count(void)
{
    return usb_endpoint_count + (platform.ps2 ? 2 : 0);
}
int k_input_get(UINT32 index, k_input_device *out)
{
    if (!out || k_cpu_id())
        return K_EINVAL;
    if (platform.ps2) {
        if (index < 2) {
            *out = (k_input_device){.id = index + 1,
                                    .transport = K_INPUT_I8042,
                                    .port = index,
                                    .max_packet = 1,
                                    .online = index == 0 || i8042_enabled[1]};
            return 0;
        }
        index -= 2;
    }
    if (index >= usb_endpoint_count)
        return K_ENOENT;
    *out = usb_endpoints[index].info;
    return 0;
}
int k_usb_report_descriptor(UINT32 id, void *out, UINT32 capacity, UINT32 *size)
{
    if (!out || !size || id < 3 || id - 3 >= usb_endpoint_count || k_cpu_id())
        return K_EINVAL;
    usb_raw_endpoint *ep = &usb_endpoints[id - 3];
    *size = ep->info.descriptor_size;
    if (!*size)
        return K_ENOENT;
    if (capacity < *size)
        return K_E2BIG;
    mem_copy(out, ep->descriptor, *size);
    return 0;
}
int k_usb_input_protocol(UINT32 id, UINT16 protocol)
{
    if (id < 3 || id - 3 >= usb_endpoint_count || protocol > 1 || k_cpu_id())
        return K_EINVAL;
    int e = k_mutex_lock(&input_transport_lock);
    if (e)
        return e;
    usb_raw_endpoint *ep = &usb_endpoints[id - 3];
    xhci_current = &xhci_controllers[ep->controller];
    if (!ep->info.online || xhci_failed)
        e = K_EIO;
    else if (ep->pending || ep->enabled)
        e = K_EBUSY; /* choose protocol before starting this byte stream */
    else {
        xhci_dev_t *dev = &xhci_current->devices[ep->slot];
        e = xhci_transfer_ok(xhci_control_no_data(
                dev, usb_setup_packet(0x21, 0x0b, protocol, ep->info.interface_number, 0)))
                ? 0
                : K_EIO;
    }
    k_mutex_unlock(&input_transport_lock);
    return e;
}
int k_usb_input_start(UINT32 id)
{
    if (id < 3 || id - 3 >= usb_endpoint_count || k_cpu_id())
        return K_EINVAL;
    usb_raw_endpoint *ep = &usb_endpoints[id - 3];
    xhci_controller *controller = &xhci_controllers[ep->controller];
    if (!ep->info.online || controller->failed || !controller->running)
        return K_EIO;
    ep->enabled = TRUE;
    return 0;
}
void k_input_pump(void)
{
    if (k_cpu_id())
        return;
    if (platform.ps2) {
        UINT64 f = k_irq_save();
        ps2_irq(); /* also works when firmware supplied no usable IRQ routing */
        k_irq_restore(f);
    }
    if (k_mutex_trylock(&input_transport_lock))
        return;
    for (UINT32 i = 0; i < xhci_controller_count; ++i) {
        xhci_current = &xhci_controllers[i];
        if (xhci_current->running)
            xhci_pump_raw();
    }
    k_mutex_unlock(&input_transport_lock);
}
