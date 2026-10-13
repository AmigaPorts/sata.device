/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>

struct AtaIdentity {
    uint64_t sectors;
    uint32_t sector_bytes;
    uint8_t lba48;
    uint8_t flush_command;
    uint8_t write_cache;
    char serial[21];
    char firmware[9];
    char model[41];
};

enum AtaOperation {
    ATA_IDENTIFY,
    ATA_READ,
    ATA_WRITE,
    ATA_FLUSH
};

int identifyAta(const uint8_t data[512], struct AtaIdentity* identity);
int buildAtaFis(uint8_t fis[20], enum AtaOperation operation,
    const struct AtaIdentity* identity, uint64_t lba, uint32_t sectors);

// AHCI 1.3.1 section 4: little-endian DMA structures, independent of host ABI.
// Command tables are 128-byte aligned; each PRD covers at most 4 MiB.
struct AhciRegion {
    uint64_t address;
    uint32_t bytes;
};

int buildAhciCommand(uint8_t header[32], uint8_t* table, size_t table_bytes,
    uint64_t table_address, int address64, const uint8_t fis[20], int write,
    const struct AhciRegion* regions, unsigned count);
