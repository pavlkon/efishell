// SPDX-License-Identifier: GPL-2.0-only
#ifndef KERNEL_INTERNAL_H
#define KERNEL_INTERNAL_H
#include "kernel.h"
#include "arch.h"

/* Private contracts between kernel modules. */

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define PMM_PAGE_SIZE 4096ULL
#define PMM_MAX_PAGES (256ULL * 262144ULL)
#define STACK_PAGES 32
#define MAX_PROCESSES K_MAX_PROCESSES
#define MAX_AS 72
#define MAX_USER_PAGES 16384
#define MAX_REGIONS 64
#define MAX_EXPORTS 64
#define MAIL_DEPTH 16

typedef struct {
    UINT64 number, args[6];
} k_syscall_request;

typedef arch_irq_frame irq_frame;
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
    UINT64 tls_base;
    UINT32 control_request;
    k_message mail[MAIL_DEPTH];
    arch_task_state arch;
} task;
typedef struct cpu_state {
    arch_cpu_entry entry;
    task *stack_owner, *current;
    UINT32 index, apic_id;
    volatile UINT32 online;
    UINT64 ticks, switches, fair_floor, idle_ticks;
    UINT32 slice, work_pending;
    task *idle;
    arch_cpu_tables tables;
} cpu_state;
struct k_address_space {
    BOOLEAN used;
    arch_address_space mmu;
    UINT64 next;
    UINT32 pages;
    k_process *owner;
};
struct k_process {
    BOOLEAN used, loaded;
    UINT32 parent, execution_class;
    UINT8 uuid[16];
    UINT64 tls;
    char arguments[K_PROCESS_ARGS_MAX];
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

enum { DISK_NVME = 1, DISK_AHCI = 2, DISK_ATA = 3, DISK_RAM = 4, DISK_USB = 5 };
typedef struct {
    UINT32 kind, controller, nsid, port;
    k_disk_info info;
    UINT8 *ram;
    UINT8 flush_command;
} disk;

#define MAX_PARTITIONS 128

int as_copy(k_address_space *as, UINT64 u, void *buf, UINT64 n, BOOLEAN to, BOOLEAN loader);
void bit_clear(UINT8 *b, UINT64 n);
BOOLEAN bit_get(UINT8 *b, UINT64 n);
void bit_set(UINT8 *b, UINT64 n);
extern UINTN boot_desc_size;
extern UINTN boot_entries;
extern EFI_MEMORY_DESCRIPTOR *boot_map;
void console_buffer_init(void);
void console_panic_mode(void);
extern cpu_state cpus[K_MAX_CPUS];
task *current_task(void);
void decimal_name(char *out, const char *prefix, UINT32 n);
int disk_flush(UINT32 id);
int usb_storage_init(void);
int disk_write_fs(UINT32 id, UINT64 lba, UINT32 count, const void *in, UINT64 bytes);
extern disk disks[K_MAX_DISKS];
extern UINT32 *fb_base;
extern UINT32 fb_width;
extern UINT32 fb_height;
extern UINT32 fb_pitch;
extern BOOLEAN fb_pat_wc;
void free_task(task *t);
extern UINT64 global_ticks;
void idle_entry(void *unused);
extern BOOLEAN interrupts_ready;
extern BOOLEAN smp_started;
void mem_copy(void *d, const void *s, UINTN n);
void mem_zero(void *p, UINTN n);
int memcmp(const void *a, const void *b, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *p, int v, size_t n);
INT64 net_syscall(k_process *p, const k_syscall_request *f);
task *new_task(const char *name, k_task_entry entry, void *arg, UINT32 weight, UINT32 cpu,
               BOOLEAN idle);
BOOLEAN overlap(UINT64 a, UINT64 n, UINT64 b, UINT64 m);
extern UINT32 partition_count;
extern k_partition_info partitions[MAX_PARTITIONS];
void pci_command(UINT16 bdf, UINT16 add, UINT16 remove);
UINT32 pci_get(UINT16 bdf, UINT8 off);
UINT64 pci_mbar(UINT16 bdf, UINT8 reg);
void pci_put(UINT16 bdf, UINT8 off, UINT32 v);
UINT32 pci_read_32(UINT8 bus, UINT8 slot, UINT8 func, UINT8 off);
extern k_platform_info platform;
BOOLEAN power2(UINT64 n);
void process_resources_release(k_process *p);
BOOLEAN process_return_control(task *t);
extern struct k_process processes[MAX_PROCESSES];
void ps2_irq(void);
UINT16 rd16(const void *p);
UINT32 rd32(const void *p);
UINT64 rd64(const void *p);
extern k_spinlock sched_lock;
extern BOOLEAN scheduler_ready;
BOOLEAN span_ok(UINT64 off, UINT64 n, UINT64 size);
void str_copy(char *d, const char *s, UINTN cap);
BOOLEAN str_eq(const char *a, const char *b);
UINTN str_len(const char *s);
INT64 syscall_dispatch(const k_syscall_request *f);
INT64 syscall_extended(k_process *p, task *t, const k_syscall_request *f);
void task_ready(task *t);
extern task tasks[K_MAX_TASKS];
extern UINT32 timer_hz;
void wr16(void *p, UINT16 v);
void wr32(void *p, UINT32 v);
void wr64(void *p, UINT64 v);

irq_frame *kernel_schedule(irq_frame *, BOOLEAN);
#endif
