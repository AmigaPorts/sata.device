/* SPDX-License-Identifier: MPL-2.0 */
#include "ata.h"

static uint16_t readWord(const uint8_t* data, unsigned word)
{
    return (uint16_t)data[word * 2] | (uint16_t)data[word * 2 + 1] << 8;
}

static void writeLong(uint8_t* data, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) {
        data[i] = value >> (8 * i);
    }
}

static void readString(char* destination, const uint8_t* data, unsigned first, unsigned words)
{
    for (unsigned i = 0; i < words * 2; ++i) {
        destination[i] = data[first * 2 + (i ^ 1)];
    }

    unsigned length = words * 2;

    while (length && destination[length - 1] == ' ') {
        --length;
    }

    destination[length] = 0;
}

int identifyAta(const uint8_t data[512], struct AtaIdentity* identity)
{
    if (!data || !identity) {
        return 0;
    }

    // This device implements ATA disks, not the different ATAPI packet protocol.
    uint16_t general = readWord(data, 0), capabilities = readWord(data, 49);

    if ((general & 0x8004) || (capabilities & 0x0300) != 0x0300) {
        return 0;
    }

    // The checksum is optional; its signature makes it mandatory when present.
    if (data[510] == 0xa5) {
        uint8_t sum = 0;

        for (unsigned i = 0; i < 512; ++i) {
            sum += data[i];
        }

        if (sum) {
            return 0;
        }
    }

    struct AtaIdentity result = {0};
    uint16_t commands = readWord(data, 83);
    int commands_valid = (commands & 0xc000) == 0x4000;

    result.lba48 = commands_valid && (commands & 0x0400);

    if (result.lba48) {
        for (unsigned i = 0; i < 4; ++i) {
            result.sectors |= (uint64_t)readWord(data, 100 + i) << (16 * i);
        }

        if (result.sectors > (UINT64_C(1) << 48)) {
            return 0;
        }
    } else {
        result.sectors = readWord(data, 60) | (uint32_t)readWord(data, 61) << 16;

        if (result.sectors > (UINT64_C(1) << 28)) {
            return 0;
        }
    }

    if (!result.sectors) {
        return 0;
    }

    result.sector_bytes = 512;

    uint16_t sector = readWord(data, 106);

    if ((sector & 0xc000) == 0x4000 && (sector & 0x1000)) {
        uint32_t words = readWord(data, 117) | (uint32_t)readWord(data, 118) << 16;

        if (words <= 256 || words > UINT32_MAX / 2) {
            return 0;
        }

        result.sector_bytes = words * 2;
    }

    result.flush_command = commands_valid && (commands & 0x2000) && result.lba48 ? 0xea :
        (commands_valid && (commands & 0x1000) ? 0xe7 : 0);
    result.write_cache = commands_valid && (readWord(data, 82) & 0x20);
    readString(result.serial, data, 10, 10);
    readString(result.firmware, data, 23, 4);
    readString(result.model, data, 27, 20);
    *identity = result;

    return 1;
}

int buildAtaFis(uint8_t fis[20], enum AtaOperation operation,
    const struct AtaIdentity* identity, uint64_t lba, uint32_t sectors)
{
    if (!fis || operation < ATA_IDENTIFY || operation > ATA_FLUSH) {
        return 0;
    }

    uint8_t command;

    if (operation == ATA_IDENTIFY) {
        if (lba || sectors) {
            return 0;
        }

        command = 0xec;
    } else if (operation == ATA_FLUSH) {
        if (!identity || !identity->flush_command || lba || sectors) {
            return 0;
        }

        command = identity->flush_command;
    } else {
        if (!identity || !sectors || lba >= identity->sectors || sectors > identity->sectors - lba) {
            return 0;
        }

        uint64_t limit = UINT64_C(1) << (identity->lba48 ? 48 : 28);

        if (lba >= limit || sectors > limit - lba || sectors > (identity->lba48 ? 65536U : 256U)) {
            return 0;
        }

        command = operation == ATA_READ ? (identity->lba48 ? 0x25 : 0xc8) : (identity->lba48 ? 0x35 : 0xca);
    }

    for (unsigned i = 0; i < 20; ++i) {
        fis[i] = 0;
    }

    fis[0] = 0x27; // Register H2D FIS.
    fis[1] = 0x80; // Command update.
    fis[2] = command;

    if (operation == ATA_READ || operation == ATA_WRITE) {
        fis[4] = lba;
        fis[5] = lba >> 8;
        fis[6] = lba >> 16;
        fis[7] = 0x40;
        fis[12] = sectors;

        if (identity->lba48) {
            fis[8] = lba >> 24;
            fis[9] = lba >> 32;
            fis[10] = lba >> 40;
            fis[13] = sectors >> 8;
        } else {
            fis[7] |= (lba >> 24) & 15;
        }
    }

    return 1;
}

int buildAhciCommand(uint8_t header[32], uint8_t* table, size_t table_bytes,
    uint64_t table_address, int address64, const uint8_t fis[20], int write,
    const struct AhciRegion* regions, unsigned count)
{
    if (!header || !table || !fis || (count && !regions) || count > 65535 ||
        table_bytes < 128 || count > (table_bytes - 128) / 16 || (table_address & 127)) {
        return 0;
    }

    size_t bytes = 128 + 16 * count;
    uint64_t limit = address64 ? UINT64_MAX : UINT32_MAX;

    if (table_address > limit || bytes - 1 > limit - table_address) {
        return 0;
    }

    uint64_t total = 0;

    for (unsigned i = 0; i < count; ++i) {
        const struct AhciRegion* region = &regions[i];

        if (!region->bytes || region->bytes > 0x400000 || (region->bytes & 1) || (region->address & 1) ||
            region->address > limit || region->bytes - 1 > limit - region->address) {
            return 0;
        }

        total += region->bytes;

        if (total > UINT32_MAX) {
            return 0;
        } // PRDBC is 32-bit.
    }

    for (unsigned i = 0; i < 32; ++i) {
        header[i] = 0;
    }

    for (size_t i = 0; i < bytes; ++i) {
        table[i] = 0;
    }

    for (unsigned i = 0; i < 20; ++i) {
        table[i] = fis[i];
    }

    writeLong(header, 5 | (write ? 0x40 : 0) | (uint32_t)count << 16);
    writeLong(header + 8, table_address);
    writeLong(header + 12, table_address >> 32);

    for (unsigned i = 0; i < count; ++i) {
        uint8_t* entry = table + 128 + 16 * i;

        writeLong(entry, regions[i].address);
        writeLong(entry + 4, regions[i].address >> 32);
        writeLong(entry + 12, regions[i].bytes - 1);
    }

    return 1;
}
