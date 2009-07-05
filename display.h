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

struct frame_format {
    unsigned width, height;
    unsigned disp_x, disp_y;
    unsigned disp_w, disp_h;
};

struct frame {
    uint8_t *data[3];
    int linesize[3];
    int frame_num;
    int pic_num;
    int next;
    int prev;
    int refs;
};

struct display {
    const char *name;
    int  (*open)(const char *name, struct frame_format *fmt, unsigned flags,
                 unsigned max_mem, struct frame **frames, unsigned *nframes);
    void (*show)(struct frame *f);
    void (*close)(void);
};

extern const struct display *ofb_display_start[], *ofb_display_end[];

#define DISPLAY(name)                                                   \
    static const struct display ofb_display_##name;                     \
    static const struct display *ofb_display_##name_p                   \
    __attribute__((section(".ofb_display"), used)) = &ofb_display_##name; \
    static const struct display ofb_display_##name

#define ALIGN(n, a) (((n)+((a)-1))&~((a)-1))
#define MIN(a, b) ((a) < (b)? (a): (b))
#define MAX(a, b) ((a) > (b)? (a): (b))

#define OFB_FULLSCREEN 1
#define OFB_DOUBLE_BUF 2

void yuv420_to_yuv422(uint8_t *yuv, uint8_t *y, uint8_t *u, uint8_t *v,
                      int w, int h, int yw, int cw, int dw);

#endif /* OFB_DISPLAY_H */
