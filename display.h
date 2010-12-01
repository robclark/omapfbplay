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

#ifndef OFBP_DISPLAY_H
#define OFBP_DISPLAY_H

#include <stdint.h>

#include "frame.h"
#include "pixconv.h"
#include "util.h"

struct display {
    const char *name;
    unsigned flags;
    int  (*open)(const char *name, struct frame_format *df,
                 struct frame_format *ff);
    int  (*enable)(struct frame_format *fmt, unsigned flags,
                   const struct pixconv *pc, struct frame_format *df);
    void (*prepare)(struct frame *f);
    void (*show)(struct frame *f);
    void (*close)(void);
    const struct memman *memman;
};

extern const struct display *ofbp_display_start[];

#define DISPLAY(name) DRIVER(display, name)

void ofbp_scale(unsigned *x, unsigned *y, unsigned *w, unsigned *h,
                unsigned dw, unsigned dh);

#endif /* OFBP_DISPLAY_H */
