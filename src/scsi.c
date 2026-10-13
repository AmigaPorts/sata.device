/* SPDX-License-Identifier: MPL-2.0 */
// SCSI-direct for ATA disks. Commands use the same serialized worker and
// transfer path as trackdisk requests; this is not an ATAPI packet transport.
#include "sata.h"

#include <exec/errors.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>

static ULONG readBig(const UBYTE* bytes, unsigned count)
{
    ULONG result = 0;

    while (count--) {
        result = result << 8 | *bytes++;
    }

    return result;
}

static void writeBig(UBYTE* bytes, uint64_t value, unsigned count)
{
    while (count) {
        bytes[--count] = value;
        value >>= 8;
    }
}

static BOOL validBuffer(const void* data, ULONG length)
{
    return !length || (data && length - 1 <= ~((ULONG)data));
}

static void checkCondition(struct IOStdReq* request, UBYTE key, UBYTE asc)
{
    struct SataUnit* unit = (void*)request->io_Unit;
    struct SCSICmd* scsi = request->io_Data;

    for (unsigned i = 0; i < sizeof(unit->sense); ++i) {
        unit->sense[i] = 0;
    }

    unit->sense[0] = 0x70;
    unit->sense[2] = key;
    unit->sense[7] = 10;
    unit->sense[12] = asc;
    scsi->scsi_Status = 2;
    request->io_Error = HFERR_BadStatus;

    if (scsi->scsi_Flags & SCSIF_AUTOSENSE) {
        ULONG count = scsi->scsi_SenseLength;

        if (count > sizeof(unit->sense)) {
            count = sizeof(unit->sense);
        }

        // Old autosense asks for the first four bytes only.
        if ((scsi->scsi_Flags & (1U << SCSIB_OLDAUTOSENSE)) && count > 4) {
            count = 4;
        }

        for (ULONG i = 0; i < count; ++i) {
            scsi->scsi_SenseData[i] = unit->sense[i];
        }

        scsi->scsi_SenseActual = count;
    }
}

static BOOL replyData(struct IOStdReq* request, const UBYTE* data, ULONG size, ULONG allocation)
{
    struct SCSICmd* scsi = request->io_Data;

    if (allocation && !(scsi->scsi_Flags & SCSIF_READ)) {
        request->io_Error = HFERR_Phase;

        return FALSE;
    }

    if (size > allocation) {
        size = allocation;
    }

    if (size > scsi->scsi_Length) {
        size = scsi->scsi_Length;
    }

    for (ULONG i = 0; i < size; ++i) {
        ((UBYTE*)scsi->scsi_Data)[i] = data[i];
    }

    scsi->scsi_Actual = size;

    return TRUE;
}

static void paddedString(UBYTE* output, const char* input, ULONG length)
{
    while (length--) {
        *output++ = *input ? *input++ : ' ';
    }
}

static LONG flushDisk(struct SataUnit* unit)
{
    const struct AtaIdentity* identity = &unit->port->identity;

    if (identity->flush_command) {
        return commandAhci(unit->port, ATA_FLUSH, 0, 0);
    }

    return identity->write_cache ? IOERR_NOCMD : 0;
}

void performScsi(struct IOStdReq* request)
{
    struct SataUnit* unit = (void*)request->io_Unit;
    const struct AtaIdentity* identity = &unit->port->identity;

    if (request->io_Length != sizeof(struct SCSICmd) ||
        !validBuffer(request->io_Data, sizeof(struct SCSICmd))) {
        request->io_Error = IOERR_BADLENGTH;
        return;
    }

    struct SCSICmd* scsi = request->io_Data;

    scsi->scsi_Actual = scsi->scsi_CmdActual = scsi->scsi_SenseActual = 0;
    scsi->scsi_Status = 0;

    if (!scsi->scsi_CmdLength || !validBuffer(scsi->scsi_Command, scsi->scsi_CmdLength) ||
        !validBuffer(scsi->scsi_Data, scsi->scsi_Length) ||
        ((scsi->scsi_Flags & SCSIF_AUTOSENSE) &&
            !validBuffer(scsi->scsi_SenseData, scsi->scsi_SenseLength))) {
        request->io_Error = IOERR_BADADDRESS;
        return;
    }

    const UBYTE* cdb = scsi->scsi_Command;
    static const UBYTE lengths[] = {6, 10, 10, 0, 16, 12, 0, 0};
    ULONG length = lengths[cdb[0] >> 5];

    if (!length || scsi->scsi_CmdLength != length) {
        checkCondition(request, 5, 0x24);
        return;
    }

    scsi->scsi_CmdActual = length;

    // No linked commands, NACA, legacy LUNs or vendor control bits.
    if (cdb[length - 1] || (length == 6 && (cdb[1] & 0xe0))) {
        checkCondition(request, 5, 0x24);
        return;
    }

    UBYTE data[96] = {0};

    switch (cdb[0]) {
        case 0x00: // TEST UNIT READY.
            if (unit->port->failed) {
                checkCondition(request, 2, 4);
            }

            break;
        case 0x03: // REQUEST SENSE; retain pending sense until consumed.
            if (cdb[1] || cdb[2] || cdb[3]) {
                goto invalid;
            }

            if (!unit->sense[0]) {
                unit->sense[0] = 0x70;
                unit->sense[7] = 10;
            }

            if (replyData(request, unit->sense, sizeof(unit->sense), cdb[4])) {
                for (unsigned i = 0; i < sizeof(unit->sense); ++i) {
                    unit->sense[i] = 0;
                }
            }

            break;
        case 0x12: // INQUIRY, including supported pages and ATA unit serial.
            if ((cdb[1] & ~1U) || cdb[3]) {
                goto invalid;
            }

            if (cdb[1] & 1) {
                data[1] = cdb[2];

                if (cdb[2] == 0) {
                    data[3] = 2;
                    data[4] = 0;
                    data[5] = 0x80;
                    replyData(request, data, 6, cdb[4]);
                } else if (cdb[2] == 0x80) {
                    unsigned count = 0;

                    while (identity->serial[count]) {
                        data[4 + count] = identity->serial[count];
                        ++count;
                    }

                    data[3] = count;
                    replyData(request, data, 4 + count, cdb[4]);
                } else {
                    goto invalid;
                }
            } else {
                if (cdb[2]) {
                    goto invalid;
                }

                data[2] = 2;
                data[3] = 2;
                data[4] = 31;
                paddedString(data + 8, "ATA", 8);
                paddedString(data + 16, identity->model, 16);
                paddedString(data + 32, identity->firmware, 4);
                replyData(request, data, 36, cdb[4]);
            }

            break;
        case 0x25: // READ CAPACITY(10).
            if (cdb[1] || cdb[6] || cdb[7] || (cdb[8] & ~1U) ||
                (!(cdb[8] & 1) && readBig(cdb + 2, 4))) {
                goto invalid;
            }

            writeBig(data, unit->block_count - 1 > 0xffffffffUL ? 0xffffffffUL : unit->block_count - 1, 4);
            writeBig(data + 4, identity->sector_bytes, 4);
            replyData(request, data, 8, 8);
            break;
        case 0x9e: // READ CAPACITY(16), no other service actions.
            if (cdb[1] != 0x10 || (cdb[14] & ~1U) ||
                (!(cdb[14] & 1) && (readBig(cdb + 2, 4) || readBig(cdb + 6, 4)))) {
                goto invalid;
            }

            writeBig(data, unit->block_count - 1, 8);
            writeBig(data + 8, identity->sector_bytes, 4);
            replyData(request, data, 32, readBig(cdb + 10, 4));
            break;
        case 0x1a:
        case 0x5a: { // MODE SENSE(6/10), read-only logical geometry pages.
            BOOL ten = cdb[0] == 0x5a;

            if ((cdb[1] & ~8U) || (cdb[2] & 0xc0) || cdb[3]) {
                goto invalid;
            }

            UBYTE page = cdb[2] & 0x3f;

            if (page != 0 && page != 3 && page != 4 && page != 0x3f) {
                goto invalid;
            }

            ULONG count = ten ? 8 : 4;

            if (!(cdb[1] & 8)) {
                data[ten ? 7 : 3] = 8;
                writeBig(data + count + 1, unit->block_count > 0xffffff ? 0xffffff : unit->block_count, 3);
                writeBig(data + count + 5, identity->sector_bytes, 3);
                count += 8;
            }

            // Translate LBA capacity into geometry for partitioning tools. Round
            // cylinders down: a geometry-derived partition must never extend
            // beyond the last sector. Small media still get a nonzero cylinder.
            ULONG sectors = unit->block_count < 63 ? unit->block_count : 63;
            uint64_t tracks = unit->block_count / sectors;
            ULONG heads = tracks < 255 ? tracks : 255;
            uint64_t cylinders = tracks / heads;

            if (page == 3 || page == 0x3f) {
                UBYTE* format = data + count;

                format[0] = 3;
                format[1] = 22;
                writeBig(format + 10, sectors, 2);
                writeBig(format + 12, identity->sector_bytes, 2);
                writeBig(format + 14, 1, 2);
                format[20] = 0x80; // Soft sectored.
                count += 24;
            }

            if (page == 4 || page == 0x3f) {
                UBYTE* geometry = data + count;

                geometry[0] = 4;
                geometry[1] = 22;
                writeBig(geometry + 2, cylinders > 0xffffff ? 0xffffff : cylinders, 3);
                geometry[5] = heads;
                count += 24;
            }

            if (ten) {
                writeBig(data, count - 2, 2);
            } else {
                data[0] = count - 1;
            }

            replyData(request, data, count, ten ? readBig(cdb + 7, 2) : cdb[4]);
            break;
        }

        case 0x08:
        case 0x0a:
        case 0x28:
        case 0x2a:
        case 0xa8:
        case 0xaa:
        case 0x88:
        case 0x8a: {
            BOOL write = (cdb[0] & 2) != 0;
            uint64_t lba;
            ULONG count;

            if (length == 6) {
                lba = readBig(cdb + 1, 3) & 0x1fffff;
                count = cdb[4] ? cdb[4] : 256;
            } else {
                // FUA is supported for writes by completing a real cache flush.
                if (cdb[1] & ~(write ? 8U : 0U)) {
                    goto invalid;
                }

                if ((cdb[1] & 8) && identity->write_cache && !identity->flush_command) {
                    goto invalid;
                }

                if (cdb[length == 16 ? 14 : length == 12 ? 10
                                                         : 6]) {
                    goto invalid;
                }

                lba = readBig(cdb + 2, 4);

                if (length == 16) {
                    lba = lba << 32 | readBig(cdb + 6, 4);
                }

                count = length == 10 ? readBig(cdb + 7, 2) : readBig(cdb + (length == 16 ? 10 : 6), 4);
            }

            if (count && !!(scsi->scsi_Flags & SCSIF_READ) == write) {
                request->io_Error = HFERR_Phase;
                break;
            }

            if (lba > unit->block_count || count > unit->block_count - lba ||
                lba > UINT64_MAX / identity->sector_bytes) {
                checkCondition(request, 5, 0x21);
                break;
            }

            if (count > 0xffffffffUL / identity->sector_bytes ||
                count * identity->sector_bytes > scsi->scsi_Length) {
                request->io_Error = IOERR_BADLENGTH;
                break;
            }

            LONG error = transferBlocks(unit, scsi->scsi_Data, lba * identity->sector_bytes,
                count * identity->sector_bytes, write, &scsi->scsi_Actual);

            if (!error && write && length != 6 && (cdb[1] & 8)) {
                error = flushDisk(unit);
            }

            if (error == IOERR_ABORTED) {
                request->io_Error = error;
            } else if (error) {
                checkCondition(request, 4, 0x44);
            }

            break;
        }

        case 0x35:
        case 0x91: // SYNCHRONIZE CACHE, synchronous only.
            if (cdb[1] || cdb[length == 16 ? 14 : 6]) {
                goto invalid;
            }

            if (flushDisk(unit)) {
                checkCondition(request, 4, 0x44);
            }

            break;
        default:
            checkCondition(request, 5, 0x20);
            break;
    }

    return;
invalid:
    checkCondition(request, 5, 0x24);
}
