/* SPDX-License-Identifier: MPL-2.0 */
#include "sata.h"

#include <exec/errors.h>

#define __NOLIBBASE__
#include <proto/exec.h>

void beginIO(struct IOStdReq* request asm("a1"), struct SataBase* base asm("a6"))
{
    struct ExecBase* SysBase = base->exec_base;
    struct SataUnit* unit = (void*)request->io_Unit;

    request->io_Error = 0;
    request->io_Flags &= ~IOF_QUICK;
    PutMsg(unit->controller->queue, &request->io_Message);
}

LONG abortIO(struct IOStdReq* request asm("a1"), struct SataBase* base asm("a6"))
{
    struct ExecBase* SysBase = base->exec_base;
    struct SataUnit* unit = (void*)request->io_Unit;
    struct SataController* controller = unit->controller;
    LONG result = IOERR_NOCMD;

    Disable();

    if (controller->active == request) {
        // Finish the current DMA before returning the caller's buffer. Suppress
        // subsequent chunks; never ReplyMsg while hardware still owns memory.
        controller->abort = TRUE;
        result = 0;
    } else {
        for (struct Node* node = controller->queue->mp_MsgList.lh_Head; node->ln_Succ; node = node->ln_Succ) {
            if (node != &request->io_Message.mn_Node) {
                continue;
            }

            Remove(node);
            request->io_Error = IOERR_ABORTED;
            request->io_Actual = 0;
            ReplyMsg(&request->io_Message);
            result = 0;
            break;
        }
    }

    Enable();

    return result;
}
