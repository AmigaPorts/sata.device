/*
 * Copyright © 2021 Michal Schulz <michal.schulz@gmx.de>
 * Resident/device organization based on brcm-sdhc.device.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. https://mozilla.org/MPL/2.0/
 */
#include "sata.h"

#include <exec/resident.h>
#include <exec/errors.h>

#define __NOLIBBASE__
#include <proto/exec.h>

extern const UBYTE rom_end;

// A library HUNK is not a Shell application. The linker keeps this entry first.
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void)
{
    return -1;
}

const char deviceName[] = LIBRARY_NAME;
const char deviceId[] = VERSION_STRING;

const struct Resident RomTag __attribute__((used, aligned(2))) = {
    RTC_MATCHWORD,
    (struct Resident*)&RomTag,
    (APTR)&rom_end,
    RTF_COLDSTART,
    1,
    NT_DEVICE,
    20,
    (APTR)deviceName,
    (APTR)deviceId,
    (APTR)deviceInit
};

static LONG deviceOpen(struct IORequest* request asm("a1"), ULONG number asm("d0"),
    ULONG flags asm("d1"), struct SataBase* base asm("a6"))
{
    (void)flags;

    struct ExecBase* SysBase = base->exec_base;

    Forbid();

    struct SataUnit* unit = NULL;

    for (struct Node* node = base->units.lh_Head; node->ln_Succ; node = node->ln_Succ) {
        struct SataUnit* candidate = (void*)((UBYTE*)node - offsetof(struct SataUnit, node));

        if (candidate->number == number) {
            unit = candidate;
            break;
        }
    }

    if (unit && request->io_Message.mn_Length >= sizeof(struct IOStdReq)) {
        request->io_Unit = &unit->unit;
        request->io_Error = 0;
        ++unit->unit.unit_OpenCnt;
        ++base->device.dd_Library.lib_OpenCnt;
    } else {
        request->io_Device = (void*)-1;
        request->io_Unit = (void*)-1;
        request->io_Error = IOERR_OPENFAIL;
    }

    Permit();

    return request->io_Error;
}

static ULONG deviceClose(struct IORequest* request asm("a1"), struct SataBase* base asm("a6"))
{
    struct ExecBase* SysBase = base->exec_base;

    Forbid();
    --request->io_Unit->unit_OpenCnt;
    --base->device.dd_Library.lib_OpenCnt;
    request->io_Unit = (void*)-1;
    request->io_Device = (void*)-1;
    Permit();

    return 0;
}

// Mounted expansion boot devices and their controller tasks persist until reset.
static ULONG deviceExpunge(void)
{
    return 0;
}

static ULONG deviceReserved(void)
{
    return 0;
}

const APTR deviceVectors[] = {(APTR)deviceOpen, (APTR)deviceClose, (APTR)deviceExpunge,
    (APTR)deviceReserved, (APTR)beginIO, (APTR)abortIO, (APTR)-1};
