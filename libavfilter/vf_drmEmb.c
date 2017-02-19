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
 * DRM watermarks embedding.
 * Quite same to overlay filter, except that 'overlayed' watermarks is invisable.
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


enum EOFAction {
    EOF_ACTION_REPEAT,
    EOF_ACTION_ENDALL,
    EOF_ACTION_PASS
};

static const char * const eof_action_str[] = {
    "repeat", "endall", "pass"
};

#define MAIN    0               ///< host picture
#define DRM     1               ///< drm wartermark

enum WbdrmEmbFormat {
    EMBOUT_FORMAT_YUV420,
    EMBOUT_FORMAT_YUV422,
    EMBOUT_FORMAT_YUV444,
    EMBOUT_FORMAT_RGB,
    EMBOUT_FORMAT_NB
};

static const enum AVPixelFormat alpha_pix_fmts[] = {
    AV_PIX_FMT_YUVA420P,
    AV_PIX_FMT_YUVA422P,
    AV_PIX_FMT_YUVA444P,
    AV_PIX_FMT_ARGB,
    AV_PIX_FMT_ABGR,
    AV_PIX_FMT_RGBA,
    AV_PIX_FMT_BGRA,
    AV_PIX_FMT_NONE
};


typedef struct DrmEmbContext {
    const AVClass *class;

    int     allow_packed_rgb;
    int     main_is_packed_rgb;
    uint8_t main_rgba_map[4];
    int     main_has_alpha;

    int     drm_is_packed_rgb;
    uint8_t drm_rgba_map[4];
    int     drm_has_alpha;

    int format;                 ///< output format

    FFDualInputContext dinput;
    
    int main_pix_step[4];       ///< steps per pixel for each plane of the main output
    int drm_pix_step[4];        ///< steps per pixel for each plane of the overlay
    int hsub, vsub;             ///< chroma subsampling values

    int eof_action;             ///< action to take on EOF from source


    int dm_step;
    int xshift;
    int yshift;
} DrmEmbContext;

#define OFFSET(x) offsetof(DrmEmbContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
static const AVOption drmEmb_options[] = {
    { "step",  "dither module step",  OFFSET(dm_step),  AV_OPT_TYPE_INT, {.i64=DEFAULT_DCDM_STEP}, 0, 256, FLAGS },
    { "xshift", "where to embedding on the main pic", OFFSET(xshift), AV_OPT_TYPE_INT, {.i64=0}, 0, 4096, FLAGS },
    { "yshift", "where to embedding on the main pic", OFFSET(yshift), AV_OPT_TYPE_INT, {.i64=0}, 0, 4096, FLAGS },
    { "eof_action", "Action to take when encountering EOF from DRM input ",
        OFFSET(eof_action), AV_OPT_TYPE_INT, { .i64 = EOF_ACTION_REPEAT },
        EOF_ACTION_REPEAT, EOF_ACTION_PASS, .flags = FLAGS, "eof_action" },
        { "repeat", "Repeat the previous frame.",   0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_REPEAT }, .flags = FLAGS, "eof_action" },
        { "endall", "End both streams.",            0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_ENDALL }, .flags = FLAGS, "eof_action" },
        { "pass",   "Pass through the main input.", 0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_PASS },   .flags = FLAGS, "eof_action" },
    //{ "rgb", "force packed RGB in input and output (deprecated)", OFFSET(allow_packed_rgb), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS },
    { "shortest", "force termination when the shortest input terminates", OFFSET(dinput.shortest), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, FLAGS },
    { "format", "set output format", OFFSET(format), AV_OPT_TYPE_INT, {.i64=EMBOUT_FORMAT_YUV420}, 0, EMBOUT_FORMAT_NB-1, FLAGS, "format" },
        { "yuv420", "", 0, AV_OPT_TYPE_CONST, {.i64=EMBOUT_FORMAT_YUV420}, .flags = FLAGS, .unit = "format" },
        //{ "yuv422", "", 0, AV_OPT_TYPE_CONST, {.i64=EMBOUT_FORMAT_YUV422}, .flags = FLAGS, .unit = "format" },
        //{ "yuv444", "", 0, AV_OPT_TYPE_CONST, {.i64=EMBOUT_FORMAT_YUV444}, .flags = FLAGS, .unit = "format" },
        //{ "rgb",    "", 0, AV_OPT_TYPE_CONST, {.i64=EMBOUT_FORMAT_RGB},   .flags = FLAGS, .unit = "format" },
    { "repeatlast", "repeat embedding of the last drm frame", OFFSET(dinput.repeatlast), AV_OPT_TYPE_INT, {.i64=1}, 0, 1, FLAGS },
    { NULL }
};
AVFILTER_DEFINE_CLASS(drmEmb);


static AVFrame *do_embedding(AVFilterContext *ctx, AVFrame *in, AVFrame *drm);

static av_cold int init(AVFilterContext *ctx)
{
    DrmEmbContext *s = ctx->priv;

    if (!s->dinput.repeatlast || s->eof_action == EOF_ACTION_PASS) {
        s->dinput.repeatlast = 0;
        s->eof_action = EOF_ACTION_PASS;
    }
    if (s->dinput.shortest || s->eof_action == EOF_ACTION_ENDALL) {
        s->dinput.shortest = 1;
        s->eof_action = EOF_ACTION_ENDALL;
    }

    s->dinput.process = do_embedding;
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    DrmEmbContext *s = ctx->priv;

    ff_dualinput_uninit(&s->dinput);
}

static int query_formats(AVFilterContext *ctx)
{
    DrmEmbContext *s = ctx->priv;

    /* overlay formats contains alpha, for avoiding conversion with alpha information loss */
    static const enum AVPixelFormat main_pix_fmts_yuv420[] = {
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVA420P, AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat drm_pix_fmts_yuv420[] = {
        AV_PIX_FMT_YUVA420P, AV_PIX_FMT_NONE
    };

    static const enum AVPixelFormat main_pix_fmts_yuv422[] = {
        AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVA422P, AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat drm_pix_fmts_yuv422[] = {
        AV_PIX_FMT_YUVA422P, AV_PIX_FMT_NONE
    };

    static const enum AVPixelFormat main_pix_fmts_yuv444[] = {
        AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVA444P, AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat drm_pix_fmts_yuv444[] = {
        AV_PIX_FMT_YUVA444P, AV_PIX_FMT_NONE
    };

    static const enum AVPixelFormat main_pix_fmts_rgb[] = {
        AV_PIX_FMT_ARGB,  AV_PIX_FMT_RGBA,
        AV_PIX_FMT_ABGR,  AV_PIX_FMT_BGRA,
        AV_PIX_FMT_RGB24, AV_PIX_FMT_BGR24,
        AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat drm_pix_fmts_rgb[] = {
        AV_PIX_FMT_ARGB,  AV_PIX_FMT_RGBA,
        AV_PIX_FMT_ABGR,  AV_PIX_FMT_BGRA,
        AV_PIX_FMT_NONE
    };

    AVFilterFormats *main_formats = NULL;
    AVFilterFormats *drm_formats = NULL;
    int ret;

    switch (s->format) {
    case EMBOUT_FORMAT_YUV420:
        if (!(main_formats = ff_make_format_list(main_pix_fmts_yuv420)) ||
            !(drm_formats  = ff_make_format_list(drm_pix_fmts_yuv420))) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        break;
    case EMBOUT_FORMAT_YUV422:
        if (!(main_formats = ff_make_format_list(main_pix_fmts_yuv422)) ||
            !(drm_formats  = ff_make_format_list(drm_pix_fmts_yuv422))) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        break;
    case EMBOUT_FORMAT_YUV444:
        if (!(main_formats = ff_make_format_list(main_pix_fmts_yuv444)) ||
            !(drm_formats  = ff_make_format_list(drm_pix_fmts_yuv444))) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        break;
    case EMBOUT_FORMAT_RGB:
        if (!(main_formats = ff_make_format_list(main_pix_fmts_rgb)) ||
            !(drm_formats  = ff_make_format_list(drm_pix_fmts_rgb))) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        break;
    default:
        av_assert0(0);
    }

    if ((ret = ff_formats_ref(main_formats, &ctx->inputs[MAIN]->out_formats)) < 0 ||
        (ret = ff_formats_ref(drm_formats,  &ctx->inputs[DRM]->out_formats )) < 0 ||
        (ret = ff_formats_ref(main_formats, &ctx->outputs[MAIN]->in_formats)) < 0)
            goto fail;

    return 0;

fail:
    if (main_formats)
        av_freep(&main_formats->formats);
    av_freep(&main_formats);
    if (drm_formats)
        av_freep(&drm_formats->formats);
    av_freep(&drm_formats);
    return ret;
}

static int config_input_main(AVFilterLink *inlink)
{
    DrmEmbContext *s = inlink->dst->priv;

    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(inlink->format);
    av_image_fill_max_pixsteps(s->main_pix_step, NULL, pix_desc);
    s->main_is_packed_rgb =ff_fill_rgba_map(s->main_rgba_map, inlink->format) >= 0;
    s->main_has_alpha = ff_fmt_is_in(inlink->format, alpha_pix_fmts);

    s->hsub = pix_desc->log2_chroma_w;
    s->vsub = pix_desc->log2_chroma_h;

    switch (s->format) {
    case EMBOUT_FORMAT_YUV420:
    case EMBOUT_FORMAT_YUV422:
    case EMBOUT_FORMAT_YUV444:
        break;
    //case EMBOUT_FORMAT_RGB:
    //    break;
    default:
        av_log(s, AV_LOG_ERROR, "Currently not support format other than YUV420\n");
        return -1;
    }
    return 0;
}

static int config_input_drm(AVFilterLink *inlink)
{
    DrmEmbContext *s = inlink->dst->priv;

    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(inlink->format);
    av_image_fill_max_pixsteps(s->drm_pix_step, NULL, pix_desc);
    s->drm_is_packed_rgb = ff_fill_rgba_map(s->drm_rgba_map, inlink->format) >= 0;
    s->drm_has_alpha = ff_fmt_is_in(inlink->format, alpha_pix_fmts);

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    DrmEmbContext *s = ctx->priv;

    int ret = ff_dualinput_init(ctx, &s->dinput);
    if (ret < 0)
        return ret;

    outlink->w = ctx->inputs[MAIN]->w;
    outlink->h = ctx->inputs[MAIN]->h;
    outlink->time_base = ctx->inputs[MAIN]->time_base;

    return 0;
}

static AVFrame *do_embedding(AVFilterContext *ctx, AVFrame *in, AVFrame *drm)
{
    DrmEmbContext *s = ctx->priv;

    av_log(NULL, AV_LOG_DEBUG, "%s() called\n", __FUNCTION__);

    drm_plane_t mainpl = {
        in->data[0], in->linesize[0], in->width, in->height
    };

    drm_plane_t drmpl = {
        drm->data[0], drm->linesize[0], drm->width, drm->height
    };

    dcdm2(DRM_EMBEDDING, &mainpl, &drmpl, s->dm_step, s->xshift, s->yshift);

    return in;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *inpicref)
{
    DrmEmbContext *s = inlink->dst->priv;

    av_log(inlink->dst, AV_LOG_DEBUG, "Incoming frame (time:%s) from link #%d\n", 
            av_ts2timestr(inpicref->pts, &inlink->time_base), 
            FF_INLINK_IDX(inlink));

    // do acture work in @do_embedding() which has been set to s->dinput.process
    return ff_dualinput_filter_frame(&s->dinput, inlink, inpicref);
}

static int request_frame(AVFilterLink *outlink)
{
    DrmEmbContext *s = outlink->src->priv;
    return ff_dualinput_request_frame(&s->dinput, outlink);
}

static const AVFilterPad avfilter_vf_drmEmb_inputs[] = {
    {
        .name         = "main",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_main,
        .filter_frame = filter_frame,
        .needs_writable = 1,
    },
    {
        .name         = "drm",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_drm,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_drmEmb_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter ff_vf_drmEmb = {
    .name          = "drmEmb",
    .description   = NULL_IF_CONFIG_SMALL("Weibo DRM watermarks embedding."),
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(DrmEmbContext),
    .priv_class    = &drmEmb_class,
    .query_formats = query_formats,
    .inputs        = avfilter_vf_drmEmb_inputs,
    .outputs       = avfilter_vf_drmEmb_outputs,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
};