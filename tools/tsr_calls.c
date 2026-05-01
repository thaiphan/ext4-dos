#include <stdio.h>
#include <stdint.h>
#include <dos.h>

#define EXT4_DOS_MAGIC_PROBE 0xE4D0u

static const char *name(uint8_t al) {
    switch (al) {
    case 0x00: return "InstallCheck";
    case 0x01: return "RmDir";
    case 0x03: return "MkDir";
    case 0x05: return "ChDir";
    case 0x06: return "Close";
    case 0x07: return "Commit";
    case 0x08: return "Read";
    case 0x09: return "Write";
    case 0x0A: return "Lock";
    case 0x0B: return "Unlock";
    case 0x0C: return "GetDiskSpace";
    case 0x0E: return "SetAttrs";
    case 0x0F: return "GetAttrs";
    case 0x11: return "Rename";
    case 0x13: return "Delete";
    case 0x16: return "Open";
    case 0x17: return "Create";
    case 0x18: return "FindFirst-alt";
    case 0x1B: return "FindFirst";
    case 0x1C: return "FindNext";
    case 0x1D: return "FindClose";
    case 0x21: return "LseekFromEnd";
    case 0x23: return "Qualify";
    case 0x2E: return "ExtOpen";
    default:   return "?";
    }
}

int main(void) {
    union REGS r;
    int al;
    printf("AH=11h subfunction call counts since TSR load:\n");
    for (al = 0; al < 0x40; al++) {
        r.w.ax = 0x11FDu;
        r.w.bx = EXT4_DOS_MAGIC_PROBE;
        r.h.cl = (uint8_t)al;
        r.w.dx = 0xFFFFu;
        int86(0x2F, &r, &r);
        if (r.h.al == 0xFFu && r.w.dx > 0u) {
            printf("  AL=%02x %-15s : %u\n", al, name((uint8_t)al), r.w.dx);
        }
    }
    return 0;
}
