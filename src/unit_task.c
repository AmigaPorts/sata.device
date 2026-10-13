/*
 * Copyright © 2021 Michal Schulz <michal.schulz@gmx.de>
 * Task and boot-node organization based on brcm-sdhc.device.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. https://mozilla.org/MPL/2.0/
 */
#include "sata.h"

#include <exec/errors.h>
#include <libraries/pcitags.h>

#define __NOLIBBASE__
#include <proto/exec.h>
#include <proto/pcie.h>

#ifdef SATA_DEBUG
// Exec's existing debugger output vector, also used by brcm-sdhc.device.
// No UART registers or Emu68-private diagnostic address is accessed here.
static void debugCharacter(UBYTE value)
{
    register ULONG data asm("d0") = value;
    register struct ExecBase* exec_base asm("a6") = *(struct ExecBase**)4;

    __asm__ volatile("jsr -516(a6)" : "+r"(data) : "r"(exec_base) : "d1", "a0", "a1", "cc", "memory");
}

void sataLog(const char* text)
{
    while (*text) {
        debugCharacter(*text++);
    }
}

void sataHex(ULONG value)
{
    for (unsigned i = 0; i < 8; ++i) {
        debugCharacter("0123456789abcdef"[(value >> (28 - 4 * i)) & 15]);
    }
}

#endif

void controllerTask(void)
{
    struct ExecBase* SysBase = *(struct ExecBase**)4;
    struct SataController* controller = FindTask(NULL)->tc_UserData;

    controller->queue = CreateMsgPort();

    struct Library* PCIEBase = controller->base->pci_base;
    ULONG bus = 0, slot = 0, function = 0;
    struct TagItem attributes[] = {
        {PRM_BusNumber, (ULONG)&bus}, {PRM_SlotNumber, (ULONG)&slot},
        {PRM_FunctionNumber, (ULONG)&function}, {TAG_DONE, 0}};
    sataLog("[sata] Opening AHCI controller");

    if (GetBoardAttrsA(controller->board, attributes) == 3) {
        sataLog(" PCI ");
        sataHex(bus);
        sataLog(":");
        sataHex(slot);
        sataLog(".");
        sataHex(function);
    }

    sataLog("\n");

    if (controller->queue && openAhci(&controller->ahci, SysBase, controller->base->pci_base, controller->board)) {
        for (unsigned i = 0; i < 32; ++i) {
            if (!(controller->ahci.implemented & (1UL << i))) {
                continue;
            }

            struct AhciPort* port = &controller->ahci.ports[i];

            if (!openAhciPort(port)) {
                continue;
            }

            struct SataUnit* unit = &controller->units[i];

            unit->controller = controller;
            unit->port = port;
            unit->block_count = port->identity.sectors;
            Forbid();
            unit->number = controller->base->unit_count++;
            AddTail(&controller->base->units, &unit->node);
            Permit();
            sataLog("[sata] Unit ");
            sataHex(unit->number);
            sataLog(" port ");
            sataHex(i);
            sataLog(": ");
            sataLog(port->identity.model);
            sataLog(" sectors ");
            sataHex(port->identity.sectors >> 32);
            sataHex(port->identity.sectors);
            sataLog(" bytes/sector ");
            sataHex(port->identity.sector_bytes);
            sataLog("\n");
            mountPartitions(unit);
            findContainerUnits(unit);
        }
    } else {
        sataLog("[sata] Controller unavailable: ");
        sataLog(controller->ahci.error ? controller->ahci.error : "request queue");
        sataLog("\n");
    }

    Signal(controller->caller, controller->ready_signal);
    controller->caller = NULL;

    if (!controller->queue) {
        RemTask(NULL);
        return;
    }

    for (;;) {
        WaitPort(controller->queue);

        for (;;) {
            Disable();

            struct IOStdReq* request = (void*)GetMsg(controller->queue);

            controller->active = request;
            controller->abort = FALSE;
            Enable();

            if (!request) {
                break;
            }

            performIO(request);
            Disable();

            if (controller->abort) {
                request->io_Error = IOERR_ABORTED;
            }

            controller->active = NULL;
            ReplyMsg(&request->io_Message);
            Enable();
        }
    }
}
