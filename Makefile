# x86-64 UEFI kernel, drivers, OS shell, and host RIEF compiler.
ARCH        ?= x86_64
ifneq ($(ARCH),x86_64)
$(error No kernel backend for ARCH=$(ARCH) yet)
endif
INCLUDE_DIR ?= /usr/include/efi
CROSS       ?= x86_64-w64-mingw32
CC          = $(CROSS)-gcc
HOSTCC      ?= cc

CPPFLAGS += -I$(INCLUDE_DIR) -I$(INCLUDE_DIR)/$(ARCH) -I$(INCLUDE_DIR)/protocol
CFLAGS   ?= -O2
CFLAGS   += -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
            -fno-stack-check -fshort-wchar -Wall -Wextra -Werror
ARCH_CFLAGS = -mno-red-zone -mgeneral-regs-only
LDFLAGS  += -nostdlib -Wl,-dll -Wl,--subsystem,10 -Wl,-e,efi_main

TARGET = BOOTX64.EFI
KERNEL_SRCS = kernel.c console.c memory.c platform.c pci.c storage.c \
              fs.c process.c input.c networking.c
ARCH_SRCS = x86_64.c x86_64_platform.c x86_64_mmu.c
ARCH_ASM = x86_64_entry.S
ARCH_HEADERS = arch.h x86_64.h x86_64_internal.h
OBJS = $(KERNEL_SRCS:.c=.o) $(ARCH_SRCS:.c=.o) $(ARCH_ASM:.S=.o) os.o

all: $(TARGET) rief

$(KERNEL_SRCS:.c=.o) $(ARCH_SRCS:.c=.o): %.o: %.c kernel.h kernel_internal.h $(ARCH_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ARCH_CFLAGS) -c $< -o $@

$(ARCH_ASM:.S=.o): %.o: %.S
	$(CC) $(CPPFLAGS) $(ARCH_CFLAGS) -c $< -o $@

os.o: os.c kernel.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ARCH_CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

rief: rief.c
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror $< -o $@

syntax:
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ARCH_CFLAGS) -fsyntax-only $(KERNEL_SRCS) $(ARCH_SRCS) os.c
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -fsyntax-only rief.c

image: $(TARGET)
	mkdir -p image/EFI/BOOT
	cp $(TARGET) image/EFI/BOOT/BOOTX64.EFI

OVMF_CODE ?= /usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS ?= /usr/share/edk2/x64/OVMF_VARS.4m.fd

OVMF_VARS.fd:
	cp $(OVMF_VARS) $@

run: image OVMF_VARS.fd
	qemu-system-x86_64 \
		-machine q35,i8042=off,accel=tcg -m 512M -vga std \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=OVMF_VARS.fd \
		-drive format=raw,file=fat:rw:image -serial mon:stdio -net none \
		-device nec-usb-xhci,id=xhci,msi=on,msix=on -device usb-kbd,bus=xhci.0

runps: image OVMF_VARS.fd
	qemu-system-x86_64 \
		-machine q35 -m 256M -vga std \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=OVMF_VARS.fd \
		-drive format=raw,file=fat:rw:image -serial mon:stdio -net none

clean:
	rm -f $(OBJS) $(TARGET) rief
	rm -rf image

.PHONY: all syntax image run runps clean
