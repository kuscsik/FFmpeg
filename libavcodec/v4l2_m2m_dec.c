/*
 * V4L2 mem2mem decoders
 * Copyright (C) 2014 Alexis Ballier
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
 *   - seek (flush)
 *   - resolution change
 */

#include <sys/ioctl.h>

#include "libavutil/pixfmt.h"
#include "libavutil/pixdesc.h"
#include "avcodec.h"
#include "v4l2-buffers.h"
#include "v4l2_m2m.h"
#include "v4l2_m2m_avcodec.h"
#include "v4l2-common.h"
#include "libavutil/opt.h"

static int try_start(AVCodecContext *avctx) {
    struct v4l2_control ctrl;
    struct v4l2_crop crop;
    struct v4l2_cropcap cropcap;
    int ret;
    int cropcap_supported = 1;
    V4Lm2mContext *s = avctx->priv_data;
    memset (&cropcap, 0, sizeof (cropcap));

    cropcap.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

    if(!s->output_pool.streamon && (ret = avpriv_set_stream_status(&s->output_pool, VIDIOC_STREAMON) < 0)) {
        av_log(avctx, AV_LOG_ERROR, "VIDIOC_STREAMON failed on input pool\n");
        return ret;
    }

    s->capture_pool.format.type = s->capture_pool.type;

    if ( ret = ioctl (s->fd, VIDIOC_CROPCAP, &cropcap) ) {
        av_log(avctx, AV_LOG_ERROR, "Calling VIDIOC_CROPCAP ioctl failed\n");
        cropcap_supported = 0;
    }

    if(ret = ioctl(s->fd, VIDIOC_G_FMT, &s->capture_pool.format)) {
        av_log(avctx, AV_LOG_WARNING, "Failed to get output format\n");
        return ret;
    }

    crop.type = s->capture_pool.type;

    s->capture_pool.width      = avctx->width   = s->capture_pool.format.fmt.pix_mp.width;
    s->capture_pool.height     = avctx->height  = s->capture_pool.format.fmt.pix_mp.height;
    s->capture_pool.av_pix_fmt = avctx->pix_fmt = avpriv_v4l_fmt_v4l2ff(s->capture_pool.format.fmt.pix_mp.pixelformat, AV_CODEC_ID_RAWVIDEO);

    if(ret = ioctl(s->fd, VIDIOC_G_CROP, &crop)) {
        av_log(avctx, AV_LOG_WARNING, "Failed to get cropping information. Using default size. \n");

        if (cropcap_supported) {
            avctx->coded_width  = cropcap.defrect.width;
            avctx->coded_height = cropcap.defrect.height;
        } else {
            /* No crop information, using widht, height obtained from the format */
            avctx->coded_width  = s->capture_pool.format.fmt.pix_mp.width;
            avctx->coded_height = s->capture_pool.format.fmt.pix_mp.height;
        }
    } else {
        /* VIDIOC_G_CROP success, use the obtained crop information */
        avctx->coded_width  = crop.c.width;
        avctx->coded_height = crop.c.height;
    }


    ctrl.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    if(ret = ioctl(s->fd, VIDIOC_G_CTRL, &ctrl)) {
        av_log(avctx, AV_LOG_WARNING, "Failed to get minimum input packets for decoding. Assuming minimum 4 buffer in queued buffers\n");
          s->capture_pool.min_queued_buffers  = 4;
    } else {
          s->capture_pool.min_queued_buffers = ctrl.value;
    }
    /* Init capture pool after starting output pool */
    if(!s->capture_pool.buffers && (ret = avpriv_init_v4lbufpool(&(s->capture_pool)))) {
        av_log(avctx, AV_LOG_ERROR, "Failed to request output buffers\n");
        return ret;
    }

    if(ret = avpriv_set_stream_status(&s->capture_pool, VIDIOC_STREAMON)) {
        av_log(avctx, AV_LOG_ERROR, "VIDIOC_STREAMON failed on output pool\n");
        return ret;
    }

    return 0;
}

static av_cold int v4lm2m_decode_init(AVCodecContext *avctx) {
    int ret;
    V4Lm2mContext *s = avctx->priv_data;

    s->output_pool.av_pix_fmt  = AV_PIX_FMT_NONE;
    s->output_pool.av_codec_id = avctx->codec_id;
    s->output_pool_needs_format = 1;
    s->output_pool_needs_init   = 1;
    s->output_pool.default_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;

    s->capture_pool.av_pix_fmt = avctx->pix_fmt;
    s->capture_pool.av_codec_id = AV_CODEC_ID_RAWVIDEO;
    s->capture_pool.default_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;

    if(ret = ff_v4lm2m_codec_init(avctx))
        return ret;

    if( avctx->extradata &&
        avctx->extradata_size &&
        !avpriv_v4l_enqueue_frame_or_pkt_or_buf(&s->output_pool, NULL, NULL, avctx->extradata, avctx->extradata_size))
        try_start(avctx);

    return 0;
}


static int v4lm2m_decode_frame(AVCodecContext *avctx, void *data,
                             int *got_frame, AVPacket *avpkt) {
    int ret;
    V4Lm2mContext *s = avctx->priv_data;
    AVFrame *frame = data;
    *got_frame = 0;

    if( avpkt->size &&
        !s->capture_pool.streamon &&
        avctx->extradata &&
        avctx->extradata_size &&
        !avpriv_v4l_enqueue_frame_or_pkt_or_buf(&s->output_pool, NULL, NULL, avctx->extradata, avctx->extradata_size))
        try_start(avctx);

    /* Send regular packet, or one empty packet to notify EOS */
    if(avpkt->size || s->output_pool.streamon)
        if((ret = avpriv_v4l_enqueue_frame_or_pkt_or_buf(&s->output_pool, NULL, avpkt, NULL, 0)) < 0)
            return ret;

    if(!s->capture_pool.streamon)
        try_start(avctx);

    /* Got EOS. We already sent the empty packet to the driver to notify EOS. */
    if(!avpkt->size && s->output_pool.streamon)
        s->output_pool.streamon = 0;

    if(!s->capture_pool.streamon)
        return avpkt->size;

    /* Need to wait for decoder before enqueuing more if output pool is almost full or on EOS */
    if(s->output_pool.num_queued >= s->output_pool.num_buffers - 2 || !avpkt->size) {
        s->capture_pool.blocking_dequeue = 1000;
    } else {
        s->capture_pool.blocking_dequeue = 0;
    }

    ret = avpriv_v4l_dequeue_frame_or_pkt(&(s->capture_pool), frame, NULL);

    if(!ret) {
        *got_frame = 1;
    } else if(AVERROR(EAGAIN) == ret && !s->capture_pool.blocking_dequeue) {
        ret = 0;
    }

    return (ret < 0 ? ret : avpkt->size);
}

#define OFFSET(x) offsetof(V4Lm2mContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM

static const AVOption options[] = {
    V4L_M2M_DEFAULT_OPTS,
    { "num_capture_pool_extra_buffers",
        "Number of extra buffers in the capture pool",
        OFFSET(capture_pool.num_buffers),
        AV_OPT_TYPE_INT,
        {.i64 = 4 },
        4, INT_MAX, FLAGS },
    { NULL },
};

#define M2MDEC(NAME, LONGNAME, CODEC) \
static const AVClass v4l2_m2m_ ## NAME ## _dec_class = {\
    .class_name = #NAME "_v4l2_m2m_decoder",\
    .item_name  = av_default_item_name,\
    .option     = options,\
    .version    = LIBAVUTIL_VERSION_INT,\
};\
\
AVCodec ff_ ## NAME ## _v4l2m2m_decoder = { \
    .name           = #NAME "_v4l2m2m" ,\
    .long_name      = NULL_IF_CONFIG_SMALL("V4L2 mem2mem " LONGNAME " decoder wrapper"),\
    .type           = AVMEDIA_TYPE_VIDEO,\
    .id             = CODEC ,\
    .priv_data_size = sizeof(V4Lm2mContext),\
    .priv_class     = &v4l2_m2m_ ## NAME ## _dec_class,\
    .init           = v4lm2m_decode_init,\
    .decode         = v4lm2m_decode_frame,\
    .close          = ff_v4lm2m_codec_end,\
    .capabilities   = CODEC_CAP_DELAY,\
};

#if CONFIG_H263_V4L2M2M_DECODER
M2MDEC(h263 , "H.263" , AV_CODEC_ID_H263 );
#endif

#if CONFIG_H264_V4L2M2M_DECODER
M2MDEC(h264 , "H.264" , AV_CODEC_ID_H264 );
#endif

#if CONFIG_MPEG1_V4L2M2M_DECODER
M2MDEC(mpeg1, "MPEG1", AV_CODEC_ID_MPEG1VIDEO);
#endif

#if CONFIG_MPEG2_V4L2M2M_DECODER
M2MDEC(mpeg2, "MPEG2", AV_CODEC_ID_MPEG2VIDEO);
#endif

#if CONFIG_MPEG4_V4L2M2M_DECODER
M2MDEC(mpeg4, "MPEG4", AV_CODEC_ID_MPEG4);
#endif

#if CONFIG_VC1_V4L2M2M_DECODER
M2MDEC(vc1  , "VC1"  , AV_CODEC_ID_VC1  );
#endif

#if CONFIG_VP8_V4L2M2M_DECODER
M2MDEC(vp8  , "VP8"  , AV_CODEC_ID_VP8  );
#endif
