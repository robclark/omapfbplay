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
#include <sdma.h>

#include "pixconv.h"
#include "util.h"

#define DMA4_CCR        0x00
#define DMA4_CLNK_CTRL  0x01
#define DMA4_CICR       0x02
#define DMA4_CSR        0x03
#define DMA4_CSDP       0x04
#define DMA4_CEN        0x05
#define DMA4_CFN        0x06
#define DMA4_CSSA       0x07
#define DMA4_CDSA       0x08
#define DMA4_CSEI       0x09
#define DMA4_CSFI       0x0a
#define DMA4_CDEI       0x0b
#define DMA4_CDFI       0x0c
#define DMA4_CSAC       0x0d
#define DMA4_CDAC       0x0e
#define DMA4_CCEN       0x0f
#define DMA4_CCFN       0x10
#define DMA4_COLOR      0x11

static SDMA_ChannelDescriptor sdmac[5];
static unsigned dest_stride;

static int sdma_open(const struct frame_format *ff,
                     const struct frame_format *df)
{
    unsigned w = ff->disp_w;
    unsigned h = ff->disp_h;
    unsigned yw = ff->y_stride;
    unsigned cw = ff->uv_stride;
    unsigned dw = 2 * ff->disp_w;
    int i;

    if (SDMA_init())
        return -1;

    if (SDMA_getChannels(5, sdmac))
        return -1;

    for (i = 0; i < 5; i++) {
        sdmac[i].addr[DMA4_CICR] = 0;
        sdmac[i].addr[DMA4_CSDP] = 1<<16 | 2<<7 | 1<<6;
        sdmac[i].addr[DMA4_CSEI] = 1;
        sdmac[i].addr[DMA4_CDEI] = 4;
        sdmac[i].addr[DMA4_CICR] = 0;
        sdmac[i].addr[DMA4_CCR]  = 3<<14 | 3<<12 | 1<<6;
        sdmac[i].addr[DMA4_CEN]  = w / 2;
        sdmac[i].addr[DMA4_CFN]  = h / 2;
        sdmac[i].addr[DMA4_CSFI] = cw - w/2   + 1;
        sdmac[i].addr[DMA4_CDFI] = 2*dw - 2*w + 4;
    }

    sdmac[0].addr[DMA4_CDEI] = 2;
    sdmac[0].addr[DMA4_CEN]  = w;
    sdmac[0].addr[DMA4_CFN]  = h;
    sdmac[0].addr[DMA4_CSFI] = yw - w   + 1;
    sdmac[0].addr[DMA4_CDFI] = dw - 2*w + 2;

    for (i = 0; i < 4; i++)
        sdmac[i].addr[DMA4_CLNK_CTRL] = 1<<15 | sdmac[i+1].chanNum;
    sdmac[4].addr[DMA4_CLNK_CTRL] = 0;
    sdmac[4].addr[DMA4_CICR]      = 1<<5;

    dest_stride = dw;

    return 0;
}

static void sdma_convert(uint8_t *vdst[3], uint8_t *vsrc[3],
                         uint8_t *pdst[3], uint8_t *psrc[3])
{
    uint8_t *yuv = pdst[0];
    uint8_t *y   = psrc[0];
    uint8_t *u   = psrc[1];
    uint8_t *v   = psrc[2];

    sdmac[0].addr[DMA4_CSSA] = (unsigned)y;
    sdmac[0].addr[DMA4_CDSA] = (unsigned)yuv;

    sdmac[1].addr[DMA4_CSSA] = (unsigned)u;
    sdmac[1].addr[DMA4_CDSA] = (unsigned)yuv + 1;

    sdmac[2].addr[DMA4_CSSA] = (unsigned)v;
    sdmac[2].addr[DMA4_CDSA] = (unsigned)yuv + 3;

    sdmac[3].addr[DMA4_CSSA] = (unsigned)u;
    sdmac[3].addr[DMA4_CDSA] = (unsigned)yuv + dest_stride + 1;

    sdmac[4].addr[DMA4_CSSA] = (unsigned)v;
    sdmac[4].addr[DMA4_CDSA] = (unsigned)yuv + dest_stride + 3;
    sdmac[4].transferState.transferCompleted = 0;

    asm volatile ("");
    sdmac[0].addr[DMA4_CCR]  = 3<<14 | 3<<12 | 1<<7 | 1<<6;
    asm volatile ("dmb");
}

static void sdma_finish(void)
{
    SDMA_wait(&sdmac[4]);
}

static void sdma_close(void)
{
    SDMA_freeChannels(5, sdmac);
    SDMA_exit();
}

DRIVER(pixconv, sdma) = {
    .name    = "sdma",
    .flags   = OFB_PHYS_MEM,
    .open    = sdma_open,
    .convert = sdma_convert,
    .finish  = sdma_finish,
    .close   = sdma_close,
};
