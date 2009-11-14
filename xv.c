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
#include "util.h"
#include "memman.h"

#define YV12 0x32315659

static Display *dpy;
static Window win;
static XvPortID xv_port;
struct frame_format ffmt;
static unsigned num_frames;
static struct frame *frames;
static struct {
    XvImage *xvi;
    XShmSegmentInfo xshm;
} *xv_frames;
static unsigned out_x, out_y, out_w, out_h;

static int
xv_alloc_frames(struct frame_format *ff, unsigned bufsize,
                struct frame **fr, unsigned *nf)
{
    unsigned y_offset;
    unsigned uv_offset;
    unsigned frame_size;
    int i;

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

static void
set_fullscreen(void)
{
    Atom supporting;
    int netwm = 0;

    supporting = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", True);

    if (supporting != None) {
        Atom xa_window = XInternAtom(dpy, "WINDOW", True);
        unsigned long count, bytes_remain;
        unsigned char *p = NULL, *p2 = NULL;
        int r, r_format;
        Atom r_type;

        r = XGetWindowProperty(dpy, DefaultRootWindow(dpy),
                               supporting, 0, 1, False, xa_window,
                               &r_type, &r_format, &count, &bytes_remain, &p);

        if (r == Success && p && r_type == xa_window && r_format == 32 &&
            count == 1) {
            Window w = *(Window *)p;

            r = XGetWindowProperty(dpy, w, supporting, 0, 1,
                                   False, xa_window, &r_type, &r_format,
                                   &count, &bytes_remain, &p2);

            if(r == Success && p2 && *p2 == *p && r_type == xa_window &&
               r_format == 32 && count == 1){
                netwm = 1;
            }
        }

        if (p)  XFree(p);
        if (p2) XFree(p2);
    }

    if (netwm) {
        Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
        Atom wm_fs = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
        XEvent xev = {};

        xev.type = ClientMessage;
        xev.xclient.window = win;
        xev.xclient.message_type = wm_state;
        xev.xclient.format = 32;
        xev.xclient.data.l[0] = 1;
        xev.xclient.data.l[1] = wm_fs;
        xev.xclient.data.l[2] = 0;

        XSendEvent(dpy, RootWindow(dpy, DefaultScreen(dpy)),
                   False, SubstructureNotifyMask, &xev);
    }
}

static int xv_open(const char *name, struct display_props *dp)
{
    unsigned ver, rev, rb, evb, erb;
    unsigned na;
    XvAdaptorInfo *xai;
    XWindowAttributes attr;
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
                XvPortID port = xai[i].base_id + j;
                if (xif[k].id == YV12 &&
                    XvGrabPort(dpy, port, CurrentTime) == Success) {
                    xv_port = port;
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

    fprintf(stderr, "Xv: using port %li\n", xv_port);

    XGetWindowAttributes(dpy, RootWindow(dpy, DefaultScreen(dpy)), &attr);
    dp->width  = attr.width;
    dp->height = attr.height;

    return 0;
}

static int xv_enable(struct frame_format *ff, unsigned flags)
{
    win = XCreateWindow(dpy, RootWindow(dpy, DefaultScreen(dpy)),
			0, 0, ff->disp_w, ff->disp_h, 0, CopyFromParent,
			InputOutput, CopyFromParent, 0, NULL);
    XSelectInput(dpy, win, StructureNotifyMask);
    XSetWindowBackground(dpy, win, 0);

    ffmt = *ff;

    out_x = 0;
    out_y = 0;
    out_w = ff->disp_w;
    out_h = ff->disp_h;

    XMapWindow(dpy, win);

    if (flags & OFB_FULLSCREEN)
        set_fullscreen();

    return 0;
}

static void xv_prepare(struct frame *f)
{
    XEvent xe, cn;

    cn.type = 0;

    while (XCheckMaskEvent(dpy, ~0, &xe))
        if (xe.type == ConfigureNotify)
            cn = xe;

    if (cn.type) {
        XWindowAttributes xwa;
        XGetWindowAttributes(dpy, win, &xwa);
        out_w = ffmt.disp_w;
        out_h = ffmt.disp_h;
        ofb_scale(&out_x, &out_y, &out_w, &out_h, xwa.width, xwa.height);
        XClearWindow(dpy, win);
    }
}

static void xv_show(struct frame *f)
{
    GC gc = DefaultGC(dpy, DefaultScreen(dpy));

    XvShmPutImage(dpy, xv_port, win, gc, xv_frames[f->frame_num].xvi,
                  ffmt.disp_x, ffmt.disp_y, ffmt.disp_w, ffmt.disp_h,
                  out_x, out_y, out_w, out_h, False);

    XSync(dpy, False);
}

static void xv_close(void)
{
    XvUngrabPort(dpy, xv_port, CurrentTime);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
}

static void xv_free_frames(struct frame *frames, unsigned nf)
{
    int i;

    for (i = 0; i < num_frames; i++) {
        XShmDetach(dpy, &xv_frames[i].xshm);
        XFree(xv_frames[i].xvi);
    }

    free(frames);
    free(xv_frames);
}

const struct memman xv_mem = {
    .name = "xv",
    .alloc_frames = xv_alloc_frames,
    .free_frames  = xv_free_frames,
};

DISPLAY(xv) = {
    .name  = "xv",
    .flags = OFB_FULLSCREEN,
    .open  = xv_open,
    .enable  = xv_enable,
    .prepare = xv_prepare,
    .show  = xv_show,
    .close = xv_close,
    .memman = &xv_mem,
};
