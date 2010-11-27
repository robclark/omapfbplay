/*
    Copyright (C) 2008 Mans Rullgard

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
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "display.h"
#include "timer.h"
#include "util.h"
#include "memman.h"
#include "codec.h"
#include "frame.h"
#include "pixconv.h"

#define BUFFER_SIZE (64*1024*1024)

static AVFormatContext *
open_file(const char *filename)
{
    AVFormatContext *afc;
    int err = av_open_input_file(&afc, filename, NULL, 0, NULL);

    if (!err)
        err = av_find_stream_info(afc);

    if (err < 0) {
        fprintf(stderr, "%s: lavf error %d\n", filename, err);
        exit(1);
    }

    dump_format(afc, 0, filename, 0);

    return afc;
}

static AVStream *
find_stream(AVFormatContext *afc)
{
    AVStream *st = NULL;
    int i;

    for (i = 0; i < afc->nb_streams; i++) {
        if (afc->streams[i]->codec->codec_type == CODEC_TYPE_VIDEO && !st)
            st = afc->streams[i];
        else
            afc->streams[i]->discard = AVDISCARD_ALL;
    }

    return st;
}

static const void *
find_driver(const char *name, const char **param, void *start)
{
    const char ***drv = start;
    const char *prm;
    int nlen = 0;

    if (!name)
        return *drv;

    prm = strchr(name, ':');

    if (prm) {
        nlen = prm - name;
        prm++;
    } else {
        nlen = strlen(name);
    }

    if (param)
        *param = prm;

    for (; *drv; drv++)
        if (!strncmp(**drv, name, nlen) && !(**drv)[nlen])
            break;

    return *drv;
}

static const struct timer *
timer_open(const char *dname)
{
    const struct timer *tmr = NULL;
    const char *param = NULL;

    tmr = find_driver(dname, &param, ofb_timer_start);
    if (tmr && !tmr->open(param))
        return tmr;

    fprintf(stderr, "Timer driver failed or missing\n");

    return NULL;
}

static const struct display *
display_open(const char *dname, struct frame_format *dp,
             struct frame_format *ff)
{
    const struct display *disp = NULL;
    const char *param = NULL;

    disp = find_driver(dname, &param, ofb_display_start);
    if (disp && !disp->open(param, dp, ff))
        return disp;

    fprintf(stderr, "Display driver failed or missing\n");

    return NULL;
}

static const struct pixconv *
pixconv_open(const char *name, const struct frame_format *ffmt,
             const struct frame_format *dfmt)
{
    const struct pixconv **start = ofb_pixconv_start;
    const struct pixconv *conv;

    do {
        conv = find_driver(name, NULL, start);
        if (conv && !conv->open(ffmt, dfmt))
            return conv;
    } while (*start++);

    fprintf(stderr, "No pixel converter found\n");

    return NULL;
}

static const struct display *display;
static const struct timer *timer;
static struct frame *frames;
static unsigned num_frames;
static int free_head;
static int free_tail;
static int disp_head = -1;
static int disp_tail = -1;
static int disp_count;

static pthread_mutex_t disp_lock;
static sem_t disp_sem;
static sem_t free_sem;

static int stop;

static int noaspect;

#define EDGE_WIDTH 32

struct frame *ofbp_get_frame(void)
{
    struct frame *f = frames + free_tail;

    sem_wait(&free_sem);

    if (free_tail < 0) {
        fprintf(stderr, "no more buffers\n");
        return NULL;
    }

    free_tail = f->next;
    frames[free_tail].prev = -1;
    f->next = -1;
    f->refs++;

    return f;
}

void ofbp_put_frame(struct frame *f)
{
    unsigned fnum = f->frame_num;

    if (!--f->refs) {
        f->prev = free_head;
        if (free_head != -1)
            frames[free_head].next = fnum;
        free_head = fnum;
        sem_post(&free_sem);
    }
}

static void *
disp_thread(void *p)
{
    AVStream *st = p;
    unsigned long fper =
        1000000000ull * st->r_frame_rate.den / st->r_frame_rate.num;
    struct timespec ftime;
    struct timespec tstart, t1, t2;
    int nf1 = 0, nf2 = 0;
    int sval;

    while (sem_getvalue(&free_sem, &sval), sval && !stop)
        usleep(100000);

    timer->start(&tstart);
    ftime = t1 = tstart;

    while (!sem_wait(&disp_sem) && !stop) {
        struct frame *f;

        pthread_mutex_lock(&disp_lock);
        f = frames + disp_tail;
        disp_tail = f->next;
        if (disp_tail != -1)
            frames[disp_tail].prev = -1;
        disp_count--;
        pthread_mutex_unlock(&disp_lock);

        f->next = -1;

        display->prepare(f);
        timer->wait(&ftime);
        display->show(f);

        if (++nf1 - nf2 == 50) {
            timer->read(&t2);
            fprintf(stderr, "%3d fps, buffer %3d\r",
                    (nf1-nf2)*1000 / ts_diff_ms(&t2, &t1),
                    disp_count);
            nf2 = nf1;
            t1 = t2;
        }

        ts_add_ns(&ftime, fper);

        timer->read(&t2);
        if (t2.tv_sec > ftime.tv_sec ||
            (t2.tv_sec == ftime.tv_sec && t2.tv_nsec > ftime.tv_nsec))
            ftime = t2;
    }

    if (nf1) {
        timer->read(&t2);
        fprintf(stderr, "%3d fps\n", nf1*1000 / ts_diff_ms(&t2, &tstart));
    }

    while (disp_tail != -1) {
        struct frame *f = frames + disp_tail;
        disp_tail = f->next;
        ofbp_put_frame(f);
    }

    return NULL;
}

void ofbp_post_frame(struct frame *f)
{
    unsigned fnum = f->frame_num;

    f->prev = disp_head;
    f->next = -1;

    if (disp_head != -1)
        frames[disp_head].next = fnum;
    disp_head = fnum;

    pthread_mutex_lock(&disp_lock);
    if (disp_tail == -1)
        disp_tail = fnum;
    disp_count++;
    pthread_mutex_unlock(&disp_lock);

    f->refs++;

    if (disp_count > 1)
        sem_post(&disp_sem);
}

static void
frame_format(int width, int height, struct frame_format *ff)
{
    ff->width  = ALIGN(width,  32) + EDGE_WIDTH * 2;
    ff->height = ALIGN(height, 32) + EDGE_WIDTH * 2;
    ff->disp_x = EDGE_WIDTH;
    ff->disp_y = EDGE_WIDTH;
    ff->disp_w = width;
    ff->disp_h = height;
}

static void
init_frames(void)
{
    int i;

    for (i = 0; i < num_frames; i++) {
        frames[i].frame_num = i;
        frames[i].pic_num = -num_frames;
        frames[i].next = i + 1;
        frames[i].prev = i - 1;
        frames[i].refs = 0;
    }

    free_head = num_frames - 1;
    frames[free_head].next = -1;
    sem_init(&free_sem, 0, num_frames - 1);
}

void ofb_scale(unsigned *x, unsigned *y, unsigned *w, unsigned *h,
               unsigned dw, unsigned dh)
{
    *x = 0;
    *y = 0;

    if (noaspect) {
        *w = dw;
        *h = dh;
    } else if (*w * dh > dw * *h) {
        *h = *h * dw / *w;
        *w = dw;
        *y = (dh - *h) / 2;
    } else {
        *w = *w * dh / *h;
        *h = dh;
        *x = (dw - *w) / 2;
    }
}

static void set_scale(struct frame_format *df, const struct frame_format *ff,
                      int flags)
{
    if ((flags & OFB_FULLSCREEN) ||
        ff->disp_w > df->width || ff->disp_h > df->height) {
        df->disp_w = ff->disp_w;
        df->disp_h = ff->disp_h;
        ofb_scale(&df->disp_x, &df->disp_y, &df->disp_w, &df->disp_h,
                  df->width, df->height);
    } else {
        df->disp_x = df->width  / 2 - ff->disp_w / 2;
        df->disp_y = df->height / 2 - ff->disp_h / 2;
        df->disp_w = ff->disp_w;
        df->disp_h = ff->disp_h;
    }

}

static void
sigint(int s)
{
    stop = 1;
    sem_post(&disp_sem);
}

#define TPVAL(i, sub) (i & (0x100 >> sub)? 255 - (i << sub) : ( i<< sub))

static void test_pattern(const struct frame *frames, int num_frames,
                         const struct frame_format *ff)
{
    int yplane, uplane, vplane;
    int ystart, ustart, vstart;
    int yinc, uinc, vinc;
    int hsub, vsub;
    int i, j, k;

    switch (ff->pixfmt) {
    case PIX_FMT_YUV420P:
        yplane = 0; ystart = 0; yinc = 1;
        uplane = 1; ustart = 0; uinc = 1;
        vplane = 2; vstart = 0; vinc = 1;
        hsub = vsub = 1;
        break;
    case PIX_FMT_YUYV422:
        yplane = uplane = vplane = 0;
        ystart = 0; yinc = 2;
        ustart = 1; uinc = 4;
        vstart = 3; vinc = 4;
        hsub = 1;
        vsub = 0;
        break;
    case PIX_FMT_NV12:
        yplane = 0; ystart = 0; yinc = 1;
        uplane = vplane = 1;
        ustart = 0;
        vstart = 1;
        uinc = vinc = 2;
        hsub = vsub = 1;
        break;
    default:
        fprintf(stderr, "Unknown pixel format %d\n", ff->pixfmt);
        return;
    }

    for (k = 0; k < num_frames; k++) {
        const struct frame *f = frames + k;
        uint8_t *y = f->data[yplane] + ystart;
        uint8_t *u = f->data[uplane] + ustart;
        uint8_t *v = f->data[vplane] + vstart;

        for (i = 0; i < ff->disp_h; i++)
            for (j = 0; j < ff->disp_w; j++)
                y[i*f->linesize[yplane] + j*yinc] = 128;

        for (i = 0; i < ff->disp_h >> vsub; i++) {
            for (j = 0; j < ff->disp_w >> hsub; j++) {
                u[i*f->linesize[uplane] + j*uinc] = TPVAL(i, vsub);
                v[i*f->linesize[vplane] + j*vinc] = TPVAL(j, hsub);
            }
        }
    }
}

static int
speed_test(const char *drv, const char *mem, const char *conv,
           char *size, unsigned disp_flags)
{
    const struct pixconv *pixconv = NULL;
    const struct memman *memman;
    struct frame_format dp;
    struct frame_format ff;
    struct timespec t1, t2;
    unsigned w, h = 0;
    unsigned n = 1000;
    unsigned bufsize;
    char *ss = size;
    int i, j;

    w = strtoul(size, &size, 0);
    if (*size++)
        h = strtoul(size, &size, 0);
    if (*size++)
        n = strtoul(size, NULL, 0);

    if (!w || !h || !n) {
        fprintf(stderr, "Invalid size/count '%s'\n", ss);
        return 1;
    }

    frame_format(w, h, &ff);

    dp.pixfmt = ff.pixfmt = PIX_FMT_YUV420P;
    display = display_open(drv, &dp, &ff);
    if (!display)
        return 1;

    set_scale(&dp, &ff, disp_flags);

    memman = display->memman;
    if (!memman)
        memman = find_driver(mem, NULL, ofb_memman_start);

    if (memman->alloc_frames(&ff, 0, &frames, &num_frames))
        return 1;

    if (!(display->flags & OFB_NOCONV)) {
        pixconv = pixconv_open(conv, &ff, &dp);
        if (!pixconv)
            return 1;
        if ((pixconv->flags & OFB_PHYS_MEM) &&
            !(memman->flags & display->flags & OFB_PHYS_MEM)) {
            fprintf(stderr, "Incompatible display/memman/pixconv\n");
            return 1;
        }
    }

    if (display->enable(&ff, disp_flags, pixconv, &dp))
        return 1;

    init_frames();

    bufsize = ff.disp_w * ff.disp_h * 3 / 2;

    test_pattern(frames, num_frames, &ff);

    signal(SIGINT, sigint);

    clock_gettime(CLOCK_REALTIME, &t1);

    for (i = 0; i < n && !stop; i++) {
        struct frame *f = ofbp_get_frame();
        display->prepare(f);
        display->show(f);
    }

    clock_gettime(CLOCK_REALTIME, &t2);
    j = ts_diff_ms(&t2, &t1);
    fprintf(stderr, "%d ms, %d fps, read %lld B/s, write %lld B/s\n",
            j, i*1000 / j, 1000LL*i*bufsize / j, 2000LL*i*w*h / j);

    memman->free_frames(frames, num_frames);
    display->close();
    if (pixconv) pixconv->close();

    return 0;
}

int
main(int argc, char **argv)
{
    AVFormatContext *afc;
    AVStream *st;
    AVPacket pk;
    struct frame_format frame_fmt;
    const struct pixconv *pixconv = NULL;
    const struct memman *memman = NULL;
    const struct codec *codec = NULL;
    struct frame_format dp;
    int bufsize = BUFFER_SIZE;
    pthread_t dispt;
    unsigned flags = OFB_DOUBLE_BUF;
    char *test_param = NULL;
    char *dispdrv = NULL;
    char *timer_drv = NULL;
    char *memman_drv = NULL;
    char *pixconv_drv = NULL;
    char *codec_drv = NULL;
    int opt;
    int ret = 0;

#define error(n) do { ret = n; goto out; } while (0)

    while ((opt = getopt(argc, argv, "b:d:fFM:P:st:T:v:")) != -1) {
        switch (opt) {
        case 'b':
            bufsize = strtol(optarg, NULL, 0) * 1048576;
            break;
        case 'd':
            dispdrv = optarg;
            break;
        case 'F':
            noaspect = 1;
        case 'f':
            flags |= OFB_FULLSCREEN;
            break;
        case 'M':
            memman_drv = optarg;
            break;
        case 'P':
            pixconv_drv = optarg;
            break;
        case 's':
            flags &= ~OFB_DOUBLE_BUF;
            break;
        case 't':
            test_param = optarg;
            break;
        case 'T':
            timer_drv = optarg;
            break;
        case 'v':
            codec_drv = optarg;
            break;
        }
    }

    argc -= optind;
    argv += optind;

    if (test_param)
        return speed_test(dispdrv, memman_drv, pixconv_drv, test_param, flags);

    if (argc < 1)
        return 1;

    av_register_all();
    avcodec_register_all();

    afc = open_file(argv[0]);

    st = find_stream(afc);
    if (!st) {
        fprintf(stderr, "No video streams found.\n");
        exit(1);
    }

    codec = find_driver(codec_drv, NULL, ofb_codec_start);
    if (!codec) {
        fprintf(stderr, "Decoder '%s' not found\n", codec_drv);
        error(1);
    }

    if (codec->open(NULL, st->codec)) {
        fprintf(stderr, "Error opening decoder\n");
        error(1);
    }

    frame_fmt.pixfmt = st->codec->pix_fmt;
    frame_format(st->codec->width, st->codec->height, &frame_fmt);

    dp.pixfmt = st->codec->pix_fmt;
    display = display_open(dispdrv, &dp, &frame_fmt);
    if (!display)
        error(1);

    set_scale(&dp, &frame_fmt, flags);

    memman = display->memman;
    if (!memman)
        memman = find_driver(memman_drv, NULL, ofb_memman_start);
    if (!memman)
        error(1);

    if (memman->alloc_frames(&frame_fmt, bufsize, &frames, &num_frames))
        error(1);

    if (!(display->flags & OFB_NOCONV)) {
        pixconv = pixconv_open(pixconv_drv, &frame_fmt, &dp);
        if (!pixconv)
            error(1);
        if ((pixconv->flags & OFB_PHYS_MEM) &&
            !(memman->flags & display->flags & OFB_PHYS_MEM)) {
            fprintf(stderr, "Incompatible display/memman/pixconv\n");
            error(1);
        }
    }

    timer = timer_open(timer_drv);
    if (!timer)
        error(1);

    if (display->enable(&frame_fmt, flags, pixconv, &dp))
        error(1);

    init_frames();

    pthread_mutex_init(&disp_lock, NULL);
    sem_init(&disp_sem, 0, 0);

    signal(SIGINT, sigint);

    pthread_create(&dispt, NULL, disp_thread, st);

    while (!stop && !av_read_frame(afc, &pk)) {
        if (pk.stream_index == st->index)
            codec->decode(&pk);
        av_free_packet(&pk);
    }

    if (!stop) {
        sem_post(&disp_sem);
        while (disp_tail != -1)
            usleep(100000);
    }

    stop = 1;
    sem_post(&disp_sem);
    pthread_join(dispt, NULL);

out:
    if (afc) av_close_input_file(afc);

    if (codec)   codec->close();
    if (timer)   timer->close();
    if (memman)  memman->free_frames(frames, num_frames);
    if (display) display->close();
    if (pixconv) pixconv->close();

    return ret;
}
