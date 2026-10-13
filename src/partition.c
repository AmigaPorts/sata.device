/* SPDX-License-Identifier: MPL-2.0 */
// MBR 0x76 follows brcm-sdhc.device's virtual-unit convention. GPT uses the
// Emu68 RDB container GUID documented by michalsc/Emu68 (see README.md).
#include "partition.h"

static uint32_t read32(const uint8_t* data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
        (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint64_t read64(const uint8_t* data)
{
    return read32(data) | (uint64_t)read32(data + 4) << 32;
}

static uint32_t crc32(uint32_t crc, const uint8_t* data, uint32_t length)
{
    while (length--) {
        crc ^= *data++;

        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
        }
    }

    return crc;
}

static bool equalBytes(const uint8_t* left, const uint8_t* right, unsigned count)
{
    while (count--) {
        if (*left++ != *right++) {
            return false;
        }
    }

    return true;
}

struct Gpt {
    uint64_t table, first, last;
    uint32_t count, entry_bytes;
};

static bool readGpt(struct PartitionDisk* disk, uint64_t block, struct Gpt* gpt)
{
    uint8_t* data = disk->buffer;

    if (!disk->read(disk->context, block, data) ||
        !equalBytes(data, (const uint8_t*)"EFI PART", 8) || read32(data + 8) != 0x10000 ||
        read32(data + 20) || read64(data + 24) != block ||
        read64(data + 32) != (block == 1 ? disk->blocks - 1 : 1)) {
        return false;
    }

    uint32_t size = read32(data + 12), expected = read32(data + 16);

    if (size < 92 || size > disk->block_bytes) {
        return false;
    }

    for (unsigned i = 16; i < 20; ++i) {
        data[i] = 0;
    }

    if (~crc32(UINT32_MAX, data, size) != expected) {
        return false;
    }

    gpt->first = read64(data + 40);
    gpt->last = read64(data + 48);
    gpt->table = read64(data + 72);
    gpt->count = read32(data + 80);
    gpt->entry_bytes = read32(data + 84);
    expected = read32(data + 88);

    if (!gpt->count || gpt->entry_bytes < 128 || (gpt->entry_bytes & (gpt->entry_bytes - 1)) ||
        gpt->first < 3 || gpt->last < gpt->first || gpt->last >= disk->blocks - 1 ||
        gpt->table < 2 || gpt->table >= disk->blocks - 1) {
        return false;
    }

    uint64_t bytes = (uint64_t)gpt->count * gpt->entry_bytes;
    uint64_t blocks = bytes / disk->block_bytes + (bytes % disk->block_bytes != 0);

    if (block == 1) {
        if (gpt->table >= gpt->first || blocks > gpt->first - gpt->table) {
            return false;
        }
    } else if (gpt->table <= gpt->last || blocks > block - gpt->table) {
        return false;
    }

    // Validate the entire array before exposing any unit, including entries of
    // other types and any extended entry bytes covered by the GPT checksum.
    uint32_t crc = UINT32_MAX;

    for (uint64_t i = 0; i < blocks; ++i) {
        if (!disk->read(disk->context, gpt->table + i, data)) {
            return false;
        }

        uint32_t count = bytes < disk->block_bytes ? bytes : disk->block_bytes;

        crc = crc32(crc, data, count);
        bytes -= count;
    }

    return ~crc == expected;
}

bool findAmigaPartitions(struct PartitionDisk* disk)
{
    if (!disk || !disk->buffer || !disk->read || !disk->add || disk->blocks < 1 ||
        disk->block_bytes < 512 || (disk->block_bytes & (disk->block_bytes - 1))) {
        return false;
    }

    uint8_t* data = disk->buffer;

    if (!disk->read(disk->context, 0, data)) {
        return false;
    }

    if (data[510] != 0x55 || data[511] != 0xaa) {
        return true;
    }

    bool protective = false;

    for (unsigned i = 0; i < 4; ++i) {
        if (data[446 + 16 * i + 4] == 0xee) {
            protective = true;
        }
    }

    if (!protective) {
        uint32_t starts[4], counts[4];
        unsigned found = 0;

        for (unsigned i = 0; i < 4; ++i) {
            const uint8_t* entry = data + 446 + 16 * i;

            if (entry[4] != 0x76) {
                continue;
            }

            uint32_t start = read32(entry + 8), count = read32(entry + 12);

            if (!start || !count || start >= disk->blocks || count > disk->blocks - start) {
                return false;
            }

            starts[found] = start;
            counts[found++] = count;
        }

        for (unsigned i = 0; i < found; ++i) {
            if (!disk->add(disk->context, starts[i], counts[i])) {
                return false;
            }
        }

        return true;
    }

    struct Gpt gpt;

    if (disk->blocks < 6 || (!readGpt(disk, 1, &gpt) && !readGpt(disk, disk->blocks - 1, &gpt))) {
        return false;
    }

    // GUID fields Data1/Data2/Data3 are little-endian on disk.
    static const uint8_t type[] = {0xbc, 0xee, 0x82, 0x3f, 0xc9, 0x87, 0x97, 0x40,
        0x81, 0x65, 0x89, 0xd6, 0x54, 0x05, 0x57, 0xc0};
    uint64_t cached = UINT64_MAX;

    for (uint32_t i = 0; i < gpt.count; ++i) {
        uint64_t offset = (uint64_t)i * gpt.entry_bytes;
        uint64_t block = gpt.table + offset / disk->block_bytes;

        if (block != cached) {
            if (!disk->read(disk->context, block, data)) {
                return false;
            }

            cached = block;
        }

        const uint8_t* entry = data + offset % disk->block_bytes;

        if (!equalBytes(entry, type, sizeof(type))) {
            continue;
        }

        uint64_t start = read64(entry + 32), last = read64(entry + 40);

        if (start < gpt.first || last < start || last > gpt.last) {
            return false;
        }

        if (!disk->add(disk->context, start, last - start + 1)) {
            return false;
        }
    }

    return true;
}
