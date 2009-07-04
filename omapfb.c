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
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/fb.h>
#include <linux/omapfb.h>

#include "display.h"

static struct fb_var_screeninfo sinfo_p0;
static struct fb_var_screeninfo sinfo;
static struct omapfb_mem_info minfo;
static struct omapfb_plane_info pinfo;

static struct {
    unsigned x;
    unsigned y;
    uint8_t *buf;
} fb_pages[2];

static int dev_fd;
static int fb_page_flip;
static int fb_page;

static uint8_t *frame_buf;
static struct frame *frames;

static int
xioctl(const char *name, int fd, int req, void *param)
{
    int err = ioctl(fd, req, param);

    if (err == -1) {
        perror(name);
        exit(1);
    }

    return err;
}

#define xioctl(fd, req, param) xioctl(#req, fd, req, param)

static int
alloc_buffers(const struct frame_format *ff, unsigned bufsize,
              struct frame **fr, unsigned *nf)
{
    int buf_w = ff->width, buf_h = ff->height;
    unsigned frame_offset;
    unsigned num_frames;
    unsigned frame_size;
    void *fbp;
    int i;

    frame_offset = ff->width * ff->disp_y + ff->disp_x;
    frame_size = buf_w * buf_h * 3 / 2;
    num_frames = MAX(bufsize / frame_size, 1);
    bufsize = num_frames * frame_size;

    fprintf(stderr, "Using %d frame buffers, frame_size=%d\n",
            num_frames, frame_size);

    if (posix_memalign(&fbp, 16, bufsize)) {
        fprintf(stderr, "Error allocating frame buffers: %d bytes\n", bufsize);
        return -1;
    }

    frame_buf = fbp;
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

static int omapfb_open(const char *name, struct frame_format *ff,
                       unsigned flags, unsigned bufsize,
                       struct frame **frames, unsigned *nframes)
{
    int fb = open("/dev/fb0", O_RDWR);
    uint8_t *fbmem;
    int i;

    if (fb == -1) {
        perror("/dev/fb0");
        return -1;
    }

    xioctl(fb, FBIOGET_VSCREENINFO, &sinfo_p0);
    close(fb);

    fb = open("/dev/fb1", O_RDWR);

    if (fb == -1) {
        perror("/dev/fb1");
        return -1;
    }

    xioctl(fb, FBIOGET_VSCREENINFO, &sinfo);
    xioctl(fb, OMAPFB_QUERY_PLANE, &pinfo);
    xioctl(fb, OMAPFB_QUERY_MEM, &minfo);

    fbmem = mmap(NULL, minfo.size, PROT_READ|PROT_WRITE, MAP_SHARED, fb, 0);
    if (fbmem == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    for (i = 0; i < minfo.size / 4; i++)
        ((uint32_t*)fbmem)[i] = 0x80008000;

    sinfo.xres = MIN(sinfo_p0.xres, ff->disp_w) & ~15;
    sinfo.yres = MIN(sinfo_p0.yres, ff->disp_h) & ~15;
    sinfo.xoffset = 0;
    sinfo.yoffset = 0;
    sinfo.nonstd = OMAPFB_COLOR_YUY422;

    fb_pages[0].x = 0;
    fb_pages[0].y = 0;
    fb_pages[0].buf = fbmem;

    if (flags & OFB_DOUBLE_BUF && minfo.size >= sinfo.xres * sinfo.yres * 2) {
        sinfo.xres_virtual = sinfo.xres;
        sinfo.yres_virtual = sinfo.yres * 2;
        fb_pages[1].x = 0;
        fb_pages[1].y = sinfo.yres;
        fb_pages[1].buf = fbmem + sinfo.xres * sinfo.yres * 2;
        fb_page_flip = 1;
    }

    xioctl(fb, FBIOPUT_VSCREENINFO, &sinfo);

    pinfo.enabled = 1;
    if (flags & OFB_FULLSCREEN) {
        pinfo.pos_x = 0;
        pinfo.pos_y = 0;
        pinfo.out_width  = sinfo_p0.xres;
        pinfo.out_height = sinfo_p0.yres;
    } else {
        pinfo.pos_x = sinfo_p0.xres / 2 - sinfo.xres / 2;
        pinfo.pos_y = sinfo_p0.yres / 2 - sinfo.yres / 2;
        pinfo.out_width  = sinfo.xres;
        pinfo.out_height = sinfo.yres;
    }

    if (alloc_buffers(ff, bufsize, frames, nframes)) {
        close(fb);
        return -1;
    }

    ioctl(fb, OMAPFB_SETUP_PLANE, &pinfo);

    dev_fd = fb;

    return 0;
}

static void omapfb_show(struct frame *f)
{
    yuv420_to_yuv422(fb_pages[fb_page].buf,
                     f->data[0], f->data[1], f->data[2],
                     sinfo.xres, sinfo.yres,
                     f->linesize[0], f->linesize[1],
                     2*sinfo.xres_virtual);

    if (fb_page_flip) {
        sinfo.xoffset = fb_pages[fb_page].x;
        sinfo.yoffset = fb_pages[fb_page].y;
        xioctl(dev_fd, FBIOPAN_DISPLAY, &sinfo);
        fb_page ^= fb_page_flip;
    }
}

static void omapfb_close(void)
{
    pinfo.enabled = 0;
    ioctl(dev_fd, OMAPFB_SETUP_PLANE, &pinfo);
    close(dev_fd);

    free(frame_buf);
    free(frames);
}

DISPLAY(omapfb) = {
    .name  = "omapfb",
    .open  = omapfb_open,
    .show  = omapfb_show,
    .close = omapfb_close,
};
