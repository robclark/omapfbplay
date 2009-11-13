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
#include "util.h"

static struct fb_var_screeninfo gfx_sinfo;
static struct omapfb_plane_info gfx_pinfo;

static struct fb_var_screeninfo vid_sinfo;
static struct omapfb_plane_info vid_pinfo;
static struct omapfb_mem_info   vid_minfo;

static struct {
    unsigned x;
    unsigned y;
    uint8_t *buf;
} fb_pages[2];

static int gfx_fd = -1;
static int vid_fd = -1;
static int fb_page_flip;
static int fb_page;

#define xioctl(fd, req, param) do {             \
        if (ioctl(fd, req, param) == -1)        \
            goto err;                           \
    } while (0)

static void
cleanup(void)
{
    close(gfx_fd);   gfx_fd    = -1;
    close(vid_fd);   vid_fd    = -1;
}

static int omapfb_open(const char *name, struct display_props *dp)
{
    gfx_fd = open("/dev/fb0", O_RDWR);
    if (gfx_fd == -1) {
        perror("/dev/fb0");
        goto err;
    }

    vid_fd = open("/dev/fb1", O_RDWR);
    if (vid_fd == -1) {
        perror("/dev/fb1");
        goto err;
    }

    xioctl(gfx_fd, FBIOGET_VSCREENINFO, &gfx_sinfo);
    xioctl(gfx_fd, OMAPFB_QUERY_PLANE,  &gfx_pinfo);

    xioctl(vid_fd, FBIOGET_VSCREENINFO, &vid_sinfo);
    xioctl(vid_fd, OMAPFB_QUERY_PLANE,  &vid_pinfo);
    xioctl(vid_fd, OMAPFB_QUERY_MEM,    &vid_minfo);

    dp->width  = gfx_sinfo.xres;
    dp->height = gfx_sinfo.yres;

    return 0;

err:
    cleanup();
    return -1;
}

static int
omapfb_enable(struct frame_format *ff, unsigned flags)
{
    unsigned frame_size;
    unsigned mem_size;
    uint8_t *fbmem;
    int i;

    vid_sinfo.xres = MIN(gfx_sinfo.xres, ff->disp_w) & ~15;
    vid_sinfo.yres = MIN(gfx_sinfo.yres, ff->disp_h) & ~15;
    vid_sinfo.xoffset = 0;
    vid_sinfo.yoffset = 0;
    vid_sinfo.nonstd = OMAPFB_COLOR_YUY422;

    frame_size = vid_sinfo.xres * vid_sinfo.yres * 2;
    mem_size = vid_minfo.size;

    if (!mem_size) {
        struct omapfb_mem_info mi = vid_minfo;
        mi.size = frame_size * 2;
        if (ioctl(vid_fd, OMAPFB_SETUP_MEM, &mi)) {
            mi.size = frame_size;
            if (ioctl(vid_fd, OMAPFB_SETUP_MEM, &mi)) {
                perror("Unable to allocate FB memory");
                return -1;
            }
        }
        mem_size = mi.size;
    }        

    fbmem = mmap(NULL, mem_size, PROT_READ|PROT_WRITE, MAP_SHARED, vid_fd, 0);
    if (fbmem == MAP_FAILED) {
        perror("mmap");
        goto err;
    }

    for (i = 0; i < mem_size / 4; i++)
        ((uint32_t*)fbmem)[i] = 0x80008000;

    fb_pages[0].x = 0;
    fb_pages[0].y = 0;
    fb_pages[0].buf = fbmem;

    if (flags & OFB_DOUBLE_BUF && mem_size >= frame_size * 2) {
        vid_sinfo.xres_virtual = vid_sinfo.xres;
        vid_sinfo.yres_virtual = vid_sinfo.yres * 2;
        fb_pages[1].x = 0;
        fb_pages[1].y = vid_sinfo.yres;
        fb_pages[1].buf = fbmem + frame_size;
        fb_page_flip = 1;
    }

    xioctl(vid_fd, FBIOPUT_VSCREENINFO, &vid_sinfo);

    vid_pinfo.enabled = 1;

    if (flags & OFB_FULLSCREEN) {
        unsigned x, y, w = vid_sinfo.xres, h = vid_sinfo.yres;
        ofb_scale(&x, &y, &w, &h, gfx_sinfo.xres, gfx_sinfo.yres);
        vid_pinfo.pos_x      = x;
        vid_pinfo.pos_y      = y;
        vid_pinfo.out_width  = w;
        vid_pinfo.out_height = h;
    } else {
        vid_pinfo.pos_x = gfx_sinfo.xres / 2 - vid_sinfo.xres / 2;
        vid_pinfo.pos_y = gfx_sinfo.yres / 2 - vid_sinfo.yres / 2;
        vid_pinfo.out_width  = vid_sinfo.xres;
        vid_pinfo.out_height = vid_sinfo.yres;
    }

    xioctl(vid_fd, OMAPFB_SETUP_PLANE, &vid_pinfo);

    if (flags & OFB_FULLSCREEN ||
        (vid_pinfo.pos_x <= gfx_pinfo.pos_x  &&
         vid_pinfo.pos_y >= gfx_pinfo.pos_y  &&
         vid_pinfo.pos_x + vid_pinfo.out_width  >=
             gfx_pinfo.pos_x + gfx_pinfo.out_width &&
         vid_pinfo.pos_y + vid_pinfo.out_height >=
             gfx_pinfo.pos_y + gfx_pinfo.out_height)) {
        struct omapfb_plane_info pi = gfx_pinfo;
        pi.enabled = 0;
        xioctl(gfx_fd, OMAPFB_SETUP_PLANE, &pi);
    }

    return 0;

err:
    return -1;
}

static inline void
convert_frame(struct frame *f)
{
    yuv420_to_yuv422(fb_pages[fb_page].buf,
                     f->data[0], f->data[1], f->data[2],
                     vid_sinfo.xres, vid_sinfo.yres,
                     f->linesize[0], f->linesize[1],
                     2*vid_sinfo.xres_virtual);
}

static void omapfb_prepare(struct frame *f)
{
    if (fb_page_flip)
        convert_frame(f);
}

static void omapfb_show(struct frame *f)
{
    if (!fb_page_flip)
        convert_frame(f);

    if (fb_page_flip) {
        vid_sinfo.xoffset = fb_pages[fb_page].x;
        vid_sinfo.yoffset = fb_pages[fb_page].y;
        ioctl(vid_fd, FBIOPAN_DISPLAY, &vid_sinfo);
        fb_page ^= fb_page_flip;
        ioctl(vid_fd, OMAPFB_WAITFORGO);
    }
}

static void omapfb_close(void)
{
    ioctl(gfx_fd, OMAPFB_SETUP_PLANE, &gfx_pinfo);

    vid_pinfo.enabled = 0;
    ioctl(vid_fd, OMAPFB_SETUP_PLANE, &vid_pinfo);
    ioctl(vid_fd, OMAPFB_SETUP_MEM,   &vid_minfo);

    cleanup();
}

DISPLAY(omapfb) = {
    .name  = "omapfb",
    .flags = OFB_FULLSCREEN | OFB_DOUBLE_BUF,
    .open  = omapfb_open,
    .enable  = omapfb_enable,
    .prepare = omapfb_prepare,
    .show  = omapfb_show,
    .close = omapfb_close,
};
