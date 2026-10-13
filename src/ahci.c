/* SPDX-License-Identifier: MPL-2.0 */
// Independent AHCI 1.3.1 transport. PCI access and interrupt ownership are
// exclusively public pcie.library operations; no platform or emulator headers.
#include "ahci.h"

#include <exec/memory.h>
#include <devices/trackdisk.h>
#include <libraries/pcitags.h>
#include <libraries/pci_constants.h>

#define __NOLIBBASE__
#include <proto/exec.h>
#include <proto/pcie.h>

enum {
    HBA_CAP = 0,
    HBA_GHC = 4,
    HBA_IS = 8,
    HBA_PI = 12,
    HBA_VS = 16,
    HBA_CAP2 = 36,
    HBA_BOHC = 40,
    PORT_CLB = 0,
    PORT_CLBU = 4,
    PORT_FB = 8,
    PORT_FBU = 12,
    PORT_IS = 16,
    PORT_IE = 20,
    PORT_CMD = 24,
    PORT_TFD = 32,
    PORT_SIG = 36,
    PORT_SSTS = 40,
    PORT_SCTL = 44,
    PORT_SERR = 48,
    PORT_SACT = 52,
    PORT_CI = 56
};

#define PORT_ERRORS 0x79000010UL
#define PORT_INTERRUPTS (PORT_ERRORS | 0x0040004fUL)

static ULONG readRegister(struct AhciController* controller, UBYTE* registers, ULONG offset)
{
    struct Library* PCIEBase = controller->pci_base;

    return pci_inl(registers + offset);
}

static void writeRegister(struct AhciController* controller, UBYTE* registers, ULONG offset, ULONG value)
{
    struct Library* PCIEBase = controller->pci_base;

    pci_outl(value, registers + offset);
}

static void delayMicros(struct AhciController* controller, ULONG micros)
{
    struct ExecBase* SysBase = controller->exec_base;
    struct timerequest* timer = controller->timer_request;

    timer->tr_node.io_Command = TR_ADDREQUEST;
    timer->tr_time.tv_secs = micros / 1000000;
    timer->tr_time.tv_micro = micros % 1000000;
    DoIO(&timer->tr_node);
}

static BOOL waitRegister(struct AhciController* controller, UBYTE* registers, ULONG offset,
    ULONG mask, ULONG value, ULONG milliseconds)
{
    // Initialization/stop handshakes only. Each retry sleeps through timer.device.
    do {
        if ((readRegister(controller, registers, offset) & mask) == value) {
            return TRUE;
        }

        if (!milliseconds) {
            break;
        }

        delayMicros(controller, 1000);
    } while (milliseconds--);

    return FALSE;
}

static ULONG interruptServer(ULONG irq asm("d0"), struct AhciController* controller asm("a1"),
    struct ExecBase* SysBase asm("a6"))
{
    (void)irq;

    ULONG pending = readRegister(controller, controller->registers, HBA_IS) & controller->implemented;

    if (!pending) {
        return 0;
    }

    for (unsigned i = 0; i < 32; ++i) {
        if (!(pending & (1UL << i))) {
            continue;
        }

        struct AhciPort* port = &controller->ports[i];
        ULONG status = readRegister(controller, port->registers, PORT_IS);

        port->pending |= status;
        writeRegister(controller, port->registers, PORT_IS, status);
    }

    writeRegister(controller, controller->registers, HBA_IS, pending);
    Signal(controller->task, 1UL << controller->signal);

    return 1;
}

static BOOL stopPort(struct AhciPort* port)
{
    struct AhciController* controller = port->controller;

    writeRegister(controller, port->registers, PORT_IE, 0);

    ULONG command = readRegister(controller, port->registers, PORT_CMD);

    writeRegister(controller, port->registers, PORT_CMD, command & ~1UL);

    if (!waitRegister(controller, port->registers, PORT_CMD, 0x8000, 0, 500)) {
        return FALSE;
    }

    command = readRegister(controller, port->registers, PORT_CMD);
    writeRegister(controller, port->registers, PORT_CMD, command & ~0x10UL);

    if (!waitRegister(controller, port->registers, PORT_CMD, 0x4000, 0, 500)) {
        return FALSE;
    }

    port->running = FALSE;

    return TRUE;
}

BOOL closeAhciPort(struct AhciPort* port)
{
    struct AhciController* controller = port->controller;
    struct Library* PCIEBase = controller->pci_base;

    if (!stopPort(port)) {
        return FALSE;
    }

    if (port->allocation) {
        writeRegister(controller, port->registers, PORT_CLB, 0);
        writeRegister(controller, port->registers, PORT_CLBU, 0);
        writeRegister(controller, port->registers, PORT_FB, 0);
        writeRegister(controller, port->registers, PORT_FBU, 0);
        ReleaseDMAMemoryForBoard(controller->board, port->allocation, port->allocation_size);
        port->allocation = NULL;
    }

    return TRUE;
}

BOOL closeAhci(struct AhciController* controller)
{
    struct ExecBase* SysBase = controller->exec_base;
    struct Library* PCIEBase = controller->pci_base;

    if (controller->configured) {
        // Keep ownership and all DMA allocations if any engine cannot stop.
        // Releasing live DMA memory would allow later device writes into Exec.
        for (unsigned i = 0; i < 32; ++i) {
            if ((controller->implemented & (1UL << i)) && !stopPort(&controller->ports[i])) {
                return FALSE;
            }
        }

        writeRegister(controller, controller->registers, HBA_GHC,
            readRegister(controller, controller->registers, HBA_GHC) & ~2UL);

        for (unsigned i = 0; i < 32; ++i) {
            if ((controller->implemented & (1UL << i)) && !closeAhciPort(&controller->ports[i])) {
                return FALSE;
            }
        }

        controller->configured = FALSE;
    }

    if (controller->server) {
        RemIntVectorServer(controller->board, 0, &controller->interrupt);
    }

    if (controller->vectors) {
        FreeIntVectors(controller->board);
    }

    controller->server = controller->vectors = FALSE;

    if (controller->registers) {
        pci_write_config_word(4, controller->saved_command, controller->board);
        ReleasePCIRegion(controller->board, controller->registers);
        controller->registers = NULL;
    }

    if (controller->claimed) {
        struct TagItem tags[] = {{PRM_BoardOwner, 0}, {TAG_DONE, 0}};

        if (!SetBoardAttrsA(controller->board, tags)) {
            return FALSE;
        }

        controller->claimed = FALSE;
    }

    if (controller->signal >= 0) {
        FreeSignal(controller->signal);
        controller->signal = -1;
    }

    if (controller->timer_request) {
        if (controller->timer_request->tr_node.io_Device) {
            CloseDevice(&controller->timer_request->tr_node);
        }

        DeleteIORequest(&controller->timer_request->tr_node);
        controller->timer_request = NULL;
    }

    if (controller->timer_port) {
        DeleteMsgPort(controller->timer_port);
        controller->timer_port = NULL;
    }

    return TRUE;
}

BOOL openAhci(struct AhciController* controller, struct ExecBase* exec_base,
    struct Library* pci_base, struct pci_dev* board)
{
    struct ExecBase* SysBase = exec_base;
    struct Library* PCIEBase = pci_base;
    UBYTE* clear = (UBYTE*)controller;

    for (ULONG i = 0; i < sizeof(*controller); ++i) {
        clear[i] = 0;
    }

    controller->exec_base = exec_base;
    controller->pci_base = pci_base;
    controller->board = board;
    controller->signal = -1;
    controller->error = "PCI class";
    controller->task = FindTask(NULL);

    if (!board || board->devclass != 0x010601) {
        return FALSE;
    }

    controller->error = "timer.device";
    controller->timer_port = CreateMsgPort();

    if (!controller->timer_port) {
        return FALSE;
    }

    controller->timer_request = (void*)CreateIORequest(controller->timer_port, sizeof(struct timerequest));

    if (!controller->timer_request) {
        goto fail;
    }

    if (OpenDevice(TIMERNAME, UNIT_MICROHZ, &controller->timer_request->tr_node, 0)) {
        controller->timer_request->tr_node.io_Device = NULL;
        goto fail;
    }

    controller->error = "completion signal";
    controller->signal = AllocSignal(-1);

    if (controller->signal < 0) {
        goto fail;
    }

    controller->error = "AHCI BAR5 attributes";

    ULONG address = 0, size = 0, flags = 0;
    struct TagItem properties[] = {{PRM_MemoryAddr5, (ULONG)&address}, {PRM_MemorySize5, (ULONG)&size},
        {PRM_MemoryFlags5, (ULONG)&flags}, {TAG_DONE, 0}};

    if (GetBoardAttrsA(board, properties) != 3 || !address || size < 0x180 || (flags & 1)) {
        goto fail;
    }

    controller->error = "PCI ownership";
    controller->owner.ln_Name = "sata.device";

    struct TagItem ownership[] = {{PRM_BoardOwner, (ULONG)&controller->owner}, {TAG_DONE, 0}};

    if (!SetBoardAttrsA(board, ownership)) {
        goto fail;
    }

    controller->claimed = TRUE;
    controller->error = "PCI region";

    APTR bus = pci_logic_to_physic_addr((APTR)address, board);

    if (!bus) {
        goto fail;
    }

    controller->registers = ObtainPCIRegion(board, (ULONG)bus, size);

    if (!controller->registers) {
        goto fail;
    }

    controller->register_size = size;
    controller->saved_command = pci_read_config_word(4, board);
    pci_write_config_word(4, controller->saved_command | 2, board);
    controller->error = "AHCI version";

    ULONG version = readRegister(controller, controller->registers, HBA_VS);

    if (version == ~0UL || version < 0x10000 || version >= 0x20000) {
        goto fail;
    }

    controller->error = "BIOS ownership handoff";

    // BIOS ownership is optional in AHCI >= 1.2. Do not touch CAP2 on older HBAs.
    if (version >= 0x10200 && (readRegister(controller, controller->registers, HBA_CAP2) & 1)) {
        writeRegister(controller, controller->registers, HBA_BOHC,
            readRegister(controller, controller->registers, HBA_BOHC) | 2);

        if (!waitRegister(controller, controller->registers, HBA_BOHC, 0x11, 0, 2000)) {
            goto fail;
        }
    }

    writeRegister(controller, controller->registers, HBA_GHC, 0x80000000UL);
    controller->configured = TRUE;
    controller->capabilities = readRegister(controller, controller->registers, HBA_CAP);
    controller->error = "port engine stop";

    ULONG implemented = readRegister(controller, controller->registers, HBA_PI);

    for (unsigned i = 0; i < 32; ++i) {
        if (!(implemented & (1UL << i))) {
            continue;
        }

        if (0x100 + (i + 1) * 0x80 > size) {
            goto fail;
        }

        struct AhciPort* port = &controller->ports[i];

        port->controller = controller;
        port->number = i;
        port->registers = controller->registers + 0x100 + i * 0x80;
        controller->implemented |= 1UL << i;

        if (!stopPort(port)) {
            goto fail;
        }

        writeRegister(controller, port->registers, PORT_IS, ~0UL);
    }

    writeRegister(controller, controller->registers, HBA_IS, implemented);
    controller->error = "PCI interrupt allocation";

    if (AllocIntVectors(board, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX) != 1) {
        goto fail;
    }

    controller->vectors = TRUE;
    controller->interrupt.is_Node.ln_Type = NT_INTERRUPT;
    controller->interrupt.is_Node.ln_Name = "sata.device";
    controller->interrupt.is_Data = controller;
    controller->interrupt.is_Code = (void (*)())interruptServer;
    controller->error = "PCI interrupt registration";

    if (AddIntVectorServer(board, 0, &controller->interrupt) != PCIE_OK) {
        goto fail;
    }

    controller->server = TRUE;
    controller->error = "PCI bus mastering";

    if (!pci_set_master(board)) {
        goto fail;
    }

    // AddIntVectorServer activates delivery. MSI per-vector masking is optional;
    // requiring UnmaskIntVector would reject HBAs without that PCI capability.
    writeRegister(controller, controller->registers, HBA_GHC, 0x80000002UL);
    controller->error = NULL;

    return TRUE;
fail:
    closeAhci(controller);

    return FALSE;
}

BOOL openAhciPort(struct AhciPort* port)
{
    struct AhciController* controller = port->controller;
    struct ExecBase* SysBase = controller->exec_base;
    struct Library* PCIEBase = controller->pci_base;

    if (port->allocation || !stopPort(port)) {
        return FALSE;
    }

    ULONG command = readRegister(controller, port->registers, PORT_CMD);

    // Spin-up and power-on according to the controller's capability bits.
    if (controller->capabilities & (1UL << 27)) {
        command |= 2;
    }

    if (command & (1UL << 20)) {
        command |= 4;
    }

    writeRegister(controller, port->registers, PORT_CMD, command);

    if (!waitRegister(controller, port->registers, PORT_SSTS, 15, 3, 1000)) {
        return FALSE;
    }

    if (readRegister(controller, port->registers, PORT_SIG) != 0x00000101) {
        return FALSE;
    }

    if (!waitRegister(controller, port->registers, PORT_TFD, 0x88, 0, 30000)) {
        return FALSE;
    }

    port->allocation_size = AHCI_BUFFER_BYTES + 4096;
    port->allocation = AllocateDMAMemoryForBoard(controller->board, port->allocation_size, 0);

    if (!port->allocation) {
        return FALSE;
    }

    ULONG start = (ULONG)pci_logic_to_physic_addr(port->allocation, controller->board);
    ULONG adjustment = (0UL - start) & 1023;

    if (!start || port->allocation_size - 1 > ~start ||
        (ULONG)pci_logic_to_physic_addr(port->allocation + port->allocation_size - 1, controller->board) !=
            start + port->allocation_size - 1) {
        goto fail;
    }

    port->command_list = port->allocation + adjustment;
    port->dma_address = start + adjustment;
    port->received_fis = port->command_list + 1024;
    port->command_table = port->command_list + 1280;
    port->buffer = port->command_list + 2048;

    for (ULONG i = 0; i < port->allocation_size; ++i) {
        port->allocation[i] = 0;
    }

    ULONG bytes = 2048;

    CachePreDMA(port->command_list, &bytes, 0);
    CachePostDMA(port->command_list, &bytes, 0);

    if (bytes != 2048) {
        goto fail;
    }

    writeRegister(controller, port->registers, PORT_CLB, port->dma_address);
    writeRegister(controller, port->registers, PORT_CLBU, 0);
    writeRegister(controller, port->registers, PORT_FB, port->dma_address + 1024);
    writeRegister(controller, port->registers, PORT_FBU, 0);
    writeRegister(controller, port->registers, PORT_SERR, ~0UL);
    writeRegister(controller, port->registers, PORT_IS, ~0UL);
    writeRegister(controller, controller->registers, HBA_IS, 1UL << port->number);
    writeRegister(controller, port->registers, PORT_IE, PORT_INTERRUPTS);
    writeRegister(controller, port->registers, PORT_CMD, command | 0x10);
    writeRegister(controller, port->registers, PORT_CMD, command | 0x11);
    port->running = TRUE;

    if (commandAhci(port, ATA_IDENTIFY, 0, 0) || !identifyAta(port->buffer, &port->identity) ||
        port->identity.sector_bytes > AHCI_BUFFER_BYTES) {
        goto fail;
    }

    return TRUE;
fail:
    closeAhciPort(port);

    return FALSE;
}

LONG commandAhci(struct AhciPort* port, enum AtaOperation operation, uint64_t lba, ULONG sectors)
{
    struct AhciController* controller = port->controller;
    struct ExecBase* SysBase = controller->exec_base;
    ULONG bytes = operation == ATA_IDENTIFY ? 512 :
        (operation == ATA_FLUSH ? 0 : sectors * port->identity.sector_bytes);

    if (!port->running || port->failed || FindTask(NULL) != controller->task ||
        (operation != ATA_IDENTIFY && operation != ATA_FLUSH &&
            (!port->identity.sector_bytes || sectors > AHCI_BUFFER_BYTES / port->identity.sector_bytes)) ||
        bytes > AHCI_BUFFER_BYTES) {
        return TDERR_NotSpecified;
    }

    uint8_t fis[20];

    if (!buildAtaFis(fis, operation, &port->identity, lba, sectors)) {
        return TDERR_SeekError;
    }

    if (readRegister(controller, port->registers, PORT_CI) ||
        readRegister(controller, port->registers, PORT_SACT)) {
        return TDERR_DriveInUse;
    }

    struct AhciRegion region = {port->dma_address + 2048, bytes};

    if (!buildAhciCommand(port->command_list, port->command_table, 256, port->dma_address + 1280,
            FALSE, fis, operation == ATA_WRITE, &region, bytes ? 1 : 0)) {
        return TDERR_NotSpecified;
    }

    ULONG data_length = bytes, header_length = 32, table_length = 256;
    ULONG cache_flags = operation == ATA_WRITE ? DMA_ReadFromRAM : 0;

    if (bytes) {
        CachePreDMA(port->buffer, &data_length, cache_flags);
    }

    CachePreDMA(port->command_table, &table_length, DMA_ReadFromRAM);
    CachePreDMA(port->command_list, &header_length, 0);

    if (data_length != bytes || table_length != 256 || header_length != 32) {
        if (bytes) {
            CachePostDMA(port->buffer, &data_length, cache_flags);
        }

        CachePostDMA(port->command_list, &header_length, 0);
        CachePostDMA(port->command_table, &table_length, DMA_ReadFromRAM);

        return TDERR_NotSpecified;
    }

    struct timerequest* timer = controller->timer_request;

    timer->tr_node.io_Command = TR_ADDREQUEST;
    timer->tr_time.tv_secs = operation == ATA_FLUSH ? 120 : 30;
    timer->tr_time.tv_micro = 0;
    Disable();
    port->pending = 0;
    SetSignal(0, 1UL << controller->signal);
    writeRegister(controller, port->registers, PORT_IS, ~0UL);
    Enable();
    SendIO(&timer->tr_node);
    writeRegister(controller, port->registers, PORT_CI, 1);

    BOOL timed_out = FALSE;

    for (;;) {
        if (port->pending & PORT_ERRORS) {
            break;
        }

        if (!(readRegister(controller, port->registers, PORT_CI) & 1)) {
            break;
        }

        if (CheckIO(&timer->tr_node)) {
            timed_out = TRUE;
            break;
        }

        Wait((1UL << controller->signal) | (1UL << controller->timer_port->mp_SigBit));
    }

    if (!CheckIO(&timer->tr_node)) {
        AbortIO(&timer->tr_node);
    }

    WaitIO(&timer->tr_node);

    ULONG status = readRegister(controller, port->registers, PORT_TFD);
    BOOL failed = timed_out || (port->pending & PORT_ERRORS) || (status & 0x21);

    if (failed) {
        port->failed = TRUE;

        // Never return or reuse a data buffer while DMA may still be running.
        // On a failed stop, retain the allocation and prevent all later I/O.
        if (!stopPort(port)) {
            return TDERR_NotSpecified;
        }
    }

    if (bytes) {
        CachePostDMA(port->buffer, &data_length, cache_flags);
    }

    CachePostDMA(port->command_list, &header_length, 0);
    CachePostDMA(port->command_table, &table_length, DMA_ReadFromRAM);

    // A cleared slot does not excuse a short transfer. Never report unread
    // bytes from a reused DMA buffer as successful disk data.
    ULONG transferred = (ULONG)port->command_list[4] | (ULONG)port->command_list[5] << 8 |
        (ULONG)port->command_list[6] << 16 | (ULONG)port->command_list[7] << 24;

    if (!failed && transferred != bytes) {
        return TDERR_NotSpecified;
    }

    return failed ? TDERR_NotSpecified : 0;
}
