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

#ifndef OFBP_FRAME_H
#define OFBP_FRAME_H

#include <stdint.h>
#include <libavutil/pixfmt.h>

struct frame_format {
    unsigned width, height;
    unsigned disp_x, disp_y;
    unsigned disp_w, disp_h;
    unsigned y_stride, uv_stride;
    enum PixelFormat pixfmt;
};

struct frame {
    uint8_t *virt[3];
    uint8_t *phys[3];
    uint8_t *vdata[3];
    uint8_t *pdata[3];
    int linesize[3];
    int x, y;
    int frame_num;
    int pic_num;
    int next;
    int prev;
    int refs;
};

struct pixfmt {
    enum PixelFormat fmt;
    int plane[3];
    int start[3];
    int inc[3];
    int hsub[3];
    int vsub[3];
};

#define MIN_FRAMES 2

struct frame *ofbp_get_frame(void);
void ofbp_put_frame(struct frame *f);
void ofbp_post_frame(struct frame *f);

const struct pixfmt *ofbp_get_pixfmt(enum PixelFormat fmt);
void ofbp_get_plane_offsets(int offs[3], const struct pixfmt *p,
                            int x, int y, const int stride[3]);

#endif
