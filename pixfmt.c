/*
    Copyright (C) 2010 Mans Rullgard

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

#include <stddef.h>

#include "pixfmt.h"
#include "util.h"

static const struct pixfmt pixfmt_tab[] = {
    {
        .fmt   = PIX_FMT_YUV420P,
        .plane = { 0, 1, 2 },
        .inc   = { 1, 1, 1 },
        .hsub  = { 0, 1, 1 },
        .vsub  = { 0, 1, 1 },
    },
    {
        .fmt   = PIX_FMT_YUYV422,
        .plane = { 0, 0, 0 },
        .start = { 0, 1, 3 },
        .inc   = { 2, 4, 4 },
        .hsub  = { 0, 1, 1 },
        .vsub  = { 0, 0, 0 },
    },
    {
        .fmt   = PIX_FMT_NV12,
        .plane = { 0, 1, 1 },
        .start = { 0, 0, 1 },
        .inc   = { 1, 2, 2 },
        .hsub  = { 0, 1, 1 },
        .vsub  = { 0, 1, 1 },
    },
};

const struct pixfmt *ofbp_get_pixfmt(enum PixelFormat fmt)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(pixfmt_tab); i++)
        if (pixfmt_tab[i].fmt == fmt)
            return &pixfmt_tab[i];

    return NULL;
}

void ofbp_get_plane_offsets(int offs[3], const struct pixfmt *p,
                            int x, int y, const int stride[3])
{
    int i;
    for (i = 0; i < 3; i++)
        offs[i] = (y>>p->vsub[i]) * stride[i] + (x>>p->hsub[i]) * p->inc[i];
}
