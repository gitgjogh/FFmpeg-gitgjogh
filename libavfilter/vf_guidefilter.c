/*
 * Copyright (c) 2002 Michael Niedermayer <michaelni@gmx.at>
 * Copyright (c) 2011 Stefano Sabatini
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file
 * Apply a boxblur filter to the input video.
 * Ported from MPlayer libmpcodecs/vf_boxblur.c.
 */

#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

void boxfilter(uint8_t *dst, int dst_linesize, const uint8_t *src, int src_linesize,
                  int w, int h, int radius, int power, uint8_t *temp[2], int pixsize);

static inline void blur8(uint8_t *dst, int dst_step, const uint8_t *src, int src_step,
                        int len, int radius)
{
    /* Naive boxblur would sum source pixels from x-radius .. x+radius
     * for destination pixel x. That would be O(radius*width).
     * If you now look at what source pixels represent 2 consecutive
     * output pixels, then you see they are almost identical and only
     * differ by 2 pixels, like:
     * src0       111111111
     * dst0           1
     * src1        111111111
     * dst1            1
     * src0-src1  1       -1
     * so when you know one output pixel you can find the next by just adding
     * and subtracting 1 input pixel.
     * The following code adopts this faster variant.
     */
    const int length = radius*2 + 1;
    const int inv = ((1<<16) + length/2)/length;
    int x, sum = src[radius*src_step];

    //av_log(0, AV_LOG_ERROR, "%s, dst_step=%d, src_step=%d\n", 
    //    __FUNCTION__, dst_step, src_step);

    for (x = 0; x < radius; x++)
        sum += src[x*src_step]<<1;

    sum = sum*inv + (1<<15);

    for (x = 0; x <= radius; x++) {
        sum += (src[(radius+x)*src_step] - src[(radius-x)*src_step])*inv;
        dst[x*dst_step] = sum>>16;
    }

    for (; x < len-radius; x++) {
        sum += (src[(radius+x)*src_step] - src[(x-radius-1)*src_step])*inv;
        dst[x*dst_step] = sum >>16;
    }

    for (; x < len; x++) {
        sum += (src[(2*len-radius-x-1)*src_step] - src[(x-radius-1)*src_step])*inv;
        dst[x*dst_step] = sum>>16;
    }
}

static inline void blur16(uint16_t *dst, int dst_step, const uint16_t *src, int src_step,
                          int len, int radius)
{
    const int length = radius*2 + 1;
    const int inv = ((1<<16) + length/2)/length;
    int x, sum = src[radius*src_step];

    //av_log(0, AV_LOG_ERROR, "%s, dst_step=%d, src_step=%d\n", 
    //    __FUNCTION__, dst_step, src_step);

    for (x = 0; x < radius; x++)
        sum += src[x*src_step]<<1;

    sum = sum*inv + (1<<15);

    for (x = 0; x <= radius; x++) {
        sum += (src[(radius+x)*src_step] - src[(radius-x)*src_step])*inv;
        dst[x*dst_step] = sum>>16;
    }

    for (; x < len-radius; x++) {
        sum += (src[(radius+x)*src_step] - src[(x-radius-1)*src_step])*inv;
        dst[x*dst_step] = sum >>16;
    }

    for (; x < len; x++) {
        sum += (src[(2*len-radius-x-1)*src_step] - src[(x-radius-1)*src_step])*inv;
        dst[x*dst_step] = sum>>16;
    }
}

static inline void blur32f(float *dst, int dst_step, const float *src, int src_step,
                          int len, int radius)
{
    const int length = radius*2 + 1;
    int x;
    float sum = src[radius*src_step];

    //av_log(0, AV_LOG_ERROR, "%s, dst_step=%d, src_step=%d, len=%d\n", 
    //    __FUNCTION__, dst_step, src_step, len);

    for (x = 0; x < radius; x++)
        sum += src[x*src_step] * 2;

    for (x = 0; x <= radius; x++) {
        sum += (src[(radius+x)*src_step] - src[(radius-x)*src_step]);
        dst[x*dst_step] = sum / length;
    }

    for (; x < len-radius; x++) {
        sum += (src[(radius+x)*src_step] - src[(x-radius-1)*src_step]);
        dst[x*dst_step] = sum / length;
    }

    for (; x < len; x++) {
        sum += (src[(2*len-radius-x-1)*src_step] - src[(x-radius-1)*src_step]);
        dst[x*dst_step] = sum / length;
    }
}

static inline void blur(uint8_t *dst, int dst_step, const uint8_t *src, int src_step,
                        int len, int radius, int pixsize)
{
    if (pixsize == 1) 
        blur8 (dst, dst_step, src, src_step   , len, radius);
    else if (pixsize == 2) {
        blur16((uint16_t*)dst, dst_step>>1, (const uint16_t*)src, src_step>>1, len, radius);
    } else {
        blur32f((float*)dst, dst_step>>2, (const float*)src, src_step>>2, len, radius);
    }
}

static inline void blur_power(uint8_t *dst, int dst_step, const uint8_t *src, int src_step,
                              int len, int radius, int power, uint8_t *temp[2], int pixsize)
{
    uint8_t *a = temp[0], *b = temp[1];

    //av_log(0, AV_LOG_ERROR, "%s, dst_step=%d, src_step=%d, pixsize=%d\n", 
    //    __FUNCTION__, dst_step, src_step, pixsize);

    if (radius && power) {
        blur(a, pixsize, src, src_step, len, radius, pixsize);
        for (; power > 2; power--) {
            uint8_t *c;
            blur(b, pixsize, a, pixsize, len, radius, pixsize);
            c = a; a = b; b = c;
        }
        if (power > 1) {
            blur(dst, dst_step, a, pixsize, len, radius, pixsize);
        } else {
            int i;
            if (pixsize == 1) {
                for (i = 0; i < len; i++)
                    dst[i*dst_step] = a[i];
            } else if (pixsize == 2) {
                for (i = 0; i < len; i++)
                    *(uint16_t*)(dst + i*dst_step) = ((uint16_t*)a)[i];
            } else {
                for (i = 0; i < len; i++)
                    *(float*)(dst + i*dst_step) = ((float*)a)[i];
            }
        }
    } else {
        int i;
        if (pixsize == 1) {
            for (i = 0; i < len; i++)
                dst[i*dst_step] = src[i*src_step];
        } else if (pixsize == 2) {
            for (i = 0; i < len; i++)
                *(uint16_t*)(dst + i*dst_step) = *(uint16_t*)(src + i*src_step);
        } else {
            for (i = 0; i < len; i++)
                *(float*)(dst + i*dst_step) = ((float*)a)[i];
        }
    }
}

static void hblur(uint8_t *dst, int dst_linesize, const uint8_t *src, int src_linesize,
                  int w, int h, int radius, int power, uint8_t *temp[2], int pixsize)
{
    int y;

    if (radius == 0 && dst == src)
        return;

    for (y = 0; y < h; y++)
        blur_power(dst + y*dst_linesize, pixsize, src + y*src_linesize, pixsize,
                   w, radius, power, temp, pixsize);
}

/**
 * @param power [in] Specify how many times the boxblur filter is applied to the 
 *                   corresponding plane. A value of 0 will disable the effect.
 * @param temp [in] temporary buffer used in blur_power()
 *                      temp[i] = av_malloc(2*FFMAX(w, h))
 * @param pixsize [in] num of bytes each pixle used
 */
static void vblur(uint8_t *dst, int dst_linesize, const uint8_t *src, int src_linesize,
                  int w, int h, int radius, int power, uint8_t *temp[2], int pixsize)
{
    int x;

    if (radius == 0 && dst == src)
        return;

    for (x = 0; x < w; x++)
        blur_power(dst + x*pixsize, dst_linesize, src + x*pixsize, src_linesize,
                   h, radius, power, temp, pixsize);
}

void boxfilter(uint8_t *dst, int dst_linesize, const uint8_t *src, int src_linesize,
                  int w, int h, int radius, int power, uint8_t *temp[2], int pixsize)
{
    hblur(dst, dst_linesize, src, src_linesize, w, h, radius, power, temp, pixsize);
    vblur(dst, dst_linesize, dst, dst_linesize, w, h, radius, power, temp, pixsize);
}