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
 * DRM watermarks extracting.
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
#include "vf_drm.h"

typedef struct DrmDecContext {
    const AVClass *class;

    int     dm_step;
    int     xshift;
    int     yshift;
    int     drmw;
    int     drmh;
} DrmDecContext;


#define OFFSET(x) offsetof(DrmDecContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
static const AVOption drmDec_options[] = {
    { "step",  "dither module step",  OFFSET(dm_step),  AV_OPT_TYPE_INT, {.i64=DEFAULT_DCDM_STEP}, 0, 256, FLAGS },
    { "xshift", "where to embedding on the main pic", OFFSET(xshift), AV_OPT_TYPE_INT, {.i64=0}, 0, 4096, FLAGS },
    { "yshift", "where to embedding on the main pic", OFFSET(yshift), AV_OPT_TYPE_INT, {.i64=0}, 0, 4096, FLAGS },
    { "drmw", "drm watermark width",  OFFSET(drmw),  AV_OPT_TYPE_INT, {.i64=32}, 0, 8192, FLAGS },
    { "drmh", "drm watermark height", OFFSET(drmh),  AV_OPT_TYPE_INT, {.i64=32}, 0, 8192, FLAGS },
    { NULL }
};
AVFILTER_DEFINE_CLASS(drmDec);


static av_cold int init(AVFilterContext *ctx)
{
    //DrmDecContext *s = ctx->priv;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    //DrmDecContext *s = ctx->priv;

}

static int query_formats(AVFilterContext *ctx)
{
    //DrmDecContext *s = ctx->priv;

    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV444P, 
        AV_PIX_FMT_NONE
    };

    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static int config_input(AVFilterLink *inlink)
{
    //AVFilterContext *ctx = inlink->dst;
    //DrmDecContext *s   = ctx->priv;
    
    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    DrmDecContext *s   = ctx->priv;

    outlink->w = s->drmw;
    outlink->h = s->drmh;
    outlink->time_base = ctx->inputs[0]->time_base;

    return 0;
}

void drm_plane_set(uint8_t *base, int linesize, int h, int val)
{
    for (int y=0; y<h; y++) {
        uint8_t *line = base + y * linesize;
        memset(line, val, linesize);
    }
}

static int do_extracting(AVFilterContext *ctx, AVFrame *in, AVFrame *drm)
{
    DrmDecContext *s = ctx->priv;

    av_log(NULL, AV_LOG_DEBUG, "%s() called\n", __FUNCTION__);

    int vsub = 0;
    switch (drm->format) {
    case AV_PIX_FMT_YUV420P: vsub = 1; break;
    case AV_PIX_FMT_YUV422P: vsub = 0; break;
    case AV_PIX_FMT_YUV444P: vsub = 0; break;
    default:
        return -1;
    }

    drm_plane_set(drm->data[1], drm->linesize[1], drm->height >> vsub, 128);
    drm_plane_set(drm->data[2], drm->linesize[2], drm->height >> vsub, 128);
    
    drm_plane_t mainpl = {
        in->data[0], in->linesize[0], in->width, in->height
    };

    drm_plane_t drmpl = {
        drm->data[0], drm->linesize[0], drm->width, drm->height
    };

    dcdm2(DRM_DECODING, &mainpl, &drmpl, s->dm_step, s->xshift, s->yshift);

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    DrmDecContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];

    AVFrame *drm = ff_get_video_buffer(outlink, s->drmw, s->drmh);

    if (!drm) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(drm, in);
    do_extracting(ctx, in, drm);
    av_frame_free(&in);
    return ff_filter_frame(outlink, drm);
}

static const AVFilterPad avfilter_vf_drmDec_inputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
        .filter_frame   = filter_frame,
        .config_props   = config_input,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_drmDec_outputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
        .config_props   = config_output,
    },
    { NULL }
};

AVFilter ff_vf_drmDec = {
    .name          = "drmDec",
    .description   = NULL_IF_CONFIG_SMALL("Weibo DRM watermarks extracting."),
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(DrmDecContext),
    .priv_class    = &drmDec_class,
    .query_formats = query_formats,
    .inputs        = avfilter_vf_drmDec_inputs,
    .outputs       = avfilter_vf_drmDec_outputs,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
};