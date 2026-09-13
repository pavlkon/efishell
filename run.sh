#!/bin/bash
set -e

make
sha256sum BOOTX64.EFI

rm -f boot.img boot.vdi
mkfs.vfat -C boot.img 65536
mmd -i boot.img ::/EFI
mmd -i boot.img ::/EFI/BOOT
mcopy -i boot.img BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
vboxmanage convertfromraw boot.img boot.vdi --format VDI

vboxmanage storageattach "OSDev" --storagectl "SATA" --port 0 --device 0 --medium none
vboxmanage closemedium disk boot.vdi --delete 2>&1

vboxmanage convertfromraw boot.img boot.vdi --format VDI
vboxmanage internalcommands sethduuid boot.vdi "824f45c2-eeb1-4574-b5ae-45e46a16a7b7"
vboxmanage storageattach "OSDev" --storagectl "SATA" --port 0 --device 0 --type hdd --medium boot.vdi
vboxmanage startvm "OSDev"
