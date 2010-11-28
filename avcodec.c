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

#include <libavcodec/avcodec.h>
#include "frame.h"
#include "codec.h"

static AVCodecContext *avc;
static int pic_num;

static int get_buffer(AVCodecContext *ctx, AVFrame *pic)
{
    struct frame *f = ofbp_get_frame();
    int i;

    if (!f)
        return -1;

    for (i = 0; i < 3; i++) {
        pic->data[i]     = f->vdata[i];
        pic->linesize[i] = f->linesize[i];
    }

    pic->opaque = f;
    pic->type = FF_BUFFER_TYPE_USER;
    pic->age = ++pic_num - f->pic_num;
    f->pic_num = pic_num;

    return 0;
}

static void release_buffer(AVCodecContext *ctx, AVFrame *pic)
{
    struct frame *f = pic->opaque;
    int i;

    for (i = 0; i < 3; i++)
        pic->data[i] = NULL;

    ofbp_put_frame(f);
}


static int
reget_buffer(AVCodecContext *ctx, AVFrame *pic)
{
    struct frame *f = pic->opaque;
    fprintf(stderr, "reget_buffer   %2d\n", f->frame_num);

    if (!pic->data[0]) {
        pic->buffer_hints |= FF_BUFFER_HINTS_READABLE;
        return get_buffer(ctx, pic);
    }

    return 0;
}

static int lavc_open(const char *name, AVCodecContext *params,
                     struct frame_format *ff)
{
    int x_off, y_off;
    int edge_width;
    AVCodec *codec;
    int err;

    codec = avcodec_find_decoder(params->codec_id);
    if (!codec) {
        fprintf(stderr, "Can't find codec %x\n", params->codec_id);
        return -1;
    }

    avc = avcodec_alloc_context();

    avc->width          = params->width;
    avc->height         = params->height;
    avc->time_base      = params->time_base;
    avc->extradata      = params->extradata;
    avc->extradata_size = params->extradata_size;

    avc->get_buffer     = get_buffer;
    avc->release_buffer = release_buffer;
    avc->reget_buffer   = reget_buffer;

    err = avcodec_open(avc, codec);
    if (err) {
        fprintf(stderr, "avcodec_open: %d\n", err);
        return err;
    }

    edge_width = avcodec_get_edge_width();
    x_off      = ALIGN(edge_width, 32);
    y_off      = edge_width;

    ff->width  = ALIGN(params->width, 32) + 2 * x_off;
    ff->height = params->height           + 2 * y_off;
    ff->disp_x = x_off;
    ff->disp_y = y_off;
    ff->disp_w = params->width;
    ff->disp_h = params->height;
    ff->pixfmt = params->pix_fmt;

    return 0;
}

static int lavc_decode(AVPacket *p)
{
    AVFrame f;
    int gp = 0;

    if (avcodec_decode_video2(avc, &f, &gp, p) < 0)
        return -1;

    if (gp)
        ofbp_post_frame(f.opaque);

    return 0;
}

static void lavc_close(void)
{
    avcodec_close(avc);
    av_freep(&avc);
}

CODEC(avcodec) = {
    .name   = "avcodec",
    .open   = lavc_open,
    .decode = lavc_decode,
    .close  = lavc_close,
};
