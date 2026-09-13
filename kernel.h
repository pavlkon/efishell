// SPDX-License-Identifier: GPL-2.0-only
#ifndef KERNEL_H
#define KERNEL_H

#include <efi.h>
#include <stdint.h>
#include <stddef.h>

/* x86-64 UEFI kernel API. No symbols or policy are imported from os.c.
 * EFI is used only for boot descriptions; kernel code calls no EFI services. */
#define K_OK 0
#define K_EINVAL (-1)
#define K_ENOMEM (-2)
#define K_ENOENT (-3)
#define K_EBUSY (-4)
#define K_EFAULT (-5)
#define K_ENOTSUP (-6)
#define K_EIO (-7)
#define K_ETIMEDOUT (-8)
#define K_EROFS (-9)
#define K_EAGAIN (-10)
#define K_EPERM (-11)
#define K_E2BIG (-12)
#define K_ENOEXEC (-13)
#define K_EDEADLK (-14)
#define K_EEXIST (-15)
#define K_ENOSPC (-16)
const char *k_strerror(int error);

void kernel_fb_init(UINT32 *, UINT32, UINT32, UINT32, UINT8, UINT8, UINT8);
UINT32 kernel_fb_width(void);
UINT32 kernel_fb_height(void);
UINT32 kernel_con_cols(void);
UINT32 kernel_con_rows(void);
BOOLEAN kernel_console_buffered(void);
BOOLEAN kernel_fb_pat_wc(void); /* PAT requests WC; firmware MTRRs may restrict it. */
void kernel_console_clear(void);
UINT32 kernel_console_scroll(INT32 rows); /* positive = older lines; returns view offset */
/* Generic copied bitmap overlay, at most 32x32. ARGB: alpha 0 transparent, nonzero opaque. A zero dimension hides it. The OS chooses shape/position. */
int kernel_overlay_set(INT32 x, INT32 y, UINT32 width, UINT32 height, const UINT32 *argb);
void kernel_overlay_move(INT32 x, INT32 y);
void con_putc(char);
void con_print(CONST char *);
void con_print_uint(UINT64);
void con_print_int(INT32);
void con_print_2digit(UINT16);
void con_print_hex(UINT64);
void kernel_usb_init(void);
/* Generic transport API. Numeric USB interface metadata and raw reports only;
 * the OS owns keyboard layouts, mouse packets, HID usages and input policy.
 * Devices are enumerated at boot; attach before boot (no runtime re-enumeration).
 * Up to 8 xHCI controllers, 30 USB interfaces and 2 i8042 ports. CPU 0 callers.
 * One OS input task should pump/read. Queue overflow is counted per device;
 * consumers must release stale state and resynchronize when 'lost' changes. */
#define K_INPUT_MAX_DEVICES 32
#define K_INPUT_REPORT_MAX 1024
#define K_INPUT_DESCRIPTOR_MAX 4096
#define K_INPUT_I8042 1u
#define K_INPUT_USB 2u
#define K_INPUT_ERROR 1u
#define K_INPUT_OFFLINE 2u
typedef struct {
    UINT32 id, transport, controller, port;
    UINT16 vendor, product, max_packet, descriptor_size;
    UINT8 interface_number, interface_class, interface_subclass, interface_protocol, endpoint;
    BOOLEAN online;
} k_input_device;
typedef struct {
    UINT32 device, size, flags, lost;
    UINT64 tick;
    UINT8 bytes[K_INPUT_REPORT_MAX];
} k_input_report;
UINT32 k_input_count(void);
int k_input_get(UINT32 index, k_input_device *);
void k_input_pump(void);
int k_input_read(k_input_report *);
int k_usb_report_descriptor(UINT32 device, void *, UINT32 capacity, UINT32 *size);
int k_usb_input_protocol(UINT32 device, UINT16 protocol);
int k_usb_input_start(UINT32 device);
int k_i8042_enable(UINT32 port);
int k_i8042_exchange(UINT32 port, UINT8 byte, UINT8 *response, UINT32 size);
void kernel_interrupts_init(void);
void kernel_interrupts_enable(void);
__attribute__((noreturn)) void kernel_halt(const char *);

typedef struct {
    volatile UINT32 value;
} k_spinlock;
typedef struct {
    volatile UINT32 owner;
} k_mutex;
typedef struct {
    volatile UINT32 count;
} k_semaphore;
#define K_SPINLOCK_INIT {0}
#define K_MUTEX_INIT {0}
UINT64 k_irq_save(void);
void k_irq_restore(UINT64);
UINT64 k_spin_lock(k_spinlock *);
void k_spin_unlock(k_spinlock *, UINT64);
int k_mutex_lock(k_mutex *);
int k_mutex_trylock(k_mutex *);
int k_mutex_unlock(k_mutex *);
void k_sem_init(k_semaphore *, UINT32);
int k_sem_wait(k_semaphore *);
int k_sem_trywait(k_semaphore *);
void k_sem_post(k_semaphore *);
void k_preempt_disable(void);
void k_preempt_enable(void);

void pmm_seed_from_efi_map(EFI_MEMORY_DESCRIPTOR *, UINTN, UINTN);
UINT64 pmm_alloc_page(void);
void pmm_free_page(UINT64);
UINT64 k_pmm_alloc_pages(UINT32 count, UINT64 max_exclusive);
void k_pmm_free_pages(UINT64, UINT32);
UINT64 kernel_pmm_managed_mib(void);
UINT64 kernel_pmm_free_mib(void);
UINT64 kernel_pmm_used_mib(void);
UINT64 k_pmm_free_count(void);

#define PAGE_PRESENT (1ULL << 0)
#define PAGE_RW (1ULL << 1)
#define PAGE_USER (1ULL << 2)
#define PAGE_PWT (1ULL << 3)
#define PAGE_PCD (1ULL << 4)
#define PAGE_NX (1ULL << 63)
#define K_PAGE_SIZE 4096ULL
#define K_USER_BASE 0x0000400000000000ULL
#define K_USER_LIMIT 0x0000408000000000ULL
#define K_TEST_VA 0xfffffe0000000000ULL
int kernel_vmm_init(void);
UINT64 *kernel_get_pml4(void);
int vmm_map_page(UINT64 *, UINT64, UINT64, UINT64);
int vmm_unmap_page(UINT64 *, UINT64);
int vmm_translate(UINT64 *, UINT64, UINT64 *, UINT64 *);
int k_mmio_map(UINT64 physical, UINT64 bytes);

typedef struct k_address_space k_address_space;
k_address_space *k_as_create(void);
int k_as_destroy(k_address_space *);
int k_as_alloc(k_address_space *, UINT64 bytes, UINT64 alignment, UINT32 rwx, UINT64 *base);
int k_as_check(k_address_space *, UINT64 address, UINT64 bytes, BOOLEAN write);
int k_copy_to_user(k_address_space *, UINT64, const void *, UINT64);
int k_copy_from_user(k_address_space *, void *, UINT64, UINT64);

#define K_MAX_CPUS 32
#define K_MAX_TASKS 128
typedef void (*k_task_entry)(void *);
enum k_task_state {
    K_TASK_UNUSED,
    K_TASK_READY,
    K_TASK_RUNNING,
    K_TASK_SLEEPING,
    K_TASK_BLOCKED,
    K_TASK_ZOMBIE
};
typedef struct {
    UINT32 id, cpu, state, weight, process_id;
    UINT64 runtime_ticks, virtual_runtime, switches;
    INT32 exit_code;
    char name[32];
} k_task_info;
typedef struct {
    UINT32 index, apic_id, online;
    UINT64 ticks, switches, idle_ticks;
} k_cpu_info;
typedef struct {
    UINT32 discovered_cpus, online_cpus;
    BOOLEAN apic, x2apic, ps2, pic, nx, smep, dma_allowed;
    const char *clock_source;
    const char *timer_source;
    const char *dma_reason;
} k_platform_info;
/* rsdp and trampoline (one EFI-reserved page below 1 MiB) may be zero.
 * No AP starts until k_smp_start(). */
int k_platform_init(UINT64 rsdp, UINT64 trampoline);
const k_platform_info *k_platform(void);
int k_scheduler_init(void);
int k_timer_init(UINT32 hz);
UINT64 k_ticks(void);
UINT64 k_uptime_ms(void);
UINT32 k_tick_hz(void);
void k_delay_us(UINT32);
int k_smp_start(void);
UINT32 k_cpu_id(void);
int k_cpu_get(UINT32, k_cpu_info *);
int k_task_create(const char *, k_task_entry, void *, UINT32 weight, UINT32 cpu, UINT32 *id);
UINT32 k_task_id(void);
int k_task_get(UINT32 index, k_task_info *);
int k_task_query(UINT32 id, k_task_info *);
void k_yield(void);
void k_sleep(UINT64 milliseconds);
__attribute__((noreturn)) void k_task_exit(INT32);
int k_task_reap(UINT32 id, INT32 *exit_code);

#define K_IPC_BYTES 128
typedef struct {
    UINT32 sender, size;
    UINT8 bytes[K_IPC_BYTES];
} k_message;
/* Each task has a bounded mailbox. A user process can send to itself or to target tasks explicitly granted by k_ipc_grant. Kernel tasks can send to any live task. Receive is nonblocking. IDs are not reused during a boot. */
int k_ipc_send(UINT32 task, const void *, UINT32);
int k_ipc_receive(k_message *);
int k_ipc_grant(UINT32 process_id, UINT32 target_task);

#define K_MAX_DISKS 24
#define K_PATH_MAX 256
#define K_NAME_MAX 255
typedef struct {
    UINT32 id, sector_size;
    UINT64 sectors;
    BOOLEAN read_only, online;
    char driver[12], model[48], name[20];
} k_disk_info;
int k_storage_init(void);
UINT32 k_disk_count(void);
int k_disk_get(UINT32, k_disk_info *);
int k_disk_read(UINT32 disk, UINT64 lba, UINT32 sectors, void *, UINT64 buffer_bytes);
/* Raw physical writes remain blocked. Filesystem-private native writes are available through k_vfs_put/k_vfs_write/k_vfs_mkdir. This raw API writes RAM disks only, so diagnostics cannot accidentally overwrite a partition table. No physical format, erase, trim or sanitize command is implemented. */
int k_disk_write(UINT32 disk, UINT64 lba, UINT32 sectors, const void *, UINT64 buffer_bytes);
int k_ramdisk_create(UINT32 sectors, UINT32 *id);

typedef struct {
    UINT32 kind, object, cluster, attributes;
    UINT64 size, directory_offset;
} k_file;
typedef struct {
    char name[K_NAME_MAX + 1];
    UINT64 size;
    BOOLEAN directory;
    UINT32 attributes;
    UINT64 directory_offset;
} k_dirent;
typedef void (*k_dir_callback)(const k_dirent *, void *);
/* Storage and namespace policy calls belong on CPU 0. Listing callbacks must not reenter VFS. k_vfs_put creates/replaces a RAM or FAT16/32 file (<=4 MiB).
 * k_vfs_write preserves other bytes; offset=UINT64_MAX means atomic append
 * under the VFS lock. No delete/rename API or executable permission bit is
 * supplied. RIEF validation determines executability; suffixes are irrelevant.
 * FAT16/32 writes require a clean, fully validated filesystem and working
 * native device flush. FAT12 and raw physical disk objects stay read-only.
 * New FAT names use ASCII long-name entries (up to 255 characters). Directory
 * creation is supported. All successful physical mutations flush before return.
 * The first write audits cluster ownership/mirrors and caches up to 64 MiB
 * of FAT, with at most 4096 directories; unsupported/invalid volumes refuse
 * writes. A mutation error freezes that volume read-only until reboot/repair.
 * Copy-on-write ordering and dirty flags do not make FAT power-fail atomic.
 * k_root_bind("name", "/path") provides @name/relative addressing; '..' cannot
 * escape that root. The OS creates /system, /users/pavlkon and their aliases.
 * Discovered FAT volumes appear at /volumes/diskNpM and @diskNpM; @diskN names
 * the first supported FAT volume on that device. Enumeration is not a stable
 * device identity. @usb0 is available only if a caller explicitly binds it;
 * this build has no USB mass-storage driver. */
int k_vfs_init(void);
int k_vfs_mkdir(const char *);
int k_vfs_put(const char *, const void *, UINT64);
int k_vfs_create(const char *, k_file *);
int k_vfs_write(k_file *, UINT64 offset, const void *, UINT64, UINT64 *written);
int k_vfs_sync(void);
int k_vfs_mount_ramfat(UINT32 disk);
int k_vfs_open(const char *, k_file *);
int k_vfs_read(const k_file *, UINT64 offset, void *, UINT64 bytes, UINT64 *read);
int k_vfs_list(const char *, k_dir_callback, void *);
int k_vfs_mount_disks(void);
int k_root_bind(const char *name, const char *absolute_path);
int k_path_resolve(const char *path, const char *cwd, char out[K_PATH_MAX]);
void k_roots_list(void (*callback)(const char *, const char *, void *), void *);
void k_mounts_list(void (*callback)(const char *, const char *, void *), void *);

/* RIEF v1.0: all integers little-endian, fixed wire sizes, reserved fields zero.
 * No on-disk structure requests a virtual or physical address. The loader
 * allocates every region independently, zero-fills memory_size - file_size,
 * applies relocations through supervisor aliases and enforces user W^X.
 *
 * Compiler contract:
 * - Header size >= 96, <= 4096; extension bytes must be zero. Version 1.0,
 *   architecture 1, flags/reserved zero. Tables start at 8-byte file offsets.
 * - Empty tables use count=0, offset=0. All metadata and region file bytes
 *   are bounded and nonoverlapping. Region alignment is a power of two,
 *   <= 1 GiB; the actual allocation has at least 4 KiB alignment.
 * - Every region has R, optional W or X, nonzero memory_size >= file_size.
 *   The entry must be inside file-backed bytes of an executable region.
 * - Relocations are sorted by (patch_region,patch_offset), do not overlap,
 *   and patch bytes entirely inside that region's memory_size.
 *   ABS64 writes base[target_region]+target_offset+addend.
 *   REL32/REL64 write that target minus (patch_address+4/+8), signed.
 *   IMPORT64 uses target_region as the import index, target_offset=0, and
 *   writes binding_address+addend. Symbols are NUL-terminated, 1..127 bytes
 *   of printable non-space ASCII within the string table; exports are unique.
 * - The loader accepts at most 64 regions, 4096 relocations, 64 imports,
 *   64 exports and 64 MiB of page-rounded regions. Reserve space for the
 *   32 KiB user stack within the address space's 64 MiB allocation budget.
 * - Entry receives zeroed GPRs except RSP. RSP is 8 modulo 16, with a zero
 *   return slot and 32 bytes of spare space above it. No arguments, TLS or
 *   C runtime are provided. Exit by syscall; returning jumps to address zero.
 * - Imports require explicit bindings to this process's own mapped user
 *   objects; there is no kernel-symbol import or automatic library search.
 */
#define RIEF_MAGIC 0x46454952u
#define RIEF_ARCH_X86_64 1u
#define RIEF_REGION_R (1u << 0)
#define RIEF_REGION_W (1u << 1)
#define RIEF_REGION_X (1u << 2)
#define RIEF_RELOC_ABS64 1u
#define RIEF_RELOC_REL32 2u
#define RIEF_RELOC_REL64 3u
#define RIEF_RELOC_IMPORT64 4u
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version_major, version_minor;
    uint16_t architecture, flags;
    uint32_t header_size;
    uint32_t region_count, reloc_count, import_count, export_count;
    uint64_t region_table_offset, reloc_table_offset;
    uint64_t import_table_offset, export_table_offset;
    uint64_t string_table_offset, string_table_size;
    uint32_t entry_region, reserved;
    uint64_t entry_offset;
} rief_header_t;
typedef struct {
    uint32_t flags, alignment;
    uint64_t file_offset, file_size, memory_size;
} rief_region_t;
typedef struct {
    uint32_t patch_region, type, target_region, reserved;
    uint64_t patch_offset, target_offset;
    int64_t addend;
} rief_reloc_t;
typedef struct {
    uint32_t name_offset, reserved;
} rief_import_t;
typedef struct {
    uint32_t name_offset, region;
    uint64_t offset;
} rief_export_t;
#pragma pack(pop)
_Static_assert(sizeof(rief_header_t) == 96, "RIEF header ABI");
_Static_assert(sizeof(rief_region_t) == 32, "RIEF region ABI");
_Static_assert(sizeof(rief_reloc_t) == 40, "RIEF relocation ABI");

typedef struct k_process k_process;
typedef struct {
    const char *name;
    UINT64 address;
} k_rief_binding;
typedef struct {
    UINT32 id, task_id, state, fault_vector;
    INT32 exit_code;
    UINT64 fault_address, fault_ip;
    char name[32];
} k_process_info;
/* Load is transactional: no runnable task exists until every check/relocation
 * succeeds. Bindings must point into this process's existing user space. */
k_process *k_process_create(const char *);
int k_rief_load(k_process *, const void *, UINT64, const k_rief_binding *, UINT32);
int k_rief_export(k_process *, const char *, UINT64 *);
int k_process_start(k_process *, UINT32 *task_id);
int k_process_destroy(k_process *);
int k_process_get(UINT32 index, k_process_info *);
k_address_space *k_process_space(k_process *);
UINT32 k_process_id(k_process *);

/* SYSCALL and INT 0x80 share this ABI:
 * RAX=number; RDI,RSI,RDX,R10,R8,R9=args; RAX=result/negative K_E*.
 * SYSCALL clobbers RCX/R11 as defined by x86-64. No pointer reaches a
 * kernel driver without page-by-page validation and a bounded copy.
 * User ABI is freestanding x86-64; x87/SSE state is saved, AVX is disabled.
 * Arguments by call (in the register order above):
 * EXIT(code); WRITE(console fd=1/2 or writable file fd,ptr,len<=4096); YIELD();
 * SLEEP(ms<=86400000); GETPID(); TICKS(); SEND(target_task,ptr,len<=128); RECV(k_message
 * *,capacity>=sizeof(k_message)); OPEN(NUL-terminated absolute-or-named-root path,flags);
 * READ(fd,ptr,len<=4096); MKDIR(path); SYNC(). OPEN flags are K_OPEN_* below.
 * CREATE/TRUNC/APPEND/EXCL require WRITE; EXCL also requires CREATE. CREATE|EXCL never replaces a
 * file. File writes advance the descriptor offset; APPEND chooses EOF atomically. Persistent file
 * mutation is limited to 4 MiB per resulting file. CLOSE(fd). OPEN returns fd >= 3; READ advances
 * its offset and returns byte count (0 at EOF). RECV returns 0 or K_EAGAIN. User raw disk access is
 * denied. Each user process has one thread pinned to CPU 0; syscall pointers are checked over their
 * whole range before any externally visible operation. */
#define K_OPEN_WRITE 1u
#define K_OPEN_CREATE 2u
#define K_OPEN_TRUNC 4u
#define K_OPEN_APPEND 8u
#define K_OPEN_EXCL 16u
/* Flags=0 preserves the original read-only OPEN ABI. */
enum {
    K_SYS_EXIT = 0,
    K_SYS_WRITE = 1,
    K_SYS_YIELD = 2,
    K_SYS_SLEEP = 3,
    K_SYS_GETPID = 4,
    K_SYS_TICKS = 5,
    K_SYS_SEND = 6,
    K_SYS_RECV = 7,
    K_SYS_OPEN = 8,
    K_SYS_READ = 9,
    K_SYS_CLOSE = 10,
    K_SYS_MKDIR = 11,
    K_SYS_SYNC = 12
};
#endif
