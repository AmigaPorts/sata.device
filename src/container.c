/* SPDX-License-Identifier: MPL-2.0 */
#include "sata.h"
#include "partition.h"

#include <exec/memory.h>

#define __NOLIBBASE__
#include <proto/exec.h>

static bool readSector(void* context, uint64_t block, void* buffer)
{
    struct SataUnit* unit = context;
    ULONG actual, bytes = unit->port->identity.sector_bytes;

    return block <= UINT64_MAX / bytes &&
        !transferBlocks(unit, buffer, block * bytes, bytes, FALSE, &actual) && actual == bytes;
}

static bool addUnit(void* context, uint64_t start, uint64_t count)
{
    struct SataUnit* disk = context;
    struct SataBase* base = disk->controller->base;
    struct ExecBase* SysBase = base->exec_base;

    // Do not publish two overlapping container aliases from a corrupt table.
    for (struct Node* node = base->units.lh_Head; node->ln_Succ; node = node->ln_Succ) {
        struct SataUnit* other = (void*)((UBYTE*)node - offsetof(struct SataUnit, node));

        if (other->port == disk->port && other->start_block &&
            start < other->start_block + other->block_count && other->start_block < start + count) {
            return false;
        }
    }

    struct SataUnit* unit = AllocMem(sizeof(*unit), MEMF_PUBLIC | MEMF_CLEAR);

    if (!unit) {
        return false;
    }

    unit->controller = disk->controller;
    unit->port = disk->port;
    unit->start_block = start;
    unit->block_count = count;
    Forbid();
    unit->number = base->unit_count++;
    AddTail(&base->units, &unit->node);
    Permit();
    sataLog("[sata] Container unit ");
    sataHex(unit->number);
    sataLog(" parent ");
    sataHex(disk->number);
    sataLog(" start ");
    sataHex(start >> 32);
    sataHex(start);
    sataLog(" sectors ");
    sataHex(count >> 32);
    sataHex(count);
    sataLog("\n");
    mountPartitions(unit);

    return true;
}

void findContainerUnits(struct SataUnit* unit)
{
    struct ExecBase* SysBase = unit->controller->base->exec_base;
    ULONG bytes = unit->port->identity.sector_bytes;
    UBYTE* buffer = AllocMem(bytes, MEMF_PUBLIC);

    if (!buffer) {
        sataLog("[sata] No memory for container scan\n");
        return;
    }

    struct PartitionDisk disk = {unit->block_count, bytes, buffer, unit, readSector, addUnit};

    if (!findAmigaPartitions(&disk)) {
        sataLog("[sata] Container scan failed\n");
    }

    FreeMem(buffer, bytes);
}
