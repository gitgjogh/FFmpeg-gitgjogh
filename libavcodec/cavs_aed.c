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

#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/timer.h"
#include "avcodec.h"
#include "get_bits.h"
#include "golomb.h"
#include "cavs.h"
#include "cavs_aed.h"
//#include "cabac_functions.h"
#include "internal.h"
#include "mpeg12data.h"


enum SyntaxElement {
    MB_SKIP_RUN = 0,
    MB_TYPE,
    MB_PART_TYPE,
    INTRA_LUMA_PRED_MODE,
    INTRA_CHROMA_PRED_MODE,
    MB_REFERENCE_INDEX,
    MV_DIFF_X,
    MV_DIFF_Y,
    CBP,
    MB_QP_DELTA,
    FRAME_LUMA,
    FRAME_CHROMA,
    FIELD_LUMA,
    FIELD_CHROMA,
    WEIGHTING_PREDICTION,
}

enum CtxIdxOffset {
    OFST_OF_MB_SKIP_RUN     = 0,  
    OFST_OF_MB_TYPE         = 4,  
    OFST_OF_MB_PART_TYPE    = 19, 
    OFST_OF_IPMODE_LUMA     = 22, 
    OFST_OF_IPMODE_CHROMA   = 26, 
    OFST_OF_MB_REF_INDEX    = 30, 
    OFST_OF_MV_DIFF_X       = 36, 
    OFST_OF_MV_DIFF_Y       = 42, 
    OFST_OF_CBP             = 48, 
    OFST_OF_MB_QP_DELTA     = 54, 
    OFST_OF_FRAME_LUMA      = 58, 
    OFST_OF_FRAME_CHROMA    = 124,
    OFST_OF_FIELD_LUMA      = 190,
    OFST_OF_FIELD_CHROMA    = 256,
    OFST_OF_WEIGHT_PRED     = 322,
};

bool cavs_aed_decision(AEContext *c, unsigned int predMps, unsigned int lgPmps)
{
    unsigned int rS1     = c->rS1;
    unsigned int rT1     = c->rT1;
    unsigned int valueS  = c->valueS;
    unsigned int valueT  = c->valueT;
    unsigned int binVal;
    unsigned int sFlag, rS2, rT2;

    if (rT1 >= (lgPmps >> 2)) {
        rS2 = rS1;
        rT2 = rT1 - (lgPmps >> 2);
        sFlag = 0;
    } else {
        rS2 = rS1 + 1;
        rT2 = 256 + rT1 - (lgPmps >> 2);
        sFlag = 1;
    }

    if (rS2 > valueS || (rS2 == valueS && valueT >= rT2)) {
        register unsigned int tRlps = (lgPmps >> 2) 
                                    + (sFlag == 0) ? 0 : rT1;
        
        binVal = !predMps;

        if (rS2 == valueS)
            valueT = valueT - rT2;
        else
            valueT = 256 + ((valueT << 1) | get_bits(c->pb, 1)) - rT2;

        while (tRlps < 0x100) {
            tRlps = tRlps << 1;
            valueT = (valueT << 1) | get_bits(c->pb, 1);
        }

        c->rS1 = 0;
        c->rT1 = tRlps & 0xFF;
        c->valueS = 0;
        while (valueT < 0x100) {
            valueS++;
            valueT = (valueT << 1) | get_bits(c->pb, 1);
        }
        c->valueT = valueT & 0xFF;
    } else {
        binVal = predMps;
        c->rS1 = rS2;
        c->rT1 = rT2;
    }

    return (binVal);
}

int cavs_aed_update_ctx(bool binVal, AEState *ctx)
{
    unsigned int lgPmps = ctx->lgPmps;
    
    int cwr = 5;
    if (ctx->cycno <= 1)
        cwr = 3;
    else if (ctx->cycno == 2)
        cwr = 4;

    if (binVal != ctx->mps) {
        if (ctx->cycno <= 2)
            ctx->cycno += 1;
        else
            ctx->cycno = 3;
    } else if (ctx->cycno == 0)
        ctx->cycno = 1;

    if (binVal == ctx->mps)
        ctx->lgPmps = lgPmps - (lgPmps >> cwr) - (lgPmps >> (cwr + 2));
    else {
        int lgPmps_upd[] = {46, 46, 46, 197, 95, 46};
        ctx->lgPmps += lgPmps_upd[cwr];
        
        if (ctx->lgPmps > 1023) {
            ctx->lgPmps = 2047 - ctx->lgPmps;
            ctx->mps = !(ctx->mps);
        }
    }
    return;
}

bool cavs_aed_bypass(AEContext *c)
{
    return cavs_aed_decision(c, 0, 1023);
}

bool cavs_aed_stuffing_bit()
{
    return cavs_aed_decision(c, 0, 4);
}

bool cavs_aed_symbol(AEContext *c, AEState *ctx)
{
    bool binVal = cavs_aed_decision(c, ctx->mps, ctx->lgPmps);
    cavs_aed_update_ctx(binVal, ctx);
    return (binVal);
}

bool cavs_aed_symbolW(AEContext *c, AEState *ctx1, AEState *ctx2)
{
    int predMps = ctx1->mps;
    int lgPmps = (ctx1->lgPmps + ctx2->lgPmps) / 2;
    
    if (ctx1->mps != ctx2->mps) {
        if (ctx1->lgPmps < ctx2->lgPmps) {
            predMps = ctx1->mps;
            lgPmps = 1023 - ((ctx2->lgPmps - ctx1->lgPmps) >> 1);
        } else {
            predMps = ctx2->mps;
            lgPmps = 1023 - ((ctx1->lgPmps - ctx2->lgPmps) >> 1);
        }
    }
    
    bool binVal = cavs_aed_decision(c, predMps, lgPmps);
    cavs_aed_update_ctx(binVal, ctx1);
    cavs_aed_update_ctx(binVal, ctx2);

    return (binVal);
}

/**
 * 8.4.2 Initialization
 * @param buf_size size of buf in bits
 */
int ff_cavs_aed_init(AEContext *c, const uint8_t *buf, int buf_size){
    c->bytestream_start=
    c->bytestream= buf;
    c->bytestream_end= buf + buf_size;

    /* 8.4.2.1 */
    AEState *s = c->ae_state
    for (int i=0; i<AE_CTX_COUNT; ++i, ++s) {
        s->mps = s->cycno = 0;
        s->lgPmps = 1023
    }

    /* 8.4.2.2 */
    c->rS1 = 0;
    c->rT1 = 0xFF;
    c->valueS = 0;
    c->valueT = get_bits(c->pb, 9);
    while ( ! ((c->valueT >> 8) & 0x01) ) {
        c->valueT = (c->valueT << 1) | get_bits(c->pb, 1);
        c->valueS++;
    }
    c->valueT = c->valueT & 0xFF;
        
    return 0;
}

int ff_cavs_aed_mb_skip_run(AVSContext *h, AEContext *c)
{
    AEState *ctx = c->ae_state + OFST_OF_MB_SKIP_RUN;
    int binIdx = 0;
    while (!cavs_aed_symbol(c, ctx)) {
        ctx += (++binIdx <= 3);
    }
    return binIdx;
}

int ff_cavs_aed_mb_type_p(AVSContext *h, AEContext *c)
{
    AEState *ctx = c->ae_state + OFST_OF_MB_TYPE;
    int binIdx = 0;
    while (!cavs_aed_symbol(c, ctx)) {
        ctx += (++binIdx <= 4);
    }
    return binIdx;
}

int ff_cavs_aed_mb_type_b(AVSContext *h, AEContext *c)
{
    int a = (h->flags & A_AVAIL) && (h->left_type_B not in [P_SKIP, B_SKIP, B_DIRECT]);
    int b = (h->flags & B_AVAIL) && (h->top_type_B[h->mbx] not in [P_SKIP, B_SKIP, B_DIRECT]);
    
    AEState *ctx = c->ae_state + OFST_OF_MB_TYPE + a + b;
    int binIdx = 0;
    if (!cavs_aed_symbol(c, ctx)) {
        return 0;
    } 

    *ctx = c->ae_state + OFST_OF_MB_TYPE + 7 + (binIdx = 1);
    while (!cavs_aed_symbol(c, ctx)) {
        ctx += (++binIdx <= 7);
    }
    return binIdx;
}

int ff_cavs_aed_mb_part_type(AVSContext *h, AEContext *c)
{
    AEState *ctx = c->ae_state + OFST_OF_MB_PART_TYPE;
    int s = cavs_aed_symbol(c, ctx);
    s = (s<<1) | cavs_aed_symbol(c, ctx + 1 + s);
    return s;
}

int ff_cavs_aed_ipmode_luma(AVSContext *h, AEContext *c)
{
    /**   0   | 1
     *    1   | 0 1
     *    2   | 0 0 1
     *    3   | 0 0 0 1
     *    4   | 0 0 0 0
     * -------+---------
     * binIdx | 0 1 2 3
     */
    AEState *ctx = c->ae_state + OFST_OF_IPMODE_LUMA;
    int binIdx = 0;
    while (binIdx<=3 && !cavs_aed_symbol(c, ctx)) {
        // ctx += (++binIdx <= 3);
        ++ ctx;
        ++ binIdx;
    }
    return binIdx;
}

int ff_cavs_aed_ipmode_chroma(AVSContext *h, AEContext *c)
{
    int a = (h->flags & A_AVAIL) && (h->left_pred_C != INTRA_C_LP);
    int b = (h->flags & B_AVAIL) && (h->top_pred_C[h->mbx] != INTRA_C_LP);
    int ctxIdxInc = a+b;    // for binIdx==0
    
    /**   0   | 0
     *    1   | 1 0
     *    2   | 1 1 0
     *    3   | 1 1 1
     * -------+-------
     * binIdx | 0 1 2
     */
    AEState *ctx = c->ae_state + OFST_OF_IPMODE_CHROMA;
    int binIdx = 0;
    while (binIdx<=2 && cavs_aed_symbol(c, ctx + ctxIdxInc)) {
        ++ binIdx;
        ctxIdxInc = 3;;
    }
    return binIdx;
}

int ff_cavs_aed_mb_ref_idx_p(AVSContext *h, AEContext *c)
{
    AEState *ctx = c->ae_state + OFST_OF_MB_REF_INDEX;
    int binIdx = 0;
    int nbit = 1 + h->field;
}

int ff_cavs_aed_mb_ref_idx_b(AVSContext *h, AEContext *c)
{
}

int ff_cavs_aed_mvdx(AVSContext *h, AEContext *c)
{
}

int ff_cavs_aed_mvdy(AVSContext *h, AEContext *c)
{
}

int ff_cavs_aed_cbp(AVSContext *h, AEContext *c)
{
}

int ff_cavs_aed_mb_qp_delta(AVSContext *h, AEContext *c)
{
}

int ff_cavs_aed_coeff_frame_luma(AVSContext *h, AEContext *c)
{
}

int ff_cavs_aed_coeff_frame_chroma(AVSContext *h, AEContext *c)
{
}

int ff_cavs_aed_coeff_field_luma(AVSContext *h, AEContext *c)
{
}

int ff_cavs_aed_coeff_field_chroma(AVSContext *h, AEContext *c)
{
}

int ff_cavs_aed_weight_pred(AVSContext *h, AEContext *c)
{
}

