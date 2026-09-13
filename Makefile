ARCH        = x86_64
INCLUDE_DIR ?= /usr/include/efi
CROSS       = x86_64-w64-mingw32
CC          = $(CROSS)-gcc

CFLAGS = -I$(INCLUDE_DIR) -I$(INCLUDE_DIR)/$(ARCH) -I$(INCLUDE_DIR)/protocol \
         -ffreestanding -fno-stack-protector -fno-stack-check \
         -fshort-wchar -mno-red-zone -Wall -mgeneral-regs-only

LDFLAGS = -nostdlib -Wl,-dll -Wl,--subsystem,10 -Wl,-e,efi_main

TARGET = BOOTX64.EFI
OBJS   = kernel.o os.o

all: $(TARGET)

kernel.o: kernel.c kernel.h
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o

os.o: os.c kernel.h
	$(CC) $(CFLAGS) -c os.c -o os.o

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $(TARGET)

image: $(TARGET)
	mkdir -p image/EFI/BOOT
	cp $(TARGET) image/EFI/BOOT/BOOTX64.EFI

OVMF_CODE ?= /usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS ?= /usr/share/edk2/x64/OVMF_VARS.4m.fd

run: image
	cp -n $(OVMF_VARS) OVMF_VARS.fd
	qemu-system-x86_64 \
		-machine q35,i8042=off,accel=tcg \
		-m 512M \
		-vga std \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=OVMF_VARS.fd \
		-drive format=raw,file=fat:rw:image \
		-serial mon:stdio \
		-net none \
		-device nec-usb-xhci,id=xhci,msi=on,msix=on \
		-device usb-kbd,bus=xhci.0 \
		-d guest_errors,unimp \
		-trace "usb_xhci_*"

runps: image
	cp -n $(OVMF_VARS) OVMF_VARS.fd
	qemu-system-x86_64 \
		-machine q35 -m 256M \
		-vga std \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=OVMF_VARS.fd \
		-drive format=raw,file=fat:rw:image \
		-serial mon:stdio \
		-net none

clean:
	rm -f *.o $(TARGET)
	rm -rf image

.PHONY: all image run runps clean
