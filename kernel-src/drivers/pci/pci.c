// SPDX-License-Identifier: GPL-2.0-only
#include "kernel_internal.h"

/* Never echo status W1C bits while changing PCI command bits. */
UINT32 pci_read_32(UINT8 bus, UINT8 slot, UINT8 func, UINT8 off)
{
    return arch_pci_read32(bus, slot, func, off);
}
UINT32 pci_get(UINT16 bdf, UINT8 off)
{
    return pci_read_32(bdf >> 8, (bdf >> 3) & 31, bdf & 7, off);
}
void pci_put(UINT16 bdf, UINT8 off, UINT32 v)
{
    arch_pci_write32(bdf >> 8, (bdf >> 3) & 31, bdf & 7, off, v);
}
void pci_command(UINT16 bdf, UINT16 add, UINT16 remove)
{
    UINT16 cmd = (UINT16)pci_get(bdf, 4);
    cmd = (cmd | add) & ~remove;
    pci_put(bdf, 4, cmd);
}
UINT64 pci_mbar(UINT16 bdf, UINT8 reg)
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
void decimal_name(char *out, const char *prefix, UINT32 n)
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
