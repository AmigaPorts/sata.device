/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/devices.h>
#include <exec/io.h>
#include <libraries/configvars.h>
#include "ahci.h"

struct SataBase;
struct SataController;
struct SataUnit {
    struct Unit unit;
    struct Node node;
    struct SataController* controller;
    struct AhciPort* port;
    ULONG number;
    uint64_t start_block, block_count;
    UBYTE sense[18];
};

struct SataController {
    struct Node node;
    struct SataBase* base;
    struct AhciController ahci;
    struct pci_dev* board;
    struct Task task;
    UBYTE stack[16384];
    struct MsgPort* queue;
    struct IOStdReq* active;
    BOOL abort;
    struct Task* caller;
    ULONG ready_signal;
    struct SataUnit units[32];
};

struct SataBase {
    struct Device device;
    struct ExecBase* exec_base;
    struct Library* pci_base;
    struct ConfigDev* config;
    struct List controllers, units;
    ULONG unit_count;
};

extern const char deviceName[];
extern const char deviceId[];
extern const APTR deviceVectors[];
APTR deviceInit(ULONG seglist asm("a0"), struct ExecBase* SysBase asm("a6"));
void beginIO(struct IOStdReq* request asm("a1"), struct SataBase* base asm("a6"));
LONG abortIO(struct IOStdReq* request asm("a1"), struct SataBase* base asm("a6"));
void controllerTask(void);
void performIO(struct IOStdReq* request);
void performScsi(struct IOStdReq* request);
LONG transferBlocks(struct SataUnit* unit, APTR data, uint64_t offset, ULONG length, BOOL write, ULONG* actual);
void mountPartitions(struct SataUnit* unit);
void findContainerUnits(struct SataUnit* unit);
#ifdef SATA_DEBUG
void sataLog(const char* text);
void sataHex(ULONG value);
#else
static inline void sataLog(const char* text)
{
    (void)text;
}

static inline void sataHex(ULONG value)
{
    (void)value;
}
#endif
static inline void initializeList(struct List* list)
{
    list->lh_Head = (struct Node*)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node*)&list->lh_Head;
}
