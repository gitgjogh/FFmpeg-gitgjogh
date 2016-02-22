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
#include "cavsae.h"
//#include "cabac_functions.h"
#include "internal.h"
#include "mpeg12data.h"

/**
 * number of bin by SyntaxElement.
 */
av_unused static const int8_t num_bins_in_se[] = {
     1, // sao_merge_flag
     1, // sao_type_idx
     0, // sao_eo_class
     0, // sao_band_position
     0, // sao_offset_abs
     0, // sao_offset_sign
     0, // end_of_slice_flag
     3, // split_coding_unit_flag
     1, // cu_transquant_bypass_flag
     3, // skip_flag
     3, // cu_qp_delta
     1, // pred_mode
     4, // part_mode
     0, // pcm_flag
     1, // prev_intra_luma_pred_mode
     0, // mpm_idx
     0, // rem_intra_luma_pred_mode
     2, // intra_chroma_pred_mode
     1, // merge_flag
     1, // merge_idx
     5, // inter_pred_idc
     2, // ref_idx_l0
     2, // ref_idx_l1
     2, // abs_mvd_greater0_flag
     2, // abs_mvd_greater1_flag
     0, // abs_mvd_minus2
     0, // mvd_sign_flag
     1, // mvp_lx_flag
     1, // no_residual_data_flag
     3, // split_transform_flag
     2, // cbf_luma
     4, // cbf_cb, cbf_cr
     2, // transform_skip_flag[][]
     2, // explicit_rdpcm_flag[][]
     2, // explicit_rdpcm_dir_flag[][]
    18, // last_significant_coeff_x_prefix
    18, // last_significant_coeff_y_prefix
     0, // last_significant_coeff_x_suffix
     0, // last_significant_coeff_y_suffix
     4, // significant_coeff_group_flag
    44, // significant_coeff_flag
    24, // coeff_abs_level_greater1_flag
     6, // coeff_abs_level_greater2_flag
     0, // coeff_abs_level_remaining
     0, // coeff_sign_flag
     8, // log2_res_scale_abs
     2, // res_scale_sign_flag
     1, // cu_chroma_qp_offset_flag
     1, // cu_chroma_qp_offset_idx
};

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

/**
 * Offset to ctxIdx 0 in init_values and states, indexed by SyntaxElement.
 */
static const int elem_offset[sizeof(num_bins_in_se)] = {
    0,      // mb_skip_run
    4,      // mb_type
    19,     // mb_part_type
    22,     // intra_luma_pred_mode
    26,     // intra_chroma_pred_mode
    30,     // mb_reference_index
    36,     // mv_diff_x
    42,     // mv_diff_y
    48,     // cbp
    54,     // mb_qp_delta
    58,     // frame_luma
    124,    // frame_chroma
    190,    // field_luma
    256,    // field_chroma
    322,    // weighting_prediction
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
    //int predMps = ctx->mps;
    //int lgPmps = ctx->lgPmps;
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

int ff_cavs_aed_mbskip_run(AVSContext *h)
{
}

int ff_cavs_aed_mbtype(AVSContext *h)
{
}

int ff_cavs_aed_mb_part_type(AVSContext *h)
{
}

int ff_cavs_aed_intra_luma_pred_mode(AVSContext *h)
{
}

int ff_cavs_aed_intra_chroma_pred_mode(AVSContext *h)
{
}

int ff_cavs_aed_mbref_idx(AVSContext *h)
{
}

int ff_cavs_aed_mvdx(AVSContext *h)
{
}

int ff_cavs_aed_mvdy(AVSContext *h)
{
}

int ff_cavs_aed_cbp(AVSContext *h)
{
}

int ff_cavs_aed_mb_qp_delta(AVSContext *h)
{
}

int ff_cavs_aed_coeff_frame_luma(AVSContext *h)
{
}

int ff_cavs_aed_coeff_frame_chroma(AVSContext *h)
{
}

int ff_cavs_aed_coeff_field_luma(AVSContext *h)
{
}

int ff_cavs_aed_coeff_field_chroma(AVSContext *h)
{
}

int ff_cavs_aed_weight_pred(AVSContext *h)
{
}

