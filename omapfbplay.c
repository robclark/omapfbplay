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
    const char ***drv;
    int nlen = 0;

    *param = strchr(name, ':');

    if (*param) {
        nlen = *param - name;
        (*param)++;
    } else if (name) {
        nlen = strlen(name);
    }

    for (drv = start; *drv; drv++)
        if (!strncmp(**drv, name, nlen) && !(**drv)[nlen])
            return *drv;

    return NULL;
}

static const struct timer *
timer_open(const char *dname)
{
    const struct timer *tmr = NULL;
    const char *param = NULL;

    if (dname)
        tmr = find_driver(dname, &param, ofb_timer_start);
    else
        tmr = ofb_timer_start[0];

    if (tmr && !tmr->open(param))
        return tmr;

    fprintf(stderr, "Timer driver failed or missing\n");

    return NULL;
}

static const struct display *
display_open(const char *dname, struct frame_format *fmt, unsigned flags,
             unsigned max_mem, struct frame **frames, unsigned *nframes)
{
    const struct display *disp = NULL;
    const char *param = NULL;

    if (dname)
        disp = find_driver(dname, &param, ofb_display_start);
    else
        disp = ofb_display_start[0];

    if (disp && !disp->open(param, fmt, flags, max_mem, frames, nframes))
        return disp;

    fprintf(stderr, "Display driver failed or missing\n");

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

static int pic_num;

static int stop;

#define EDGE_WIDTH 16

static int
ofb_get_buffer(AVCodecContext *ctx, AVFrame *pic)
{
    struct frame *f = frames + free_tail;
    int i;

    sem_wait(&free_sem);

    if (free_tail < 0) {
        fprintf(stderr, "no more buffers\n");
        return -1;
    }

    for (i = 0; i < 3; i++) {
        pic->data[i] = pic->base[i] = f->data[i];
        pic->linesize[i] = f->linesize[i];
    }

    pic->opaque = f;
    pic->type = FF_BUFFER_TYPE_USER;
    pic->age = ++pic_num - f->pic_num;
    f->pic_num = pic_num;
    f->refs++;

    free_tail = f->next;
    frames[free_tail].prev = -1;
    f->next = -1;

    return 0;
}

static void
ofb_release_frame(struct frame *f)
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

static void
ofb_release_buffer(AVCodecContext *ctx, AVFrame *pic)
{
    struct frame *f = pic->opaque;
    int i;

    for (i = 0; i < 3; i++)
        pic->data[i] = NULL;

    ofb_release_frame(f);
}

static int
ofb_reget_buffer(AVCodecContext *ctx, AVFrame *pic)
{
    struct frame *f = pic->opaque;
    fprintf(stderr, "reget_buffer   %2d\n", f->frame_num);

    if (!pic->data[0]) {
        pic->buffer_hints |= FF_BUFFER_HINTS_READABLE;
        return ofb_get_buffer(ctx, pic);
    }

    return 0;
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

        ofb_release_frame(f);

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
        ofb_release_frame(f);
    }

    return NULL;
}

static void
post_frame(AVFrame *pic)
{
    struct frame *f = pic->opaque;
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

    sem_post(&disp_sem);
}

static void
frame_format(int width, int height, int border, struct frame_format *ff)
{
    ff->width  = ALIGN(width,  16);
    ff->height = ALIGN(height, 16);
    ff->disp_x = 0;
    ff->disp_y = 0;
    ff->disp_w = width;
    ff->disp_h = height;

    if (border) {
        ff->width  += EDGE_WIDTH * 2;
        ff->height += EDGE_WIDTH * 2;
        ff->disp_x = EDGE_WIDTH;
        ff->disp_y = EDGE_WIDTH;
    }
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
}

static void
sigint(int s)
{
    stop = 1;
    sem_post(&disp_sem);
}

static int
speed_test(const char *drv, char *size, unsigned disp_flags)
{
    struct frame_format ff;
    struct timespec t1, t2;
    uint8_t *y, *u, *v;
    unsigned w, h = 0;
    unsigned n = 1000;
    unsigned bufsize;
    int i, j;

    w = strtoul(size, &size, 0);
    if (*size++)
        h = strtoul(size, &size, 0);
    if (*size++)
        n = strtoul(size, NULL, 0);

    w &= ~15;
    h &= ~15;

    if (!w || !h || !n) {
        fprintf(stderr, "Invalid size/count '%s'\n", size);
        return 1;
    }

    frame_format(w, h, 0, &ff);

    display = display_open(drv, &ff, disp_flags, 0, &frames, &num_frames);
    if (!display)
        return 1;

    bufsize = ff.disp_w * ff.disp_h * 3 / 2;

    y = frames->data[0];
    u = frames->data[1];
    v = frames->data[2];

    memset(y, 128, ff.disp_h * frames->linesize[0]);

    for (i = 0; i < ff.disp_h / 2; i++) {
        for (j = 0; j < ff.disp_w / 2; j++) {
            u[i*frames->linesize[1] + j] = 2*i;
            v[i*frames->linesize[2] + j] = 2*j;
        }
    }

    signal(SIGINT, sigint);

    clock_gettime(CLOCK_REALTIME, &t1);

    for (i = 0; i < n && !stop; i++) {
        display->prepare(frames);
        display->show(frames);
    }

    clock_gettime(CLOCK_REALTIME, &t2);
    j = ts_diff_ms(&t2, &t1);
    fprintf(stderr, "%d ms, %d fps, read %lld B/s, write %lld B/s\n",
            j, i*1000 / j, 1000LL*i*bufsize / j, 2000LL*i*w*h / j);

    display->close();

    return 0;
}

int
main(int argc, char **argv)
{
    AVFormatContext *afc;
    AVCodec *codec;
    AVCodecContext *avc;
    AVStream *st;
    AVPacket pk;
    struct frame_format frame_fmt;
    int bufsize = BUFFER_SIZE;
    pthread_t dispt;
    unsigned flags = OFB_DOUBLE_BUF;
    char *test_param = NULL;
    char *dispdrv = NULL;
    char *timer_drv = NULL;
    int opt;
    int err;

    while ((opt = getopt(argc, argv, "b:d:fst:T:")) != -1) {
        switch (opt) {
        case 'b':
            bufsize = strtol(optarg, NULL, 0) * 1048576;
            break;
        case 'd':
            dispdrv = optarg;
            break;
        case 'f':
            flags |= OFB_FULLSCREEN;
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
        }
    }

    argc -= optind;
    argv += optind;

    if (test_param)
        return speed_test(dispdrv, test_param, flags);

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

    codec = avcodec_find_decoder(st->codec->codec_id);
    if (!codec) {
        fprintf(stderr, "Can't find codec %x\n", st->codec->codec_id);
        exit(1);
    }

    avc = avcodec_alloc_context();

    avc->width          = st->codec->width;
    avc->height         = st->codec->height;
    avc->time_base      = st->codec->time_base;
    avc->extradata      = st->codec->extradata;
    avc->extradata_size = st->codec->extradata_size;

    avc->get_buffer     = ofb_get_buffer;
    avc->release_buffer = ofb_release_buffer;
    avc->reget_buffer   = ofb_reget_buffer;

    err = avcodec_open(avc, codec);
    if (err) {
        fprintf(stderr, "avcodec_open: %d\n", err);
        exit(1);
    }

    frame_format(st->codec->width, st->codec->height,
                 !(st->codec->flags & CODEC_FLAG_EMU_EDGE),
                 &frame_fmt);

    display = display_open(dispdrv, &frame_fmt, flags, bufsize,
                           &frames, &num_frames);
    if (!display)
        return 1;

    timer = timer_open(timer_drv);
    if (!timer)
        return 1;

    init_frames();

    pthread_mutex_init(&disp_lock, NULL);
    sem_init(&disp_sem, 0, 0);
    sem_init(&free_sem, 0, num_frames - 1);

    signal(SIGINT, sigint);

    pthread_create(&dispt, NULL, disp_thread, st);

    while (!stop && !av_read_frame(afc, &pk)) {
        AVFrame f;
        int gp = 0;

        if (pk.stream_index == st->index) {
            avcodec_decode_video2(avc, &f, &gp, &pk);

            if (gp) {
                post_frame(&f);
            }
        }

        av_free_packet(&pk);
    }

    if (!stop) {
        while (disp_tail != -1)
            usleep(100000);
    }

    stop = 1;
    sem_post(&disp_sem);
    pthread_join(dispt, NULL);

    avcodec_close(avc);
    av_close_input_file(afc);
    av_free(avc);

    timer->close();
    display->close();

    return 0;
}
