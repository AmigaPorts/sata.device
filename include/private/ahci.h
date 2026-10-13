/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <devices/timer.h>
#include <libraries/openpci.h>
#include "ata.h"

#define AHCI_BUFFER_BYTES 65536UL
struct AhciController;
struct AhciPort {
    struct AhciController* controller;
    UBYTE* registers;
    UBYTE* allocation;
    UBYTE* command_list;
    UBYTE* received_fis;
    UBYTE* command_table;
    UBYTE* buffer;
    ULONG allocation_size, dma_address;
    ULONG number;
    volatile ULONG pending;
    BOOL running, failed;
    struct AtaIdentity identity;
};

struct AhciController {
    struct ExecBase* exec_base;
    struct Library* pci_base;
    struct pci_dev* board;
    struct Node owner;
    struct Task* task;
    struct MsgPort* timer_port;
    struct timerequest* timer_request;
    struct Interrupt interrupt;
    BYTE signal;
    const char* error;
    UBYTE* registers;
    ULONG register_size, implemented, capabilities;
    UWORD saved_command;
    BOOL claimed, configured, vectors, server;
    struct AhciPort ports[32];
};

// A controller is owned by one device worker task. Commands sleep on real
// completion interrupts or timer.device deadlines; there is no polling I/O loop.
BOOL openAhci(struct AhciController* controller, struct ExecBase* exec_base,
    struct Library* pci_base, struct pci_dev* board);
BOOL closeAhci(struct AhciController* controller);
BOOL openAhciPort(struct AhciPort* port);
BOOL closeAhciPort(struct AhciPort* port);
LONG commandAhci(struct AhciPort* port, enum AtaOperation operation, uint64_t lba, ULONG sectors);
