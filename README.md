A from-scratch x86-64 UEFI kernel.
Currently includes SMP, userspace, scheduling, virtual memory, RIEF executables, IPC, xHCI/i8042 input, NVMe/AHCI/IDE storage, FAT filesystems, and a small reference OS.
Still early.
Build:
make
Run in virtualbox if installed:
./run.sh

Run in qemu if installed:
make run
