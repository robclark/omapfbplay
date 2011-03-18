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

#include <stdint.h>
#include <stdlib.h>
#include <memmgr/tilermem.h>
#include <memmgr/memmgr.h>
#include <dce/dce.h>
#include <dce/xdc/std.h>
#include <dce/ti/sdo/ce/Engine.h>
#include <dce/ti/sdo/ce/video3/viddec3.h>
#include <libavcodec/avcodec.h>

#include "frame.h"
#include "codec.h"
#include "util.h"

#define PADX  32
#define PADY  24

static Engine_Handle           engine;
static VIDDEC3_Handle          codec;
static VIDDEC3_Params         *params;
static VIDDEC3_DynamicParams  *dyn_params;
static VIDDEC3_Status         *status;
static XDM2_BufDesc           *inbufs;
static XDM2_BufDesc           *outbufs;
static VIDDEC3_InArgs         *in_args;
static VIDDEC3_OutArgs        *out_args;

static uint8_t *input_buf;
static uint32_t input_phys;
static int      input_size;

static AVCodecContext           *avc;
static AVBitStreamFilterContext *bsf;

static int alloc_input(int size)
{
    MemAllocBlock mablk = { 0 };
    uint8_t *buf;

    if (size <= input_size)
        return 0;

    size = ALIGN(size * 5 / 4, 8192);

    mablk.pixelFormat = PIXEL_FMT_PAGE;
    mablk.dim.len     = size;

    buf = MemMgr_Alloc(&mablk, 1);
    if (!buf)
        return -1;

    MemMgr_Free(input_buf);

    input_buf  = buf;
    input_phys = TilerMem_VirtToPhys(input_buf);
    input_size = size;

    return 0;
}

static void dce_close(void)
{
    if (codec)      VIDDEC3_delete(codec);           codec      = NULL;
    if (engine)     Engine_close(engine);            engine     = NULL;
    if (params)     dce_free(params);                params     = NULL;
    if (dyn_params) dce_free(dyn_params);            dyn_params = NULL;
    if (status)     dce_free(status);                status     = NULL;
    if (inbufs)     dce_free(inbufs);                inbufs     = NULL;
    if (outbufs)    dce_free(outbufs);               outbufs    = NULL;
    if (in_args)    dce_free(in_args);               in_args    = NULL;
    if (out_args)   dce_free(out_args);              out_args   = NULL;
    if (input_buf)  MemMgr_Free(input_buf);          input_buf  = NULL;
    if (bsf)        av_bitstream_filter_close(bsf);  bsf        = NULL;
}

static int dce_open(const char *name, AVCodecContext *cc,
                    struct frame_format *ff)
{
    Engine_Error ec;
    XDAS_Int32 err;

    if (cc->codec_id != CODEC_ID_H264) {
        fprintf(stderr, "DCE: unsupported codec %d\n", cc->codec_id);
        return -1;
    }

    if (cc->extradata && cc->extradata_size > 0 && cc->extradata[0] == 1) {
        bsf = av_bitstream_filter_init("h264_mp4toannexb");
        if (!bsf)
            return -1;
        avc = cc;
    }

    ff->width  = ALIGN(cc->width  + 2*PADX, 128);
    ff->height = ALIGN(cc->height + 4*PADY, 16);
    ff->disp_x = 0;
    ff->disp_y = 0;
    ff->disp_w = cc->width;
    ff->disp_h = cc->height;
    ff->pixfmt = PIX_FMT_NV12;

    engine = Engine_open("ivahd_vidsvr", NULL, &ec);
    if (!engine) {
        fprintf(stderr, "Engine_open() failed\n");
        goto err;
    }

    params = dce_alloc(sizeof(*params));
    if (!params)
        return -1;
    params->size = sizeof(*params);

    params->maxWidth           = ALIGN(ff->disp_w, 16);
    params->maxHeight          = ALIGN(ff->disp_h, 16);
    params->maxFrameRate       = 30000;
    params->maxBitRate         = 10000000;
    params->dataEndianness     = XDM_BYTE;
    params->forceChromaFormat  = XDM_YUV_420SP;
    params->operatingMode      = IVIDEO_DECODE_ONLY;
    params->displayDelay       = IVIDDEC3_DISPLAY_DELAY_AUTO;
    params->displayBufsMode    = IVIDDEC3_DISPLAYBUFS_EMBEDDED;
    params->inputDataMode      = IVIDEO_ENTIREFRAME;
    params->metadataType[0]    = IVIDEO_METADATAPLANE_NONE;
    params->metadataType[1]    = IVIDEO_METADATAPLANE_NONE;
    params->metadataType[2]    = IVIDEO_METADATAPLANE_NONE;
    params->numInputDataUnits  = 0;
    params->outputDataMode     = IVIDEO_ENTIREFRAME;
    params->numOutputDataUnits = 0;
    params->errorInfoMode      = IVIDEO_ERRORINFO_OFF;

    codec = VIDDEC3_create(engine, "ivahd_h264dec", params);
    if (!codec) {
        fprintf(stderr, "VIDDEC3_create() failed\n");
        goto err;
    }

    dyn_params = dce_alloc(sizeof(*dyn_params));
    if (!dyn_params)
        goto err;
    dyn_params->size = sizeof(*dyn_params);

    dyn_params->decodeHeader  = XDM_DECODE_AU;
    dyn_params->displayWidth  = 0;
    dyn_params->frameSkipMode = IVIDEO_NO_SKIP;
    dyn_params->newFrameFlag  = XDAS_TRUE;

    status = dce_alloc(sizeof(*status));
    if (!status)
        goto err;
    status->size = sizeof(*status);

    err = VIDDEC3_control(codec, XDM_SETPARAMS, dyn_params, status);
    if (err) {
        fprintf(stderr, "VIDDEC3_control(XDM_SETPARAMS) failed %d\n", err);
        goto err;
    }

    err = VIDDEC3_control(codec, XDM_GETBUFINFO, dyn_params, status);
    if (err) {
        fprintf(stderr, "VIDDEC3_control(XDM_GETBUFINFO) failed %d\n", err);
        goto err;
    }

    if (alloc_input(16384))
        goto err;

    inbufs = dce_alloc(sizeof(*inbufs));
    if (!inbufs)
        goto err;

    inbufs->numBufs = 1;
    inbufs->descs[0].memType = XDM_MEMTYPE_RAW;

    outbufs = dce_alloc(sizeof(*outbufs));
    if (!outbufs)
        goto err;

    outbufs->numBufs = 2;

    outbufs->descs[0].memType = XDM_MEMTYPE_TILED8;
    outbufs->descs[0].bufSize.tileMem.width  = ff->width;
    outbufs->descs[0].bufSize.tileMem.height = ff->height;

    outbufs->descs[1].memType = XDM_MEMTYPE_TILED16;
    outbufs->descs[1].bufSize.tileMem.width  = ff->width;
    outbufs->descs[1].bufSize.tileMem.height = ff->height / 2;

    in_args = dce_alloc(sizeof(*in_args));
    if (!in_args)
        goto err;
    in_args->size = sizeof(*in_args);

    out_args = dce_alloc(sizeof(*out_args));
    if (!out_args)
        goto err;
    out_args->size = sizeof(*out_args);

    return 0;
err:
    dce_close();
    return -1;
}

static int dce_decode(AVPacket *p)
{
    struct frame *f;
    uint8_t *buf;
    int bufsize;
    int err;
    int i;

    if (bsf) {
        if (av_bitstream_filter_filter(bsf, avc, NULL, &buf, &bufsize,
                                       p->data, p->size, 0) < 0) {
            fprintf(stderr, "DCE: bsf error\n");
            return -1;
        }
    } else {
        buf     = p->data;
        bufsize = p->size;
    }

    if (alloc_input(bufsize))
        return -1;

    memcpy(input_buf, buf, bufsize);

    if (bsf)
        av_free(buf);

    f = ofbp_get_frame();

    if (!f->phys[0]) {
        f->phys[0] = (uint8_t*)TilerMem_VirtToPhys(f->virt[0]);
        f->phys[1] = (uint8_t*)TilerMem_VirtToPhys(f->virt[1]);
    }

    in_args->inputID  = (XDAS_Int32)f;
    in_args->numBytes = bufsize;

    inbufs->descs[0].buf = (int8_t*)input_phys;
    inbufs->descs[0].bufSize.bytes = bufsize;

    outbufs->descs[0].buf = (int8_t*)f->phys[0];
    outbufs->descs[1].buf = (int8_t*)f->phys[1];

    err = VIDDEC3_process(codec, inbufs, outbufs, in_args, out_args);
    if (err) {
        fprintf(stderr, "VIDDEC3_process() error %d %08x\n", err,
                    out_args->extendedError);
        /* for non-fatal errors, keep going.. a non-fatal error could
         * just indicate an error in the input stream which the codec
         * was able to conceal
         */
        if (XDM_ISFATALERROR(out_args->extendedError))
            return -1;
    }

    for (i = 0; i < out_args->outputID[i]; i++) {
        XDM_Rect *r = &out_args->displayBufs.bufDesc[0].activeFrameRegion;
        f->x = r->topLeft.x;
        f->y = r->topLeft.y;
        ofbp_post_frame((struct frame *)out_args->outputID[i]);
    }

    for (i = 0; out_args->freeBufID[i]; i++)
        ofbp_put_frame((struct frame *)out_args->freeBufID[i]);

    return 0;
}

CODEC(avcodec) = {
    .name   = "dce",
    .flags  = OFBP_PHYS_MEM,
    .open   = dce_open,
    .decode = dce_decode,
    .close  = dce_close,
};
