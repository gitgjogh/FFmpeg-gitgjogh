/*
 * Chinese AVS+ video (AVS1-P16,  Broadcasting profile) ae(v) decoder.
 * Copyright (c) 2016 JianfengZheng <163jogh@163.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Chinese AVS+ video (AVS1-P16,  Broadcasting profile) ae(v) decoder.
 * @author JianfengZheng <163jogh@163.com>
 */

#ifndef AVCODEC_CAVSAE_H
#define AVCODEC_CAVSAE_H

#include <stdint.h>

extern const uint8_t ff_cavsae_tables[512 + 4*2*64 + 4*64 + 63];
#define CAVS_NORM_SHIFT_OFFSET 0
#define CAVS_LPS_RANGE_OFFSET 512
#define CAVS_MLPS_STATE_OFFSET 1024
#define CAVS_LAST_COEFF_FLAG_OFFSET_8x8_OFFSET 1280

#define AE_BITS 16
#define AE_CYCNO_BITS 2
#define AE_LGPMPS_BITS 11
#define AE_LGPMPS_MASK ((1<<AE_LGPMPS_BITS)-1)
#define AE_CTX_COUNT 323

typedef struct AEState{
    unsigned int mps     :1;
    unsigned int cycno   :2;
    unsigned int lgPmps  :11;
} AEState;

typedef struct AEContext{
    AEState ae_state[AE_CTX_COUNT];
    
    unsigned int rS1;                    /*  8-bits */
    unsigned int rT1;                    /*  8-bits */
    unsigned int valueS;                 /* 32-bits */
    unsigned int valueT;                 /*  9-bits */
    const uint8_t *bytestream_start;
    const uint8_t *bytestream;
    const uint8_t *bytestream_end;
    GetBitContext pb;
} AEContext;

typedef struct CoeffContext{
    AEState *stBase;
    int pos, lMax, priIdx, level;
} CoeffContext;

int ff_init_cavsae_decoder(AEContext *c, const uint8_t *buf, int buf_size);

#endif /* AVCODEC_CAVSAE_H */

