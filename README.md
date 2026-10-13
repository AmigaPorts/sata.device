# sata.device
[![CMake](https://github.com/amigaports/sata.device/actions/workflows/cmake.yml/badge.svg)](https://github.com/amigaports/sata.device/actions/workflows/cmake.yml)

AHCI SATA disk driver for AmigaOS 3.x on Emu68. Uses `pcie.library` for PCI
resources, DMA and interrupts, and `timer.device` for command deadlines.

Discovers disks across all AHCI controllers and ports, with RDB boot support,
trackdisk/TD64/NSD I/O and SCSI-direct commands for ATA disks. Amiga containers
in primary MBR (`0x76`) or GPT (`3F82EEBC-87C9-4097-8165-89D6540557C0`)
partitions appear as additional units; the whole disk remains accessible.

Controllers must be in AHCI mode. ATAPI, port multipliers, NCQ and hotplug
recovery are not supported.

## Building

Requires CMake, Git, `xxd`, and an `m68k-amigaos` cross-toolchain with AmigaOS
headers and `sfdc`. From a configured cross-toolchain environment:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DM68K_CPU=custom -DM68K_FPU=custom -DM68K_CRT=custom
cmake --build build --parallel
cmake --build build --target install
```

CPM fetches the pinned build helpers and public PCI SDK dependencies.
Outputs are `build/sata.device` and `build/sata.device.h` for ROM embedding.
Installation places the device in `Devs`. Runtime requires `pcie.library` 2.0
or later and its dependencies. Use `HDToolBox sata.device` to inspect disks.

## License

[Mozilla Public License 2.0](LICENSE). Device and RDB integration derive from
Michal Schulz's [brcm-sdhc.device](https://github.com/michalsc/brcm-sdhc.device/tree/0ad06bc4559ed6f2986b2d77f14afc8941aba3ad).
The ATA/AHCI transport and partition parser are independent implementations.
