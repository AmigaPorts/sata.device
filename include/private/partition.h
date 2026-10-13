/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Logical sectors of the parent disk. The caller owns one sector of scratch
// space; callbacks must not retain it. No partition table writes are performed.
struct PartitionDisk {
    uint64_t blocks;
    uint32_t block_bytes;
    uint8_t* buffer;
    void* context;
    bool (*read)(void* context, uint64_t block, void* buffer);
    bool (*add)(void* context, uint64_t start, uint64_t count);
};

bool findAmigaPartitions(struct PartitionDisk* disk);
