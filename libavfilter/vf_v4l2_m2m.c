/*
 * Copyright (c) 2014 Alexis Ballier
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

/*
 * Missing features:
 *  - get_buffer
 *  - effects (hflip, vflip, etc.)
 */
#include <sys/ioctl.h>

#include "libavcodec/avcodec.h"
#include "libavcodec/v4l2_m2m.h"
#include "libavcodec/v4l2-common.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"

static av_cold void uninit(AVFilterContext *ctx) {
    V4Lm2mContext *s = ctx->priv;
    avpriv_v4lm2m_end(s);
}

static av_cold int init(AVFilterContext *ctx) {
    V4Lm2mContext *s = ctx->priv;

    s->output_pool.default_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
    s->capture_pool.default_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;

    // For probing raw2raw device
    s->output_pool.av_codec_id = AV_CODEC_ID_RAWVIDEO;
    s->output_pool.av_pix_fmt = AV_PIX_FMT_NONE;
    s->output_pool_needs_format = 1;

    // We must have 1:1 input:output mapping, so we wait for the device to process data.
    s->capture_pool.blocking_dequeue = 100;

    // For probing raw2raw device
    s->capture_pool.av_codec_id = AV_CODEC_ID_RAWVIDEO;
    s->capture_pool.av_pix_fmt = AV_PIX_FMT_NONE;
    s->capture_pool_needs_format = 1;

    return avpriv_v4lm2m_init(s, ctx);
}

static int query_formats_local(AVFilterFormats **fmts, V4Lm2mContext *s, V4LBufferPool* bp) {
    int ret;
    AVFilterFormats *formats = NULL;
    struct v4l2_fmtdesc fmtdesc = { 0 };
    enum AVPixelFormat pixfmt;

    fmtdesc.type = bp->type;

    while(!ioctl(s->fd, VIDIOC_ENUM_FMT, &fmtdesc)) {
        pixfmt = avpriv_v4l_fmt_v4l2ff(fmtdesc.pixelformat, AV_CODEC_ID_RAWVIDEO);
        if(AV_PIX_FMT_NONE != pixfmt) {
            /* Avoid duplicates, lavfi really does not like it */
            if(formats && formats->nb_formats) {
                int i;
                for(i=0; i<formats->nb_formats; i++) {
                    if(formats->formats[i] == pixfmt)
                        break;
                }
                if(i<formats->nb_formats) {
                    goto ignore_format;
                }
            }

            if((ret = ff_add_format(&formats, pixfmt)) < 0) {
                ff_formats_unref(&formats);
                return ret;
            }
        }
ignore_format:
        fmtdesc.index++;
    }

    ff_formats_ref(formats, fmts);
    return 0;
}

static int query_formats(AVFilterContext *ctx) {
    int ret;
    V4Lm2mContext *s = ctx->priv;
    if(ctx->inputs[0] && (ret = query_formats_local(&(ctx->inputs[0]->out_formats), s, &s->output_pool)) < 0) {
        return ret;
    }
    if(ctx->inputs[0] && (ret = query_formats_local(&(ctx->outputs[0]->in_formats), s, &s->capture_pool)) < 0) {
        return ret;
    }
    return 0;
}

static inline int config_pool(V4LBufferPool *bp) {
    int ret;
    av_log(bp->log_ctx, AV_LOG_DEBUG, "Setting pixelformat %s on %s\n", av_get_pix_fmt_name(bp->av_pix_fmt), bp->name);
    if((ret = avpriv_set_pool_format(bp, 1)) < 0)
        return ret;
    return avpriv_init_v4lbufpool(bp);
}

static int config_input(AVFilterLink *link) {
    AVFilterContext     *ctx = link->dst;
    V4Lm2mContext         *s = ctx->priv;

    s->output_pool.width      = link->w;
    s->output_pool.height     = link->h;
    s->output_pool.av_pix_fmt = link->format;

    return config_pool(&s->output_pool);
}

static int config_output(AVFilterLink *link) {
    AVFilterContext     *ctx = link->src;
    V4Lm2mContext         *s = ctx->priv;

    if(!s->capture_pool.width)
        s->capture_pool.width  = ctx->inputs[0]->w;

    link->w = s->capture_pool.width;

    if(!s->capture_pool.height)
        s->capture_pool.height = ctx->inputs[0]->h;

    link->h = s->capture_pool.height;

    s->capture_pool.av_pix_fmt = link->format;

    return config_pool(&s->capture_pool);
}

static int filter_frame(AVFilterLink *link, AVFrame *pict) {
    AVFilterContext  *ctx = link->dst;
    V4Lm2mContext      *s = ctx->priv;
    AVFilterLink *outlink = link->dst->outputs[0];
    AVFrame *out = NULL;
    int ret;

    if(pict) {
        if((ret = avpriv_v4l_enqueue_frame_or_pkt_or_buf(&s->output_pool, pict, NULL, NULL, 0)) < 0)
            return ret;

        if(!s->output_pool.streamon && (ret = avpriv_set_stream_status(&s->output_pool, VIDIOC_STREAMON))) {
            av_log(ctx, AV_LOG_ERROR, "VIDIOC_STREAMON failed on output pool\n");
            return ret;
        }
        if(!s->capture_pool.streamon && (ret = avpriv_set_stream_status(&s->capture_pool, VIDIOC_STREAMON))) {
            av_log(ctx, AV_LOG_ERROR, "VIDIOC_STREAMON failed on capture pool\n");
            return ret;
        }
    } else {
        return 0;
    }

    av_frame_free(&pict);
    out = av_frame_alloc();

    if(!out) {
        return AVERROR(ENOMEM);
    }

    if(ret = avpriv_v4l_dequeue_frame_or_pkt(&(s->capture_pool), out, NULL)) {
        return ret;
    }

    return ff_filter_frame(outlink, out);
}

#define OFFSET(x) offsetof(V4Lm2mContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption m2m_options[] = {
    V4L_M2M_DEFAULT_OPTS,
    { "width",
        "Output video width",
        OFFSET(capture_pool.width),
        AV_OPT_TYPE_INT,
        {.i64 = 0 },
        0, INT_MAX, FLAGS },
    { "height",
        "Output video height",
        OFFSET(capture_pool.height),
        AV_OPT_TYPE_INT,
        {.i64 = 0 },
        0, INT_MAX, FLAGS },
    { "num_capture_pool_buffers",
        "Number of buffers in the capture pool",
        OFFSET(capture_pool.num_buffers),
        AV_OPT_TYPE_INT,
        {.i64 = 16 },
        4, INT_MAX, FLAGS },
    { NULL }
};

static const AVClass v4l2_m2m_class = {
    .class_name       = "v4l2_m2m_filter",
    .item_name        = av_default_item_name,
    .option           = m2m_options,
    .version          = LIBAVUTIL_VERSION_INT,
    .category         = AV_CLASS_CATEGORY_FILTER,
};

static const AVFilterPad avfilter_vf_v4l2_m2m_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_v4l2_m2m_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
    { NULL }
};

AVFilter ff_vf_v4l2_m2m = {
    .name           = "v4l2_m2m",
    .description    = NULL_IF_CONFIG_SMALL("V4L2 M2M filter: Scale the input video size and convert the image format."),
    .init           = init,
    .uninit         = uninit,
    .query_formats  = query_formats,
    .priv_size      = sizeof(V4Lm2mContext),
    .priv_class     = &v4l2_m2m_class,
    .inputs         = avfilter_vf_v4l2_m2m_inputs,
    .outputs        = avfilter_vf_v4l2_m2m_outputs,
};
