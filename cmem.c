/*
    Copyright (C) 2009 Mans Rullgard

    Permission is hereby granted, free of charge, to any person
    obtaining a copy of this software and associated documentation
    files (the "Software"), to deal in the Software without
    restriction, including without limitation the rights to use, copy,
    modify, merge, publish, distribute, sublicense, and/or sell copies
    of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be
    included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
    HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <cmem.h>

#include "display.h"
#include "memman.h"
#include "util.h"

static CMEM_AllocParams cma;
static uint8_t *frame_buf;

static int
cmem_alloc_frames(const struct frame_format *ff, unsigned bufsize,
                  struct frame **fr, unsigned *nf)
{
    int buf_w = ff->width, buf_h = ff->height;
    struct frame *frames;
    unsigned frame_offset;
    unsigned num_frames;
    unsigned frame_size;
    int i;

    if (CMEM_init())
        return -1;

    frame_offset = ff->width * ff->disp_y + ff->disp_x;
    frame_size = buf_w * buf_h * 3 / 2;
    num_frames = MAX(bufsize / frame_size, 1);
    bufsize = num_frames * frame_size;

    fprintf(stderr, "CMEM: using %d frame buffers\n", num_frames);

    cma.type      = CMEM_HEAP;
    cma.flags     = CMEM_CACHED;
    cma.alignment = 16;

    frame_buf = CMEM_alloc(bufsize, &cma);

    if (!frame_buf) {
        fprintf(stderr, "Error allocating frame buffers: %d bytes\n", bufsize);
        return -1;
    }

    frames = malloc(num_frames * sizeof(*frames));

    for (i = 0; i < num_frames; i++) {
        uint8_t *p = frame_buf + i * frame_size;

        frames[i].data[0] = p + frame_offset;
        frames[i].data[1] = p + buf_w * buf_h + frame_offset / 2;
        frames[i].data[2] = frames[i].data[1] + buf_w / 2;
        frames[i].linesize[0] = ff->width;
        frames[i].linesize[1] = ff->width;
        frames[i].linesize[2] = ff->width;
    }

    *fr = frames;
    *nf = num_frames;

    return 0;
}

static void
cmem_free_frames(struct frame *frames, unsigned nf)
{
    CMEM_free(frame_buf, &cma);
    frame_buf = NULL;
    CMEM_exit();
}

DRIVER(memman, cmem) = {
    .name         = "cmem",
    .alloc_frames = cmem_alloc_frames,
    .free_frames  = cmem_free_frames,
};
