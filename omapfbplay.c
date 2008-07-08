#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <signal.h>

#include <linux/fb.h>
#include <asm-arm/arch-omap/omapfb.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

static void
yuv420_to_yuv422(uint8_t *yuv, uint8_t *y, uint8_t *u, uint8_t *v,
                 int w, int h, int yw, int cw, int dw)
{
    uint8_t *tyuv, *ty, *tu, *tv;
    int i;

    asm volatile(
        "str       %[w], [sp, #-4]!                        \n\t"
        "1:                                                \n\t"
        "mov       %[tu],   %[u]                           \n\t"
        "mov       %[tv],   %[v]                           \n\t"
        "vld1.64   {d2},    [%[u],:64], %[cw]              \n\t"/* u0 */
        "vld1.64   {d3},    [%[v],:64], %[cw]              \n\t"/* v0 */
        "mov       %[tyuv], %[yuv]                         \n\t"
        "mov       %[ty],   %[y]                           \n\t"
        "vzip.8    d2, d3                                  \n\t"/* u0v0 */
        "mov       %[i], #16                               \n\t"
        "2:                                                \n\t"
        "pld       [%[y], #64]                             \n\t"
        "vld1.64   {d0,d1},   [%[y],:128], %[yw]           \n\t"/* y0 */
        "pld       [%[u], #64]                             \n\t"
        "subs      %[i], %[i], #4                          \n\t"
        "pld       [%[y], #64]                             \n\t"
        "vld1.64   {d6},      [%[u],:64],  %[cw]           \n\t"/* u2 */
        "pld       [%[v], #64]                             \n\t"
        "vld1.64   {d4,d5},   [%[y],:128], %[yw]           \n\t"/* y1 */
        "vld1.64   {d7},      [%[v],:64],  %[cw]           \n\t"/* v2 */
        "pld       [%[y], #64]                             \n\t"
        "vld1.64   {d16,d17}, [%[y],:128], %[yw]           \n\t"/* y2 */
        "vzip.8    d6, d7                                  \n\t"/* u2v2 */
        "pld       [%[u], #64]                             \n\t"
        "vld1.64   {d22},     [%[u],:64],  %[cw]           \n\t"/* u4 */
        "pld       [%[v], #64]                             \n\t"
        "vld1.64   {d23},     [%[v],:64],  %[cw]           \n\t"/* v4 */
        "pld       [%[y], #64]                             \n\t"
        "vld1.64   {d20,d21}, [%[y],:128], %[yw]           \n\t"/* y3 */
        "vmov      q9, q3                                  \n\t"/* u2v2 */
        "vrhadd.u8 q3, q1, q3                              \n\t"/* u1v1 */
        "vzip.8    d22, d23                                \n\t"/* u4v4 */
        "vzip.8    q0, q1                                  \n\t"/* y0u0y0v0 */
        "vmov      q12, q11                                \n\t"/* u4v4 */
        "vrhadd.u8 q11, q9, q11                            \n\t"/* u3v3 */
        "vzip.8    q2, q3                                  \n\t"/* y1u1y1v1 */
        "vst1.64   {d0,d1,d2,d3},     [%[yuv],:128], %[dw] \n\t"/* y0u0y0v0 */
        "vzip.8    q8,  q9                                 \n\t"/* y2u2y2v2 */
        "vst1.64   {d4,d5,d6,d7},     [%[yuv],:128], %[dw] \n\t"/* y1u1y1v1 */
        "vzip.8    q10, q11                                \n\t"/* y3u3y3v3 */
        "vst1.64   {d16,d17,d18,d19}, [%[yuv],:128], %[dw] \n\t"/* y2u2y2v2 */
        "vmov      q1, q12                                 \n\t"
        "vst1.64   {d20,d21,d22,d23}, [%[yuv],:128], %[dw] \n\t"/* y3u3y3v3 */
        "bgt       2b                                      \n\t"
        "subs      %[w],   %[w],    #16                    \n\t"
        "add       %[yuv], %[tyuv], #32                    \n\t"
        "add       %[y],   %[ty],   #16                    \n\t"
        "add       %[u],   %[tu],   #8                     \n\t"
        "add       %[v],   %[tv],   #8                     \n\t"
        "bgt       1b                                      \n\t"
        "ldr       %[w],   [sp]                            \n\t"
        "subs      %[h],   %[h],   #16                     \n\t"
        "add       %[yuv], %[yuv], %[dw], lsl #4           \n\t"
        "sub       %[yuv], %[yuv], %[w],  lsl #1           \n\t"
        "add       %[y],   %[y],   %[yw], lsl #4           \n\t"
        "sub       %[y],   %[y],   %[w]                    \n\t"
        "add       %[u],   %[u],   %[cw], lsl #3           \n\t"
        "sub       %[u],   %[u],   %[w],  asr #1           \n\t"
        "add       %[v],   %[v],   %[cw], lsl #3           \n\t"
        "sub       %[v],   %[v],   %[w],  asr #1           \n\t"
        "bgt       1b                                      \n\t"
        "add       sp, sp, #4                              \n\t"
        : [yuv]"+r"(yuv), [y]"+r"(y), [u]"+r"(u), [v]"+r"(v),
          [tyuv]"=&r"(tyuv), [ty]"=&r"(ty), [tu]"=&r"(tu), [tv]"=&r"(tv),
          [w]"+r"(w), [h]"+r"(h), [i]"=&r"(i)
        : [yw]"r"(yw), [cw]"r"(cw), [dw]"r"(dw)
        : "q0", "q1", "q2", "q3", "q8", "q9", "q10", "q11", "q12",
          "memory");
}

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

static struct fb_var_screeninfo sinfo_p0;
static struct fb_var_screeninfo sinfo;
static struct omapfb_mem_info minfo;
static struct omapfb_plane_info pinfo;

static struct {
    unsigned x;
    unsigned y;
    uint8_t *buf;
} fb_pages[2];

static int fb_page_flip;

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
setup_fb(AVStream *st, int fullscreen)
{
    int fb = open("/dev/fb0", O_RDWR);
    uint8_t *fbmem;

    if (fb == -1) {
        perror("/dev/fb0");
        exit(1);
    }

    xioctl(fb, FBIOGET_VSCREENINFO, &sinfo_p0);
    close(fb);

    fb = open("/dev/fb1", O_RDWR);

    if (fb == -1) {
        perror("/dev/fb1");
        exit(1);
    }

    xioctl(fb, FBIOGET_VSCREENINFO, &sinfo);
    xioctl(fb, OMAPFB_QUERY_PLANE, &pinfo);
    xioctl(fb, OMAPFB_QUERY_MEM, &minfo);

    fbmem = mmap(NULL, minfo.size, PROT_READ|PROT_WRITE, MAP_SHARED, fb, 0);
    if (fbmem == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    sinfo.xres = FFMIN(sinfo_p0.xres, st->codec->width)  & ~15;
    sinfo.yres = FFMIN(sinfo_p0.xres, st->codec->height) & ~15;
    sinfo.xoffset = 0;
    sinfo.yoffset = 0;
    sinfo.nonstd = OMAPFB_COLOR_YUY422;

    fb_pages[0].x = 0;
    fb_pages[0].y = 0;
    fb_pages[0].buf = fbmem;

    if (minfo.size >= sinfo.xres * sinfo.yres * 2) {
        sinfo.xres_virtual = sinfo.xres;
        sinfo.yres_virtual = sinfo.yres * 2;
        fb_pages[1].x = 0;
        fb_pages[1].y = sinfo.yres;
        fb_pages[1].buf = fbmem + sinfo.xres * sinfo.yres * 2;
        fb_page_flip = 1;
    }

    xioctl(fb, FBIOPUT_VSCREENINFO, &sinfo);

    pinfo.enabled = 1;
    if (fullscreen) {
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

    ioctl(fb, OMAPFB_SETUP_PLANE, &pinfo);

    return fb;
}

static int stop;

static void
sigint(int s)
{
    stop = 1;
}

static int
tv_diff(struct timeval *tv1, struct timeval *tv2)
{
    return (tv1->tv_sec - tv2->tv_sec) * 1000 +
        (tv1->tv_usec - tv2->tv_usec) / 1000;
}

int
main(int argc, char **argv)
{
    struct timeval tstart, t1, t2;
    int nf1 = 0, nf2 = 0;
    AVFormatContext *afc;
    AVCodec *codec;
    AVCodecContext *avc;
    AVStream *st;
    AVPacket pk;
    int fullscreen = 0;
    int page = 0;
    int opt;
    int err;
    int fb;

    while ((opt = getopt(argc, argv, "f")) != -1) {
        switch (opt) {
        case 'f':
            fullscreen = 1;
            break;
        }
    }

    argc -= optind;
    argv += optind;

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
    avc->extradata      = st->codec->extradata;
    avc->extradata_size = st->codec->extradata_size;

    err = avcodec_open(avc, codec);
    if (err) {
        fprintf(stderr, "avcodec_open: %d\n", err);
        exit(1);
    }

    fb = setup_fb(st, fullscreen);

    signal(SIGINT, sigint);
    gettimeofday(&tstart, NULL);
    t1 = tstart;

    while (!stop && !av_read_frame(afc, &pk)) {
        AVFrame f;
        int gp = 0;

        if (pk.stream_index == st->index) {
            avcodec_decode_video(avc, &f, &gp, pk.data, pk.size);

            if (gp) {
                yuv420_to_yuv422(fb_pages[page].buf,
                                 f.data[0], f.data[1], f.data[2],
                                 sinfo.xres, sinfo.yres,
                                 f.linesize[0], f.linesize[1],
                                 2*sinfo.xres_virtual);

                if (fb_page_flip) {
                    sinfo.xoffset = fb_pages[page].x;
                    sinfo.yoffset = fb_pages[page].y;
                    xioctl(fb, FBIOPAN_DISPLAY, &sinfo);
                    page ^= fb_page_flip;
                }

                nf1++;
            }
        }

        av_free_packet(&pk);

        if (nf1 - nf2 == 50) {
            gettimeofday(&t2, NULL);
            fprintf(stderr, "%3d fps\r", (nf1-nf2)*1000 / tv_diff(&t2, &t1));
            nf2 = nf1;
            t1 = t2;
        }
    }
    gettimeofday(&t2, NULL);
    fprintf(stderr, "%3d fps\n", nf1*1000 / tv_diff(&t2, &tstart));

    pinfo.enabled = 0;
    ioctl(fb, OMAPFB_SETUP_PLANE, &pinfo);
    close(fb);

    avcodec_close(avc);
    av_close_input_file(afc);

    return 0;
}
