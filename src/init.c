/* SPDX-License-Identifier: MPL-2.0 */
#include "sata.h"

#include <exec/resident.h>

#include <exec/memory.h>
#include <libraries/expansion.h>

#define __NOLIBBASE__
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/pcie.h>

extern const struct Resident RomTag;

APTR deviceInit(ULONG seglist asm("a0"), struct ExecBase* SysBase asm("a6"))
{
    (void)seglist;
    Forbid();

    struct SataBase* base = (void*)FindName(&SysBase->DeviceList, (STRPTR)deviceName);

    if (base) {
        Permit();

        return base;
    }

    struct Library* PCIEBase = OpenLibrary("pcie.library", 2);

    if (!PCIEBase) {
        Permit();

        return NULL;
    }

    base = (void*)MakeLibrary((APTR)deviceVectors, NULL, NULL, sizeof(*base), 0);

    if (!base) {
        CloseLibrary(PCIEBase);
        Permit();

        return NULL;
    }

    base->exec_base = SysBase;
    base->pci_base = PCIEBase;
    base->device.dd_Library.lib_Node.ln_Name = (char*)deviceName;
    base->device.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    base->device.dd_Library.lib_Version = 1;
    base->device.dd_Library.lib_IdString = (STRPTR)deviceId;
    base->device.dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    initializeList(&base->controllers);
    initializeList(&base->units);

    struct ExpansionBase* ExpansionBase = (void*)OpenLibrary("expansion.library", 36);

    if (ExpansionBase) {
        struct CurrentBinding binding = {0};

        GetCurrentBinding(&binding, sizeof(binding));
        base->config = binding.cb_ConfigDev;

        if (!base->config || (ULONG)&RomTag - (ULONG)base->config->cd_BoardAddr >= base->config->cd_BoardSize) {
            base->config = NULL;

            while ((base->config = FindConfigDev(base->config, -1, -1))) {
                if ((ULONG)&RomTag - (ULONG)base->config->cd_BoardAddr < base->config->cd_BoardSize) {
                    break;
                }
            }
        }

        CloseLibrary((struct Library*)ExpansionBase);
    }

    AddDevice(&base->device);
    Permit();

    BYTE signal = AllocSignal(-1);

    if (signal < 0) {
        return base;
    }

    struct pci_dev* board = NULL;

    while ((board = pci_find_class(0x010601, board))) {
        struct SataController* controller = AllocMem(sizeof(*controller), MEMF_PUBLIC | MEMF_CLEAR);

        if (!controller) {
            break;
        }

        controller->base = base;
        controller->board = board;
        controller->caller = FindTask(NULL);
        controller->ready_signal = 1UL << signal;
        controller->task.tc_UserData = controller;
        controller->task.tc_Node.ln_Name = (char*)deviceName;
        controller->task.tc_Node.ln_Type = NT_TASK;
        controller->task.tc_Node.ln_Pri = 5;
        controller->task.tc_SPLower = controller->stack;
        controller->task.tc_SPUpper = controller->stack + sizeof(controller->stack);
        controller->task.tc_SPReg = controller->task.tc_SPUpper;
        initializeList(&controller->task.tc_MemEntry);
        AddTail(&base->controllers, &controller->node);
        SetSignal(0, controller->ready_signal);
        AddTask(&controller->task, (APTR)controllerTask, NULL);
        Wait(controller->ready_signal);
    }

    FreeSignal(signal);
    sataLog("[sata] Units: ");
    sataHex(base->unit_count);
    sataLog("\n");

    return base;
}
