/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/types.h>
#include <dos/dos.h>
struct ExecBase;
BPTR loadFilesystemHunk(struct ExecBase* exec_base, const UBYTE* data, ULONG length);
void freeFilesystemHunk(struct ExecBase* exec_base, BPTR segments);
