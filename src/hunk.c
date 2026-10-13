/*
 * Copyright © 2021 Michal Schulz <michal.schulz@gmx.de>
 * RDB filesystem loading based on brcm-sdhc.device's LoadSegBlock design.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. https://mozilla.org/MPL/2.0/
 */
#include "hunk.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/doshunks.h>

#define __NOLIBBASE__
#include <proto/exec.h>

struct Reader {
    const UBYTE* data;
    ULONG length, offset;
    BOOL valid;
};

struct Hunk {
    ULONG* memory;
    ULONG bytes;
};

static ULONG readValue(struct Reader* reader, unsigned bytes)
{
    if (reader->offset > reader->length || bytes > reader->length - reader->offset) {
        reader->valid = FALSE;

        return 0;
    }

    ULONG result = 0;

    while (bytes--) {
        result = result << 8 | reader->data[reader->offset++];
    }

    return result;
}

static BOOL skipWords(struct Reader* reader, ULONG count)
{
    if (reader->offset > reader->length || count > (reader->length - reader->offset) / 4) {
        reader->valid = FALSE;

        return FALSE;
    }

    reader->offset += count * 4;

    return TRUE;
}

void freeFilesystemHunk(struct ExecBase* SysBase, BPTR segments)
{
    while (segments) {
        ULONG* data = (APTR)(segments << 2);

        segments = data[0];
        FreeMem(data - 1, data[-1]);
    }
}

BPTR loadFilesystemHunk(struct ExecBase* SysBase, const UBYTE* data, ULONG length)
{
    struct Reader reader = {data, length, 0, TRUE};

    if (!data || readValue(&reader, 4) != HUNK_HEADER) {
        return 0;
    }

    ULONG size;

    while ((size = readValue(&reader, 4))) {
        if (!skipWords(&reader, size)) {
            return 0;
        }
    }

    ULONG table_size = readValue(&reader, 4);
    ULONG first = readValue(&reader, 4), last = readValue(&reader, 4);

    if (!reader.valid || last < first || last >= table_size || last - first >= length / 4) {
        return 0;
    }

    ULONG count = last - first + 1;

    if (count > 0xffffffffUL / sizeof(struct Hunk)) {
        return 0;
    }

    struct Hunk* hunks = AllocMem(count * sizeof(*hunks), MEMF_PUBLIC | MEMF_CLEAR);

    if (!hunks) {
        return 0;
    }

    BPTR result = 0;

    for (ULONG i = 0; i < count; ++i) {
        ULONG specification = readValue(&reader, 4), flags = MEMF_PUBLIC;

        size = specification & 0x3fffffff;

        if ((specification & 0xc0000000) == 0xc0000000) {
            flags |= readValue(&reader, 4);
        } else if (specification & HUNKF_CHIP) {
            flags |= MEMF_CHIP;
        } else if (specification & HUNKF_FAST) {
            flags |= MEMF_FAST;
        }

        if (!reader.valid || size > (0xffffffffUL - 8) / 4) {
            goto done;
        }

        hunks[i].bytes = size * 4 + 8;
        hunks[i].memory = AllocMem(hunks[i].bytes, flags | MEMF_CLEAR);

        if (!hunks[i].memory) {
            goto done;
        }

        hunks[i].memory[0] = hunks[i].bytes;

        if (i) {
            hunks[i - 1].memory[1] = (ULONG)(hunks[i].memory + 1) >> 2;
        }
    }

    ULONG current = 0;
    BOOL loaded = FALSE;

    while (reader.valid && current < count) {
        ULONG type = readValue(&reader, 4) & 0x3fffffff;

        switch (type) {
            case HUNK_CODE:
            case HUNK_DATA:
            case HUNK_BSS:
                size = readValue(&reader, 4);

                if (!reader.valid || loaded || size > (hunks[current].bytes - 8) / 4) {
                    goto done;
                }

                if (type != HUNK_BSS) {
                    ULONG offset = reader.offset;

                    if (!skipWords(&reader, size)) {
                        goto done;
                    }

                    CopyMem((APTR)(data + offset), hunks[current].memory + 2, size * 4);
                }

                loaded = TRUE;
                break;
            case HUNK_RELOC32:
            case HUNK_RELOC32SHORT:
            case HUNK_DREL32: {
                if (!loaded) {
                    goto done;
                }

                unsigned width = type == HUNK_RELOC32 ? 4 : 2;
                ULONG entries;

                while ((entries = readValue(&reader, width))) {
                    ULONG target = readValue(&reader, width);

                    if (!reader.valid || target < first || target > last ||
                        entries > (reader.length - reader.offset) / width) {
                        goto done;
                    }

                    while (entries--) {
                        ULONG offset = readValue(&reader, width);

                        if (!reader.valid || hunks[current].bytes < 12 || (offset & 1) ||
                            offset > hunks[current].bytes - 12) {
                            goto done;
                        }

                        ULONG* address = (APTR)((UBYTE*)(hunks[current].memory + 2) + offset);
                        *address += (ULONG)(hunks[target - first].memory + 2);
                    }
                }

                if (width == 2 && (reader.offset & 2)) {
                    readValue(&reader, 2);
                }

                break;
            }

            case HUNK_NAME:
            case HUNK_DEBUG:
                size = readValue(&reader, 4);

                if (!skipWords(&reader, size)) {
                    goto done;
                }

                break;
            case HUNK_SYMBOL:
                while ((size = readValue(&reader, 4))) {
                    if (!skipWords(&reader, size) || !skipWords(&reader, 1)) {
                        goto done;
                    }
                }

                break;
            case HUNK_END:
                if (!loaded) {
                    goto done;
                }

                ++current;
                loaded = FALSE;
                break;
            default:
                goto done;
        }
    }

    if (reader.valid && current == count) {
        CacheClearU();
        result = (ULONG)(hunks[0].memory + 1) >> 2;
    }

done:
    if (!result) {
        for (ULONG i = 0; i < count; ++i) {
            if (hunks[i].memory) {
                FreeMem(hunks[i].memory, hunks[i].bytes);
            }
        }
    }

    FreeMem(hunks, count * sizeof(*hunks));

    return result;
}
