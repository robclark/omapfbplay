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
static unsigned xv_port;
struct frame_format ffmt;
static struct frame *frames;
static struct {
    XvImage *xvi;
    XShmSegmentInfo xshm;
} *xv_frames;

static int
alloc_buffers(const struct frame_format *ff, unsigned bufsize,
              struct frame **fr, unsigned *nf)
{
    unsigned y_offset;
    unsigned uv_offset;
    unsigned num_frames;
    unsigned frame_size;
    int i;

    ffmt = *ff;

    y_offset = ff->width * ff->disp_y + ff->disp_x;
    uv_offset = ff->width * ff->disp_y / 4 + ff->disp_x / 2;
    frame_size = ff->width * ff->height * 3 / 2;
    num_frames = MAX(bufsize / frame_size, 1);
    bufsize = num_frames * frame_size;

    frames = malloc(num_frames * sizeof(*frames));
    if (!frames)
        goto err;

    xv_frames = malloc(num_frames * sizeof(*xv_frames));
    if (!xv_frames)
        goto err;

    for (i = 0; i < num_frames; i++) {
        XShmSegmentInfo *xshm = &xv_frames[i].xshm;
        XvImage *xvi = XvShmCreateImage(dpy, xv_port, YV12, NULL,
                                        ff->width, ff->height, xshm);

        xshm->shmid = shmget(IPC_PRIVATE, xvi->data_size, IPC_CREAT | 0777);
        xshm->shmaddr = shmat(xshm->shmid, 0, 0);
        xshm->readOnly = False;
        XShmAttach(dpy, xshm);
        shmctl(xshm->shmid, IPC_RMID, NULL);

        xvi->data = xshm->shmaddr;

        xv_frames[i].xvi = xvi;

        frames[i].data[0] = xvi->data + xvi->offsets[0] + y_offset;
        frames[i].data[1] = xvi->data + xvi->offsets[2] + uv_offset;
        frames[i].data[2] = xvi->data + xvi->offsets[1] + uv_offset;
        frames[i].linesize[0] = xvi->pitches[0];
        frames[i].linesize[1] = xvi->pitches[2];
        frames[i].linesize[2] = xvi->pitches[1];
    }

    *fr = frames;
    *nf = num_frames;

    return 0;

err:
    free(frames);
    free(xv_frames);

    return -1;
}

int display_open(const char *name, struct frame_format *ff, unsigned flags,
                 unsigned bufsize, struct frame **fr, unsigned *nframes)
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

    if (alloc_buffers(ff, bufsize, fr, nframes))
        return -1;

    win = XCreateWindow(dpy, RootWindow(dpy, DefaultScreen(dpy)),
			0, 0, ff->disp_w, ff->disp_h, 0, CopyFromParent,
			InputOutput, CopyFromParent, 0, NULL);

    XMapWindow(dpy, win);
    XSync(dpy, False);

    return 0;
}

void display_frame(struct frame *f)
{
    GC gc = DefaultGC(dpy, DefaultScreen(dpy));

    XvShmPutImage(dpy, xv_port, win, gc, xv_frames[f->frame_num].xvi,
                  ffmt.disp_x, ffmt.disp_y, ffmt.disp_w, ffmt.disp_h,
                  0, 0, ffmt.disp_w, ffmt.disp_h, False);

    XFlush(dpy);
}

void display_close(void)
{
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
}
