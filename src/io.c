/* SPDX-License-Identifier: MPL-2.0 */
#include "sata.h"

#include <exec/errors.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>
#include <devices/scsidisk.h>

#define __NOLIBBASE__
#include <proto/exec.h>

#ifndef TD_READ64
#define TD_READ64 24
#define TD_WRITE64 25
#define TD_FORMAT64 27
#endif

static const UWORD supported[] = {CMD_READ, CMD_WRITE, CMD_UPDATE, CMD_CLEAR,
    TD_FORMAT, TD_CHANGENUM, TD_CHANGESTATE, TD_GETDRIVETYPE, TD_GETGEOMETRY,
    TD_PROTSTATUS, TD_MOTOR, TD_READ64, TD_WRITE64, TD_FORMAT64,
    NSCMD_DEVICEQUERY, NSCMD_TD_READ64, NSCMD_TD_WRITE64, NSCMD_TD_FORMAT64, HD_SCSICMD, 0};

LONG transferBlocks(struct SataUnit* unit, APTR data, uint64_t offset, ULONG length, BOOL write, ULONG* actual)
{
    struct SataController* controller = unit->controller;
    struct ExecBase* SysBase = controller->base->exec_base;
    struct AhciPort* port = unit->port;
    ULONG sector_bytes = port->identity.sector_bytes;
    *actual = 0;

    if (!sector_bytes || offset % sector_bytes || length % sector_bytes) {
        return IOERR_BADLENGTH;
    }

    uint64_t lba = offset / sector_bytes;
    ULONG count = length / sector_bytes;

    if (lba > unit->block_count || count > unit->block_count - lba) {
        return TDERR_SeekError;
    }

    if (length && (!data || length - 1 > ~((ULONG)data))) {
        return IOERR_BADADDRESS;
    }

    lba += unit->start_block;

    ULONG maximum = AHCI_BUFFER_BYTES / sector_bytes;

    if (!port->identity.lba48 && maximum > 256) {
        maximum = 256;
    }

    while (count) {
        if (controller->abort) {
            return IOERR_ABORTED;
        }

        ULONG chunk = count < maximum ? count : maximum;
        ULONG bytes = chunk * sector_bytes;

        if (write) {
            CopyMem((UBYTE*)data + *actual, port->buffer, bytes);
        }

        LONG error = commandAhci(port, write ? ATA_WRITE : ATA_READ, lba, chunk);

        if (error) {
            return error;
        }

        if (!write) {
            CopyMem(port->buffer, (UBYTE*)data + *actual, bytes);
        }

        *actual += bytes;
        count -= chunk;
        lba += chunk;
    }

    return 0;
}

void performIO(struct IOStdReq* request)
{
    struct SataUnit* unit = (void*)request->io_Unit;
    struct AtaIdentity* identity = &unit->port->identity;
    ULONG high = request->io_Actual;

    request->io_Actual = 0;

    ULONG command = request->io_Command;

    switch (command) {
        case HD_SCSICMD:
            performScsi(request);
            break;
        case CMD_READ:
        case CMD_WRITE:
        case TD_FORMAT:
        case TD_READ64:
        case TD_WRITE64:
        case TD_FORMAT64:
        case NSCMD_TD_READ64:
        case NSCMD_TD_WRITE64:
        case NSCMD_TD_FORMAT64: {
            uint64_t offset = request->io_Offset;

            if (command == TD_READ64 || command == TD_WRITE64 || command == TD_FORMAT64 || command >= 0xc000) {
                offset |= (uint64_t)high << 32;
            }

            BOOL write = command != CMD_READ && command != TD_READ64 && command != NSCMD_TD_READ64;

            request->io_Error = transferBlocks(unit, request->io_Data, offset, request->io_Length,
                write, &request->io_Actual);
            break;
        }

        case CMD_UPDATE:
            if (identity->flush_command) {
                request->io_Error = commandAhci(unit->port, ATA_FLUSH, 0, 0);
            } else if (identity->write_cache) {
                request->io_Error = IOERR_NOCMD;
            }

            break;
        case CMD_CLEAR: // No software write-back cache.
        case TD_CHANGENUM:
        case TD_MOTOR:
        case TD_PROTSTATUS:
            break;
        case TD_CHANGESTATE:
            request->io_Actual = unit->port->failed;
            break;
        case TD_GETDRIVETYPE:
            request->io_Actual = DG_DIRECT_ACCESS;
            break;
        case TD_GETGEOMETRY: {
            if (!request->io_Data || request->io_Length < sizeof(struct DriveGeometry)) {
                request->io_Error = IOERR_BADLENGTH;
                break;
            }

            struct DriveGeometry* geometry = request->io_Data;
            *geometry = (struct DriveGeometry){0};
            geometry->dg_SectorSize = identity->sector_bytes;
            geometry->dg_TotalSectors = unit->block_count > 0xffffffffUL ? 0xffffffffUL : unit->block_count;

            // Linear logical geometry; RDB's partition geometry remains authoritative.
            geometry->dg_Cylinders = geometry->dg_TotalSectors;
            geometry->dg_CylSectors = geometry->dg_Heads = geometry->dg_TrackSectors = 1;
            geometry->dg_DeviceType = DG_DIRECT_ACCESS;
            request->io_Actual = sizeof(*geometry);
            break;
        }

        case NSCMD_DEVICEQUERY: {
            if (!request->io_Data || request->io_Length < sizeof(struct NSDeviceQueryResult)) {
                request->io_Error = IOERR_BADLENGTH;
                break;
            }

            struct NSDeviceQueryResult* result = request->io_Data;

            if (result->nsdqr_DevQueryFormat) {
                request->io_Error = IOERR_NOCMD;
                break;
            }

            result->nsdqr_DevQueryFormat = 0;
            result->nsdqr_SizeAvailable = sizeof(*result);
            result->nsdqr_DeviceType = NSDEVTYPE_TRACKDISK;
            result->nsdqr_DeviceSubType = 0;
            result->nsdqr_SupportedCommands = (UWORD*)supported;
            request->io_Actual = sizeof(*result);
            break;
        }

        default:
            request->io_Error = IOERR_NOCMD;
            break;
    }
}
