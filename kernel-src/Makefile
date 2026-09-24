# UEFI kernel, subsystem drivers, test OS and host RIEF compiler.
ARCH ?= x86_64
ifneq ($(ARCH),x86_64)
$(error No backend for ARCH=$(ARCH) yet)
endif
SRCROOT := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
BUILD ?= $(SRCROOT)build
INCLUDE_DIR ?= /usr/include/efi
CROSS ?= x86_64-w64-mingw32
CC = $(CROSS)-gcc
HOSTCC ?= cc
CPPFLAGS += -I$(INCLUDE_DIR) -I$(INCLUDE_DIR)/$(ARCH) -I$(INCLUDE_DIR)/protocol
INCLUDES = -I$(SRCROOT)include -I$(SRCROOT)arch/$(ARCH)/include
CFLAGS ?= -O2
CFLAGS += -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector -fno-stack-check \
          -fshort-wchar -Wall -Wextra -Werror
ARCH_CFLAGS = -mno-red-zone -mgeneral-regs-only
LDFLAGS += -nostdlib -Wl,-dll -Wl,--subsystem,10 -Wl,-e,efi_main
SOURCES = $(filter-out %.old.c,$(sort $(wildcard $(SRCROOT)kernel/*.c $(SRCROOT)kernel/mm/*.c \
          $(SRCROOT)drivers/*/*.c $(SRCROOT)fs/*.c $(SRCROOT)arch/$(ARCH)/*.c $(SRCROOT)os/*.c)))
ASSEMBLY = $(filter-out %.old.S,$(wildcard $(SRCROOT)arch/$(ARCH)/*.S $(SRCROOT)os/arch/$(ARCH)/*.S))
OBJECTS = $(patsubst $(SRCROOT)%.c,$(BUILD)/%.o,$(SOURCES)) \
          $(patsubst $(SRCROOT)%.S,$(BUILD)/%.o,$(ASSEMBLY))
TARGET = $(BUILD)/BOOTX64.EFI
all: $(TARGET) $(BUILD)/rief
$(BUILD)/%.o: $(SRCROOT)%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(CFLAGS) $(ARCH_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/%.o: $(SRCROOT)%.S
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(ARCH_CFLAGS) -MMD -MP -c $< -o $@
$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -o $@
$(BUILD)/rief: $(SRCROOT)tools/rief.c
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror $< -o $@
syntax:
	$(CC) $(CPPFLAGS) $(INCLUDES) $(CFLAGS) $(ARCH_CFLAGS) -fsyntax-only $(SOURCES)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -fsyntax-only $(SRCROOT)tools/rief.c
image: $(TARGET)
	mkdir -p $(BUILD)/image/EFI/BOOT
	cp $(TARGET) $(BUILD)/image/EFI/BOOT/BOOTX64.EFI
OVMF_CODE ?= /usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS ?= /usr/share/edk2/x64/OVMF_VARS.4m.fd
$(BUILD)/OVMF_VARS.fd:
	@mkdir -p $(@D)
	cp $(OVMF_VARS) $@
run: image $(BUILD)/OVMF_VARS.fd
	qemu-system-x86_64 \
		-machine q35,i8042=off,accel=tcg -m 512M -vga std \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
		-drive format=raw,file=fat:rw:$(BUILD)/image -serial mon:stdio -net none \
		-device nec-usb-xhci,id=xhci,msi=on,msix=on -device usb-kbd,bus=xhci.0
runps: image $(BUILD)/OVMF_VARS.fd
	qemu-system-x86_64 \
		-machine q35 -m 256M -vga std \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
		-drive format=raw,file=fat:rw:$(BUILD)/image -serial mon:stdio -net none
clean:
	rm -rf $(BUILD)
-include $(OBJECTS:.o=.d)
.PHONY: all syntax image run runps clean
