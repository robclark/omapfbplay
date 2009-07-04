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
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xvlib.h>

#include "display.h"

#define YV12 0x32315659

static Display *dpy;
static Window win;
static XvImage *xvi;
static unsigned xv_port;
static XShmSegmentInfo xshm;

int display_open(const char *name, unsigned width, unsigned height,
                 unsigned flags)
{
    unsigned ver, rev, rb, evb, erb;
    unsigned na;
    XvAdaptorInfo *xai;
    int i;

    dpy = XOpenDisplay(name);
    if (!dpy) {
        fprintf(stderr, "Xv: error opening display\n");
        return -1;
    }

    if (XvQueryExtension(dpy, &ver, &rev, &rb, &evb, &erb) != Success) {
        fprintf(stderr, "XVideo extension not present\n");
        return -1;
    }

    fprintf(stderr, "XVideo version %u.%u\n", ver, rev);

    if (XvQueryAdaptors(dpy, DefaultRootWindow(dpy), &na, &xai) != Success) {
        fprintf(stderr, "XvQueryAdaptors failed\n");
        return -1;
    }

    fprintf(stderr, "Xv: %i adaptor(s)\n", na);

    for (i = 0, xv_port = 0; i < na && !xv_port; i++) {
        int j;

        fprintf(stderr, "Xv: adaptor %i: \"%s\", %li ports, base %lu\n",
                i, xai[i].name, xai[i].num_ports, xai[i].base_id);

        for (j = 0; j < xai[i].num_ports && !xv_port; j++) {
            XvImageFormatValues *xif;
            int nf, k;

            xif = XvListImageFormats(dpy, xai[i].base_id + j, &nf);
            for (k = 0; k < nf; k++) {
                if (xif[k].id == YV12) {
                    xv_port = xai[i].base_id + j;
                    break;
                }
            }
            XFree(xif);
        }
    }

    XvFreeAdaptorInfo(xai);

    if (!xv_port) {
        fprintf(stderr, "Xv: no suitable port found\n");
        return -1;
    }

    fprintf(stderr, "Xv: using port %i\n", xv_port);

    win = XCreateWindow(dpy, RootWindow(dpy, DefaultScreen(dpy)),
			0, 0, width, height, 0, CopyFromParent,
			InputOutput, CopyFromParent, 0, NULL);

    xvi = XvShmCreateImage(dpy, xv_port, YV12, NULL,
			   width, height, &xshm);

    xshm.shmid = shmget(IPC_PRIVATE, xvi->data_size, IPC_CREAT | 0777);
    xshm.shmaddr = shmat(xshm.shmid, 0, 0);
    xshm.readOnly = False;

    XShmAttach(dpy, &xshm);
    shmctl(xshm.shmid, IPC_RMID, NULL);

    xvi->data = xshm.shmaddr;

    XMapWindow(dpy, win);
    XSync(dpy, False);

    return 0;
}

void display_frame(struct frame *f)
{
    GC gc = DefaultGC(dpy, DefaultScreen(dpy));
    uint8_t *src;
    char *dst;
    int i;

    src = f->data[0];
    dst = xvi->data + xvi->offsets[0];

    for (i = 0; i < xvi->height; i++) {
        memcpy(dst, src, xvi->width);
        src += f->linesize;
        dst += xvi->pitches[0];
    }

    src = f->data[2];
    dst = xvi->data + xvi->offsets[1];

    for (i = 0; i < xvi->height / 2; i++) {
        memcpy(dst, src, xvi->width / 2);
        src += f->linesize;
        dst += xvi->pitches[1];
    }

    src = f->data[1];
    dst = xvi->data + xvi->offsets[2];

    for (i = 0; i < xvi->height / 2; i++) {
        memcpy(dst, src, xvi->width / 2);
        src += f->linesize;
        dst += xvi->pitches[2];
    }

    XvShmPutImage(dpy, xv_port, win, gc, xvi,
                  0, 0, xvi->width, xvi->height,
                  0, 0, xvi->width, xvi->height, False);

    XFlush(dpy);
}

void display_close(void)
{
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
}
