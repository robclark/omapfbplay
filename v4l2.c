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

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>
#include <linux/fb.h>

#include "display.h"
#include "util.h"

static const unsigned format_map[][3] = {
    { PIX_FMT_YUV420P, V4L2_PIX_FMT_YUYV,   PIX_FMT_YUYV422 },
    { PIX_FMT_YUV420P, V4L2_PIX_FMT_NV12,   PIX_FMT_NV12    },
    { PIX_FMT_YUV420P, V4L2_PIX_FMT_YUV420, PIX_FMT_YUV420P },
    { PIX_FMT_NONE,    0,                   PIX_FMT_NONE    },
};

static const unsigned (*find_format(const unsigned (*tab)[3],
                                    enum PixelFormat fmt,
                                    unsigned vfmt))[3]
{
    const unsigned (*orig)[3] = tab;
    while (tab[0][0] != PIX_FMT_NONE) {
        if (tab[0][0] == fmt && tab[0][1] == vfmt)
            return tab;
        tab++;
    }
    return orig;
}

#define NEEDED_CAPS \
    (V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_VIDEO_OVERLAY | V4L2_CAP_STREAMING)

#define NUM_BUFFERS 2

static int vid_fd = -1;
static const struct pixconv *pixconv;

struct vid_buffer {
    struct v4l2_buffer buf;
    uint8_t *data[3];
    unsigned size;
};

static struct vid_buffer vid_buffers[NUM_BUFFERS];
static struct vid_buffer *cur_buf;

#define xioctl(fd, req, param) do {             \
        if (ioctl(fd, req, param) == -1) {      \
            perror(#req);                       \
            goto err;                           \
        }                                       \
    } while (0)

static void cleanup(void)
{
    int i;

    i = 0;
    ioctl(vid_fd, VIDIOC_OVERLAY, &i);

    i = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(vid_fd, VIDIOC_STREAMOFF, &i);

    for (i = 0; i < NUM_BUFFERS; i++) {
        if (vid_buffers[i].data[0])
            munmap(vid_buffers[i].data[0], vid_buffers[i].size);
        vid_buffers[i].data[0] = NULL;
    }

    close(vid_fd);
    vid_fd = -1;

    pixconv = NULL;
}

static int get_fbsize(struct frame_format *df)
{
    int fd = open("/dev/fb0", O_RDONLY);
    struct fb_var_screeninfo sinfo;
    int err;

    if (fd == -1) {
        perror("/dev/fb0");
        return -1;
    }

    err = ioctl(fd, FBIOGET_VSCREENINFO, &sinfo);

    if (!err) {
        df->width  = sinfo.xres;
        df->height = sinfo.yres;
    }

    close(fd);
    return err;
}

static int v4l2_open(const char *name, struct frame_format *df,
                     struct frame_format *ff)
{
    struct v4l2_format sfmt = { 0 };
    struct v4l2_capability cap;
    struct v4l2_fmtdesc fmt;
    const unsigned (*pixfmt)[3] = format_map;

    if (!name)
        name = "/dev/video1";

    vid_fd = open(name, O_RDWR);
    if (vid_fd == -1) {
        perror(name);
        goto err;
    }

    xioctl(vid_fd, VIDIOC_QUERYCAP, &cap);
    fprintf(stderr, "V4L2: driver=%s card=%s caps=%x\n",
            cap.driver, cap.card, cap.capabilities);

    if ((cap.capabilities & NEEDED_CAPS) != NEEDED_CAPS) {
        fprintf(stderr, "V4L2: device %s lacks required capabilities\n", name);
        goto err;
    }

    fmt.index = 0;
    fmt.type  = V4L2_BUF_TYPE_VIDEO_OUTPUT;

    while (!ioctl(vid_fd, VIDIOC_ENUM_FMT, &fmt)) {
        pixfmt = find_format(pixfmt, df->pixfmt, fmt.pixelformat);
        fmt.index++;
    }

    if (pixfmt[0][0] == df->pixfmt) {
        fprintf(stderr, "V4L2: using pixel format %08x\n", pixfmt[0][1]);
    } else {
        fprintf(stderr, "V4L2: no suitable pixel format supported\n");
        goto err;
    }

    sfmt.type                 = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    sfmt.fmt.pix.width        = ff->disp_w;
    sfmt.fmt.pix.height       = ff->disp_h;
    sfmt.fmt.pix.pixelformat  = pixfmt[0][1];
    sfmt.fmt.pix.field        = V4L2_FIELD_NONE;
    sfmt.fmt.pix.bytesperline = ALIGN(ff->disp_w, 32);

    xioctl(vid_fd, VIDIOC_S_FMT, &sfmt);

    df->pixfmt    = pixfmt[0][2];
    df->y_stride  = sfmt.fmt.pix.bytesperline;
    df->uv_stride = 0;

    if (get_fbsize(df)) {
        df->width     = 0;
        df->height    = 0;
    }

    return 0;
err:
    cleanup();
    return -1;
}

static int v4l2_enable(struct frame_format *ff, unsigned flags,
                       const struct pixconv *pc,
                       struct frame_format *df)
{
    struct v4l2_requestbuffers req;
    struct v4l2_format fmt = { 0 };
    int i;

    req.count  = NUM_BUFFERS;
    req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_MMAP;

    xioctl(vid_fd, VIDIOC_REQBUFS, &req);

    for (i = 0; i < req.count; i++) {
        struct v4l2_buffer *buf = &vid_buffers[i].buf;
        buf->index  = i;
        buf->type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf->memory = V4L2_MEMORY_MMAP;

        xioctl(vid_fd, VIDIOC_QUERYBUF, buf);

        vid_buffers[i].size    = buf->length;
        vid_buffers[i].data[0] = mmap(NULL, buf->length, PROT_READ|PROT_WRITE,
                                      MAP_SHARED, vid_fd, buf->m.offset);

        if (vid_buffers[i].data[0] == MAP_FAILED) {
            vid_buffers[i].data[0] = NULL;
            perror("mmap");
            goto err;
        }
    }

    fmt.type                 = V4L2_BUF_TYPE_VIDEO_OVERLAY;
    fmt.fmt.win.w.left       = df->disp_x;
    fmt.fmt.win.w.top        = df->disp_y;
    fmt.fmt.win.w.width      = df->disp_w;
    fmt.fmt.win.w.height     = df->disp_h;
    fmt.fmt.win.field        = V4L2_FIELD_NONE;
    fmt.fmt.win.global_alpha = 255;
    xioctl(vid_fd, VIDIOC_S_FMT, &fmt);

    for (i = 0; i < req.count; i++)
        xioctl(vid_fd, VIDIOC_QBUF, &vid_buffers[i].buf);

    i = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    xioctl(vid_fd, VIDIOC_STREAMON, &i);

    pixconv = pc;

    return 0;
err:
    return -1;
}

static void v4l2_prepare(struct frame *f)
{
    struct v4l2_buffer buf = { 0 };

    buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory = V4L2_MEMORY_MMAP;
    ioctl(vid_fd, VIDIOC_DQBUF, &buf);

    cur_buf = &vid_buffers[buf.index];
    pixconv->convert(cur_buf->data, f->data, NULL, NULL);
}

static void v4l2_show(struct frame *f)
{
    if (!cur_buf)
        return;

    pixconv->finish();
    ioctl(vid_fd, VIDIOC_QBUF, &cur_buf->buf);
    cur_buf = NULL;
    ofbp_put_frame(f);
}

static void v4l2_close(void)
{
    cleanup();
}

DISPLAY(v4l2) = {
    .name    = "v4l2",
    .flags   = OFB_DOUBLE_BUF,
    .open    = v4l2_open,
    .enable  = v4l2_enable,
    .prepare = v4l2_prepare,
    .show    = v4l2_show,
    .close   = v4l2_close,
};
