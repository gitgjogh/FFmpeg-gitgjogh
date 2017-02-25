/**
 * Copyright (c) 2012~2017 Jeff <163jogh@163.com>
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
 * Implement of "Single Image Haze Removal Using Dark Channel Prior"
 */

#include "avfilter.h"
#include "formats.h"
#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/avstring.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/timestamp.h"
#include "internal.h"
#include "dualinput.h"
#include "drawutils.h"
#include "video.h"

#ifndef MIN
#define MIN(a, b) ((a)<(b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a)>(b) ? (a) : (b))
#endif

enum {
    DARK,
    TMAP,
    TMP,
    GRAYCOUNT,
};

enum {
    DEFOG_STEP_DARK_CHANNEL,
    DEFOG_STEP_TRANS_MAP,
    DEFOG_STEP_TMAP_REFINE,
    DEFOG_STEP_FINAL,
};

enum {
    IMG_I = 0,
    IMG_P,
    MEAN_I,
    MEAN_P,
    MUL_II,
    MUL_IP,
    MEAN_II,
    MEAN_IP,
    COV_II,
    COV_IP,
    COEF_A,
    COEF_B,
    MEAN_A,
    MEAN_B,
    GF_COUNT
};

typedef struct DefogContext {
    const AVClass *class;
    int     dbgstep;

    float   fogrsv;
    int     radius;
    float   percent;
    float   feps;
    int     gf_radius;

    AVFrame *rgb_tmp;
    AVFrame *gray_tmp;
    AVFrame *gray;      // for dark channel or transmission map
    AVFrame *gf[GF_COUNT];
} DefogContext;

#define OFFSET(x) offsetof(DefogContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
static const AVOption defog_options[] = {
    { "step", "debug step", OFFSET(dbgstep), AV_OPT_TYPE_INT, {.i64=DEFOG_STEP_FINAL}, 
        DEFOG_STEP_DARK_CHANNEL, DEFOG_STEP_FINAL, FLAGS, "step" },
        { "dark",   "", 0, AV_OPT_TYPE_CONST, { .i64 = DEFOG_STEP_DARK_CHANNEL }, .flags = FLAGS, "step" },
        { "trans",  "", 0, AV_OPT_TYPE_CONST, { .i64 = DEFOG_STEP_TRANS_MAP },    .flags = FLAGS, "step" },
        { "refine", "", 0, AV_OPT_TYPE_CONST, { .i64 = DEFOG_STEP_TMAP_REFINE},   .flags = FLAGS, "step" },
        { "final",  "", 0, AV_OPT_TYPE_CONST, { .i64 = DEFOG_STEP_FINAL },        .flags = FLAGS, "step" },
    { "fogrsv", "fog reserve factor",  OFFSET(fogrsv),  AV_OPT_TYPE_FLOAT, {.dbl=0.95}, 0, 1, FLAGS },
    { "radius", "minimal filter radius", OFFSET(radius), AV_OPT_TYPE_INT, {.i64=7}, 0, 30, FLAGS },
    { "percent", "percent of brightest pixels used to calculate atmospheric light", 
        OFFSET(percent), AV_OPT_TYPE_FLOAT, {.dbl=0.1}, 0.01, 1.0, FLAGS },
    { "gf_radius", "minimal filter radius", OFFSET(gf_radius), AV_OPT_TYPE_INT, {.i64=33}, 0, 300, FLAGS },
    { "feps", "incase zero dividen", OFFSET(feps), AV_OPT_TYPE_FLOAT, {.dbl=0.01}, 0.001, 0.1, FLAGS },
    { NULL }
};
AVFILTER_DEFINE_CLASS(defog);

static av_cold int init(AVFilterContext *ctx)
{
    DefogContext *s = ctx->priv;

    s->gray_tmp = 0;
    s->rgb_tmp = 0;
    s->gray = 0;
    for (int i=0; i<GF_COUNT; i++) {
        s->gf[i] = 0;
    }
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    DefogContext *s = ctx->priv;

    av_frame_free(&s->gray_tmp);
    av_frame_free(&s->rgb_tmp);
    av_frame_free(&s->gray);
    
    for (int i=0; i<GF_COUNT; i++) {
        av_frame_free(&s->gf[i]);
    }
}

static int query_formats(AVFilterContext *ctx)
{
    DefogContext *s = ctx->priv;

    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_RGB24,
        AV_PIX_FMT_BGR24,
        //AV_PIX_FMT_0RGB,
        //AV_PIX_FMT_RGB0,
        //AV_PIX_FMT_0BGR,
        //AV_PIX_FMT_BGR0,
        //AV_PIX_FMT_ARGB,
        //AV_PIX_FMT_RGBA,
        //AV_PIX_FMT_ABGR,
        //AV_PIX_FMT_BGRA,
        AV_PIX_FMT_NONE
    };

    if (s->dbgstep == DEFOG_STEP_FINAL) {
        AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
        if (!fmts_list)
            return AVERROR(ENOMEM);
        return ff_set_common_formats(ctx, fmts_list);
    }
    else {
        static const enum AVPixelFormat out_fmts[] = {
            AV_PIX_FMT_GRAY8,
            AV_PIX_FMT_NONE
        };

        int ret = 0;
        AVFilterFormats *iformats = ff_make_format_list(pix_fmts);
        AVFilterFormats *oformats = ff_make_format_list(out_fmts);
        if (!iformats || !oformats) {
            ret = AVERROR(ENOMEM);
            goto gray_out_fail;
        }
        if ((ret = ff_formats_ref(iformats, &ctx->inputs[0]->out_formats)) < 0 ||
            (ret = ff_formats_ref(oformats, &ctx->outputs[0]->in_formats) < 0)) {
            goto gray_out_fail;
        }
        return 0;

    gray_out_fail:
        av_log(ctx, AV_LOG_ERROR, "query_formats() failed\n");
        if (iformats)
            av_freep(&iformats->formats);
        av_freep(&iformats);
        if (oformats)
            av_freep(&oformats->formats);
        av_freep(&oformats);
        return ret;
    }
  
}

static int config_input(AVFilterLink *inlink)
{
    //AVFilterContext *ctx = inlink->dst;
    //DefogContext *s = ctx->priv;
    
    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    DefogContext *s = ctx->priv;
    int ret = 0;

    outlink->w = ctx->inputs[0]->w;
    outlink->h = ctx->inputs[0]->h;
    outlink->time_base = ctx->inputs[0]->time_base;

    s->gray = av_frame_alloc();
    if (!s->gray) {
        return AVERROR(ENOMEM);
    }
    s->gray->format = AV_PIX_FMT_GRAY8;
    s->gray->width  = outlink->w;
    s->gray->height = outlink->h;
    ret = av_frame_get_buffer(s->gray, 32);
    if (ret < 0) {
        av_frame_free(&s->gray);
        return ret;
    }

    s->gray_tmp = av_frame_alloc();
    if (!s->gray_tmp) {
        return AVERROR(ENOMEM);
    }
    s->gray_tmp->format = AV_PIX_FMT_GRAY8;
    s->gray_tmp->width  = outlink->w;
    s->gray_tmp->height = outlink->h;
    ret = av_frame_get_buffer(s->gray_tmp, 32);
    if (ret < 0) {
        av_frame_free(&s->gray_tmp);
        return ret;
    }

    s->rgb_tmp = av_frame_alloc();
    if (!s->rgb_tmp ) {
        return AVERROR(ENOMEM);
    }
    s->rgb_tmp->format = AV_PIX_FMT_RGB24;
    s->rgb_tmp->width  = outlink->w;
    s->rgb_tmp->height = outlink->h;
    ret = av_frame_get_buffer(s->rgb_tmp, 32);
    if (ret < 0) {
        av_frame_free(&s->rgb_tmp);
        return ret;
    }

    for (int i=0; i<GF_COUNT; i++) {
        s->gf[i] = av_frame_alloc();
        if (!s->gf[i] ) {
            return AVERROR(ENOMEM);
        }
        s->gf[i]->format = AV_PIX_FMT_BGRA;
        s->gf[i]->width  = outlink->w;
        s->gf[i]->height = outlink->h;
        ret = av_frame_get_buffer(s->gf[i], 64);
        if (ret < 0) {
            av_frame_free(&s->gf[i]);
            return ret;
        }        
    }

    return 0;
}

static uint8_t get_block_minimal(uint8_t *base, int stride, int l, int u, int r, int d)
{
    uint8_t m = 255;
    for (int y=u; y<=d; y++) {
        uint8_t *p = base + y * stride;
        for (int x=l; x<=r; x++) {
            uint8_t v = p[x];
            m = MIN(v, m);
        }
    }
    return m;
}

static void minimal_filter(AVFrame *src, AVFrame *dst, int radius)
{
    av_assert0(src->format == dst->format);
    av_assert0(dst->format == AV_PIX_FMT_GRAY8);

    uint8_t *src_base = src->data[0];   int src_stride = src->linesize[0];
    uint8_t *dst_base = dst->data[0];   int dst_stride = dst->linesize[0];
    int w = src->width - 1;
    int h = src->height - 1;

    for (int y=0; y<=h; y++) {
        //uint8_t *psrc = src_base + y * src_stride;
        uint8_t *pdst = dst_base + y * dst_stride;
        for (int x=0; x<=w; x++) {
            int l = x - radius;  l = MAX(0, l);
            int u = y - radius;  u = MAX(0, u);
            int r = x + radius;  r = MIN(w, r);
            int d = y + radius;  d = MIN(h, d);
            pdst[x] = get_block_minimal(src_base, src_stride, l, u, r, d);
        }
    }
}

static void get_dark_channel(DefogContext *s, AVFrame *in, AVFrame *dark)
{
    av_assert0(dark->format == AV_PIX_FMT_GRAY8);

    int w = in->width, h = in->height;
    uint8_t a, b, c, m;
    uint8_t *pi, *po;
    
    av_log(s, AV_LOG_DEBUG, "ifmt=%d,ofmt=%d,w=%d,h=%d,stride=%d,radius=%d\n", 
            in->format, dark->format, w, h, dark->linesize[0], s->radius);

    AVFrame *tmp = s->gray_tmp;
    for (int y=0; y<h; y++) {
        pi = in->data[0] + y * in->linesize[0];
        po = tmp->data[0] + y * tmp->linesize[0];
        for (int x=0; x<w; x++) {
            a = *pi++;
            b = *pi++;
            c = *pi++;
            m = a < b ? a : b;
            m = c < m ? c : m;
            *po++ = m;
        }
    }

    minimal_filter(tmp, dark, s->radius);
}

/**
 * @param in [in] the input (of this filter) rgb image
 * @param dark [in] the dark channel image of @in
 * @param A [out] 
 */
static int atmospheric_light(DefogContext *s, AVFrame *in, AVFrame *dark, uint8_t A[4])
{
    av_assert0(dark->format == AV_PIX_FMT_GRAY8);

    int w = in->width;
    int h = in->height;

    uint32_t hist[256] = {0};
    for (int y=0; y<h; y++) {
        uint8_t *pl = dark->data[0] + y * dark->linesize[0];
        for (int x=0; x<w; x++) {
            hist[ pl[x] ] += 1;
        }
    }

    for (int t=255; t>0; t--) {
        hist[t-1] += hist[t];       // accumulate
    }

    /**
     * get "brightest" threshold used for estimating atmospheric light
     */
    int threshold = 255;
    int count = (int)(w * h * s->percent / 100);
    for (int t=255; t>=0; t--) {
        if (hist[t] >= count) {
            threshold = t;
            count = hist[t];
            av_log(s, AV_LOG_DEBUG, "atmospheric: want=%f, get=%f, t=%d\n", 
                    s->percent, (float)count / (w * h) * 100, threshold);
            break;
        }
    }

    /**
     * get average light of the "brightest" points in seperate color
     */
    int nstep = 3;
    int sum[4] = {0};
    for (int y=0; y<h; y++) {
        uint8_t *pcl = in->data[0] + y * in->linesize[0];
        uint8_t *pl = dark->data[0] + y * dark->linesize[0];
        for (int x=0; x<w; x++) {
            if (pl[x] >= threshold) {
                uint8_t *pc = pcl + nstep * x;
                sum[0] += pc[0];
                sum[1] += pc[1];
                sum[2] += pc[2];
                sum[3] += pl[x];
            }
        }
    }

    for (int i=0; i<4; i++) {
        A[i] = (sum[i] + (count>>1)) / count;
    }
    av_log(s, AV_LOG_DEBUG, "atmospheric light: color=%d,%d,%d, dark=%d\n", 
            A[0], A[1], A[2], A[3]);

    return count;
}

static void transmission_map(DefogContext *s, AVFrame *in, AVFrame *tmap, uint8_t A[])
{
    av_assert0(tmap->format == AV_PIX_FMT_GRAY8);

    AVFrame *tmp = s->rgb_tmp;
    int w = in->width;
    int h = in->height;

    // tmp = rgb[] / A[]
    int src_step = 3, tmp_step = 3;
    for (int y=0; y<h; y++) {
        uint8_t *psrc = in->data[0] + y * in->linesize[0];
        uint8_t *ptmp = tmp->data[0] + y * tmp->linesize[0];
        for (int x=0; x<w; x++) {
            for (int c=0; c<3; c++) {
                ptmp[c] = psrc[c] >= A[c] ? 255 : (psrc[c] * 255 / A[c]);
            }

            psrc += src_step;
            ptmp += tmp_step;
        }
    }
    
    // t = 1 - w * dark_channel(tmp)
    get_dark_channel(s, tmp, tmap);
    uint16_t w255 = (uint16_t)(255 * s->fogrsv);
    for (int y=0; y<h; y++) {
        uint8_t *pmap = tmap->data[0] + y * tmap->linesize[0];
        for (int x=0; x<w; x++) {
            pmap[x] = 255 - (uint8_t)((w255 * pmap[x] + 127) >> 8);
        }
    }
}

/**
 * A = B * C;
 */
static void mat_dot_mul(AVFrame *A, AVFrame *B, AVFrame *C)
{
    int w = A->width;
    int h = A->height;

    for (int y=0; y<h; y++) {
        float *pA = (float*)(A->data[0] + y * A->linesize[0]);
        float *pB = (float*)(B->data[0] + y * B->linesize[0]);
        float *pC = (float*)(C->data[0] + y * C->linesize[0]);
        for (int x=0; x<w; x++) {
            pA[x] = pB[x] * pC[x];
        }
    }
}

/**
 * A = B - C * D;
 */
static void mat_dot_cov(AVFrame *A, AVFrame *B, AVFrame *C, AVFrame *D)
{
    int w = A->width;
    int h = A->height;

    for (int y=0; y<h; y++) {
        float *pA = (float*)(A->data[0] + y * A->linesize[0]);
        float *pB = (float*)(B->data[0] + y * B->linesize[0]);
        float *pC = (float*)(C->data[0] + y * C->linesize[0]);
        float *pD = (float*)(D->data[0] + y * D->linesize[0]);
        for (int x=0; x<w; x++) {
            pA[x] = pB[x] - pC[x] * pD[x];
        }
    }
}

/**
 * A = B / C;
 */
static void mat_dot_div(AVFrame *A, AVFrame *B, AVFrame *C)
{
    int w = A->width;
    int h = A->height;

    for (int y=0; y<h; y++) {
        float *pA = (float*)(A->data[0] + y * A->linesize[0]);
        float *pB = (float*)(B->data[0] + y * B->linesize[0]);
        float *pC = (float*)(C->data[0] + y * C->linesize[0]);
        for (int x=0; x<w; x++) {
            pA[x] = pB[x] / (pC[x] + 0.001);
        }
    }
}

/**
 * A = B * C + D;
 */
static void mat_dot_mul_add(AVFrame *A, AVFrame *B, AVFrame *C, AVFrame *D)
{
    int w = A->width;
    int h = A->height;

    for (int y=0; y<h; y++) {
        float *pA = (float*)(A->data[0] + y * A->linesize[0]);
        float *pB = (float*)(B->data[0] + y * B->linesize[0]);
        float *pC = (float*)(C->data[0] + y * C->linesize[0]);
        float *pD = (float*)(D->data[0] + y * D->linesize[0]);
        for (int x=0; x<w; x++) {
            pA[x] = pB[x] * pC[x] + pD[x];
        }
    }
}

static void convframe_rgb2f(AVFrame *byt, AVFrame *flt)
{
    int w = flt->width;
    int h = flt->height;

    uint8_t *baseR = byt->data[0];
    uint8_t *baseG = byt->data[0];
    uint8_t *baseB = byt->data[0];
    if (byt->format == AV_PIX_FMT_RGB24) {
        baseR += 0;
        baseG += 1;
        baseB += 2;
    } else if (byt->format == AV_PIX_FMT_BGR24) {
        baseB += 0;
        baseG += 1;
        baseR += 2;
    } else {
        baseR = baseG = baseB = byt->data[0];
    }

    for (int y=0; y<h; y++) {
        float *pf = (float*)(flt->data[0] + y * flt->linesize[0]);
        uint8_t *pR = baseR + y * byt->linesize[0];
        uint8_t *pG = baseG + y * byt->linesize[0];
        uint8_t *pB = baseB + y * byt->linesize[0];
        for (int x=0; x<w; x++) {
            pf[x] = (pR[x*3]*0.299 + pG[x*3]*0.587 + pB[x*3]*0.114)/255.0;
        }
    }
}

static void convframe_b2f(AVFrame *byt, AVFrame *flt)
{
    int w = flt->width;
    int h = flt->height;

    for (int y=0; y<h; y++) {
        float *pf = (float*)(flt->data[0] + y * flt->linesize[0]);
        uint8_t *pb = byt->data[0] + y * byt->linesize[0];
        for (int x=0; x<w; x++) {
            pf[x] = pb[x]/255.0;
        }
    }
}

static void convframe_f2b(AVFrame *flt, AVFrame *byt)
{
    int w = byt->width;
    int h = byt->height;

    for (int y=0; y<h; y++) {
        float *pf = (float*)(flt->data[0] + y * flt->linesize[0]);
        uint8_t *pb = byt->data[0] + y * byt->linesize[0];
        for (int x=0; x<w; x++) {
            pb[x] = pf[x] > 1.0 ? 255 : (
                pf[x] < 0 ? 0 : (uint8_t)(pf[x] * 255));
        }
    }  
}

void boxfilter(uint8_t *dst, int dst_linesize, const uint8_t *src, int src_linesize,
               int w, int h, int radius, int power, uint8_t *temp[2], int pixsize);

static void guidedfilter(DefogContext *s, AVFrame *guider /*gray input*/, AVFrame *guidee /*tmap*/)
{
    int w = guidee->width;
    int h = guidee->height;
    int radius = s->gf_radius;
    float temp_buf[2][4096];
    uint8_t *temp[2] = { (uint8_t*)(temp_buf[0]), (uint8_t*)(temp_buf[1]) };
    //int pixsize = 4;
    int power = 1;

    av_assert0(sizeof(float)==4);

    AVFrame *i      = s->gf[IMG_I];
    AVFrame *p      = s->gf[IMG_P];
    AVFrame *mean_i = s->gf[MEAN_I];
    AVFrame *mean_p = s->gf[MEAN_P];
    AVFrame *ii     = s->gf[MUL_II];
    AVFrame *ip     = s->gf[MUL_IP];
    AVFrame *mean_ii= s->gf[MEAN_II];
    AVFrame *mean_ip= s->gf[MEAN_IP];
    AVFrame *cov_ii = s->gf[COV_II];
    AVFrame *cov_ip = s->gf[COV_IP];
    AVFrame *a      = s->gf[COEF_A];
    AVFrame *b      = s->gf[COEF_B];
    AVFrame *mean_a = s->gf[MEAN_A];
    AVFrame *mean_b = s->gf[MEAN_B];

    convframe_rgb2f(guider, i);
    convframe_b2f(guidee, p);

#define BOXFILTER(dst, src, pixsize) \
    boxfilter((uint8_t*)dst->data[0], dst->linesize[0], \
              (uint8_t*)src->data[0], src->linesize[0], \
              w, h, radius, power, temp, pixsize);

    BOXFILTER(mean_i, i, 4);                        // 0 ~ 255
    BOXFILTER(mean_p, p, 4);                        // 0 ~ 255
    
    mat_dot_mul(ip, i, p);      // ip = i .* p;     // 0 ~ 65535
    mat_dot_mul(ii, i, i);      // ii = i .* i;     // 0 ~ 65535

    BOXFILTER(mean_ip, ip, 4);                      // 0 ~ 65535
    BOXFILTER(mean_ii, ii, 4);                      // 0 ~ 65535

    // cov_ip = mean_ip - mean_i .* mean_p;
    // cov_ii = mean_ii - mean_i .* mean_i;   
    mat_dot_cov(cov_ip, mean_ip, mean_i, mean_p);   // -65535 ~ 65535
    mat_dot_cov(cov_ii, mean_ii, mean_i, mean_i);   // -65535 ~ 65535

    // a = cov_ip ./ cov_ii;
    // b = mean_p - a .* mean_i;
    mat_dot_div(a, cov_ip, cov_ii);
    mat_dot_cov(b, mean_p, a, mean_i);

    BOXFILTER(mean_a, a, 4);
    BOXFILTER(mean_b, b, 4);

    // p = mean_a * i + mean_b;
    mat_dot_mul_add(p, mean_a, i, mean_b);

    convframe_f2b(p, guidee);
}

/**
 * dst(x) = (src(x) - A) / t(x) + A
 */
static void final_defog(DefogContext *s, AVFrame *in, AVFrame *tmap, uint8_t A[])
{
    #define CLIP_UINT8(a) ((a&(~255)) ? (-a)>>31 : a)

    int src_step = 3;
    int w = in->width;
    int h = in->height;
    for (int y=0; y<h; y++) {
        uint8_t *psrc = in->data[0] + y * in->linesize[0];
        uint8_t *pmap = tmap->data[0] + y * tmap->linesize[0];
        for (int x=0; x<w; x++) {
            for (int c=0; c<3; c++) {
                long dst = (psrc[c] - A[c]) * 255L / pmap[x] + A[c];
                psrc[c] = CLIP_UINT8(dst);
            }
            psrc += src_step;
        }
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    DefogContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];

    AVFrame *gray = s->gray;
    if (s->dbgstep != DEFOG_STEP_FINAL) {
        gray = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!gray) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(gray, in);
    }

    uint8_t airlight[4] = {220};
    get_dark_channel(s, in, gray);
    if (s->dbgstep >= DEFOG_STEP_TRANS_MAP) {
        atmospheric_light(s, in, gray, airlight);
        transmission_map(s, in, gray, airlight);
    }
    if (s->dbgstep >= DEFOG_STEP_TMAP_REFINE) {
        guidedfilter(s, in, gray);
    }
    if (s->dbgstep >= DEFOG_STEP_FINAL) {
        final_defog(s, in, gray, airlight);
    }

    if (s->dbgstep == DEFOG_STEP_FINAL) {
        return ff_filter_frame(outlink, in);
    } else {
        av_frame_free(&in);
        return ff_filter_frame(outlink, gray);
    }
}

static const AVFilterPad avfilter_vf_defog_inputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
        .filter_frame   = filter_frame,
        .config_props   = config_input,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_defog_outputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
        .config_props   = config_output,
    },
    { NULL }
};

AVFilter ff_vf_defog = {
    .name          = "defog",
    .description   = NULL_IF_CONFIG_SMALL("defog."),
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(DefogContext),
    .priv_class    = &defog_class,
    .query_formats = query_formats,
    .inputs        = avfilter_vf_defog_inputs,
    .outputs       = avfilter_vf_defog_outputs,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
};
