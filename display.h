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

#ifndef OFB_DISPLAY_H
#define OFB_DISPLAY_H

#include <stdint.h>

#include "util.h"

struct frame_format {
    unsigned width, height;
    unsigned disp_x, disp_y;
    unsigned disp_w, disp_h;
    unsigned y_stride, uv_stride;
};

struct frame {
    uint8_t *data[3];
    uint8_t *phys[3];
    int linesize[3];
    int frame_num;
    int pic_num;
    int next;
    int prev;
    int refs;
};

struct display_props {
    unsigned width, height;
};

struct pixconv {
    const char *name;
    unsigned flags;
    int  (*open)(const struct frame_format *fmt);
    void (*convert)(uint8_t *vdst[3], uint8_t *vsrc[3],
                    uint8_t *pdst[3], uint8_t *psrc[3]);
    void (*finish)(void);
    void (*close)(void);
};

struct display {
    const char *name;
    unsigned flags;
    int  (*open)(const char *name, struct display_props *dp);
    int  (*enable)(struct frame_format *fmt, unsigned flags,
                   const struct pixconv *pc);
    void (*prepare)(struct frame *f);
    void (*show)(struct frame *f);
    void (*close)(void);
    const struct memman *memman;
};

extern const struct display *ofb_display_start[];
extern const struct pixconv *ofb_pixconv_start[];

#define DISPLAY(name) DRIVER(display, name)

#define OFB_FULLSCREEN 1
#define OFB_DOUBLE_BUF 2
#define OFB_PHYS_MEM   4
#define OFB_NOCONV     8

void ofb_scale(unsigned *x, unsigned *y, unsigned *w, unsigned *h,
               unsigned dw, unsigned dh);

void yuv420_to_yuv422(uint8_t *yuv, uint8_t *y, uint8_t *u, uint8_t *v,
                      int w, int h, int yw, int cw, int dw);

#endif /* OFB_DISPLAY_H */
