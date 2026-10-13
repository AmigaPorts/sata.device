/*
 * Copyright © 2021 Michal Schulz <michal.schulz@gmx.de>
 * RDB, FileSysResource and AddBootNode integration based on brcm-sdhc.device.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. https://mozilla.org/MPL/2.0/
 */
#include "sata.h"
#include "hunk.h"

#include <exec/memory.h>
#include <devices/hardblocks.h>
#include <resources/filesysres.h>
#include <libraries/expansionbase.h>
#include <dos/filehandler.h>

#define __NOLIBBASE__
#include <proto/exec.h>
#include <proto/expansion.h>

struct Rdb {
    struct SataUnit* unit;
    ULONG bytes, low, high, partitions, filesystems;
    UBYTE* block;
};

static BOOL readBlock(struct Rdb* rdb, ULONG number, ULONG type, ULONG minimum)
{
    if (number < rdb->low || number > rdb->high || number >= rdb->unit->block_count) {
        return FALSE;
    }

    ULONG actual;

    if (transferBlocks(rdb->unit, rdb->block, (uint64_t)number * rdb->bytes, rdb->bytes, FALSE, &actual) ||
        actual != rdb->bytes) {
        return FALSE;
    }

    const ULONG* words = (void*)rdb->block;

    if (words[0] != type || words[1] < minimum || words[1] > rdb->bytes / 4) {
        return FALSE;
    }

    ULONG sum = 0;

    for (ULONG i = 0; i < words[1]; ++i) {
        sum += words[i];
    }

    return !sum;
}

static struct FileSysEntry* findFilesystem(struct FileSysResource* resource, ULONG type)
{
    struct FileSysEntry* best = NULL;

    for (struct Node* node = resource->fsr_FileSysEntries.lh_Head; node->ln_Succ; node = node->ln_Succ) {
        struct FileSysEntry* entry = (void*)node;

        if (entry->fse_DosType == type && (!best || entry->fse_Version > best->fse_Version)) {
            best = entry;
        }
    }

    return best;
}

static struct FileSysEntry* loadFilesystem(struct Rdb* rdb, ULONG type)
{
    struct ExecBase* SysBase = rdb->unit->controller->base->exec_base;
    struct FileSysResource* resource = OpenResource(FSRNAME);

    if (!resource) {
        return NULL;
    }

    Forbid();

    struct FileSysEntry* best = findFilesystem(resource, type);

    Permit();

    ULONG header = rdb->filesystems, budget = rdb->high - rdb->low + 1;

    while (header != 0xffffffffUL && budget--) {
        if (!readBlock(rdb, header, IDNAME_FILESYSHEADER, 43)) {
            break;
        }

        struct FileSysHeaderBlock filesystem = *(struct FileSysHeaderBlock*)rdb->block;

        header = filesystem.fhb_Next;

        if (filesystem.fhb_DosType != type || (best && best->fse_Version >= filesystem.fhb_Version)) {
            continue;
        }

        // NDK DeviceNode fields described by the original nine PatchFlags.
        // Reject unknown fields instead of writing beyond DeviceNode.
        if (filesystem.fhb_PatchFlags & ~0x1ffUL) {
            continue;
        }

        ULONG block = filesystem.fhb_SegListBlocks, blocks = 0, length = 0;
        ULONG limit = rdb->high - rdb->low + 1;

        while (block != 0xffffffffUL && blocks < limit) {
            if (!readBlock(rdb, block, IDNAME_LOADSEG, 5)) {
                break;
            }

            ULONG bytes = (((ULONG*)rdb->block)[1] - 5) * 4;

            if (bytes > 0xffffffffUL - length) {
                break;
            }

            length += bytes;
            ++blocks;
            block = ((struct LoadSegBlock*)rdb->block)->lsb_Next;
        }

        if (block != 0xffffffffUL || !length) {
            continue;
        }

        UBYTE* data = AllocMem(length, MEMF_PUBLIC);

        if (!data) {
            continue;
        }

        ULONG offset = 0;

        block = filesystem.fhb_SegListBlocks;

        for (ULONG i = 0; i < blocks; ++i) {
            if (!readBlock(rdb, block, IDNAME_LOADSEG, 5)) {
                break;
            }

            ULONG bytes = (((ULONG*)rdb->block)[1] - 5) * 4;

            if (offset > length || bytes > length - offset) {
                break;
            }

            CopyMem(((struct LoadSegBlock*)rdb->block)->lsb_LoadData, data + offset, bytes);
            offset += bytes;
            block = ((struct LoadSegBlock*)rdb->block)->lsb_Next;
        }

        BPTR segments = offset == length && block == 0xffffffffUL ? loadFilesystemHunk(SysBase, data, length) : 0;
        FreeMem(data, length);

        if (!segments) {
            continue;
        }

        struct FileSysEntry* entry = AllocMem(sizeof(*entry), MEMF_PUBLIC | MEMF_CLEAR);

        if (!entry) {
            freeFilesystemHunk(SysBase, segments);
            continue;
        }

        CopyMem(&filesystem.fhb_DosType, &entry->fse_DosType, 12 * sizeof(ULONG));
        entry->fse_SegList = segments;
        entry->fse_PatchFlags |= 1UL << 7;
        entry->fse_Node.ln_Name = (char*)deviceName;
        Forbid();
        best = findFilesystem(resource, type);

        if (!best || best->fse_Version < entry->fse_Version) {
            AddHead(&resource->fsr_FileSysEntries, &entry->fse_Node);
            best = entry;
        }

        Permit();

        if (best != entry) {
            freeFilesystemHunk(SysBase, segments);
            FreeMem(entry, sizeof(*entry));
        } else {
            sataLog("[sata] RDB filesystem ");
            sataHex(type);
            sataLog(" loaded\n");
        }
    }

    return best;
}

static BOOL nameExists(struct ExpansionBase* expansion, const char* name)
{
    for (struct Node* node = expansion->MountList.lh_Head; node->ln_Succ; node = node->ln_Succ) {
        struct DeviceNode* device = ((struct BootNode*)node)->bn_DeviceNode;

        if (!device || !device->dn_Name) {
            continue;
        }

        const UBYTE* old = (void*)(device->dn_Name << 2);
        unsigned i = 0;

        while (i < old[0] && name[i]) {
            UBYTE a = old[i + 1], b = name[i];

            if (a >= 'a' && a <= 'z') {
                a -= 'a' - 'A';
            }

            if (b >= 'a' && b <= 'z') {
                b -= 'a' - 'A';
            }

            if (a != b) {
                break;
            }

            ++i;
        }

        if (i == old[0] && !name[i]) {
            return TRUE;
        }
    }

    return FALSE;
}

void mountPartitions(struct SataUnit* unit)
{
    struct ExecBase* SysBase = unit->controller->base->exec_base;
    struct ExpansionBase* ExpansionBase = (void*)OpenLibrary("expansion.library", 36);

    if (!ExpansionBase) {
        return;
    }

    struct Rdb rdb = {.unit = unit, .bytes = unit->port->identity.sector_bytes, .low = 0,
        .high = RDB_LOCATION_LIMIT - 1};

    rdb.block = AllocMem(rdb.bytes, MEMF_PUBLIC);

    if (!rdb.block) {
        goto done;
    }

    BOOL found = FALSE;

    for (unsigned i = 0; i < RDB_LOCATION_LIMIT; ++i) {
        if (!readBlock(&rdb, i, IDNAME_RIGIDDISK, 38)) {
            continue;
        }

        const struct RigidDiskBlock* disk = (void*)rdb.block;

        if (disk->rdb_BlockBytes != rdb.bytes || disk->rdb_RDBBlocksHi < disk->rdb_RDBBlocksLo ||
            disk->rdb_RDBBlocksHi == 0xffffffffUL || disk->rdb_RDBBlocksHi >= unit->block_count) {
            continue;
        }

        rdb.low = disk->rdb_RDBBlocksLo;
        rdb.high = disk->rdb_RDBBlocksHi;
        rdb.partitions = disk->rdb_PartitionList;
        rdb.filesystems = disk->rdb_FileSysHeaderList;
        found = TRUE;
        break;
    }

    if (!found) {
        sataLog("[sata] No valid RDB\n");
        goto done;
    }

    ULONG block = rdb.partitions, budget = rdb.high - rdb.low + 1;

    while (block != 0xffffffffUL && budget--) {
        if (!readBlock(&rdb, block, IDNAME_PARTITION, 49)) {
            break;
        }

        struct PartitionBlock partition = *(struct PartitionBlock*)rdb.block;

        block = partition.pb_Next;

        if (partition.pb_Flags & PBFF_NOMOUNT) {
            continue;
        }

        ULONG* environment = partition.pb_Environment;
        ULONG words = environment[DE_TABLESIZE] + 1;

        if (words <= DE_DOSTYPE || words > sizeof(partition.pb_Environment) / sizeof(ULONG) ||
            partition.pb_SummedLongs < offsetof(struct PartitionBlock, pb_Environment) / 4 + words ||
            environment[DE_SECORG] || !environment[DE_NUMHEADS] || !environment[DE_BLKSPERTRACK] ||
            environment[DE_UPPERCYL] < environment[DE_LOWCYL] || !partition.pb_DriveName[0] ||
            partition.pb_DriveName[0] > 31 || environment[DE_SIZEBLOCK] != rdb.bytes / 4) {
            continue;
        }

        uint64_t per_cylinder = (uint64_t)environment[DE_NUMHEADS] * environment[DE_BLKSPERTRACK];

        if ((uint64_t)environment[DE_UPPERCYL] + 1 > unit->block_count / per_cylinder) {
            continue;
        }

        struct FileSysEntry* filesystem = loadFilesystem(&rdb, environment[DE_DOSTYPE]);

        // DOS may have a built-in filesystem. RDB filesystems take precedence
        // only according to FileSysResource's version and PatchFlags contract.
        char name[44];
        unsigned length = partition.pb_DriveName[0];

        for (unsigned i = 0; i < length; ++i) {
            name[i] = partition.pb_DriveName[i + 1];
        }

        name[length] = 0;
        Forbid();

        ULONG suffix = 0;

        while (nameExists(ExpansionBase, name)) {
            char digits[10];
            unsigned count = 0;
            ULONG value = suffix++;

            do {
                digits[count++] = '0' + value % 10;
                value /= 10;
            } while (value);
            name[length] = '.';

            for (unsigned i = 0; i < count; ++i) {
                name[length + 1 + i] = digits[count - 1 - i];
            }

            name[length + 1 + count] = 0;
        }

        ULONG parameters[4 + sizeof(partition.pb_Environment) / sizeof(ULONG)] = {0};

        parameters[0] = (ULONG)name;
        parameters[1] = (ULONG)deviceName;
        parameters[2] = unit->number;

        for (unsigned i = 0; i < words; ++i) {
            parameters[4 + i] = environment[i];
        }

        BOOL bootable = (partition.pb_Flags & PBFF_BOOTABLE) != 0;

        if (!bootable) {
            parameters[4 + DE_BOOTPRI] = -128;
        }

        struct DeviceNode* node = MakeDosNode(parameters);

        if (node) {
            if (filesystem) {
                ULONG flags = filesystem->fse_PatchFlags;
                ULONG* destination = &node->dn_Type;
                const ULONG* source = &filesystem->fse_Type;

                for (unsigned i = 0; i < 9; ++i) {
                    if (flags & (1UL << i)) {
                        destination[i] = source[i];
                    }
                }
            }

            if (AddBootNode((BYTE)parameters[4 + DE_BOOTPRI], 0, node,
                    bootable ? unit->controller->base->config : NULL)) {
                sataLog("[sata] Boot node ");
                sataLog(name);
                sataLog("\n");
            }
        }

        Permit();
    }

done:
    if (rdb.block) {
        FreeMem(rdb.block, rdb.bytes);
    }

    CloseLibrary((struct Library*)ExpansionBase);
}
