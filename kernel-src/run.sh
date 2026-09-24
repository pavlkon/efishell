#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
for tool in make VBoxManage mformat mmd mcopy xorriso truncate; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing $tool (requires VirtualBox, mtools and xorriso)." >&2
        exit 1
    }
done

vm_dir="$project_dir/vbox"
mkdir -p "$vm_dir"
if [ -f "$vm_dir/name" ]; then
    IFS= read -r vm_name < "$vm_dir/name"
else
    vm_name="kernel-$(date +%Y%m%d%H%M%S)-$$"
    VBoxManage createvm --name "$vm_name" --ostype Other_64 --basefolder "$vm_dir" --register
    printf '%s\n' "$vm_name" > "$vm_dir/name"
fi
vm_info=$(VBoxManage showvminfo "$vm_name" --machinereadable)
case "$vm_info" in
    *'VMState="poweroff"'*|*'VMState="aborted"'*) ;;
    *) echo "Shut down $vm_name before rebuilding its boot image." >&2; exit 1 ;;
esac

make -C "$project_dir" BUILD="$project_dir/build" image
stage="$project_dir/build/vbox-iso"
mkdir -p "$stage"
truncate -s 16M "$stage/efiboot.img"
mformat -i "$stage/efiboot.img" -T 32768 -h 64 -s 32 ::
mmd -i "$stage/efiboot.img" ::/EFI ::/EFI/BOOT
mcopy -i "$stage/efiboot.img" "$project_dir/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
xorriso -as mkisofs -quiet -R -J -V KERNEL_BOOT -e efiboot.img -no-emul-boot \
    -o "$project_dir/build/kernel.iso" "$stage"

VBoxManage modifyvm "$vm_name" --firmware efi64 --memory 512 --cpus 2 --ioapic on \
    --vram 64 --graphicscontroller vboxvga --keyboard ps2 --mouse ps2 \
    --nic1 none --boot1 dvd --boot2 none --boot3 none --boot4 none
case "$vm_info" in
    *'storagecontrollername0="Boot"'*) ;;
    *) VBoxManage storagectl "$vm_name" --name Boot --add sata --controller IntelAhci ;;
esac
VBoxManage storageattach "$vm_name" --storagectl Boot --port 0 --device 0 \
    --type dvddrive --medium "$project_dir/build/kernel.iso"
exec VBoxManage startvm "$vm_name" --type gui
