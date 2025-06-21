# nForce PC Machine

This document describes the nForce PC machine implementation in xemu.

## Overview

The nForce PC machine (`-machine nforcepc`) is a PC-compatible machine that uses the nForce chipset from the Xbox but boots standard PC BIOS (SeaBIOS) instead of Xbox firmware. This provides a standard x86 PC environment with nForce hardware.

## Features

- **Standard PC Boot**: Uses SeaBIOS instead of Xbox BIOS for full PC compatibility
- **nForce Chipset**: Reuses the Xbox nForce implementation for hardware accuracy
- **PC Peripherals**: Enables floppy drives, CD-ROM, and other standard PC peripherals
- **Standard CPU**: Uses Pentium III CPU appropriate for nForce era
- **Network**: Includes nForce network controller (nvnet)
- **USB**: Standard USB controllers without Xbox-specific configurations

## Usage

```bash
qemu-system-i386 -machine nforcepc -bios seabios.bin -cdrom os.iso
```

## Differences from Xbox Machine

| Feature | Xbox Machine | nForce PC Machine |
|---------|--------------|-------------------|
| BIOS | Xbox BIOS | SeaBIOS (PC BIOS) |
| Floppy | Disabled | Enabled |
| CD-ROM | Disabled | Enabled |
| SMBus Devices | Xbox SMC, Video Encoders | None (basic) |
| USB Controllers | 2x 4-port OHCI | 1x 4-port OHCI |
| APU/ACI | Xbox APU/ACI | Not included |
| GPU | NV2A (Xbox GPU) | Not included |

## Implementation

The machine is implemented in `hw/xbox/nforcepc.c` and reuses most of the Xbox infrastructure:

- `xbox_pci_init()`: Sets up the nForce chipset
- `pc_memory_init()`: Standard PC memory initialization with SeaBIOS support
- Standard PC peripherals initialization
- Removes Xbox-specific devices

This provides a good balance between hardware authenticity (real nForce chipset) and PC compatibility.