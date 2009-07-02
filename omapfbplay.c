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

static int stop;

static int
ts_diff(struct timespec *tv1, struct timespec *tv2)
{
    return (tv1->tv_sec - tv2->tv_sec) * 1000 +
        (tv1->tv_nsec - tv2->tv_nsec) / 1000000;
}

static void
ts_add(struct timespec *ts, unsigned long nsec)
{
    ts->tv_nsec += nsec;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
}

static struct frame  *frames;
static uint8_t *frame_buf;
static unsigned num_frames;
static unsigned frame_size;
static unsigned linesize;
static int free_head;
static int free_tail;
static int disp_head = -1;
static int disp_tail = -1;
static int disp_count;

static pthread_mutex_t disp_lock;
static sem_t disp_sem;
static sem_t free_sem;

static int pic_num;

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
        pic->linesize[i] = linesize;
    }

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
    unsigned fnum = f - frames;

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
    unsigned fnum = (pic->data[0] - frame_buf) / frame_size;
    struct frame *f = frames + fnum;
    int i;

    for (i = 0; i < 3; i++)
        pic->data[i] = NULL;

    ofb_release_frame(f);
}

static int
ofb_reget_buffer(AVCodecContext *ctx, AVFrame *pic)
{
    unsigned fnum = (pic->data[0] - frame_buf) / frame_size;
    fprintf(stderr, "reget_buffer   %2d\n", fnum);

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

    timer_init();

    while (sem_getvalue(&free_sem, &sval), sval && !stop)
        usleep(100000);

    timer_start(&tstart);
    ftime = t1 = tstart;

    while (!sem_wait(&disp_sem) && !stop) {
        struct frame *f;

        timer_wait(&ftime);

        pthread_mutex_lock(&disp_lock);
        f = frames + disp_tail;
        disp_tail = f->next;
        if (disp_tail != -1)
            frames[disp_tail].prev = -1;
        disp_count--;
        pthread_mutex_unlock(&disp_lock);

        f->next = -1;

        display_frame(f);

        ofb_release_frame(f);

        if (++nf1 - nf2 == 50) {
            timer_read(&t2);
            fprintf(stderr, "%3d fps, buffer %3d\r",
                    (nf1-nf2)*1000 / ts_diff(&t2, &t1),
                    disp_count);
            nf2 = nf1;
            t1 = t2;
        }

        ts_add(&ftime, fper);

        timer_read(&t2);
        if (t2.tv_sec > ftime.tv_sec ||
            (t2.tv_sec == ftime.tv_sec && t2.tv_nsec > ftime.tv_nsec))
            ftime = t2;
    }

    if (nf1) {
        timer_read(&t2);
        fprintf(stderr, "%3d fps\n", nf1*1000 / ts_diff(&t2, &tstart));
    }

    timer_close();

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
    unsigned fnum = (pic->data[0] - frame_buf) / frame_size;
    struct frame *f = frames + fnum;

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

static int
alloc_buffers(AVStream *st, unsigned bufsize)
{
    int buf_w, buf_h;
    unsigned frame_offset = 0;
    void *fbp;
    int i;

    buf_w = ALIGN(st->codec->width,  16);
    buf_h = ALIGN(st->codec->height, 16);

    if (!(st->codec->flags & CODEC_FLAG_EMU_EDGE)) {
        buf_w += EDGE_WIDTH * 2;
        buf_h += EDGE_WIDTH * 2;
        frame_offset = buf_w * EDGE_WIDTH + EDGE_WIDTH;
    }

    frame_size = buf_w * buf_h * 3 / 2;
    num_frames = bufsize / frame_size;
    bufsize = num_frames * frame_size;
    linesize = buf_w;

    fprintf(stderr, "Using %d frame buffers, frame_size=%d\n",
            num_frames, frame_size);

    if (posix_memalign(&fbp, 16, bufsize)) {
        fprintf(stderr, "Error allocating frame buffers: %d bytes\n", bufsize);
        exit(1);
    }

    frame_buf = fbp;
    frames = malloc(num_frames * sizeof(*frames));

    for (i = 0; i < num_frames; i++) {
        uint8_t *p = frame_buf + i * frame_size;

        frames[i].data[0] = p + frame_offset;
        frames[i].data[1] = p + buf_w * buf_h + frame_offset / 2;
        frames[i].data[2] = frames[i].data[1] + buf_w / 2;
        frames[i].linesize = linesize;

        frames[i].pic_num = -num_frames;
        frames[i].next = i + 1;
        frames[i].prev = i - 1;
        frames[i].refs = 0;
    }

    free_head = num_frames - 1;
    frames[free_head].next = -1;

    return 0;
}

static void
sigint(int s)
{
    stop = 1;
    sem_post(&disp_sem);
}

static int
speed_test(char *size, unsigned disp_flags)
{
    struct frame frame;
    struct timespec t1, t2;
    uint8_t *y, *u, *v;
    unsigned w, h = 0;
    unsigned n = 1000;
    unsigned bufsize;
    void *buf;
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

    bufsize = w * h * 3 / 2;
    if (posix_memalign(&buf, 16, bufsize)) {
        fprintf(stderr, "Error allocating %u bytes\n", bufsize);
        return 1;
    }

    y = buf;
    u = y + w * h;
    v = u + w / 2;

    memset(y, 128, w * h);

    for (i = 0; i < h / 2; i++) {
        for (j = 0; j < w / 2; j++) {
            u[i*w + j] = 2*i;
            v[i*w + j] = 2*j;
        }
    }

    frame.data[0] = y;
    frame.data[1] = u;
    frame.data[2] = v;
    frame.linesize = w;

    display_open(NULL, w, h, disp_flags);
    signal(SIGINT, sigint);

    clock_gettime(CLOCK_REALTIME, &t1);

    for (i = 0; i < n && !stop; i++) {
        display_frame(&frame);
    }

    clock_gettime(CLOCK_REALTIME, &t2);
    j = ts_diff(&t2, &t1);
    fprintf(stderr, "%d ms, %d fps, read %lld B/s, write %lld B/s\n",
            j, i*1000 / j, 1000LL*i*bufsize / j, 2000LL*i*w*h / j);

    display_close();

    free(buf);

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
    int bufsize = BUFFER_SIZE;
    pthread_t dispt;
    unsigned flags = OFB_DOUBLE_BUF;
    char *test_param = NULL;
    int opt;
    int err;

    while ((opt = getopt(argc, argv, "b:fst:")) != -1) {
        switch (opt) {
        case 'b':
            bufsize = strtol(optarg, NULL, 0) * 1048576;
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
        }
    }

    argc -= optind;
    argv += optind;

    if (test_param)
        return speed_test(test_param, flags);

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

    alloc_buffers(st, bufsize);

    pthread_mutex_init(&disp_lock, NULL);
    sem_init(&disp_sem, 0, 0);
    sem_init(&free_sem, 0, num_frames - 1);

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

    display_open(NULL, st->codec->width, st->codec->height, flags);

    signal(SIGINT, sigint);

    pthread_create(&dispt, NULL, disp_thread, st);

    while (!stop && !av_read_frame(afc, &pk)) {
        AVFrame f;
        int gp = 0;

        if (pk.stream_index == st->index) {
            avcodec_decode_video(avc, &f, &gp, pk.data, pk.size);

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

    display_close();

    avcodec_close(avc);
    av_close_input_file(afc);

    free(frame_buf);
    free(frames);

    return 0;
}
